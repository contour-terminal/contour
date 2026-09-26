// SPDX-License-Identifier: Apache-2.0
//
// The free `sleepUntil(EventLoop*, tp)` and `nextWakeStep`.
//
// Ported from fastcached's `Async/SleepUntil_test.cpp` at `0708dd54`. Its subject is the two
// resolutions that must NOT park — a null loop and a deadline already gone — and the cases below
// assert that by asking `await_suspend()` directly and by resuming a flow exactly once. A case
// that only checked the flow finished "promptly" would pass on an implementation that parked and
// was resumed a turn later, which is the difference this file exists to hold.
//
// `await_suspend`, not `await_ready`: the awaiter's `await_ready` is a constant `false`, and both
// resolutions are its `await_suspend` declining to park (fastcached#1546, pinned in
// EventLoop_test.cpp).

#include <core/async/Task.hpp>
#include <core/net/EventLoop.hpp>
#include <core/net/SleepUntil.hpp>
#include <core/net/testing/TestLoop.hpp>
#include <core/platform/Clock.hpp>

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <coroutine>
#include <tuple>

using core::async::StopSource;
using core::async::Task;
using core::net::EventLoop;
using core::net::nextWakeStep;
using core::net::sleepUntil;
using core::net::testing::TestLoop;
using core::platform::ManualClock;
using core::platform::SteadyDuration;
using core::platform::SteadyTimePoint;
using namespace std::chrono_literals;

namespace
{

/// Awaits a free `sleepUntil` and produces a value, so a case can resume it once and ask whether
/// it finished.
/// @param loopOrNull The loop to sleep on, or nullptr.
/// @param deadline When to resume.
/// @return 7, once the sleep resolves.
Task<int> awaitSleepUntil(EventLoop* loopOrNull, SteadyTimePoint deadline)
{
    co_await sleepUntil(loopOrNull, deadline);
    co_return 7;
}

/// Records that a sleep resolved, for a case that drives the loop rather than resuming by hand.
/// @param loop The loop to sleep on.
/// @param deadline When to resume.
/// @param resumed Set once the sleep resolves; must outlive the loop.
Task<void> sleepThenMark(EventLoop* loop, SteadyTimePoint deadline, bool* resumed)
{
    co_await sleepUntil(loop, deadline);
    *resumed = true;
}

// `nextWakeStep` is the arithmetic, and it is usable at compile time so a caller can bound a wait
// in a constant. Asserted here rather than only at runtime, because a change that made it call
// something non-constexpr would still pass every runtime check below.
constexpr auto Epoch = SteadyTimePoint {};
static_assert(nextWakeStep(Epoch, Epoch + 100ms, 30ms) == Epoch + 30ms);
static_assert(nextWakeStep(Epoch, Epoch + 100ms, SteadyDuration::zero()) == Epoch + 100ms);

} // namespace

TEST_CASE("sleepUntil with no loop never suspends", "[SleepUntil]")
{
    auto const clock = ManualClock {};

    // Asked of the awaitable itself: `await_suspend() == false` is what "never parks" MEANS --
    // the flow resumes before `co_await` returns, with nothing filed anywhere. A case that only
    // observed a prompt return could not tell this from a park resumed in the same turn.
    auto awaiter = sleepUntil(nullptr, clock.now() + 1h);
    REQUIRE_FALSE(awaiter.await_suspend(std::noop_coroutine()));

    // And from the flow's side: one resume, and it is done. A suspension would leave it pending
    // with nothing in the world able to resume it, since there is no loop.
    auto task = awaitSleepUntil(nullptr, clock.now() + 1h);
    task.handle().resume();
    REQUIRE(task.done());
    CHECK(task.result() == 7);
}

TEST_CASE("sleepUntil with a deadline already gone never suspends", "[SleepUntil]")
{
    auto clock = ManualClock {};
    auto loop = TestLoop { clock };
    auto const elapsed = clock.now() - 1ms;

    auto awaiter = sleepUntil(&loop, elapsed);
    REQUIRE_FALSE(awaiter.await_suspend(std::noop_coroutine()));
    REQUIRE(loop.pendingTimerCount() == 0);

    auto task = awaitSleepUntil(&loop, elapsed);
    task.handle().resume();
    REQUIRE(task.done());
    CHECK(task.result() == 7);

    // Nothing was filed with the loop, so nothing has to be taken back out of it.
    CHECK(loop.pendingTimerCount() == 0);
    CHECK(loop.readyCount() == 0);
}

TEST_CASE("sleepUntil with a deadline already gone resolves even for a flow already stopped", "[SleepUntil]")
{
    // What the move into `await_suspend` must not change. The elapsed deadline used to be
    // `await_ready`'s answer, so the flow's token was never read and a stopped flow resumed
    // normally. `await_suspend` asks the clock BEFORE it reads the token, which keeps that: a
    // token read first would make `await_resume` throw here.
    auto clock = ManualClock {};
    auto loop = TestLoop { clock };
    auto source = StopSource {};
    source.request_stop();

    auto task = awaitSleepUntil(&loop, clock.now() - 1ms);
    task.handle().promise().setStopToken(source.get_token());
    task.handle().resume();

    REQUIRE(task.done());
    CHECK(task.result() == 7);
    CHECK(loop.pendingTimerCount() == 0);
}

TEST_CASE("sleepUntil with a deadline ahead parks until the clock reaches it", "[SleepUntil]")
{
    auto resumed = false;
    auto clock = ManualClock {};
    auto loop = TestLoop { clock };

    loop.spawn(sleepThenMark(&loop, clock.now() + 50ms, &resumed));
    std::ignore = loop.drain();
    REQUIRE_FALSE(resumed);
    REQUIRE(loop.pendingTimerCount() == 1);

    clock.advance(50ms);
    std::ignore = loop.drain();

    CHECK(resumed);
    CHECK(loop.pendingTimerCount() == 0);
}

TEST_CASE("nextWakeStep caps a step at the bound and never steps past the deadline", "[SleepUntil]")
{
    auto const deadline = Epoch + 100ms;

    CHECK(nextWakeStep(Epoch, deadline, 30ms) == Epoch + 30ms);
    CHECK(nextWakeStep(Epoch + 80ms, deadline, 30ms) == deadline); // the bound would overshoot
    CHECK(nextWakeStep(deadline, deadline, 30ms) == deadline);

    // A non-positive bound means "do not step", never "step by nothing": a zero-length step would
    // resolve as already-ready and spin the loop.
    CHECK(nextWakeStep(Epoch, deadline, SteadyDuration::zero()) == deadline);
    CHECK(nextWakeStep(Epoch, deadline, -5ms) == deadline);
}
