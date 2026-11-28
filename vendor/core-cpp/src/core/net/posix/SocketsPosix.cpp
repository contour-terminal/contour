// SPDX-License-Identifier: Apache-2.0
#include <core/net/SocketAddress.hpp>
#include <core/net/Sockets.hpp>
#include <core/net/detail/SocketEndpoint.hpp>
#include <core/net/detail/SocketErrors.hpp>
#include <core/net/posix/FdUtils.hpp>
#include <core/net/posix/PosixListener.hpp>
#include <core/net/posix/PosixSocket.hpp>
#include <core/net/posix/UnixListener.hpp>
#include <core/platform/WinsockInit.hpp>

#include <sys/socket.h>
#include <sys/un.h>

#include <array>
#include <cassert>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <expected>
#include <filesystem>
#include <span>
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
        co_return std::unexpected(detail::socketError(errno, "socket"));

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
        co_return std::unexpected(detail::socketError(soError, "connect"));
    }

    auto const err = errno;
    discardSocket(loop, fd);
    co_return std::unexpected(detail::socketError(err, "connect"));
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

namespace
{
#if defined(__FreeBSD__) && !defined(EAI_NODATA)
    /// FreeBSD's retired `EAI_NODATA`: its `netdb.h` has it under `#if 0`, but `gai_strerror` still
    /// describes the value, so the table holds a row for it.
    constexpr auto FreeBsdObsoleteNoData = 7;
#endif
#if defined(__FreeBSD__) && !defined(EAI_ADDRFAMILY)
    /// FreeBSD's retired `EAI_ADDRFAMILY`, kept the same way.
    constexpr auto FreeBsdObsoleteAddrFamily = 1;
#endif

    /// Every `EAI_*` code a POSIX libc defines, classified. The codes only some libcs define are
    /// guarded by their own macro, which is the per-OS choice: glibc and macOS name `EAI_NODATA` and
    /// `EAI_ADDRFAMILY`, macOS alone `EAI_BADHINTS` and `EAI_PROTOCOL`, glibc alone the
    /// `getaddrinfo_a` codes. `posix/ResolverErrors_test.cpp` asks `gai_strerror` which codes this
    /// libc knows and fails on one without a row.
    constexpr auto ResolverErrors = std::array {
        // No such name, or no address for it: WASI's `name-unresolvable`. `EAI_NODATA` is a name
        // with no address at all, `EAI_ADDRFAMILY` one with none of the asked family -- with
        // `AF_UNSPEC`, the same thing.
        ResolverErrorRow { .gaiCode = EAI_NONAME, .code = NetErrorCode::NameNotFound },
#ifdef EAI_NODATA
        ResolverErrorRow { .gaiCode = EAI_NODATA, .code = NetErrorCode::NameNotFound },
#elifdef __FreeBSD__
        ResolverErrorRow { .gaiCode = FreeBsdObsoleteNoData, .code = NetErrorCode::NameNotFound },
#endif
#ifdef EAI_ADDRFAMILY
        ResolverErrorRow { .gaiCode = EAI_ADDRFAMILY, .code = NetErrorCode::NameNotFound },
#elifdef __FreeBSD__
        ResolverErrorRow { .gaiCode = FreeBsdObsoleteAddrFamily, .code = NetErrorCode::NameNotFound },
#endif
        // The name server did not answer now: WASI's `temporary-resolver-failure`.
        ResolverErrorRow { .gaiCode = EAI_AGAIN, .code = NetErrorCode::ResolverTemporaryFailure },
        // The name server answered that it cannot: WASI's `permanent-resolver-failure`.
        ResolverErrorRow { .gaiCode = EAI_FAIL, .code = NetErrorCode::AddressError },
        ResolverErrorRow { .gaiCode = EAI_MEMORY, .code = NetErrorCode::ResourceExhausted },
        // The cause is in errno (`detail::resolverSystemCode`).
        ResolverErrorRow { .gaiCode = EAI_SYSTEM, .code = NetErrorCode::SystemError },
        // Refusals of the request itself. The hints and the numeric service are this library's own
        // (`AF_UNSPEC`, `SOCK_STREAM`), so none of these is expected; each is permanent, and the WASI
        // mapping has no third answer for a lookup than not-found, temporary and permanent.
        ResolverErrorRow { .gaiCode = EAI_FAMILY, .code = NetErrorCode::AddressError },
        ResolverErrorRow { .gaiCode = EAI_SOCKTYPE, .code = NetErrorCode::AddressError },
        ResolverErrorRow { .gaiCode = EAI_BADFLAGS, .code = NetErrorCode::AddressError },
        ResolverErrorRow { .gaiCode = EAI_SERVICE, .code = NetErrorCode::AddressError },
        ResolverErrorRow { .gaiCode = EAI_OVERFLOW, .code = NetErrorCode::AddressError },
#ifdef EAI_BADHINTS
        ResolverErrorRow { .gaiCode = EAI_BADHINTS, .code = NetErrorCode::AddressError },
#endif
#ifdef EAI_PROTOCOL
        ResolverErrorRow { .gaiCode = EAI_PROTOCOL, .code = NetErrorCode::AddressError },
#endif
    // glibc's `getaddrinfo_a` and IDN codes. `getaddrinfo` without `AI_IDN` returns none of them;
    // they are rows because `gai_strerror` knows them, and a row is a decision where a missing
    // one would be a guess.
#ifdef EAI_INPROGRESS
        ResolverErrorRow { .gaiCode = EAI_INPROGRESS, .code = NetErrorCode::AddressError },
#endif
#ifdef EAI_CANCELED
        ResolverErrorRow { .gaiCode = EAI_CANCELED, .code = NetErrorCode::AddressError },
#endif
#ifdef EAI_NOTCANCELED
        ResolverErrorRow { .gaiCode = EAI_NOTCANCELED, .code = NetErrorCode::AddressError },
#endif
#ifdef EAI_ALLDONE
        ResolverErrorRow { .gaiCode = EAI_ALLDONE, .code = NetErrorCode::AddressError },
#endif
#ifdef EAI_INTR
        ResolverErrorRow { .gaiCode = EAI_INTR, .code = NetErrorCode::AddressError },
#endif
#ifdef EAI_IDN_ENCODE
        ResolverErrorRow { .gaiCode = EAI_IDN_ENCODE, .code = NetErrorCode::AddressError },
#endif
    };
} // namespace

std::span<ResolverErrorRow const> resolverErrorTable() noexcept
{
    return ResolverErrors;
}

namespace detail
{
    int resolverSystemCode(int gaiCode) noexcept
    {
        return gaiCode == EAI_SYSTEM ? errno : 0;
    }

    std::expected<ResolvedEndpoint, NetError> socketEndpoint(platform::NativeHandle socket, SocketSide side)
    {
        /// The call each side is asked with, indexed by @c SocketSide.
        struct EndpointCall
        {
            decltype(&::getsockname) call; ///< `getsockname` or `getpeername`.
            char const* name;              ///< Its name, for the error's context.
        };
        static constexpr auto Calls = std::array {
            EndpointCall { .call = &::getsockname, .name = "getsockname" },
            EndpointCall { .call = &::getpeername, .name = "getpeername" },
        };
        auto const& [call, name] = Calls.at(static_cast<std::size_t>(side));

        auto storage = sockaddr_storage {};
        auto length = static_cast<socklen_t>(sizeof(storage));
        if (call(socket, reinterpret_cast<sockaddr*>(&storage), &length) != 0)
            return std::unexpected(socketError(errno, name));
        return endpointFromSockaddr(&storage, static_cast<std::uint32_t>(length));
    }
} // namespace detail

} // namespace core::net
