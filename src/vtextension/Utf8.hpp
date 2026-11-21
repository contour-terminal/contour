// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <ranges>
#include <string>
#include <string_view>
#include <utility>

namespace vtextension
{

/// The lowest code point of a sequence of @p length UTF-8 bytes; a shorter encoding is overlong.
inline constexpr auto Utf8MinimumCodePoints = std::array<char32_t, 5> { 0, 0, 0x80, 0x800, 0x10000 };

/// The largest Unicode code point, and the surrogate range, which no UTF-8 sequence encodes.
inline constexpr char32_t MaxCodePoint = 0x10FFFF;
inline constexpr char32_t FirstSurrogate = 0xD800;
inline constexpr char32_t LastSurrogate = 0xDFFF;

/// The length of the UTF-8 sequence @p lead starts; 0 if it cannot start one.
[[nodiscard]] constexpr std::size_t utf8SequenceLength(uint8_t lead) noexcept
{
    if (lead < 0x80)
        return 1;
    if (lead < 0xC0)
        return 0;
    if (lead < 0xE0)
        return 2;
    if (lead < 0xF0)
        return 3;
    return lead < 0xF8 ? 4 : 0;
}

/// Decodes the UTF-8 sequence at the front of @p text, which must not be empty.
/// @return The code point and the sequence's length; `std::nullopt` for an invalid sequence.
[[nodiscard]] constexpr std::optional<std::pair<char32_t, std::size_t>> decodeUtf8(
    std::string_view text) noexcept
{
    auto const lead = static_cast<uint8_t>(text.front());
    auto const length = utf8SequenceLength(lead);
    if (length == 0 || text.size() < length)
        return std::nullopt;
    auto codePoint = char32_t { length == 1 ? lead : lead & (0xFFU >> (length + 1)) };
    for (auto const index: std::views::iota(std::size_t { 1 }, length))
    {
        auto const next = static_cast<uint8_t>(text[index]);
        if ((next & 0xC0U) != 0x80U)
            return std::nullopt;
        codePoint = (codePoint << 6U) | (next & 0x3FU);
    }
    if (codePoint < Utf8MinimumCodePoints.at(length) || codePoint > MaxCodePoint
        || (codePoint >= FirstSurrogate && codePoint <= LastSurrogate))
        return std::nullopt;
    return std::pair { codePoint, length };
}

/// Whether @p text is valid UTF-8: no truncated, overlong or surrogate sequence, nothing above
/// U+10FFFF.
/// @param text The bytes to check.
/// @return Whether every sequence in @p text decodes.
[[nodiscard]] constexpr bool isValidUtf8(std::string_view text) noexcept
{
    while (!text.empty())
    {
        auto const decoded = decodeUtf8(text);
        if (!decoded)
            return false;
        text.remove_prefix(decoded->second);
    }
    return true;
}

/// U+FFFD REPLACEMENT CHARACTER, UTF-8 encoded.
inline constexpr std::string_view ReplacementCharacter = "\xEF\xBF\xBD";

/// @p text as valid UTF-8: each byte that does not begin a decodable sequence is replaced by U+FFFD, so a
/// sequence cut short becomes one U+FFFD per byte it kept. Valid text is returned as it is, unallocated.
/// @param text Bytes that should be UTF-8.
/// @return Valid UTF-8.
[[nodiscard]] inline std::string toValidUtf8(std::string text)
{
    if (isValidUtf8(text))
        return text;
    auto result = std::string {};
    result.reserve(text.size() + ReplacementCharacter.size());
    auto rest = std::string_view { text };
    while (!rest.empty())
    {
        if (auto const decoded = decodeUtf8(rest))
        {
            result += rest.substr(0, decoded->second);
            rest.remove_prefix(decoded->second);
        }
        else
        {
            result += ReplacementCharacter;
            rest.remove_prefix(1);
        }
    }
    return result;
}

/// A range of code points, both ends included.
struct CodePointRange
{
    char32_t first; ///< The first code point of the range.
    char32_t last;  ///< The last code point of the range.
};

/// What renders as nothing or as something else, and what reorders or breaks the text around it: what
/// neutralizedForDisplay() shows as code points, and what a manifest's titles may not hold.
inline constexpr auto DisguisingCodePoints = std::array {
    CodePointRange { .first = 0x0000, .last = 0x001F },   // C0 controls
    CodePointRange { .first = 0x007F, .last = 0x009F },   // DEL, C1 controls
    CodePointRange { .first = 0x00AD, .last = 0x00AD },   // soft hyphen
    CodePointRange { .first = 0x061C, .last = 0x061C },   // Arabic letter mark
    CodePointRange { .first = 0x180E, .last = 0x180E },   // Mongolian vowel separator
    CodePointRange { .first = 0x200B, .last = 0x200F },   // zero-width space/joiners, LRM, RLM
    CodePointRange { .first = 0x2028, .last = 0x202E },   // line/paragraph separators, embeddings
    CodePointRange { .first = 0x2060, .last = 0x2064 },   // word joiner, invisible operators
    CodePointRange { .first = 0x2066, .last = 0x206F },   // isolates, deprecated format characters
    CodePointRange { .first = 0xFEFF, .last = 0xFEFF },   // zero-width no-break space
    CodePointRange { .first = 0xFFF9, .last = 0xFFFB },   // interlinear annotation
    CodePointRange { .first = 0xE0000, .last = 0xE007F }, // tag characters
};

/// @param codePoint A code point.
/// @return Whether it is one of DisguisingCodePoints.
[[nodiscard]] constexpr bool disguisesText(char32_t codePoint) noexcept
{
    return std::ranges::any_of(DisguisingCodePoints, [codePoint](CodePointRange const& range) {
        return codePoint >= range.first && codePoint <= range.last;
    });
}

} // namespace vtextension
