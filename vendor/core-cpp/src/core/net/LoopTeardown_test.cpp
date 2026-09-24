// SPDX-License-Identifier: Apache-2.0
//
// What a loop owes the coroutines it is holding when it goes away
// ([fastcached#1025](https://github.com/LASTRADA-Software/fastcached/issues/1025)).
//
// A reactor stopped with work parked resumed none of it and freed none of it: the stop set a
// flag, the loop returned, and the deadline heap and submit queue were destroyed as containers of
// non-owning handles. Every frame in them, and everything reachable from it, leaked -- reported by
// LeakSanitizer as an INDIRECT-ONLY set, which is what a `Task` chain looks like when it refers to
// itself through its own continuations.
//
// **These cases do not rely on a sanitizer noticing.** A leak reported only by LSan is a red once
// in N runs and reads as a flake; every case here counts destructions with a sentinel and asserts
// the number, so it fails for its own reason on every platform.
//
// **Both directions, and never both at once.** Freeing everything at teardown is as wrong as
// freeing nothing: a deadline that arrives must be RESUMED, and a handle whose frame something
// else owns must be LEFT ALONE. That second case is not hypothetical -- a caller commonly submits
// a `Task` local it still holds -- and a loop that freed what it merely borrows would double free
// there. It is `workSomebodyElseOwnsIsLeftAlone` below, and it is why the answer to *destroy or
// resume* is neither on its own: the loop frees exactly the chains
// `core::async::detail::parkedWorkFor` says nothing owns.
//
// **Every property runs on every backend this platform builds**, over `testing::BackendMatrix`,
// plus the deterministic double. The thing that stops a new backend inheriting the defect is not
// this file: `submit(ParkedWork)` and `schedule(TimePoint, ParkedWork)` are pure virtual on
// `async::IExecutor`, so a new executor is handed the question rather than left not knowing about
// it. What this file holds is the LOOP's answer, once, for all of them.
#include <core/async/Cancellation.hpp>
#include <core/async/DetachedTask.hpp>
#include <core/async/ParkedWork.hpp>
#include <core/async/ResumeOn.hpp>
#include <core/async/Task.hpp>
#include <core/net/EventLoop.hpp>
#include <core/net/IoBackend.hpp>
#include <core/net/testing/BackendMatrix.hpp>
#include <core/net/testing/TestLoop.hpp>
#include <core/platform/Clock.hpp>
#include <core/platform/SystemPipe.hpp>

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <coroutine>
#include <memory>
#include <optional>
#include <ranges>
#include <tuple>
#include <utility>

using core::async::DetachedTask;
using core::async::Task;
using core::net::EventLoop;
using core::platform::ManualClock;
using namespace std::chrono_literals;

namespace
{

/// What each case counts.
struct Counters
{
    int parked = 0;    ///< Coroutines that reached their suspend point.
    int completed = 0; ///< Coroutine bodies that ran to their end.
    int destroyed = 0; ///< Frames freed, counted by the sentinel each carries.
    int reentered = 0; ///< Times a dying frame called back into the loop.

    /// What the re-entrant destructor below saw when it asked the loop a question while being
    /// freed. Both are read from containers a single-pass teardown would already have destroyed.
    bool cancelAnswered = true;
    std::size_t parkedAtFree = 1;
};

/// A coroutine-frame sentinel: one per frame under test, counted when the frame dies.
///
/// Passed BY VALUE into every coroutine here, which is both this project's coroutine rule and what
/// puts it in the FRAME -- a body local would not exist in a lazy `Task` that has never started,
/// and one of these cases is precisely about such a task. Move-aware, so the caller's temporary
/// being destroyed at the end of the call expression does not count as the frame dying.
class FrameSentinel
{
  public:
    /// @param counters Where the destruction is tallied; never null.
    explicit FrameSentinel(Counters* counters) noexcept: _counters { counters } {}

    FrameSentinel(FrameSentinel&& other) noexcept: _counters { std::exchange(other._counters, nullptr) } {}

