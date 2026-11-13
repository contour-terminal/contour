// SPDX-License-Identifier: Apache-2.0
#include <contour/platform/ApplicationReadiness.hpp>

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

#include <atomic>
#include <chrono>
#include <optional>
#include <thread>
#include <tuple>

using namespace std::chrono_literals;
using contour::platform::ApplicationReadiness;
using State = ApplicationReadiness::State;

namespace
{

/// Waits for @p readiness and keeps what it settled on in @p settled.
core::async::Task<void> settleInto(ApplicationReadiness* readiness, std::optional<State>* settled)
{
    *settled = co_await readiness->whenSettled();
}

/// Sleeps until @p token is stopped: the link's stop(), as ClientSupervisor races its connect against it.
core::async::Task<void> untilStopped(core::net::EventLoop* loop, core::async::StopToken token)
{
    std::ignore = co_await core::net::interruptibleSleepUntil(loop, token, loop->clock().now() + 1h);
}

/// Spins, without sleeping a fixed time, until @p readiness has @p count parked waiters.
void awaitWaiters(ApplicationReadiness const& readiness, std::size_t count)
{
    while (readiness.waiting() != count)
        std::this_thread::yield();
}

} // namespace

TEST_CASE("an application readiness settles once", "[platform][readiness]")
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
    SECTION("made ready from the start, as where the application object exists already")
    {
        auto readiness = ApplicationReadiness { State::Ready };
        readiness.abandon();
        CHECK(readiness.state() == State::Ready);
    }
}

TEST_CASE("a wait on a settled application readiness returns at once", "[platform][readiness]")
{
    auto readiness = ApplicationReadiness { State::Pending };
    readiness.markReady();
    // No loop, no executor: nothing to park on, and nothing needed.
    CHECK(core::async::syncRun(readiness.whenSettled()) == State::Ready);
    CHECK(readiness.waiting() == 0);
}

TEST_CASE("an application readiness wakes every waiter", "[platform][readiness]")
{
    auto readiness = ApplicationReadiness { State::Pending };
    auto first = std::optional<State> {};
    auto second = std::optional<State> {};
    auto waiting = std::thread { [&] {
        auto const backend = core::net::makeDefaultBackend();
        auto loop = core::net::EventLoop { *backend };
        loop.blockOn([](ApplicationReadiness* readiness,
                        std::optional<State>* first,
                        std::optional<State>* second) -> core::async::Task<void> {
            co_await core::async::whenAll(settleInto(readiness, first), settleInto(readiness, second));
        }(&readiness, &first, &second));
    } };
    awaitWaiters(readiness, 2);
    readiness.markReady();
    waiting.join();
    CHECK(first == State::Ready);
    CHECK(second == State::Ready);
    CHECK(readiness.waiting() == 0);
}

TEST_CASE("an abandoned application readiness wakes its waiter", "[platform][readiness]")
{
    auto readiness = ApplicationReadiness { State::Pending };
    auto settled = std::optional<State> {};
    auto waiting = std::thread { [&] {
        auto const backend = core::net::makeDefaultBackend();
        auto loop = core::net::EventLoop { *backend };
        loop.blockOn(settleInto(&readiness, &settled));
    } };
    awaitWaiters(readiness, 1);
    readiness.abandon();
    waiting.join();
    CHECK(settled == State::Abandoned);
}

TEST_CASE("a stop ends a wait on the application readiness", "[platform][readiness]")
{
    auto readiness = ApplicationReadiness { State::Pending };
    auto settled = std::optional<State> {};
    auto stop = core::async::StopSource {};
    auto waiting = std::thread { [&] {
        auto const backend = core::net::makeDefaultBackend();
        auto loop = core::net::EventLoop { *backend };
        loop.blockOn([](core::net::EventLoop* loop,
                        ApplicationReadiness* readiness,
                        std::optional<State>* settled,
                        core::async::StopToken token) -> core::async::Task<void> {
            std::ignore =
                co_await core::async::whenAny(settleInto(readiness, settled), untilStopped(loop, token));
        }(&loop, &readiness, &settled, stop.get_token()));
    } };
    awaitWaiters(readiness, 1);
    stop.request_stop();
    waiting.join();
    // Unparked without an answer, and no longer counted: a later ready reaches nobody.
    CHECK_FALSE(settled.has_value());
    CHECK(readiness.waiting() == 0);
    readiness.markReady();
    CHECK(readiness.state() == State::Ready);
}

TEST_CASE("a deadline ends a wait on the application readiness", "[platform][readiness]")
{
    auto readiness = ApplicationReadiness { State::Pending };
    auto const backend = core::net::makeDefaultBackend();
    auto loop = core::net::EventLoop { *backend };
    auto const settled = loop.blockOn(core::net::withTimeout(&loop, readiness.whenSettled(), 10ms));
    CHECK_FALSE(settled.has_value());
    CHECK(readiness.waiting() == 0);
    CHECK(readiness.state() == State::Pending);
}
