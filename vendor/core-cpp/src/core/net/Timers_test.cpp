// SPDX-License-Identifier: Apache-2.0
//
// `EventLoop::addTimer` / `cancelTimer`: the callback half of the loop's one deadline mechanism.
//
// A callback timer has no coroutine frame, so what it shares with `delay()` is the PARK TABLE --
// the same heap, the same sequence numbers, the same generation-checked ids, the same turn step.
// These cases are what says so: the firing order across the two kinds, the cancellation window,
// and the measurement of what lazy pruning costs a loop that arms and cancels a deadline per
// request (Task B4's report, concern 4).

#include <core/async/Cancellation.hpp>
#include <core/async/ParkedWork.hpp>
#include <core/async/Task.hpp>
#include <core/net/EventLoop.hpp>
#include <core/net/testing/ScriptedBackend.hpp>
#include <core/net/testing/TestLoop.hpp>
#include <core/platform/Clock.hpp>

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <cstddef>
#include <optional>
#include <ranges>
#include <string>
#include <tuple>
#include <type_traits>
#include <vector>

using core::async::OperationCancelled;
using core::async::Task;
using core::net::EventLoop;
using core::net::ParkEntry;
using core::net::TimerId;
using core::net::testing::ScriptedBackend;
using core::net::testing::TestLoop;
using core::platform::ManualClock;
using namespace std::chrono_literals;

namespace
{

// The half of `TimerId`'s reason that no runtime case can reach, checked by the compiler instead.
// `cancelTimer` retires a callback and `requestCancel` unwinds a coroutine; only one of them is
// meaningful for any given id, and the strong type is what stops them being handed each other's.
// The program that would prove this by failing is the one that does not compile, so the assertion
// has to live here rather than in a TEST_CASE -- and in prose it was load-bearing but unchecked.
static_assert(std::is_invocable_v<decltype(&EventLoop::cancelTimer), EventLoop&, TimerId>,
              "cancelTimer takes a TimerId");
static_assert(!std::is_invocable_v<decltype(&EventLoop::cancelTimer), EventLoop&, core::net::ParkId>,
              "a bare ParkId must not reach cancelTimer: it would name a park of the wrong kind");
static_assert(!std::is_invocable_v<decltype(&EventLoop::requestCancel), EventLoop&, TimerId>,
              "a TimerId must not reach requestCancel either -- the conversion goes neither way");

/// What a timer callback did, so a case can assert the order rather than only the count.
struct Trace
{
    std::vector<std::string> fired; ///< Labels, in the order their callbacks ran.

    /// @param label What to record.
    void record(std::string label) { fired.push_back(std::move(label)); }
};

/// One armed label: what @c recordLabel writes into @c Trace when it runs.
struct Marker
{
    Trace* trace = nullptr; ///< Where to record.
    std::string label;      ///< What to record.
};

/// A @c core::net::TimerCallback that records its marker's label.
/// @param state A @c Marker, which must outlive the loop.
void recordLabel(void* state)
{
    auto* const marker = static_cast<Marker*>(state);
    marker->trace->record(marker->label);
}

/// A @c core::net::TimerCallback that counts its calls.
/// @param state A @c std::size_t counter, which must outlive the loop.
void countCall(void* state)
{
    ++*static_cast<std::size_t*>(state);
}

/// State for @c reArmOnce: the loop and clock to arm against, and how often it has run.
struct Rearm
{
    TestLoop* loop = nullptr;     ///< The loop to arm the second timer on.
    ManualClock* clock = nullptr; ///< The clock the second deadline is measured against.
    std::size_t calls = 0;        ///< How many times the callback has run.
};

/// A @c core::net::TimerCallback that arms a second timer the first time it runs.
/// @param state A @c Rearm, which must outlive the loop.
void reArmOnce(void* state)
{
    auto* const self = static_cast<Rearm*>(state);
    ++self->calls;
    if (self->calls == 1)
        std::ignore = self->loop->addTimer(self->clock->now() + 10ms, &reArmOnce, state);
}

/// Runs to completion the moment it is resumed, for a case that parks its handle by hand.
/// @param ran Set when the body runs.
Task<void> markRan(bool* ran)
{
    *ran = true;
    co_return;
}

/// Parks on a deadline an hour out and records that it was CANCELLED rather than resumed.
/// @param loop The loop to park on.
/// @param cancelled Set when the flow unwinds through @c OperationCancelled.
Task<void> recordCancellation(EventLoop* loop, bool* cancelled)
{
    try
    {
        co_await loop->delay(1h);
    }
    catch (OperationCancelled const&)
    {
        *cancelled = true;
    }
}

/// Parks a flow on a deadline so a case has a coroutine timer beside its callback timers.
/// @param loop The loop to park on.
/// @param delay How long to sleep.
/// @param trace Where to record the resumption.
/// @param label What to record.
Task<void> recordAfterDelay(EventLoop* loop, std::chrono::milliseconds delay, Trace* trace, std::string label)
{
    co_await loop->delay(delay);
    trace->record(std::move(label));
}

} // namespace

