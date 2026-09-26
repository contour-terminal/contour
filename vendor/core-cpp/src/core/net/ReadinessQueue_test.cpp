// SPDX-License-Identifier: Apache-2.0
//
// What a readiness report queues for a frameless park: one entry, however many waits report the
// handle before the drain reaches it.
//
// A frameless readiness park -- every parked socket operation -- stays filed across its wakes, and
// its owner's callback runs in the drain step (G2). A level-triggered backend reports the handle on
// every wait until somebody reads it, and nobody does while that callback waits in the ready queue:
// behind the drain bound, for one. See `.agent/rules/async-and-net.md`, "The turn, and the orderings
// inside it".
#include <core/async/ParkedWork.hpp>
#include <core/async/Task.hpp>
#include <core/net/EventLoop.hpp>
#include <core/net/testing/ScriptedBackend.hpp>
#include <core/platform/Clock.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <coroutine>
#include <cstddef>
#include <cstdint>
#include <ranges>
#include <tuple>
#include <type_traits>

using core::async::Task;
using core::net::EventLoop;
using core::net::EventLoopOptions;
using core::net::ParkWake;
using core::platform::ManualClock;

namespace
{

/// Hands itself back to the loop's ready queue each time it is resumed, until told to stop: work
/// that keeps a bounded drain busy.
/// @param loop The loop.
/// @param stop Ends the flow at its next resumption.
/// @param resumed Counts its resumptions.
Task<void> keepRequeueing(EventLoop* loop, bool const* stop, std::size_t* resumed)
{
    struct Requeue
    {
        EventLoop* loop;
        [[nodiscard]] bool await_ready() const noexcept { return false; }
        void await_suspend(std::coroutine_handle<> self) const
        {
            loop->resumeSoon(core::async::ParkedWork { .resume = self });
        }
        void await_resume() const noexcept {}
    };
    while (!*stop)
    {
        co_await Requeue { loop };
        ++*resumed;
    }
}

/// @return A handle the scripted backend accepts and nothing else interprets, however the platform
///         spells one.
template <typename Handle = core::platform::NativeHandle>
Handle scriptedHandle()
{
    if constexpr (std::is_pointer_v<Handle>)
        return reinterpret_cast<Handle>(std::intptr_t { 7 });
    else
        return Handle { 7 };
}

/// A readiness callback that only counts its calls.
/// @param state A @c std::size_t counter.
/// @param wake Why it was called.
void countReadyCall(void* state, ParkWake wake)
{
    if (wake == ParkWake::Ready)
        ++*static_cast<std::size_t*>(state);
}

} // namespace

TEST_CASE("A frameless readiness park is queued once however many waits report it first",
          "[net][eventloop][readiness]")
{
    // A level-triggered backend reports a handle on every wait for as long as nobody has read it,
    // and nobody has while the owner's callback is still waiting in the ready queue -- behind a
    // drain bound, say. Each report queued the callback again, and each copy took a slot of the
    // bound while doing nothing, so the copies crowded out the work that would have consumed the
    // readiness and multiplied: 64 connections on one loop, the default bound, made each round trip
    // cost some 1,460 dispatches and as many drain slots. One entry per park, and the bound serves
    // what it is for.
    //
    // A bound of one and a flow that re-queues itself on every resumption keep the callback's entry
    // waiting across turns, and the script reports the handle on every wait.
    auto clock = ManualClock {};
    auto backend = core::net::testing::ScriptedBackend {};
    auto loop = EventLoop { backend,
                            clock,
                            EventLoopOptions { .idle = core::net::IdlePolicy::Return, .dispatchBatch = 1 } };
    auto calls = std::size_t { 0 };
    auto const park = loop.registerPark(core::net::ParkEntry::onReadyCallback(
        &countReadyCall, &calls, scriptedHandle(), core::net::DefaultHandleKind, core::net::Interest::Read));
    REQUIRE(park);
    auto const handler = backend.lastHandlerId();
    REQUIRE(handler);

    auto stop = false;
    auto requeued = std::size_t { 0 };
    auto busy = keepRequeueing(&loop, &stop, &requeued);
    busy.handle().resume();

    constexpr auto Turns = std::size_t { 16 };
    auto mostQueued = std::size_t { 0 };
    for ([[maybe_unused]] auto const turn: std::views::iota(std::size_t { 0 }, Turns))
    {
        backend.pushReadable(handler);
        std::ignore = loop.runOnce(core::platform::SteadyDuration::zero());
        mostQueued = std::max(mostQueued, loop.readyCount());
    }
    CHECK(mostQueued <= 2);          // the busy flow and one entry for the park, never a copy
    CHECK(calls >= (Turns / 2) - 1); // and the park is still served, every other turn
    CHECK(requeued >= (Turns / 2) - 1);

    loop.unregisterPark(park);
    stop = true;
    while (!busy.handle().done() && loop.readyCount() != 0)
        std::ignore = loop.runOnce(core::platform::SteadyDuration::zero());
    CHECK(busy.handle().done());
}
