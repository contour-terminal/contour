// SPDX-License-Identifier: Apache-2.0
//
// A loop that does not own its thread.
//
// Every other backend blocks, and the loop's turn is what waits. A host-driven one cannot: under
// single-threaded WebAssembly there is no thread to block, and inside somebody else's event loop
// — Qt's, GLib's, a game engine's frame callback — the thread is not ours to stop. So the loop is
// PUMPED: it runs one turn, asks the host for the next pump, and returns.
//
// **It is portable, and the browser is only one of its hosts.** Every case here runs on every
// platform over `testing::ManualHostScheduler`, which is what makes the behaviour something this
// repository can hold still rather than something only a node run can observe. What a node run
// adds is that the same code compiles and links under Emscripten, which this file's membership of
// the WebAssembly test binary is what proves.
#include <core/async/DetachedTask.hpp>
#include <core/async/ParkedWork.hpp>
#include <core/async/Task.hpp>
#include <core/net/EventLoop.hpp>
#include <core/net/HostDrivenBackend.hpp>
#include <core/net/testing/ManualHostScheduler.hpp>
#include <core/platform/Clock.hpp>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <chrono>
#include <cstddef>
#include <memory>
#include <string_view>
#include <tuple>

using core::async::Task;
using core::net::EventLoop;
using core::net::HostDrivenBackend;
using core::net::testing::ManualHostScheduler;
using core::platform::ManualClock;
using namespace std::chrono_literals;

namespace
{

/// Parks on a delay and records that it came back.
/// @param loop The loop to park on.
/// @param duration How long to wait.
/// @param fired Set once it resumes.
Task<void> delayThenFlag(EventLoop* loop, core::platform::SteadyDuration duration, bool* fired)
{
    co_await loop->delay(duration);
    *fired = true;
}

/// Parks on a delay and records that it came back, from a flow NOBODY owns.
///
/// @c core::async::DetachedTask has @c std::suspend_never for its initial suspension, so calling
/// this runs the body -- and reaches the @c co_await -- INLINE, on whatever thread called it and
/// outside any turn. That is what makes it the reachable form of the defect the case below is
/// about: no test double, no new API, just a flow started from a host callback.
/// @param loop The loop to park on.
/// @param duration How long to wait.
/// @param fired Set once it resumes; must outlive the loop.
core::async::DetachedTask delayThenFlagDetached(EventLoop* loop,
                                                core::platform::SteadyDuration duration,
                                                bool* fired)
{
    co_await loop->delay(duration);
    *fired = true;
}

/// A lazy flow that records that it ran.
///
/// @c Task suspends at its initial suspension, so the frame exists and has not run: its handle is
/// something a caller can hand to @c resumeSoon, and the @c Task value owns it until then.
/// @param ran Set when the body runs.
/// @return The suspended flow.
Task<void> markRan(bool* ran)
{
    *ran = true;
    co_return;
}

/// A @c core::net::TimerCallback that counts its calls.
/// @param state A @c std::size_t counter, which must outlive the loop.
void countCall(void* state)
{
    ++*static_cast<std::size_t*>(state);
}

/// @param host The host to ask.
/// @return The delay of the host's soonest pending request, in milliseconds.
[[nodiscard]] long long soonestDelayMs(ManualHostScheduler const& host)
{
    auto soonest = std::chrono::milliseconds::max();
    for (auto const& request: host.pending())
        soonest = request.delay < soonest ? request.delay : soonest;
    return soonest == std::chrono::milliseconds::max() ? -1 : soonest.count();
}

} // namespace

