// SPDX-License-Identifier: Apache-2.0
#pragma once

/// @file
/// `detail::applyStreamSocketOptions` and `detail::applySocketBufferSizes` -- the one place a stream
/// socket gets its options, dialled or accepted, on every platform.

#include <core/net/KeepAlive.hpp>
#include <core/net/SocketBuffers.hpp>
#include <core/platform/Types.hpp>

#include <cstddef>

namespace core::net::detail
{

/// What a connected stream socket is configured with beyond what every one of them carries.
///
/// Per dial, from its @c DialOptions: the buffer sizes go on before `connect`
/// (@c applySocketBufferSizes) and keepalive after it (@c applyStreamSocketOptions).
struct StreamSocketOptions
{
    KeepAlive keepAlive = KeepAlive::No; ///< Whether the socket probes a silent peer.
    SocketBufferSizes buffers {};        ///< Kernel buffer sizes; unset ones are left alone.
};

/// Applies the options every connected stream socket carries, and the keepalive @p keepAlive asks
/// for.
///
/// **Every dial and every accept goes through this, on every platform.** Before it existed only
/// the dial set `TCP_NODELAY`, so a server's replies waited on Nagle for the client's delayed ACK
/// while the client's requests did not. What it sets:
///
/// - close-on-exec, where the platform has it (POSIX; a Windows socket is created
///   non-inheritable, and an accepted one inherits that from its listener);
/// - `TCP_NODELAY`, so a small write is not held back behind an unacknowledged one;
/// - keepalive, only if @p keepAlive asks for it -- after and apart from the rest, because it is
///   asked for by one connection rather than carried by all.
///
/// Not the buffer sizes: those are @c applySocketBufferSizes's, before the connection exists.
///
/// Best-effort by contract: an AF_UNIX socket refuses `TCP_NODELAY`, and that is no reason to fail
/// a connection that is otherwise up.
/// @param socket The connected socket.
/// @param keepAlive Whether this socket probes a silent peer.
void applyStreamSocketOptions(platform::NativeHandle socket, KeepAlive keepAlive) noexcept;

/// Asks the kernel for the send and receive buffers @p sizes names, on a socket that is NOT
/// connected yet: a dialled one before `connect`, and a listening one before `listen` -- the
/// sockets it accepts inherit them, an `AcceptEx` one included, whatever it was created with.
///
/// **Before, because the TCP window scale is announced once, in the SYN or SYN-ACK**, and tcp(7)
/// asks for the buffer sizes to be set before `listen` or `connect` for them to count toward it. A
/// receive buffer enlarged after the handshake can outgrow the window the scale lets an end
/// advertise. Linux sizes the scale from `tcp_rmem`'s maximum rather than from the buffer, so there
/// the order seldom shows; it costs nothing, and it is the documented one.
///
/// Best-effort by contract: a kernel may refuse or cap a size, and that is no reason to fail a
/// dial or a bind. An unset size leaves the kernel's value alone.
/// @param socket The unconnected socket.
/// @param sizes The sizes to ask for.
void applySocketBufferSizes(platform::NativeHandle socket, SocketBufferSizes const& sizes) noexcept;

/// What the kernel reports for the options the two functions above set, for tests and
/// diagnostics: asked of the socket rather than trusted from the call.
struct StreamSocketReport
{
    bool noDelay = false;          ///< `TCP_NODELAY`.
    std::size_t sendBuffer = 0;    ///< `SO_SNDBUF`, as reported (Linux reports twice the value set).
    std::size_t receiveBuffer = 0; ///< `SO_RCVBUF`, likewise.
};

/// @param socket A stream socket.
/// @return What its options read back as; a zero for any option the kernel would not report.
[[nodiscard]] StreamSocketReport reportStreamSocketOptions(platform::NativeHandle socket) noexcept;

} // namespace core::net::detail
