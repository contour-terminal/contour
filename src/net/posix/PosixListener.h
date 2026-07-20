// SPDX-License-Identifier: Apache-2.0
#pragma once

#ifndef _WIN32

    #include <cstdint>
    #include <memory>
    #include <string_view>

    #include <coro/Task.hpp>
    #include <net/EventLoop.h>
    #include <net/IListener.h>

namespace net
{

/// A reactor-driven, non-blocking POSIX TCP listener. accept() parks on the
/// loop's waitReadable until a connection is pending, then accept4()s it as a
/// non-blocking PosixSocket.
class PosixListener final: public IListener
{
  public:
    ~PosixListener() override;

    PosixListener(PosixListener const&) = delete;
    PosixListener& operator=(PosixListener const&) = delete;
    PosixListener(PosixListener&&) = delete;
    PosixListener& operator=(PosixListener&&) = delete;

    /// Binds and listens on @p host : @p port.
    /// @param loop The loop whose reactor drives accept readiness (not owned).
    /// @param host The bind address (e.g. "127.0.0.1", "0.0.0.0", "::").
    /// @param port The bind port; 0 asks the OS for an ephemeral port.
    /// @param backlog The listen backlog.
    /// @return The bound listener, or a @c NetError on failure.
    [[nodiscard]] static std::expected<std::unique_ptr<PosixListener>, NetError> bind(EventLoop& loop,
                                                                                      std::string_view host,
                                                                                      std::uint16_t port,
                                                                                      int backlog = 128);

    [[nodiscard]] coro::Task<AcceptResult> accept() override;

    [[nodiscard]] std::uint16_t localPort() const noexcept override { return _localPort; }

    void close() noexcept override;

  private:
    PosixListener(EventLoop& loop, int fd, std::uint16_t localPort) noexcept;

    EventLoop& _loop;
    int _fd;
    std::uint16_t _localPort;
    bool _closed = false;
};

} // namespace net

#endif // !_WIN32