TEST_CASE("A host-driven loop runs a posted callback only once the host pumps", "[EventLoop][hostdriven]")
{
    // The whole of what "does not own its thread" means, in one assertion. `post` hands work over
    // and asks the host for a turn; nothing runs until the host gives one. A loop that ran the
    // callback inline would be running application code on whichever thread called `post`, which
    // is the one thing a single-threaded host cannot survive.
    auto clock = ManualClock {};
    auto host = ManualHostScheduler {};
    auto backend = HostDrivenBackend { host, clock };
    auto loop = EventLoop { backend, clock };

    // Declared after the loop, and safe only because the work that writes here has
    // FINISHED by the time the loop is destroyed. A case that left this work parked would
    // make ~EventLoop resume it into storage already gone; every case here that does so
    // declares its counter BEFORE the loop for that reason.
    auto ran = 0;
    loop.post([&ran] { ++ran; });

    CHECK(ran == 0);
    REQUIRE(host.pendingCount() >= 1); // the post asked for a pump
    CHECK(backend.pumpScheduled());

    host.pump();
    CHECK(ran == 1);
    CHECK(backend.pumpCount() == 1);
}

TEST_CASE("A host-driven loop arms the host at its next deadline, and resumes when that pump comes",
          "[EventLoop][hostdriven]")
{
    // After every turn the loop tells the backend when its next deadline is, and the backend asks
    // the host to pump THEN. The deadline itself is asserted, not merely that something was asked:
    // a loop that armed the host for "as soon as possible" would spin the browser's timer at full
    // rate while its flow waited fifty milliseconds, and a loop that armed it too late would be a
    // timer that fires late with nothing to say so.
    auto clock = ManualClock {};
    auto host = ManualHostScheduler {};
    auto backend = HostDrivenBackend { host, clock };
    auto loop = EventLoop { backend, clock };

    auto fired = false;
    loop.spawn(delayThenFlag(&loop, 50ms, &fired));

    // The spawn queued work, so the first pump is asked for at once.
    host.pump();
    CHECK_FALSE(fired);
    REQUIRE(loop.pendingTimerCount() == 1);

    // And that turn armed the host at the deadline it parked on: 50ms out, exactly.
    REQUIRE(host.pendingCount() == 1);
    CHECK(soonestDelayMs(host) == 50);

    // The host's timer is what waits; a ManualClock is what makes the waiting exact.
    clock.advance(50ms);
    host.pump();
    CHECK(loop.pendingTimerCount() == 0); // step 5 of that turn found the deadline due
    CHECK_FALSE(fired);                   // and queued it: resumption is the NEXT turn's step 2

    host.pump();
    CHECK(fired);

    // Nothing is left to wait for, so nothing further is asked of the host: a loop that kept
    // arming would keep the page's timer alive with no work behind it.
    host.pump();
    CHECK(host.pendingCount() == 0);
}

TEST_CASE("A timer armed on a quiescent host-driven loop asks the host for the turn that runs it",
          "[EventLoop][hostdriven][timer]")
{
    // **`addTimer` files work that only a turn can reach, and on a host-driven loop nothing else
    // will ever start one.** `armHostWake()` runs at the END of a turn, and a quiescent
    // host-driven loop has nothing scheduled with the host, so there is no turn coming to arm the
    // deadline this call just filed: the timer would be correct, filed, and silently never fired.
    //
    // This comment used to list the members that wake and call the list complete. It was not:
    // `registerPark`, `resumeSoon` and `requestStop` did not, and the three cases below are theirs.
    // `addTimer` now inherits its arming from `registerPark` rather than computing its own, so
    // this case also covers the path the next three are about -- which is why it is still here
    // rather than folded into them: it is the one that pins the DEADLINE, 50ms and not now.
    //
    // Armed from the loop's own thread but OUTSIDE a turn, which is legal by the documented
    // contract -- `teardownIsSerialisedWithDispatch()` is true when nothing is driving -- and is
    // how a DOM event handler, a frame callback or a TUI input path arms one.
    auto calls = std::size_t { 0 };
    auto clock = ManualClock {};
    auto host = ManualHostScheduler {};
    auto backend = HostDrivenBackend { host, clock };
    auto loop = EventLoop { backend, clock };

    // Quiescent, asserted rather than assumed: a pump pending from anything else is exactly what
    // masked this in both WebAssembly programs, where a `spawn` preceded the timer and its wake
    // bought the turn that picked the deadline up.
    REQUIRE(host.pendingCount() == 0);

    std::ignore = loop.addTimer(clock.now() + 50ms, &countCall, &calls);

    // THE assertion. And the deadline itself, not merely that something was asked: a loop that
    // asked for "as soon as you can" would spin the page's timer for fifty milliseconds.
    REQUIRE(host.pendingCount() == 1);
    CHECK(soonestDelayMs(host) == 50);

    clock.advance(50ms);
    host.pump(); // a turn: step 5 finds the deadline due and QUEUES the callback
    CHECK(calls == 0);

    host.pump(); // the next turn's step 2 runs it
    CHECK(calls == 1);
}

