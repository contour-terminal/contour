// SPDX-License-Identifier: Apache-2.0
#pragma once

/// @file
/// `ISocket` — a connected, bidirectional byte transport whose I/O is expressed as
/// `coro::Task<IoResult>`, so it composes with the loop's coroutine
/// vocabulary (`whenAny`, `withTimeout`, cancellation) natively.
///
/// Implementations: @c PosixSocket / @c WindowsSocket (production, reactor-driven
/// non-blocking I/O), and @c InMemoryTransport (deterministic, in-process, for
/// tests). All are driven by an @c EventLoop: a slow read/write parks the calling
/// coroutine via `loop.waitReadable/waitWritable` rather than blocking a thread.

#include <cstddef>
#include <span>
#include <string>

#include <coro/Task.hpp>
#include <net/IoResult.h>

namespace net
{

/// A connected, streamed, bidirectional byte transport.
///
/// The buffer passed to @c read / @c write must stay valid until the returned task
/// completes (the operation may suspend and resume across reactor frames).
class ISocket
{
  public:
    ISocket() = default;
    virtual ~ISocket() = default;

    ISocket(ISocket const&) = delete;
    ISocket& operator=(ISocket const&) = delete;
    ISocket(ISocket&&) = delete;
    ISocket& operator=(ISocket&&) = delete;

    /// Reads up to @p buffer.size() bytes into @p buffer.
    /// @param buffer Destination span; must outlive the returned task.
    /// @return A task resolving to the byte count read, 0 on clean EOF, or a
    ///         @c NetError on failure.
    [[nodiscard]] virtual coro::Task<IoResult> read(std::span<std::byte> buffer) = 0;

    /// Writes all of @p buffer's bytes (looping over partial writes / backpressure).
    /// @param buffer Source span; must outlive the returned task.
    /// @return A task resolving to the byte count written (== buffer.size() on
    ///         success), or a @c NetError on failure.
    [[nodiscard]] virtual coro::Task<IoResult> write(std::span<std::byte const> buffer) = 0;

    /// @return The remote peer's printable address ("127.0.0.1", "::1"), or "" if
    ///         unknown (e.g. the in-memory transport).
    [[nodiscard]] virtual std::string peerAddress() const { return {}; }

    /// Closes the socket. Idempotent; a pending or subsequent read/write resolves
    /// with @c NetErrorCode::BadHandle.
    virtual void close() noexcept = 0;

    /// @return True once @c close() has been called or the peer closed and a read
    ///         observed EOF.
    [[nodiscard]] virtual bool isClosed() const noexcept = 0;
};

} // namespace net
