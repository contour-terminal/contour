// SPDX-License-Identifier: Apache-2.0

// winsock2.h MUST precede windows.h / ws2tcpip.h (which project headers pull in), so this block
// leads every Win32 net translation unit.
// clang-format off
#include <winsock2.h>
#include <windows.h>
#include <ws2tcpip.h>
#include <mstcpip.h>
// clang-format on

#include <core/net/detail/DialPrimitives.hpp>

#include <core/net/EventLoop.hpp>
#include <core/net/Sockets.hpp>
#include <core/net/detail/SocketErrors.hpp>
#include <core/net/windows/InvalidSocket.hpp>
#include <core/net/windows/WinsockError.hpp>
#include <core/platform/WinsockInit.hpp>

#include <memory>
#include <string>
#include <tuple>
#include <utility>

namespace core::net::detail
{

NetError fromWinsockError(int error, std::string context)
{
    // The module's Winsock table (`windows/SocketErrors.cpp`) answers every `WSAE*` code, so a
    // reset, an abort after a FIN and a refused connect mean the same here as on every other
    // Windows transport. Only two rows are the completion's and the dial's own: an ABORT is
    // `Cancelled`, because on a completion port that is what a `close()`, a `cancelRead()` or a stop
    // looks like from the operation's side -- the kernel answers `ERROR_OPERATION_ABORTED`, which
    // is not a `WSAE*` code at all -- and a family or protocol the host lacks is `Unsupported`.
    auto code = NetErrorCode::SystemError;
    switch (error)
    {
        case ERROR_OPERATION_ABORTED: code = NetErrorCode::Cancelled; break;
        case WSAEAFNOSUPPORT:
        case WSAEPROTONOSUPPORT: code = NetErrorCode::Unsupported; break;
        default: code = classifySocketError(error); break;
    }
    return makeNetError(code, error, std::move(context));
}

std::expected<DialHandles, NetError> openDialSocket(ResolvedEndpoint const& endpoint)
{
    platform::ensureWinsockInitialized();

    auto const socket = ::socket(endpoint.family, SOCK_STREAM, endpoint.protocol);
    if (socket == detail::InvalidSocket)
        return std::unexpected(fromWinsockError(WSAGetLastError(), "socket"));

    // **The socket is not what the loop watches here.** Winsock reports readiness for a socket
    // through a `WSAEVENT` associated with it, which the completion port's waitable-handle bridge
    // waits on — so a dial holds two handles where POSIX holds one. `WSAEventSelect` also puts the socket
    // into non-blocking mode, which is what the connect below relies on.
    auto const event = ::WSACreateEvent();
    if (event == WSA_INVALID_EVENT
        || ::WSAEventSelect(socket, event, FD_CONNECT | FD_WRITE | FD_CLOSE) == SOCKET_ERROR)
    {
        auto const err = WSAGetLastError();
        if (event != WSA_INVALID_EVENT)
            ::WSACloseEvent(event);
        ::closesocket(socket);
        return std::unexpected(fromWinsockError(err, "WSAEventSelect"));
    }

    // A dialled socket is created by a plain `::socket`, so it is inheritable unless told
    // otherwise — and a process that dials and also spawns children would hand every child an
    // open peer connection. Best-effort: a failure here costs inheritance hygiene, not the dial.
    std::ignore = ::SetHandleInformation(reinterpret_cast<HANDLE>(socket), HANDLE_FLAG_INHERIT, 0);

    return DialHandles { .socket = reinterpret_cast<platform::NativeHandle>(socket),
                         .readiness = static_cast<platform::NativeHandle>(event),
                         .kind = DefaultHandleKind };
}

void closeDialSocket(EventLoop* loop, DialHandles& handles) noexcept
{
    if (!handles.valid())
        return;
    if (loop != nullptr)
        loop->notifyHandleClosing(handles.readiness, FdWakePolicy::Cancel);
    ::WSACloseEvent(static_cast<WSAEVENT>(handles.readiness));
    ::closesocket(reinterpret_cast<SOCKET>(handles.socket));
    handles = DialHandles {};
}

std::expected<ConnectProgress, NetError> beginConnect(DialHandles const& handles,
                                                      ResolvedEndpoint const& endpoint)
{
    auto const* const address = reinterpret_cast<sockaddr const*>(endpoint.storage.data());
    if (::connect(reinterpret_cast<SOCKET>(handles.socket), address, static_cast<int>(endpoint.length)) == 0)
        return ConnectProgress::Completed;

    auto const err = WSAGetLastError();
    if (err == WSAEWOULDBLOCK || err == WSAEALREADY || err == WSAEINPROGRESS)
        return ConnectProgress::Pending;
    return std::unexpected(fromWinsockError(err, "connect"));
}

std::expected<void, NetError> pendingSocketError(DialHandles const& handles)
{
    auto pending = 0;
    auto length = static_cast<int>(sizeof(pending));
    if (::getsockopt(reinterpret_cast<SOCKET>(handles.socket),
                     SOL_SOCKET,
                     SO_ERROR,
                     reinterpret_cast<char*>(&pending),
                     &length)
        != 0)
        return std::unexpected(fromWinsockError(WSAGetLastError(), "getsockopt(SO_ERROR)"));
    if (pending != 0)
        return std::unexpected(fromWinsockError(pending, "connect"));
    return {};
}

std::expected<void, NetError> dialableOn(EventLoop& loop)
{
    // `adoptSocket` would refuse the socket after the handshake; this says so before one.
    if (loop.completionPort() == nullptr)
        return std::unexpected(
            makeNetError(NetErrorCode::Unsupported, 0, "the loop's backend lends no completion port"));
    return {};
}

SocketResult adoptDialled(EventLoop& loop, DialHandles& handles, std::string peer)
{
    auto const socket = handles.socket;

    // The dial's own event goes with the dial, and the socket's association with it too: the
    // socket is served through the loop's completion port from here on, not through readiness. The
    // loop is told before the event closes, for the reason `closeDialSocket` states. The socket
    // stays non-blocking, which is all an overlapped socket asks of it.
    loop.notifyHandleClosing(handles.readiness, FdWakePolicy::Cancel);
    std::ignore = ::WSAEventSelect(reinterpret_cast<SOCKET>(socket), nullptr, 0);
    ::WSACloseEvent(static_cast<WSAEVENT>(handles.readiness));
    handles = DialHandles {};

    // As every accepted and adopted socket is: `adoptSocket` closes it on failure.
    return adoptSocket(loop, socket, std::move(peer));
}

} // namespace core::net::detail