    FrameSentinel(FrameSentinel const&) = delete;
    FrameSentinel& operator=(FrameSentinel const&) = delete;
    FrameSentinel& operator=(FrameSentinel&&) = delete;

    ~FrameSentinel()
    {
        if (_counters != nullptr)
            ++_counters->destroyed;
    }

  private:
    Counters* _counters;
};

/// A detached chain that parks on a deadline nothing will ever reach.
/// @param loop The loop to park on.
/// @param sentinel Counted when this frame dies.
/// @param counters Where the progress is recorded.
DetachedTask parkOnDeadlineForever(EventLoop* loop, FrameSentinel sentinel, Counters* counters)
{
    static_cast<void>(sentinel);
    ++counters->parked;
    co_await loop->sleepUntil(loop->clock().now() + 1h);
    ++counters->completed;
    co_return;
}

/// A detached chain that parks on a deadline that DOES arrive.
/// @param loop The loop to park on.
/// @param sentinel Counted when this frame dies.
/// @param counters Where the progress is recorded.
/// @param delay How far out the deadline is.
DetachedTask parkOnDeadlineBriefly(EventLoop* loop,
                                   FrameSentinel sentinel,
                                   Counters* counters,
                                   core::platform::SteadyDuration delay)
{
    static_cast<void>(sentinel);
    ++counters->parked;
    co_await loop->sleepUntil(loop->clock().now() + delay);
    ++counters->completed;
    co_return;
}

/// The innermost frame of a nested chain: it is what the loop actually holds.
/// @param loop The loop to park on.
/// @param sentinel Counted when this frame dies.
/// @param counters Where the progress is recorded.
Task<void> parkInner(EventLoop* loop, FrameSentinel sentinel, Counters* counters)
{
    static_cast<void>(sentinel);
    ++counters->parked;
    co_await loop->sleepUntil(loop->clock().now() + 1h);
    co_return;
}

/// The middle frame: it owns `parkInner`'s frame through its awaiter and is itself owned by the
/// root's.
/// @param loop The loop the chain parks on.
/// @param sentinel Counted when this frame dies.
/// @param counters Where the progress is recorded.
Task<void> parkMiddle(EventLoop* loop, FrameSentinel sentinel, Counters* counters)
{
    static_cast<void>(sentinel);
    co_await parkInner(loop, FrameSentinel { counters }, counters);
    co_return;
}

/// A detached chain three frames deep, the shape #1025 was reported on.
/// @param loop The loop the chain parks on.
/// @param sentinel Counted when this frame dies.
/// @param counters Where the progress is recorded.
DetachedTask parkNested(EventLoop* loop, FrameSentinel sentinel, Counters* counters)
{
    static_cast<void>(sentinel);
    co_await parkMiddle(loop, FrameSentinel { counters }, counters);
    ++counters->completed;
    co_return;
}

/// A detached chain parked on the SUBMIT side rather than the deadline heap.
/// @param loop The loop to hand itself to.
/// @param sentinel Counted when this frame dies.
/// @param counters Where the progress is recorded.
DetachedTask parkOnSubmit(EventLoop* loop, FrameSentinel sentinel, Counters* counters)
{
    static_cast<void>(sentinel);
    ++counters->parked;
    co_await core::async::ResumeOn { *loop };
    ++counters->completed;
    co_return;
}

/// A frame member that asks the loop to take a handle back as it dies.
///
/// It stands in for a deadline timer, whose destructor disarms itself through
/// @c EventLoop::cancelPending. That re-entry is what makes the ORDER of a loop's own teardown
/// observable: `cancelPending` searches the ready queue AND the park table, so a chain freed after
/// one of them has been destroyed reads freed storage.
///
/// The handle it cancels is deliberately one the loop never had -- a miss is what forces BOTH
/// containers to be searched, where a hit would stop at the first.
class ReentrantDisarm
{
  public:
    /// @param loop Asked to cancel as this dies; never null.
    /// @param handle A live handle this loop does not hold, so the search misses.
    /// @param counters Where the re-entry is recorded.
    ReentrantDisarm(EventLoop* loop, std::coroutine_handle<> handle, Counters* counters) noexcept:
        _loop { loop }, _handle { handle }, _counters { counters }
    {
    }

