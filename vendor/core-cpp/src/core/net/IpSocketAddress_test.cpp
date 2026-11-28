// SPDX-License-Identifier: Apache-2.0
//
// `IpSocketAddress` against the sockaddr bytes it stands for.
//
// A round trip alone agrees with itself: a conversion that stored the port or the flow label in the
// wrong byte order in BOTH directions would pass one. So the cases below also build the sockaddr by
// hand, the way `getsockname` hands one over, and read what `toEndpoint` wrote back through the
// platform's own structure.
#include <core/net/IpSocketAddress.hpp>
#include <core/net/NetError.hpp>
#include <core/net/SocketAddress.hpp>

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include <array>
#include <cstdint>
#include <cstring>
#include <string>
#include <variant>

#ifdef _WIN32
// clang-format off
    #include <winsock2.h>
    #include <ws2tcpip.h>
// clang-format on
#else
    #include <sys/socket.h>

    #include <arpa/inet.h>
    #include <netinet/in.h>
#endif

using core::net::AddressFamily;
using core::net::IpSocketAddress;
using core::net::Ipv4Address;
using core::net::Ipv6Address;
using core::net::NetErrorCode;
using core::net::ResolvedEndpoint;

namespace
{

/// One row of the round-trip table.
struct RoundTripRow
{
    char const* name = nullptr; ///< What the row is, for the failure message.
    IpSocketAddress address;    ///< The address that must come back unchanged.
};

/// IPv4, IPv6, an IPv4-mapped IPv6 address (which stays IPv6: nothing unmaps it), and a scoped
/// link-local address with a flow label -- the two fields only `sockaddr_in6` carries.
[[nodiscard]] std::array<RoundTripRow, 5> const& roundTripRows()
{
    static auto const rows = std::array<RoundTripRow, 5> {
        RoundTripRow { .name = "IPv4 loopback",
                       .address =
                           IpSocketAddress { .address = Ipv4Address { { 127, 0, 0, 1 } }, .port = 8080 } },
        RoundTripRow { .name = "IPv4 with every octet distinct",
                       .address =
                           IpSocketAddress { .address = Ipv4Address { { 203, 0, 113, 7 } }, .port = 65535 } },
        RoundTripRow {
            .name = "IPv6 with every segment distinct",
            .address =
                IpSocketAddress {
                    .address =
                        Ipv6Address { { 0x2001, 0x0db8, 0x0102, 0x0304, 0x0506, 0x0708, 0x090a, 0x0b0c } },
                    .port = 443 } },
        RoundTripRow {
            .name = "IPv4-mapped IPv6",
            .address = IpSocketAddress { .address = Ipv6Address { { 0, 0, 0, 0, 0, 0xffff, 0xc000, 0x0201 } },
                                         .port = 1 } },
        RoundTripRow { .name = "fe80::1%scope with a flow label",
                       .address =
                           IpSocketAddress { .address = Ipv6Address { { 0xfe80, 0, 0, 0, 0, 0, 0, 1 } },
                                             .port = 5353,
                                             .flowInfo = 0x000a'bcde,
                                             .scopeId = 7 } },
    };
    return rows;
}

/// @return @p endpoint's storage read back as a @c sockaddr_in6.
[[nodiscard]] sockaddr_in6 asV6(ResolvedEndpoint const& endpoint)
{
    auto v6 = sockaddr_in6 {};
    std::memcpy(&v6, endpoint.storage.data(), sizeof(v6));
    return v6;
}

} // namespace

TEST_CASE("An IP socket address survives a round trip through ResolvedEndpoint", "[net][ip-socket-address]")
{
    for (auto const& row: roundTripRows())
    {
        DYNAMIC_SECTION(row.name)
        {
            auto const endpoint = core::net::toEndpoint(row.address);
            auto const back = core::net::toIpSocketAddress(endpoint);
            REQUIRE(back.has_value());
            CHECK(*back == row.address);
            CHECK(core::net::detail::portOfSockaddr(endpoint.storage.data(), endpoint.length)
                  == row.address.port);
        }
    }
}

