// SPDX-License-Identifier: Apache-2.0
//
// `testing::TestLoop`, and the three things a loop promises every caller that hands it work:
// FIFO order, a `submit` that is safe from any thread, and a `cancelPending` whose `true` is an
// ownership TRANSFER rather than a status.
//
// Ported from fastcached's `Async/TestReactor_test.cpp` at `0708dd54`. Two things changed in the
// port. The double is no longer a second implementation of the reactor -- it is the real
// `EventLoop` over `NullBackend` -- so a case that passes here and fails on a `PlatformLoop` is a
// backend difference and nothing else; and the cancellation cases run over `BackendMatrix` as well
// as the double, because "`cancelPending` means the same on every backend" is a rule of the merged
// design (Part I §2, rule 5) and was a rule nothing checked.
#include <core/async/DetachedTask.hpp>
#include <core/async/ParkedWork.hpp>
#include <core/async/Task.hpp>
#include <core/net/EventLoop.hpp>
#include <core/net/IoBackend.hpp>
#include <core/net/testing/BackendMatrix.hpp>
#include <core/net/testing/TestLoop.hpp>
#include <core/platform/Clock.hpp>
#include <core/platform/SystemPipe.hpp>

#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <barrier>
#include <chrono>
#include <coroutine>
#include <cstddef>
#include <memory>
#include <ranges>
#include <thread>
#include <tuple>
#include <utility>
#include <vector>

using core::async::DetachedTask;
using core::async::Task;
using core::net::EventLoop;
using core::net::testing::TestLoop;
using core::platform::ManualClock;
using namespace std::chrono_literals;

namespace
{

/// Yields control back to the loop's ready queue, borrowing rather than owning.
struct YieldToLoop
{
    EventLoop* loop; ///< Where to hand the coroutine back.

    /// @return False: always suspend.
    [[nodiscard]] bool await_ready() const noexcept { return false; }

    /// Templated on the promise, so the loop is told whether it may free this chain: a detached
    /// flow parking here is one nothing else owns, and that is the whole question
    /// `cancelPending`'s ownership transfer turns on.
    /// @tparam Promise The awaiting coroutine's promise type.
    /// @param handle The coroutine to re-queue.
    template <typename Promise>
    void await_suspend(std::coroutine_handle<Promise> handle) const
    {
        loop->submit(core::async::detail::parkedWorkFor(handle));
    }

    void await_resume() const noexcept {}
};

/// Suspends until the loop's clock reaches a deadline.
struct SleepOnLoop
{
    EventLoop* loop {};                          ///< The loop whose clock and heap this uses.
    core::platform::SteadyTimePoint deadline {}; ///< When to resume.

    /// @return False: reading the clock is a call, so a deadline already passed is
    ///         @c await_suspend's to answer (fastcached#1546, `.agent/rules/async-and-net.md`).
    [[nodiscard]] constexpr bool await_ready() const noexcept { return false; }

    /// @param handle The coroutine to resume at the deadline.
    /// @return False, resuming at once, when the deadline has already passed; true once scheduled.
    [[nodiscard]] bool await_suspend(std::coroutine_handle<> handle) const
    {
        if (loop->clock().now() >= deadline)
            return false;
        loop->schedule(deadline, handle);
        return true;
    }

    void await_resume() const noexcept {}
};

/// Hands the awaiting coroutine's own handle to the caller, without suspending.
///
/// What makes a case able to name a frame the LOOP owns: a detached chain has no `Task` object to
/// ask, so the only thing that knows its handle is the frame itself.
struct PublishSelf
{
    std::coroutine_handle<>* out; ///< Where to record the handle.

    /// @return False, so the handle is captured through @c await_suspend.
    [[nodiscard]] bool await_ready() const noexcept { return false; }

    /// @param handle The awaiting coroutine.
    /// @return False: resume at once, having recorded it.
    [[nodiscard]] bool await_suspend(std::coroutine_handle<> handle) const noexcept
    {
        *out = handle;
        return false;
    }

