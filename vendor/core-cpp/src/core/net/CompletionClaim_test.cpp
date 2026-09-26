// SPDX-License-Identifier: Apache-2.0
//
// What a frame-free completion hands the loop for a chain nobody owns, and what it costs.
//
// `ResultAwaitable::complete` settles an operation and queues its waiter for the drain step (G2).
// For a chain rooted in a `DetachedTask` -- every connection a server spawns -- the queued waiter
// carries a claim: the loop FREES such a chain at teardown rather than resuming it, and the claim
// is what counts it among the chain's other parks and arms it, so the last park to go frees it
// once (fastcached#1025, controller ruling R97). This is the hot path of every socket operation
// that parks, so these cases pin what the claim is -- counted and armed while queued, given back
// when the waiter resumes or is taken back -- and that it holds no reference of its own to the
// chain's state: the root's promise already holds one for as long as a counted park can exist.
//
// The owner completes from a timer callback the drain step runs, which is the path a socket's
// readiness completion takes, with no kernel in it.
#include <core/async/DetachedTask.hpp>
#include <core/async/ParkedWork.hpp>
#include <core/async/Task.hpp>
#include <core/net/EventLoop.hpp>
#include <core/net/IoAwaitable.hpp>
#include <core/net/testing/TestLoop.hpp>
#include <core/platform/Clock.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <coroutine>
#include <cstddef>
#include <format>
#include <functional>
#include <iostream>
#include <memory>
#include <ranges>
#include <stdexcept>
#include <tuple>
#include <utility>
#include <vector>

using core::async::DetachedTask;
using core::async::Task;
using core::net::EventLoop;
using core::net::EventLoopOptions;
using core::net::IoAwaitable;
using core::net::TimerId;
using core::net::testing::TestLoop;
using core::platform::ManualClock;

namespace
{

/// A frame-free operation whose owner completes it from a timer callback due at once.
class TimerCompletion
{
  public:
    /// @param loop The loop the timer and the resumption run on; must outlive this.
    /// @param clock The loop's clock.
    TimerCompletion(EventLoop& loop, core::platform::IClock& clock) noexcept: _loop(&loop), _clock(&clock) {}

    /// @return One operation, completed with a count of 1 by the turn after the one that arms it.
    [[nodiscard]] IoAwaitable wait() { return IoAwaitable { &arm, &retire, this }; }

  private:
    static void arm(void* owner, IoAwaitable& self)
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
        std::ignore = source->_loop->cancelTimer(std::exchange(source->_timer, TimerId::invalid()));
    }

    static void fire(void* state)
    {
        auto* const source = static_cast<TimerCompletion*>(state);
        source->_timer = TimerId::invalid();
        if (auto* const operation = std::exchange(source->_operation, nullptr))
            operation->complete(std::size_t { 1 });
    }

    EventLoop* _loop;
    core::platform::IClock* _clock;
    IoAwaitable* _operation = nullptr;
    TimerId _timer {};
};

/// Records the awaiting coroutine's handle and carries on without suspending.
struct RecordHandle
{
    std::coroutine_handle<>* handle;
    [[nodiscard]] bool await_ready() const noexcept { return false; }
    [[nodiscard]] bool await_suspend(std::coroutine_handle<> self) const noexcept
    {
        *handle = self;
        return false;
    }
    void await_resume() const noexcept {}
};

/// Sets a flag when destroyed, so a case can tell a freed frame from a leaked one.
class DestroyedFlag
{
  public:
    /// @param destroyed The flag to set; must outlive this.
    explicit DestroyedFlag(bool* destroyed) noexcept: _destroyed(destroyed) {}
    DestroyedFlag(DestroyedFlag const&) = delete;
    DestroyedFlag(DestroyedFlag&&) = delete;
    DestroyedFlag& operator=(DestroyedFlag const&) = delete;
    DestroyedFlag& operator=(DestroyedFlag&&) = delete;
    ~DestroyedFlag() { *_destroyed = true; }

