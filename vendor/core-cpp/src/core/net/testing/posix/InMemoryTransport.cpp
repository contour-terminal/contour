// SPDX-License-Identifier: Apache-2.0
#include <core/net/testing/InMemoryTransport.hpp>

#include <core/net/posix/PosixSocket.hpp>
#include <core/platform/WinsockInit.hpp>

#include <sys/socket.h>

#include <array>
#include <cerrno>

#include <fcntl.h>
#include <unistd.h>

namespace core::net::testing
{

std::expected<SocketPair, NetError> makeSocketPair(EventLoop& loop)
{
    platform::ensureWinsockInitialized();
    auto fds = std::array<int, 2> {};
    if (::socketpair(AF_UNIX, SOCK_STREAM, 0, fds.data()) != 0)
        return std::unexpected(makeNetError(NetErrorCode::SystemError, errno, "socketpair"));

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

} // namespace core::net::testing
