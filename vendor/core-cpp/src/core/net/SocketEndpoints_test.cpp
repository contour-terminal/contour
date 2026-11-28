// SPDX-License-Identifier: Apache-2.0
//
// `ISocket::localEndpoint`/`remoteEndpoint` and `IListener::localEndpoint`: the ports a connection
// actually has, on every transport this library hands out.
//
// **The ports are what distinguish.** Every address here is a loopback, so an implementation that
// answered the listener's address for both ends, or swapped `getsockname` for `getpeername`, would
// still report the right host. The ephemeral port the kernel gave the dialling end is the one value
// only the right call on the right socket can produce, so each assertion is about a port seen from
// two sides.
#include <core/async/Task.hpp>
#include <core/net/EventLoop.hpp>
#include <core/net/IListener.hpp>
#include <core/net/ISocket.hpp>
#include <core/net/ITlsSession.hpp>
#include <core/net/IoBackend.hpp>
#include <core/net/IpSocketAddress.hpp>
#include <core/net/NetError.hpp>
#include <core/net/SocketAddress.hpp>
#include <core/net/Sockets.hpp>
#include <core/net/SplitSocket.hpp>
#include <core/net/testing/InMemorySocket.hpp>
#include <core/net/testing/InMemoryTransport.hpp>
#include <core/net/testing/SocketDecorator.hpp>

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <memory>
#include <optional>
#include <string>
#include <utility>

using core::async::Task;
using core::net::AddressFamily;
using core::net::EventLoop;
using core::net::IListener;
using core::net::IpSocketAddress;
using core::net::Ipv4Address;
using core::net::ISocket;
using core::net::NetError;
using core::net::NetErrorCode;
using core::net::ResolvedEndpoint;
using core::net::testing::InMemoryEndpoints;
using core::net::testing::InMemoryPairEndpoints;
using core::net::testing::InMemoryPipe;
using core::net::testing::InMemorySocket;
using core::net::testing::InMemorySocketPair;

namespace
{

/// The two ends of one loopback connection, or nothing where a step failed.
struct Connection
{
    std::unique_ptr<ISocket> client;   ///< The dialling end.
    std::unique_ptr<ISocket> accepted; ///< The end the listener handed out.
};

/// Dials the listener and accepts the connection. The dial completes out of the kernel's backlog
/// before anything accepts, so the two steps run in sequence on one loop and neither can hang the
/// other.
/// @param loop The loop both ends live on.
/// @param host The listener's host, unbracketed.
/// @param listener The bound listener.
/// @param out Where the two ends go.
Task<void> dialAndAccept(EventLoop* loop, std::string host, IListener* listener, Connection* out)
{
    auto connected = co_await core::net::connect(loop, std::move(host), listener->boundPort());
    if (!connected.has_value())
        co_return;
    out->client = std::move(*connected);
    auto accepted = co_await listener->accept();
    if (accepted.has_value())
        out->accepted = std::move(*accepted);
}

/// @return The IP form of @p endpoint, which the case has already required to exist.
[[nodiscard]] IpSocketAddress ipOf(std::expected<ResolvedEndpoint, NetError> const& endpoint)
{
    REQUIRE(endpoint.has_value());
    auto address = core::net::toIpSocketAddress(*endpoint);
    REQUIRE(address.has_value());
    return *address;
}

/// @return An endpoint for 192.0.2.@p last : @p port, standing for an address an in-memory pair
///         was created with.
[[nodiscard]] ResolvedEndpoint documentationEndpoint(std::uint8_t last, std::uint16_t port)
{
    return core::net::toEndpoint(
        IpSocketAddress { .address = Ipv4Address { { 192, 0, 2, last } }, .port = port });
}

/// @return Whether @p result failed with @p code.
[[nodiscard]] bool failedWith(std::expected<ResolvedEndpoint, NetError> const& result, NetErrorCode code)
{
    return !result.has_value() && result.error().code == code;
}

/// @return Whether @p result is the socket's own refusal of a closed handle: @c BadHandle with no
///         system code, which a call that reached the OS would carry.
[[nodiscard]] bool refusedAsClosed(std::expected<ResolvedEndpoint, NetError> const& result)
{
    return failedWith(result, NetErrorCode::BadHandle) && result.error().systemCode == 0;
}

/// A session that answers nothing: what the forwarding case compares pointers against.
class FakeSession final: public core::net::ITlsSession
{
  public:
    FakeSession() = default;

