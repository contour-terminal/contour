// SPDX-License-Identifier: Apache-2.0
#include <core/net/Sockets.hpp>
#include <core/net/posix/FdUtils.hpp>
#include <core/net/posix/PosixListener.hpp>
#include <core/net/posix/PosixSocket.hpp>
#include <core/net/posix/UnixListener.hpp>
#include <core/platform/WinsockInit.hpp>

#include <sys/socket.h>
#include <sys/un.h>

#include <cassert>
#include <cerrno>
#include <cstring>
#include <filesystem>
#include <string>
#include <utility>

#include <fcntl.h>
#include <netdb.h>
#include <unistd.h>

namespace core::net
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

std::expected<std::unique_ptr<IListener>, NetError> listen(EventLoop& loop, ListenOptions options)
{
    platform::ensureWinsockInitialized();
    return PosixListener::bind(
               loop, options.host, options.port, options.backlog, options.sharing, options.buffers)
        .transform(
            [](std::unique_ptr<PosixListener> listener) -> std::unique_ptr<IListener> { return listener; });
}

std::expected<std::unique_ptr<IListener>, NetError> listen(EventLoop& loop,
                                                           std::string_view host,
                                                           std::uint16_t port,
                                                           int backlog)
{
    return listen(loop, ListenOptions { .host = host, .port = port, .backlog = backlog });
}

std::expected<std::unique_ptr<IListener>, NetError> adoptListener(EventLoop& loop,
                                                                  platform::NativeHandle handle)
{
    if (handle < 0)
        return std::unexpected(makeNetError(NetErrorCode::BadHandle, EBADF, "adoptListener"));
    return PosixListener::adopt(loop, handle)
        .transform(
            [](std::unique_ptr<PosixListener> listener) -> std::unique_ptr<IListener> { return listener; });
}

std::expected<std::unique_ptr<IListener>, NetError> listenUnix(EventLoop& loop,
                                                               std::string_view path,
                                                               int backlog)
{
    return UnixListener::bind(loop, std::filesystem::path { path }, backlog)
        .transform(
            [](std::unique_ptr<UnixListener> listener) -> std::unique_ptr<IListener> { return listener; });
}

async::Task<std::expected<std::unique_ptr<ISocket>, NetError>> connectUnix(EventLoop* loop,
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
        co_return std::unexpected(makeNetError(NetErrorCode::SystemError, errno, "socket"));

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
        catch (async::OperationCancelled const&)
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
        co_return std::unexpected(
            makeNetError(soError == ECONNREFUSED ? NetErrorCode::ConnRefused : NetErrorCode::SystemError,
                         soError,
                         "connect"));
    }

    auto const err = errno;
    discardSocket(loop, fd);
    co_return std::unexpected(makeNetError(
        err == ECONNREFUSED ? NetErrorCode::ConnRefused : NetErrorCode::SystemError, err, "connect"));
}

std::expected<std::unique_ptr<ISocket>, NetError> adoptSocket(EventLoop& loop,
                                                              platform::NativeHandle handle,
                                                              std::string peerAddress)
{
    assert(loop.teardownIsSerialisedWithDispatch()
           && "adoptSocket off the loop's thread: the socket would join registrations another thread "
              "is dispatching");
    if (handle < 0)
        return std::unexpected(makeNetError(NetErrorCode::BadHandle, EBADF, "adoptSocket"));
    // Non-blocking is what the reactor needs to work at all, not an option chosen for the caller:
    // an accepted socket inherits blocking mode, and a blocking read here would stall the loop.
    if (auto const flags = ::fcntl(handle, F_GETFL, 0);
        flags < 0 || ::fcntl(handle, F_SETFL, flags | O_NONBLOCK) != 0)
    {
        auto const error = errno;
        ::close(handle);
        return std::unexpected(makeNetError(NetErrorCode::SystemError, error, "adoptSocket: O_NONBLOCK"));
    }
    try
    {
        return std::unique_ptr<ISocket>(new PosixSocket(loop, handle, std::move(peerAddress)));
    }
    catch (...)
    {
        // Allocating the wrapper threw: nothing owns the descriptor, and this call promised to
        // close it.
        ::close(handle);
        throw;
    }
}

std::expected<std::unique_ptr<ISocket>, NetError> adoptFd(EventLoop& loop, int fd)
{
    if (fd < 0)
        return std::unexpected(makeNetError(NetErrorCode::SystemError, EBADF, "adoptFd"));
    // The reactor requires non-blocking I/O; the descriptor may be a PTY
    // master or socketpair end created without it.
    if (auto const flags = ::fcntl(fd, F_GETFL, 0); flags >= 0)
        ::fcntl(fd, F_SETFL, flags | O_NONBLOCK);
    if (auto const fdFlags = ::fcntl(fd, F_GETFD, 0); fdFlags >= 0)
        ::fcntl(fd, F_SETFD, fdFlags | FD_CLOEXEC);
    return std::unique_ptr<ISocket>(new PosixSocket(loop, fd));
}

} // namespace core::net
