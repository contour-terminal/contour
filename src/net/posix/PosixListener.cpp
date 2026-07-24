// SPDX-License-Identifier: Apache-2.0
#include <net/posix/PosixListener.h>

#ifndef _WIN32

    #include <sys/socket.h>

    #include <cerrno>

    #include <netdb.h>
    #include <unistd.h>

    #include <arpa/inet.h>
    #include <net/posix/AcceptLoop.h>
    #include <net/posix/FdUtils.h>
    #include <netinet/in.h>

namespace net
{

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
    // The shared loop records the TCP peer's printable host via formatPeer.
    return acceptOne(&_loop, &_fd, &_closed);
}

} // namespace net

#endif // !_WIN32