TEST_CASE("A coroutine parking on a delay outside a turn asks a quiescent host-driven loop for one",
          "[EventLoop][hostdriven][timer]")
{
    // **The same defect as the case above, one level down, and the case above cannot see it.**
    // `addTimer` was given its arming directly, so the fix sits ABOVE `registerPark` -- and
    // `registerPark` is what every other park goes through. `DelayAwaiter::await_suspend` calls it
    // and nothing else asks the host for anything.
    //
    // Reachable today with no new API: a `DetachedTask` is eagerly started, so its body and its
    // `co_await` run inline at the call. A browser event handler that paces a redraw with
    // `co_await loop->delay(16ms)` parks off-turn on a quiescent loop, and the park is filed,
    // correct, and silently never resumed.
    //
    // Declared before the loop: this flow is owned by the loop, and a case that left it parked
    // would have `~EventLoop` free a frame whose body writes here.
    auto fired = false;
    auto clock = ManualClock {};
    auto host = ManualHostScheduler {};
    auto backend = HostDrivenBackend { host, clock };
    auto loop = EventLoop { backend, clock };

    // Quiescent, asserted rather than assumed -- a pump pending from anything else buys a turn
    // whose `armHostWake()` picks the deadline up, which is exactly what masks this.
    REQUIRE(host.pendingCount() == 0);

    delayThenFlagDetached(&loop, 50ms, &fired); // runs inline, parks, returns

    // THE assertion, and the deadline with it: a loop that asked for "as soon as you can" would
    // spin the host's timer for fifty milliseconds.
    REQUIRE(host.pendingCount() == 1);
    CHECK(soonestDelayMs(host) == 50);

    clock.advance(50ms);
    host.pump(); // step 5 finds the deadline due and queues the waiter
    host.pump(); // the next turn's step 2 resumes it
    CHECK(fired);
}

TEST_CASE("resumeSoon outside a turn asks a quiescent host-driven loop for the turn that runs it",
          "[EventLoop][hostdriven]")
{
    // `resumeSoon` files ready work and asks for nothing. `spawn` and `submit`, which queue the
    // same way, both wake; this one is public, documented loop-thread-only like the two of them,
    // and is what an awaitable outside this module resumes through.
    // **Both declared before the loop, and the failing path is why.** The loop BORROWS this
    // frame: it holds the handle in `_ready` and does not own it. Declared after the loop, `flow`
    // would be destroyed first, and `~EventLoop` would then reach a frame that is already gone --
    // which is not hypothetical, it is what the first run of this case did, turning the red below
    // into a SIGSEGV that also stopped the two cases after it from running at all.
    auto ran = false;
    auto flow = markRan(&ran);
    auto clock = ManualClock {};
    auto host = ManualHostScheduler {};
    auto backend = HostDrivenBackend { host, clock };
    auto loop = EventLoop { backend, clock };

    REQUIRE(host.pendingCount() == 0);

    loop.resumeSoon(core::async::ParkedWork { .resume = flow.handle() });

    // Ready work means "as soon as you can", which on a host-driven loop is a deadline of now --
    // not no deadline at all, which is what it asks for today.
    REQUIRE(host.pendingCount() == 1);
    CHECK(soonestDelayMs(host) == 0);

    host.pump();
    CHECK(ran);
}

