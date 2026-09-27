// SPDX-License-Identifier: Apache-2.0
#pragma once

/// @file
/// The OS calls a BLOCKING stream socket and a blocking dial need, behind one platform-free
/// declaration: `posix/BlockingPrimitives.cpp` and `windows/BlockingPrimitives.cpp`, chosen by the
/// source list rather than by an `#ifdef` in the logic (`.agent/rules/platform.md`). Private to
/// `core::net`.
///
/// Every call here blocks the calling thread, bounded only by the socket's own `SO_RCVTIMEO` and
/// `SO_SNDTIMEO`. That is the whole point of the transports built on them and the reason they
/// never run on a loop thread (`.agent/rules/async-and-net.md`, *a synchronous dial spends a thread
/// the caller does not own*).

#include <core/net/NetError.hpp>
#include <core/net/detail/DialPrimitives.hpp>
#include <core/platform/Types.hpp>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <span>

namespace core::net::detail
{

/// Which direction a socket timeout bounds.
enum class IoDirection : std::uint8_t
{
    Receive, ///< `SO_RCVTIMEO`.
    Send,    ///< `SO_SNDTIMEO`.
};

/// Receives up to @p buffer.size() bytes, retrying an interrupted call.
/// @param socket A connected, blocking stream socket.
/// @param buffer The destination; must be non-empty.
/// @return The count, `0` at EOF, or the classified error -- a receive deadline expiring is one
///         @c isDeadlineExpiry recognises.
[[nodiscard]] std::expected<std::size_t, NetError> receiveSome(platform::NativeHandle socket,
                                                               std::span<std::byte> buffer);

/// Waits until @p socket is readable, consuming nothing.
/// @param socket A connected, blocking stream socket.
/// @return `0` when the peer has finished sending, `>0` when bytes are pending, or the error.
[[nodiscard]] std::expected<std::size_t, NetError> waitReadable(platform::NativeHandle socket);

/// Sends a prefix of @p buffer, never raising SIGPIPE, retrying an interrupted call.
/// @param socket A connected, blocking stream socket.
/// @param buffer What to send.
/// @return How many bytes the kernel took, or the classified error.
[[nodiscard]] std::expected<std::size_t, NetError> sendSome(platform::NativeHandle socket,
                                                            std::span<std::byte const> buffer);

/// Closes the write half.
/// @param socket A connected stream socket.
/// @return Nothing, or why the half-close could not be delivered. A peer that is already gone is
///         not a failure: that is the state the caller asked for.
[[nodiscard]] std::expected<void, NetError> shutdownSend(platform::NativeHandle socket);

/// Closes @p socket. Best-effort; @c platform::InvalidHandle is a no-op.
void closeSocket(platform::NativeHandle socket) noexcept;

/// Bounds every later blocking call in @p direction.
/// @param socket The socket.
/// @param direction Which calls to bound.
/// @param timeout How long one call may block; **non-positive removes the bound**, which is the
///        option's own reading of zero and @c ISocket::setReceiveDeadline's contract.
void setIoTimeout(platform::NativeHandle socket,
                  IoDirection direction,
                  std::chrono::milliseconds timeout) noexcept;

/// Arms whatever keeps a write to a broken pipe from raising SIGPIPE on this platform: a socket
/// option where one exists (`SO_NOSIGPIPE`), nothing where the suppression rides on each send
/// (`MSG_NOSIGNAL`, which @c sendSome passes), nothing on Windows, which has no SIGPIPE. **Per
/// socket, never process-wide**: an ignored disposition is inherited across `exec`.
/// @param socket The socket.
void armNoSigPipe(platform::NativeHandle socket) noexcept;

/// Blocks until an outstanding non-blocking dial resolves, or @p timeout passes.
///
/// Resolves is not SUCCEEDS: a refused connect also wakes this, and @c pendingSocketError is what
/// tells them apart.
/// @param handles What @c openDialSocket produced and @c beginConnect left pending.
/// @param timeout How long to wait; non-positive waits for as long as the kernel does.
/// @return Nothing once the dial resolved, @c NetErrorCode::Timeout when the wait ran out, or the
///         error the wait itself reported.
[[nodiscard]] std::expected<void, NetError> waitDialled(DialHandles const& handles,
                                                        std::chrono::milliseconds timeout);

/// Takes the connected socket out of @p handles in BLOCKING mode, releasing whatever readiness
/// object the platform attached for the dial.
/// @param handles The dial's handles; left invalid, because the caller owns the socket now.
/// @return The socket, or why it could not be made blocking -- in which case it has been closed.
[[nodiscard]] std::expected<platform::NativeHandle, NetError> releaseBlocking(DialHandles& handles);

} // namespace core::net::detail