    [[nodiscard]] std::expected<std::array<std::byte, 32>, std::string> channelBinding() const override
    {
        return std::unexpected(std::string { "fake" });
    }
    [[nodiscard]] std::optional<std::array<std::byte, 32>> peerCertificateDigest() const override
    {
        return std::nullopt;
    }
    [[nodiscard]] std::optional<core::net::TlsVersion> negotiatedVersion() const override
    {
        return std::nullopt;
    }
};

/// A transport that reports @p session as its TLS session, as a TLS socket reports itself.
class SessionSocket final: public core::net::testing::SocketDecorator
{
  public:
    SessionSocket(ISocket& inner, core::net::ITlsSession const& session) noexcept:
        SocketDecorator { inner }, _session { session }
    {
    }

    [[nodiscard]] core::net::ITlsSession const* tlsSession() const noexcept override { return &_session; }

  private:
    core::net::ITlsSession const& _session;
};

} // namespace

TEST_CASE("A loopback connection reports each end's port from both sides", "[net][endpoints][loopback]")
{
    auto const host = GENERATE(std::string { "127.0.0.1" }, std::string { "::1" });
    auto const family = host == "::1" ? AddressFamily::Ipv6 : AddressFamily::Ipv4;

    auto backend = core::net::makeDefaultBackend();
    auto loop = EventLoop { *backend };
    auto listener = core::net::listen(loop, host, 0);
    if (!listener.has_value() && family == AddressFamily::Ipv6)
        SKIP("this host has no IPv6 loopback: " << listener.error().toString());
    REQUIRE(listener.has_value());

    // The listener's own answer agrees with the port it reports it got.
    auto const listening = ipOf((*listener)->localEndpoint());
    CHECK(listening.family() == family);
    CHECK(listening.port == (*listener)->boundPort());
    CHECK(listening.port != 0);

    auto connection = Connection {};
    loop.blockOn(dialAndAccept(&loop, host, listener->get(), &connection));
    REQUIRE(connection.client != nullptr);
    REQUIRE(connection.accepted != nullptr);

    auto const clientLocal = ipOf(connection.client->localEndpoint());
    auto const clientRemote = ipOf(connection.client->remoteEndpoint());
    auto const acceptedLocal = ipOf(connection.accepted->localEndpoint());
    auto const acceptedRemote = ipOf(connection.accepted->remoteEndpoint());

    // The client dialled the listener's port, and the accepted end sits on it.
    CHECK(clientRemote.port == listening.port);
    CHECK(acceptedLocal.port == listening.port);

    // The client's own port is an ephemeral one the kernel chose, and the accepted end sees it.
    CHECK(clientLocal.port != 0);
    CHECK(clientLocal.port != listening.port);
    CHECK(acceptedRemote.port == clientLocal.port);

    // Every end speaks the family it was opened with, and the two sides agree on the hosts.
    CHECK(clientLocal.family() == family);
    CHECK(clientRemote.family() == family);
    CHECK(acceptedLocal.family() == family);
    CHECK(acceptedRemote.family() == family);
    CHECK(acceptedRemote.address == clientLocal.address);
    CHECK(clientRemote.address == acceptedLocal.address);

    SECTION("A closed socket and a closed listener answer BadHandle, without asking the OS")
    {
        // The socket refuses on its own (system code 0) rather than handing a closed descriptor to
        // `getsockname`: a descriptor number may already belong to someone else by then, and this
        // is the one assertion that tells the socket's refusal from the kernel's EBADF.
        connection.client->close();
        CHECK(refusedAsClosed(connection.client->localEndpoint()));
        CHECK(refusedAsClosed(connection.client->remoteEndpoint()));
        (*listener)->close();
        CHECK(refusedAsClosed((*listener)->localEndpoint()));
    }

    SECTION("A decorator and a split socket forward both")
    {
        auto decorator = core::net::testing::SocketDecorator { *connection.client };
        CHECK(ipOf(decorator.localEndpoint()) == clientLocal);
        CHECK(ipOf(decorator.remoteEndpoint()) == clientRemote);

        // The duplex socket reports its READ half's endpoints, as it reports its peer address.
        auto pair = InMemorySocketPair::create();
        auto split = core::net::combineHalves(std::move(connection.client), std::move(pair.client));
        CHECK(ipOf(split->localEndpoint()) == clientLocal);
        CHECK(ipOf(split->remoteEndpoint()) == clientRemote);
    }
}

