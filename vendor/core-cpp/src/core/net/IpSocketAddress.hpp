// SPDX-License-Identifier: Apache-2.0
#pragma once

/// @file
/// `IpSocketAddress` -- an IPv4 or IPv6 socket address as typed values, and its conversions to and
/// from the raw @c ResolvedEndpoint every socket call takes.
///
/// **A value, not a sockaddr.** @c ResolvedEndpoint carries the platform's own bytes, which is what
/// `connect`, `bind` and `getsockname` speak, but nothing a caller can read: the port sits at a
/// family-dependent offset in network byte order, and an IPv6 flow label likewise. This is the form
/// a caller inspects and builds, with every field in host order, and the two conversions below are
/// the only code that knows the layouts (`IpSocketAddress.cpp`, pure over `sockaddr_in` and
/// `sockaddr_in6`). Like @c ResolvedEndpoint, this header names no platform type.

#include <core/net/NetError.hpp>
#include <core/net/NetExport.hpp>
#include <core/net/SocketAddress.hpp>

#include <array>
#include <compare>
#include <cstdint>
#include <expected>
#include <variant>

namespace core::net
{

/// An IPv4 address, most significant octet first: 127.0.0.1 is `{ 127, 0, 0, 1 }`.
struct Ipv4Address
{
    std::array<std::uint8_t, 4> octets {}; ///< The four octets, in the order they are written.

    auto operator<=>(Ipv4Address const&) const = default;
};

/// An IPv6 address as eight 16-bit segments in host byte order, most significant first:
/// `fe80::1` is `{ 0xfe80, 0, 0, 0, 0, 0, 0, 1 }`. An IPv4-mapped address (`::ffff:192.0.2.1`)
/// stays an IPv6 one; nothing here unmaps it.
struct Ipv6Address
{
    std::array<std::uint16_t, 8> segments {}; ///< The eight segments, in the order they are written.

    auto operator<=>(Ipv6Address const&) const = default;
};

/// Which IP version an address or a socket speaks.
enum class AddressFamily : std::uint8_t
{
    Ipv4, ///< IPv4: `AF_INET`.
    Ipv6, ///< IPv6: `AF_INET6`.
};

/// An IP address with the port and, for IPv6, the flow label and scope that a socket address carries.
struct IpSocketAddress
{
    std::variant<Ipv4Address, Ipv6Address> address; ///< The host; its alternative is the family.
    std::uint16_t port = 0;                         ///< The port, in host byte order.
    std::uint32_t flowInfo = 0; ///< IPv6 only: `sin6_flowinfo`, in host byte order. Zero for IPv4.
    std::uint32_t scopeId = 0;  ///< IPv6 only: `sin6_scope_id`, the zone's interface index. Zero for IPv4.

    /// @return The family @c address holds.
    [[nodiscard]] AddressFamily family() const noexcept
    {
        return std::holds_alternative<Ipv6Address>(address) ? AddressFamily::Ipv6 : AddressFamily::Ipv4;
    }

    bool operator==(IpSocketAddress const&) const = default;
};

/// Converts @p address into the `sockaddr_in` or `sockaddr_in6` bytes a socket call takes.
///
/// For an IPv4 address @c IpSocketAddress::flowInfo and @c IpSocketAddress::scopeId have nowhere to
/// go and are not written. The endpoint's protocol is 0, which `socket()` reads as the family's
/// default for the socket type -- TCP for a stream, UDP for a datagram -- so the one endpoint serves
/// both.
/// @param address The address to convert.
/// @return The endpoint; its family is the platform's `AF_INET` or `AF_INET6`.
[[nodiscard]] CORE_NET_API ResolvedEndpoint toEndpoint(IpSocketAddress const& address) noexcept;

/// Reads an IPv4 or IPv6 endpoint -- what a resolver, `getsockname` or `getpeername` produced -- as
/// typed values.
/// @param endpoint The endpoint to read.
/// @return The address; @c NetErrorCode::Unsupported for an endpoint of any other family (an
///         AF_UNIX one, an empty one), and @c NetErrorCode::AddressError for one shorter than its
///         family's sockaddr, whose missing bytes would otherwise be read as an address.
[[nodiscard]] CORE_NET_API std::expected<IpSocketAddress, NetError> toIpSocketAddress(
    ResolvedEndpoint const& endpoint);

} // namespace core::net