TEST_CASE("requestStop outside a turn asks a host-driven loop for the turn that unwinds a park "
          "nothing cancelled",
          "[EventLoop][hostdriven]")
{
    // **The narrow form, and the wording is the finding.** A `spawn`ed flow parked on `delay()`
    // does NOT need this: `spawn` gives the flow `_rootStop`'s token (`EventLoop.cpp:644`) and
    // `DelayAwaiter::await_suspend` registers a cancel on it (`EventLoop.hpp:812`), so
    // `_rootStop.request_stop()` reaches `requestCancel`, which wakes off-turn -- before
    // `unparkEverything()` runs at all. I wrote that case first and it passed; the wake was coming
    // from the cancellation, not from `requestStop`.
    //
    // What has no stop registration is a park filed through the public `registerPark` by hand,
    // which is what an awaitable outside this module does. `unparkEverything()` queues its waiter
    // into `_ready` and nothing tells the host, so the unwind waits for a turn that never comes.
    auto ran = false;
    auto flow = markRan(&ran); // borrowed by the loop, so declared before it
    auto clock = ManualClock {};
    auto host = ManualHostScheduler {};
    auto backend = HostDrivenBackend { host, clock };
    auto loop = EventLoop { backend, clock };

    auto const park = loop.registerPark(core::net::ParkEntry::onDeadline(
        core::async::ParkedWork { .resume = flow.handle() }, clock.now() + 1000ms));
    REQUIRE(static_cast<bool>(park));

    // **Isolated by pumping, not by `host.clear()`, and the first version got that wrong.**
    // Clearing drops the host's queue while leaving the BACKEND believing a pump is still
    // outstanding, so its coalescing then swallows the next request and the case measured the
    // desync rather than `requestStop`. A pump consumes the request on both sides, which is what
    // a real host does. After it, the loop is armed for the park's own deadline and nothing else.
    host.pump();
    REQUIRE(host.pendingCount() == 1);
    REQUIRE(soonestDelayMs(host) == 1000);

    loop.requestStop();

    // THE assertion. The unparked waiter is ready NOW, so the host must be asked for a turn now --
    // not left holding the one-second pump the park armed, which is what it would wait for.
    CHECK(soonestDelayMs(host) == 0);

    host.pump();
    CHECK(ran);
}

TEST_CASE("A host-driven loop asks for one pump however many wakes it takes", "[EventLoop][hostdriven]")
{
    // Coalescing, seen from the loop rather than the backend. A burst of posts from a worker — or
    // from inside the turn — must cost the page ONE scheduled callback, not one per post, or a
    // busy moment spends the frame budget in the scheduler.
    auto clock = ManualClock {};
    auto host = ManualHostScheduler {};
    auto backend = HostDrivenBackend { host, clock };
    auto loop = EventLoop { backend, clock };

    auto ran = 0;
    for ([[maybe_unused]] auto const index: { 0, 1, 2, 3, 4 })
        loop.post([&ran] { ++ran; });

    CHECK(host.pendingCount() == 1);

    host.pump();
    CHECK(ran == 5); // all five, in one turn
}

TEST_CASE("A destroyed host-driven loop leaves nothing for the host to call", "[EventLoop][hostdriven]")
{
    // The last step of the teardown. A host-driven backend holds a pointer to the loop it pumps,
    // and a host holds whatever the backend asked for; a pump that arrives after the loop is gone
    // would call into freed storage on the host's own thread, at a point where nothing in the
    // stack names this loop. The backend outlives the loop here on purpose, because that is the
    // arrangement that makes the mistake possible at all.
    auto clock = ManualClock {};
    auto host = ManualHostScheduler {};
    auto backend = HostDrivenBackend { host, clock };

    auto ran = 0;
    {
        auto loop = EventLoop { backend, clock };
        loop.post([&ran] { ++ran; });
        host.pump();
        REQUIRE(ran == 1);
    }

    // Whatever is still pending with the host may fire; it must find nothing to pump.
    auto const pumpsBefore = backend.pumpCount();
    host.pump();
    CHECK(ran == 1);
    CHECK(backend.pumpCount() >= pumpsBefore); // the backend still counts, it just has nobody to call
    CHECK_FALSE(backend.pumpScheduled());
}

