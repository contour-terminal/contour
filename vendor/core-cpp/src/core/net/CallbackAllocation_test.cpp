// SPDX-License-Identifier: Apache-2.0
//
// What a drain-step callback costs in heap allocations.
//
// A readiness completion is fastcached's hot path: every socket operation that parks is completed
// by one. Running a callback so that what it queues resumes in the callback's position must not
// make each of them allocate, so this binary counts: it replaces the global allocation functions,
// which is why it is a binary of its own -- a replacement reaches every test linked beside it.
#include <core/async/DetachedTask.hpp>
#include <core/async/Task.hpp>
#include <core/net/EventLoop.hpp>
#include <core/net/IoAwaitable.hpp>
#include <core/net/testing/TestLoop.hpp>
#include <core/platform/Clock.hpp>
#include <core/testing/ReplacedGlobalAllocation.hpp>

#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <chrono>
#include <coroutine>
#include <cstddef>
#include <cstdlib>
#include <functional>
#include <new>
#include <ranges>
#include <tuple>
#include <utility>

namespace
{

/// @return Whether the standard library allocates a debugging proxy per container: MSVC's checked
///         iterators, on in its Debug runtime. A function rather than a constant, so the case
///         below branches on it at run time and every compiler keeps both branches.
bool checkedIterators() noexcept
{
#ifdef _ITERATOR_DEBUG_LEVEL
    return _ITERATOR_DEBUG_LEVEL != 0;
#else
    return false;
#endif
}

/// Every global `operator new` and `operator new[]` this process has served.
std::atomic<std::size_t> allocations { 0 };

/// Fails the next allocation, once.
std::atomic<bool> failNextAllocation { false };

/// Serves a counted allocation from `malloc`, so a sanitizer's interception of `malloc` still sees
/// it.
/// @param size The size asked for.
/// @return The storage.
/// @throws std::bad_alloc If `malloc` refuses.
void* countedAllocation(std::size_t size)
{
    if (failNextAllocation.exchange(false))
        throw std::bad_alloc {};
    allocations.fetch_add(1, std::memory_order_relaxed);
    if (auto* const storage = std::malloc(size == 0 ? 1 : size))
        return storage;
    throw std::bad_alloc {};
}

/// A timer callback that queues nothing.
/// @param state A @c std::size_t counter of its calls.
void countCall(void* state)
{
    ++*static_cast<std::size_t*>(state);
}

/// Arms a timer due now and runs the turn that queues it, so the NEXT turn's drain runs only the
/// callback.
/// @param loop The loop.
/// @param clock The loop's clock.
/// @param calls The callback's counter.
void queueOneCallback(core::net::EventLoop& loop, core::platform::ManualClock& clock, std::size_t* calls)
{
    std::ignore = loop.addTimer(clock.now(), &countCall, calls);
    std::ignore = loop.runOnce();
}

/// Parks the awaiting flow and records its handle, for a callback to hand to `resumeSoon`.
struct ParkHere
{
    std::coroutine_handle<>* parked;
    [[nodiscard]] bool await_ready() const noexcept { return false; }
    void await_suspend(std::coroutine_handle<> self) const noexcept { *parked = self; }
    void await_resume() const noexcept {}
};

/// A waiter that parks again every time it is resumed, as a socket's reader does between reads.
/// @param parked Where it records its handle.
/// @param resumes Counts its resumptions.
core::async::Task<void> parkForever(std::coroutine_handle<>* parked, std::size_t* resumes)
{
    while (true)
    {
        co_await ParkHere { parked };
        ++*resumes;
    }
}

/// What the waking callback needs.
struct Waking
{
    core::net::EventLoop* loop = nullptr;
    std::coroutine_handle<> parked;
};

/// A timer callback that completes the parked waiter, as a readiness callback completes a read.
/// @param state The @c Waking.
void wakeWaiter(void* state)
{
    auto* const waking = static_cast<Waking*>(state);
    waking->loop->resumeSoon(core::async::ParkedWork { .resume = std::exchange(waking->parked, {}) });
}

/// A frame-free operation completed from a timer callback due at once: the path a socket's
/// readiness completion takes -- `ResultAwaitable::complete`, the ready queue, the drain step --
/// with no kernel in it.
class TimerCompletion
{
  public:
    /// @param loop The loop; must outlive this.
    /// @param clock The loop's clock.
    TimerCompletion(core::net::EventLoop& loop, core::platform::IClock& clock) noexcept:
        _loop(&loop), _clock(&clock)
    {
    }

