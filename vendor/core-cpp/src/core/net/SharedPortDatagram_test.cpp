// SPDX-License-Identifier: Apache-2.0
#include <core/net/SharedPortDatagram.hpp>
#include <core/net/UdpSocket.hpp>
#include <core/net/testing/DatagramPayload.hpp>
#include <core/net/testing/InMemoryDatagram.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <memory>
#include <ranges>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

using namespace std::chrono_literals;
using core::net::answerFromOwnAddress;
using core::net::BroadcastMode;
using core::net::DatagramAddress;
using core::net::DatagramWait;
using core::net::IDatagramSocket;
using core::net::NetErrorCode;
using core::net::openSharedPortUdpSocket;
using core::net::openUdpSocket;
using core::net::PortSharing;
using core::net::testing::DatagramBus;
using core::net::testing::datagramBytes;
using core::net::testing::datagramText;

namespace
{

/// The port every node in this segment listens for beacons on.
///
/// Named once rather than per case, because it is half of the distinction every co-hosted case
/// turns on — "the port everybody bound" against "the address only I hold" — and two cases that
/// drifted to different numbers would each still pass while describing different segments.
constexpr std::uint16_t TestBeaconPort = 6681;

/// A socket for a node sharing a host, and therefore a beacon port, with others.
///
/// Two halves, which is the point: the shared one hears the broadcast every node on the segment
/// listens for, and the private one is what this node sends from — so the address a peer replies to
/// names this node rather than the machine it happens to share.
/// @param bus The segment.
/// @param host The machine; co-hosted nodes pass the same one.
/// @param ownPort The port only this node holds.
/// @return The pair, as one socket.
[[nodiscard]] std::unique_ptr<IDatagramSocket> coHostedDatagramSocket(DatagramBus& bus,
                                                                      std::string_view host,
                                                                      std::uint16_t ownPort)
{
    auto paired = answerFromOwnAddress(
        bus.open(DatagramAddress { .host = std::string { host }, .port = TestBeaconPort }),
        bus.open(DatagramAddress { .host = std::string { host }, .port = ownPort }));
    REQUIRE(paired.has_value());
    return std::move(*paired);
}

} // namespace

TEST_CASE("A shared-port socket sends from the address only it holds", "[net][datagram][sharedport]")
{
    // The whole rule, in one assertion. What a peer replies to is the sender address, so a datagram
    // that went out of the shared socket would have it answered at a port a co-hosted node may hold
    // too.
    DatagramBus bus;
    auto node = coHostedDatagramSocket(bus, "10.0.0.1", 40001);
    auto peer = bus.open(DatagramAddress { .host = "10.0.0.2", .port = TestBeaconPort });

    REQUIRE(node->send(datagramBytes("beacon"), peer->boundAddress()).has_value());

    auto const received = peer->receive(10ms);
    REQUIRE(received.has_value());
    CHECK(datagramText(*received) == "beacon");
    CHECK(received->from == DatagramAddress { .host = "10.0.0.1", .port = 40001 });
    CHECK(received->from == node->boundAddress());
}

TEST_CASE("A shared-port socket receives on both of its halves", "[net][datagram][sharedport]")
{
    // A beacon arrives on the shared half and an answer on the private one, and the caller above
    // sees one stream. Sent before either is read, so the case does not depend on which half is
    // polled first — that alternates.
    DatagramBus bus;
    auto node = coHostedDatagramSocket(bus, "10.0.0.1", 40001);
    auto peer = bus.open(DatagramAddress { .host = "10.0.0.2", .port = TestBeaconPort });

    REQUIRE(peer->send(datagramBytes("beacon"), DatagramBus::broadcastAddressOn(TestBeaconPort)).has_value());
    REQUIRE(peer->send(datagramBytes("proof"), node->boundAddress()).has_value());

    std::vector<std::string> heard;
    for ([[maybe_unused]] auto const attempt: std::views::iota(0, 2))
    {
        auto const received = node->receive(10ms);
        REQUIRE(received.has_value());
        heard.push_back(datagramText(*received));
    }

    std::ranges::sort(heard);
    CHECK(heard == std::vector<std::string> { "beacon", "proof" });
}

