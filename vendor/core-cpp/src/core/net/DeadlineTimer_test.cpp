// SPDX-License-Identifier: Apache-2.0
//
// `DeadlineTimer` — a deadline that can be disarmed, over `EventLoop::addTimer`.
//
// Ported from fastcached's `Async/DeadlineTimer_test.cpp` at `0708dd54`, **without its
// poll-interval cases**: upstream's timer could not take a deadline back off the reactor, so it
// slept in steps of 50ms and re-read a flag. Here `cancelTimer` retires the park by id, so the
// only wake-up an armed timer causes is the one at its deadline. `anArmedTimerBoundsTheWait`
// below is what says so, and it is the case upstream could not have written.

#include <core/net/DeadlineTimer.hpp>
#include <core/net/EventLoop.hpp>
#include <core/net/testing/ScriptedBackend.hpp>
#include <core/net/testing/TestLoop.hpp>
#include <core/platform/Clock.hpp>

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <cstddef>
#include <memory>
#include <optional>
#include <tuple>
#include <type_traits>

using core::net::DeadlineTimer;
using core::net::EventLoop;
using core::net::TimerId;
using core::net::testing::ScriptedBackend;
using core::net::testing::TestLoop;
using core::platform::ManualClock;
using namespace std::chrono_literals;

namespace
{

/// The members @c DeadlineTimer holds, in declaration order, as a plain aggregate.
///
/// **This is what makes "allocates nothing and holds no coroutine frame" a checked claim rather
/// than a sentence in a report.** The report called a `sizeof` assertion fragile, and for an
/// EQUALITY it is: padding, member reordering and a platform with a different pointer size each
/// break it without anything being wrong. An upper bound against a struct with the SAME members
/// has none of those failure modes -- both types get the same layout rules on every target, so
/// the bound holds on 32- and 64-bit alike -- and it breaks on exactly one thing, a sixth member
/// appearing. A coroutine frame could only arrive as a `unique_ptr`, a `shared_ptr` or a
/// `std::function`, and every one of those is at least a pointer wide, so none of them fits.
///
/// It also pins the count. The report's prose said "four members, none owning" while the header
/// declared five, which is the failure the claims table exists to catch: this struct cannot
/// disagree with the header without the assertion below going red.
struct DeadlineTimerShape
{
    EventLoop* loop = nullptr;                   ///< @c _loop
    DeadlineTimer::Callback onExpired = nullptr; ///< @c _onExpired
    void* state = nullptr;                       ///< @c _state
    TimerId timer {};                            ///< @c _timer
    bool settled = false;                        ///< @c _settled
};

static_assert(sizeof(DeadlineTimer) <= sizeof(DeadlineTimerShape),
              "DeadlineTimer has grown state beyond the five members it documents. It is "
              "specified to allocate nothing and to hold no coroutine frame, and every way of "
              "holding one is at least a pointer wide.");
static_assert(alignof(DeadlineTimer) <= alignof(DeadlineTimerShape),
              "DeadlineTimer gained a member with a stricter alignment than any it documents.");
static_assert(!std::is_polymorphic_v<DeadlineTimer>,
              "a vtable pointer is per-object state the type does not declare, and it would make "
              "a DeadlineTimer neither trivially relocatable nor free to construct.");

/// A @c DeadlineTimer::Callback that counts its calls.
/// @param state A @c std::size_t counter, which must outlive the timer.
void countCall(void* state)
{
    ++*static_cast<std::size_t*>(state);
}

/// What a callback that destroys its own timer needs: the owning handle, and a record that it ran.
struct SelfDestroying
{
    std::unique_ptr<DeadlineTimer> timer; ///< The timer whose callback destroys it.
    std::size_t calls = 0;                ///< How many times that callback ran.

