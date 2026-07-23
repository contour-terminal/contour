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

    // A bound AF_UNIX socket is a file-system reparse point carrying this tag. The
    // constant lives in the WDK's ntifs.h; user-mode SDK headers omit it, so define
    // it exactly as libuv and Go's runtime do for their AF_UNIX support.
    #ifndef IO_REPARSE_TAG_AF_UNIX
        #define IO_REPARSE_TAG_AF_UNIX 0x80000023
    #endif

namespace net
{

namespace
{
    /// Probes whether the file at @p path may be reclaimed for binding — the
    /// Windows cousin of UnixListener's owner probe, differing deliberately in
    /// two ways: Windows gets an EXPLICIT file-type guard (only a reparse point
    /// tagged AF_UNIX is a socket file; the POSIX probe needs none because
    /// connect() to a regular file fails, which POSIX then reads as "stale"),
    /// and this connect blocks while the POSIX probe is non-blocking. A live
    /// daemon keeps its path (AddressInUse), and a non-socket file at the path
    /// is never touched — bindUnix must not delete user data that happens to
    /// sit at the chosen socket path.
    /// @param address The AF_UNIX address already populated for @p path.
    /// @param path The socket file path, for diagnostics.
    /// @return Nothing when the path is absent or a stale socket; a NetError otherwise.
    [[nodiscard]] std::expected<void, NetError> probeUnixSocketOwner(sockaddr_un const& address,
                                                                     std::string const& path)
    {
        // FindFirstFileA fails on an absent path and reports the reparse tag on a
        // present one — the single syscall answering both questions.
        auto findData = WIN32_FIND_DATAA {};
        auto const find = ::FindFirstFileA(path.c_str(), &findData);
        if (find == INVALID_HANDLE_VALUE)
        {
            auto const statError = ::GetLastError();
            if (statError == ERROR_FILE_NOT_FOUND || statError == ERROR_PATH_NOT_FOUND)
                return {}; // nothing at the path: safe to bind
            return std::unexpected(
                makeNetError(NetErrorCode::Other, static_cast<int>(statError), "probe stat " + path));
        }
        ::FindClose(find);

        // Only a reparse point tagged AF_UNIX is a socket file; anything else is
        // user data that must never be deleted.
        auto const isSocketFile = (findData.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0
                                  && findData.dwReserved0 == IO_REPARSE_TAG_AF_UNIX;
        if (!isSocketFile)
            return std::unexpected(makeNetError(
                NetErrorCode::AddressInUse, ERROR_FILE_EXISTS, path + " exists and is not a socket file"));

        // A socket file is there: reclaim it only when no live server answers.
        auto const probe = ::socket(AF_UNIX, SOCK_STREAM, 0);
        if (probe == INVALID_SOCKET)
            return std::unexpected(makeNetError(NetErrorCode::Other, WSAGetLastError(), "probe socket"));
        auto const rc = ::connect(probe, reinterpret_cast<sockaddr const*>(&address), sizeof(address));
        auto const err = WSAGetLastError();
        ::closesocket(probe);
        if (rc == 0)
            return std::unexpected(makeNetError(
                NetErrorCode::AddressInUse, WSAEADDRINUSE, "a daemon is already serving " + path));
        if (err == WSAECONNREFUSED)
            return {}; // a crashed server's stale socket
        return std::unexpected(makeNetError(NetErrorCode::Other, err, "probe connect " + path));
    }
} // namespace

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

    // Never hijack a live daemon and never delete user files: probe the path first
    // (exactly as UnixListener::bind does) and reclaim only a stale socket file.
    auto const pathString = std::string { path };
    if (auto const probe = probeUnixSocketOwner(addr, pathString); !probe)
        return std::unexpected(probe.error());
    ::DeleteFileA(pathString.c_str()); // the probe proved this a stale socket

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
