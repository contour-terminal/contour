// SPDX-License-Identifier: Apache-2.0
#include <orchestrator/core/Sanitize.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <ranges>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <orchestrator/core/Utf8.hpp>

namespace orchestrator
{

namespace
{
    /// What displayText does with a code point.
    enum class Disposition : uint8_t
    {
        Keep,  ///< Drawn as it is.
        Drop,  ///< Removed without a trace.
        Space, ///< Drawn as a space, one for a whole run of them.
    };

    /// A range of code points, both ends included, and what becomes of them.
    struct CodepointClass
    {
        char32_t first;
        char32_t last;
        Disposition disposition;
    };

    /// Every code point that is not kept as it is; the first range that holds a code point decides.
    constexpr auto CodepointClasses = std::array {
        CodepointClass { .first = U'\t', .last = U'\n', .disposition = Disposition::Space },
        CodepointClass {
            .first = 0x00, .last = 0x1F, .disposition = Disposition::Drop }, // C0, ESC and CR too
        CodepointClass { .first = U' ', .last = U' ', .disposition = Disposition::Space },
        CodepointClass { .first = 0x7F, .last = 0x9F, .disposition = Disposition::Drop },      // DEL and C1
        CodepointClass { .first = 0x061C, .last = 0x061C, .disposition = Disposition::Drop },  // ALM
        CodepointClass { .first = 0x200E, .last = 0x200F, .disposition = Disposition::Drop },  // LRM, RLM
        CodepointClass { .first = 0x2028, .last = 0x2029, .disposition = Disposition::Space }, // LS, PS
        CodepointClass { .first = 0x202A, .last = 0x202E, .disposition = Disposition::Drop },  // LRE…RLO
        CodepointClass { .first = 0x2066, .last = 0x2069, .disposition = Disposition::Drop },  // LRI…PDI
    };

    /// A range of code points, both ends included.
    struct CodepointRange
    {
        char32_t first;
        char32_t last;
    };

    /// The code points that draw as nothing, which holdsInvisible() looks for.
    constexpr auto InvisibleCodepoints = std::array {
        CodepointRange { .first = 0x00AD, .last = 0x00AD },   // soft hyphen
        CodepointRange { .first = 0x034F, .last = 0x034F },   // combining grapheme joiner
        CodepointRange { .first = 0x115F, .last = 0x1160 },   // Hangul choseong and jungseong fillers
        CodepointRange { .first = 0x180E, .last = 0x180E },   // Mongolian vowel separator
        CodepointRange { .first = 0x200B, .last = 0x200D },   // zero-width space, non-joiner, joiner
        CodepointRange { .first = 0x2060, .last = 0x2064 },   // word joiner, invisible operators
        CodepointRange { .first = 0x3164, .last = 0x3164 },   // Hangul filler
        CodepointRange { .first = 0xFE00, .last = 0xFE0F },   // variation selectors
        CodepointRange { .first = 0xFEFF, .last = 0xFEFF },   // zero-width no-break space, BOM
        CodepointRange { .first = 0xFFA0, .last = 0xFFA0 },   // halfwidth Hangul filler
        CodepointRange { .first = 0xE0000, .last = 0xE007F }, // tags
        CodepointRange { .first = 0xE0100, .last = 0xE01EF }, // variation selectors supplement
    };

    /// The character that marks a cut.
    constexpr auto Ellipsis = U'…';

    /// What becomes of @p decoded.
    [[nodiscard]] constexpr Disposition dispositionOf(Decoded decoded) noexcept
    {
        // A malformed byte decodes as U+FFFD of length 1; a U+FFFD spelled out in the input takes three.
        if (decoded.codepoint == ReplacementCharacter && decoded.length == 1)
            return Disposition::Drop;
        // A loop rather than std::ranges::find_if: a std::array's iterator is a pointer in libstdc++ and a
        // class in libc++ (the guest's), and no one spelling of the found iterator suits both.
        for (auto const& range: CodepointClasses)
            if (range.first <= decoded.codepoint && decoded.codepoint <= range.last)
                return range.disposition;
        return Disposition::Keep;
    }

    /// Whether BoundedLine::append took the code point or cut the line.
    enum class Appended : uint8_t
    {
        Cut,
        Taken,
    };