TEST_CASE("A timer armed for a deadline already past does not fire from addTimer itself",
          "[EventLoop][timer]")
{
    // The caller must never be re-entered from its own arming call: a DeadlineTimer's constructor
    // arms, and a callback running from inside it would reach an object that does not exist yet.
    auto clock = ManualClock {};
    auto loop = TestLoop { clock };

    auto calls = std::size_t { 0 };
    auto const timer = loop.addTimer(clock.now() - 1ms, &countCall, &calls);

    REQUIRE(static_cast<bool>(timer));
    REQUIRE(calls == 0); // not from the arming call
    REQUIRE(loop.pendingTimerCount() == 1);

    std::ignore = loop.drain();
    CHECK(calls == 1);
    CHECK(loop.pendingTimerCount() == 0);
}

TEST_CASE("A timer fires once its deadline is reached and not before", "[EventLoop][timer]")
{
    auto clock = ManualClock {};
    auto loop = TestLoop { clock };

    auto calls = std::size_t { 0 };
    std::ignore = loop.addTimer(clock.now() + 50ms, &countCall, &calls);

    std::ignore = loop.drain();
    REQUIRE(calls == 0); // the clock has not moved

    clock.advance(49ms);
    std::ignore = loop.drain();
    REQUIRE(calls == 0); // one millisecond short

    clock.advance(1ms);
    std::ignore = loop.drain();
    CHECK(calls == 1);

    // And exactly once: a park is due once, and its deadline is disarmed as it fires.
    clock.advance(1s);
    std::ignore = loop.drain();
    CHECK(calls == 1);
}

TEST_CASE("Timers due at the same instant fire in the order they were armed", "[EventLoop][timer]")
{
    // FIFO by sequence among equal deadlines, which is the park table's tie-break and is what
    // makes two timers armed for one instant deterministic rather than hash-ordered.
    auto clock = ManualClock {};
    auto loop = TestLoop { clock };

    auto trace = Trace {};
    auto const due = clock.now() + 10ms;
    auto first = Marker { .trace = &trace, .label = "first" };
    auto second = Marker { .trace = &trace, .label = "second" };
    auto third = Marker { .trace = &trace, .label = "third" };
    std::ignore = loop.addTimer(due, &recordLabel, &first);
    std::ignore = loop.addTimer(due, &recordLabel, &second);
    std::ignore = loop.addTimer(due, &recordLabel, &third);

    clock.advance(10ms);
    std::ignore = loop.drain();

    CHECK(trace.fired == std::vector<std::string> { "first", "second", "third" });
}

TEST_CASE("A callback timer and a coroutine deadline share one firing order", "[EventLoop][timer]")
{
    // The point of putting callbacks in the PARK TABLE rather than in a list of their own: the
    // two kinds interleave by deadline, then by arming order, because there is one heap and one
    // sequence counter. Two mechanisms would order each kind among itself and nothing across.
    auto clock = ManualClock {};
    auto loop = TestLoop { clock };

    auto trace = Trace {};
    auto early = Marker { .trace = &trace, .label = "callback@5" };
    auto late = Marker { .trace = &trace, .label = "callback@15" };
    std::ignore = loop.addTimer(clock.now() + 5ms, &recordLabel, &early);
    loop.spawn(recordAfterDelay(&loop, 10ms, &trace, "coroutine@10"));
    std::ignore = loop.addTimer(clock.now() + 15ms, &recordLabel, &late);

    // Drained BEFORE the clock moves, because `delay()` measures from the instant the flow RUNS,
    // not from the `spawn`: a spawned coroutine has not parked on anything until a turn has
    // resumed it. Without this the flow's deadline lands 10ms past the advance and the two
    // callbacks fire alone -- which is what the first run of this case reported.
    std::ignore = loop.drain();
    REQUIRE(loop.pendingTimerCount() == 3);

    // The coroutine was armed SECOND and is due between the two callbacks, so an order that came
    // from anything but the shared deadline heap -- per-kind queues, or arming sequence -- would
    // put it somewhere else.
    clock.advance(20ms);
    std::ignore = loop.drain();

    CHECK(trace.fired == std::vector<std::string> { "callback@5", "coroutine@10", "callback@15" });
}

