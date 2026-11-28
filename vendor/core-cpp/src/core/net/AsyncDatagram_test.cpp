// SPDX-License-Identifier: Apache-2.0
//
// `IAsyncDatagramSocket`'s state machine and cancellation, over `testing::InMemoryAsyncDatagramNetwork`.
//
// **In memory, and on a loop that waits on nothing**, because what is asserted here is the
// socket's own: which state refuses what, what a stop and a close each resolve a pending receive
// with, and that a datagram already taken is never lost to a stop. The double runs the real
// socket's state machine, so every refusal here is the real socket's too; what only the kernel can
// answer is `AsyncDatagramLoopback_test.cpp`.
#include <core/async/Cancellation.hpp>
#include <core/async/StopToken.hpp>
#include <core/async/Task.hpp>
#include <core/net/IAsyncDatagramSocket.hpp>
#include <core/net/IpSocketAddress.hpp>
#include <core/net/NetError.hpp>
#include <core/net/SocketAddress.hpp>
#include <core/net/SocketOption.hpp>
#include <core/net/UdpSocket.hpp>
#include <core/net/testing/InMemoryAsyncDatagram.hpp>
#include <core/net/testing/TestLoop.hpp>
#include <core/platform/Clock.hpp>

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include <cstddef>
#include <cstdint>
#include <expected>
#include <limits>
#include <memory>
#include <optional>
#include <utility>
#include <variant>
#include <vector>

using core::async::Task;
using core::net::AddressFamily;
using core::net::AsyncDatagram;
using core::net::IAsyncDatagramSocket;
using core::net::IpSocketAddress;
using core::net::Ipv4Address;
using core::net::Ipv6Address;
using core::net::NetError;
using core::net::NetErrorCode;
using core::net::ResolvedEndpoint;
using core::net::SocketOption;
using core::net::testing::InMemoryAsyncDatagramNetwork;
using core::net::testing::TestLoop;

namespace
{

/// @return The loopback address of @p family, at @p port.
[[nodiscard]] ResolvedEndpoint loopback(AddressFamily family, std::uint16_t port)
{
    if (family == AddressFamily::Ipv6)
        return core::net::toEndpoint(
            IpSocketAddress { .address = Ipv6Address { { 0, 0, 0, 0, 0, 0, 0, 1 } }, .port = port });
    return core::net::toEndpoint(
        IpSocketAddress { .address = Ipv4Address { { 127, 0, 0, 1 } }, .port = port });
}

/// @return @p family's wildcard address at @p port.
[[nodiscard]] ResolvedEndpoint wildcard(AddressFamily family, std::uint16_t port)
{
    if (family == AddressFamily::Ipv6)
        return core::net::toEndpoint(IpSocketAddress { .address = Ipv6Address {}, .port = port });
    return core::net::toEndpoint(IpSocketAddress { .address = Ipv4Address {}, .port = port });
}

/// @return The IP form of @p endpoint, which the case requires to exist.
[[nodiscard]] IpSocketAddress ipOf(std::expected<ResolvedEndpoint, NetError> const& endpoint)
{
    REQUIRE(endpoint.has_value());
    auto address = core::net::toIpSocketAddress(*endpoint);
    REQUIRE(address.has_value());
    return *address;
}

/// @return Whether @p result failed with @p code.
template <typename T>
[[nodiscard]] bool failedWithCode(std::expected<T, NetError> const& result, NetErrorCode code)
{
    return !result.has_value() && result.error().code == code;
}

/// @return Whether @p result failed with @p code and no system code: a refusal that never
///         reached an OS.
template <typename T>
[[nodiscard]] bool refusedWith(std::expected<T, NetError> const& result, NetErrorCode code)
{
    return !result.has_value() && result.error().code == code && result.error().systemCode == 0;
}

/// @return @p count bytes, each its own index.
[[nodiscard]] std::vector<std::byte> bytes(std::size_t count)
{
    auto out = std::vector<std::byte>(count);
    for (auto index = std::size_t { 0 }; auto& value: out)
        value = static_cast<std::byte>(index++ & 0xFFU);
    return out;
}

/// What a receiving flow ended with.
struct Received
{
    std::optional<std::expected<AsyncDatagram, NetError>> result; ///< Set when it returned.
    bool isCancelled = false;                                     ///< Set when it unwound.
    bool isFinished = false;                                      ///< Set either way.
};

/// Receives one datagram into @p out, recording an unwind rather than letting it escape.
Task<void> receiveInto(IAsyncDatagramSocket* socket, std::size_t maxBytes, Received* out)
{
    try
    {
        out->result = co_await socket->receive(maxBytes);
    }
    catch (core::async::OperationCancelled const&)
    {
        out->isCancelled = true;
    }
    out->isFinished = true;
}

/// Sends one datagram and records the outcome.
Task<void> sendInto(IAsyncDatagramSocket* socket,
                    std::vector<std::byte> payload,
                    std::optional<ResolvedEndpoint> to,
                    std::optional<std::expected<void, NetError>>* out)
{
    *out = co_await socket->send(std::move(payload), to);
}

/// The network, a loop, and the means to run a flow on it.
struct Fixture
{
    core::platform::ManualClock clock {};
    TestLoop loop { clock };
    InMemoryAsyncDatagramNetwork network {};

