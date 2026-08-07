// SPDX-License-Identifier: Apache-2.0
#pragma once

#ifdef _WIN32

    #include <net/ISocket.hpp>

// clang-format off
    #include <winsock2.h>
    #include <windows.h>
// clang-format on

    #include <cstddef>
    #include <optional>
    #include <span>
    #include <string>

    #include <coro/Task.hpp>
    #include <net/EventLoop.hpp>

namespace net
{

/// A reactor-driven, non-blocking Windows stream socket. Because a SOCKET is not
/// directly waitable by WaitForMultipleObjects, readiness is observed through a
/// WSAEVENT associated with the socket via WSAEventSelect (the same technique as
/// platform::SystemPipe): the loop waits on the event, while recv/send operate
/// on the socket. read/write try the syscall and, on WSAEWOULDBLOCK, park on the
/// event until ready, then retry.
///
/// One event serves BOTH directions (WSAEventSelect permits no more), so the indications are
/// consumed through `WSAEnumNetworkEvents` and latched per direction — @see latchNetworkEvents
/// for what a bare `WSAResetEvent` costs when a reader and a writer share the socket.
class WindowsSocket final: public ISocket
{
  public:
    /// Wraps a connected socket, creating and associating its readiness event.
    /// @param loop The loop whose reactor drives readiness (not owned).
    /// @param socket The connected SOCKET (ownership transferred).
    /// @param peerAddress Printable peer address, or "" if unknown.
    WindowsSocket(EventLoop& loop, SOCKET socket, std::string peerAddress = {}) noexcept;
    ~WindowsSocket() override;

    WindowsSocket(WindowsSocket const&) = delete;
    WindowsSocket& operator=(WindowsSocket const&) = delete;
    WindowsSocket(WindowsSocket&&) = delete;
    WindowsSocket& operator=(WindowsSocket&&) = delete;

    [[nodiscard]] coro::Task<IoResult> read(std::span<std::byte> buffer) override;
    [[nodiscard]] coro::Task<IoResult> write(std::span<std::byte const> buffer) override;

    [[nodiscard]] std::string peerAddress() const override { return _peerAddress; }

    void close() noexcept override;

    [[nodiscard]] bool isClosed() const noexcept override { return _closed; }

  private:
    /// Closes the socket and its event, telling the loop first so a flow parked on
    /// the event is resumed rather than left waiting on a handle that can never
    /// signal again.
    /// @param policy How a parked flow observes the close. The public @c close()
    ///        passes @c Resume — this object is still alive, so the flow may safely
    ///        re-read @c _closed. The destructor passes @c Cancel, because by then
    ///        the flow would be reading `this` through a dangling pointer.
    void close(FdWakePolicy policy) noexcept;

    /// Which readiness a park waits for.
    enum class Ready
    {
        Read,
        Write
    };

    /// @return A BadHandle error described by @p op when the socket is closed,
    ///         else nullopt — the guard read() and write() both open with.
    [[nodiscard]] std::optional<NetError> closedError(char const* op) const noexcept;

    /// Read-and-clears the shared readiness event into the per-direction latches below.
    ///
    /// WSAEventSelect allows exactly ONE event object per socket, so both directions signal the
    /// same handle — and `WSAResetEvent` discards every indication standing on it, the other
    /// direction's included. That is precisely how a parked writer was stranded: the reader reset
    /// the event before each recv and wiped the FD_WRITE raised for the writer, which Winsock
    /// re-raises only after another send returns WSAEWOULDBLOCK. `WSAEnumNetworkEvents` resets the
    /// event and REPORTS what it cleared in one atomic step, so every indication is recorded
    /// against the direction that wants it instead of being thrown away.
    void latchNetworkEvents() noexcept;

    /// Parks the caller until the socket is ready for @p kind: the WSAEWOULDBLOCK retry path
    /// read() and write() share.
    ///
    /// Consumes an already-latched indication before parking, and latches only AFTER a wake — the
    /// order is load-bearing. Enumerating before a park would clear the event the other direction
    /// may be suspended on, and a suspended waiter cannot re-check its latch; the event is
    /// therefore never reset ahead of a park, which is also what makes an indication raised
    /// between the failing syscall and the park resolve the wait instead of being lost.
    [[nodiscard]] coro::Task<void> parkUntilReady(Ready kind);

    EventLoop& _loop;
    SOCKET _socket;
    WSAEVENT _event;
    std::string _peerAddress;
    bool _closed = false;
    /// Latched readiness per direction, fed by @ref latchNetworkEvents. Sticky until the
    /// direction that wants it consumes it, so an indication raised for one direction can no
    /// longer be destroyed by the other's wait.
    bool _readReady = false;
    bool _writeReady = false;
};

} // namespace net

#endif // _WIN32
