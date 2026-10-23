// SPDX-License-Identifier: Apache-2.0
#pragma once

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

} // namespace vtextension