    /// @return An open, unbound socket of @p family.
    [[nodiscard]] std::unique_ptr<IAsyncDatagramSocket> open(AddressFamily family)
    {
        auto opened = network.open(loop, family);
        REQUIRE(opened.has_value());
        return std::move(*opened);
    }

    /// @return A socket of @p family bound to its loopback at a port the network chose.
    [[nodiscard]] std::unique_ptr<IAsyncDatagramSocket> bound(AddressFamily family)
    {
        auto socket = open(family);
        REQUIRE(socket->bind(loopback(family, 0)).has_value());
        return socket;
    }

    /// Sends @p payload from @p socket and returns what the send answered.
    [[nodiscard]] std::expected<void, NetError> send(IAsyncDatagramSocket& socket,
                                                     std::vector<std::byte> payload,
                                                     std::optional<ResolvedEndpoint> to)
    {
        auto out = std::optional<std::expected<void, NetError>> {};
        auto flow = sendInto(&socket, std::move(payload), to, &out);
        flow.handle().resume();
        std::ignore = loop.drain();
        REQUIRE(out.has_value());
        return *out;
    }

    /// Starts @p flow and runs the loop until nothing is left to do.
    void run(Task<void>& flow)
    {
        flow.handle().resume();
        std::ignore = loop.drain();
    }

    /// @return What one receive on @p socket answered, the loop run until it settled.
    [[nodiscard]] std::expected<AsyncDatagram, NetError> receive(IAsyncDatagramSocket& socket,
                                                                 std::size_t maxBytes)
    {
        auto out = Received {};
        auto flow = receiveInto(&socket, maxBytes, &out);
        run(flow);
        REQUIRE(out.result.has_value());
        return std::move(*out.result);
    }
};

[[nodiscard]] AddressFamily eachFamily()
{
    return GENERATE(AddressFamily::Ipv4, AddressFamily::Ipv6);
}

} // namespace

TEST_CASE("An in-memory datagram arrives whole, with its sender", "[net][async-datagram][inmemory]")
{
    auto const family = eachFamily();
    auto fixture = Fixture {};
    auto server = fixture.bound(family);
    auto client = fixture.bound(family);

    REQUIRE(fixture.send(*client, bytes(300), server->localEndpoint().value()).has_value());
    auto const got = fixture.receive(*server, 1024);
    REQUIRE(got.has_value());
    CHECK(got->payload == bytes(300));
    CHECK(ipOf(got->from) == ipOf(client->localEndpoint()));

    // And back, to the address the datagram said it came from.
    REQUIRE(fixture.send(*server, bytes(7), got->from).has_value());
    auto const echoed = fixture.receive(*client, 7);
    REQUIRE(echoed.has_value());
    CHECK(echoed->payload == bytes(7));
    CHECK(ipOf(echoed->from) == ipOf(server->localEndpoint()));
}

