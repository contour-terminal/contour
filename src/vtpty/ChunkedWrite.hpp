// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <algorithm>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <string_view>

namespace vtpty
{

/// How the pipe took a chunk as it was handed over.
enum class ChunkStart : std::uint8_t
{
    Completed = 0, ///< At once: it had the room.
    Pending,       ///< Not yet: it waits for the reader, until it completes or is cancelled.
    Failed,        ///< Not at all: the pipe is gone.
};

/// What ended the wait for a chunk the pipe did not take at once.
enum class PendingWait : std::uint8_t
{
    Completed = 0, ///< The chunk completed.
    Interrupted, ///< Writes were interrupted first, or the wait itself failed: the chunk is to be cancelled.
};

/// How a chunk ended, as the pipe reports it once nothing is pending any more.
enum class ChunkStatus : std::uint8_t
{
    Written = 0, ///< Completed: by itself, or as its cancellation came too late.
    Cancelled,   ///< Cancelled before it completed.
    Failed,      ///< The pipe failed under it.
};

/// One chunk's end: how, and how many of its bytes the pipe reports transferred.
struct ChunkResult
{
    ChunkStatus status;
    std::size_t transferred;
};

/// What a write that waits for its reader asks of the pipe, one call each, in this order per chunk:
/// isInterrupted(), room() where the room is not known to suffice, start(), awaitPending() when start() said
/// Pending, cancel() when that said Interrupted, and finish() after every start() that did not say Failed.
/// One chunk is in flight at a time: start() is called only once the previous chunk's finish() returned.
template <typename T>
concept ChunkedPipe = requires(T& pipe, std::string_view chunk) {
    /// Whether writes are interrupted: no chunk starts then.
    { pipe.isInterrupted() } -> std::same_as<bool>;
    /// How many bytes the pipe takes now without waiting for its reader.
    { pipe.room() } -> std::same_as<std::size_t>;
    /// Hands @p chunk to the pipe.
    { pipe.start(chunk) } -> std::same_as<ChunkStart>;
    /// Waits until the chunk started last completes, or until writes are interrupted.
    { pipe.awaitPending() } -> std::same_as<PendingWait>;
    /// Asks for the chunk started last to be cancelled, without waiting for it.
    { pipe.cancel() };
    /// Waits until the chunk started last has ended, cancelled or not.
    { pipe.finish() } -> std::same_as<ChunkResult>;
};

/// How a chunked write ended.
enum class WriteEnd : std::uint8_t
{
    Complete = 0, ///< Everything was written.
    Interrupted,  ///< Writes were interrupted before everything was.
    Failed,       ///< The pipe failed before everything was.
};

/// What a chunked write wrote, and why it stopped.
struct WriteOutcome
{
    std::size_t written;
    WriteEnd end;
};

/// Writes @p data to @p pipe, never more at once than the pipe takes without waiting for its reader; only
/// while it has no room, a single byte, which waits until the reader takes it.
///
/// A chunk that waits anyway -- the room overstated -- is waited for like the single byte: until it completes
/// or writes are interrupted, which cancels it. What the pipe then reports transferred counts as written.
///
/// That count is exact where the reader took none of the cancelled chunk, as a reader that has stopped takes
/// none. Where a live but slow reader took part of it before the interruption, the pipe reports none of it
/// transferred (a cancelled request reports none), and the caller writes that part again once writes are
/// allowed: input repeated, where a write that ignored the interruption would wait until the pipe closed. It
/// takes an overstated room, a reader that is reading, and an interruption, which only a live upgrade asks
/// for; and only the first such chunk, as the next one asks the pipe its room again.
///
/// The room is asked of the pipe only when a write needs more than is known to be free: the pipe has one
/// writer, and its reader only ever frees room, so what was free less what was written since is free still.
/// A keystroke asks nothing. A chunk that waited, or did not end written, proved the room it was sized by
/// wrong: none is known then, and the next chunk asks.
///
/// @param pipe      The pipe, as ChunkedPipe describes it.
/// @param data      What to write.
/// @param knownRoom Bytes the pipe is known to take without waiting; kept up to date across writes.
/// @return What was written, and why the write stopped.
template <ChunkedPipe Pipe>
[[nodiscard]] WriteOutcome writeInChunks(Pipe& pipe, std::string_view data, std::size_t& knownRoom)
{
    auto written = std::size_t { 0 };
    while (written < data.size())
    {
        if (pipe.isInterrupted())
            return { .written = written, .end = WriteEnd::Interrupted };
        auto const left = data.size() - written;
        if (left > knownRoom && left > 1)
            knownRoom = pipe.room();
        auto const chunk = data.substr(written, std::clamp<std::size_t>(knownRoom, 1, left));
        auto const started = pipe.start(chunk);
        switch (started)
        {
            case ChunkStart::Failed: {
                knownRoom = 0;
                return { .written = written, .end = WriteEnd::Failed };
            }
            case ChunkStart::Pending:
                if (pipe.awaitPending() == PendingWait::Interrupted)
                    pipe.cancel();
                break;
            case ChunkStart::Completed: break;
        }
        auto const [status, transferred] = pipe.finish();
        written += transferred;
        if (started == ChunkStart::Pending || status != ChunkStatus::Written)
            knownRoom = 0;
        else
            knownRoom -= std::min(knownRoom, transferred);
        switch (status)
        {
            case ChunkStatus::Written: break;
            case ChunkStatus::Cancelled: return { .written = written, .end = WriteEnd::Interrupted };
            case ChunkStatus::Failed: return { .written = written, .end = WriteEnd::Failed };
        }
    }
    return { .written = written, .end = WriteEnd::Complete };
}

} // namespace vtpty