    /// What @c DeadlineTimer::settled answered while the callback was running.
    ///
    /// Asserted, and it is what makes the case below fail on a VALUE rather than only under a
    /// sanitizer: a timer that marked itself settled after its callback returned would destroy an
    /// object from inside the call and then write to it, which reads as green everywhere but ASan.
    bool settledWhenCalled = false;
};

/// A @c DeadlineTimer::Callback that destroys the timer it was called from.
/// @param state A @c SelfDestroying, which must outlive the loop.
void destroySelf(void* state)
{
    auto* const owner = static_cast<SelfDestroying*>(state);
    ++owner->calls;
    owner->settledWhenCalled = owner->timer->settled();
    owner->timer.reset(); // ~DeadlineTimer disarms, from inside the callback it is disarming
}

/// A killer timer and the timer it retires, for the cancellation window's real shape.
struct SiblingKill
{
    std::unique_ptr<DeadlineTimer> victim; ///< Due in the same batch as its killer.
    std::size_t killerCalls = 0;           ///< How many times the killer's callback ran.
    std::size_t victimCalls = 0;           ///< How many times the victim's did. Must stay zero.
};

/// A @c DeadlineTimer::Callback that destroys a SIBLING timer already queued by the same step 5.
/// @param state A @c SiblingKill, which must outlive the loop.
void killSibling(void* state)
{
    auto* const pair = static_cast<SiblingKill*>(state);
    ++pair->killerCalls;
    pair->victim.reset(); // ~DeadlineTimer -> disarm() -> cancelTimer, from inside the drain
}

/// A @c DeadlineTimer::Callback that counts the victim's calls.
/// @param state A @c SiblingKill, which must outlive the loop.
void countVictim(void* state)
{
    ++static_cast<SiblingKill*>(state)->victimCalls;
}

} // namespace

TEST_CASE("A DeadlineTimer fires its callback when the deadline arrives", "[DeadlineTimer]")
{
    auto clock = ManualClock {};
    auto loop = TestLoop { clock };

    auto calls = std::size_t { 0 };
    auto timer = DeadlineTimer { loop, clock.now() + 50ms, &countCall, &calls };
    REQUIRE_FALSE(timer.settled());

    std::ignore = loop.drain();
    REQUIRE(calls == 0);

    clock.advance(50ms);
    std::ignore = loop.drain();
    CHECK(calls == 1);
    CHECK(timer.settled());
}

TEST_CASE("A deadline already in the past fires on a turn, not from the constructor", "[DeadlineTimer]")
{
    // Upstream states this promise and keeps it by hopping onto the reactor from a detached
    // coroutine; here the arming simply files a park, and the turn is the only thing that fires
    // one. Either way a caller is never re-entered from its own constructor.
    auto clock = ManualClock {};
    auto loop = TestLoop { clock };

    auto calls = std::size_t { 0 };
    auto timer = DeadlineTimer { loop, clock.now() - 1s, &countCall, &calls };
    REQUIRE(calls == 0);
    REQUIRE_FALSE(timer.settled());

    std::ignore = loop.drain();
    CHECK(calls == 1);
}

TEST_CASE("A DeadlineTimer destroyed before its deadline never fires and leaves nothing parked",
          "[DeadlineTimer]")
{
    // Declared BEFORE the loop so it outlives it: the case's whole claim is about what did NOT
    // happen, and a counter destroyed first could not report it.
    auto calls = std::size_t { 0 };
    auto clock = ManualClock {};
    auto loop = TestLoop { clock };

    {
        auto const timer = DeadlineTimer { loop, clock.now() + 50ms, &countCall, &calls };
        REQUIRE(loop.pendingTimerCount() == 1);
    }

    // The park is retired by the destructor, not left to expire: upstream left one parked frame
    // per settled operation, which is the leak that made an ASan build of fastcache-cc exit
    // non-zero.
    CHECK(loop.pendingTimerCount() == 0);

    clock.advance(1s);
    std::ignore = loop.drain();
    CHECK(calls == 0);
}

TEST_CASE("disarm() is idempotent and safe after the callback has already run", "[DeadlineTimer]")
{
    auto clock = ManualClock {};
    auto loop = TestLoop { clock };

    auto calls = std::size_t { 0 };
    auto timer = DeadlineTimer { loop, clock.now() + 10ms, &countCall, &calls };

    timer.disarm();
    timer.disarm();
    CHECK(timer.settled());

    clock.advance(1s);
    std::ignore = loop.drain();
    CHECK(calls == 0);

    timer.disarm(); // after the deadline it would have fired at
    CHECK(timer.settled());
}

