// SPDX-License-Identifier: Apache-2.0
//
// `interruptibleSleepUntil` — sleep to a deadline, or until a token is stopped.
//
// Ported from fastcached's `Async/InterruptibleSleep_test.cpp` at `0708dd54` and **rewritten**:
// upstream's wait was a poll, so its cases asserted that a cancel was noticed within one
// `wakeBound` of real time. Here the park is retired by the stop callback, so the cases below run
// with a `ManualClock` FROZEN -- no case can pass by time elapsing -- and each one asserts that
// the park table is empty afterwards, which is what tells a cancel apart from a race that
// happened to come out right.

#include <core/async/Cancellation.hpp>
#include <core/async/Task.hpp>
#include <core/net/EventLoop.hpp>
#include <core/net/InterruptibleSleep.hpp>
#include <core/net/testing/TestLoop.hpp>
#include <core/platform/Clock.hpp>

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <cstdint>
#include <tuple>

using core::async::OperationCancelled;
using core::async::StopSource;
using core::async::StopToken;
using core::async::Task;
using core::net::EventLoop;
using core::net::interruptibleSleepUntil;
using core::net::WakeReason;
using core::net::testing::TestLoop;
using core::platform::ManualClock;
using core::platform::SteadyTimePoint;
using namespace std::chrono_literals;

namespace
{

/// How a wait ended, from the awaiting flow's point of view — which is one answer wider than
/// @c WakeReason, because the FLOW's own cancellation unwinds rather than reporting.
enum class Outcome : std::uint8_t
{
    Pending = 0, ///< It has not finished.
    Deadline,    ///< It returned @c WakeReason::Deadline.
    Cancelled,   ///< It returned @c WakeReason::Cancelled.
    Unwound,     ///< It threw @c OperationCancelled.
};

/// Awaits an interruptible sleep and records how it ended.
/// @param loop The loop to sleep on, or nullptr.
/// @param token The token that interrupts the sleep.
/// @param deadline When the sleep ends if nothing interrupts it.
/// @param outcome Where to record the answer; must outlive the loop.
Task<void> sleepAndRecord(EventLoop* loop, StopToken token, SteadyTimePoint deadline, Outcome* outcome)
{
    try
    {
        auto const reason = co_await interruptibleSleepUntil(loop, std::move(token), deadline);
        *outcome = reason == WakeReason::Cancelled ? Outcome::Cancelled : Outcome::Deadline;
    }
    catch (OperationCancelled const&)
    {
        *outcome = Outcome::Unwound;
    }
}

/// Awaits the deprecated four-argument form and records what it answered.
/// @param loop The loop to sleep on.
/// @param token The token that interrupts the sleep.
/// @param deadline When the sleep ends if nothing interrupts it.
/// @param wakeBound The bound the overload ignores.
/// @param reason Where to record the answer; must outlive the loop.
/// @param finished Set once the wait has ended; must outlive the loop.
Task<void> sleepWithBound(EventLoop* loop,
                          StopToken token,
                          SteadyTimePoint deadline,
                          core::platform::SteadyDuration wakeBound,
                          WakeReason* reason,
                          bool* finished)
{
    *reason = co_await interruptibleSleepUntil(loop, std::move(token), deadline, wakeBound);
    *finished = true;
}

} // namespace

TEST_CASE("A stopped token wakes an interruptible sleep with the clock frozen, and leaves nothing parked",
          "[InterruptibleSleep]")
{
    // Declared BEFORE the loop so it outlives it (~EventLoop unwinds what is parked, and the
    // unwinding writes here).
    auto outcome = Outcome::Pending;
    auto clock = ManualClock {};
    auto source = StopSource {};
    auto loop = TestLoop { clock };

    // An hour out, and the clock never moves: nothing below can be explained by time passing.
    loop.spawn(sleepAndRecord(&loop, source.get_token(), clock.now() + 1h, &outcome));
    std::ignore = loop.drain();
    REQUIRE(outcome == Outcome::Pending);
    REQUIRE(loop.pendingTimerCount() == 1);

    source.request_stop();

    // ONE turn: step 1 resolves the cancel, step 2 resumes the flow. Not "eventually".
    CHECK(loop.tick() == 1);
    CHECK(outcome == Outcome::Cancelled);

    // What distinguishes a cancel from a race that came out right: the park is gone, so nothing
    // is left for the deadline an hour from now to fire into.
    CHECK(loop.pendingTimerCount() == 0);
    CHECK(loop.readyCount() == 0);
}

TEST_CASE("A token already stopped never parks at all", "[InterruptibleSleep]")
{
    auto outcome = Outcome::Pending;
    auto clock = ManualClock {};
    auto source = StopSource {};
    source.request_stop();
    auto loop = TestLoop { clock };

    loop.spawn(sleepAndRecord(&loop, source.get_token(), clock.now() + 1h, &outcome));
    std::ignore = loop.drain();

    CHECK(outcome == Outcome::Cancelled);
    CHECK(loop.pendingTimerCount() == 0); // asked before anything is scheduled
}

