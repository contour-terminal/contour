// SPDX-License-Identifier: Apache-2.0
//
// `openAsyncDatagramSocket` over the platform's real stack and its default backend: readiness on
// POSIX, the completion port on Windows.
//
// **Only what the kernel answers is asserted here**: that a datagram crosses loopback whole and
// says who sent it, that a connected socket hears its remote alone, that an unbound sender is
// given a port, that a datagram longer than the receive is dropped rather than cut, and that a
// dead peer's ICMP answer does not fail the next receive. The state machine's refusals are the
// in-memory suite's (`AsyncDatagram_test.cpp`), which runs the same implementation.
#include <core/async/Cancellation.hpp>
#include <core/async/StopToken.hpp>
#include <core/async/Task.hpp>
#include <core/net/EventLoop.hpp>
#include <core/net/IAsyncDatagramSocket.hpp>
#include <core/net/IoBackend.hpp>
#include <core/net/IpSocketAddress.hpp>
#include <core/net/NetError.hpp>
#include <core/net/SocketAddress.hpp>
#include <core/net/SocketOption.hpp>
#include <core/net/testing/CoroTestSupport.hpp>

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <catch2/interfaces/catch_interfaces_capture.hpp>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <limits>
#include <memory>
#include <optional>
#include <utility>
#include <vector>

using core::async::Task;
using core::net::AddressFamily;
using core::net::AsyncDatagram;
using core::net::EventLoop;
using core::net::IAsyncDatagramSocket;
using core::net::IpSocketAddress;
using core::net::Ipv4Address;
using core::net::Ipv6Address;
using core::net::NetError;
using core::net::NetErrorCode;
using core::net::ResolvedEndpoint;
using core::net::SocketOption;

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
[[nodiscard]] bool failedWith(std::expected<T, NetError> const& result, NetErrorCode code)
{
    return !result.has_value() && result.error().code == code;
}

/// @return @p count bytes, each its own index, offset by @p seed so two senders differ.
[[nodiscard]] std::vector<std::byte> bytes(std::size_t count, unsigned seed = 0)
{
    auto out = std::vector<std::byte>(count);
    for (auto index = std::size_t { seed }; auto& value: out)
        value = static_cast<std::byte>(index++ & 0xFFU);
    return out;
}

/// A loop over the platform's default backend, kept alive with it.
struct Loop
{
    std::unique_ptr<core::net::IoBackend> backend = core::net::makeDefaultBackend();
    EventLoop loop { *backend };
};

/// Runs @p task on @p loop, failing the case -- naming it -- if it has not finished within
/// @p limit, so a datagram that never arrives fails one case rather than hanging the binary.
void blockOnWithin(EventLoop& loop,
                   Task<void> task,
                   std::chrono::milliseconds limit = std::chrono::seconds { 5 })
{
    if (!core::net::testing::blockOnWithin(loop, std::move(task), limit))
        FAIL("a datagram wait in \"" << Catch::getResultCapture().getCurrentTestName()
                                     << "\" took longer than " << limit.count() << " ms");
}

/// @return An open, unbound socket of @p family, or a SKIP where the host has no such family.
[[nodiscard]] std::unique_ptr<IAsyncDatagramSocket> openOrSkip(EventLoop& loop, AddressFamily family)
{
    auto opened = core::net::openAsyncDatagramSocket(loop, family);
    if (!opened.has_value() && family == AddressFamily::Ipv6)
        SKIP("this host has no IPv6: " << opened.error().toString());
    REQUIRE(opened.has_value());
    return std::move(*opened);
}

/// @return A socket of @p family bound to its loopback at a kernel-chosen port, or a SKIP where
///         the host has no IPv6 loopback.
[[nodiscard]] std::unique_ptr<IAsyncDatagramSocket> boundOrSkip(EventLoop& loop, AddressFamily family)
{
    auto socket = openOrSkip(loop, family);
    auto const bound = socket->bind(loopback(family, 0));
    if (!bound.has_value() && family == AddressFamily::Ipv6)
        SKIP("this host has no IPv6 loopback: " << bound.error().toString());
    REQUIRE(bound.has_value());
    return socket;
}