    ReentrantDisarm(ReentrantDisarm&& other) noexcept:
        _loop { std::exchange(other._loop, nullptr) },
        _handle { other._handle },
        _counters { other._counters }
    {
    }

    ReentrantDisarm(ReentrantDisarm const&) = delete;
    ReentrantDisarm& operator=(ReentrantDisarm const&) = delete;
    ReentrantDisarm& operator=(ReentrantDisarm&&) = delete;

    ~ReentrantDisarm()
    {
        if (_loop == nullptr)
            return;
        // Two questions, both answered out of containers a single-pass teardown would already
        // have let die. Recorded rather than only asked, so the case fails for its own reason
        // where a sanitizer is not watching.
        // An empty handle is answered `false` without a search, so it asks nothing of the
        // containers; a live one the loop never had is what forces BOTH of them to be walked.
        _counters->cancelAnswered = _loop->cancelPending(_handle);
        _counters->parkedAtFree = _loop->parkedWaiterCount() + _loop->pendingTimerCount();
        ++_counters->reentered;
    }

  private:
    EventLoop* _loop;
    std::coroutine_handle<> _handle;
    Counters* _counters;
};

/// A frame member that parks a NEW chain on the loop as it dies, exactly once.
///
/// This is what makes the abandon step's FIXPOINT observable. A single pass frees what the loop
/// was holding and returns; anything that parked during that pass is then left to MEMBER
/// destruction, where the park table is already being destroyed -- and the new chain's own
/// re-entrant disarm reads it. With the fixpoint, the pass that created it is followed by another
/// that frees it while every container is still whole.
class ReparkOnce
{
  public:
    /// @param loop The loop to park on as this dies; never null.
    /// @param counters Where the new chain's progress is recorded.
    /// @param armed Cleared by the first of these to fire, so the re-parking does not recurse.
    ReparkOnce(EventLoop* loop, Counters* counters, bool* armed) noexcept:
        _loop { loop }, _counters { counters }, _armed { armed }
    {
    }

    ReparkOnce(ReparkOnce&& other) noexcept:
        _loop { std::exchange(other._loop, nullptr) }, _counters { other._counters }, _armed { other._armed }
    {
    }

    ReparkOnce(ReparkOnce const&) = delete;
    ReparkOnce& operator=(ReparkOnce const&) = delete;
    ReparkOnce& operator=(ReparkOnce&&) = delete;

    ~ReparkOnce();

  private:
    EventLoop* _loop;
    Counters* _counters;
    bool* _armed;
};

/// A detached chain parked on the submit side whose frame re-enters the loop as it is freed.
/// @param loop The loop to hand itself to, and the one the disarm re-enters.
/// @param sentinel Counted when this frame dies.
/// @param disarm Runs `cancelPending` on this loop as this frame dies.
/// @param repark Parks a NEW chain on this loop as this frame dies, exactly once.
/// @param counters Where the progress is recorded.
DetachedTask parkOnSubmitWithDisarm(
    EventLoop* loop, FrameSentinel sentinel, ReentrantDisarm disarm, ReparkOnce repark, Counters* counters)
{
    static_cast<void>(sentinel);
    static_cast<void>(disarm);
    static_cast<void>(repark);
    ++counters->parked;
    co_await core::async::ResumeOn { *loop };
    ++counters->completed;
    co_return;
}

/// The chain `ReparkOnce` starts as it dies: it parks on a deadline nothing reaches, and holds a
/// re-entrant disarm of its own, so freeing IT asks the loop a question too.
/// @param loop The loop to park on.
/// @param sentinel Counted when this frame dies.
/// @param disarm Runs `cancelPending` on this loop as this frame dies.
/// @param counters Where the progress is recorded.
DetachedTask reparkedChain(EventLoop* loop,
                           FrameSentinel sentinel,
                           ReentrantDisarm disarm,
                           Counters* counters)
{
    static_cast<void>(sentinel);
    static_cast<void>(disarm);
    ++counters->parked;
    co_await loop->sleepUntil(loop->clock().now() + 1h);
    ++counters->completed;
    co_return;
}

ReparkOnce::~ReparkOnce()
{
    if (_loop == nullptr || !*_armed)
        return;
    *_armed = false;
    reparkedChain(_loop, FrameSentinel { _counters }, ReentrantDisarm { _loop, {}, _counters }, _counters);
}

/// A lazy task carrying a sentinel, for the cases that need a frame rather than a chain.
/// @param sentinel Counted when this frame dies.
/// @param counters Where the progress is recorded.
Task<void> immediateWithSentinel(FrameSentinel sentinel, Counters* counters)
{
    static_cast<void>(sentinel);
    ++counters->completed;
    co_return;
}

/// The deterministic double, driven by hand.
struct ManualDriver
{
    /// @param kind Ignored: this driver has no backend to choose. Taken so that one template body
    ///        can construct either driver.
    explicit ManualDriver(core::net::BackendKind /*kind*/) noexcept {}

