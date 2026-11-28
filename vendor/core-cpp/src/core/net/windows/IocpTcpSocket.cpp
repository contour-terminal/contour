// SPDX-License-Identifier: Apache-2.0

/// @file
/// The Windows halves of an `ITcpSocket`: an overlapped, non-inheritable socket, which is what
/// `ConnectEx` and `AcceptEx` take, and plain `bind` and `listen`. What it becomes is
/// `dialCompletionOn`'s `IocpSocket` or `adoptListener`'s `IocpListener`; the completion-port
/// association is theirs to make, so a socket that is only bound holds none.

// winsock2.h MUST precede windows.h / ws2tcpip.h (which project headers pull in), so this block
// leads every Win32 net translation unit.
// clang-format off
#include <winsock2.h>
#include <windows.h>
#include <ws2tcpip.h>
// clang-format on

#include <core/net/detail/SocketErrors.hpp>
#include <core/net/detail/SocketOptionAccess.hpp>
#include <core/net/detail/TcpSocketPrimitives.hpp>
#include <core/net/windows/InvalidSocket.hpp>
#include <core/net/windows/WinsockError.hpp>
#include <core/platform/WinsockInit.hpp>

#include <expected>
#include <utility>

namespace core::net::detail
{

std::expected<DialHandles, NetError> openTcpHandles(AddressFamily family)
{
    platform::ensureWinsockInitialized();
    auto const socket = ::WSASocketW(family == AddressFamily::Ipv6 ? AF_INET6 : AF_INET,
                                     SOCK_STREAM,
                                     IPPROTO_TCP,
                                     nullptr,
                                     0,
                                     WSA_FLAG_OVERLAPPED | WSA_FLAG_NO_HANDLE_INHERIT);
    if (socket == InvalidSocket)
        return std::unexpected(fromWinsockError(::WSAGetLastError(), "socket"));
    auto const handle = reinterpret_cast<platform::NativeHandle>(socket);

    // Windows' own default, stated rather than inherited: the one platform rule is IPv6-only.
    if (family == AddressFamily::Ipv6)
        if (auto only = setIntOption(handle, NativeOption { .level = IPPROTO_IPV6, .name = IPV6_V6ONLY }, 1);
            !only.has_value())
        {
            ::closesocket(socket);
            return std::unexpected(std::move(only.error()));
        }

    return DialHandles { .socket = handle };
}

std::expected<void, NetError> bindTcpHandle(platform::NativeHandle socket, ResolvedEndpoint const& local)
{
    auto const* const address = reinterpret_cast<sockaddr const*>(local.storage.data());
    if (::bind(reinterpret_cast<SOCKET>(socket), address, static_cast<int>(local.length)) != 0)
        return std::unexpected(fromWinsockError(::WSAGetLastError(), "bind"));
    return {};
}

std::expected<void, NetError> listenTcpHandle(platform::NativeHandle socket, int backlog)
{
    if (::listen(reinterpret_cast<SOCKET>(socket), backlog) != 0)
        return std::unexpected(fromWinsockError(::WSAGetLastError(), "listen"));
    return {};
}

} // namespace core::net::detail
