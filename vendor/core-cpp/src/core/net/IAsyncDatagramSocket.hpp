// SPDX-License-Identifier: Apache-2.0
#pragma once

/// @file
/// `IAsyncDatagramSocket` — a UDP socket driven by an @c EventLoop: it binds, optionally connects,
/// and sends and receives whole datagrams as coroutines.
///
/// The blocking @c IDatagramSocket (`<core/net/IDatagramSocket.hpp>`) is for a thread of its own
/// that sends a handful of datagrams a minute; this is for a flow on a loop, which must not block
/// it. Implementations: the real socket behind @c openAsyncDatagramSocket (readiness on POSIX, the
/// completion port on Windows), and the in-process network behind
/// `testing::InMemoryAsyncDatagramNetwork` (`<core/net/testing/InMemoryAsyncDatagram.hpp>`). Both
/// run the same state machine, so the double cannot answer a state more permissively than the
/// real socket does.

#include <core/async/Task.hpp>
#include <core/net/IpSocketAddress.hpp>
#include <core/net/NetError.hpp>
#include <core/net/NetExport.hpp>
#include <core/net/SocketAddress.hpp>
#include <core/net/SocketOption.hpp>

#include <cstddef>
#include <cstdint>
#include <expected>
#include <memory>
#include <optional>
#include <vector>

namespace core::net
{

class EventLoop;

/// One received datagram, and who sent it.
struct AsyncDatagram
{
    std::vector<std::byte> payload; ///< The whole message, exactly as it arrived.
    ResolvedEndpoint from;          ///< The sender, as the kernel reports it.
};

/// A UDP socket whose sends and receives are coroutines on one @c EventLoop.
///
/// **States.** A socket opens unbound. @c bind binds it; @c connect and @c send bind it to the
/// family's wildcard address first when it is not bound yet, so a socket that only sends still has
/// a port to be answered on. @c connect with a remote fixes the default destination and filters
/// what @c receive yields to that remote; @c connect with `std::nullopt` lifts both again. @c close
/// ends every state.
///
/// **Refusals that never reach the OS** carry @c NetErrorCode::BadHandle with systemCode 0: any
/// call after @c close, @c receive or @c localEndpoint on an unbound socket, a second @c bind,
/// @c remoteEndpoint or a disconnect on an unconnected socket, a @c send with no destination on
/// an unconnected socket, and a second @c receive or a second @c send while one is in flight (a task
/// is in flight from the call until it is destroyed). An endpoint of the other family, or a @c send to an
/// address other than the connected remote, is @c NetErrorCode::AddressError with systemCode 0.
///
/// **Cancellation.** A stop of the awaiting flow throws @c async::OperationCancelled; @c close
/// resolves a pending @c receive or @c send with @c NetErrorCode::Cancelled. A datagram that had
/// already been received when the stop arrived is returned, never dropped: the data wins.
///
/// **Thread affinity.** Every call, and the destructor, on the loop's thread.
class IAsyncDatagramSocket
{
  public:
    IAsyncDatagramSocket() = default;
    virtual ~IAsyncDatagramSocket() = default;
    IAsyncDatagramSocket(IAsyncDatagramSocket const&) = delete;
    IAsyncDatagramSocket& operator=(IAsyncDatagramSocket const&) = delete;
    IAsyncDatagramSocket(IAsyncDatagramSocket&&) = delete;
    IAsyncDatagramSocket& operator=(IAsyncDatagramSocket&&) = delete;

    /// Binds the socket to @p local. Port 0 lets the kernel choose; @c localEndpoint reports it.
    /// @param local The address and port to bind, of the socket's family.
    /// @return Nothing, or why it could not bind.
    [[nodiscard]] virtual std::expected<void, NetError> bind(ResolvedEndpoint const& local) = 0;

