// SPDX-License-Identifier: Apache-2.0
#include <contour/mux/MuxController.h>

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <condition_variable>
#include <mutex>
#include <string>
#include <thread>

using namespace std::chrono_literals;
using contour::awaitMuxConnect;
using contour::makeUnboundFallbackPty;
using contour::MuxConnectPhase;

TEST_CASE("awaitMuxConnect reports Ready without waiting once the phase is set", "[mux][controller]")
{
    auto mutex = std::mutex {};
    auto cv = std::condition_variable {};
    auto phase = MuxConnectPhase::Ready; // already transitioned
    auto const failure = std::string {};

    auto const outcome = awaitMuxConnect(mutex, cv, phase, failure, 5s);

    CHECK(outcome.ready);
    CHECK_FALSE(outcome.timedOut);
}

TEST_CASE("awaitMuxConnect surfaces the recorded failure reason", "[mux][controller]")
{
    auto mutex = std::mutex {};
    auto cv = std::condition_variable {};
    auto phase = MuxConnectPhase::Failed;
    auto const failure = std::string { "daemon refused" };

    auto const outcome = awaitMuxConnect(mutex, cv, phase, failure, 5s);

    CHECK_FALSE(outcome.ready);
    CHECK_FALSE(outcome.timedOut);
    CHECK(outcome.failure == "daemon refused");
}

TEST_CASE("awaitMuxConnect times out while still Connecting", "[mux][controller]")
{
    auto mutex = std::mutex {};
    auto cv = std::condition_variable {};
    auto phase = MuxConnectPhase::Connecting; // never transitions
    auto const failure = std::string {};

    auto const outcome = awaitMuxConnect(mutex, cv, phase, failure, 20ms);

    CHECK(outcome.timedOut);
    CHECK_FALSE(outcome.ready);
}

TEST_CASE("awaitMuxConnect wakes on a cross-thread transition", "[mux][controller]")
{
    auto mutex = std::mutex {};
    auto cv = std::condition_variable {};
    auto phase = MuxConnectPhase::Connecting;
    auto const failure = std::string {};

    // The reactor thread: transition to Ready and notify, as a controller does.
    auto reactor = std::thread { [&] {
        std::this_thread::sleep_for(5ms);
        {
            auto const lock = std::lock_guard { mutex };
            phase = MuxConnectPhase::Ready;
        }
        cv.notify_all();
    } };

    auto const outcome = awaitMuxConnect(mutex, cv, phase, failure, 5s);
    reactor.join();

    CHECK(outcome.ready);
    CHECK_FALSE(outcome.timedOut);
}

TEST_CASE("makeUnboundFallbackPty honors the requested size, else defaults", "[mux][controller]")
{
    auto const sized =
        makeUnboundFallbackPty(vtbackend::PageSize { vtbackend::LineCount(10), vtbackend::ColumnCount(40) });
    REQUIRE(sized != nullptr);
    CHECK(sized->pageSize() == vtpty::PageSize { vtpty::LineCount(10), vtpty::ColumnCount(40) });

    auto const defaulted = makeUnboundFallbackPty(std::nullopt);
    REQUIRE(defaulted != nullptr);
    CHECK(defaulted->pageSize() == vtpty::PageSize { vtpty::LineCount(25), vtpty::ColumnCount(80) });
}