    /// A line that holds at most a number of characters, the last a '…' once more were offered.
    class BoundedLine
    {
      public:
        /// @param maxChars The most characters the line may hold, at least 1.
        explicit BoundedLine(std::size_t maxChars) noexcept: _maxChars { maxChars } {}

        /// Appends @p codepoint, or cuts the line when it is full.
        /// A cut keeps the characters that fit before the '…', less a space that would end them.
        /// @return Appended::Cut once the line is final; nothing more may be appended then.
        [[nodiscard]] Appended append(char32_t codepoint)
        {
            if (_chars == _maxChars)
            {
                _text.resize(_lastCharOffset);
                // A collapsed space would sit right before the '…'; collapsing leaves at most one, never
                // leading.
                if (_text.ends_with(' '))
                    _text.pop_back();
                appendUtf8(_text, Ellipsis);
                return Appended::Cut;
            }
            if (_chars + 1 == _maxChars)
                _lastCharOffset = _text.size();
            appendUtf8(_text, codepoint);
            ++_chars;
            return Appended::Taken;
        }

        /// Whether nothing has been appended yet.
        [[nodiscard]] bool empty() const noexcept { return _chars == 0; }

        /// The line, moved out.
        [[nodiscard]] std::string take() && noexcept { return std::move(_text); }

      private:
        std::size_t _maxChars;
        std::size_t _chars = 0;
        std::size_t _lastCharOffset = 0; ///< Where the last character that fits begins, for the cut.
        std::string _text;
    };

    /// The code points in @p text, a malformed byte counting as one, up to @p limit + 1.
    [[nodiscard]] constexpr std::size_t countCharacters(std::string_view text, std::size_t limit) noexcept
    {
        auto count = std::size_t { 0 };
        for ([[maybe_unused]] auto const decoded: Codepoints { text })
            if (++count > limit)
                break;
        return count;
    }
} // namespace

bool holdsInvisible(std::string_view text) noexcept
{
    for (auto const decoded: Codepoints { text })
        // A loop rather than std::ranges::any_of, for the reason dispositionOf() gives.
        for (auto const& range: InvisibleCodepoints)
            if (range.first <= decoded.codepoint && decoded.codepoint <= range.last)
                return true;
    return false;
}

std::string displayText(std::string_view input, std::size_t maxChars)
{
    if (maxChars == 0)
        return {};

    auto line = BoundedLine { maxChars };
    auto spacePending = false;
    for (auto const decoded: Codepoints { input })
    {
        switch (dispositionOf(decoded))
        {
            case Disposition::Drop: continue;
            case Disposition::Space:
                // Leading spaces are trimmed here, trailing ones by never being followed by a character.
                spacePending = !line.empty();
                continue;
            case Disposition::Keep: break;
        }
        if (spacePending && line.append(U' ') == Appended::Cut)
            return std::move(line).take();
        spacePending = false;
        if (line.append(decoded.codepoint) == Appended::Cut)
            return std::move(line).take();
    }
    return std::move(line).take();
}

std::vector<std::string> displayLines(std::string_view input, std::size_t maxLines, std::size_t maxChars)
{
    auto lines = std::vector<std::string> {};
    if (maxLines == 0)
        return lines;
    // A final LF ends the last line.
    if (input.ends_with('\n'))
        input.remove_suffix(1);
    if (input.empty())
        return lines;
    for (auto const piece: std::views::split(input, '\n'))
    {
        if (lines.size() == maxLines)
        {
            lines.back() = "…";
            return lines;
        }
        auto line = std::string_view { piece.begin(), piece.end() };
        auto indent = std::size_t { 0 };
        while (!line.empty() && (line.front() == ' ' || line.front() == '\t'))
        {
            indent += line.front() == '\t' ? TabIndentColumns : 1;
            line.remove_prefix(1);
        }
        auto rest = displayText(line, maxChars);
        lines.push_back(rest.empty() ? std::string {}
                                     : std::string(std::min(indent, MaxIndentColumns), ' ') + rest);
    }
    return lines;
}

std::expected<std::string, NoteError> singleLineNote(std::string_view input)
{
    if (countCharacters(input, MaxNoteChars) > MaxNoteChars)
        return std::unexpected { NoteError::TooLong };
    auto note = displayText(input, MaxNoteChars);
    if (note.starts_with('-'))
        return std::unexpected { NoteError::StartsWithDash };
    return note;
}

} // namespace orchestrator
