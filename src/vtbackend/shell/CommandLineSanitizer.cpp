// SPDX-License-Identifier: Apache-2.0
#include <vtbackend/shell/CommandLineSanitizer.hpp>

#include <core/Utils.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <optional>
#include <utility>

namespace vtbackend
{

namespace
{
    /// What becomes of one code point.
    enum class Treatment : uint8_t
    {
        Keep = 0,
        Replace, ///< Becomes U+FFFD, so the reader sees that something was there.
        Drop,
        Space, ///< Becomes U+0020: a line or field break, joined into one line.
    };

    /// What becomes of the spaces the sanitised text ends with.
    enum class TrailingSpaces : uint8_t
    {
        Keep = 0,
        Trim,
    };

    /// How many purposes there are: the columns of Rules, the entries of TrailingSpacesOf.
    constexpr auto PurposeCount = size_t { std::to_underlying(SanitizePurpose::Clipboard) + 1 };

    /// A run of code points not kept as they are, and what each purpose does with them.
    struct Rule
    {
        char32_t first;                                ///< The run's first code point.
        char32_t last;                                 ///< The run's last code point, inclusive.
        std::array<Treatment, PurposeCount> treatment; ///< Indexed by SanitizePurpose.
    };

    constexpr auto R = Treatment::Replace;
    constexpr auto D = Treatment::Drop;
    constexpr auto K = Treatment::Keep;
    constexpr auto S = Treatment::Space;

    // clang-format off
    /// Every code point not kept as it is; anything not listed is kept. A new purpose is a new column.
    constexpr auto Rules = std::array {
        //                                                          Display, Insert, Type, Clipboard
        Rule { .first = 0x00,   .last = 0x08,   .treatment = { R, D, D, D } }, // C0 < TAB
        Rule { .first = 0x09,   .last = 0x0A,   .treatment = { R, K, D, S } }, // TAB, LF: keys when typed
        Rule { .first = 0x0B,   .last = 0x0C,   .treatment = { R, D, D, D } }, // VT, FF
        Rule { .first = 0x0D,   .last = 0x0D,   .treatment = { R, D, D, S } }, // CR: a line end too
        Rule { .first = 0x0E,   .last = 0x1F,   .treatment = { R, D, D, D } }, // ESC & co.
        Rule { .first = 0x7F,   .last = 0x7F,   .treatment = { R, D, D, D } }, // DEL
        Rule { .first = 0x80,   .last = 0x9F,   .treatment = { R, D, D, D } }, // C1
        Rule { .first = 0x2028, .last = 0x2029, .treatment = { R, R, R, R } }, // LS, PS
        Rule { .first = 0x202A, .last = 0x202E, .treatment = { R, R, R, R } }, // LRE..RLO
        Rule { .first = 0x2066, .last = 0x2069, .treatment = { R, R, R, R } }, // LRI..PDI
    };

    /// A run of code points, both ends inclusive.
    struct CodePointRange
    {
        char32_t first;
        char32_t last;
    };

    /// Every code point with Unicode's White_Space property (PropList.txt).
    constexpr auto WhiteSpace = std::array {
        CodePointRange { .first = 0x0009, .last = 0x000D }, // TAB, LF, VT, FF, CR
        CodePointRange { .first = 0x0020, .last = 0x0020 }, // SPACE
        CodePointRange { .first = 0x0085, .last = 0x0085 }, // NEXT LINE
        CodePointRange { .first = 0x00A0, .last = 0x00A0 }, // NO-BREAK SPACE
        CodePointRange { .first = 0x1680, .last = 0x1680 }, // OGHAM SPACE MARK
        CodePointRange { .first = 0x2000, .last = 0x200A }, // EN QUAD..HAIR SPACE
        CodePointRange { .first = 0x2028, .last = 0x2029 }, // LINE SEPARATOR, PARAGRAPH SEPARATOR
        CodePointRange { .first = 0x202F, .last = 0x202F }, // NARROW NO-BREAK SPACE
        CodePointRange { .first = 0x205F, .last = 0x205F }, // MEDIUM MATHEMATICAL SPACE
        CodePointRange { .first = 0x3000, .last = 0x3000 }, // IDEOGRAPHIC SPACE
    };