    /// @return One operation, completed by the turn after the one that arms it.
    [[nodiscard]] core::net::IoAwaitable wait() { return core::net::IoAwaitable { &arm, &retire, this }; }

  private:
    static void arm(void* owner, core::net::IoAwaitable& self)
    {
        auto* const source = static_cast<TimerCompletion*>(owner);
        source->_operation = &self;
        source->_timer = source->_loop->addTimer(source->_clock->now(), &fire, source);
        self.cancelThrough(*source->_loop, core::net::ParkId::invalid());
    }

    static void retire(void* owner, void* awaitable) noexcept
    {
        auto* const source = static_cast<TimerCompletion*>(owner);
        if (source->_operation != awaitable)
            return;
        source->_operation = nullptr;
        std::ignore =
            source->_loop->cancelTimer(std::exchange(source->_timer, core::net::TimerId::invalid()));
    }

    static void fire(void* state)
    {
        auto* const source = static_cast<TimerCompletion*>(state);
        source->_timer = core::net::TimerId::invalid();
        if (auto* const operation = std::exchange(source->_operation, nullptr))
            operation->complete(std::size_t { 1 });
    }

    core::net::EventLoop* _loop;
    core::platform::IClock* _clock;
    core::net::IoAwaitable* _operation = nullptr;
    core::net::TimerId _timer {};
};

/// An operation completed by whoever holds it, with no timer of its own: a case completes it from a
/// callback that does something else too.
class ManualCompletion
{
  public:
    /// @param loop The loop the resumption runs on; must outlive this.
    explicit ManualCompletion(core::net::EventLoop& loop) noexcept: _loop(&loop) {}

    /// @return One operation, completed by @c complete.
    [[nodiscard]] core::net::IoAwaitable wait() { return core::net::IoAwaitable { &arm, &retire, this }; }

    /// Completes the parked operation with a count of @p value, if one is parked.
    /// @param value The count.
    void complete(std::size_t value)
    {
        if (auto* const operation = std::exchange(_operation, nullptr))
            operation->complete(value);
    }

  private:
    static void arm(void* owner, core::net::IoAwaitable& self)
    {
        auto* const source = static_cast<ManualCompletion*>(owner);
        source->_operation = &self;
        self.cancelThrough(*source->_loop, core::net::ParkId::invalid());
    }

    static void retire(void* owner, void* awaitable) noexcept
    {
        auto* const source = static_cast<ManualCompletion*>(owner);
        if (source->_operation == awaitable)
            source->_operation = nullptr;
    }

    core::net::EventLoop* _loop;
    core::net::IoAwaitable* _operation = nullptr;
};

/// Awaits one operation, counting a value.
/// @param source The owner. @param completed Counts it.
core::async::Task<void> awaitOne(ManualCompletion* source, std::size_t* completed)
{
    if ((co_await source->wait()).has_value())
        ++*completed;
}

/// A timer callback that runs whatever the case scripted.
/// @param state A @c std::function<void()>.
void runScript(void* state)
{
    (*static_cast<std::function<void()>*>(state))();
}

/// A timer that re-arms itself each time it fires: the owner's own scaffolding above, with no
/// operation completed and no waiter resumed.
struct Rearming
{
    core::net::EventLoop* loop = nullptr;
    core::platform::IClock* clock = nullptr;
    std::size_t fired = 0;
};

/// Fires a @c Rearming timer and arms it again, due at once.
/// @param state The @c Rearming.
void rearm(void* state)
{
    auto* const timer = static_cast<Rearming*>(state);
    ++timer->fired;
    std::ignore = timer->loop->addTimer(timer->clock->now(), &rearm, timer);
}

/// Awaits completions until told to stop, as a connection's handler awaits its socket's reads.
/// @param source The owner.
/// @param stop Ends the loop at the next completion.
/// @param completed Counts the completions.
core::async::Task<void> awaitCompletions(TimerCompletion* source, bool const* stop, std::size_t* completed)
{
    while (!*stop)
        if ((co_await source->wait()).has_value())
            ++*completed;
}

/// The root nobody owns, as a server's per-connection flow is, so every completion carries a claim.
/// @param child The handler it awaits.
core::async::DetachedTask runDetached(core::async::Task<void> child)
{
    co_await std::move(child);
}

} // namespace

