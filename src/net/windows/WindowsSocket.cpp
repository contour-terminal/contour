// SPDX-License-Identifier: Apache-2.0
#include <net/windows/WindowsSocket.h>

#ifdef _WIN32

    #include <utility>

namespace net
{

namespace
{
    /// Maps a WSA error to a NetError category.
    [[nodiscard]] NetError fromWsa(int err, std::string context)
    {
        auto code = NetErrorCode::Other;
        switch (err)
        {
            case WSAECONNRESET: code = NetErrorCode::ConnReset; break;
            case WSAECONNREFUSED: code = NetErrorCode::ConnRefused; break;
            case WSAENOTSOCK:
            case WSAEBADF: code = NetErrorCode::BadHandle; break;
            default: break;
        }
        return makeNetError(code, err, std::move(context));
    }
} // namespace

WindowsSocket::WindowsSocket(EventLoop& loop, SOCKET socket, std::string peerAddress) noexcept:
    _loop(loop), _socket(socket), _event(WSACreateEvent()), _peerAddress(std::move(peerAddress))
{
    // Associate the socket's read/write/close readiness with the event so the
    // loop can wait on it. WSAEventSelect also sets the socket non-blocking.
    if (_event != WSA_INVALID_EVENT && _socket != INVALID_SOCKET)
        WSAEventSelect(_socket, _event, FD_READ | FD_WRITE | FD_CLOSE);
}

WindowsSocket::~WindowsSocket()
{
    close();
}

void WindowsSocket::close() noexcept
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

std::optional<NetError> WindowsSocket::closedError(char const* op) const noexcept
{
    if (_closed || _socket == INVALID_SOCKET)
        return makeNetError(NetErrorCode::BadHandle, 0, op);
    return std::nullopt;
}

void WindowsSocket::latchNetworkEvents() noexcept
{
    if (_event == WSA_INVALID_EVENT || _socket == INVALID_SOCKET)
        return;
    auto events = WSANETWORKEVENTS {};
    if (WSAEnumNetworkEvents(_socket, _event, &events) != 0)
        return;
    // FD_CLOSE feeds BOTH latches: a peer that hung up makes recv report EOF and send fail, and
    // whichever direction is parked has to wake up to observe it.
    if ((events.lNetworkEvents & (FD_READ | FD_CLOSE)) != 0)
        _readReady = true;
    if ((events.lNetworkEvents & (FD_WRITE | FD_CLOSE)) != 0)
        _writeReady = true;
}

coro::Task<void> WindowsSocket::parkUntilReady(Ready kind)
{
    auto& latch = kind == Ready::Read ? _readReady : _writeReady;
    // The close() guard is the loop's exit, not an optimization: close() invalidates the event, and
    // a handle that can never signal again would otherwise be parked on for ever — or, since the
    // reactor reports an invalid handle as ready, spun on. Returning hands the decision back to
    // read()/write(), whose own closedError() guard reports it as the BadHandle it is.
    while (!_closed && _event != WSA_INVALID_EVENT)
    {
        // Consume a latched indication WITHOUT touching the shared event. Enumerating here instead
        // would clear the event the OTHER direction may be parked on at this very moment, and that
        // waiter has no way to re-check its latch — it is already suspended. Reading the latch is
        // the only safe thing to do before parking.
        if (std::exchange(latch, false))
            co_return;

        // Nothing latched: park. An indication raised between the syscall that returned
        // WSAEWOULDBLOCK and this point has left the event SIGNALLED, so the wait resolves on the
        // next pump rather than being lost — which is why the event is never reset before a park.
        if (kind == Ready::Read)
            co_await _loop.waitReadable(static_cast<HANDLE>(_event));
        else
            co_await _loop.waitWritable(static_cast<HANDLE>(_event));

        // Woken. Both directions wake together (they share the event), so read-and-clear it into
        // the per-direction latches: whichever gets there first RECORDS the other's indication for
        // it instead of destroying it. Then loop, to consume our own if it is among them.
        latchNetworkEvents();
    }
}

coro::Task<IoResult> WindowsSocket::read(std::span<std::byte> buffer)
{
    while (true)
    {
        if (auto const closed = closedError("read on closed socket"))
            co_return std::unexpected(*closed);

        // Clamp to INT_MAX: ::recv's length parameter is int; a larger buffer
        // would have its size truncated (possibly to a negative value), causing
        // WSAEFAULT and a spurious connection drop.
        auto const n = ::recv(_socket,
                              reinterpret_cast<char*>(buffer.data()),
                              static_cast<int>(std::min(buffer.size(), static_cast<size_t>(INT_MAX))),
                              0);
        if (n > 0)
            co_return static_cast<std::size_t>(n);
        if (n == 0)
            co_return std::size_t { 0 }; // clean EOF

        auto const err = WSAGetLastError();
        if (err == WSAEWOULDBLOCK)
        {
            co_await parkUntilReady(Ready::Read);
            continue;
        }
        co_return std::unexpected(fromWsa(err, "recv"));
    }
}

coro::Task<IoResult> WindowsSocket::write(std::span<std::byte const> buffer)
{
    std::size_t total = 0;
    while (total < buffer.size())
    {
        if (auto const closed = closedError("write on closed socket"))
            co_return std::unexpected(*closed);

        auto const remaining = buffer.subspan(total);
        // Clamp to INT_MAX: ::send's length parameter is int; the write loop
        // handles the remainder on the next iteration, so a truncated length
        // is a safe partial-send rather than a silent failure.
        auto const n = ::send(_socket,
                              reinterpret_cast<char const*>(remaining.data()),
                              static_cast<int>(std::min(remaining.size(), static_cast<size_t>(INT_MAX))),
                              0);
        if (n > 0)
        {
            total += static_cast<std::size_t>(n);
            continue;
        }

        auto const err = WSAGetLastError();
        if (err == WSAEWOULDBLOCK)
        {
            co_await parkUntilReady(Ready::Write);
            continue;
        }
        co_return std::unexpected(fromWsa(err, "send"));
    }
    co_return total;
}

} // namespace net

#endif // _WIN32
