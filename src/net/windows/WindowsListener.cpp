// SPDX-License-Identifier: Apache-2.0

// winsock2.h MUST precede windows.h / ws2tcpip.h (which project headers pull in),
// so this block leads every Win32 net translation unit.
// clang-format off
#ifdef _WIN32
    #include <winsock2.h>
    #include <windows.h>
    #include <ws2tcpip.h>
#endif
// clang-format on

#include <net/windows/WindowsListener.h>

#include <net/windows/WindowsSocket.h>

#ifdef _WIN32

    #include <cstring>
    #include <string>

    #include <afunix.h>

    #include <net/platform/PeerAddress.h>

namespace net
{

WindowsListener::WindowsListener(EventLoop& loop,
                                 SOCKET socket,
                                 WSAEVENT event,
                                 std::uint16_t localPort) noexcept:
    _loop(loop), _socket(socket), _event(event), _localPort(localPort)
{
}

WindowsListener::~WindowsListener()
{
    close();
}

void WindowsListener::close() noexcept
{
    if (_closed)
        return;
    _closed = true;
    if (_event != WSA_INVALID_EVENT)
    {
        WSACloseEvent(_event);
        _event = WSA_INVALID_EVENT;
    }
    if (_socket != INVALID_SOCKET)
    {
        closesocket(_socket);
        _socket = INVALID_SOCKET;
    }
}

std::expected<std::unique_ptr<WindowsListener>, NetError> WindowsListener::bind(EventLoop& loop,
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
        getaddrinfo(hostStr.empty() ? nullptr : hostStr.c_str(), portStr.c_str(), &hints, &resolved);
    if (rc != 0 || resolved == nullptr)
        return std::unexpected(makeNetError(NetErrorCode::AddressError, rc, "getaddrinfo"));

    SOCKET sock = INVALID_SOCKET;
    NetError lastError = makeNetError(NetErrorCode::AddressError, 0, "no usable address");
    for (auto const* ai = resolved; ai != nullptr; ai = ai->ai_next)
    {
        sock = ::socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
        if (sock == INVALID_SOCKET)
        {
            lastError = makeNetError(NetErrorCode::Other, WSAGetLastError(), "socket");
            continue;
        }

        BOOL const one = TRUE;
        ::setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<char const*>(&one), sizeof(one));

        if (::bind(sock, ai->ai_addr, static_cast<int>(ai->ai_addrlen)) == 0 && ::listen(sock, backlog) == 0)
            break; // success

        lastError = makeNetError(WSAGetLastError() == WSAEADDRINUSE ? NetErrorCode::AddressInUse
                                                                    : NetErrorCode::Other,
                                 WSAGetLastError(),
                                 "bind/listen");
        closesocket(sock);
        sock = INVALID_SOCKET;
    }
    freeaddrinfo(resolved);

    if (sock == INVALID_SOCKET)
        return std::unexpected(lastError);

    auto const event = WSACreateEvent();
    if (event == WSA_INVALID_EVENT || WSAEventSelect(sock, event, FD_ACCEPT) == SOCKET_ERROR)
    {
        if (event != WSA_INVALID_EVENT)
            WSACloseEvent(event);
        closesocket(sock);
        return std::unexpected(makeNetError(NetErrorCode::Other, WSAGetLastError(), "WSAEventSelect"));
    }

    auto bound = sockaddr_storage {};
    auto boundLen = int { sizeof(bound) };
    std::uint16_t actualPort = port;
    if (::getsockname(sock, reinterpret_cast<sockaddr*>(&bound), &boundLen) == 0)
    {
        if (bound.ss_family == AF_INET)
            actualPort = ntohs(reinterpret_cast<sockaddr_in const*>(&bound)->sin_port);
        else if (bound.ss_family == AF_INET6)
            actualPort = ntohs(reinterpret_cast<sockaddr_in6 const*>(&bound)->sin6_port);
    }

    return std::unique_ptr<WindowsListener>(new WindowsListener(loop, sock, event, actualPort));
}

std::expected<std::unique_ptr<WindowsListener>, NetError> WindowsListener::bindUnix(EventLoop& loop,
                                                                                    std::string_view path,
                                                                                    int backlog)
{
    auto addr = sockaddr_un {};
    addr.sun_family = AF_UNIX;
    if (path.size() >= sizeof(addr.sun_path))
        return std::unexpected(makeNetError(NetErrorCode::AddressError, 0, "unix socket path too long"));
    std::memcpy(addr.sun_path, path.data(), path.size());

    ::DeleteFileA(std::string { path }.c_str()); // a stale socket file blocks bind

    auto const sock = ::socket(AF_UNIX, SOCK_STREAM, 0);
    if (sock == INVALID_SOCKET)
        return std::unexpected(makeNetError(NetErrorCode::Unsupported, WSAGetLastError(), "socket(AF_UNIX)"));

    if (::bind(sock, reinterpret_cast<sockaddr const*>(&addr), sizeof(addr)) != 0
        || ::listen(sock, backlog) != 0)
    {
        auto const err = WSAGetLastError();
        closesocket(sock);
        return std::unexpected(
            makeNetError(err == WSAEADDRINUSE ? NetErrorCode::AddressInUse : NetErrorCode::Other,
                         err,
                         "bind/listen unix"));
    }

    auto const event = WSACreateEvent();
    if (event == WSA_INVALID_EVENT || WSAEventSelect(sock, event, FD_ACCEPT) == SOCKET_ERROR)
    {
        if (event != WSA_INVALID_EVENT)
            WSACloseEvent(event);
        closesocket(sock);
        return std::unexpected(makeNetError(NetErrorCode::Other, WSAGetLastError(), "WSAEventSelect"));
    }

    return std::unique_ptr<WindowsListener>(new WindowsListener(loop, sock, event, /*localPort=*/0));
}

coro::Task<AcceptResult> WindowsListener::accept()
{
    while (true)
    {
        if (_closed || _socket == INVALID_SOCKET)
            co_return std::unexpected(makeNetError(NetErrorCode::Cancelled, 0, "accept on closed listener"));

        auto peer = sockaddr_storage {};
        auto peerLen = int { sizeof(peer) };
        auto const conn = ::accept(_socket, reinterpret_cast<sockaddr*>(&peer), &peerLen);
        if (conn != INVALID_SOCKET)
            co_return std::unique_ptr<ISocket>(new WindowsSocket(_loop, conn, formatPeer(peer)));

        auto const err = WSAGetLastError();
        if (err == WSAEWOULDBLOCK)
        {
            WSAResetEvent(_event);
            try
            {
                co_await _loop.waitReadable(static_cast<HANDLE>(_event));
            }
            catch (coro::OperationCancelled const&)
            {
                co_return std::unexpected(makeNetError(NetErrorCode::Cancelled, 0, "accept cancelled"));
            }
            continue;
        }
        co_return std::unexpected(makeNetError(NetErrorCode::Other, err, "accept"));
    }
}

} // namespace net

#endif // _WIN32
