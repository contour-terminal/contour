// SPDX-License-Identifier: Apache-2.0
#include <core/net/SocketAddress.hpp>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

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

using core::net::ResolvedEndpoint;

namespace
{

/// Builds an IPv4 sockaddr for a literal and a port, as accept/getpeername would hand one over.
[[nodiscard]] sockaddr_in makeV4(std::string_view literal, std::uint16_t port)
{
    auto address = sockaddr_in {};
    address.sin_family = AF_INET;
    address.sin_port = htons(port);
    auto const text = std::string { literal };
    REQUIRE(::inet_pton(AF_INET, text.c_str(), &address.sin_addr) == 1);
    return address;
}

} // namespace

TEST_CASE("a literal is told from a name by the platform's own parser", "[net]")
{
    // The half that is pinned STRICTLY is "a name is never reported as a literal": such a host
    // would be handed straight to ::connect as an address and could not resolve at all. The
    // other direction is deliberately loose — a scoped literal (`fe80::1%eth0`) is a literal on
    // macOS and a name on glibc, and both answers produce a working dial.
    CHECK(core::net::detail::isNumericHost("127.0.0.1"));
    CHECK(core::net::detail::isNumericHost("0.0.0.0"));
    CHECK(core::net::detail::isNumericHost("::1"));
    CHECK(core::net::detail::isNumericHost("::"));

    CHECK_FALSE(core::net::detail::isNumericHost(""));
    CHECK_FALSE(core::net::detail::isNumericHost("localhost"));
    CHECK_FALSE(core::net::detail::isNumericHost("example.invalid"));
    CHECK_FALSE(core::net::detail::isNumericHost("127.0.0.1:80")); // a port is not part of a host
    CHECK_FALSE(core::net::detail::isNumericHost("[::1]"));        // brackets are the URL's, not ours
}

TEST_CASE("a raw sockaddr round-trips through an endpoint", "[net]")
{
    auto const address = makeV4("203.0.113.7", 4242);
    auto const endpoint =
        core::net::detail::endpointFromSockaddr(&address, static_cast<std::uint32_t>(sizeof(address)));

    CHECK(endpoint.family == AF_INET);
    CHECK(endpoint.length == sizeof(address));
    CHECK(core::net::formatPeerAddress(endpoint) == "203.0.113.7");
    CHECK(core::net::detail::portOfSockaddr(endpoint.storage.data(), endpoint.length) == 4242);
}

TEST_CASE("an endpoint that carries nothing formats as nothing", "[net]")
{
    // The fail-safe direction: a peer address that was never captured must print as "" rather
    // than as whatever the zeroed storage happens to decode to.
    CHECK(core::net::formatPeerAddress(ResolvedEndpoint {}).empty());
    CHECK(core::net::detail::portOfSockaddr(nullptr, 0) == 0);

    // A length larger than the storage is refused rather than memcpy'd.
    auto oversized = std::array<std::byte, ResolvedEndpoint::StorageSize * 2> {};
    auto const endpoint = core::net::detail::endpointFromSockaddr(
        oversized.data(), static_cast<std::uint32_t>(oversized.size()));
    CHECK(endpoint.length == 0);
}

TEST_CASE("the system resolver turns a literal into a dial-able endpoint", "[net]")
{
    // A LITERAL, deliberately: this case must not depend on a working DNS server. The port is
    // carried through, which is what a dial reads back out of the storage.
    auto resolver = core::net::SystemAddressResolver {};
    auto const resolved = resolver.resolve("127.0.0.1", 9);
    REQUIRE(resolved.has_value());
    REQUIRE_FALSE(resolved->empty());

    auto const& first = resolved->front();
    CHECK(first.family == AF_INET);
    CHECK(first.length >= sizeof(sockaddr_in));
    CHECK(core::net::formatPeerAddress(first) == "127.0.0.1");
    CHECK(core::net::detail::portOfSockaddr(first.storage.data(), first.length) == 9);
}

TEST_CASE("the system resolver reports why a host produced nothing", "[net]")
{
    // `.invalid` is reserved by RFC 2606 precisely so it cannot resolve, and the message must
    // name the host: "resolution failed" without it is the one thing the reader already knew.
    auto resolver = core::net::SystemAddressResolver {};
    auto const resolved = resolver.resolve("core-cpp-b8.invalid", 80);
    if (resolved.has_value())
    {
        // A resolver behind a wildcard-answering DNS provider hands back an address for
        // everything. That is the network lying, not this code failing, so the case abstains
        // rather than failing on somebody's captive portal.
        SKIP("this host's resolver answers for RFC 2606 .invalid names, so a failure cannot be provoked");
    }
    CHECK(resolved.error().contains("core-cpp-b8.invalid"));
}