/// @return What one receive on @p socket answered, bounded.
[[nodiscard]] std::expected<AsyncDatagram, NetError> receiveWithin(EventLoop& loop,
                                                                   IAsyncDatagramSocket& socket,
                                                                   std::size_t maxBytes)
{
    auto out = std::optional<std::expected<AsyncDatagram, NetError>> {};
    blockOnWithin(loop, [](IAsyncDatagramSocket* s, std::size_t max, decltype(out)* into) -> Task<void> {
        *into = co_await s->receive(max);
    }(&socket, maxBytes, &out));
    REQUIRE(out.has_value());
    return std::move(*out);
}

/// @return What one send from @p socket answered, bounded.
[[nodiscard]] std::expected<void, NetError> sendWithin(EventLoop& loop,
                                                       IAsyncDatagramSocket& socket,
                                                       std::vector<std::byte> payload,
                                                       std::optional<ResolvedEndpoint> to)
{
    auto out = std::optional<std::expected<void, NetError>> {};
    blockOnWithin(loop,
                  [](IAsyncDatagramSocket* s,
                     std::vector<std::byte> bytesToSend,
                     std::optional<ResolvedEndpoint> destination,
                     decltype(out)* into) -> Task<void> {
                      *into = co_await s->send(std::move(bytesToSend), destination);
                  }(&socket, std::move(payload), to, &out));
    REQUIRE(out.has_value());
    return *out;
}

[[nodiscard]] AddressFamily eachFamily()
{
    return GENERATE(AddressFamily::Ipv4, AddressFamily::Ipv6);
}

} // namespace

TEST_CASE("A datagram echoes over loopback, with its sender", "[net][async-datagram][loopback]")
{
    auto const family = eachFamily();
    auto context = Loop {};
    auto server = boundOrSkip(context.loop, family);
    auto client = boundOrSkip(context.loop, family);

    REQUIRE(sendWithin(context.loop, *client, bytes(1200), server->localEndpoint().value()).has_value());
    auto const got = receiveWithin(context.loop, *server, 2048);
    REQUIRE(got.has_value());
    CHECK(got->payload == bytes(1200));
    CHECK(ipOf(got->from) == ipOf(client->localEndpoint()));

    REQUIRE(sendWithin(context.loop, *server, got->payload, got->from).has_value());
    auto const echoed = receiveWithin(context.loop, *client, 2048);
    REQUIRE(echoed.has_value());
    CHECK(echoed->payload == bytes(1200));
    CHECK(ipOf(echoed->from) == ipOf(server->localEndpoint()));
}

TEST_CASE("A receive and a send in flight together both complete", "[net][async-datagram][loopback]")
{
    auto context = Loop {};
    auto server = boundOrSkip(context.loop, AddressFamily::Ipv4);
    auto client = boundOrSkip(context.loop, AddressFamily::Ipv4);
    auto const to = server->localEndpoint().value();

    // The receive parks before the datagram exists, so the readiness or completion path -- not a
    // datagram already queued -- is what answers it.
    auto received = std::optional<std::expected<AsyncDatagram, NetError>> {};
    auto sent = std::optional<std::expected<void, NetError>> {};
    blockOnWithin(
        context.loop,
        core::net::testing::allOf(
            [](IAsyncDatagramSocket* s, decltype(received)* into) -> Task<void> {
                *into = co_await s->receive(64);
            }(server.get(), &received),
            [](EventLoop* loop, IAsyncDatagramSocket* s, ResolvedEndpoint target, decltype(sent)* into)
                -> Task<void> {
                co_await loop->delay(std::chrono::milliseconds { 20 });
                *into = co_await s->send(bytes(33), target);
            }(&context.loop, client.get(), to, &sent)));
    REQUIRE(sent.has_value());
    CHECK(sent->has_value());
    REQUIRE(received.has_value());
    REQUIRE(received->has_value());
    CHECK((*received)->payload == bytes(33));
}

