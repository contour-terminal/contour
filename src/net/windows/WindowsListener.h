// SPDX-License-Identifier: Apache-2.0
#pragma once

#ifdef _WIN32

    #include <net/IListener.h>

// clang-format off
    #include <winsock2.h>
    #include <windows.h>
// clang-format on

    #include <cstdint>
    #include <memory>
    #include <string_view>

    #include <coro/Task.hpp>
    #include <net/EventLoop.h>

namespace net
{

/// A reactor-driven Windows TCP listener. Accept readiness is observed through a
/// WSAEVENT associated with the listening socket via WSAEventSelect(FD_ACCEPT);
/// accept() parks on the event, then accepts the pending connection as a
/// WindowsSocket.
class WindowsListener final: public IListener
{
  public:
    ~WindowsListener() override;

    WindowsListener(WindowsListener const&) = delete;
    WindowsListener& operator=(WindowsListener const&) = delete;
    WindowsListener(WindowsListener&&) = delete;
    WindowsListener& operator=(WindowsListener&&) = delete;

    /// Binds and listens on @p host : @p port.
    /// @param loop The loop whose reactor drives accept readiness (not owned).
    /// @param host The bind address.
    /// @param port The bind port; 0 requests an ephemeral port.
    /// @param backlog The listen backlog.
    /// @return The bound listener, or a @c NetError on failure.
    [[nodiscard]] static std::expected<std::unique_ptr<WindowsListener>, NetError> bind(EventLoop& loop,
                                                                                        std::string_view host,
                                                                                        std::uint16_t port,
                                                                                        int backlog = 128);

    /// Binds and listens on the AF_UNIX socket file @p path (Windows 10
    /// 1803+, afunix.h). A stale socket file is removed first. POSIX-style
    /// permission hardening does not apply: NTFS ACLs govern access.
    /// @param loop The loop whose reactor drives accept readiness (not owned).
    /// @param path The socket file path.
    /// @param backlog The listen backlog.
    /// @return The bound listener, or a @c NetError on failure.
    [[nodiscard]] static std::expected<std::unique_ptr<WindowsListener>, NetError> bindUnix(
        EventLoop& loop, std::string_view path, int backlog = 128);

    [[nodiscard]] coro::Task<AcceptResult> accept() override;

    [[nodiscard]] std::uint16_t localPort() const noexcept override { return _localPort; }

    void close() noexcept override;

  private:
    WindowsListener(EventLoop& loop, SOCKET socket, WSAEVENT event, std::uint16_t localPort) noexcept;

    EventLoop& _loop;
    SOCKET _socket;
    WSAEVENT _event;
    std::uint16_t _localPort;
    bool _closed = false;
};

} // namespace net

#endif // _WIN32
