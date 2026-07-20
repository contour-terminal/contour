// SPDX-License-Identifier: Apache-2.0
#pragma once

#ifdef _WIN32

    #include <net/ISocket.h>

// clang-format off
    #include <winsock2.h>
    #include <windows.h>
// clang-format on

    #include <cstddef>
    #include <span>
    #include <string>

    #include <coro/Task.hpp>
    #include <net/EventLoop.h>

namespace net
{

/// A reactor-driven, non-blocking Windows stream socket. Because a SOCKET is not
/// directly waitable by WaitForMultipleObjects, readiness is observed through a
/// WSAEVENT associated with the socket via WSAEventSelect (the same technique as
/// platform::SystemPipe): the loop waits on the event, while recv/send operate
/// on the socket. read/write try the syscall and, on WSAEWOULDBLOCK, park on the
/// event until ready, then retry.
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
    EventLoop& _loop;
    SOCKET _socket;
    WSAEVENT _event;
    std::string _peerAddress;
    bool _closed = false;
};

} // namespace net

#endif // _WIN32