// The replaced global allocation functions are in ReplacedGlobalAllocation.cpp, which forwards
// them here (its header says why they are not in this file).
void* core::testing::replacedAllocate(std::size_t size)
{
    return countedAllocation(size);
}

void core::testing::replacedRelease(void* storage) noexcept
{
    std::free(storage);
}

TEST_CASE("A completion held in the callback's slot survives the callback's next queueing failing to "
          "allocate",
          "[EventLoop][turn][ordering]")
{
    // The callback completes one operation -- its waiter goes into the callback's slot -- and then
    // queues another waiter, which moves the slot's waiter into the callback's range first. That
    // took the waiter out of the slot and only then made room for it: a failed allocation dropped
    // it, and the completed flow never resumed.
    auto clock = core::platform::ManualClock {};
    auto loop = core::net::testing::TestLoop { clock };
    auto source = ManualCompletion { loop };
    auto completed = std::size_t { 0 };
    auto flow = awaitOne(&source, &completed);
    flow.handle().resume();
    auto other = std::coroutine_handle<> {};
    auto otherResumes = std::size_t { 0 };
    auto waiter = parkForever(&other, &otherResumes);
    waiter.handle().resume();
    REQUIRE(other);

    auto refused = false;
    auto script = std::function<void()> { [&] {
        source.complete(5);
        failNextAllocation.store(true);
        try
        {
            loop.resumeSoon(core::async::ParkedWork { .resume = other });
        }
        catch (std::bad_alloc const&)
        {
            refused = true;
        }
        failNextAllocation.store(false);
    } };
    std::ignore = loop.addTimer(clock.now(), &runScript, &script);
    std::ignore = loop.drain();

    CHECK(refused);
    CHECK(completed == 1);
    CHECK(flow.done());
    CHECK(otherResumes == 0);
}

TEST_CASE("A drain-step callback that queues nothing costs no allocation", "[EventLoop][turn][ordering]")
{
    // Measured against a turn that runs no callback at all, so whatever an idle turn allocates on
    // this platform cancels out. Warmed first, because a container's first growth is not the cost
    // of a callback.
    auto clock = core::platform::ManualClock {};
    auto loop = core::net::testing::TestLoop { clock };
    auto calls = std::size_t { 0 };
    for ([[maybe_unused]] auto const warm: { 0, 1, 2 })
    {
        queueOneCallback(loop, clock, &calls);
        std::ignore = loop.runOnce();
    }
    REQUIRE(calls == 3);

    auto const beforeIdle = allocations.load();
    std::ignore = loop.runOnce();
    auto const idleTurn = allocations.load() - beforeIdle;

    queueOneCallback(loop, clock, &calls);
    auto const beforeCallback = allocations.load();
    std::ignore = loop.runOnce(); // the drain runs the callback, and only that
    auto const callbackTurn = allocations.load() - beforeCallback;

    REQUIRE(calls == 4);
    CHECK(callbackTurn == idleTurn);
}

TEST_CASE("A drain-step callback that resumes a parked waiter costs no allocation",
          "[EventLoop][turn][ordering]")
{
    // The hot case: a readiness completion hands its waiter to `resumeSoon`, the waiter runs in the
    // callback's position and parks again. After warm-up that turn allocates exactly what an idle
    // turn does. A per-callback container, or a queue that allocates as the waiter goes in at its
    // front, is an allocation per completion.
    auto clock = core::platform::ManualClock {};
    auto loop = core::net::testing::TestLoop { clock };
    auto resumes = std::size_t { 0 };
    auto waking = Waking { .loop = &loop, .parked = {} };
    auto waiter = parkForever(&waking.parked, &resumes);
    waiter.handle().resume();
    REQUIRE(waking.parked);

    auto const completeOnce = [&] {
        std::ignore = loop.addTimer(clock.now(), &wakeWaiter, &waking);
        std::ignore = loop.runOnce(); // queues the callback
    };
    for ([[maybe_unused]] auto const warm: { 0, 1, 2, 3, 4, 5, 6, 7 })
    {
        completeOnce();
        std::ignore = loop.runOnce();
    }
    REQUIRE(resumes == 8);

    auto const beforeIdle = allocations.load();
    std::ignore = loop.runOnce();
    auto const idleTurn = allocations.load() - beforeIdle;

    completeOnce();
    auto const beforeCallback = allocations.load();
    std::ignore = loop.runOnce(); // the callback, then its waiter in the callback's position
    auto const callbackTurn = allocations.load() - beforeCallback;

    REQUIRE(resumes == 9);
    CHECK(callbackTurn == idleTurn);
}