TEST_CASE("toEndpoint writes the sockaddr the platform reads", "[net][ip-socket-address]")
{
    SECTION("IPv4: family, network-order port and address")
    {
        auto const endpoint = core::net::toEndpoint(
            IpSocketAddress { .address = Ipv4Address { { 192, 0, 2, 33 } }, .port = 0x1234 });
        CHECK(endpoint.family == AF_INET);
        CHECK(endpoint.length == sizeof(sockaddr_in));
        CHECK(endpoint.protocol == 0);
        CHECK(core::net::formatPeerAddress(endpoint) == "192.0.2.33");

        auto v4 = sockaddr_in {};
        std::memcpy(&v4, endpoint.storage.data(), sizeof(v4));
        CHECK(ntohs(v4.sin_port) == 0x1234);
    }

    SECTION("IPv6: segments in network order, the flow label in network order, the scope as is")
    {
        auto const endpoint = core::net::toEndpoint(
            IpSocketAddress { .address = Ipv6Address { { 0xfe80, 0, 0, 0, 0, 0, 0, 0x0102 } },
                              .port = 0x1234,
                              .flowInfo = 0x0001'2345,
                              .scopeId = 0x0102'0304 });
        CHECK(endpoint.family == AF_INET6);
        CHECK(endpoint.length == sizeof(sockaddr_in6));
        CHECK(core::net::formatPeerAddress(endpoint) == "fe80::102");

        auto const v6 = asV6(endpoint);
        CHECK(ntohs(v6.sin6_port) == 0x1234);
        CHECK(ntohl(v6.sin6_flowinfo) == 0x0001'2345U);
        CHECK(v6.sin6_scope_id == 0x0102'0304U);
    }
}

TEST_CASE("toIpSocketAddress reads a sockaddr built the way the kernel builds one",
          "[net][ip-socket-address]")
{
    SECTION("IPv4")
    {
        auto v4 = sockaddr_in {};
        v4.sin_family = AF_INET;
        v4.sin_port = htons(0xbeef);
        REQUIRE(::inet_pton(AF_INET, "10.20.30.40", &v4.sin_addr) == 1);
        auto const endpoint = core::net::detail::endpointFromSockaddr(&v4, sizeof(v4));

        auto const address = core::net::toIpSocketAddress(endpoint);
        REQUIRE(address.has_value());
        CHECK(address->family() == AddressFamily::Ipv4);
        CHECK(address->address
              == std::variant<Ipv4Address, Ipv6Address> { Ipv4Address { { 10, 20, 30, 40 } } });
        CHECK(address->port == 0xbeef);
        CHECK(address->flowInfo == 0);
        CHECK(address->scopeId == 0);
    }

    SECTION("IPv6, with a flow label and a scope")
    {
        auto v6 = sockaddr_in6 {};
        v6.sin6_family = AF_INET6;
        v6.sin6_port = htons(0xbeef);
        v6.sin6_flowinfo = htonl(0x000f'0001);
        v6.sin6_scope_id = 3;
        REQUIRE(::inet_pton(AF_INET6, "2001:db8::a:b", &v6.sin6_addr) == 1);
        auto const endpoint = core::net::detail::endpointFromSockaddr(&v6, sizeof(v6));

        auto const address = core::net::toIpSocketAddress(endpoint);
        REQUIRE(address.has_value());
        CHECK(address->family() == AddressFamily::Ipv6);
        CHECK(address->address
              == std::variant<Ipv4Address, Ipv6Address> {
                  Ipv6Address { { 0x2001, 0x0db8, 0, 0, 0, 0, 0xa, 0xb } } });
        CHECK(address->port == 0xbeef);
        CHECK(address->flowInfo == 0x000f'0001U);
        CHECK(address->scopeId == 3U);
    }

    SECTION("A literal the resolver path produced")
    {
        auto const endpoint = core::net::detail::endpointOfLiteral("::ffff:192.0.2.1", 99);
        REQUIRE(endpoint.has_value());
        auto const address = core::net::toIpSocketAddress(*endpoint);
        REQUIRE(address.has_value());
        CHECK(address->address
              == std::variant<Ipv4Address, Ipv6Address> {
                  Ipv6Address { { 0, 0, 0, 0, 0, 0xffff, 0xc000, 0x0201 } } });
        CHECK(address->port == 99);
    }
}

TEST_CASE("toIpSocketAddress refuses what is not an IP endpoint", "[net][ip-socket-address]")
{
    SECTION("An empty endpoint is Unsupported")
    {
        auto const address = core::net::toIpSocketAddress(ResolvedEndpoint {});
        REQUIRE_FALSE(address.has_value());
        CHECK(address.error().code == NetErrorCode::Unsupported);
    }

    SECTION("A non-IP family is Unsupported")
    {
        auto other = sockaddr_in {};
        other.sin_family = AF_UNSPEC;
        auto const endpoint = core::net::detail::endpointFromSockaddr(&other, sizeof(other));
        auto const address = core::net::toIpSocketAddress(endpoint);
        REQUIRE_FALSE(address.has_value());
        CHECK(address.error().code == NetErrorCode::Unsupported);
    }

    SECTION("An IP family shorter than its sockaddr is an AddressError")
    {
        auto const family = GENERATE(as<int> {}, AF_INET, AF_INET6);
        auto v6 = sockaddr_in6 {};
        v6.sin6_family = static_cast<decltype(v6.sin6_family)>(family);
        // One byte short of the family's own structure: a kernel never hands such a thing over, and
        // reading the missing bytes would invent an address.
        auto const size = family == AF_INET ? sizeof(sockaddr_in) - 1 : sizeof(sockaddr_in6) - 1;
        auto const endpoint = core::net::detail::endpointFromSockaddr(&v6, static_cast<std::uint32_t>(size));
        auto const address = core::net::toIpSocketAddress(endpoint);
        REQUIRE_FALSE(address.has_value());
        CHECK(address.error().code == NetErrorCode::AddressError);
    }
}
