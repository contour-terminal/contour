// SPDX-License-Identifier: Apache-2.0
#include <net/Sockets.hpp>

#ifndef _WIN32

    #include <sys/socket.h>
    #include <sys/un.h>

    #include <cerrno>
    #include <cstring>
    #include <filesystem>
    #include <string>

    #include <fcntl.h>
    #include <netdb.h>
    #include <unistd.h>

    #include <net/platform/WinsockInit.hpp>
    #include <net/posix/FdUtils.hpp>
    #include <net/posix/PosixListener.hpp>
    #include <net/posix/PosixSocket.hpp>
    #include <net/posix/UnixListener.hpp>

namespace net
{

namespace
{
    /// Closes a descriptor the connect path is abandoning, announcing it to the loop
    /// first.
    ///
    /// Nothing is normally parked on it by this point — the awaiter detached in its
    /// own await_resume — but the loop's contract is that a descriptor which may be
    /// registered is announced BEFORE it closes, and honouring that unconditionally
    /// is what keeps a second waiter on the same descriptor from being stranded on
    /// epoll or kqueue, neither of which can report a closed one.
    /// @param loop The loop the descriptor was registered with.
    /// @param fd The descriptor to close.
    void discardSocket(EventLoop* loop, int fd) noexcept
    {
        loop->notifyHandleClosing(fd, FdWakePolicy::Cancel);
        ::close(fd);
    }
} // namespace

std::expected<std::unique_ptr<IListener>, NetError> listen(EventLoop& loop,
                                                           std::string_view host,
                                                           std::uint16_t port,
                                                           int backlog)
{
    ensureWinsockInitialized();
    return PosixListener::bind(loop, host, port, backlog)
        .transform(
            [](std::unique_ptr<PosixListener> listener) -> std::unique_ptr<IListener> { return listener; });
}

coro::Task<std::expected<std::unique_ptr<ISocket>, NetError>> connect(EventLoop* loop,
                                                                      std::string_view host,
                                                                      std::uint16_t port)
{
    ensureWinsockInitialized();
    auto hints = addrinfo {};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_flags = AI_NUMERICSERV;

    auto const hostStr = std::string { host };
    auto const portStr = std::to_string(port);

    addrinfo* resolved = nullptr;
    auto const rc = ::getaddrinfo(hostStr.c_str(), portStr.c_str(), &hints, &resolved);
    if (rc != 0 || resolved == nullptr)
        co_return std::unexpected(makeNetError(NetErrorCode::AddressError, rc, "getaddrinfo"));

    NetError lastError = makeNetError(NetErrorCode::AddressError, 0, "no usable address");
    for (auto const* ai = resolved; ai != nullptr; ai = ai->ai_next)
    {
        auto const fd = makeStreamSocket(ai->ai_family, ai->ai_protocol);
        if (fd < 0)
        {
            lastError = makeNetError(NetErrorCode::Other, errno, "socket");
            continue;
        }

        auto const rcConnect = ::connect(fd, ai->ai_addr, ai->ai_addrlen);
        if (rcConnect == 0)
        {
            ::freeaddrinfo(resolved);
            co_return std::unique_ptr<ISocket>(new PosixSocket(*loop, fd));
        }
        if (errno == EINPROGRESS)
        {
            // Non-blocking connect in progress: park until writable, then check the
            // pending socket error to learn whether it succeeded.
            try
            {
                co_await loop->waitWritable(fd);
            }
            catch (coro::OperationCancelled const&)
            {
                discardSocket(loop, fd);
                ::freeaddrinfo(resolved);
                co_return std::unexpected(makeNetError(NetErrorCode::Cancelled, 0, "connect cancelled"));
            }

            int soError = 0;
            auto soLen = socklen_t { sizeof(soError) };
            ::getsockopt(fd, SOL_SOCKET, SO_ERROR, &soError, &soLen);
            if (soError == 0)
            {
                ::freeaddrinfo(resolved);
                co_return std::unique_ptr<ISocket>(new PosixSocket(*loop, fd));
            }
            lastError =
                makeNetError(soError == ECONNREFUSED ? NetErrorCode::ConnRefused : NetErrorCode::Other,
                             soError,
                             "connect");
        }
        else
        {
            lastError = makeNetError(
                errno == ECONNREFUSED ? NetErrorCode::ConnRefused : NetErrorCode::Other, errno, "connect");
        }
        discardSocket(loop, fd);
    }
    ::freeaddrinfo(resolved);
    co_return std::unexpected(lastError);
}

std::expected<std::unique_ptr<IListener>, NetError> listenUnix(EventLoop& loop,
                                                               std::string_view path,
                                                               int backlog)
{
    return UnixListener::bind(loop, std::filesystem::path { path }, backlog)
        .transform(
            [](std::unique_ptr<UnixListener> listener) -> std::unique_ptr<IListener> { return listener; });
}

coro::Task<std::expected<std::unique_ptr<ISocket>, NetError>> connectUnix(EventLoop* loop,
                                                                          std::string_view path)
{
    auto address = sockaddr_un {};
    if (path.size() >= sizeof(address.sun_path))
        co_return std::unexpected(
            makeNetError(NetErrorCode::AddressError, ENAMETOOLONG, "socket path too long"));
    address.sun_family = AF_UNIX;
    std::memcpy(address.sun_path, path.data(), path.size());
    address.sun_path[path.size()] = '\0';

    auto const fd = makeStreamSocket(AF_UNIX, 0);
    if (fd < 0)
        co_return std::unexpected(makeNetError(NetErrorCode::Other, errno, "socket"));

    auto const rc = ::connect(fd, reinterpret_cast<sockaddr const*>(&address), sizeof(address));
    if (rc == 0)
        co_return std::unique_ptr<ISocket>(new PosixSocket(*loop, fd));

    // A non-blocking AF_UNIX connect defers with EINPROGRESS (rarely, EAGAIN when
    // the server's backlog is full): park until writable, then read the outcome.
    if (errno == EINPROGRESS || errno == EAGAIN)
    {
        try
        {
            co_await loop->waitWritable(fd);
        }
        catch (coro::OperationCancelled const&)
        {
            discardSocket(loop, fd);
            co_return std::unexpected(makeNetError(NetErrorCode::Cancelled, 0, "connect cancelled"));
        }
        int soError = 0;
        auto soLen = socklen_t { sizeof(soError) };
        ::getsockopt(fd, SOL_SOCKET, SO_ERROR, &soError, &soLen);
        if (soError == 0)
            co_return std::unique_ptr<ISocket>(new PosixSocket(*loop, fd));
        discardSocket(loop, fd);
        co_return std::unexpected(makeNetError(
            soError == ECONNREFUSED ? NetErrorCode::ConnRefused : NetErrorCode::Other, soError, "connect"));
    }

    auto const err = errno;
    discardSocket(loop, fd);
    co_return std::unexpected(
        makeNetError(err == ECONNREFUSED ? NetErrorCode::ConnRefused : NetErrorCode::Other, err, "connect"));
}

std::expected<std::unique_ptr<ISocket>, NetError> adoptFd(EventLoop& loop, int fd)
{
    if (fd < 0)
        return std::unexpected(makeNetError(NetErrorCode::Other, EBADF, "adoptFd"));
    // The reactor requires non-blocking I/O; the descriptor may be a PTY
    // master or socketpair end created without it.
    if (auto const flags = ::fcntl(fd, F_GETFL, 0); flags >= 0)
        ::fcntl(fd, F_SETFL, flags | O_NONBLOCK);
    if (auto const fdFlags = ::fcntl(fd, F_GETFD, 0); fdFlags >= 0)
        ::fcntl(fd, F_SETFD, fdFlags | FD_CLOEXEC);
    return std::unique_ptr<ISocket>(new PosixSocket(loop, fd));
}

} // namespace net

#endif // !_WIN32