  private:
    bool* _destroyed;
};

/// Awaits @p count completions, as a connection's handler awaits its socket's reads.
/// @param source The owner.
/// @param count How many.
/// @param completed Counts the completions that answered with a value.
/// @param destroyed Set when this frame is destroyed.
Task<void> awaitCompletions(TimerCompletion* source,
                            std::size_t count,
                            std::size_t* completed,
                            bool* destroyed)
{
    auto const flag = DestroyedFlag { destroyed };
    for ([[maybe_unused]] auto const index: std::views::iota(std::size_t { 0 }, count))
        if (auto const got = co_await source->wait(); got.has_value())
            ++*completed;
}

/// The root nobody owns, as a server's per-connection flow is.
/// @param child The handler it awaits.
/// @param root Where it records its own handle.
/// @param ended Set when the handler has returned.
DetachedTask runDetached(Task<void> child, std::coroutine_handle<>* root, bool* ended)
{
    co_await RecordHandle { root };
    co_await std::move(child);
    *ended = true;
}

/// @param root A @c DetachedTask's handle.
/// @return The state its parks share, or null before the first one.
std::shared_ptr<core::async::detail::AbandonState> const& abandonStateOf(std::coroutine_handle<> root)
{
    return std::coroutine_handle<DetachedTask::promise_type>::from_address(root.address())
        .promise()
        .abandonState;
}

/// An operation completed by whoever holds it, with no timer of its own: a case completes it from a
/// callback that does something else too.
class ManualCompletion
{
  public:
    /// @param loop The loop the resumption runs on; must outlive this.
    explicit ManualCompletion(EventLoop& loop) noexcept: _loop(&loop) {}

    /// @return One operation, completed by @c complete.
    [[nodiscard]] IoAwaitable wait() { return IoAwaitable { &arm, &retire, this }; }

    /// Completes the parked operation with a count of @p value, if one is parked.
    /// @param value The count.
    void complete(std::size_t value)
    {
        if (auto* const operation = std::exchange(_operation, nullptr))
            operation->complete(value);
    }

  private:
    static void arm(void* owner, IoAwaitable& self)
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

    EventLoop* _loop;
    IoAwaitable* _operation = nullptr;
};

/// Awaits one operation and records its count in @p order.
/// @param source The owner.
/// @param order Where the count goes once the flow resumes.
/// @param destroyed Set when this frame is destroyed.
Task<void> recordOne(ManualCompletion* source, std::vector<std::size_t>* order, bool* destroyed)
{
    auto const flag = DestroyedFlag { destroyed };
    if (auto const got = co_await source->wait(); got.has_value())
        order->push_back(*got);
}

/// A timer callback that runs whatever the case scripted.
/// @param state A @c std::function<void()>.
void runScript(void* state)
{
    (*static_cast<std::function<void()>*>(state))();
}

} // namespace

TEST_CASE("A completion queues a detached chain counted and armed, holding no reference of its own",
          "[net][ioawaitable][resume][claim]")
{
    // A bound of one stops the drain right after the owner's callback, so the waiter is left queued
    // between turns and the claim can be looked at there.
    auto clock = ManualClock {};
    auto loop = TestLoop { clock, EventLoopOptions { .dispatchBatch = 1 } };
    auto source = TimerCompletion { loop, clock };
    auto root = std::coroutine_handle<> {};
    auto completed = std::size_t { 0 };
    auto destroyed = false;
    auto ended = false;
    runDetached(awaitCompletions(&source, 2, &completed, &destroyed), &root, &ended);
    REQUIRE(root);

    std::ignore = loop.tick(); // the timer fires, queueing the owner's callback
    std::ignore = loop.tick(); // the callback completes the operation, and the bound ends the drain
    REQUIRE(completed == 0);
    REQUIRE(loop.pendingSubmissions() == 1);

    auto const& state = abandonStateOf(root);
    REQUIRE(state != nullptr);
    CHECK(state->armed());         // the loop's to free if it is torn down now
    CHECK(state.use_count() == 1); // the root's promise, and no copy in the queue

    std::ignore = loop.tick(); // resumes the waiter, which parks on the owner again
    CHECK(completed == 1);
    CHECK_FALSE(state->armed()); // given back: nothing of the loop's names it now
    CHECK(state.use_count() == 1);

    std::ignore = loop.drain();
    CHECK(completed == 2);
    CHECK(ended);
    CHECK(destroyed);
}