TEST_CASE("A broadcast reaches the shared half and not the private one", "[net][datagram][sharedport]")
{
    // Only once. A node whose private socket also heard the beacon port would see every beacon
    // twice, challenge every peer twice per beacon, and reject the first proof back as an answer to
    // a nonce it had already replaced.
    DatagramBus bus;
    auto node = coHostedDatagramSocket(bus, "10.0.0.1", 40001);
    auto peer = bus.open(DatagramAddress { .host = "10.0.0.2", .port = TestBeaconPort });

    REQUIRE(peer->send(datagramBytes("beacon"), DatagramBus::broadcastAddressOn(TestBeaconPort)).has_value());

    auto const once = node->receive(10ms);
    REQUIRE(once.has_value());
    CHECK(datagramText(*once) == "beacon");
    CHECK_FALSE(node->receive(10ms).has_value());
}

TEST_CASE("Two co-hosted shared-port sockets each get their own answers", "[net][datagram][sharedport]")
{
    // The configuration the whole class exists for: one machine, two nodes, one beacon port. Both
    // hear the broadcast, and each is answered at an address the other does not hold — which is
    // what a single shared socket could not give them, because a unicast to the port they share
    // reaches only one.
    DatagramBus bus;
    auto first = coHostedDatagramSocket(bus, "10.0.0.1", 40001);
    auto second = coHostedDatagramSocket(bus, "10.0.0.1", 40002);
    auto peer = bus.open(DatagramAddress { .host = "10.0.0.2", .port = TestBeaconPort });

    REQUIRE(peer->send(datagramBytes("beacon"), DatagramBus::broadcastAddressOn(TestBeaconPort)).has_value());

    for (auto* const node: { first.get(), second.get() })
    {
        auto const beacon = node->receive(10ms);
        REQUIRE(beacon.has_value());
        CHECK(datagramText(*beacon) == "beacon");
    }

    REQUIRE(peer->send(datagramBytes("for first"), first->boundAddress()).has_value());
    REQUIRE(peer->send(datagramBytes("for second"), second->boundAddress()).has_value());

    auto const atFirst = first->receive(10ms);
    REQUIRE(atFirst.has_value());
    CHECK(datagramText(*atFirst) == "for first");

    auto const atSecond = second->receive(10ms);
    REQUIRE(atSecond.has_value());
    CHECK(datagramText(*atSecond) == "for second");
}

TEST_CASE("Closing a shared-port socket stops its receive loop", "[net][datagram][sharedport]")
{
    // The property every `receive(timeout)` in this tree exists for: nothing but a poll return can
    // tell a loop to stop. It has to hold whichever half is polled first, so it is asserted twice —
    // and the order alternates, so two calls cover both.
    DatagramBus bus;
    auto node = coHostedDatagramSocket(bus, "10.0.0.1", 40001);

    node->close();

    for ([[maybe_unused]] auto const attempt: std::views::iota(0, 2))
    {
        auto const result = node->receive(10ms);
        REQUIRE_FALSE(result.has_value());
        CHECK(result.error() == DatagramWait::Closed);
    }
}

TEST_CASE("An idle shared-port socket times out rather than blocking", "[net][datagram][sharedport]")
{
    DatagramBus bus;
    auto node = coHostedDatagramSocket(bus, "10.0.0.1", 40001);

    auto const result = node->receive(10ms);
    REQUIRE_FALSE(result.has_value());
    CHECK(result.error() == DatagramWait::TimedOut);
}