    /// What each purpose does with the spaces its text ends with, indexed by SanitizePurpose. A copy trims
    /// them: a line end it ended with would otherwise run the pasted command by itself.
    constexpr auto TrailingSpacesOf = std::array<TrailingSpaces, PurposeCount> {
        //  Display,              Insert,               Type,                 Clipboard
        TrailingSpaces::Keep, TrailingSpaces::Keep, TrailingSpaces::Keep, TrailingSpaces::Trim,
    };
    // clang-format on

    /// A well-formed UTF-8 lead byte (Unicode §3.9, Table 3-7) and what may follow it.
    struct LeadByte
    {
        uint8_t first;       ///< The first lead byte this row covers.
        uint8_t last;        ///< The last lead byte this row covers, inclusive.
        uint8_t length;      ///< Bytes in the whole sequence.
        uint8_t payloadMask; ///< The lead byte's share of the code point.
        uint8_t secondMin;   ///< The second byte's range, narrowed against overlongs, surrogates and
        uint8_t secondMax;   ///< code points beyond U+10FFFF; every later byte is 0x80..0xBF.
    };

    // clang-format off
    /// Every byte that may lead a multi-byte sequence; any other byte from 0x80 up is ill-formed there.
    constexpr auto LeadBytes = std::array {
        LeadByte { .first = 0xC2, .last = 0xDF, .length = 2, .payloadMask = 0x1F, .secondMin = 0x80, .secondMax = 0xBF },
        LeadByte { .first = 0xE0, .last = 0xE0, .length = 3, .payloadMask = 0x0F, .secondMin = 0xA0, .secondMax = 0xBF },
        LeadByte { .first = 0xE1, .last = 0xEC, .length = 3, .payloadMask = 0x0F, .secondMin = 0x80, .secondMax = 0xBF },
        LeadByte { .first = 0xED, .last = 0xED, .length = 3, .payloadMask = 0x0F, .secondMin = 0x80, .secondMax = 0x9F },
        LeadByte { .first = 0xEE, .last = 0xEF, .length = 3, .payloadMask = 0x0F, .secondMin = 0x80, .secondMax = 0xBF },
        LeadByte { .first = 0xF0, .last = 0xF0, .length = 4, .payloadMask = 0x07, .secondMin = 0x90, .secondMax = 0xBF },
        LeadByte { .first = 0xF1, .last = 0xF3, .length = 4, .payloadMask = 0x07, .secondMin = 0x80, .secondMax = 0xBF },
        LeadByte { .first = 0xF4, .last = 0xF4, .length = 4, .payloadMask = 0x07, .secondMin = 0x80, .secondMax = 0x8F },
    };
    // clang-format on

    /// A multi-byte sequence part-way through decoding.
    struct Partial
    {
        size_t start;       ///< Offset of its lead byte.
        char32_t codePoint; ///< The bits gathered so far.
        uint8_t remaining;  ///< Continuation bytes still to come.
        uint8_t nextMin;    ///< The lowest byte the next continuation byte may be.
        uint8_t nextMax;    ///< The highest byte the next continuation byte may be.
    };

    /// U+FFFD REPLACEMENT CHARACTER, UTF-8 encoded.
    constexpr auto ReplacementCharacter = std::string_view { "\xEF\xBF\xBD" };

    /// Whether @p ranges -- runs of code points with inclusive `first` and `last` ends -- are sorted and
    /// disjoint: what findRange() needs of them to bisect.
    /// @param ranges The table.
    /// @return Whether it is.
    template <typename Range, std::size_t N>
    [[nodiscard]] constexpr bool isSortedAndDisjoint(std::array<Range, N> const& ranges) noexcept
    {
        return std::ranges::is_sorted(ranges, {}, &Range::first)
               && std::ranges::adjacent_find(ranges, [](Range const& a, Range const& b) {
                      return a.last >= b.first;
                  }) == ranges.end();
    }

