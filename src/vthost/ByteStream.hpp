// SPDX-License-Identifier: Apache-2.0
#pragma once

/// @file
/// Blocking byte I/O, for the places a reactor cannot reach: process stdio, Windows pipes. What
/// `contour daemon --stdio` relays between, and what bridges a spawned child's pipes to a socket.

#include <chrono>
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
    DaemonEnded,  ///< The daemon closed its side.
    Failed,       ///< Reading the daemon or writing the client failed.
    DaemonSilent, ///< The client spoke, or ended its side, and the daemon sent nothing by the deadline.
};

/// How long the daemon has, by default, to answer a client that has spoken: longer than the daemon's own
/// budget for a hello (PreAuthTimeout), so that a daemon that is merely refusing a silent client closes the
/// connection itself before the relay gives up on it.
inline constexpr auto DefaultDaemonReplyDeadline = std::chrono::milliseconds { 30'000 };

/// What a relay is configured with.
struct RelayConfig
{
    /// How long the daemon has, from the client's first bytes or its half-close, to send its first byte or
    /// end its side. Only the first answer is timed: a session that is merely idle afterwards is not silent.
    std::chrono::milliseconds daemonReplyDeadline = DefaultDaemonReplyDeadline;
};

/// Relays both ways between @p client and @p daemon.
///
/// Each direction runs on a detached thread that co-owns both streams: a blocking read on process stdin or
/// a socket cannot be interrupted portably, and the process exits right after the relay returns, so nothing
/// is left to join it. The caller waits for the daemon→client direction to end -- the relay never waits for
/// the client to stop writing -- or for the daemon to miss @ref RelayConfig::daemonReplyDeadline, after
/// which the client's side is closed and the daemon's read is abandoned to its thread.
/// @param client The attaching client's stream.
/// @param daemon The daemon's stream.
/// @param config The relay's deadline.
/// @return Why the relay stopped.
[[nodiscard]] RelayEnd runStdioRelay(std::shared_ptr<IByteStream> client,
                                     std::shared_ptr<IByteStream> daemon,
                                     RelayConfig config = {});

} // namespace vthost
