// SPDX-License-Identifier: Apache-2.0
#pragma once

/// @file
/// `openUdpSocket` — a real UDP socket over the platform's stack, and the two options that decide
/// what it may do with an address.
///
/// The implementation is per platform (`posix/UdpSocket.cpp`, `windows/UdpSocket.cpp`), because the
/// option each one spells differs: `SO_REUSEPORT` exists on one side and `SO_EXCLUSIVEADDRUSE` on
/// the other, and the receive deadline is a `timeval` against a `DWORD`. This header is portable
/// and says nothing about either.
///
/// Origin: fastcached `src/FastCache/Net/UdpSocket.hpp`
/// (`0708dd54dc7ee72622c8c0783c2bd4a06f0e9b21`).

#include <core/net/IDatagramSocket.hpp>

#include <cstdint>
#include <expected>
#include <memory>
#include <string_view>

namespace core::net
{

/// Whether a socket may address the subnet broadcast.
///
/// An `enum class` rather than a `bool` because the call site is stating a capability the kernel
/// refuses by default, and `true` says nothing about which.
enum class BroadcastMode : std::uint8_t
{
    Off, ///< Unicast only.
    On,  ///< `SO_BROADCAST`, so a beacon may reach the segment.
};

/// Whether other sockets may hold this address at the same time.
///
/// An `enum class` for the reason @c BroadcastMode is one: the call site is stating who owns a
/// port, and `true` says nothing about which way round.
///
/// **The distinction is load-bearing rather than a tidying.** A shared port is what lets every node
/// on a segment hear the same broadcast; it is also what makes a unicast to that port arrive at
/// only one of them, so a socket that must be *reachable* — one whose whole job is that an answer
/// addressed to it is handed to it — has to be exclusive. A node needs one of each; see
/// `<core/net/SharedPortDatagram.hpp>`.
enum class PortSharing : std::uint8_t
{
    /// This socket alone holds the address it bound.
    Exclusive,

    /// Several sockets may hold it. For a datagram socket, each hears what is broadcast to it; for
    /// a TCP listener (@c ListenOptions::sharing), each may accept, and which one a connection
    /// reaches is the platform's choice -- @c ListenOptions::sharing says which platforms spread
    /// the connections and which give them all to one listener.
    Shared,
};

/// The largest payload a UDP datagram can carry over IPv4: 65535 minus the 8-byte UDP header and
/// the 20-byte IP header, which the IPv4 length field counts.
///
/// An IPv4 socket's receive buffer is this long, so a legal datagram is never cut short. That is the
/// reason for the number rather than a smaller one chosen for memory: a receive into a short buffer
/// truncates, and upstream's bound of 8192 delivered the first 8192 bytes of a longer datagram as
/// though they were the message. A datagram longer than the buffer is dropped and reported as
/// @c DatagramWait::MessageTooLarge, never delivered cut short.
///
/// It costs one buffer per socket, allocated once and reused, not one per receive.
inline constexpr std::size_t MaxIpv4DatagramPayload = 65507;

/// The largest payload a UDP datagram can carry over IPv6: 65535 minus the 8-byte UDP header only,
/// because the IPv6 payload length field does not count the IPv6 header. Twenty bytes more than
/// @c MaxIpv4DatagramPayload, so an IPv6 socket's receive buffer is this long; sized for IPv4, a
/// legal IPv6 datagram of 65508 to 65527 bytes would not fit.
inline constexpr std::size_t MaxIpv6DatagramPayload = 65527;

/// Opens a UDP socket bound to @p bindAddress : @p port.
///
/// **Returns why it could not bind**, where upstream returned a null pointer: a caller that has to
/// report a startup failure — the port is taken, the address is not local, the port is privileged —
/// cannot say which from a null, and all three are ordinary deployment mistakes.
///
/// @param bindAddress Address to bind, e.g. `0.0.0.0` or `127.0.0.1`.
/// @param port Port to bind; 0 lets the kernel choose, which is what the client side of a handshake
///        wants. @c IDatagramSocket::boundAddress reports what was actually taken.
/// @param broadcast Whether this socket may send to a broadcast address.
/// @param sharing Whether other sockets may hold the same address. Exclusive by default, because
///        that is what a socket expecting to be answered needs and sharing is the exception a
///        caller states on purpose.
/// @return The socket, or why it could not be bound.
[[nodiscard]] std::expected<std::unique_ptr<IDatagramSocket>, NetError> openUdpSocket(
    std::string_view bindAddress,
    std::uint16_t port,
    BroadcastMode broadcast,
    PortSharing sharing = PortSharing::Exclusive);

} // namespace core::net