    ManualClock clock;
    core::net::testing::TestLoop loop { clock };

    /// @return The loop under test.
    [[nodiscard]] EventLoop& eventLoop() noexcept { return loop; }

    /// Turn the loop, letting the clock reach @p window, until @p predicate holds.
    /// @tparam Predicate What the caller is waiting for.
    /// @param window How far the deadline under test is out.
    /// @param predicate What the caller is waiting for.
    /// @return Whether it became true.
    template <typename Predicate>
    [[nodiscard]] bool runUntil(core::platform::SteadyDuration window, Predicate predicate)
    {
        // Stepped rather than jumped so a wait that polls in sub-steps still makes progress, and
        // bounded so a regression fails rather than hangs.
        for ([[maybe_unused]] auto const step: std::views::iota(0, 100))
        {
            if (predicate())
                break;
            clock.advance((window / 10) + 1ms);
            std::ignore = loop.drain();
        }
        return predicate();
    }
};

/// A real loop over one of this platform's backends, driven a turn at a time on this thread.
///
/// On this thread rather than on a worker of its own, and that is not a shortcut: every assertion
/// here is about what the loop's DESTRUCTOR does, and a destructor that has to be serialised with
/// a worker thread is one whose teardown rule (G5) the case would be exercising instead.
class BackendDriver
{
  public:
    /// @param kind Which backend to build.
    explicit BackendDriver(core::net::BackendKind kind):
        _backend { core::net::makeBackend(kind) }, _loop { std::make_unique<EventLoop>(*_backend, _clock) }
    {
    }

    /// @return The loop under test.
    [[nodiscard]] EventLoop& eventLoop() noexcept { return *_loop; }

    /// Turn the loop until @p predicate holds, bounded in real time.
    /// @tparam Predicate What the caller is waiting for.
    /// @param window How far the deadline under test is out.
    /// @param predicate What the caller is waiting for.
    /// @return Whether it became true inside the bound.
    template <typename Predicate>
    [[nodiscard]] bool runUntil(core::platform::SteadyDuration window, Predicate predicate)
    {
        // Generous rather than tuned: what is being waited for is one deadline on an idle loop,
        // so a slow runner is the only thing that can make this long. Bounded, and it says what it
        // waited for through the assertion at the call site.
        auto const bound = _clock.now() + (window * 200) + 10s;
        while (!predicate() && _clock.now() < bound)
            std::ignore = _loop->runOnce(std::optional { core::platform::SteadyDuration { 1ms } });
        return predicate();
    }