    /// Connects the socket to @p remote, or disconnects it.
    ///
    /// A connected socket only receives from, and sends by default to, the remote. Connecting again
    /// replaces the remote. An unbound socket is bound to the family's wildcard address first.
    ///
    /// A disconnect keeps the bound address and port. Linux takes a kernel-chosen port away at a
    /// disconnect, so the socket binds it again; if another socket took it in between, the
    /// disconnect still happened and answers @c NetErrorCode::AddressInUse. macOS and the BSDs reset
    /// a named address to the wildcard at a disconnect, which nothing can bind back: such a socket is
    /// closed, never left hearing every interface, and the disconnect answers
    /// @c NetErrorCode::Unsupported.
    /// @param remote The remote, or `std::nullopt` to disconnect.
    /// @return Nothing, or why the kernel refused.
    [[nodiscard]] virtual std::expected<void, NetError> connect(std::optional<ResolvedEndpoint> remote) = 0;

    /// Waits for one datagram.
    ///
    /// **Whole or not at all.** A datagram longer than @p maxBytes is dropped and answered
    /// @c NetErrorCode::MessageTooLarge, never handed back cut short; the next receive answers the
    /// next datagram.
    /// @param maxBytes The longest payload accepted, clamped to @c MaxIpv6DatagramPayload.
    /// @return The datagram, or why none was returned.
    [[nodiscard]] virtual async::Task<std::expected<AsyncDatagram, NetError>> receive(
        std::size_t maxBytes) = 0;

    /// Sends one datagram.
    ///
    /// A successful send means the local stack accepted the whole message, not that it arrived. A
    /// send the stack places only part of is @c NetErrorCode::MessageTooLarge, never a partial
    /// write: the rest resent would be a second message. An unbound socket is bound to the family's
    /// wildcard address first, when this is called rather than when the task runs.
    /// @param payload The whole message.
    /// @param to The destination, or `std::nullopt` for the connected remote. On a connected socket
    ///        it must be the remote.
    /// @return Nothing, or why the local stack refused it.
    [[nodiscard]] virtual async::Task<std::expected<void, NetError>> send(
        std::vector<std::byte> payload, std::optional<ResolvedEndpoint> to) = 0;

    /// @return The bound address and port, as the kernel reports them.
    [[nodiscard]] virtual std::expected<ResolvedEndpoint, NetError> localEndpoint() const = 0;

    /// @return The connected remote, as the kernel reports it.
    [[nodiscard]] virtual std::expected<ResolvedEndpoint, NetError> remoteEndpoint() const = 0;

    /// Reads one option. A datagram socket has the buffer sizes and the unicast hop limit; the
    /// keep-alive options are @c NetErrorCode::Unsupported.
    /// @param option The option.
    /// @return Its value, in the caller's units (see @c SocketOptionRow).
    [[nodiscard]] virtual std::expected<std::uint64_t, NetError> option(SocketOption option) const = 0;

    /// Sets one option, clamped to the row's maximum as @c ITcpSocket::setOption does.
    /// @param option The option.
    /// @param value Its new value, in the caller's units.
    /// @return Nothing, or why it was refused.
    [[nodiscard]] virtual std::expected<void, NetError> setOption(SocketOption option,
                                                                  std::uint64_t value) = 0;

    /// Closes the socket. A pending @c receive or @c send resolves with @c NetErrorCode::Cancelled.
    /// Idempotent; the destructor calls it.
    virtual void close() noexcept = 0;
};

/// Opens an unbound UDP socket of @p family on @p loop.
///
/// An IPv6 socket is IPv6-only on every platform, as an @c ITcpSocket is. On Windows the socket
/// ignores the ICMP port-unreachable a dead peer answers with (`SIO_UDP_CONNRESET` off), which would
/// otherwise fail the next receive of an unconnected socket.
/// @param loop The loop every send and receive runs on; must outlive the socket.
/// @param family The address family.
/// @return The socket, or why it could not be opened.
[[nodiscard]] CORE_NET_API std::expected<std::unique_ptr<IAsyncDatagramSocket>, NetError>
openAsyncDatagramSocket(EventLoop& loop, AddressFamily family);

} // namespace core::net
