// SPDX-License-Identifier: Apache-2.0
#include <core/net/testing/DatagramPayload.hpp>
#include <core/net/testing/InMemoryDatagram.hpp>

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <string>
#include <string_view>

using namespace std::chrono_literals;
using core::net::DatagramAddress;
using core::net::DatagramWait;
using core::net::testing::DatagramBus;
using core::net::testing::datagramBytes;
using core::net::testing::datagramText;

namespace
{

/// A node's address on the test segment.
///
/// Every node here answers on one port, because what these cases are about is which inbox a
/// datagram reaches — so naming the port at each of two dozen call sites would be noise around the
/// half that matters.
/// @param host The node's address.
/// @return Its bus address.
[[nodiscard]] DatagramAddress atHost(std::string_view host)
{
    return DatagramAddress { .host = std::string { host }, .port = 7000 };
}

} // namespace

TEST_CASE("DatagramBus delivers a unicast to exactly one inbox", "[net][datagram]")
{
    DatagramBus bus;
    auto alice = bus.open(atHost("10.0.0.1"));
    auto bob = bus.open(atHost("10.0.0.2"));
    auto carol = bus.open(atHost("10.0.0.3"));

    REQUIRE(alice->send(datagramBytes("hello"), atHost("10.0.0.2")).has_value());

    auto const received = bob->receive(10ms);
    REQUIRE(received.has_value());
    CHECK(datagramText(*received) == "hello");
    CHECK(received->from == atHost("10.0.0.1"));

    // Nobody else sees it.
    CHECK_FALSE(carol->receive(10ms).has_value());
}

TEST_CASE("Two sockets sharing one address both hear a broadcast; one hears a unicast", "[net][datagram]")
{
    // The asymmetry the whole shared-port defect rests on, and the reason this double models a
    // shared address at all. A UDP port is shareable — every node on a segment binds the beacon
    // port, co-hosted nodes on one machine included — and sharing it buys HEARING the broadcast
    // and nothing else.
    DatagramBus bus;
    auto first = bus.open(atHost("10.0.0.1"));
    auto second = bus.open(atHost("10.0.0.1"));
    auto sender = bus.open(atHost("10.0.0.9"));

    REQUIRE(sender->send(datagramBytes("beacon"), DatagramBus::broadcastAddress()).has_value());

    for (auto* socket: { first.get(), second.get() })
    {
        auto const heard = socket->receive(10ms);
        REQUIRE(heard.has_value());
        CHECK(datagramText(*heard) == "beacon");
    }

    REQUIRE(sender->send(datagramBytes("challenge"), atHost("10.0.0.1")).has_value());

    // The first to have attached, deterministically: a real kernel's answer differs between
    // platforms — Windows 11 hands it to the first-bound socket and Linux to the last — so what is
    // asserted is that exactly ONE of them gets it, which is what every platform agrees on and
    // what the layer above has to survive.
    auto const atFirst = first->receive(10ms);
    REQUIRE(atFirst.has_value());
    CHECK(datagramText(*atFirst) == "challenge");
    CHECK_FALSE(second->receive(10ms).has_value());
}

TEST_CASE("A broadcast to a port passes over a socket on another one", "[net][datagram]")
{
    // What `255.255.255.255:P` does. It matters as soon as one node holds sockets on two ports,
    // which is the shape `answerFromOwnAddress` gives it: a beacon must reach the socket listening
    // for beacons and not the one waiting for the answer to one.
    DatagramBus bus;
    auto listener = bus.open(DatagramAddress { .host = "10.0.0.1", .port = 6681 });
    auto own = bus.open(DatagramAddress { .host = "10.0.0.1", .port = 40001 });

    REQUIRE(own->send(datagramBytes("beacon"), DatagramBus::broadcastAddressOn(6681)).has_value());

    auto const atListener = listener->receive(10ms);
    REQUIRE(atListener.has_value());
    CHECK(datagramText(*atListener) == "beacon");
    CHECK_FALSE(own->receive(10ms).has_value());
}

