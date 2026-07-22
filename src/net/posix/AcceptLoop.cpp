// SPDX-License-Identifier: Apache-2.0
#include <net/posix/AcceptLoop.h>

#ifndef _WIN32

    #include <array>
    #include <cerrno>

    #include <fcntl.h>
    #include <unistd.h>

    #include <arpa/inet.h>
    #include <coro/Cancellation.hpp>
    #include <net/EventLoop.h>
    #include <net/posix/PosixSocket.h>
    #include <netinet/in.h>

namespace net
{

std::string formatPeer(sockaddr_storage const& addr) noexcept
{
    auto buf = std::array<char, INET6_ADDRSTRLEN> {};
    if (addr.ss_family == AF_INET)
    {
        auto const* v4 = reinterpret_cast<sockaddr_in const*>(&addr);
        if (::inet_ntop(AF_INET, &v4->sin_addr, buf.data(), buf.size()) != nullptr)
            return buf.data();
    }
    else if (addr.ss_family == AF_INET6)
    {
        auto const* v6 = reinterpret_cast<sockaddr_in6 const*>(&addr);
        if (::inet_ntop(AF_INET6, &v6->sin6_addr, buf.data(), buf.size()) != nullptr)
            return buf.data();
    }
    return {};
}

coro::Task<AcceptResult> acceptOne(EventLoop* loop, int const* fd, bool const* closed)
{
    while (true)
    {
        if (*closed || *fd < 0)
            co_return std::unexpected(makeNetError(NetErrorCode::Cancelled, 0, "accept on closed listener"));

        auto peer = sockaddr_storage {};
        auto peerLen = socklen_t { sizeof(peer) };
    #ifdef __linux__
        auto const conn =
            ::accept4(*fd, reinterpret_cast<sockaddr*>(&peer), &peerLen, SOCK_NONBLOCK | SOCK_CLOEXEC);
    #else
        auto const conn = ::accept(*fd, reinterpret_cast<sockaddr*>(&peer), &peerLen);
    #endif
        if (conn >= 0)
        {
    #ifndef __linux__
            // Portable fallback: set non-blocking + cloexec explicitly.
            if (auto const flags = ::fcntl(conn, F_GETFL, 0); flags >= 0)
                ::fcntl(conn, F_SETFL, flags | O_NONBLOCK);
            if (auto const fdFlags = ::fcntl(conn, F_GETFD, 0); fdFlags >= 0)
                ::fcntl(conn, F_SETFD, fdFlags | FD_CLOEXEC);
    #endif
            co_return std::unique_ptr<ISocket>(new PosixSocket(*loop, conn, formatPeer(peer)));
        }

        auto const err = errno;
        if (err == EAGAIN || err == EWOULDBLOCK)
        {
            // Park until the listener fd is readable (a connection is pending). A
            // cancelled wait (listener closed / stop requested) throws
            // OperationCancelled, which the accept loop turns into Cancelled.
            try
            {
                co_await loop->waitReadable(*fd);
            }
            catch (coro::OperationCancelled const&)
            {
                co_return std::unexpected(makeNetError(NetErrorCode::Cancelled, 0, "accept cancelled"));
            }
            continue;
        }
        if (err == EINTR || err == ECONNABORTED)
            continue;
        co_return std::unexpected(makeNetError(NetErrorCode::Other, err, "accept"));
    }
}

} // namespace net

#endif // !_WIN32