TEST_CASE("cancelTimer before the deadline prevents the callback and reports the transfer",
          "[EventLoop][timer]")
{
    auto clock = ManualClock {};
    auto loop = TestLoop { clock };

    auto calls = std::size_t { 0 };
    auto const timer = loop.addTimer(clock.now() + 50ms, &countCall, &calls);
    REQUIRE(loop.pendingTimerCount() == 1);

    CHECK(loop.cancelTimer(timer));
    CHECK(loop.pendingTimerCount() == 0);

    clock.advance(1s);
    std::ignore = loop.drain();
    CHECK(calls == 0);

    // Idempotent, and the second call reports that it did nothing.
    CHECK_FALSE(loop.cancelTimer(timer));
}

TEST_CASE("cancelTimer after the callback has run reports false", "[EventLoop][timer]")
{
    // Ids are never reused, so a cancel naming a timer that has already run resolves to nothing
    // rather than cancelling whichever timer was armed since.
    auto clock = ManualClock {};
    auto loop = TestLoop { clock };

    auto fired = std::size_t { 0 };
    auto const spent = loop.addTimer(clock.now() + 10ms, &countCall, &fired);
    clock.advance(10ms);
    std::ignore = loop.drain();
    REQUIRE(fired == 1);

    auto survivor = std::size_t { 0 };
    auto const later = loop.addTimer(clock.now() + 10ms, &countCall, &survivor);
    REQUIRE(later != spent);

    CHECK_FALSE(loop.cancelTimer(spent));

    clock.advance(10ms);
    std::ignore = loop.drain();
    CHECK(survivor == 1); // the stale cancel took nothing with it
}

TEST_CASE("cancelTimer refuses a TimerId that names a coroutine park", "[EventLoop][timer]")
{
    // **The arm the whole "TimerId is a distinct struct" argument rests on.** `TimerId` is an
    // aggregate over a public `ParkId`, so `TimerId { somePark }` is one expression -- and
    // `registerPark` is public, so a caller can hold a coroutine's park id. Without the kind
    // check, `cancelTimer` would take that park: dropping it releases the claim its `parked`
    // holds, so a suspended flow is either freed under its own feet or left parked forever with
    // nothing able to resume it.
    //
    // Every other case here passes an id `addTimer` returned, so deleting the check reds only
    // this one.
    auto clock = ManualClock {};
    auto loop = TestLoop { clock };

    auto ran = false;
    auto flow = markRan(&ran); // lazy: the body has not run, and this `Task` owns the frame
    auto const park = loop.registerPark(
        ParkEntry::onDeadline(core::async::ParkedWork { .resume = flow.handle() }, clock.now() + 10ms));
    REQUIRE(static_cast<bool>(park));
    REQUIRE(loop.pendingTimerCount() == 1);

    CHECK_FALSE(loop.cancelTimer(TimerId { park }));
    CHECK(loop.pendingTimerCount() == 1); // refused BEFORE the park was taken, so it is still here

    clock.advance(10ms);
    std::ignore = loop.drain();
    CHECK(ran); // and the flow the id named still resumed
}