TEST_CASE("An in-memory pair reports the endpoints it was created with", "[net][endpoints][inmemory]")
{
    auto const clientAddress = documentationEndpoint(1, 50'000);
    auto const serverAddress = documentationEndpoint(2, 443);
    auto pair = InMemorySocketPair::create(
        0, {}, InMemoryPairEndpoints { .client = clientAddress, .server = serverAddress });

    CHECK(ipOf(pair.client->localEndpoint()) == ipOf(clientAddress));
    CHECK(ipOf(pair.client->remoteEndpoint()) == ipOf(serverAddress));
    CHECK(ipOf(pair.server->localEndpoint()) == ipOf(serverAddress));
    CHECK(ipOf(pair.server->remoteEndpoint()) == ipOf(clientAddress));

    SECTION("A decorator forwards both")
    {
        auto decorator = core::net::testing::SocketDecorator { *pair.server };
        CHECK(ipOf(decorator.localEndpoint()) == ipOf(serverAddress));
        CHECK(ipOf(decorator.remoteEndpoint()) == ipOf(clientAddress));
    }

    SECTION("After close it answers BadHandle, as the platform sockets do")
    {
        pair.client->close();
        CHECK(failedWith(pair.client->localEndpoint(), NetErrorCode::BadHandle));
        CHECK(failedWith(pair.client->remoteEndpoint(), NetErrorCode::BadHandle));
    }
}

TEST_CASE("A transport with no address answers Unsupported", "[net][endpoints][inmemory]")
{
    SECTION("An in-memory pair created without endpoints")
    {
        auto pair = InMemorySocketPair::create();
        CHECK(failedWith(pair.client->localEndpoint(), NetErrorCode::Unsupported));
        CHECK(failedWith(pair.server->remoteEndpoint(), NetErrorCode::Unsupported));
    }

    SECTION("An in-memory socket given only one of the two")
    {
        auto pair = InMemorySocketPair::create(
            0, {}, InMemoryPairEndpoints { .client = documentationEndpoint(3, 1), .server = std::nullopt });
        CHECK(pair.client->localEndpoint().has_value());
        CHECK(failedWith(pair.client->remoteEndpoint(), NetErrorCode::Unsupported));
        CHECK(failedWith(pair.server->localEndpoint(), NetErrorCode::Unsupported));
        CHECK(pair.server->remoteEndpoint().has_value());
    }

    SECTION("A listener that does not override the default")
    {
        auto listener = core::net::testing::InMemoryListener {};
        CHECK(failedWith(listener.localEndpoint(), NetErrorCode::Unsupported));
    }

    SECTION("Endpoints given to one InMemorySocket directly")
    {
        auto const local = documentationEndpoint(4, 4);
        auto inbound = std::make_shared<InMemoryPipe>(0);
        auto outbound = std::make_shared<InMemoryPipe>(0);
        auto socket = InMemorySocket {
            inbound, outbound, {}, InMemoryEndpoints { .local = local, .remote = std::nullopt }
        };
        CHECK(ipOf(socket.localEndpoint()) == ipOf(local));
        CHECK(failedWith(socket.remoteEndpoint(), NetErrorCode::Unsupported));
    }
}

TEST_CASE("A decorator forwards the TLS session of the socket it decorates",
          "[net][socket][decorator][tls-session]")
{
    // `http` reads a connection's TLS failure and ALPN through `ISocket::tlsSession()`, whatever
    // decorates the TLS socket by then. A decorator that answered the base's null would make a
    // TLS connection look like a plaintext one.
    auto backend = core::net::makeDefaultBackend();
    auto loop = EventLoop { *backend };
    auto pair = core::net::testing::makeSocketPair(loop);
    REQUIRE(pair.has_value());
    auto const session = FakeSession {};
    CHECK(pair->first->tlsSession() == nullptr); // a plain transport has none

    SECTION("SocketDecorator")
    {
        auto tls = SessionSocket { *pair->first, session };
        auto& transport = static_cast<ISocket&>(tls); // not a copy of the decorator
        auto const decorated = core::net::testing::SocketDecorator { transport };
        CHECK(decorated.tlsSession() == &session);
    }

    SECTION("SplitSocket answers from its read half")
    {
        auto const split =
            core::net::combineHalves(std::make_unique<SessionSocket>(*pair->first, session),
                                     std::make_unique<core::net::testing::SocketDecorator>(*pair->second));
        CHECK(split->tlsSession() == &session);

        auto const reversed =
            core::net::combineHalves(std::make_unique<core::net::testing::SocketDecorator>(*pair->second),
                                     std::make_unique<SessionSocket>(*pair->first, session));
        CHECK(reversed->tlsSession() == nullptr);
    }
}
