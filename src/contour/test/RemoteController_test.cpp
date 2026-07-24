// SPDX-License-Identifier: Apache-2.0
#include <contour/remote/ReactorThread.h>
#include <contour/remote/RemoteController.h>

#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <string>
#include <thread>

#include <coro/Task.hpp>
#include <net/EventLoop.h>

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

TEST_CASE("SelfUnbindingChannelPty runs its on-destroy callback exactly once", "[mux][controller]")
{
    auto unbinds = 0;
    {
        auto pty = contour::SelfUnbindingChannelPty { vtpty::PageSize { vtpty::LineCount(12),
                                                                        vtpty::ColumnCount(40) },
                                                      {}, // write sink — unused by this test
                                                      {}, // resize sink — unused by this test
                                                      [&unbinds] { ++unbinds; } };
        CHECK(pty.pageSize() == vtpty::PageSize { vtpty::LineCount(12), vtpty::ColumnCount(40) });
        CHECK(unbinds == 0); // not until the terminal destroys it
    }
    CHECK(unbinds == 1); // exactly once, from the destructor
}

TEST_CASE("stopMuxReactor tears the reactor down once and ignores a second stop", "[mux][controller]")
{
    auto mutex = std::mutex {};
    auto stopped = false;
    auto reactor = contour::ReactorThread {};

    // A root task that parks forever, so only requestStop can end it.
    reactor.start([](net::EventLoop* loop) -> coro::Task<void> {
        while (true)
            co_await loop->delay(1h);
    });

    auto detaches = std::atomic<int> { 0 };
    auto const detach = [&detaches] {
        detaches.fetch_add(1, std::memory_order_relaxed);
    };

    // First stop: performs the teardown (posts the detach, cancels, joins).
    CHECK(contour::stopMuxReactor(mutex, stopped, reactor, detach));
    CHECK(stopped);
    CHECK(detaches.load(std::memory_order_relaxed) == 1);
    CHECK(reactor.wasCancelled());

    // Second stop: already stopped, so it neither re-posts nor re-joins.
    CHECK_FALSE(contour::stopMuxReactor(mutex, stopped, reactor, detach));
    CHECK(detaches.load(std::memory_order_relaxed) == 1);
}