TEST_CASE("requestCancel handed a timer's park leaves the timer armed", "[EventLoop][timer]")
{
    // **The mirror of the case above, in the direction the fix round did not go.** The three
    // `static_assert`s at the top of this file prove the two id types do not CONVERT, which is
    // what stops `cancelTimer(somePark)` and `requestCancel(someTimer)` compiling. They say
    // nothing about `requestCancel(timer.park)`: `TimerId::park` is a public member of an
    // aggregate, so that form compiles and no compile-time check can refuse it.
    //
    // **This case does not turn red on any single mutation, and that is stated rather than
    // discovered.** The behaviour is defended twice: `resolveCancel` returns early on
    // `!entry->parked`, and `queueParkedWaiter` would in any case short-circuit on the empty
    // waiter a callback park has. Removing either alone leaves this green. It is here because
    // nothing exercised the path at all, so a later change that removed BOTH -- or that gave a
    // callback park a waiter -- would have had nothing to answer to.
    auto clock = ManualClock {};
    auto loop = TestLoop { clock };

    auto calls = std::size_t { 0 };
    auto const timer = loop.addTimer(clock.now() + 10ms, &countCall, &calls);
    REQUIRE(loop.pendingTimerCount() == 1);

    loop.requestCancel(timer.park); // compiles, and must do nothing

    CHECK(loop.pendingTimerCount() == 1); // not taken
    CHECK(loop.readyCount() == 0);        // and nothing queued for a drain to run

    clock.advance(10ms);
    std::ignore = loop.drain();
    CHECK(calls == 1); // the timer the cancel named still fired
}

TEST_CASE("cancelTimer still prevents a callback whose deadline has fired but not yet run",
          "[EventLoop][timer]")
{
    // The window between step 5 queueing a due timer and step 2 running it. `cancelTimer` has to
    // reach into it, or a DeadlineTimer destroyed in that window would have its callback run
    // against storage that is gone -- and "already fired" would mean "no longer cancellable and
    // not yet harmless".
    auto clock = ManualClock {};
    auto loop = TestLoop { clock };

    auto calls = std::size_t { 0 };
    auto const timer = loop.addTimer(clock.now() + 10ms, &countCall, &calls);

    clock.advance(10ms);
    std::ignore = loop.tick(); // step 5 fires it: queued, not yet run
    REQUIRE(calls == 0);
    REQUIRE(loop.readyCount() == 1);

    CHECK(loop.cancelTimer(timer));

    std::ignore = loop.drain();
    CHECK(calls == 0);
}

TEST_CASE("A timer callback may arm another timer", "[EventLoop][timer]")
{
    // A repeating timer is written this way -- there is no periodic timer, and a callback that
    // re-arms is how a consumer gets one -- so the table must accept a park made while the turn
    // is walking the parks it already took.
    auto clock = ManualClock {};
    auto loop = TestLoop { clock };
    auto rearm = Rearm { .loop = &loop, .clock = &clock };

    std::ignore = loop.addTimer(clock.now(), &reArmOnce, &rearm);
    std::ignore = loop.drain();
    REQUIRE(rearm.calls == 1); // the second is armed for 10ms out, which has not arrived

    clock.advance(10ms);
    std::ignore = loop.drain();
    CHECK(rearm.calls == 2);
    CHECK(loop.pendingTimerCount() == 0); // and it stopped: the second call arms nothing
}

TEST_CASE("A loop destroyed with a timer armed never runs it", "[EventLoop][timer]")
{
    // Declared BEFORE the loop so it outlives it: ~EventLoop frees what it holds, and a counter
    // the callback would touch has to still be there for the case to be able to say it did not.
    auto calls = std::size_t { 0 };
    auto clock = ManualClock {};
    {
        auto loop = TestLoop { clock };
        std::ignore = loop.addTimer(clock.now() - 1ms, &countCall, &calls);
        std::ignore = loop.tick(); // due, queued, not yet run
        REQUIRE(calls == 0);
        REQUIRE(loop.readyCount() == 1);
    }
    CHECK(calls == 0);
}

TEST_CASE("requestStop() cancels flows and leaves callback timers armed", "[EventLoop][timer]")
{
    // Stated rather than left to be discovered, and BOTH clauses are exercised: `requestStop`
    // exists to make parked FLOWS unwind through OperationCancelled, and a callback timer has no
    // flow to unwind -- there is nothing to throw into and nobody to catch it. So it stays armed
    // and still fires, and a consumer that wants it gone cancels it. Tasks B8 and B12 both stop
    // loops with timers on them.
    //
    // `cancelled` is declared BEFORE the loop so it outlives it: ~EventLoop unwinds what is
    // parked, and the unwinding writes here.
    auto cancelled = false;
    auto clock = ManualClock {};
    auto loop = TestLoop { clock };

    auto calls = std::size_t { 0 };
    std::ignore = loop.addTimer(clock.now() + 10ms, &countCall, &calls);
    loop.spawn(recordCancellation(&loop, &cancelled));
    std::ignore = loop.drain();
    REQUIRE(loop.pendingTimerCount() == 2); // the flow's hour-long deadline, and the timer

    loop.requestStop();
    std::ignore = loop.drain();
    CHECK(cancelled);                       // the flow unwound
    REQUIRE(loop.pendingTimerCount() == 1); // and the timer did not

    clock.advance(10ms);
    std::ignore = loop.drain();
    CHECK(calls == 1);
}

