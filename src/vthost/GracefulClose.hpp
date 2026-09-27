// SPDX-License-Identifier: Apache-2.0
#pragma once

/// @file
/// The orderly end of a connection: flush what is queued, say so, then close.

#include <core/async/Task.hpp>
#include <core/net/EventLoop.hpp>
#include <core/net/ISocket.hpp>
#include <core/net/WithTimeout.hpp>
#include <core/net/WriteQueue.hpp>

#include <chrono>
#include <tuple>

namespace vthost
{

/// How long an orderly close may take before the connection is closed anyway.
inline constexpr auto DefaultGracefulCloseDeadline = std::chrono::milliseconds { 2000 };

namespace detail
{
    /// The orderly half of closeGracefully: flush, then half-close.
    [[nodiscard]] inline core::async::Task<void> flushThenHalfClose(core::net::EventLoop* loop,
                                                                    core::net::WriteQueue* writer,
                                                                    core::net::ISocket* socket)
    {
        co_await core::net::pollUntil(loop, [writer] {
            return (writer->backlogBytes() == 0 && !writer->draining()) || writer->failure().has_value();
        });
        std::ignore = co_await socket->shutdownWrite();
    }
} // namespace detail

/// Flushes @p writer, half-closes @p socket, then closes both -- the close within @p deadline
/// whatever the peer does.
///
/// The half-close is what makes the end orderly: over TLS it writes close_notify, and a peer that
/// reads a TCP FIN without one reports the stream as truncated -- so every ordinary disconnect
/// would reach the other side's log as a failed read. Over a plain socket it is the FIN, which
/// the close would have sent anyway.
///
/// Both halves can wait on the peer: the flush for it to read what is queued, and TLS's
/// close_notify for room to be written. A peer that stopped reading would hold them forever, so at
/// @p deadline they are cancelled and the connection is closed without them.
///
/// A queue that already failed or was closed has nothing left to flush; the half-close then fails
/// too, and that failure is not worth reporting: the connection is ending either way.
/// @param loop The loop the connection runs on (not owned; a pointer, since coroutine reference
///        parameters can dangle).
/// @param writer The connection's write queue, over @p socket (not owned).
/// @param socket The connection (not owned).
/// @param deadline How long the orderly part may take.
[[nodiscard]] inline core::async::Task<void> closeGracefully(
    core::net::EventLoop* loop,
    core::net::WriteQueue* writer,
    core::net::ISocket* socket,
    std::chrono::milliseconds deadline = DefaultGracefulCloseDeadline)
{
    std::ignore =
        co_await core::net::withTimeout(loop, detail::flushThenHalfClose(loop, writer, socket), deadline);
    // The queue closes the socket, which resumes any read still parked on it: nothing after this.
    writer->close();
}

} // namespace vthost