TEST_CASE("run and blockOn are declared and compiled on every platform, host-driven or not",
          "[EventLoop][hostdriven]")
{
    // They are a PRECONDITION VIOLATION on a host-driven loop rather than something the build
    // removes, and that is deliberate: the browser reaches them through a consumer's mistake, not
    // through ours, so they must exist there to be asserted in. What runs them and watches the
    // assertion fire is `core-cpp.hostdriven-canary`, which cannot be a Catch case because the
    // failure is an abort.
    //
    // What this one holds is the other half: that both are instantiable in a build where the
    // default backend IS host-driven, which is the WebAssembly one. Neither is CALLED, because
    // calling them here would abort this binary.
    //
    // `run()` is a non-template member compiled into `EventLoop.cpp`, which is in the WebAssembly
    // FILE_SET, so a *requires*-expression is enough for it. `blockOn` is a header TEMPLATE, and
    // a *requires*-expression instantiates only its declaration -- the return type is explicit, so
    // the body is never touched, and a body that did not compile there would go unnoticed. Taking
    // its address odr-uses it, which instantiates the definition. Nothing else in this binary
    // calls it under Emscripten: the canary that does is `NOT EMSCRIPTEN`.
    STATIC_REQUIRE(requires(EventLoop& loop) { loop.run(); });
    auto const blockOnVoid =
        static_cast<void (EventLoop::*)(core::async::Task<void>)>(&EventLoop::blockOn<void>);
    CHECK(blockOnVoid != nullptr);
}