    void await_resume() const noexcept {}
};

/// Counts its passes, yielding back to the loop between them.
/// @param loop The loop to yield to.
/// @param counter Incremented once per pass.
/// @param times How many passes to make.
Task<void> countYields(EventLoop* loop, int* counter, int times)
{
    for ([[maybe_unused]] auto const pass: std::views::iota(0, times))
    {
        ++*counter;
        co_await YieldToLoop { loop };
    }
}

/// Waits for @p deadline and then records that it fired.
/// @param loop The loop to park on.
/// @param deadline When to resume.
/// @param fired Set once it resumes.
Task<void> waitUntil(EventLoop* loop, core::platform::SteadyTimePoint deadline, bool* fired)
{
    co_await SleepOnLoop { .loop = loop, .deadline = deadline };
    *fired = true;
}

/// Records that it ran, and nothing else. Lazy, so handing its handle to the loop from a producer
/// thread is the only thing that advances it -- which is exactly the crossing under test.
/// @param resumed Incremented when the body runs.
Task<void> increment(std::atomic<int>* resumed)
{
    resumed->fetch_add(1, std::memory_order_relaxed);
    co_return;
}

/// Counts the frames a case has seen freed.
struct FrameCounter
{
    int destroyed = 0; ///< Frames freed, counted by the sentinel each carries.
};

/// A coroutine-frame sentinel, passed by value so it lives in the frame.
class FrameSentinel
{
  public:
    /// @param counter Where the destruction is tallied; never null.
    explicit FrameSentinel(FrameCounter* counter) noexcept: _counter { counter } {}

    FrameSentinel(FrameSentinel&& other) noexcept: _counter { std::exchange(other._counter, nullptr) } {}

    FrameSentinel(FrameSentinel const&) = delete;
    FrameSentinel& operator=(FrameSentinel const&) = delete;
    FrameSentinel& operator=(FrameSentinel&&) = delete;

    ~FrameSentinel()
    {
        if (_counter != nullptr)
            ++_counter->destroyed;
    }

  private:
    FrameCounter* _counter;
};

/// A detached chain that publishes its own handle and then parks on the loop's ready queue.
/// @param loop The loop to hand itself to.
/// @param out Receives this frame's handle.
/// @param sentinel Counted when this frame dies.
DetachedTask publishThenSubmit(EventLoop* loop, std::coroutine_handle<>* out, FrameSentinel sentinel)
{
    static_cast<void>(sentinel);
    co_await PublishSelf { out };
    co_await YieldToLoop { loop };
    co_return;
}

/// A detached chain that publishes its own handle and then parks on a deadline nothing reaches.
/// @param loop The loop to park on.
/// @param out Receives this frame's handle.
/// @param sentinel Counted when this frame dies.
DetachedTask publishThenSleep(EventLoop* loop, std::coroutine_handle<>* out, FrameSentinel sentinel)
{
    static_cast<void>(sentinel);
    co_await PublishSelf { out };
    co_await loop->sleepUntil(loop->clock().now() + 1h);
    co_return;
}

/// A detached chain that publishes its own handle and then parks on readiness that never arrives.
/// @param loop The loop to park on.
/// @param handle The handle to watch.
/// @param out Receives this frame's handle.
/// @param sentinel Counted when this frame dies.
DetachedTask publishThenWaitReadable(EventLoop* loop,
                                     core::platform::NativeHandle handle,
                                     std::coroutine_handle<>* out,
                                     FrameSentinel sentinel)
{
    static_cast<void>(sentinel);
    co_await PublishSelf { out };
    co_await loop->waitReadable(handle);
    co_return;
}

/// Builds a loop over one of this platform's backends, on this thread.
class BackendLoop
{
  public:
    /// @param kind Which backend to build.
    explicit BackendLoop(core::net::BackendKind kind):
        _backend { core::net::makeBackend(kind) },
        _loop { std::make_unique<EventLoop>(
            *_backend, _clock, core::net::EventLoopOptions { .idle = core::net::IdlePolicy::Return }) }
    {
    }

    /// @return The loop under test.
    [[nodiscard]] EventLoop& loop() noexcept { return *_loop; }

  private:
    ManualClock _clock;
    std::unique_ptr<core::net::IoBackend> _backend;
    std::unique_ptr<EventLoop> _loop; ///< By pointer: an EventLoop is immovable.
};

} // namespace

TEST_CASE("TestLoop::submit resumes a single coroutine and drains", "[TestLoop]")
{
    auto clock = ManualClock {};
    auto loop = TestLoop { clock };

    auto counter = 0;
    auto task = countYields(&loop, &counter, 3);
    loop.submit(task.handle());
    std::ignore = loop.drain();

    CHECK(counter == 3);
    CHECK(task.done());
    CHECK(loop.pendingSubmissions() == 0);
}