  private:
    core::platform::SteadyClock _clock;
    std::unique_ptr<core::net::IoBackend> _backend;
    /// By pointer, because an `EventLoop` is immovable and must be built after the backend it
    /// borrows.
    std::unique_ptr<EventLoop> _loop;
};

/// A coroutine parked on a deadline when the loop goes away is freed exactly once.
/// @param kind Which backend the driver should build.
/// @tparam Driver Which loop to exercise.
template <typename Driver>
void abandonedDeadlineIsFreedExactlyOnce(core::net::BackendKind kind)
{
    auto counters = Counters {};
    {
        Driver driver { kind };
        parkOnDeadlineForever(&driver.eventLoop(), FrameSentinel { &counters }, &counters);

        // Parked, asserted rather than assumed: a DetachedTask runs eagerly to its first
        // suspension, and a body that had NOT suspended would have completed.
        REQUIRE(counters.parked == 1);
        REQUIRE(counters.completed == 0);
        REQUIRE(counters.destroyed == 0);
        REQUIRE(driver.eventLoop().pendingTimerCount() == 1);
    }

    CHECK(counters.destroyed == 1);
    // Freed rather than resumed: nothing ran the rest of the body, which is the whole reason this
    // is safe to do at teardown.
    CHECK(counters.completed == 0);
}

/// A deadline that arrives is resumed, and is not then freed a second time.
/// @param kind Which backend the driver should build.
/// @tparam Driver Which loop to exercise.
template <typename Driver>
void anArrivedDeadlineIsResumedAndNotFreedTwice(core::net::BackendKind kind)
{
    auto counters = Counters {};
    {
        Driver driver { kind };
        parkOnDeadlineBriefly(&driver.eventLoop(), FrameSentinel { &counters }, &counters, 20ms);
        REQUIRE(counters.parked == 1);
        REQUIRE(counters.completed == 0);

        REQUIRE(driver.runUntil(20ms, [&counters] { return counters.completed == 1; }));

        // Resumed, ran to its end, and a DetachedTask frees its own frame there.
        CHECK(counters.destroyed == 1);
    }

    // And the loop did not free it again on the way out. Without this half the teardown could free
    // everything it holds and every other case would still pass.
    CHECK(counters.destroyed == 1);
    CHECK(counters.completed == 1);
}

/// An abandoned chain is freed from its ROOT, so every frame in it goes.
/// @param kind Which backend the driver should build.
/// @tparam Driver Which loop to exercise.
template <typename Driver>
void anAbandonedChainIsFreedFromItsRoot(core::net::BackendKind kind)
{
    auto counters = Counters {};
    {
        Driver driver { kind };
        parkNested(&driver.eventLoop(), FrameSentinel { &counters }, &counters);
        REQUIRE(counters.parked == 1);
        REQUIRE(counters.destroyed == 0);
    }

    // Three frames -- the detached root, the task it awaits, the task that parks -- each freed
    // once. Freeing the frame the loop HOLDS would give one: the two above it are reachable only
    // through each other, which is exactly the indirect-only leak set #1025 was reported with.
    CHECK(counters.destroyed == 3);
    CHECK(counters.completed == 0);
}

/// The submit side is covered too, not only the deadline heap.
/// @param kind Which backend the driver should build.
/// @tparam Driver Which loop to exercise.
template <typename Driver>
void anAbandonedSubmissionIsFreedExactlyOnce(core::net::BackendKind kind)
{
    auto counters = Counters {};
    {
        // Never driven, so the queued resumption is never dequeued -- which is the state a surface
        // posting its own shutdown onto a stopping loop is in.
        Driver driver { kind };
        parkOnSubmit(&driver.eventLoop(), FrameSentinel { &counters }, &counters);
        REQUIRE(counters.parked == 1);
        REQUIRE(counters.completed == 0);
        REQUIRE(counters.destroyed == 0);
    }

    CHECK(counters.destroyed == 1);
    CHECK(counters.completed == 0);
}

/// A submission that IS dequeued is resumed, and is not then freed a second time.
/// @param kind Which backend the driver should build.
/// @tparam Driver Which loop to exercise.
template <typename Driver>
void aResumedSubmissionIsNotFreedTwice(core::net::BackendKind kind)
{
    auto counters = Counters {};
    {
        Driver driver { kind };
        parkOnSubmit(&driver.eventLoop(), FrameSentinel { &counters }, &counters);
        REQUIRE(counters.parked == 1);
        REQUIRE(counters.completed == 0);

        REQUIRE(driver.runUntil(1ms, [&counters] { return counters.completed == 1; }));
        CHECK(counters.destroyed == 1);
    }

    CHECK(counters.destroyed == 1);
    CHECK(counters.completed == 1);
}

/// Work whose frame something else owns is left alone, however long it sits there.
/// @param kind Which backend the driver should build.
/// @tparam Driver Which loop to exercise.
template <typename Driver>
void workSomebodyElseOwnsIsLeftAlone(core::net::BackendKind kind)
{
    auto counters = Counters {};
    {
        // A lazy task, never started: its frame and its by-value parameters exist, and the `Task`
        // object is what frees them.
        auto owned = immediateWithSentinel(FrameSentinel { &counters }, &counters);
        {
            Driver driver { kind };
            driver.eventLoop().submit(owned.handle());
        }

        // The loop was destroyed holding this handle and did not touch it. A teardown that freed
        // what it merely borrows would have freed it here, and the line below would then be a
        // use-after-free rather than a failed check.
        CHECK(counters.destroyed == 0);
        CHECK(counters.completed == 0);
    }

    CHECK(counters.destroyed == 1);
}

} // namespace

