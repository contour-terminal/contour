// SPDX-License-Identifier: Apache-2.0
#include <core/net/EventLoop.hpp>
#include <core/net/IoBackend.hpp>
#include <core/net/WithTimeout.hpp>
#include <core/net/WriteQueue.hpp>
#include <core/net/testing/InMemoryTransport.hpp>
#include <core/net/testing/ParkingReadableSocket.hpp>

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <cstddef>
#include <string>

#include <vthost/GracefulClose.hpp>

using namespace std::chrono_literals;

TEST_CASE("closeGracefully gives up on a peer that never reads", "[vthost]")
{
    // A peer that stops reading leaves the flush -- and over TLS the close_notify behind it --
    // parked for as long as it likes. The close must still happen, at the deadline. The stall is
    // the socket's own (every write parks), not a payload too large for the kernel: how much a
    // kernel buffers differs by platform, and Windows takes tens of megabytes at once.
    auto const backend = core::net::makeDefaultBackend();
    auto loop = core::net::EventLoop { *backend };
    auto pair = core::net::testing::makeSocketPair(loop);
    REQUIRE(pair.has_value());
    auto stalled = core::net::testing::ParkingWritableSocket { *pair->first };
    stalled.stopReading();

    auto writer = core::net::WriteQueue { loop, &stalled, std::size_t { 1024 } * 1024 };
    REQUIRE(writer.enqueue(std::string(4096, 'x')));

    // Bounded from outside as well, so a regression -- a deadline ignored -- fails here instead of
    // hanging the suite.
    constexpr auto Deadline = 200ms;
    auto const started = std::chrono::steady_clock::now();
    auto const finished = loop.blockOn(
        core::net::withTimeout(&loop, vthost::closeGracefully(&loop, &writer, &stalled, Deadline), 10s));
    auto const took = std::chrono::steady_clock::now() - started;

    REQUIRE(finished);
    CHECK(took >= Deadline); // the flush never completes, so only the deadline can have ended it
    CHECK(stalled.isClosed());
}