TEST_CASE("TestLoop's counters include what was handed over between turns", "[TestLoop]")
{
    // The test thread outside a turn is not the loop's worker, so `submit` and `schedule` go to the
    // cross-thread inbound queue rather than straight to the ready queue and the park table. A
    // counter that read only those answered 0 right after a submit -- the answer depended on which
    // thread submitted, which is the reason cancelPending already searches the inbound queue.
    auto clock = ManualClock {};
    auto loop = TestLoop { clock };

    auto counter = 0;
    auto task = countYields(&loop, &counter, 1);
    loop.submit(task.handle());
    CHECK(loop.pendingSubmissions() == 1);

    loop.schedule(clock.now() + 50ms, std::noop_coroutine());
    CHECK(loop.pendingTimers() == 1);

    clock.advance(50ms);
    std::ignore = loop.drain();
    CHECK(task.done());
    CHECK(loop.pendingSubmissions() == 0);
    CHECK(loop.pendingTimers() == 0);
}

TEST_CASE("TestLoop processes submissions in FIFO order", "[TestLoop]")
{
    // FIFO is what makes a loop fair rather than merely correct: two flows handed over in an order
    // are resumed in it, so a producer cannot starve a consumer by submitting first each turn.
    auto clock = ManualClock {};
    auto loop = TestLoop { clock };

    auto order = std::vector<int> {};
    auto record = [&order](int which) {
        return [&order, which] {
            order.push_back(which);
        };
    };
    loop.post(record(1));
    loop.post(record(2));
    loop.post(record(3));
    std::ignore = loop.drain();

    CHECK(order == std::vector<int> { 1, 2, 3 });

    auto first = 0;
    auto second = 0;
    auto taskA = countYields(&loop, &first, 2);
    auto taskB = countYields(&loop, &second, 2);
    loop.submit(taskA.handle());
    loop.submit(taskB.handle());
    std::ignore = loop.drain();

    CHECK(first == 2);
    CHECK(second == 2);
}

TEST_CASE("TestLoop::schedule fires a deadline when the clock advances", "[TestLoop]")
{
    auto clock = ManualClock {};
    auto loop = TestLoop { clock };

    auto fired = false;
    auto task = waitUntil(&loop, clock.now() + 100ms, &fired);
    loop.submit(task.handle());

    std::ignore = loop.drain();
    CHECK_FALSE(fired);
    CHECK(loop.pendingTimers() == 1);

    clock.advance(99ms);
    std::ignore = loop.drain();
    CHECK_FALSE(fired);

    clock.advance(1ms);
    std::ignore = loop.drain();
    CHECK(fired);
    CHECK(loop.pendingTimers() == 0);
}

TEST_CASE("TestLoop fires deadlines in order, FIFO on a tie", "[TestLoop]")
{
    // The tie-break is the half a heap does not give: two deadlines armed for the same instant
    // come out in the order they were armed, which is what makes a debounce and its timeout
    // resolve the way the code that armed them reads.
    auto clock = ManualClock {};
    auto loop = TestLoop { clock };

    auto const start = clock.now();

    auto firedEarly = false;
    auto firedLate = false;
    auto firedSameFirst = false;
    auto firedSameSecond = false;

    auto early = waitUntil(&loop, start + 10ms, &firedEarly);
    auto late = waitUntil(&loop, start + 50ms, &firedLate);
    auto sameFirst = waitUntil(&loop, start + 25ms, &firedSameFirst);
    auto sameSecond = waitUntil(&loop, start + 25ms, &firedSameSecond);

    loop.submit(early.handle());
    loop.submit(late.handle());
    loop.submit(sameFirst.handle());
    loop.submit(sameSecond.handle());
    std::ignore = loop.drain();
    REQUIRE(loop.pendingTimers() == 4);

    clock.advance(100ms);
    std::ignore = loop.drain();
    CHECK(firedEarly);
    CHECK(firedLate);
    CHECK(firedSameFirst);
    CHECK(firedSameSecond);
    CHECK(loop.pendingTimers() == 0);
}

TEST_CASE("TestLoop::stop short-circuits run()", "[TestLoop]")
{
    // Requested before the loop is entered: no turn happens at all. A stop that only took effect
    // after one turn would run work a caller has already decided not to run.
    auto clock = ManualClock {};
    auto loop = TestLoop { clock };

    auto counter = 0;
    auto task = countYields(&loop, &counter, 10);
    loop.submit(task.handle());
    loop.stop();
    loop.run();

    CHECK(counter == 0);
}