TEST_CASE("An in-memory receive on an unbound socket is refused as BadHandle",
          "[net][async-datagram][inmemory]")
{
    auto fixture = Fixture {};
    auto socket = fixture.open(AddressFamily::Ipv4);
    CHECK(refusedWith(fixture.receive(*socket, 64), NetErrorCode::BadHandle));
    CHECK(refusedWith(socket->localEndpoint(), NetErrorCode::BadHandle));
}

TEST_CASE("A stop during an in-memory receive throws, and leaves the socket usable",
          "[net][async-datagram][inmemory]")
{
    auto fixture = Fixture {};
    auto server = fixture.bound(AddressFamily::Ipv4);
    auto client = fixture.bound(AddressFamily::Ipv4);

    auto source = core::async::StopSource {};
    auto out = Received {};
    auto flow = receiveInto(server.get(), 64, &out);
    flow.handle().promise().setStopToken(source.get_token());
    fixture.run(flow);
    REQUIRE_FALSE(out.isFinished);

    source.request_stop();
    std::ignore = fixture.loop.drain();
    CHECK(out.isFinished);
    CHECK(out.isCancelled);
    CHECK_FALSE(out.result.has_value());

    // A stop is not a close: the next receive answers the next datagram.
    REQUIRE(fixture.send(*client, bytes(5), server->localEndpoint().value()).has_value());
    auto const got = fixture.receive(*server, 64);
    REQUIRE(got.has_value());
    CHECK(got->payload == bytes(5));
}

TEST_CASE("close() during an in-memory receive resolves it with Cancelled", "[net][async-datagram][inmemory]")
{
    auto fixture = Fixture {};
    auto server = fixture.bound(AddressFamily::Ipv4);

    auto out = Received {};
    auto flow = receiveInto(server.get(), 64, &out);
    fixture.run(flow);
    REQUIRE_FALSE(out.isFinished);

    server->close();
    std::ignore = fixture.loop.drain();
    REQUIRE(out.isFinished);
    CHECK_FALSE(out.isCancelled);
    REQUIRE(out.result.has_value());
    CHECK(refusedWith(*out.result, NetErrorCode::Cancelled));
}

TEST_CASE("Destroying an in-memory socket during a receive resolves it with Cancelled",
          "[net][async-datagram][inmemory]")
{
    auto fixture = Fixture {};
    auto server = fixture.bound(AddressFamily::Ipv4);

    auto out = Received {};
    auto flow = receiveInto(server.get(), 64, &out);
    fixture.run(flow);
    REQUIRE_FALSE(out.isFinished);

    // The frame keeps its own share of what it reads after it resumes.
    server.reset();
    std::ignore = fixture.loop.drain();
    REQUIRE(out.isFinished);
    REQUIRE(out.result.has_value());
    CHECK(refusedWith(*out.result, NetErrorCode::Cancelled));
}

TEST_CASE("A datagram that arrived before the stop is delivered, not dropped",
          "[net][async-datagram][inmemory]")
{
    auto fixture = Fixture {};
    auto server = fixture.bound(AddressFamily::Ipv4);
    auto client = fixture.bound(AddressFamily::Ipv4);

    auto source = core::async::StopSource {};
    auto out = Received {};
    auto flow = receiveInto(server.get(), 64, &out);
    flow.handle().promise().setStopToken(source.get_token());
    fixture.run(flow);
    REQUIRE_FALSE(out.isFinished);

    // The datagram reaches the parked receive, and the stop lands before the loop resumes it.
    auto sent = std::optional<std::expected<void, NetError>> {};
    auto sending = sendInto(client.get(), bytes(9), server->localEndpoint().value(), &sent);
    sending.handle().resume();
    REQUIRE(sent.has_value());
    REQUIRE(sent->has_value());
    REQUIRE_FALSE(out.isFinished);
    source.request_stop();
    std::ignore = fixture.loop.drain();

    REQUIRE(out.isFinished);
    CHECK_FALSE(out.isCancelled);
    REQUIRE(out.result.has_value());
    REQUIRE(out.result->has_value());
    CHECK((*out.result)->payload == bytes(9));
}

