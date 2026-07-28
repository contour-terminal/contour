// SPDX-License-Identifier: Apache-2.0

// clang-format off
#ifdef _WIN32
    #include <winsock2.h>
    #include <windows.h>
    #include <ws2tcpip.h>
#endif
// clang-format on

#include <net/testing/InMemoryTransport.h>

#include <net/platform/WinsockInit.h>

#ifndef _WIN32
    #include <sys/socket.h>

    #include <array>
    #include <cerrno>

    #include <fcntl.h>
    #include <unistd.h>

    #include <net/posix/PosixSocket.h>
#else
    #include <net/platform/WindowsLoopback.h>
    #include <net/windows/WindowsSocket.h>
#endif

namespace net::testing
{

#ifndef _WIN32

std::expected<SocketPair, NetError> makeSocketPair(EventLoop& loop)
{
    ensureWinsockInitialized();
    auto fds = std::array<int, 2> {};
    if (::socketpair(AF_UNIX, SOCK_STREAM, 0, fds.data()) != 0)
        return std::unexpected(makeNetError(NetErrorCode::Other, errno, "socketpair"));

    for (auto const fd: fds)
    {
        auto const flags = ::fcntl(fd, F_GETFL, 0);
        if (flags >= 0)
            ::fcntl(fd, F_SETFL, flags | O_NONBLOCK);
    }

    return SocketPair {
        .first = std::unique_ptr<ISocket>(new PosixSocket(loop, fds[0])),
        .second = std::unique_ptr<ISocket>(new PosixSocket(loop, fds[1])),
    };
}

#else // _WIN32

std::expected<SocketPair, NetError> makeSocketPair(EventLoop& loop)
{
    ensureWinsockInitialized();
    auto pair = std::array<SOCKET, 2> {};
    if (!makeLoopbackPair(pair)) // the shared production helper (net/platform/WindowsLoopback)
        return std::unexpected(makeNetError(NetErrorCode::Other, WSAGetLastError(), "loopback pair"));

    return SocketPair {
        .first = std::unique_ptr<ISocket>(new WindowsSocket(loop, pair[0])),
        .second = std::unique_ptr<ISocket>(new WindowsSocket(loop, pair[1])),
    };
}

#endif

} // namespace net::testing
