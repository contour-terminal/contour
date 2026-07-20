// SPDX-License-Identifier: Apache-2.0
#include <net/posix/PosixListener.h>

#include <net/posix/PosixSocket.h>

#ifndef _WIN32

    #include <sys/socket.h>

    #include <array>
    #include <cerrno>

    #include <fcntl.h>
    #include <netdb.h>
    #include <unistd.h>

    #include <arpa/inet.h>
    #include <netinet/in.h>

namespace net
{

namespace
{
    /// Makes @p fd non-blocking and close-on-exec.
    /// @return True on success.
    [[nodiscard]] bool makeNonBlockingCloexec(int fd) noexcept
    {
        auto flags = ::fcntl(fd, F_GETFL, 0);
        if (flags < 0 || ::fcntl(fd, F_SETFL, flags | O_NONBLOCK) < 0)
            return false;
        auto fdFlags = ::fcntl(fd, F_GETFD, 0);
        return fdFlags >= 0 && ::fcntl(fd, F_SETFD, fdFlags | FD_CLOEXEC) >= 0;
    }

    /// Formats a connected peer's address as a printable host string.
    /// @param addr The peer's address storage.
    /// @return The printable host, or "" if it cannot be formatted.
    [[nodiscard]] std::string formatPeer(sockaddr_storage const& addr) noexcept
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
} // namespace

PosixListener::PosixListener(EventLoop& loop, int fd, std::uint16_t localPort) noexcept:
    _loop(loop), _fd(fd), _localPort(localPort)
{
}

PosixListener::~PosixListener()
{
    close();
}

void PosixListener::close() noexcept
{
    if (_closed)
        return;
    _closed = true;
    if (_fd >= 0)
    {
        ::close(_fd);
        _fd = -1;
    }
}

std::expected<std::unique_ptr<PosixListener>, NetError> PosixListener::bind(EventLoop& loop,
                                                                            std::string_view host,
                                                                            std::uint16_t port,
                                                                            int backlog)
{
    auto hints = addrinfo {};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_flags = AI_PASSIVE | AI_NUMERICSERV;

    auto const hostStr = std::string { host };
    auto const portStr = std::to_string(port);

    addrinfo* resolved = nullptr;
    auto const rc =
        ::getaddrinfo(hostStr.empty() ? nullptr : hostStr.c_str(), portStr.c_str(), &hints, &resolved);
    if (rc != 0 || resolved == nullptr)
        return std::unexpected(makeNetError(NetErrorCode::AddressError, rc, "getaddrinfo"));

    int fd = -1;
    NetError lastError = makeNetError(NetErrorCode::AddressError, 0, "no usable address");
    for (auto const* ai = resolved; ai != nullptr; ai = ai->ai_next)
    {
        fd = ::socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
        if (fd < 0)
        {
            lastError = makeNetError(NetErrorCode::Other, errno, "socket");
            continue;
        }

        int const one = 1;
        ::setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));

        if (::bind(fd, ai->ai_addr, ai->ai_addrlen) == 0 && ::listen(fd, backlog) == 0
            && makeNonBlockingCloexec(fd))
            break; // success

        lastError = makeNetError(
            errno == EADDRINUSE ? NetErrorCode::AddressInUse : NetErrorCode::Other, errno, "bind/listen");
        ::close(fd);
        fd = -1;
    }
    ::freeaddrinfo(resolved);

    if (fd < 0)
        return std::unexpected(lastError);

    // Read back the actual bound port (it may have been an OS-assigned ephemeral).
    auto bound = sockaddr_storage {};
    auto boundLen = socklen_t { sizeof(bound) };
    std::uint16_t actualPort = port;
    if (::getsockname(fd, reinterpret_cast<sockaddr*>(&bound), &boundLen) == 0)
    {
        if (bound.ss_family == AF_INET)
            actualPort = ntohs(reinterpret_cast<sockaddr_in const*>(&bound)->sin_port);
        else if (bound.ss_family == AF_INET6)
            actualPort = ntohs(reinterpret_cast<sockaddr_in6 const*>(&bound)->sin6_port);
    }

    return std::unique_ptr<PosixListener>(new PosixListener(loop, fd, actualPort));
}

coro::Task<AcceptResult> PosixListener::accept()
{
    while (true)
    {
        if (_closed || _fd < 0)
            co_return std::unexpected(makeNetError(NetErrorCode::Cancelled, 0, "accept on closed listener"));

        auto peer = sockaddr_storage {};
        auto peerLen = socklen_t { sizeof(peer) };
    #ifdef __linux__
        auto const conn =
            ::accept4(_fd, reinterpret_cast<sockaddr*>(&peer), &peerLen, SOCK_NONBLOCK | SOCK_CLOEXEC);
    #else
        auto const conn = ::accept(_fd, reinterpret_cast<sockaddr*>(&peer), &peerLen);
    #endif
        if (conn >= 0)
        {
    #ifndef __linux__
            // Portable fallback: set non-blocking + cloexec explicitly.
            auto flags = ::fcntl(conn, F_GETFL, 0);
            if (flags >= 0)
                ::fcntl(conn, F_SETFL, flags | O_NONBLOCK);
            auto fdFlags = ::fcntl(conn, F_GETFD, 0);
            if (fdFlags >= 0)
                ::fcntl(conn, F_SETFD, fdFlags | FD_CLOEXEC);
    #endif
            co_return std::unique_ptr<ISocket>(new PosixSocket(_loop, conn, formatPeer(peer)));
        }

        auto const err = errno;
        if (err == EAGAIN || err == EWOULDBLOCK)
        {
            // Park until the listener fd is readable (a connection is pending). A
            // cancelled wait (listener closed / stop requested) throws
            // OperationCancelled, which the accept loop turns into Cancelled.
            try
            {
                co_await _loop.waitReadable(_fd);
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