TEST_CASE("An oversized in-memory datagram is MessageTooLarge, and the next one still arrives",
          "[net][async-datagram][inmemory]")
{
    auto fixture = Fixture {};
    auto server = fixture.bound(AddressFamily::Ipv4);
    auto client = fixture.bound(AddressFamily::Ipv4);
    auto const to = server->localEndpoint().value();

    REQUIRE(fixture.send(*client, bytes(10), to).has_value());
    REQUIRE(fixture.send(*client, bytes(4), to).has_value());

    // Whole or not at all: ten bytes do not fit four, and are not handed back as four.
    CHECK(refusedWith(fixture.receive(*server, 4), NetErrorCode::MessageTooLarge));
    // Exactly the limit fits.
    auto const next = fixture.receive(*server, 4);
    REQUIRE(next.has_value());
    CHECK(next->payload == bytes(4));
}

TEST_CASE("An in-memory payload above the family's largest is refused at the send",
          "[net][async-datagram][inmemory]")
{
    auto fixture = Fixture {};
    auto server = fixture.bound(AddressFamily::Ipv6);
    auto client = fixture.bound(AddressFamily::Ipv6);
    auto const to = server->localEndpoint().value();

    CHECK(refusedWith(fixture.send(*client, bytes(core::net::MaxIpv6DatagramPayload + 1), to),
                      NetErrorCode::MessageTooLarge));
    REQUIRE(fixture.send(*client, bytes(core::net::MaxIpv6DatagramPayload), to).has_value());
    // A limit beyond the largest datagram is clamped, not allocated.
    auto const got = fixture.receive(*server, std::numeric_limits<std::size_t>::max());
    REQUIRE(got.has_value());
    CHECK(got->payload.size() == core::net::MaxIpv6DatagramPayload);

    auto v4 = fixture.bound(AddressFamily::Ipv4);
    auto v4Client = fixture.bound(AddressFamily::Ipv4);
    CHECK(refusedWith(
        fixture.send(*v4Client, bytes(core::net::MaxIpv4DatagramPayload + 1), v4->localEndpoint().value()),
        NetErrorCode::MessageTooLarge));
}

TEST_CASE("connect filters what an in-memory socket receives, and disconnect lifts it",
          "[net][async-datagram][inmemory]")
{
    auto fixture = Fixture {};
    auto server = fixture.bound(AddressFamily::Ipv4);
    auto peer = fixture.bound(AddressFamily::Ipv4);
    auto stranger = fixture.bound(AddressFamily::Ipv4);
    auto const to = server->localEndpoint().value();

    REQUIRE(server->connect(peer->localEndpoint().value()).has_value());
    CHECK(ipOf(server->remoteEndpoint()) == ipOf(peer->localEndpoint()));

    REQUIRE(fixture.send(*stranger, bytes(1), to).has_value());
    REQUIRE(fixture.send(*peer, bytes(2), to).has_value());
    auto const fromPeer = fixture.receive(*server, 64);
    REQUIRE(fromPeer.has_value());
    CHECK(fromPeer->payload == bytes(2));

    // A connected socket sends to its remote by default.
    REQUIRE(fixture.send(*server, bytes(3), std::nullopt).has_value());
    auto const atPeer = fixture.receive(*peer, 64);
    REQUIRE(atPeer.has_value());
    CHECK(atPeer->payload == bytes(3));

    REQUIRE(server->connect(std::nullopt).has_value());
    CHECK(refusedWith(server->remoteEndpoint(), NetErrorCode::BadHandle));
    REQUIRE(fixture.send(*stranger, bytes(4), to).has_value());
    auto const fromStranger = fixture.receive(*server, 64);
    REQUIRE(fromStranger.has_value());
    CHECK(fromStranger->payload == bytes(4));
}