TEST_CASE("TestLoop accepts submit and schedule from many threads", "[TestLoop]")
{
    // `IExecutor` documents `submit` as safe from any thread, and fastcached's double did not
    // honour it -- it touched a bare deque and a bare vector. Nothing noticed while every producer
    // was the test's own thread, and the primitives built on a loop (a resolver handing a result
    // back from a worker pool, a queue pushed by a producer thread) cross threads by definition.
    //
    // What is asserted is that nothing is LOST, which is the property, rather than the size of one
    // container or the other: where the work waits between the producer's call and the loop's own
    // turn is the loop's business.
    constexpr auto Producers = std::size_t { 8 };
    constexpr auto PerProducer = std::size_t { 64 };

    auto clock = ManualClock {};
    auto loop = TestLoop { clock };

    auto resumed = std::atomic<int> { 0 };
    auto tasks = std::vector<Task<void>> {};
    tasks.reserve(Producers * PerProducer);
    for ([[maybe_unused]] auto const index: std::views::iota(std::size_t { 0 }, Producers * PerProducer))
        tasks.push_back(increment(&resumed));

    auto start = std::barrier { static_cast<std::ptrdiff_t>(Producers) };
    {
        // `std::thread` with an explicit join rather than `std::jthread`: AppleClang's libc++ has
        // no `<stop_token>`, so it has no `jthread` either, and this file has to build there.
        auto threads = std::vector<std::thread> {};
        threads.reserve(Producers);
        for (auto const producer: std::views::iota(std::size_t { 0 }, Producers))
        {
            threads.emplace_back([&, producer] {
                start.arrive_and_wait();
                for (auto const index: std::views::iota(std::size_t { 0 }, PerProducer))
                {
                    auto& task = tasks[(producer * PerProducer) + index];
                    // Half through the ready queue and half through the deadline heap, because
                    // they are two paths and one of them being safe would still pass a case that
                    // used either alone.
                    if ((index % 2) == 0)
                        loop.submit(task.handle());
                    else
                        loop.schedule(clock.now(), task.handle());
                }
            });
        }
        for (auto& thread: threads)
            thread.join();
    } // every producer joined

    CHECK(loop.drain() == static_cast<std::size_t>(Producers * PerProducer));
    CHECK(resumed.load() == static_cast<int>(Producers * PerProducer));
    CHECK(loop.pendingSubmissions() == 0);
    CHECK(loop.pendingTimers() == 0);
}

namespace
{

/// `cancelPending` on a submission, a deadline park and a readiness park, on one loop.
///
/// The three are one case because the rule is one rule: `true` means THIS call removed the work,
/// so the caller alone may now resume or destroy it -- and the loop must therefore neither resume
/// it nor free it, however it is torn down afterwards. A `false` from a second call is the other
/// half: a status would answer `true` twice and two callers would both believe they owned the
/// frame.
/// @param loop The loop to exercise.
/// @param readable A handle a readiness park can watch; nothing ever writes to it.
void cancelPendingTransfersOwnership(EventLoop& loop, core::platform::NativeHandle readable)
{
    SECTION("a submission")
    {
        auto counter = FrameCounter {};
        auto handle = std::coroutine_handle<> {};
        publishThenSubmit(&loop, &handle, FrameSentinel { &counter });
        REQUIRE(handle);
        REQUIRE(counter.destroyed == 0);

        CHECK(loop.cancelPending(handle));
        CHECK_FALSE(loop.cancelPending(handle)); // taken: a second caller must not believe it owns it
        CHECK(counter.destroyed == 0);           // taking it back is not freeing it

        // Driving the loop now must not find it either: the transfer is complete.
        std::ignore = loop.runUntilIdle();
        CHECK(counter.destroyed == 0);

        handle.destroy();
        CHECK(counter.destroyed == 1);
    }

    SECTION("a deadline park")
    {
        auto counter = FrameCounter {};
        auto handle = std::coroutine_handle<> {};
        publishThenSleep(&loop, &handle, FrameSentinel { &counter });
        REQUIRE(handle);
        REQUIRE(loop.pendingTimerCount() == 1);

        CHECK(loop.cancelPending(handle));
        CHECK(loop.pendingTimerCount() == 0); // and the heap entry went with it
        CHECK_FALSE(loop.cancelPending(handle));
        CHECK(counter.destroyed == 0);

        handle.destroy();
        CHECK(counter.destroyed == 1);
    }

    SECTION("a readiness park")
    {
        auto counter = FrameCounter {};
        auto handle = std::coroutine_handle<> {};
        publishThenWaitReadable(&loop, readable, &handle, FrameSentinel { &counter });
        REQUIRE(handle);

        // Every backend this case runs against accepts the registration, so the flow HAS parked.
        // Asserted rather than branched on: the branch that used to stand here could not run --
        // `BackendMatrix` excludes `Null`, `Scripted` and `HostDriven`, and the rest accept -- and
        // if one ever did refuse, the `FdRegistrationFailed` would escape a `DetachedTask` into
        // `unhandled_exception`, which terminates, so the branch's own cleanup could not run
        // either. A guard that cannot execute is one nobody can maintain.
        REQUIRE(loop.parkedWaiterCount() == 1);

        CHECK(loop.cancelPending(handle));
        CHECK(loop.parkedWaiterCount() == 0); // and the registration was detached with it
        CHECK_FALSE(loop.cancelPending(handle));
        CHECK(counter.destroyed == 0);

        handle.destroy();
        CHECK(counter.destroyed == 1);
    }
}

} // namespace

