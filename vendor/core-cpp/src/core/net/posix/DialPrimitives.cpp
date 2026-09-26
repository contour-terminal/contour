// SPDX-License-Identifier: Apache-2.0
#include <core/net/detail/DialPrimitives.hpp>

#include <core/net/EventLoop.hpp>
#include <core/net/detail/SocketErrors.hpp>
#include <core/net/posix/FdUtils.hpp>
#include <core/net/posix/PosixSocket.hpp>

#include <sys/socket.h>

#include <cerrno>
#include <memory>
#include <string>
#include <tuple>
#include <utility>

#include <unistd.h>

#include <netinet/in.h>
#include <netinet/tcp.h>

namespace core::net::detail
{

std::expected<DialHandles, NetError> openDialSocket(ResolvedEndpoint const& endpoint)
{
    // Non-blocking AND close-on-exec in one call where the platform allows it; see makeStreamSocket.
    auto const fd = makeStreamSocket(endpoint.family, endpoint.protocol);
    if (fd < 0)
        return std::unexpected(detail::socketError(errno, "socket"));

    // The descriptor IS the readiness object here: poll, epoll and kqueue all watch it directly.
    return DialHandles { .socket = fd, .readiness = fd, .kind = DefaultHandleKind };
}

void closeDialSocket(EventLoop* loop, DialHandles& handles) noexcept
{
    if (!handles.valid())
        return;
    if (loop != nullptr)
        loop->notifyHandleClosing(handles.readiness, FdWakePolicy::Cancel);
    ::close(handles.socket);
    handles = DialHandles {};
}

std::expected<ConnectProgress, NetError> beginConnect(DialHandles const& handles,
                                                      ResolvedEndpoint const& endpoint)
{
    auto const* const address = reinterpret_cast<sockaddr const*>(endpoint.storage.data());
    if (::connect(handles.socket, address, static_cast<socklen_t>(endpoint.length)) == 0)
        return ConnectProgress::Completed;

    auto const err = errno;
    // EINPROGRESS is the documented answer for a non-blocking connect; EALREADY and EAGAIN turn
    // up on a busy AF_UNIX backlog and on some stacks, and each means the same thing here: the
    // kernel took the request and readiness will say how it ended.
    if (err == EINPROGRESS || err == EALREADY || err == EAGAIN)
        return ConnectProgress::Pending;
    return std::unexpected(detail::socketError(err, "connect"));
}

std::expected<void, NetError> pendingSocketError(DialHandles const& handles)
{
    auto pending = 0;
    auto length = static_cast<socklen_t>(sizeof(pending));
    if (::getsockopt(handles.socket, SOL_SOCKET, SO_ERROR, &pending, &length) != 0)
        return std::unexpected(detail::socketError(errno, "getsockopt(SO_ERROR)"));
    if (pending != 0)
        return std::unexpected(detail::socketError(pending, "connect"));
    return {};
}

std::expected<void, NetError> dialableOn(EventLoop& /*loop*/)
{
    return {};
}

SocketResult adoptDialled(EventLoop& loop, DialHandles& handles, std::string peer)
{
    auto const fd = std::exchange(handles.socket, platform::InvalidHandle);
    handles = DialHandles {};
    return std::unique_ptr<ISocket> { new PosixSocket(loop, fd, std::move(peer)) };
}

async::Task<SocketResult> dialCompletion(EventLoop* /*loop*/,
                                         ResolvedEndpoint /*endpoint*/,
                                         platform::SteadyTimePoint /*deadline*/,
                                         StreamSocketOptions /*options*/)
{
    // No loop here lends a completion port, so no connector asks for this; the answer is still a
    // true one rather than an unresolved symbol, because the declaration is portable.
    co_return std::unexpected(
        makeNetError(NetErrorCode::Unsupported, 0, "a completion-port dial on a platform without one"));
}

} // namespace core::net::detail