TEST_CASE("An in-memory send on an unbound socket binds the wildcard at a fresh port",
          "[net][async-datagram][inmemory]")
{
    auto const family = eachFamily();
    auto fixture = Fixture {};
    auto server = fixture.bound(family);
    auto client = fixture.open(family);

    REQUIRE(fixture.send(*client, bytes(6), server->localEndpoint().value()).has_value());
    auto const local = ipOf(client->localEndpoint());
    CHECK(local.port != 0);
    CHECK(local.address == ipOf(wildcard(family, 0)).address);

    // Answered at the port it was given, from the address a loopback route picks.
    auto const got = fixture.receive(*server, 64);
    REQUIRE(got.has_value());
    CHECK(ipOf(got->from).port == local.port);
    CHECK(ipOf(got->from).address == ipOf(server->localEndpoint()).address);
}

TEST_CASE("In-memory binds that overlap are AddressInUse", "[net][async-datagram][inmemory]")
{
    auto fixture = Fixture {};
    auto first = fixture.bound(AddressFamily::Ipv4);
    auto const port = ipOf(first->localEndpoint()).port;

    auto same = fixture.open(AddressFamily::Ipv4);
    CHECK(failedWithCode(same->bind(loopback(AddressFamily::Ipv4, port)), NetErrorCode::AddressInUse));
    auto any = fixture.open(AddressFamily::Ipv4);
    CHECK(failedWithCode(any->bind(wildcard(AddressFamily::Ipv4, port)), NetErrorCode::AddressInUse));
    // The other family's port space is its own.
    auto other = fixture.open(AddressFamily::Ipv6);
    CHECK(other->bind(loopback(AddressFamily::Ipv6, port)).has_value());
    // A closed socket gives its port back.
    first->close();
    CHECK(same->bind(loopback(AddressFamily::Ipv4, port)).has_value());
}

TEST_CASE("A datagram socket has one receive and one send in flight at a time",
          "[net][async-datagram][inmemory]")
{
    auto fixture = Fixture {};
    auto server = fixture.bound(AddressFamily::Ipv4);
    auto client = fixture.bound(AddressFamily::Ipv4);
    auto const to = server->localEndpoint().value();

    // A receive parked on the loop holds the receive slot; a send may still go.
    auto first = Received {};
    auto parked = receiveInto(server.get(), 64, &first);
    fixture.run(parked);
    REQUIRE_FALSE(first.isFinished);
    CHECK(refusedWith(fixture.receive(*server, 64), NetErrorCode::BadHandle));
    CHECK(fixture.send(*server, bytes(1), client->localEndpoint().value()).has_value());

    // Answered, the slot is free again.
    REQUIRE(fixture.send(*client, bytes(2), to).has_value());
    std::ignore = fixture.loop.drain();
    REQUIRE(first.isFinished);
    REQUIRE(fixture.send(*client, bytes(3), to).has_value());
    auto const next = fixture.receive(*server, 64);
    REQUIRE(next.has_value());
    CHECK(next->payload == bytes(3));

    // A finished task gives its slot back at once, though its owner still holds it.
    REQUIRE(fixture.send(*client, bytes(7), to).has_value());
    REQUIRE(fixture.send(*client, bytes(8), to).has_value());
    auto direct = server->receive(64);
    direct.handle().resume();
    REQUIRE(direct.done());
    auto const after = fixture.receive(*server, 64);
    REQUIRE(after.has_value());
    CHECK(after->payload == bytes(8));

    // A task holds its slot from the call, even one that never runs, until it is destroyed.
    {
        auto const unstarted = client->send(bytes(4), to);
        CHECK(refusedWith(fixture.send(*client, bytes(5), to), NetErrorCode::BadHandle));
        auto const unstartedReceive = client->receive(64);
        CHECK(refusedWith(fixture.receive(*client, 64), NetErrorCode::BadHandle));
    }
    CHECK(fixture.send(*client, bytes(6), to).has_value());
}