TEST_CASE("A connected datagram socket hears its remote alone", "[net][async-datagram][loopback]")
{
    auto context = Loop {};
    auto server = boundOrSkip(context.loop, AddressFamily::Ipv4);
    auto peer = boundOrSkip(context.loop, AddressFamily::Ipv4);
    auto stranger = boundOrSkip(context.loop, AddressFamily::Ipv4);
    auto const to = server->localEndpoint().value();

    REQUIRE(server->connect(peer->localEndpoint().value()).has_value());
    CHECK(ipOf(server->remoteEndpoint()) == ipOf(peer->localEndpoint()));

    REQUIRE(sendWithin(context.loop, *stranger, bytes(10, 1), to).has_value());
    REQUIRE(sendWithin(context.loop, *peer, bytes(10, 2), to).has_value());
    auto const first = receiveWithin(context.loop, *server, 64);
    REQUIRE(first.has_value());
    CHECK(first->payload == bytes(10, 2));
    CHECK(ipOf(first->from) == ipOf(peer->localEndpoint()));

    // To the remote, by default and by name: the second is what macOS refuses as EISCONN unless
    // the socket leaves the address off.
    REQUIRE(sendWithin(context.loop, *server, bytes(3, 3), std::nullopt).has_value());
    REQUIRE(sendWithin(context.loop, *server, bytes(3, 4), peer->localEndpoint().value()).has_value());
    auto const byDefault = receiveWithin(context.loop, *peer, 64);
    REQUIRE(byDefault.has_value());
    CHECK(byDefault->payload == bytes(3, 3));
    auto const byName = receiveWithin(context.loop, *peer, 64);
    REQUIRE(byName.has_value());
    CHECK(byName->payload == bytes(3, 4));

    // Disconnected, the stranger is heard again -- at the same port, which Linux takes from a
    // socket that bound port 0 unless it is bound again.
    auto const before = ipOf(server->localEndpoint());
    auto const disconnected = server->connect(std::nullopt);
    if (!disconnected.has_value())
    {
        // macOS and the BSDs widen the loopback address to the wildcard at a disconnect: the socket
        // is closed rather than left hearing every interface.
        CHECK(disconnected.error().code == NetErrorCode::Unsupported);
        CHECK(failedWith(server->localEndpoint(), NetErrorCode::BadHandle));
        SKIP("this system widens a bound address at a disconnect; the socket was closed, as it must be");
    }
    CHECK(ipOf(server->localEndpoint()) == before);
    CHECK(failedWith(server->remoteEndpoint(), NetErrorCode::BadHandle));
    REQUIRE(sendWithin(context.loop, *stranger, bytes(10, 5), to).has_value());
    auto const after = receiveWithin(context.loop, *server, 64);
    REQUIRE(after.has_value());
    CHECK(after->payload == bytes(10, 5));
    CHECK(ipOf(after->from) == ipOf(stranger->localEndpoint()));
}

TEST_CASE("A disconnect keeps the wildcard a datagram socket bound, at its port",
          "[net][async-datagram][loopback]")
{
    // While connected, `getsockname` reports the route's source address; binding that again after
    // the disconnect would leave the socket hearing one interface rather than every one.
    auto const family = eachFamily();
    auto context = Loop {};
    auto socket = openOrSkip(context.loop, family);
    auto const any = family == AddressFamily::Ipv6
                         ? core::net::toEndpoint(IpSocketAddress { .address = Ipv6Address {}, .port = 0 })
                         : core::net::toEndpoint(IpSocketAddress { .address = Ipv4Address {}, .port = 0 });
    REQUIRE(socket->bind(any).has_value());
    auto const port = ipOf(socket->localEndpoint()).port;
    REQUIRE(port != 0);
    auto peer = boundOrSkip(context.loop, family);

    REQUIRE(socket->connect(peer->localEndpoint().value()).has_value());
    REQUIRE(socket->connect(std::nullopt).has_value());

    auto const local = ipOf(socket->localEndpoint());
    CHECK(local.address == ipOf(any).address);
    CHECK(local.port == port);
}