TEST_CASE("A coroutine parked on a loop's deadline is freed exactly once", "[EventLoop][teardown]")
{
    SECTION("on the test double")
    {
        abandonedDeadlineIsFreedExactlyOnce<ManualDriver>(core::net::BackendKind::Null);
    }
    for (auto const& entry: core::net::testing::BackendMatrix)
    {
        if (!core::net::makeBackend(entry.kind))
            continue; // not built on this platform
        DYNAMIC_SECTION("backend=" << entry.name)
        {
            abandonedDeadlineIsFreedExactlyOnce<BackendDriver>(entry.kind);
        }
    }
}

TEST_CASE("A loop deadline that arrives is resumed rather than freed", "[EventLoop][teardown]")
{
    SECTION("on the test double")
    {
        anArrivedDeadlineIsResumedAndNotFreedTwice<ManualDriver>(core::net::BackendKind::Null);
    }
    for (auto const& entry: core::net::testing::BackendMatrix)
    {
        if (!core::net::makeBackend(entry.kind))
            continue;
        DYNAMIC_SECTION("backend=" << entry.name)
        {
            anArrivedDeadlineIsResumedAndNotFreedTwice<BackendDriver>(entry.kind);
        }
    }
}

TEST_CASE("An abandoned await chain is freed from its root rather than the frame parked",
          "[EventLoop][teardown]")
{
    SECTION("on the test double")
    {
        anAbandonedChainIsFreedFromItsRoot<ManualDriver>(core::net::BackendKind::Null);
    }
    for (auto const& entry: core::net::testing::BackendMatrix)
    {
        if (!core::net::makeBackend(entry.kind))
            continue;
        DYNAMIC_SECTION("backend=" << entry.name)
        {
            anAbandonedChainIsFreedFromItsRoot<BackendDriver>(entry.kind);
        }
    }
}

TEST_CASE("A coroutine parked on a loop submission is freed exactly once", "[EventLoop][teardown]")
{
    SECTION("on the test double")
    {
        anAbandonedSubmissionIsFreedExactlyOnce<ManualDriver>(core::net::BackendKind::Null);
    }
    for (auto const& entry: core::net::testing::BackendMatrix)
    {
        if (!core::net::makeBackend(entry.kind))
            continue;
        DYNAMIC_SECTION("backend=" << entry.name)
        {
            anAbandonedSubmissionIsFreedExactlyOnce<BackendDriver>(entry.kind);
        }
    }
}