namespace
{

/// A flow that does nothing, for `spawn`: what is spawned does not matter, only that it is filed.
/// @return A task that completes at once.
Task<void> doNothing()
{
    co_return;
}

/// Parks on an absolute deadline from a flow nobody owns, inline at the call.
/// @param loop The loop to park on.
/// @param deadline When to resume.
core::async::DetachedTask sleepUntilDetached(EventLoop* loop, core::platform::SteadyTimePoint deadline)
{
    co_await loop->sleepUntil(deadline);
}

/// One quiescent host-driven loop, and what a member of the family needs to file work on it.
///
/// Member order is load-bearing: the loop BORROWS @c flow, so @c flow is declared before the loop
/// and outlives it, and the counters a loop-owned flow writes are declared before it for the same
/// reason.
struct WakeFixture
{
    bool ran = false;                          ///< Set by @c flow, if it ever runs.
    bool fired = false;                        ///< Set by a detached delay, if it ever resumes.
    std::size_t calls = 0;                     ///< Counted by a timer callback.
    Task<void> flow = markRan(&ran);           ///< A lazy frame for the members that take a handle.
    ManualClock clock {};                      ///< The loop's time.
    ManualHostScheduler host {};               ///< Records what the backend asks for.
    HostDrivenBackend backend { host, clock }; ///< The backend under the loop.
    EventLoop loop { backend, clock };         ///< The loop under test.
    core::net::ParkId park {};                 ///< What a setup step filed, for `requestCancel`.
};

/// A member of the family "files work that only a turn can reach", and what it must ask for.
struct FilesWorkRow
{
    std::string_view member;               ///< The member under test, for the section name.
    void (*setup)(WakeFixture&) = nullptr; ///< Anything the member needs filed first, or null.
    void (*act)(WakeFixture&) = nullptr;   ///< Calls the member once, off-turn.
    long long expectedDelayMs = 0;         ///< The delay it must ask the host for; -1 for nothing.
};

/// Files a deadline park by hand, the way an awaitable outside this module does.
/// @param f The fixture whose flow to park.
/// @param after How far past now its deadline is.
void parkFlowFor(WakeFixture& f, core::platform::SteadyDuration after)
{
    f.park = f.loop.registerPark(core::net::ParkEntry::onDeadline(
        core::async::ParkedWork { .resume = f.flow.handle() }, f.clock.now() + after));
}

/// The family, derived from `EventLoop.cpp` rather than from any list in prose -- four copies of
/// such a list were wrong before this table existed. Two idioms and one exception:
///
/// - **Ready work that carries no time WAKES**, which on this backend is `scheduleAt(now)`: a
///   delay of 0. `schedule` from off the loop's thread belongs here rather than with the arming
///   members: what it files is an INBOUND entry, and the turn draining it is ready now.
/// - **A park filed with a time ARMS** for that time, through the `armHostWake()` in
///   `registerPark`, and every deadline member inherits it from that one primitive.
/// - **`notifyHandleClosing` files work and asks for nothing**, soundly: the turn exchanges
///   `_closedParks` before its wait, and on this backend no park can be on a handle at all, which
///   `armHostWake` asserts. It is a row so that the exception is asserted rather than omitted.
///
/// `spawn` is called off-turn but UNOPPOSED -- nothing is driving -- because it is loop-thread-only
/// and asserts `teardownIsSerialisedWithDispatch()`. What the family shares is "the loop is asked
/// for a turn", not "safe from any thread".
constexpr auto FilesWorkRows = std::array {
    FilesWorkRow {
        .member = "post", .act = [](WakeFixture& f) { f.loop.post([] {}); }, .expectedDelayMs = 0 },
    FilesWorkRow { .member = "submit",
                   .act = [](WakeFixture& f) { f.loop.submit(f.flow.handle()); },
                   .expectedDelayMs = 0 },
    FilesWorkRow { .member = "schedule",
                   .act = [](WakeFixture& f) { f.loop.schedule(f.clock.now() + 50ms, f.flow.handle()); },
                   .expectedDelayMs = 0 },
    FilesWorkRow {
        .member = "spawn", .act = [](WakeFixture& f) { f.loop.spawn(doNothing()); }, .expectedDelayMs = 0 },
    FilesWorkRow { .member = "stop", .act = [](WakeFixture& f) { f.loop.stop(); }, .expectedDelayMs = 0 },
    FilesWorkRow {
        .member = "requestStop", .act = [](WakeFixture& f) { f.loop.requestStop(); }, .expectedDelayMs = 0 },
    FilesWorkRow { .member = "requestCancel",
                   .setup = [](WakeFixture& f) { parkFlowFor(f, 1000ms); },
                   .act = [](WakeFixture& f) { f.loop.requestCancel(f.park); },
                   .expectedDelayMs = 0 },
    FilesWorkRow {
        .member = "resumeSoon",
        .act =
            [](WakeFixture& f) { f.loop.resumeSoon(core::async::ParkedWork { .resume = f.flow.handle() }); },
        .expectedDelayMs = 0 },
    FilesWorkRow { .member = "registerPark",
                   .act = [](WakeFixture& f) { parkFlowFor(f, 50ms); },
                   .expectedDelayMs = 50 },
    FilesWorkRow {
        .member = "addTimer",
        .act =
            [](WakeFixture& f) { std::ignore = f.loop.addTimer(f.clock.now() + 50ms, &countCall, &f.calls); },
        .expectedDelayMs = 50 },
    FilesWorkRow { .member = "delay",
                   .act = [](WakeFixture& f) { delayThenFlagDetached(&f.loop, 50ms, &f.fired); },
                   .expectedDelayMs = 50 },
    FilesWorkRow { .member = "sleepUntil",
                   .act = [](WakeFixture& f) { sleepUntilDetached(&f.loop, f.clock.now() + 50ms); },
                   .expectedDelayMs = 50 },
    // A value-initialised handle is not `InvalidHandle`, so the call reaches its walk over the
    // parks; nothing is parked on it, and the OS is never asked about it.
    FilesWorkRow { .member = "notifyHandleClosing",
                   .act =
                       [](WakeFixture& f) {
                           f.loop.notifyHandleClosing(core::platform::NativeHandle {},
                                                      core::net::FdWakePolicy::Cancel);
                       },
                   .expectedDelayMs = -1 },
};

} // namespace