TEST_CASE("A settled timer stays settled once its callback has run", "[DeadlineTimer]")
{
    auto clock = ManualClock {};
    auto loop = TestLoop { clock };

    auto calls = std::size_t { 0 };
    auto timer = DeadlineTimer { loop, clock.now(), &countCall, &calls };
    std::ignore = loop.drain();
    REQUIRE(calls == 1);
    REQUIRE(timer.settled());

    timer.disarm();
    CHECK(timer.settled());
    CHECK(calls == 1);
}

TEST_CASE("A DeadlineTimer may be destroyed from inside its own callback", "[DeadlineTimer]")
{
    // The timer is marked settled and its id dropped BEFORE the callback runs, so the ~DeadlineTimer
    // that the callback triggers finds nothing left to disarm. Without that, the destructor would
    // ask the loop to cancel a timer that is running -- and the loop has already taken the park out,
    // so it would report false and the timer would be destroyed while the turn still named it.
    auto clock = ManualClock {};
    auto loop = TestLoop { clock };

    auto owner = SelfDestroying {};
    owner.timer = std::make_unique<DeadlineTimer>(loop, clock.now() + 10ms, &destroySelf, &owner);

    clock.advance(10ms);
    std::ignore = loop.drain();

    CHECK(owner.calls == 1);
    CHECK(owner.settledWhenCalled); // marked BEFORE the call, so the destructor finds nothing to do
    CHECK(owner.timer == nullptr);
    CHECK(loop.pendingTimerCount() == 0);
}

TEST_CASE("A timer callback may retire another timer already queued by the same turn", "[DeadlineTimer]")
{
    // **The shape the cancellation window exists for**, rather than `cancelTimer` called by hand
    // between two ticks. Two deadlines fall due together, step 5 queues BOTH, and then the first
    // callback destroys the second's `DeadlineTimer` -- so `~DeadlineTimer` reaches `cancelTimer`
    // from inside the very drain that is about to reach the entry it retires. That is the path a
    // real owner takes: one timeout firing and tearing down the object whose own timer is due in
    // the same instant.
    //
    // Declared after the loop, so the victim is destroyed BEFORE it: a DeadlineTimer outliving
    // its loop would reach `_loop->cancelTimer` through freed storage.
    auto clock = ManualClock {};
    auto loop = TestLoop { clock };
    auto pair = SiblingKill {};

    auto const due = clock.now() + 10ms;
    auto const killer = DeadlineTimer { loop, due, &killSibling, &pair };
    pair.victim = std::make_unique<DeadlineTimer>(loop, due, &countVictim, &pair);

    clock.advance(10ms);
    std::ignore = loop.tick(); // step 5 queues both, FIFO by arming order; neither has run
    REQUIRE(loop.readyCount() == 2);
    REQUIRE(pair.killerCalls == 0);

    std::ignore = loop.drain();

    CHECK(pair.killerCalls == 1);
    CHECK(pair.victimCalls == 0); // retired from inside the window, before its entry was reached
    CHECK(pair.victim == nullptr);
    CHECK(killer.settled());
}

TEST_CASE("An armed DeadlineTimer bounds the turn's wait to its own deadline", "[DeadlineTimer]")
{
    // The case that names what this task removed. Upstream's timer woke every
    // DefaultPollInterval (50ms) whatever its deadline was, because a `Schedule` could not be
    // taken back and a disarmed timer had to be noticed rather than retired. Here the loop's own
    // deadline heap is what the wait is computed from, so a timer 500ms out costs exactly one
    // wake-up.
    auto clock = ManualClock {};
    auto backend = ScriptedBackend {};
    backend.pushTimeout();
    auto loop = EventLoop { backend, clock };

    auto calls = std::size_t { 0 };
    auto const timer = DeadlineTimer { loop, clock.now() + 500ms, &countCall, &calls };
    std::ignore = loop.runOnce();

    REQUIRE(backend.waitCount() == 1);
    REQUIRE(backend.recordedTimeouts().size() == 1); // the container `.front()` below indexes
    CHECK(backend.recordedTimeouts().front() == std::optional { core::platform::SteadyDuration { 500ms } });
    CHECK_FALSE(timer.settled());
}
