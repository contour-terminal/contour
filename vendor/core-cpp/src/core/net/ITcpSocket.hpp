// SPDX-License-Identifier: Apache-2.0
#pragma once

/// @file
/// `ITcpSocket` -- a TCP socket before it is connected or listening -- and `openTcpSocket`.
///
/// The lifecycle the other transports skip: a dial (`connect`, @c IConnector) and a listener
/// (`listen`) create, bind and connect or listen in one call, so nothing can be asked of the socket
/// in between. Here each step is its own call, and options can be set before the connection exists:
/// a socket binds to a chosen local address and port, then either connects, becoming an @c ISocket,
/// or listens, becoming an @c IListener. Either consumes it.

#include <core/async/Task.hpp>
#include <core/net/IListener.hpp>
#include <core/net/ISocket.hpp>
#include <core/net/IpSocketAddress.hpp>
#include <core/net/NetError.hpp>
#include <core/net/NetExport.hpp>
#include <core/net/SocketAddress.hpp>
#include <core/net/SocketOption.hpp>

#include <cstdint>
#include <expected>
#include <memory>

namespace core::net
{

class EventLoop;

/// An unconnected TCP socket: open, possibly bound, neither connected nor listening.
///
/// **`connect` and `listen` consume it.** The socket's handle moves into the connection or the
/// listener, and every call afterwards answers @c NetErrorCode::BadHandle with no system code --
/// a second `connect` included, whatever the first one's outcome. A `listen` that fails does not
/// consume it; a `connect` does, at the call rather than when its task first runs, so a task that
/// is dropped unstarted closes the socket it took.
///
/// Destroying it closes the socket. Call every member on the thread of the loop it was opened on.
class ITcpSocket
{
  public:
    ITcpSocket() = default;
    virtual ~ITcpSocket() = default;

    ITcpSocket(ITcpSocket const&) = delete;
    ITcpSocket& operator=(ITcpSocket const&) = delete;
    ITcpSocket(ITcpSocket&&) = delete;
    ITcpSocket& operator=(ITcpSocket&&) = delete;

    /// Binds the socket to @p local, which may name port 0 for an ephemeral one.
    ///
    /// **The platform's reuse-address options are set first**, so a port whose last connection is
    /// still in TIME_WAIT can be bound again: `SO_REUSEADDR` on POSIX, and nothing on Windows, whose
    /// default already allows that, and whose `SO_REUSEADDR` would let this socket take over a port
    /// another one is listening on. A port a live listener holds stays @c NetErrorCode::AddressInUse
    /// everywhere.
    /// @param local The address and port to bind; of the family the socket was opened with.
    /// @return Nothing; @c NetErrorCode::AddressInUse for a port in use; @c NetErrorCode::BadHandle,
    ///         with no system code, once consumed or already bound; otherwise the classified OS
    ///         error (an address of the other family).
    [[nodiscard]] virtual std::expected<void, NetError> bind(ResolvedEndpoint const& local) = 0;

    /// Connects to @p remote, consuming the socket.
    ///
    /// An unbound socket is bound to its family's wildcard address and an ephemeral port first, by
    /// the kernel where its connect does so (POSIX) and explicitly where it does not (Windows'
    /// `ConnectEx`). The options set on the socket carry over to the connection, and
    /// `TCP_NODELAY` is set as on every connection this library makes.
    /// @param remote The address to connect to. By value: the task is lazy.
    /// @return A task resolving to the connected socket, the classified failure
    ///         (@c NetErrorCode::ConnRefused, ...), or @c NetErrorCode::BadHandle once consumed.
    /// @throws async::OperationCancelled when the awaiting flow is stopped while the connect is
    ///         outstanding; the socket is closed.
    [[nodiscard]] virtual async::Task<SocketResult> connect(ResolvedEndpoint remote) = 0;

    /// Listens for connections, consuming the socket on success.
    ///
    /// An unbound socket is bound to its family's wildcard address and an ephemeral port first, on
    /// every platform. The options set on the socket carry over to the listener, and the
    /// connections it accepts inherit the kernel's share of them (buffer sizes, keepalive).
    /// @param backlog The listen backlog; the kernel caps it at its own maximum.
    /// @return The listener; @c NetErrorCode::BadHandle once consumed; otherwise the classified OS
    ///         error. A failed bind or `listen` leaves the socket as it was; a failure after the
    ///         socket is listening (adopting it on the loop) consumes it, because it can no longer
    ///         connect.
    [[nodiscard]] virtual std::expected<std::unique_ptr<IListener>, NetError> listen(
        std::uint32_t backlog) = 0;

    /// @return The address and port the socket is bound to -- `getsockname` -- as a raw endpoint;
    ///         @c NetErrorCode::BadHandle, with no system code, once consumed or while unbound: the
    ///         same answer on every platform, where the OS itself says the wildcard at port 0
    ///         (POSIX) or refuses (Windows).
    [[nodiscard]] virtual std::expected<ResolvedEndpoint, NetError> localEndpoint() const = 0;

    /// @param option The option to read.
    /// @return Its value in the unit its row states (@c socketOptionTable);
    ///         @c NetErrorCode::BadHandle once consumed; otherwise the classified OS error.
    [[nodiscard]] virtual std::expected<std::uint64_t, NetError> option(SocketOption option) const = 0;

    /// Sets @p option to @p value, in the unit its row states.
    ///
    /// A value above the row's maximum (@c SocketOptionRow::maximum) is clamped to it, so a value
    /// is never refused for being too large; the kernel may cap it further (Linux caps a buffer at
    /// `rmem_max`), and it then reads back capped. A value the kernel refuses for another reason is
    /// its classified error.
    /// @param option The option to set.
    /// @param value The value.
    /// @return Nothing; @c NetErrorCode::BadHandle once consumed; otherwise the classified OS error.
    [[nodiscard]] virtual std::expected<void, NetError> setOption(SocketOption option,
                                                                  std::uint64_t value) = 0;
};

/// Opens an unconnected, unbound TCP socket of @p family on @p loop.
///
/// Non-blocking and not inherited by child processes, as every socket here is. **An IPv6 socket is
/// IPv6-only** (`IPV6_V6ONLY`) on every platform, whatever the system default: an IPv6 wildcard
/// bind then takes no IPv4 connections, and the two families can bind the same port side by side.
/// @param loop The loop the socket, and the connection or listener it becomes, is pinned to.
/// @param family The address family.
/// @return The socket; @c NetErrorCode::Unsupported where the loop cannot serve one (on Windows, a
///         loop that lends no completion port) or the host lacks the family; otherwise the
///         classified OS error.
[[nodiscard]] CORE_NET_API std::expected<std::unique_ptr<ITcpSocket>, NetError> openTcpSocket(
    EventLoop& loop, AddressFamily family);

} // namespace core::net
