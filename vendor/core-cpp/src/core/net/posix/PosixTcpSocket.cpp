// SPDX-License-Identifier: Apache-2.0

/// @file
/// The POSIX halves of an `ITcpSocket`: a non-blocking, close-on-exec socket, and plain `bind` and
/// `listen`. What it becomes is the readiness dial's `PosixSocket` or `adoptListener`'s
/// `PosixListener`.

#include <core/net/detail/SocketErrors.hpp>
#include <core/net/detail/SocketOptionAccess.hpp>
#include <core/net/detail/TcpSocketPrimitives.hpp>
#include <core/net/posix/FdUtils.hpp>

#include <sys/socket.h>

#include <cerrno>
#include <expected>
#include <utility>

#include <unistd.h>

#include <netinet/in.h>

namespace core::net::detail
{

std::expected<DialHandles, NetError> openTcpHandles(AddressFamily family)
{
    auto const fd = makeStreamSocket(family == AddressFamily::Ipv6 ? AF_INET6 : AF_INET, 0);
    if (fd < 0)
        return std::unexpected(socketError(errno, "socket"));

    if (family == AddressFamily::Ipv6)
        if (auto only = setIntOption(fd, NativeOption { .level = IPPROTO_IPV6, .name = IPV6_V6ONLY }, 1);
            !only.has_value())
        {
            ::close(fd);
            return std::unexpected(std::move(only.error()));
        }

    return DialHandles { .socket = fd, .readiness = fd, .kind = DefaultHandleKind };
}

std::expected<void, NetError> bindTcpHandle(platform::NativeHandle socket, ResolvedEndpoint const& local)
{
    auto const* const address = reinterpret_cast<sockaddr const*>(local.storage.data());
    if (::bind(socket, address, static_cast<socklen_t>(local.length)) != 0)
        return std::unexpected(socketError(errno, "bind"));
    return {};
}

std::expected<void, NetError> listenTcpHandle(platform::NativeHandle socket, int backlog)
{
    if (::listen(socket, backlog) != 0)
        return std::unexpected(socketError(errno, "listen"));
    return {};
}

} // namespace core::net::detail
