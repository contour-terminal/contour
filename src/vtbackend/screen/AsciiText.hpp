// SPDX-License-Identifier: Apache-2.0
#pragma once

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

} // namespace vtbackend