TEST_CASE("A half that could not be bound yields no pair at all", "[net][datagram][sharedport]")
{
    // A pair built around a half that is not there would fail on the first datagram, on the
    // discovery thread, as a null dereference rather than as the startup refusal the caller already
    // knows how to report.
    DatagramBus bus;
    auto const at = [&bus](std::uint16_t port) {
        return bus.open(DatagramAddress { .host = "10.0.0.1", .port = port });
    };

    // A `std::array` rather than a braced list, because the elements are move-only: a braced list is
    // a `std::initializer_list`, whose elements are const, so iterating one would copy a
    // `unique_ptr` — and the three cases are exactly "one half is null", which has to be passed by
    // move to mean anything.
    auto halves = std::array {
        std::pair { at(TestBeaconPort), std::unique_ptr<IDatagramSocket> {} },
        std::pair { std::unique_ptr<IDatagramSocket> {}, at(40001) },
        std::pair { std::unique_ptr<IDatagramSocket> {}, std::unique_ptr<IDatagramSocket> {} },
    };

    for (auto& [shared, own]: halves)
    {
        auto paired = answerFromOwnAddress(std::move(shared), std::move(own));
        REQUIRE_FALSE(paired.has_value());
        CHECK(paired.error().code == NetErrorCode::BadHandle);
    }
}

TEST_CASE("A shared-port socket drains a backlog as fast as it is asked", "[net][datagram][sharedport]")
{
    // Throughput, not ordering, and the two are different properties. Halving the caller's timeout
    // across the two halves — the obvious implementation — means that whenever the half polled
    // first is idle, each datagram queued on the other costs a full half-timeout: eight a second at
    // a 250ms poll, which a large enough segment exceeds permanently, and what the kernel drops
    // then includes the proofs.
    //
    // Asserted as elapsed time rather than by reading the implementation, and against a generous
    // ceiling, because what is being ruled out is seconds. A large caller timeout, so the two
    // implementations are orders of magnitude apart rather than a factor of two: half-the-timeout
    // would spend a full SECOND on the idle half every other call, giving 8s against 0.13s, and the
    // ceiling sits between them with room for a loaded runner on both sides.
    DatagramBus bus;
    auto node = coHostedDatagramSocket(bus, "10.0.0.1", 40001);
    auto peer = bus.open(DatagramAddress { .host = "10.0.0.2", .port = TestBeaconPort });

    constexpr auto Backlog = 16;
    for ([[maybe_unused]] auto const sent: std::views::iota(0, Backlog))
        REQUIRE(peer->send(datagramBytes("proof"), node->boundAddress()).has_value());

    auto const startedAt = std::chrono::steady_clock::now();
    for ([[maybe_unused]] auto const drained: std::views::iota(0, Backlog))
    {
        auto const received = node->receive(2s);
        REQUIRE(received.has_value());
        CHECK(datagramText(*received) == "proof");
    }
    auto const elapsed = std::chrono::steady_clock::now() - startedAt;

    CHECK(elapsed < 1s);
}

TEST_CASE("A shared-port socket polls each half however small the timeout", "[net][datagram][sharedport]")
{
    // Halving a caller's timeout must not round it to nothing. Zero is not "do not wait" to a real
    // socket — `SO_RCVTIMEO` of zero means block forever — so the half is floored at a millisecond,
    // and both halves are still read.
    DatagramBus bus;
    auto node = coHostedDatagramSocket(bus, "10.0.0.1", 40001);
    auto peer = bus.open(DatagramAddress { .host = "10.0.0.2", .port = TestBeaconPort });

    REQUIRE(peer->send(datagramBytes("beacon"), DatagramBus::broadcastAddressOn(TestBeaconPort)).has_value());
    REQUIRE(peer->send(datagramBytes("proof"), node->boundAddress()).has_value());

    std::vector<std::string> heard;
    for ([[maybe_unused]] auto const attempt: std::views::iota(0, 2))
    {
        auto const received = node->receive(0ms);
        REQUIRE(received.has_value());
        heard.push_back(datagramText(*received));
    }

    std::ranges::sort(heard);
    CHECK(heard == std::vector<std::string> { "beacon", "proof" });
}