TEST_CASE("A send on an unbound datagram socket binds it to an ephemeral port",
          "[net][async-datagram][loopback]")
{
    auto const family = eachFamily();
    auto context = Loop {};
    auto server = boundOrSkip(context.loop, family);
    auto client = openOrSkip(context.loop, family);

    REQUIRE(sendWithin(context.loop, *client, bytes(8), server->localEndpoint().value()).has_value());
    auto const local = ipOf(client->localEndpoint());
    CHECK(local.port != 0);

    auto const got = receiveWithin(context.loop, *server, 64);
    REQUIRE(got.has_value());
    CHECK(ipOf(got->from).port == local.port);

    // Bound now, so it receives the answer at that port.
    REQUIRE(sendWithin(context.loop, *server, bytes(2), got->from).has_value());
    auto const answer = receiveWithin(context.loop, *client, 64);
    REQUIRE(answer.has_value());
    CHECK(answer->payload == bytes(2));
}

TEST_CASE("A datagram longer than the receive is MessageTooLarge, never cut short",
          "[net][async-datagram][loopback]")
{
    auto context = Loop {};
    auto server = boundOrSkip(context.loop, AddressFamily::Ipv4);
    auto client = boundOrSkip(context.loop, AddressFamily::Ipv4);
    auto const to = server->localEndpoint().value();

    REQUIRE(sendWithin(context.loop, *client, bytes(100), to).has_value());
    REQUIRE(sendWithin(context.loop, *client, bytes(10), to).has_value());
    REQUIRE(sendWithin(context.loop, *client, bytes(1000), to).has_value());

    CHECK(failedWith(receiveWithin(context.loop, *server, 10 - 1), NetErrorCode::MessageTooLarge));
    auto const exact = receiveWithin(context.loop, *server, 10);
    REQUIRE(exact.has_value());
    CHECK(exact->payload == bytes(10));
    // A limit past the largest datagram is clamped rather than allocated.
    auto const unlimited = receiveWithin(context.loop, *server, std::numeric_limits<std::size_t>::max());
    REQUIRE(unlimited.has_value());
    CHECK(unlimited->payload == bytes(1000));
}

TEST_CASE("A dead peer's ICMP answer does not fail the next receive", "[net][async-datagram][loopback]")
{
    // On Windows an unconnected UDP socket that sent to a closed port fails its next receive with
    // WSAECONNRESET, unless SIO_UDP_CONNRESET is turned off. Elsewhere this is how it always was.
    auto context = Loop {};
    auto server = boundOrSkip(context.loop, AddressFamily::Ipv4);
    auto peer = boundOrSkip(context.loop, AddressFamily::Ipv4);

    auto const deadPort = [&] {
        auto dead = boundOrSkip(context.loop, AddressFamily::Ipv4);
        return ipOf(dead->localEndpoint()).port;
    }();

    REQUIRE(sendWithin(context.loop, *server, bytes(4), loopback(AddressFamily::Ipv4, deadPort)).has_value());
    // Time for the port-unreachable to come back before anything else does.
    blockOnWithin(context.loop,
                  core::net::testing::sleepFor(&context.loop, std::chrono::milliseconds { 50 }));
    REQUIRE(sendWithin(context.loop, *peer, bytes(6), server->localEndpoint().value()).has_value());

    auto const got = receiveWithin(context.loop, *server, 64);
    REQUIRE(got.has_value());
    CHECK(got->payload == bytes(6));
}