    /// The run of @p ranges holding @p codePoint, by bisection.
    /// @param ranges The table, sorted and disjoint (isSortedAndDisjoint()).
    /// @param codePoint The code point to look up.
    /// @return The run, or nullptr when none holds it.
    template <typename Range, std::size_t N>
    [[nodiscard]] constexpr Range const* findRange(std::array<Range, N> const& ranges,
                                                   char32_t codePoint) noexcept
    {
        // The first run not wholly below the code point is the only one that can hold it.
        auto const range = std::ranges::lower_bound(ranges, codePoint, {}, &Range::last);
        return range != ranges.end() && range->first <= codePoint ? &*range : nullptr;
    }

    static_assert(isSortedAndDisjoint(Rules),
                  "treatmentOf() bisects the rules: they must be sorted and disjoint");
    static_assert(isSortedAndDisjoint(WhiteSpace),
                  "isWhiteSpace() bisects the ranges: they must be sorted and disjoint");

    /// @return Whether @p codePoint has Unicode's White_Space property.
    [[nodiscard]] constexpr bool isWhiteSpace(char32_t codePoint) noexcept
    {
        return findRange(WhiteSpace, codePoint) != nullptr;
    }

    /// @return What @p purpose does with @p codePoint.
    [[nodiscard]] constexpr Treatment treatmentOf(char32_t codePoint, SanitizePurpose purpose) noexcept
    {
        auto const* const rule = findRange(Rules, codePoint);
        return rule != nullptr ? rule->treatment[std::to_underlying(purpose)] : Treatment::Keep;
    }

    /// @return The rule for @p codeUnit as a lead byte, or nullopt when it can never lead.
    [[nodiscard]] constexpr std::optional<LeadByte> leadByteOf(uint8_t codeUnit) noexcept
    {
        auto const rule = std::ranges::find_if(LeadBytes, [codeUnit](LeadByte const& candidate) {
            return candidate.first <= codeUnit && codeUnit <= candidate.last;
        });
        if (rule == LeadBytes.end())
            return std::nullopt;
        return *rule;
    }

    /// Whether every purpose keeps @p ch as it is: printable ASCII, which no rule names.
    [[nodiscard]] constexpr bool isPlainAscii(char ch) noexcept
    {
        return 0x20 <= ch && ch <= 0x7E;
    }

    /// Whether a walk goes on after a step.
    enum class WalkStep : uint8_t
    {
        Continue = 0,
        Stop,
    };

    /// Walks @p text one code point at a time, decoding UTF-8 a code unit at a time, so that a broken
    /// sequence costs exactly its maximal subpart (Unicode §3.9, "U+FFFD Substitution of Maximal
    /// Subparts") and the unit that broke it starts afresh.
    /// @param text The text to walk.
    /// @param visit Called with each well-formed code point and its bytes, or with nullopt and the bytes
    ///              of each maximal ill-formed subpart; returns whether to go on.
    /// @return Stop when @p visit stopped the walk, else Continue.
    template <typename Visitor>
    WalkStep walkCodePoints(std::string_view text, Visitor visit)
    {
        auto partial = std::optional<Partial> {};
        for (auto const [offset, ch]: core::views::enumerate(text))
        {
            auto const codeUnit = static_cast<uint8_t>(ch);
            if (partial)
            {
                if (partial->nextMin <= codeUnit && codeUnit <= partial->nextMax)
                {
                    partial->codePoint =
                        static_cast<char32_t>((partial->codePoint << 6U) | (codeUnit & 0x3FU));
                    partial->nextMin = 0x80;
                    partial->nextMax = 0xBF;
                    --partial->remaining;
                    if (partial->remaining == 0)
                    {
                        auto const bytes = text.substr(partial->start, offset + 1 - partial->start);
                        if (visit(std::optional { partial->codePoint }, bytes) == WalkStep::Stop)
                            return WalkStep::Stop;
                        partial.reset();
                    }
                    continue;
                }
                auto const bytes = text.substr(partial->start, offset - partial->start);
                if (visit(std::optional<char32_t> {}, bytes) == WalkStep::Stop)
                    return WalkStep::Stop;
                partial.reset();
            }

            if (codeUnit < 0x80)
            {
                if (visit(std::optional { char32_t { codeUnit } }, text.substr(offset, 1)) == WalkStep::Stop)
                    return WalkStep::Stop;
            }
            else if (auto const lead = leadByteOf(codeUnit))
                partial = Partial { .start = offset,
                                    .codePoint = static_cast<char32_t>(codeUnit & lead->payloadMask),
                                    .remaining = static_cast<uint8_t>(lead->length - 1),
                                    .nextMin = lead->secondMin,
                                    .nextMax = lead->secondMax };
            else if (visit(std::optional<char32_t> {}, text.substr(offset, 1)) == WalkStep::Stop)
                return WalkStep::Stop;
        }
        if (partial)
            return visit(std::optional<char32_t> {}, text.substr(partial->start));
        return WalkStep::Continue;
    }

