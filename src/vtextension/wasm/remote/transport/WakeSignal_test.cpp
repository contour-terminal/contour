// SPDX-License-Identifier: Apache-2.0
//
// The wake signal Contour sends its extension host process when it raised an interrupt flag: it wakes a
// waiting receiver, never blocks the sender, ends a wait on stop() or when the sender is gone, and never ends
// the sender's process when the receiver is gone.
#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <cstddef>
#include <future>
#include <ranges>
#include <thread>
#include <tuple>
#include <utility>

#include <vtextension/wasm/remote/transport/WakeSignal.hpp>

#ifndef _WIN32
    #include <csignal>
#endif

using vtextension::wasm::remote::transport::makeWakePair;
using vtextension::wasm::remote::transport::WakeResult;
using vtextension::wasm::remote::transport::WakeWait;

TEST_CASE("WakeSignal.signalWakesTheReceiver", "[vtextension]")
{
    auto pair = makeWakePair();
    REQUIRE(pair.has_value());
    auto result = std::promise<WakeWait> {};
    auto woken = result.get_future();
    auto waiter = std::jthread { [&] { result.set_value(pair->helper.wait()); } };
    std::this_thread::sleep_for(std::chrono::milliseconds { 20 });
    CHECK(pair->parent.signal() == WakeResult::Sent);
    CHECK(woken.get() == WakeWait::Woken);
}

TEST_CASE("WakeSignal.signalNeverBlocks", "[vtextension]")
{
    auto pair = makeWakePair();
    REQUIRE(pair.has_value());

    constexpr auto Signals = 100'000;
    struct Counts
    {
        WakeResult first = WakeResult::Pending;
        std::size_t pending = 0;
    };
    // Nobody waits: a sender that blocked on a full buffer would never return.
    auto signalled = std::async(std::launch::async, [&] {
        auto counts = Counts { .first = pair->parent.signal() };
        for ([[maybe_unused]] auto const i: std::views::iota(1, Signals))
            if (pair->parent.signal() == WakeResult::Pending)
                ++counts.pending;
        return counts;
    });
    REQUIRE(signalled.wait_for(std::chrono::seconds { 5 }) == std::future_status::ready);
    auto const counts = signalled.get();
    CHECK(counts.first == WakeResult::Sent);
    CHECK(counts.pending > 0);

    // One wait takes every queued wake; the next one finds none and waits until stopped.
    CHECK(pair->helper.wait() == WakeWait::Woken);
    auto next = std::async(std::launch::async, [&] { return pair->helper.wait(); });
    CHECK(next.wait_for(std::chrono::milliseconds { 50 }) == std::future_status::timeout);
    pair->helper.stop();
    CHECK(next.get() == WakeWait::Stopped);
}

TEST_CASE("WakeSignal.stopEndsAWait", "[vtextension]")
{
    auto pair = makeWakePair();
    REQUIRE(pair.has_value());
    auto result = std::promise<WakeWait> {};
    auto stopped = result.get_future();
    auto waiter = std::jthread { [&] { result.set_value(pair->helper.wait()); } };
    std::this_thread::sleep_for(std::chrono::milliseconds { 20 });
    pair->helper.stop();
    CHECK(stopped.get() == WakeWait::Stopped);
    // Stopped for good, even with a wake queued.
    CHECK(pair->parent.signal() == WakeResult::Sent);
    CHECK(pair->helper.wait() == WakeWait::Stopped);
}

#ifndef _WIN32
TEST_CASE("WakeSignal.senderGoneIsPeerGone", "[vtextension]")
{
    auto pair = makeWakePair();
    REQUIRE(pair.has_value());
    {
        auto const sender = std::move(pair->parent);
    }
    CHECK(pair->helper.wait() == WakeWait::PeerGone);
}

TEST_CASE("WakeSignal.signalToAGoneReceiverKeepsSigpipeAway", "[vtextension]")
{
    struct sigaction before {};
    struct sigaction byDefault {};
    byDefault.sa_handler = SIG_DFL;
    REQUIRE(::sigaction(SIGPIPE, &byDefault, &before) == 0);

    auto pair = makeWakePair();
    REQUIRE(pair.has_value());
    {
        auto const receiver = std::move(pair->helper);
    }
    // At SIG_DFL, a SIGPIPE would end this process here.
    std::ignore = pair->parent.signal();
    std::ignore = pair->parent.signal();
    SUCCEED("the process lives");

    REQUIRE(::sigaction(SIGPIPE, &before, nullptr) == 0);
}
#endif