TEST_CASE("A stop or a close ends a real datagram receive", "[net][async-datagram][loopback]")
{
    auto context = Loop {};
    auto server = boundOrSkip(context.loop, AddressFamily::Ipv4);

    SECTION("a stop throws, and the socket still receives")
    {
        auto isCancelled = false;
        blockOnWithin(context.loop,
                      core::net::testing::anyOf(
                          [](IAsyncDatagramSocket* s, bool* cancelled) -> Task<void> {
                              try
                              {
                                  std::ignore = co_await s->receive(64);
                              }
                              catch (core::async::OperationCancelled const&)
                              {
                                  *cancelled = true;
                                  throw;
                              }
                          }(server.get(), &isCancelled),
                          core::net::testing::sleepFor(&context.loop, std::chrono::milliseconds { 20 })));
        CHECK(isCancelled);

        auto client = boundOrSkip(context.loop, AddressFamily::Ipv4);
        REQUIRE(sendWithin(context.loop, *client, bytes(5), server->localEndpoint().value()).has_value());
        auto const got = receiveWithin(context.loop, *server, 64);
        REQUIRE(got.has_value());
        CHECK(got->payload == bytes(5));
    }

    SECTION("a close resolves it with Cancelled")
    {
        auto out = std::optional<std::expected<AsyncDatagram, NetError>> {};
        blockOnWithin(context.loop,
                      core::net::testing::allOf(
                          [](IAsyncDatagramSocket* s, decltype(out)* into) -> Task<void> {
                              *into = co_await s->receive(64);
                          }(server.get(), &out),
                          [](EventLoop* loop, IAsyncDatagramSocket* s) -> Task<void> {
                              co_await loop->delay(std::chrono::milliseconds { 20 });
                              s->close();
                          }(&context.loop, server.get())));
        REQUIRE(out.has_value());
        CHECK(failedWith(*out, NetErrorCode::Cancelled));
        CHECK(failedWith(server->localEndpoint(), NetErrorCode::BadHandle));
    }

    SECTION("destroying the socket resolves it with Cancelled")
    {
        auto out = std::optional<std::expected<AsyncDatagram, NetError>> {};
        blockOnWithin(context.loop,
                      core::net::testing::allOf(
                          [](IAsyncDatagramSocket* s, decltype(out)* into) -> Task<void> {
                              *into = co_await s->receive(64);
                          }(server.get(), &out),
                          [](EventLoop* loop, std::unique_ptr<IAsyncDatagramSocket>* s) -> Task<void> {
                              co_await loop->delay(std::chrono::milliseconds { 20 });
                              s->reset();
                          }(&context.loop, &server)));
        REQUIRE(out.has_value());
        CHECK(failedWith(*out, NetErrorCode::Cancelled));
    }
}

TEST_CASE("A datagram socket sets and reads back its options", "[net][async-datagram][loopback]")
{
    auto const family = eachFamily();
    auto context = Loop {};
    auto socket = openOrSkip(context.loop, family);

    REQUIRE(socket->setOption(SocketOption::UnicastHopLimit, 17).has_value());
    CHECK(socket->option(SocketOption::UnicastHopLimit) == 17U);
    REQUIRE(socket->setOption(SocketOption::ReceiveBufferSize, 65536).has_value());
    auto const receiveBuffer = socket->option(SocketOption::ReceiveBufferSize);
    REQUIRE(receiveBuffer.has_value());
    CHECK(*receiveBuffer >= 4096U);
    REQUIRE(socket->setOption(SocketOption::SendBufferSize, 65536).has_value());
    CHECK(socket->option(SocketOption::SendBufferSize).has_value());

    // A row exists for keep-alive, and a datagram socket still refuses it: it is a stream's.
    CHECK(failedWith(socket->option(SocketOption::KeepAliveEnabled), NetErrorCode::Unsupported));
    CHECK(failedWith(socket->setOption(SocketOption::KeepAliveIdleTime, 30), NetErrorCode::Unsupported));
}