TEST_CASE("Every member that files work asks a quiescent host-driven loop for the turn that runs it",
          "[EventLoop][hostdriven]")
{
    // **When a function joins a family that all do X, "why does this one not do X" has to be
    // answered out loud -- and where the family is enumerable, by a table rather than a comment.**
    // A member that files work without asking the host for a turn leaves that work filed, correct
    // and never run: `armHostWake()` runs only at the END of a turn, and a quiescent host-driven
    // loop has none coming. `addTimer` shipped that way, and so did `registerPark`, `resumeSoon`
    // and `requestStop`, after a list claiming the family was complete.
    for (auto const& row: FilesWorkRows)
    {
        DYNAMIC_SECTION("member=" << row.member)
        {
            auto f = WakeFixture {};
            if (row.setup != nullptr)
            {
                row.setup(f);
                // Isolated by PUMPING, not by `host.clear()`: clearing leaves the backend believing
                // a pump is outstanding, and its coalescing would swallow the request under test.
                f.host.pump();
            }
            // Quiescent apart from what the setup itself armed, measured rather than assumed: a
            // pump pending from anything else buys the turn that would mask the member.
            auto const before = f.host.pendingCount();
            auto const armedBefore = soonestDelayMs(f.host);
            REQUIRE(armedBefore != 0);

            row.act(f);

            if (row.expectedDelayMs < 0)
            {
                CHECK(f.host.pendingCount() == before);
                CHECK(soonestDelayMs(f.host) == armedBefore);
            }
            else
            {
                // THE assertion, and the delay rather than merely "something was asked": a
                // deadline member that woke would spin the host for fifty milliseconds, and a
                // ready-work member that armed would leave its work waiting on an unrelated one.
                REQUIRE(f.host.pendingCount() >= 1);
                CHECK(soonestDelayMs(f.host) == row.expectedDelayMs);
            }
        }
    }
}

TEST_CASE("A host-driven loop and its backend may be destroyed while a pump is out with the host",
          "[EventLoop][hostdriven]")
{
    // The browser's timer cannot be retracted: a pump the backend asked for arrives whether or not
    // the backend still exists. `~EventLoop` unregisters its pump and arms nothing, and
    // `PlatformLoop` then frees the backend it owns -- so the late pump used to write the backend's
    // schedule into freed storage, a heap-use-after-free on the host's next turn. Found by morph,
    // whose timeout scheduler destroys its loop with a deadline armed. The host outlives both, as
    // `IHostScheduler` requires, and fires what is left afterwards; under ASan the defect is a
    // report here, and without it the case still proves the late pump runs nothing.
    auto clock = ManualClock {};
    auto host = ManualHostScheduler {};
    auto fired = std::size_t { 0 };
    auto backend = std::make_unique<HostDrivenBackend>(host, clock);
    auto loop = std::make_unique<EventLoop>(*backend, clock);

    SECTION("a timer added off-turn and cancelled: the wake it asked for is still out")
    {
        auto const id = loop->addTimer(clock.now() + 30s, &countCall, &fired);
        std::ignore = loop->cancelTimer(id);
    }

    SECTION("a timer still armed: the deadline's pump is out as well")
    {
        std::ignore = loop->addTimer(clock.now() + 30s, &countCall, &fired);
        host.pump(); // the wake's turn, which arms the host at the deadline
    }

    REQUIRE(host.pendingCount() >= 1);
    loop.reset();
    backend.reset(); // what `PlatformLoop` does next: the backend it owns goes with it

    clock.advance(1min);
    host.pump();
    CHECK(fired == 0);
    CHECK(host.pendingCount() == 0);
}