TEST_CASE("An armed timer is what bounds the turn's wait, so nothing polls", "[EventLoop][timer]")
{
    // The whole of Task B5 in one assertion. fastcached's DeadlineTimer woke every 50ms because
    // its `Schedule` could not be taken back; here the loop knows its next deadline, so a timer
    // 500ms out makes the turn wait 500ms and wake once.
    auto clock = ManualClock {};
    auto backend = ScriptedBackend {};
    backend.pushTimeout();
    auto loop = EventLoop { backend, clock };

    auto calls = std::size_t { 0 };
    std::ignore = loop.addTimer(clock.now() + 500ms, &countCall, &calls);
    std::ignore = loop.runOnce();

    REQUIRE(backend.waitCount() == 1);
    REQUIRE(backend.recordedTimeouts().size() == 1);
    CHECK(backend.recordedTimeouts().front() == std::optional { core::platform::SteadyDuration { 500ms } });
    CHECK(calls == 0);
}

TEST_CASE("Lazy timer pruning is bounded by the deadlines armed behind the live root",
          "[EventLoop][timer][scale]")
{
    // Task B4's report, concern 4: a cancelled deadline leaves a slot in the heap until the root
    // reaches it, and B5 is the task that arms and cancels deadlines at a rate that finds out
    // whether that is enough. Measured rather than argued, with N and the expected counts fixed
    // before the run.
    //
    // N is 1000: it is the number of arm/cancel pairs, and every assertion below is an exact
    // equality, so a larger N would buy nothing but Debug-CRT allocator time on Windows.
    constexpr auto N = std::size_t { 1000 };

    auto clock = ManualClock {};
    auto calls = std::size_t { 0 };

    SECTION("with no live deadline the whole stale heap is pruned in one turn")
    {
        auto loop = TestLoop { clock };
        for ([[maybe_unused]] auto const index: std::views::iota(std::size_t { 0 }, N))
        {
            auto const timer = loop.addTimer(clock.now() + 1h, &countCall, &calls);
            REQUIRE(loop.cancelTimer(timer));
        }
        REQUIRE(loop.pendingTimerCount() == 0);
        REQUIRE(loop.pendingTimerSlotCount() == N); // nothing has pruned yet: no turn has run

        std::ignore = loop.tick();

        // Pruning walks from the ROOT while the root is stale, and every slot here is: the heap
        // collapses in one pass. This is the shape a loop with one deadline per request has.
        CHECK(loop.pendingTimerSlotCount() == 0);
    }

    SECTION("a live root holds every deadline armed behind it, until it fires")
    {
        auto loop = TestLoop { clock };
        auto rootCalls = std::size_t { 0 };
        std::ignore = loop.addTimer(clock.now() + 1ms, &countCall, &rootCalls); // the soonest: the root
        for ([[maybe_unused]] auto const index: std::views::iota(std::size_t { 0 }, N))
        {
            auto const timer = loop.addTimer(clock.now() + 1h, &countCall, &calls);
            REQUIRE(loop.cancelTimer(timer));
        }

        std::ignore = loop.tick();

        // The growth B4 named, and its exact bound: the root is live, so pruning stops at it and
        // every cancelled slot behind it stays. Eager erase-and-reheap would report 1 here -- and
        // would be O(n) per cancel, which is the quadratic loop upstream had.
        REQUIRE(loop.pendingTimerCount() == 1);
        CHECK(loop.pendingTimerSlotCount() == N + 1);

        // And why lazy is enough: the stale slots are all LATER than the root by construction --
        // the root is the soonest live deadline -- so the turn that fires the root reclaims them.
        clock.advance(1ms);
        std::ignore = loop.drain();
        REQUIRE(rootCalls == 1);
        CHECK(loop.pendingTimerSlotCount() == 0);
        CHECK(calls == 0);
    }
}
