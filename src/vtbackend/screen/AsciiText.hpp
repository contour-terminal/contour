// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <ranges>
#include <string_view>

namespace vtbackend
{

/// Tests whether every byte of @p text is 7-bit ASCII.
///
/// ORs all bytes together and tests the high bit once, instead of stopping at the first non-ASCII
/// byte: without a per-byte exit branch the loop vectorizes. The bulk-write path asks this of runs
/// the parser already found to be text, which are almost always ASCII throughout, so an early exit
/// would almost never be taken anyway.
/// @param text The bytes to test.
/// @return true if no byte has its high bit set; true for an empty text.
[[nodiscard]] inline bool isAsciiOnly(std::string_view text) noexcept
{
    auto bits = 0u;
    for (auto const ch: text)
        bits |= static_cast<unsigned char>(ch);
    return bits < 0x80;
}

/// Counts the bytes of @p text that are not 7-bit ASCII.
///
/// Counts each block of 255 bytes in a byte-wide counter that cannot overflow there, so that the
/// count vectorizes on bytes instead of widening every byte to a 64-bit lane.
/// @param text The bytes to count.
/// @return How many bytes have their high bit set.
[[nodiscard]] inline size_t nonAsciiByteCount(std::string_view text) noexcept
{
    auto count = size_t { 0 };
    for (auto const block: text | std::views::chunk(255))
        count += std::ranges::fold_left(block, uint8_t { 0 }, [](uint8_t n, char ch) {
            return static_cast<uint8_t>(n + (static_cast<unsigned char>(ch) >> 7));
        });
    return count;
}

} // namespace vtbackend
