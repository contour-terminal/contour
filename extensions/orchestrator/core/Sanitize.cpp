// SPDX-License-Identifier: Apache-2.0
#include <orchestrator/core/Sanitize.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <string>
#include <string_view>
#include <utility>

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

    /// The character that marks a cut.
    constexpr auto Ellipsis = U'…';

    /// What becomes of @p decoded.
    [[nodiscard]] constexpr Disposition dispositionOf(Decoded decoded) noexcept
    {
        // A malformed byte decodes as U+FFFD of length 1; a U+FFFD spelled out in the input takes three.
        if (decoded.codepoint == ReplacementCharacter && decoded.length == 1)
            return Disposition::Drop;
        auto const* const found = std::ranges::find_if(CodepointClasses, [&](CodepointClass const& range) {
            return range.first <= decoded.codepoint && decoded.codepoint <= range.last;
        });
        return found != CodepointClasses.end() ? found->disposition : Disposition::Keep;
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