TEST_CASE("A loop submission that is dequeued is resumed rather than freed", "[EventLoop][teardown]")
{
    SECTION("on the test double")
    {
        aResumedSubmissionIsNotFreedTwice<ManualDriver>(core::net::BackendKind::Null);
    }
    for (auto const& entry: core::net::testing::BackendMatrix)
    {
        if (!core::net::makeBackend(entry.kind))
            continue;
        DYNAMIC_SECTION("backend=" << entry.name)
        {
            aResumedSubmissionIsNotFreedTwice<BackendDriver>(entry.kind);
        }
    }
}

TEST_CASE("A loop leaves parked work whose frame something else owns alone", "[EventLoop][teardown]")
{
    SECTION("on the test double")
    {
        workSomebodyElseOwnsIsLeftAlone<ManualDriver>(core::net::BackendKind::Null);
    }
    for (auto const& entry: core::net::testing::BackendMatrix)
    {
        if (!core::net::makeBackend(entry.kind))
            continue;
        DYNAMIC_SECTION("backend=" << entry.name)
        {
            workSomebodyElseOwnsIsLeftAlone<BackendDriver>(entry.kind);
        }
    }
}

TEST_CASE("A loop frees parked chains while all of its containers are still alive", "[EventLoop][teardown]")
{
    // **This is the abandon FIXPOINT, and the reason it is a loop rather than a pass.** Freeing
    // during member destruction is not the same as freeing in the destructor body: members die in
    // reverse declaration order, so one parked container outlives the other -- and freeing a chain
    // re-enters the loop, because a frame holding a deadline runs its disarm into `cancelPending`,
    // which searches BOTH. A chain freed from the later container therefore reads storage the
    // earlier one has already released.
    //
    // Asserted two ways, because neither alone is enough. The VALUES below are read by the dying
    // frame out of the loop's own containers, so a teardown that let them die first answers with
    // whatever the freed storage happens to hold; and `reentered` is what stops the case passing
    // for the wrong reason, since a destructor that never ran asserts nothing. Under ASan the
    // read itself is the failure, which is the form CI catches it in.
    auto counters = Counters {};

    // Owned by this test and never given to the loop, so `cancelPending` MISSES and is forced to
    // search both containers rather than stopping at the first.
    auto absent = immediateWithSentinel(FrameSentinel { &counters }, &counters);

    // Declared BEFORE the driver, so it outlives the teardown that reads it: `ReparkOnce` fires
    // from inside ~EventLoop, which is the driver's destructor, and a flag declared after the
    // driver is already gone by then.
    auto armed = true;

    {
        ManualDriver driver { core::net::BackendKind::Null };

        // A plain abandoned deadline, so the park table holds a real allocation. An empty
        // container has no buffer to read, and the defect would then be invisible.
        parkOnDeadlineForever(&driver.eventLoop(), FrameSentinel { &counters }, &counters);

        // And the chain that re-enters, parked on the SUBMIT side. Its `ReparkOnce` member is
        // what makes the fixpoint matter: freeing this chain starts another one, which a single
        // pass would leave for member destruction.
        parkOnSubmitWithDisarm(&driver.eventLoop(),
                               FrameSentinel { &counters },
                               ReentrantDisarm { &driver.eventLoop(), absent.handle(), &counters },
                               ReparkOnce { &driver.eventLoop(), &counters, &armed },
                               &counters);

        REQUIRE(counters.parked == 2);
        REQUIRE(counters.reentered == 0);
    }

    // Three chains freed -- the deadline park, the submission, and the one the submission's
    // destructor parked -- and the re-entrant destructors both ran, so both searches happened
    // against containers this loop had not yet let die.
    CHECK(counters.destroyed == 3);
    CHECK(counters.parked == 3);
    CHECK(counters.reentered == 2);
    CHECK(counters.completed == 0);
    CHECK_FALSE(counters.cancelAnswered); // a handle the loop never held is not there to take back
    CHECK(counters.parkedAtFree == 0);    // and the tables answered, rather than crashed
}

// The primitive underneath these cases -- `detail::Parked::resume()` freeing a chain it declines
// to resume, two claims on one chain freeing it once between them, `unownedRootOf`'s answer for
// each shape of chain -- is held by `src/core/async/ParkedWork_test.cpp`, where the type lives. No
// loop can reach those branches, so a case routed through one here would assert their
// unreachability rather than the rule.

