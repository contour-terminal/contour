// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <core/async/Task.hpp>
#include <core/net/EventLoop.hpp>
#include <core/net/IListener.hpp>
#include <core/net/SocketBuffers.hpp>
#include <core/net/UdpSocket.hpp>

#include <cstdint>
#include <memory>
#include <string_view>

namespace core::net
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
    /// @param sharing Whether other listeners may bind the same port (`SO_REUSEPORT_LB` where it is
    ///        defined, which is FreeBSD's, and `SO_REUSEPORT` elsewhere); see
    ///        @c ListenOptions::sharing for which platforms spread the connections.
    /// @param acceptedBuffers The kernel buffer sizes, asked of the listening socket before it
    ///        listens; every socket it accepts inherits them.
    /// @return The bound listener, or a @c NetError on failure.
    [[nodiscard]] static std::expected<std::unique_ptr<PosixListener>, NetError> bind(
        EventLoop& loop,
        std::string_view host,
        std::uint16_t port,
        int backlog = 128,
        PortSharing sharing = PortSharing::Exclusive,
        SocketBufferSizes acceptedBuffers = {});

    /// Adopts an already-bound, already-listening descriptor; @see core::net::adoptListener.
    ///
    /// It makes the descriptor non-blocking and close-on-exec, because the reactor requires the
    /// first and an inherited descriptor has neither — a listener handed over by a supervisor was
    /// created for a process that blocked on `accept`.
    /// @param loop The loop whose backend drives accept readiness (not owned).
    /// @param fd The listening descriptor; ownership transfers to the returned listener.
    /// @return The adopted listener, or a @c NetError if the descriptor could not be prepared.
    [[nodiscard]] static std::expected<std::unique_ptr<PosixListener>, NetError> adopt(EventLoop& loop,
                                                                                       int fd);

    [[nodiscard]] async::Task<AcceptResult> accept() override;

    [[nodiscard]] std::uint16_t boundPort() const noexcept override { return _boundPort; }

    void close() noexcept override;

    /// @return The listening descriptor, or -1 once closed; for diagnostics and tests.
    [[nodiscard]] int native() const noexcept { return _fd; }

  private:
    PosixListener(EventLoop& loop, int fd, std::uint16_t boundPort) noexcept;

    /// Closes the listening fd, telling the loop first so a parked accept is
    /// resumed rather than left waiting on a descriptor the poller can no longer
    /// report.
    /// @param policy How a parked accept observes the close. @c close() passes
    ///        @c Resume — this listener is alive, so acceptOne may re-read the @c _fd /
    ///        @c _closed it holds pointers to, once its lifetime token says the listener
    ///        still exists (an owner may destroy it before the loop resumes the accept).
    ///        The destructor passes @c Cancel, since those pointers are about to dangle.
    void close(FdWakePolicy policy) noexcept;

    EventLoop& _loop;
    int _fd;
    std::uint16_t _boundPort;
    bool _closed = false;
    /// Expires with this listener. A parked accept is resumed by the loop a turn after `close()`,
    /// and an owner may destroy the listener in between (`listener->close(); listener.reset();`),
    /// so the accept asks this -- never the listener -- whether there is still one to read.
    std::shared_ptr<void const> _lifetime = std::make_shared<char const>('\0');
};

} // namespace core::net
