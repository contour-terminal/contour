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

#include <net/Sockets.h>

#ifdef _WIN32

    #include <string>

    #include <net/platform/WinsockInit.h>
    #include <net/windows/WindowsListener.h>
    #include <net/windows/WindowsSocket.h>

namespace net
{

std::expected<std::unique_ptr<IListener>, NetError> listen(EventLoop& loop,
                                                           std::string_view host,
                                                           std::uint16_t port,
                                                           int backlog)
{
    ensureWinsockInitialized();
    return WindowsListener::bind(loop, host, port, backlog)
        .transform(
            [](std::unique_ptr<WindowsListener> listener) -> std::unique_ptr<IListener> { return listener; });
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
    if (getaddrinfo(hostStr.c_str(), portStr.c_str(), &hints, &resolved) != 0 || resolved == nullptr)
        co_return std::unexpected(makeNetError(NetErrorCode::AddressError, WSAGetLastError(), "getaddrinfo"));

    NetError lastError = makeNetError(NetErrorCode::AddressError, 0, "no usable address");
    for (auto const* ai = resolved; ai != nullptr; ai = ai->ai_next)
    {
        auto const sock = ::socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
        if (sock == INVALID_SOCKET)
        {
            lastError = makeNetError(NetErrorCode::Other, WSAGetLastError(), "socket");
            continue;
        }

        // Associate a connect-readiness event before the non-blocking connect.
        auto const event = WSACreateEvent();
        if (event == WSA_INVALID_EVENT
            || WSAEventSelect(sock, event, FD_CONNECT | FD_READ | FD_WRITE | FD_CLOSE) == SOCKET_ERROR)
        {
            if (event != WSA_INVALID_EVENT)
                WSACloseEvent(event);
            closesocket(sock);
            lastError = makeNetError(NetErrorCode::Other, WSAGetLastError(), "WSAEventSelect");
            continue;
        }

        auto const rc = ::connect(sock, ai->ai_addr, static_cast<int>(ai->ai_addrlen));
        auto connectErr = (rc == 0) ? 0 : WSAGetLastError();
        if (rc != 0 && connectErr == WSAEWOULDBLOCK)
        {
            try
            {
                co_await loop->waitWritable(static_cast<HANDLE>(event));
            }
            catch (coro::OperationCancelled const&)
            {
                WSACloseEvent(event);
                closesocket(sock);
                freeaddrinfo(resolved);
                co_return std::unexpected(makeNetError(NetErrorCode::Cancelled, 0, "connect cancelled"));
            }
            // Re-check the connect outcome.
            int soError = 0;
            auto soLen = int { sizeof(soError) };
            ::getsockopt(sock, SOL_SOCKET, SO_ERROR, reinterpret_cast<char*>(&soError), &soLen);
            connectErr = soError;
        }

        // The WindowsSocket re-associates the event with its own interest set.
        WSACloseEvent(event);

        if (connectErr == 0)
        {
            freeaddrinfo(resolved);
            co_return std::unique_ptr<ISocket>(new WindowsSocket(*loop, sock));
        }
        lastError =
            makeNetError(connectErr == WSAECONNREFUSED ? NetErrorCode::ConnRefused : NetErrorCode::Other,
                         connectErr,
                         "connect");
        closesocket(sock);
    }
    freeaddrinfo(resolved);
    co_return std::unexpected(lastError);
}

std::expected<std::unique_ptr<IListener>, NetError> listenUnix(EventLoop& /*loop*/,
                                                               std::string_view /*path*/,
                                                               int /*backlog*/)
{
    // Windows 10+ supports AF_UNIX via afunix.h; wiring it (and its distinct
    // security model) up is a self-contained follow-up. Until then the daemon's
    // Windows transport is TCP.
    return std::unexpected(makeNetError(NetErrorCode::Unsupported, 0, "AF_UNIX on Windows"));
}

coro::Task<std::expected<std::unique_ptr<ISocket>, NetError>> connectUnix(EventLoop* /*loop*/,
                                                                          std::string_view /*path*/)
{
    co_return std::unexpected(makeNetError(NetErrorCode::Unsupported, 0, "AF_UNIX on Windows"));
}

std::expected<std::unique_ptr<ISocket>, NetError> adoptFd(EventLoop& /*loop*/, int /*fd*/)
{
    return std::unexpected(makeNetError(NetErrorCode::Unsupported, 0, "adoptFd on Windows"));
}

} // namespace net

#endif // _WIN32