TEST_CASE("Two real nodes open a pair on one shared port", "[net][datagram][sharedport][udp]")
{
    // The real stack, because this is where the four options actually land and every wrong pairing
    // of them still starts. Transposing the two ports gives a node answering where the segment
    // shouts; making the listener exclusive gives one that locks every other node on the machine
    // out of hearing beacons. Both show up here and in no in-memory case.
    //
    // Loopback and a kernel-chosen shared port, so it needs no fixture and cannot collide with
    // anything else on the machine — this suite runs in parallel.
    auto probe = openUdpSocket("127.0.0.1", 0, BroadcastMode::Off, PortSharing::Shared);
    REQUIRE(probe.has_value());
    auto const sharedPort = (*probe)->boundAddress().port;
    REQUIRE(sharedPort != 0);
    probe->reset();

    auto first = openSharedPortUdpSocket("127.0.0.1", sharedPort, 0);
    REQUIRE(first.has_value());

    // The second is the assertion: it binds the SAME shared port, which only a shared listener
    // permits, and gets an answering address of its own.
    auto second = openSharedPortUdpSocket("127.0.0.1", sharedPort, 0);
    REQUIRE(second.has_value());

    // What each reports is where it is ANSWERED, which is never the port the segment broadcasts to
    // and never the other node's.
    CHECK((*first)->boundAddress().port != sharedPort);
    CHECK((*second)->boundAddress().port != sharedPort);
    CHECK((*first)->boundAddress().port != (*second)->boundAddress().port);

    // And they are answerable apart — the whole point, over real sockets.
    REQUIRE((*first)->send(datagramBytes("for second"), (*second)->boundAddress()).has_value());

    auto const atSecond = (*second)->receive(2s);
    REQUIRE(atSecond.has_value());
    CHECK(datagramText(*atSecond) == "for second");
    CHECK(atSecond->from == (*first)->boundAddress());

    CHECK_FALSE((*first)->receive(200ms).has_value());
}

TEST_CASE("A shared-port pair can be asked for a named answering port", "[net][datagram][sharedport][udp]")
{
    // What a `--discovery-reply-port` option is for: a host firewall that opens named ports only
    // passes the beacons and drops every challenge and proof, which presents as peers seen and
    // never admitted.
    //
    // Both numbers are kernel-chosen and then re-bound, which is the only way to name a port in a
    // suite that runs in parallel. The shared probe is HELD while the second is drawn, so the two
    // cannot come back equal — pointing both halves at one port is exactly the configuration under
    // test elsewhere, and it would present here as a bind failure nobody could explain.
    auto probe = openUdpSocket("127.0.0.1", 0, BroadcastMode::Off, PortSharing::Shared);
    REQUIRE(probe.has_value());
    auto const sharedPort = (*probe)->boundAddress().port;
    REQUIRE(sharedPort != 0);

    auto namer = openUdpSocket("127.0.0.1", 0, BroadcastMode::Off, PortSharing::Exclusive);
    REQUIRE(namer.has_value());
    auto const ownPort = (*namer)->boundAddress().port;
    REQUIRE(ownPort != 0);
    REQUIRE(ownPort != sharedPort);
    namer->reset();

    auto node = openSharedPortUdpSocket("127.0.0.1", sharedPort, ownPort);
    REQUIRE(node.has_value());
    CHECK((*node)->boundAddress().port == ownPort);

    // And a second node cannot have that one, which is why it is a port per node rather than a port
    // per fleet. Asked for while the first still holds it, so this is the refusal itself and not a
    // race with whoever had it last.
    auto const refused = openSharedPortUdpSocket("127.0.0.1", sharedPort, ownPort);
    REQUIRE_FALSE(refused.has_value());
    CHECK(refused.error().code == NetErrorCode::AddressInUse);
}