namespace
{

/// The measurement of the case below, which a build with checked iterators cannot make.
void completionAllocatesNothing()
{
    // The whole of what a parked socket operation costs the loop per completion, measured over many
    // turns rather than one: a timer park, the owner's callback queued and run in the drain step,
    // the waiter claimed, queued in the callback's position and resumed, and the next operation
    // armed. Four allocations hid on that path, none of them in one turn's worth of reading: the
    // ready queue was a `std::deque`, which allocates a node and frees one every few entries of a
    // FIFO that never grows; every parked operation's default answer spelled a sentence into a
    // `std::string`; a fired or cancelled timer's park was freed rather than kept; and each firing
    // collected its ids into a vector of its own.
    auto clock = core::platform::ManualClock {};
    auto loop = core::net::testing::TestLoop { clock };
    auto source = TimerCompletion { loop, clock };
    auto stop = false;
    auto completed = std::size_t { 0 };
    runDetached(awaitCompletions(&source, &stop, &completed));
    for ([[maybe_unused]] auto const warm: std::views::iota(0, 64))
        std::ignore = loop.runOnce();
    REQUIRE(completed > 0);

    // What a turn with nothing to do allocates on this platform, on a loop of its own, warmed; and
    // what a turn allocates whose only work is the owner's timer -- a park made, fired and
    // recycled -- with no operation completed and no waiter resumed.
    constexpr auto Turns = std::size_t { 256 };
    auto idleLoop = core::net::testing::TestLoop { clock };
    std::ignore = idleLoop.runOnce();
    auto const beforeIdle = allocations.load();
    std::ignore = idleLoop.runOnce();
    auto const idleTurn = allocations.load() - beforeIdle;

    auto timerLoop = core::net::testing::TestLoop { clock };
    auto timer = Rearming { .loop = &timerLoop, .clock = &clock };
    std::ignore = timerLoop.addTimer(clock.now(), &rearm, &timer);
    for ([[maybe_unused]] auto const warm: std::views::iota(0, 64))
        std::ignore = timerLoop.runOnce();
    auto const beforeTimer = allocations.load();
    for ([[maybe_unused]] auto const turn: std::views::iota(std::size_t { 0 }, Turns))
        std::ignore = timerLoop.runOnce();
    auto const timerTurns = allocations.load() - beforeTimer;
    REQUIRE(timer.fired >= Turns);
    CHECK(timerTurns == idleTurn * Turns); // a timer firing allocates nothing

    auto const completedBefore = completed;
    auto const before = allocations.load();
    for ([[maybe_unused]] auto const turn: std::views::iota(std::size_t { 0 }, Turns))
        std::ignore = loop.runOnce();
    auto const allocated = allocations.load() - before;

    CHECK(completed - completedBefore >= Turns - 1); // one completion per turn
    CHECK(allocated == idleTurn * Turns);            // and neither does a completion

    // The chain ends at its next completion and frees itself.
    stop = true;
    std::ignore = loop.runOnce();
    std::ignore = loop.runOnce();
}

} // namespace

TEST_CASE("A frame-free completion of a detached chain allocates nothing once warm",
          "[EventLoop][turn][alloc]")
{
    // MSVC's checked iterators allocate a debugging proxy for every string and container a turn
    // constructs or moves, so the count in such a build is that bookkeeping, not the loop's. A
    // runtime probe for it was tried and misread one CI toolchain, so this asks the library's own
    // switch.
    if (checkedIterators())
        SKIP("the standard library's checked iterators allocate a proxy per container");
    completionAllocatesNothing();
}
