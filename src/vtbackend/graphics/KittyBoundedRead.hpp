// SPDX-License-Identifier: Apache-2.0
#pragma once

/// @file
/// The bounded read every platform's medium source ends with, over whatever reads the opened file.

#include <vtbackend/graphics/KittyMediaPolicy.hpp>

#include <algorithm>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <span>
#include <vector>

namespace vtbackend::kitty_graphics
{

/// Reads an opened file to its end, refusing one that grew past @p limit since it was measured.
/// @param expectedSize The size the file had when it was judged (at most @p limit).
/// @param limit The largest file read, in bytes.
/// @param readSome Reads into the span it is given: the number of bytes read, 0 at the end of the file, or
///                 the refusal (Unreadable) when reading failed.
/// @return The file's bytes, or the refusal.
template <typename ReadSome>
    requires std::invocable<ReadSome&, std::span<uint8_t>>
[[nodiscard]] std::expected<std::vector<uint8_t>, MediaRefusal> readBounded(uint64_t expectedSize,
                                                                            size_t limit,
                                                                            ReadSome readSome)
{
    auto bytes = std::vector<uint8_t>(static_cast<size_t>(expectedSize));
    auto filled = size_t { 0 };
    while (true)
    {
        // One byte of room beyond the limit tells a file that grew past it from one that ends there.
        if (filled == bytes.size())
        {
            if (bytes.size() > limit)
                return std::unexpected(MediaRefusal::TooLarge);
            bytes.resize(std::min(bytes.size() + 4096, limit + 1));
        }
        auto const count = readSome(std::span { bytes }.subspan(filled));
        if (!count)
            return std::unexpected(count.error());
        if (*count == 0)
            break;
        filled += *count;
    }
    if (filled > limit)
        return std::unexpected(MediaRefusal::TooLarge);
    bytes.resize(filled);
    return bytes;
}

} // namespace vtbackend::kitty_graphics
