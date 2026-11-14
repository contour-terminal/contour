// SPDX-License-Identifier: Apache-2.0
#include <contour/platform/ApplicationReadiness.hpp>
#include <contour/platform/AsyncLatch.hpp>

#include <core/async/StopToken.hpp>
#include <core/async/SyncRun.hpp>
#include <core/async/Task.hpp>
#include <core/async/WhenAll.hpp>
#include <core/async/WhenAny.hpp>
#include <core/net/EventLoop.hpp>
#include <core/net/InterruptibleSleep.hpp>
#include <core/net/IoBackend.hpp>
#include <core/net/WithTimeout.hpp>

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <optional>
#include <thread>
#include <tuple>
#include <vector>

using namespace std::chrono_literals;
using contour::platform::ApplicationReadiness;
using contour::platform::AsyncLatch;
using contour::platform::AsyncLatchTestAccess;
using State = ApplicationReadiness::State;

namespace
{

/// Waits for @p latch and keeps what it was set to in @p value.
core::async::Task<void> waitInto(AsyncLatch<int>* latch, std::optional<int>* value)
{
    *value = co_await latch->wait();
}

/// Sleeps until @p token is stopped: the link's stop(), as ClientSupervisor races its connect against it.
core::async::Task<void> untilStopped(core::net::EventLoop* loop, core::async::StopToken token)
{
    std::ignore = co_await core::net::interruptibleSleepUntil(loop, token, loop->clock().now() + 1h);
}

/// Spins, without sleeping a fixed time, until @p latch has @p count parked waiters.
void awaitWaiters(AsyncLatch<int> const& latch, std::size_t count)
{
    while (AsyncLatchTestAccess::waiting(latch) != count)
        std::this_thread::yield();
}

} // namespace

TEST_CASE("an application readiness moves forward only", "[platform][readiness]")
{
    SECTION("a ready after an abandon is ignored")
    {
        auto readiness = ApplicationReadiness { State::Pending };
        readiness.abandon();
        readiness.markReady();
        CHECK(readiness.state() == State::Abandoned);
    }
    SECTION("an abandon after a ready does nothing")
    {
        auto readiness = ApplicationReadiness { State::Pending };
        readiness.markReady();
        readiness.abandon();
        CHECK(readiness.state() == State::Ready);
    }
    SECTION("a ready object closes, and stays closed")
    {
        auto readiness = ApplicationReadiness { State::Ready };
        readiness.close();
        CHECK(readiness.state() == State::Closed);
        readiness.markReady();
        readiness.abandon();
        CHECK(readiness.state() == State::Closed);
    }
    SECTION("an object never made does not close")
    {
        auto readiness = ApplicationReadiness { State::Pending };
        readiness.close();
        CHECK(readiness.state() == State::Pending);
    }
}

TEST_CASE("an application readiness tells its observers", "[platform][readiness]")
{
    auto readiness = ApplicationReadiness { State::Pending };
    auto first = std::vector<State> {};
    auto second = std::vector<State> {};
    {
        auto const one = readiness.subscribe([&](State state) { first.push_back(state); });
        auto const two = readiness.subscribe([&](State state) { second.push_back(state); });
        readiness.markReady();
        readiness.markReady(); // no change: nobody is told
        readiness.close();
    }
    CHECK(first == std::vector { State::Pending, State::Ready, State::Closed });
    CHECK(second == std::vector { State::Pending, State::Ready, State::Closed });
}

TEST_CASE("an application readiness tells nothing after the subscription ends", "[platform][readiness]")
{
    auto readiness = ApplicationReadiness { State::Pending };
    auto told = std::vector<State> {};
    {
        auto const subscription = readiness.subscribe([&](State state) { told.push_back(state); });
    }
    readiness.markReady();
    CHECK(told == std::vector { State::Pending });
}

TEST_CASE("an async latch is set once", "[platform][latch]")
{
    auto latch = AsyncLatch<int> {};
    CHECK_FALSE(latch.value().has_value());
    CHECK(latch.set(1));
    CHECK_FALSE(latch.set(2));
    CHECK(latch.value() == 1);
    // Set already: no loop, no executor, and nothing to park on.
    CHECK(core::async::syncRun(latch.wait()) == 1);
    CHECK(AsyncLatchTestAccess::waiting(latch) == 0);
}

TEST_CASE("an async latch wakes every waiter", "[platform][latch]")
{
    auto latch = AsyncLatch<int> {};
    auto first = std::optional<int> {};
    auto second = std::optional<int> {};
    auto waiting = std::thread { [&] {
        auto const backend = core::net::makeDefaultBackend();
        auto loop = core::net::EventLoop { *backend };
        loop.blockOn([](AsyncLatch<int>* latch,
                        std::optional<int>* first,
                        std::optional<int>* second) -> core::async::Task<void> {
            co_await core::async::whenAll(waitInto(latch, first), waitInto(latch, second));
        }(&latch, &first, &second));
    } };
    awaitWaiters(latch, 2);
    CHECK(latch.set(7));
    waiting.join();
    CHECK(first == 7);
    CHECK(second == 7);
    CHECK(AsyncLatchTestAccess::waiting(latch) == 0);
}

TEST_CASE("a stop ends a wait on an async latch", "[platform][latch]")
{
    auto latch = AsyncLatch<int> {};
    auto value = std::optional<int> {};
    auto stop = core::async::StopSource {};
    auto waiting = std::thread { [&] {
        auto const backend = core::net::makeDefaultBackend();
        auto loop = core::net::EventLoop { *backend };
        loop.blockOn([](core::net::EventLoop* loop,
                        AsyncLatch<int>* latch,
                        std::optional<int>* value,
                        core::async::StopToken token) -> core::async::Task<void> {
            std::ignore = co_await core::async::whenAny(waitInto(latch, value), untilStopped(loop, token));
        }(&loop, &latch, &value, stop.get_token()));
    } };
    awaitWaiters(latch, 1);
    stop.request_stop();
    waiting.join();
    // Unparked without a value, and no longer counted: a later set reaches nobody.
    CHECK_FALSE(value.has_value());
    CHECK(AsyncLatchTestAccess::waiting(latch) == 0);
    CHECK(latch.set(1));
}

TEST_CASE("a deadline ends a wait on an async latch", "[platform][latch]")
{
    auto latch = AsyncLatch<int> {};
    auto const backend = core::net::makeDefaultBackend();
    auto loop = core::net::EventLoop { *backend };
    auto const value = loop.blockOn(core::net::withTimeout(&loop, latch.wait(), 10ms));
    CHECK_FALSE(value.has_value());
    CHECK(AsyncLatchTestAccess::waiting(latch) == 0);
}
