// SPDX-License-Identifier: Apache-2.0
#pragma once

/// @file
/// Blocking byte I/O, for the places a reactor cannot reach: process stdio, Windows pipes. What
/// `contour daemon --stdio` relays between, and what bridges a spawned child's pipes to a socket.

#include <cstddef>
#include <cstdint>
#include <expected>
#include <memory>
#include <span>
#include <string>

namespace vthost
{

/// One end of a blocking, bidirectional byte stream.
class IByteStream
{
  public:
    virtual ~IByteStream() = default;

    /// Blocks until at least one byte arrives.
    /// @param buffer Receives the bytes.
    /// @return The count; 0 at end of stream; or why it failed.
    [[nodiscard]] virtual std::expected<std::size_t, std::string> readSome(std::span<std::byte> buffer) = 0;

    /// Writes all of @p bytes, blocking as long as that takes.
    /// @return Nothing, or why it failed.
    [[nodiscard]] virtual std::expected<void, std::string> writeAll(std::span<std::byte const> bytes) = 0;

    /// Ends the stream for the reader on the far side (shutdown(SHUT_WR), CloseHandle of a pipe's write
    /// end). Idempotent.
    virtual void closeWrite() noexcept = 0;
};

/// Why @ref pumpBytes returned.
enum class PumpEnd : std::uint8_t
{
    SourceEnded,  ///< The source reached its end of stream.
    SourceFailed, ///< Reading the source failed.
    SinkFailed,   ///< Writing the sink failed.
};

/// Copies from @p source to @p sink until the source ends or either side fails.
///
/// Always calls `sink.closeWrite()` before returning, so the end of one stream is the end of the other.
/// @return Why it stopped.
[[nodiscard]] PumpEnd pumpBytes(IByteStream& source, IByteStream& sink);

/// Why @ref runStdioRelay returned.
enum class RelayEnd : std::uint8_t
{
    DaemonEnded, ///< The daemon closed its side.
    Failed,      ///< Reading the daemon or writing the client failed.
};

/// Relays both ways between @p client and @p daemon.
///
/// client→daemon runs on a detached thread that co-owns both streams: a blocking read on process stdin
/// cannot be interrupted portably, and the process exits right after the relay returns, so nothing is
/// left to join it. daemon→client runs on the caller, and its end is the relay's end -- the relay never
/// waits for the client to stop writing.
/// @return Why the daemon→client direction stopped.
[[nodiscard]] RelayEnd runStdioRelay(std::shared_ptr<IByteStream> client,
                                     std::shared_ptr<IByteStream> daemon);

} // namespace vthost