TEST_CASE("Teardown frees a detached chain whose completion is queued, and does not resume it",
          "[net][ioawaitable][resume][claim][teardown]")
{
    auto clock = ManualClock {};
    auto completed = std::size_t { 0 };
    auto destroyed = false;
    auto ended = false;
    {
        auto loop = TestLoop { clock, EventLoopOptions { .dispatchBatch = 1 } };
        auto source = TimerCompletion { loop, clock };
        auto root = std::coroutine_handle<> {};
        runDetached(awaitCompletions(&source, 2, &completed, &destroyed), &root, &ended);
        std::ignore = loop.tick();
        std::ignore = loop.tick();
        REQUIRE(loop.pendingSubmissions() == 1);
        REQUIRE_FALSE(destroyed);
    }
    CHECK(destroyed);      // freed with the loop, as the loop's own chain
    CHECK(completed == 0); // and never run past its `co_await`
    CHECK_FALSE(ended);
}

TEST_CASE("A detached chain destroyed while its completion is queued takes the completion back",
          "[net][ioawaitable][resume][claim]")
{
    // The frame goes before the loop reaches it, so `~ResultAwaitable` takes the queued waiter back.
    // The claim goes with it, while the root's promise -- and the state in it -- still exists: the
    // promise is destroyed after the frame's locals, which is where the awaitable lives.
    auto clock = ManualClock {};
    auto loop = TestLoop { clock, EventLoopOptions { .dispatchBatch = 1 } };
    auto source = TimerCompletion { loop, clock };
    auto root = std::coroutine_handle<> {};
    auto completed = std::size_t { 0 };
    auto destroyed = false;
    auto ended = false;
    runDetached(awaitCompletions(&source, 2, &completed, &destroyed), &root, &ended);
    std::ignore = loop.tick();
    std::ignore = loop.tick();
    REQUIRE(loop.pendingSubmissions() == 1);

    root.destroy();
    CHECK(destroyed);
    CHECK(loop.pendingSubmissions() == 0);
    CHECK(loop.drain() == 0);
    CHECK(completed == 0);
    CHECK_FALSE(ended);
}

TEST_CASE("Two completions from one callback resume in the order they completed",
          "[net][ioawaitable][resume][ordering]")
{
    // The first completion is held in the callback's slot; the second moves it to the head of the
    // callback's range and queues behind it, and whatever the callback queues after is behind both.
    auto clock = ManualClock {};
    auto loop = TestLoop { clock };
    auto first = ManualCompletion { loop };
    auto second = ManualCompletion { loop };
    auto order = std::vector<std::size_t> {};
    auto destroyed = std::array<bool, 2> {};
    auto flows =
        std::array { recordOne(&first, &order, destroyed.data()), recordOne(&second, &order, &destroyed[1]) };
    for (auto& flow: flows)
        flow.handle().resume();

    auto script = std::function<void()> { [&] {
        first.complete(1);
        second.complete(2);
    } };
    std::ignore = loop.addTimer(clock.now(), &runScript, &script);
    std::ignore = loop.drain();
    CHECK(order == std::vector<std::size_t> { 1, 2 });
}