    /// @return What @p purpose does with the spaces its text ends with.
    [[nodiscard]] constexpr TrailingSpaces trailingSpacesOf(SanitizePurpose purpose) noexcept
    {
        return TrailingSpacesOf[std::to_underlying(purpose)];
    }

    /// @return @p text with each code point treated as @p purpose says, its trailing spaces left alone.
    [[nodiscard]] std::string treatCodePoints(std::string_view text, SanitizePurpose purpose)
    {
        // The leading run of printable ASCII -- for most command lines and paths, all of it -- is kept by
        // every purpose, so it is copied whole rather than decoded.
        auto const plain = static_cast<size_t>(std::ranges::find_if_not(text, isPlainAscii) - text.begin());
        auto out = std::string { text.substr(0, plain) };
        if (plain == text.size())
            return out;
        out.reserve(text.size());

        walkCodePoints(text.substr(plain), [&](std::optional<char32_t> codePoint, std::string_view bytes) {
            if (!codePoint)
            {
                out += ReplacementCharacter;
                return WalkStep::Continue;
            }
            switch (treatmentOf(*codePoint, purpose))
            {
                case Treatment::Keep: out += bytes; break;
                case Treatment::Replace: out += ReplacementCharacter; break;
                case Treatment::Drop: break;
                case Treatment::Space: out += ' '; break;
            }
            return WalkStep::Continue;
        });
        return out;
    }
} // namespace

std::string sanitizeCommandLine(std::string_view text, SanitizePurpose purpose)
{
    auto out = treatCodePoints(text, purpose);
    if (trailingSpacesOf(purpose) == TrailingSpaces::Trim)
        out.erase(out.find_last_not_of(' ') + 1); // npos + 1 is 0: nothing but spaces leaves nothing
    return out;
}

bool isUnchangedBySanitizing(std::string_view text, SanitizePurpose purpose)
{
    if (trailingSpacesOf(purpose) == TrailingSpaces::Trim && text.ends_with(' '))
        return false;
    auto const isKept = [purpose](std::optional<char32_t> codePoint, std::string_view /*bytes*/) {
        return codePoint && treatmentOf(*codePoint, purpose) == Treatment::Keep ? WalkStep::Continue
                                                                                : WalkStep::Stop;
    };
    return walkCodePoints(text, isKept) == WalkStep::Continue;
}

std::string_view trimTrailingWhiteSpace(std::string_view text)
{
    // The walk hands over every byte of the text once and in order, so the bytes seen so far are a prefix.
    auto seen = size_t { 0 };
    auto end = size_t { 0 };
    walkCodePoints(text, [&](std::optional<char32_t> codePoint, std::string_view bytes) {
        seen += bytes.size();
        if (!codePoint || !isWhiteSpace(*codePoint))
            end = seen;
        return WalkStep::Continue;
    });
    return text.substr(0, end);
}

} // namespace vtbackend