TEST_CASE("The datagram socket refuses each state's impossible calls without the OS",
          "[net][async-datagram][inmemory]")
{
    auto fixture = Fixture {};
    auto socket = fixture.open(AddressFamily::Ipv4);
    auto const somewhere = loopback(AddressFamily::Ipv4, 9);

    // Unconnected.
    CHECK(refusedWith(socket->remoteEndpoint(), NetErrorCode::BadHandle));
    CHECK(refusedWith(socket->connect(std::nullopt), NetErrorCode::BadHandle));
    CHECK(refusedWith(fixture.send(*socket, bytes(1), std::nullopt), NetErrorCode::BadHandle));

    // The other family, wherever an endpoint is taken.
    auto const v6 = loopback(AddressFamily::Ipv6, 9);
    CHECK(refusedWith(socket->bind(v6), NetErrorCode::AddressError));
    CHECK(refusedWith(socket->connect(v6), NetErrorCode::AddressError));
    CHECK(refusedWith(fixture.send(*socket, bytes(1), v6), NetErrorCode::AddressError));
    // None of those bound it.
    CHECK(refusedWith(socket->localEndpoint(), NetErrorCode::BadHandle));

    // Bound once.
    REQUIRE(socket->bind(loopback(AddressFamily::Ipv4, 0)).has_value());
    CHECK(refusedWith(socket->bind(loopback(AddressFamily::Ipv4, 0)), NetErrorCode::BadHandle));
    // Bound but unconnected: there is nothing to disconnect.
    CHECK(refusedWith(socket->connect(std::nullopt), NetErrorCode::BadHandle));

    // Connected: a send names the remote or nothing.
    REQUIRE(socket->connect(somewhere).has_value());
    CHECK(refusedWith(fixture.send(*socket, bytes(1), loopback(AddressFamily::Ipv4, 10)),
                      NetErrorCode::AddressError));
    CHECK(fixture.send(*socket, bytes(1), somewhere).has_value());

    // The keep-alive options belong to a stream.
    CHECK(refusedWith(socket->option(SocketOption::KeepAliveEnabled), NetErrorCode::Unsupported));
    CHECK(refusedWith(socket->setOption(SocketOption::KeepAliveCount, 3), NetErrorCode::Unsupported));

    // Closed: everything.
    socket->close();
    socket->close(); // idempotent
    CHECK(refusedWith(socket->bind(loopback(AddressFamily::Ipv4, 0)), NetErrorCode::BadHandle));
    CHECK(refusedWith(socket->connect(somewhere), NetErrorCode::BadHandle));
    CHECK(refusedWith(socket->connect(std::nullopt), NetErrorCode::BadHandle));
    CHECK(refusedWith(socket->localEndpoint(), NetErrorCode::BadHandle));
    CHECK(refusedWith(socket->remoteEndpoint(), NetErrorCode::BadHandle));
    CHECK(refusedWith(socket->option(SocketOption::ReceiveBufferSize), NetErrorCode::BadHandle));
    CHECK(refusedWith(socket->setOption(SocketOption::ReceiveBufferSize, 1), NetErrorCode::BadHandle));
    CHECK(refusedWith(fixture.send(*socket, bytes(1), somewhere), NetErrorCode::BadHandle));
    CHECK(refusedWith(fixture.receive(*socket, 1), NetErrorCode::BadHandle));
}
