// SPDX-License-Identifier: Apache-2.0
#pragma once

/// @file
/// The three OS calls an @c ITcpSocket makes that differ by platform in more than a constant:
/// creating the socket, `bind` and `listen`. Defined in `posix/PosixTcpSocket.cpp` and
/// `windows/IocpTcpSocket.cpp`; the socket's lifecycle itself is written once, in `TcpSocket.cpp`.

#include <core/net/IpSocketAddress.hpp>
#include <core/net/NetError.hpp>
#include <core/net/SocketAddress.hpp>
#include <core/net/detail/DialPrimitives.hpp>
#include <core/platform/Types.hpp>

#include <expected>

namespace core::net::detail
{

/// Creates an unbound TCP socket of @p family, of the kind the platform's dial takes: non-blocking
/// and close-on-exec on POSIX (what @c openDialSocket makes), overlapped and not inheritable on
/// Windows (what `ConnectEx` and `AcceptEx` need). An IPv6 socket is made IPv6-only.
/// @param family The address family.
/// @return The handles -- on Windows with no readiness event, which a completion-port socket does
///         not use -- or the classified OS error.
[[nodiscard]] std::expected<DialHandles, NetError> openTcpHandles(AddressFamily family);

/// `bind`, and nothing else: the reuse-address options are the caller's, from
/// @c reuseAddressOptions.
/// @param socket The socket.
/// @param local The address and port.
/// @return Nothing, or the classified OS error (@c NetErrorCode::AddressInUse for a port in use).
[[nodiscard]] std::expected<void, NetError> bindTcpHandle(platform::NativeHandle socket,
                                                          ResolvedEndpoint const& local);

/// `listen`.
/// @param socket A bound socket.
/// @param backlog The backlog, already clamped to `int`.
/// @return Nothing, or the classified OS error.
[[nodiscard]] std::expected<void, NetError> listenTcpHandle(platform::NativeHandle socket, int backlog);

} // namespace core::net::detail
