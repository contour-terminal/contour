// SPDX-License-Identifier: Apache-2.0
#include <net/Sockets.h>

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

    #include <net/platform/WinsockInit.h>
    #include <net/posix/PosixListener.h>
    #include <net/posix/PosixSocket.h>
    #include <net/posix/UnixListener.h>

namespace net
{

namespace
{
    /// Creates a non-blocking, close-on-exec stream socket portably: Linux sets the
    /// flags atomically on socket(); macOS/BSD (which lack SOCK_NONBLOCK/SOCK_CLOEXEC
    /// as socket() type flags) create it plain and set the flags via fcntl.
    /// @param family Address family (AF_INET / AF_INET6).
    /// @param protocol Protocol (usually 0).
    /// @return The fd, or -1 on failure (errno set).
    [[nodiscard]] int makeStreamSocket(int family, int protocol) noexcept
    {
    #if defined(SOCK_NONBLOCK) && defined(SOCK_CLOEXEC)
        return ::socket(family, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, protocol);
    #else
        auto const fd = ::socket(family, SOCK_STREAM, protocol);
        if (fd < 0)
            return fd;
        if (auto const flags = ::fcntl(fd, F_GETFL, 0); flags >= 0)
            ::fcntl(fd, F_SETFL, flags | O_NONBLOCK);
        if (auto const fdFlags = ::fcntl(fd, F_GETFD, 0); fdFlags >= 0)
            ::fcntl(fd, F_SETFD, fdFlags | FD_CLOEXEC);
        return fd;
    #endif
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
                ::close(fd);
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
        ::close(fd);
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
            ::close(fd);
            co_return std::unexpected(makeNetError(NetErrorCode::Cancelled, 0, "connect cancelled"));
        }
        int soError = 0;
        auto soLen = socklen_t { sizeof(soError) };
        ::getsockopt(fd, SOL_SOCKET, SO_ERROR, &soError, &soLen);
        if (soError == 0)
            co_return std::unique_ptr<ISocket>(new PosixSocket(*loop, fd));
        ::close(fd);
        co_return std::unexpected(makeNetError(
            soError == ECONNREFUSED ? NetErrorCode::ConnRefused : NetErrorCode::Other, soError, "connect"));
    }

    auto const err = errno;
    ::close(fd);
    co_return std::unexpected(
        makeNetError(err == ECONNREFUSED ? NetErrorCode::ConnRefused : NetErrorCode::Other, err, "connect"));
}

} // namespace net

#endif // !_WIN32
