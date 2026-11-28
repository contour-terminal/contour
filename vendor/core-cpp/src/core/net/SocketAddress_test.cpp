// SPDX-License-Identifier: Apache-2.0
#include <core/net/EventLoop.hpp>
#include <core/net/IoBackend.hpp>
#include <core/net/SocketAddress.hpp>
#include <core/net/Sockets.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <set>
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
    // would be handed straight to ::connect as an address and could not resolve at all. A scoped
    // literal (`fe80::1%eth0`) is a name everywhere, so its zone reaches getaddrinfo.
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

TEST_CASE("a literal becomes the endpoint a dial reads, port included", "[net]")
{
    auto const v4 = core::net::detail::endpointOfLiteral("192.0.2.7", 8080);
    REQUIRE(v4.has_value());
    CHECK(v4->family == AF_INET);
    CHECK(v4->length == sizeof(sockaddr_in));
    CHECK(v4->protocol == IPPROTO_TCP);
    CHECK(core::net::formatPeerAddress(*v4) == "192.0.2.7");
    CHECK(core::net::detail::portOfSockaddr(v4->storage.data(), v4->length) == 8080);

    auto const v6 = core::net::detail::endpointOfLiteral("2001:db8::1", 53);
    REQUIRE(v6.has_value());
    CHECK(v6->family == AF_INET6);
    CHECK(v6->length == sizeof(sockaddr_in6));
    CHECK(core::net::formatPeerAddress(*v6) == "2001:db8::1");
    CHECK(core::net::detail::portOfSockaddr(v6->storage.data(), v6->length) == 53);

    // Exactly the texts isNumericHost refuses: one definition of "literal", the platform's.
    CHECK_FALSE(core::net::detail::endpointOfLiteral("", 1).has_value());
    CHECK_FALSE(core::net::detail::endpointOfLiteral("localhost", 1).has_value());
    CHECK_FALSE(core::net::detail::endpointOfLiteral("[::1]", 1).has_value());
    CHECK_FALSE(core::net::detail::endpointOfLiteral("1.2.3.4:80", 1).has_value());
    CHECK_FALSE(core::net::detail::endpointOfLiteral(std::string_view { "127.0.0.1\0x", 11 }, 1).has_value());

    // The shorthand getaddrinfo expands is not a literal: it is looked up, never parsed here.
    CHECK_FALSE(core::net::detail::endpointOfLiteral("127.1", 1).has_value());

    // An IPv4-mapped IPv6 address is IPv6 text, and stays an IPv6 endpoint.
    auto const mapped = core::net::detail::endpointOfLiteral("::ffff:1.2.3.4", 7);
    REQUIRE(mapped.has_value());
    CHECK(mapped->family == AF_INET6);
    CHECK(core::net::detail::portOfSockaddr(mapped->storage.data(), mapped->length) == 7);

    // A zone suffix makes the text a name on every platform, whatever the platform's inet_pton
    // would accept: a literal parse would drop the scope (macOS), and getaddrinfo keeps it.
    CHECK_FALSE(core::net::detail::endpointOfLiteral("fe80::1%lo", 80).has_value());
    CHECK_FALSE(core::net::detail::endpointOfLiteral("fe80::1%eth0", 1).has_value());
    CHECK_FALSE(core::net::detail::isNumericHost("fe80::1%lo"));
}

TEST_CASE("every resolver table row names a real failure, once", "[net][errors]")
{
    // A code with two rows answers whichever the walk meets first, and a row that says Ok reports a
    // failed lookup as a success: both are a table edit away, and nothing else would notice.
    auto const table = core::net::resolverErrorTable();
    REQUIRE_FALSE(table.empty());
    auto seen = std::set<int> {};
    for (auto const& row: table)
    {
        INFO("resolver code " << row.gaiCode);
        CHECK(seen.insert(row.gaiCode).second);
        CHECK(row.code != core::net::NetErrorCode::Ok);
        CHECK(row.code < core::net::NetErrorCode::Last);
        CHECK(core::net::detail::classifyResolverError(row.gaiCode) == row.code);
    }
}

TEST_CASE("a resolver code no row names is a permanent resolver failure", "[net][errors]")
{
    // The documented catch-all: AddressError claims nothing about whether asking again helps, which
    // is all that can be said of a code nobody classified. Never Ok, never a guess.
    constexpr auto Unlisted = 987654;
    REQUIRE(std::ranges::none_of(core::net::resolverErrorTable(),
                                 [](auto const& row) { return row.gaiCode == Unlisted; }));
    CHECK(core::net::detail::classifyResolverError(Unlisted) == core::net::NetErrorCode::AddressError);
}

TEST_CASE("the system resolver classifies a name that does not exist", "[net]")
{
    auto resolver = core::net::SystemAddressResolver {};
    auto const resolved = resolver.resolveClassified("core-cpp-p13.invalid", 80);
    if (resolved.has_value())
        SKIP("this host's resolver answers for RFC 2606 .invalid names, so a failure cannot be provoked");
    if (resolved.error().code == core::net::NetErrorCode::ResolverTemporaryFailure)
        SKIP("this host has no reachable name server, so the lookup could not be answered either way");
    CHECK(resolved.error().code == core::net::NetErrorCode::NameNotFound);
    CHECK(resolved.error().context.contains("core-cpp-p13.invalid"));
}

TEST_CASE("a listener bound to a name that does not exist says so", "[net]")
{
    // The listeners resolve their bind address themselves; their failure goes through the same table
    // as a dial's, rather than reporting a raw resolver code as if it were an OS error number.
    auto backend = core::net::makeBackend(core::net::preferredBackendKind());
    REQUIRE(backend != nullptr);
    auto loop = core::net::EventLoop { *backend };
    auto const listener =
        core::net::listen(loop, core::net::ListenOptions { .host = "core-cpp-p13.invalid" });
    if (listener.has_value())
        SKIP("this host's resolver answers for RFC 2606 .invalid names, so a failure cannot be provoked");
    if (listener.error().code == core::net::NetErrorCode::ResolverTemporaryFailure)
        SKIP("this host has no reachable name server, so the lookup could not be answered either way");
    CHECK(listener.error().code == core::net::NetErrorCode::NameNotFound);
    CHECK(listener.error().context == "getaddrinfo");
}
