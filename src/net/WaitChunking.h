// SPDX-License-Identifier: Apache-2.0
#pragma once

/// @file
/// Pure, platform-neutral chunking and fair-rotation math for waiting on more
/// native handles than a single wait call accepts.
///
/// Windows' @c WaitForMultipleObjects rejects a set larger than
/// @c MAXIMUM_WAIT_OBJECTS (64). To wait on an arbitrary number of handles the
/// array is split into fixed-size chunks that are swept individually. The
/// decisions involved — how many chunks, each chunk's span, and which chunk to
/// start the next sweep on (so high-index handles are not starved) — are pure
/// integer arithmetic and are extracted here, free of @c windows.h, so they can
/// be unit-tested on any platform. @c PollEventSource's Windows path interprets
/// these spans against the real handle array.

#include <cstddef>

namespace net
{

/// A contiguous sub-range of the handle array that fits a single native wait.
struct WaitChunk
{
    std::size_t offset = 0; ///< Index of the chunk's first handle in the full array.
    std::size_t count = 0;  ///< Number of handles in the chunk (1..maxChunk).

    /// @return True if both chunks name the same span.
    [[nodiscard]] friend constexpr bool operator==(WaitChunk, WaitChunk) noexcept = default;
};

/// @param total The number of handles to wait on.
/// @param maxChunk The largest set a single native wait accepts (must be > 0).
/// @return The number of chunks the handles split into (0 when @p total is 0).
[[nodiscard]] constexpr std::size_t waitChunkCount(std::size_t total, std::size_t maxChunk) noexcept
{
    return (total + maxChunk - 1) / maxChunk;
}

/// Describes the @p chunkIndex-th chunk of @p total handles. Chunks are laid out
/// front-to-back; every chunk but possibly the last holds exactly @p maxChunk
/// handles.
/// @param total The number of handles to wait on.
/// @param maxChunk The largest set a single native wait accepts (must be > 0).
/// @param chunkIndex The chunk to describe (0-based; must be < @c waitChunkCount()).
/// @return The chunk's offset and count; the final chunk may be shorter.
[[nodiscard]] constexpr WaitChunk waitChunkAt(std::size_t total,
                                              std::size_t maxChunk,
                                              std::size_t chunkIndex) noexcept
{
    auto const offset = chunkIndex * maxChunk;
    auto const remaining = total - offset;
    return WaitChunk { .offset = offset, .count = remaining < maxChunk ? remaining : maxChunk };
}

/// Advances the fair-rotation cursor so the next sweep starts one chunk later,
/// preventing low-index chunks from perpetually winning and starving the rest.
/// Wraps at @p chunkCount, so a stale cursor (e.g. after handles were detached)
/// still lands in range.
/// @param chunkCount The number of chunks (must be > 0).
/// @param current The current start-chunk cursor (any value; taken modulo).
/// @return The next start-chunk cursor, in `[0, chunkCount)`.
[[nodiscard]] constexpr std::size_t nextWaitRotation(std::size_t chunkCount, std::size_t current) noexcept
{
    auto const advanced = (current % chunkCount) + 1;
    return advanced < chunkCount ? advanced : 0;
}

} // namespace net