namespace
{

/// A detached chain parked on readiness that never arrives -- the loop's third kind of park, and
/// the one that also holds a registration the backend knows only by address.
/// @param loop The loop to park on.
/// @param handle The handle to watch; nothing ever writes to it.
/// @param sentinel Counted when this frame dies.
/// @param counters Where the progress is recorded.
DetachedTask parkOnReadinessForever(EventLoop* loop,
                                    core::platform::NativeHandle handle,
                                    FrameSentinel sentinel,
                                    Counters* counters)
{
    static_cast<void>(sentinel);
    ++counters->parked;
    co_await loop->waitReadable(handle);
    ++counters->completed;
    co_return;
}

/// A flow whose frame the CALLER owns, parked on readiness that never arrives.
///
/// The other half of the pair: it inherits the loop's root stop token, so teardown resumes it and
/// it unwinds here rather than being freed.
/// @param loop The loop to park on.
/// @param handle The handle to watch; nothing ever writes to it.
/// @param sentinel Counted when this frame dies.
/// @param counters Where the progress is recorded.
Task<void> ownedParkOnReadiness(EventLoop* loop,
                                core::platform::NativeHandle handle,
                                FrameSentinel sentinel,
                                Counters* counters)
{
    static_cast<void>(sentinel);
    ++counters->parked;
    try
    {
        co_await loop->waitReadable(handle);
        ++counters->completed;
    }
    catch (core::async::OperationCancelled const&)
    {
        ++counters->reentered;
    }
}

} // namespace

TEST_CASE("At teardown a readiness park is freed if the loop owns it and unwound if it does not",
          "[EventLoop][teardown]")
{
    // **The two halves of the teardown rule, on the kind of park the deadline cases do not reach.**
    //
    // A readiness park carries something a deadline park does not: a registration the BACKEND
    // holds by address. Teardown has to detach it before the park's handler is freed, whichever
    // half the park falls into, or the backend is left walking dead storage -- which is what ASan
    // reports here and what the parked-waiter count asserts before the loop goes.
    //
    // The halves themselves: the DETACHED chain belongs to nobody, carries no stop token (a
    // detached flow has no awaiting coroutine to inherit one from), and so cannot be told to
    // unwind -- resuming it would simply run the rest of its body on a loop that is being
    // destroyed. It is freed. The SPAWNED flow's frame belongs to the loop's root list and its
    // token is the loop's, so it is resumed, observes the stop, and unwinds through its own
    // `catch` with its RAII cleanup run.
    for (auto const& entry: core::net::testing::BackendMatrix)
    {
        if (!core::net::makeBackend(entry.kind))
            continue;

        DYNAMIC_SECTION("backend=" << entry.name)
        {
            auto pipe = core::platform::createSystemPipe();
            REQUIRE(pipe.has_value());

            auto detached = Counters {};
            auto owned = Counters {};
            {
                BackendDriver driver { entry.kind };
                parkOnReadinessForever(
                    &driver.eventLoop(), (*pipe)->readFd(), FrameSentinel { &detached }, &detached);
                driver.eventLoop().spawn(ownedParkOnReadiness(
                    &driver.eventLoop(), (*pipe)->readFd(), FrameSentinel { &owned }, &owned));
                REQUIRE(driver.runUntil(1ms, [&owned] { return owned.parked == 1; }));

                REQUIRE(detached.parked == 1);
                REQUIRE(driver.eventLoop().parkedWaiterCount() == 2);
                REQUIRE(detached.destroyed == 0);
                REQUIRE(owned.destroyed == 0);
            }

            // The detached chain: freed, never run on.
            CHECK(detached.destroyed == 1);
            CHECK(detached.completed == 0);

            // The spawned flow: resumed, unwound through OperationCancelled, freed once.
            CHECK(owned.reentered == 1);
            CHECK(owned.completed == 0);
            CHECK(owned.destroyed == 1);
        }
    }
}
