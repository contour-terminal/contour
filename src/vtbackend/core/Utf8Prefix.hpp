// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <cstddef>
#include <ranges>
#include <string_view>

namespace vtbackend
{

/// The longest tail a UTF-8 sequence has: a four-byte sequence carries three continuation bytes.
inline constexpr auto MaxUtf8ContinuationBytes = std::size_t { 3 };

/// @param octet A byte of UTF-8 text.
/// @return Whether @p octet continues a UTF-8 sequence (10xxxxxx) rather than starting one.
[[nodiscard]] constexpr bool isUtf8ContinuationByte(char octet) noexcept
{
    return (static_cast<unsigned>(static_cast<unsigned char>(octet)) & 0b1100'0000U) == 0b1000'0000U;
}

/// How much of @p text a bound of @p maxBytes keeps without splitting a UTF-8 code point: the one cut
/// every bounded copy of untrusted text makes (truncateUtf8, an OSC 99 notification's text).
///
/// Text that is not UTF-8 at the cut is cut there anyway: a run of continuation bytes longer than any
/// sequence has is not a code point to protect.
/// @param text The text as received.
/// @param maxBytes The most bytes to keep.
/// @return The length of @p text's longest prefix of at most @p maxBytes bytes that does not end inside
///         a code point; @p text's own length when it fits.
[[nodiscard]] constexpr std::size_t utf8PrefixLength(std::string_view text, std::size_t maxBytes) noexcept
{
    if (text.size() <= maxBytes)
        return text.size();

    // text[cut] is the first byte left out, in range: cut <= maxBytes < text.size(). When it continues a
    // sequence, the kept prefix would end inside that sequence, so back off to the byte that starts it.
    auto cut = maxBytes;
    for ([[maybe_unused]] auto const step: std::views::iota(std::size_t { 0 }, MaxUtf8ContinuationBytes))
    {
        if (cut == 0 || !isUtf8ContinuationByte(text[cut]))
            break;
        --cut;
    }
    return cut;
}

} // namespace vtbackend