TEST_CASE("Closing one of two sockets on one address leaves the other serving", "[net][datagram]")
{
    // Sharing an address is not sharing a socket. A double that gave one address one inbox would
    // report the survivor closed — and, worse, would have handed both nodes the same queue all
    // along.
    DatagramBus bus;
    auto first = bus.open(atHost("10.0.0.1"));
    auto second = bus.open(atHost("10.0.0.1"));
    auto sender = bus.open(atHost("10.0.0.9"));

    first->close();

    auto const closed = first->receive(10ms);
    REQUIRE_FALSE(closed.has_value());
    CHECK(closed.error() == DatagramWait::Closed);

    REQUIRE(sender->send(datagramBytes("beacon"), DatagramBus::broadcastAddress()).has_value());

    auto const heard = second->receive(10ms);
    REQUIRE(heard.has_value());
    CHECK(datagramText(*heard) == "beacon");
}

TEST_CASE("DatagramBus delivers a broadcast to everyone, sender included", "[net][datagram]")
{
    // Sender included, because that is what a real broadcast does — and it is exactly the case a
    // peer directory has to ignore. A double that quietly spared the sender would hide the bug
    // where a lone node records its own beacon and proposes a membership change to admit itself.
    DatagramBus bus;
    auto alice = bus.open(atHost("10.0.0.1"));
    auto bob = bus.open(atHost("10.0.0.2"));

    REQUIRE(alice->send(datagramBytes("beacon"), DatagramBus::broadcastAddress()).has_value());

    auto const atBob = bob->receive(10ms);
    REQUIRE(atBob.has_value());
    CHECK(datagramText(*atBob) == "beacon");

    auto const atAlice = alice->receive(10ms);
    REQUIRE(atAlice.has_value());
    CHECK(datagramText(*atAlice) == "beacon");
    CHECK(atAlice->from == atHost("10.0.0.1"));
}

TEST_CASE("DatagramBus loses what it is told to lose", "[net][datagram]")
{
    // Scripted rather than random: a test that dropped datagrams by chance would fail occasionally
    // for reasons nobody could reproduce, and what discovery has to survive is a specific peer
    // going quiet, not a global rate.
    DatagramBus bus;
    auto alice = bus.open(atHost("10.0.0.1"));
    auto bob = bus.open(atHost("10.0.0.2"));
    auto carol = bus.open(atHost("10.0.0.3"));

    // Asserted, not discarded: starving an address nobody holds is a no-op, and the case below
    // would then be green because the datagram never arrived rather than because it was dropped.
    REQUIRE(bus.dropNext(atHost("10.0.0.2"), 2) == 1);

    for (auto const* const text: { "one", "two", "three" })
        REQUIRE(alice->send(datagramBytes(text), DatagramBus::broadcastAddress()).has_value());

    // Bob lost the first two; the third gets through.
    auto const atBob = bob->receive(10ms);
    REQUIRE(atBob.has_value());
    CHECK(datagramText(*atBob) == "three");

    // Carol was never starved, so she has all three — a drop is per destination, which is the
    // partition shape a global drop rate cannot express.
    for (auto const* const expected: { "one", "two", "three" })
    {
        auto const next = carol->receive(10ms);
        REQUIRE(next.has_value());
        CHECK(datagramText(*next) == expected);
    }
}

TEST_CASE("A datagram to nobody is discarded, not reported", "[net][datagram]")
{
    // What UDP does. Reporting it would hand the layer above a delivery signal the real network
    // cannot provide, and discovery would come to rely on it.
    DatagramBus bus;
    auto alice = bus.open(atHost("10.0.0.1"));

    CHECK(alice->send(datagramBytes("into the void"), atHost("10.0.0.9")).has_value());
    CHECK(bus.sendCount() == 1);
}

TEST_CASE("A closed datagram socket stops its receive loop", "[net][datagram]")
{
    // The property the whole `receive(timeout)` shape exists for: POSIX does not unblock a parked
    // receive when another thread closes the socket, so a loop that could not observe a shutdown
    // would hang a `systemctl stop`.
    DatagramBus bus;
    auto alice = bus.open(atHost("10.0.0.1"));

    alice->close();

    auto const result = alice->receive(10ms);
    REQUIRE_FALSE(result.has_value());
    CHECK(result.error() == DatagramWait::Closed);
}

TEST_CASE("An idle datagram socket times out rather than blocking", "[net][datagram]")
{
    DatagramBus bus;
    auto alice = bus.open(atHost("10.0.0.1"));

    auto const result = alice->receive(5ms);
    REQUIRE_FALSE(result.has_value());
    CHECK(result.error() == DatagramWait::TimedOut);
}