TEST_CASE("cancelPending hands the work back, on every backend", "[TestLoop][cancel]")
{
    auto pipe = core::platform::createSystemPipe();
    REQUIRE(pipe.has_value());

    SECTION("on the test double")
    {
        auto clock = ManualClock {};
        auto loop = TestLoop { clock };
        cancelPendingTransfersOwnership(loop, (*pipe)->readFd());
    }

    for (auto const& entry: core::net::testing::BackendMatrix)
    {
        if (!core::net::makeBackend(entry.kind))
            continue; // not built on this platform

        DYNAMIC_SECTION("backend=" << entry.name)
        {
            auto driver = BackendLoop { entry.kind };
            cancelPendingTransfersOwnership(driver.loop(), (*pipe)->readFd());
        }
    }
}

namespace
{

/// `cancelPending` on a waiter whose handle has ALREADY become ready: dispatched into the ready
/// queue, its drain not yet run. That waiter is in two places at once -- the ready queue holds the
/// frame, and the park it came from is still filed and still registered with the backend, because
/// only `await_resume` unregisters a readiness park and a frame taken back never runs it.
///
/// A `true` that took the frame and left the park would hand the caller a frame the backend still
/// holds a handler for: the caller destroys it, and the next readiness on that handle dispatches
/// into a park naming freed storage. That was
/// [core-cpp#41](https://github.com/contour-terminal/core-cpp/issues/41).
/// @param loop The loop to exercise.
/// @param pipe A pipe nothing else reads; this case writes one byte to it.
void cancelPendingDetachesAQueuedWaiter(EventLoop& loop, core::platform::SystemPipe& pipe)
{
    auto counter = FrameCounter {};
    auto handle = std::coroutine_handle<> {};
    publishThenWaitReadable(&loop, pipe.waitHandle(), &handle, FrameSentinel { &counter });
    REQUIRE(handle);
    REQUIRE(loop.parkedWaiterCount() == 1);

    auto const byte = std::byte { 0x2a };
    REQUIRE(pipe.write(&byte, 1).value_or(0) == 1);

    // Turns until readiness has been dispatched. Bounded, because a readiness bridge may deliver
    // on another thread and a turn can come back before it has; the queue is what is waited for,
    // and a timeout says so rather than hanging.
    constexpr auto MaxTurns = 500;
    auto turns = 0;
    while (loop.readyCount() == 0 && turns < MaxTurns)
    {
        std::ignore = loop.runOnce(std::chrono::milliseconds { 10 });
        ++turns;
    }
    INFO("waited " << turns << " turns for the pipe's readiness to reach the ready queue");
    REQUIRE(loop.readyCount() == 1);
    REQUIRE(loop.parkedWaiterCount() == 1); // queued, and its park still filed and attached

    CHECK(loop.cancelPending(handle));
    CHECK(loop.readyCount() == 0);
    CHECK(loop.parkedWaiterCount() == 0); // the park came down with the frame
    CHECK_FALSE(loop.cancelPending(handle));
    CHECK(counter.destroyed == 0);

    handle.destroy();
    CHECK(counter.destroyed == 1);

    // And the loop runs on without reaching it: the byte is still unread, so a registration left
    // behind would dispatch again here.
    std::ignore = loop.runOnce(core::platform::SteadyDuration::zero());
    CHECK(loop.readyCount() == 0);
}

} // namespace

TEST_CASE("cancelPending on a waiter queued after readiness takes its park too", "[TestLoop][cancel]")
{
    for (auto const& entry: core::net::testing::BackendMatrix)
    {
        if (!core::net::makeBackend(entry.kind))
            continue; // not built on this platform

        DYNAMIC_SECTION("backend=" << entry.name)
        {
            auto pipe = core::platform::createSystemPipe();
            REQUIRE(pipe.has_value());
            auto driver = BackendLoop { entry.kind };
            cancelPendingDetachesAQueuedWaiter(driver.loop(), **pipe);
        }
    }
}