TEST_CASE("A null loop reports Deadline without parking", "[InterruptibleSleep]")
{
    // The nullable-loop contract `sleepUntil` states, kept here for the same callers: a transport
    // with no loop behind it has no deadline mechanism, so the wait is over and nothing cancelled
    // it.
    auto outcome = Outcome::Pending;
    auto clock = ManualClock {};
    auto source = StopSource {};
    auto loop = TestLoop { clock };

    loop.spawn(sleepAndRecord(nullptr, source.get_token(), clock.now() + 1h, &outcome));
    std::ignore = loop.drain();

    // No `pendingTimerCount() == 0` here: the sleep was handed `nullptr` and has no way to reach
    // this loop, so that assertion is true for every possible implementation. The line above is
    // the one that can come out the other way.
    CHECK(outcome == Outcome::Deadline);
}

TEST_CASE("The deadline arriving reports Deadline", "[InterruptibleSleep]")
{
    auto outcome = Outcome::Pending;
    auto clock = ManualClock {};
    auto source = StopSource {};
    auto loop = TestLoop { clock };

    loop.spawn(sleepAndRecord(&loop, source.get_token(), clock.now() + 50ms, &outcome));
    std::ignore = loop.drain();
    REQUIRE(outcome == Outcome::Pending);

    clock.advance(50ms);
    std::ignore = loop.drain();

    CHECK(outcome == Outcome::Deadline);
    CHECK(loop.pendingTimerCount() == 0);
}

TEST_CASE("A deadline already gone reports Deadline without parking", "[InterruptibleSleep]")
{
    // Decided in the awaiter's `await_suspend`, which declines to park, and no longer in its
    // `await_ready`, which is a constant (fastcached#1546). The answer and the empty park table
    // are what that move must not change.
    auto outcome = Outcome::Pending;
    auto clock = ManualClock {};
    auto source = StopSource {};
    auto loop = TestLoop { clock };

    loop.spawn(sleepAndRecord(&loop, source.get_token(), clock.now() - 1ms, &outcome));
    std::ignore = loop.drain();

    CHECK(outcome == Outcome::Deadline);
    CHECK(loop.pendingTimerCount() == 0);
}

TEST_CASE("A cancel of the awaiting flow's own token unwinds instead of reporting", "[InterruptibleSleep]")
{
    // Two cancellations reach this wait and they mean different things. The token the CALLER
    // supplied is the interruption this function exists to report, so it comes back as a value.
    // The FLOW's own token is the loop's ordinary cancellation, and every loop awaitable answers
    // that by throwing, so the frame unwinds and its cleanup runs rather than carrying on with a
    // value nobody will look at.
    auto outcome = Outcome::Pending;
    auto clock = ManualClock {};
    auto source = StopSource {};
    auto loop = TestLoop { clock };

    loop.spawn(sleepAndRecord(&loop, source.get_token(), clock.now() + 1h, &outcome));
    std::ignore = loop.drain();
    REQUIRE(outcome == Outcome::Pending);

    loop.requestStop(); // the ROOT token, which the spawned flow inherits — not `source`
    std::ignore = loop.drain();

    CHECK(outcome == Outcome::Unwound);
    CHECK(loop.pendingTimerCount() == 0);
}

TEST_CASE("The supplied token is reported even when it is also the flow's own", "[InterruptibleSleep]")
{
    // A caller that hands in the token its own flow carries has asked to be TOLD, and it gets an
    // answer rather than an exception. The order of the two checks in await_resume is what decides
    // this, and it is the friendlier way round.
    auto outcome = Outcome::Pending;
    auto clock = ManualClock {};
    auto loop = TestLoop { clock };

    loop.spawn(sleepAndRecord(&loop, loop.rootStopSource().get_token(), clock.now() + 1h, &outcome));
    std::ignore = loop.drain();
    REQUIRE(outcome == Outcome::Pending);

    loop.requestStop();
    std::ignore = loop.drain();

    CHECK(outcome == Outcome::Cancelled);
    CHECK(loop.pendingTimerCount() == 0);
}

TEST_CASE("The wakeBound overload forwards, and the bound changes nothing", "[InterruptibleSleep]")
{
    // Kept for one release so a fastcached caller compiles unchanged. The bound is IGNORED: it
    // named the longest uninterruptible step of a poll, and there is no poll left to bound. The
    // case asserts that what a caller passing 5ms gets is one park and one wake-up at the cancel,
    // not a step every 5ms -- which is the behaviour change the overload's doc promises.
    auto clock = ManualClock {};
    auto source = StopSource {};
    auto loop = TestLoop { clock };

    auto reason = WakeReason::Deadline;
    auto done = false;
    loop.spawn(sleepWithBound(&loop, source.get_token(), clock.now() + 1h, 5ms, &reason, &done));
    std::ignore = loop.drain();
    REQUIRE_FALSE(done);
    REQUIRE(loop.pendingTimerCount() == 1); // one park, not one park per 5ms step

    source.request_stop();
    std::ignore = loop.drain();

    CHECK(done);
    CHECK(reason == WakeReason::Cancelled);
    CHECK(loop.pendingTimerCount() == 0);
}
