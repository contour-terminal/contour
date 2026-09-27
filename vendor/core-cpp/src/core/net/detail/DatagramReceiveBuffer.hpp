// SPDX-License-Identifier: Apache-2.0
#pragma once

/// @file
/// How long a UDP socket's receive buffer is, and the one way to open a socket with a shorter one.
///
/// Private to `core::net`. The shorter buffer exists for one reason: a receive into a buffer too
/// short for the datagram is a path no legal datagram can reach once the buffer is sized by family,
/// so without it the truncation check on either platform could only be read, never run.

#include <core/net/UdpSocket.hpp>
#include <core/net/detail/DatagramAddressing.hpp>

#include <cstddef>
#include <cstdint>
#include <expected>
#include <memory>
#include <optional>
#include <string_view>

namespace core::net::detail
{

/// @param family The address family the socket bound, `AF_INET` or `AF_INET6`.
/// @return The receive buffer that holds every legal datagram of that family whole.
[[nodiscard]] constexpr std::size_t receiveBufferFor(int family) noexcept
{
    return family == AF_INET6 ? MaxIpv6DatagramPayload : MaxIpv4DatagramPayload;
}

/// @c openUdpSocket, with the receive buffer chosen by the caller rather than by the family.
/// @param bindAddress Address to bind.
/// @param port Port to bind; 0 lets the kernel choose.
/// @param broadcast Whether this socket may send to a broadcast address.
/// @param sharing Whether other sockets may hold the same address.
/// @param receiveBuffer The receive buffer's length, or `std::nullopt` for @c receiveBufferFor the
///        family that bound. Only a test passes a length.
/// @return The socket, or why it could not be bound.
[[nodiscard]] std::expected<std::unique_ptr<IDatagramSocket>, NetError> openUdpSocketWithReceiveBuffer(
    std::string_view bindAddress,
    std::uint16_t port,
    BroadcastMode broadcast,
    PortSharing sharing,
    std::optional<std::size_t> receiveBuffer);

} // namespace core::net::detail