TEST_CASE("A waiter destroyed while the callback that completed it still runs is taken back",
          "[net][ioawaitable][resume][claim]")
{
    // The completion sits in the running callback's slot, not in any queue; the frame goes before
    // the callback returns, and nothing may resume it.
    auto clock = ManualClock {};
    auto loop = TestLoop { clock };
    auto source = ManualCompletion { loop };
    auto root = std::coroutine_handle<> {};
    auto order = std::vector<std::size_t> {};
    auto destroyed = false;
    auto ended = false;
    runDetached(recordOne(&source, &order, &destroyed), &root, &ended);
    REQUIRE(root);

    auto script = std::function<void()> { [&] {
        source.complete(1);
        root.destroy();
    } };
    std::ignore = loop.addTimer(clock.now(), &runScript, &script);
    std::ignore = loop.drain();
    CHECK(destroyed);
    CHECK(order.empty());
    CHECK_FALSE(ended);
    CHECK(loop.pendingSubmissions() == 0);
}

TEST_CASE("A completed waiter survives a callback that throws after completing it",
          "[net][ioawaitable][resume][claim]")
{
    // The throw leaves the drain; the waiter in the callback's slot takes the head of the ready
    // queue for the next drain rather than being lost with the callback's frame.
    auto clock = ManualClock {};
    auto loop = TestLoop { clock };
    auto source = ManualCompletion { loop };
    auto root = std::coroutine_handle<> {};
    auto order = std::vector<std::size_t> {};
    auto destroyed = false;
    auto ended = false;
    runDetached(recordOne(&source, &order, &destroyed), &root, &ended);

    auto script = std::function<void()> { [&] {
        source.complete(7);
        throw std::runtime_error { "callback failed after completing" };
    } };
    std::ignore = loop.addTimer(clock.now(), &runScript, &script);
    CHECK_THROWS_AS(loop.drain(), std::runtime_error);
    CHECK(order.empty());
    REQUIRE(loop.pendingSubmissions() == 1);

    std::ignore = loop.drain();
    CHECK(order == std::vector<std::size_t> { 7 });
    CHECK(ended);
    CHECK(destroyed);
}

TEST_CASE("Completion cost through a drain-step callback", "[.][bench][net][ioawaitable]")
{
    // Not run by default: `core-cpp-net_backend-test "[bench]"`. Reports the time per completion,
    // the median of five runs, for a chain nobody owns (a claim per completion) and for one a
    // caller owns (none). Each completion is one turn: the owner's timer fires in step 5 and its
    // callback completes the waiter in the next step 2, where the waiter resumes and parks again.
    constexpr auto Completions = std::size_t { 200'000 };
    constexpr auto Runs = std::size_t { 5 };

    auto const timeOne = [](bool detached) {
        auto clock = ManualClock {};
        auto loop = TestLoop { clock };
        auto source = TimerCompletion { loop, clock };
        auto completed = std::size_t { 0 };
        auto destroyed = false;
        auto ended = false;
        auto root = std::coroutine_handle<> {};
        auto owned = Task<void> {};
        auto const started = std::chrono::steady_clock::now();
        if (detached)
            runDetached(awaitCompletions(&source, Completions, &completed, &destroyed), &root, &ended);
        else
        {
            owned = awaitCompletions(&source, Completions, &completed, &destroyed);
            owned.handle().resume();
        }
        while (completed < Completions)
            std::ignore = loop.tick();
        auto const elapsed = std::chrono::steady_clock::now() - started;
        REQUIRE(completed == Completions);
        return std::chrono::duration<double, std::nano>(elapsed).count() / static_cast<double>(Completions);
    };

    for (auto const detached: { true, false })
    {
        auto samples = std::array<double, Runs> {};
        std::ranges::generate(samples, [&] { return timeOne(detached); });
        std::ranges::sort(samples);
        std::cout << std::format("completion via drain-step callback, {} chain: median {:.1f} ns/op "
                                 "(min {:.1f}, max {:.1f}, {} completions x {} runs)\n",
                                 detached ? "detached" : "owned",
                                 samples[Runs / 2],
                                 samples.front(),
                                 samples.back(),
                                 Completions,
                                 Runs);
    }
}
