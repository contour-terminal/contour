// SPDX-License-Identifier: Apache-2.0
#include <catch2/catch_test_macros.hpp>

#include <coroutine>
#include <stdexcept>
#include <vector>

#include <coro/Cancellation.hpp>
#include <coro/Task.hpp>
#include <coro/WhenAny.hpp>

using coro::OperationCancelled;
using coro::StopToken;
using coro::Task;
using coro::whenAny;

namespace
{

/// An awaitable the test resumes by hand to control which child of a whenAny
/// finishes first. It captures the awaiting coroutine's stop token; on resume it
/// throws @c OperationCancelled if a cancel was requested while parked, so a losing
/// child unwinds exactly as a real runtime awaitable would.
struct ManualEvent
{
    std::vector<std::coroutine_handle<>>* waiters;
    StopToken token {}; // filled in await_suspend; defaulted so call sites name only waiters

    [[nodiscard]] bool await_ready() const noexcept { return false; }

    template <typename Promise>
    [[nodiscard]] bool await_suspend(std::coroutine_handle<Promise> awaiting) noexcept
    {
        if constexpr (requires { awaiting.promise().stopToken(); })
            token = awaiting.promise().stopToken();
        if (token.stop_requested())
            return false;
        waiters->push_back(awaiting);
        return true;
    }

    void await_resume() const
    {
        if (token.stop_requested())
            throw OperationCancelled {};
    }
};

/// Parks on a ManualEvent, then records that it ran to completion (the winner path)
/// or, if cancelled while parked, that it was cancelled (the loser path).
Task<void> racer(std::vector<std::coroutine_handle<>>* waiters, bool* completed, bool* cancelled)
{
    try
    {
        co_await ManualEvent { .waiters = waiters };
        *completed = true;
    }
    catch (OperationCancelled const&)
    {
        *cancelled = true;
    }
}

/// A racer whose task throws (not cancellation) once resumed, to prove the winner's
/// failure surfaces through whenAny.
Task<void> failingRacer(std::vector<std::coroutine_handle<>>* waiters)
{
    co_await ManualEvent { .waiters = waiters };
    throw std::runtime_error("boom");
}

/// A racer that completes synchronously on first resume (never parks).
Task<void> instantRacer(bool* completed)
{
    *completed = true;
    co_return;
}

/// Drives whenAny over two manual racers and records the winner index.
Task<void> raceTwo(std::vector<std::coroutine_handle<>>* waiters,
                   bool* aDone,
                   bool* aCancelled,
                   bool* bDone,
                   bool* bCancelled,
                   std::size_t* winner)
{
    *winner = co_await whenAny(racer(waiters, aDone, aCancelled), racer(waiters, bDone, bCancelled));
}

/// Drives whenAny where the first child completes synchronously.
Task<void> raceWithInstantWinner(std::vector<std::coroutine_handle<>>* waiters,
                                 bool* instantDone,
                                 bool* parkedDone,
                                 bool* parkedCancelled,
                                 std::size_t* winner)
{
    *winner = co_await whenAny(instantRacer(instantDone), racer(waiters, parkedDone, parkedCancelled));
}

/// Drives whenAny where the winning child fails; the exception must propagate.
Task<void> raceWithFailingWinner(std::vector<std::coroutine_handle<>>* waiters, bool* threw)
{
    try
    {
        static_cast<void>(co_await whenAny(failingRacer(waiters)));
    }
    catch (std::runtime_error const&)
    {
        *threw = true;
    }
}

} // namespace

TEST_CASE("whenAny resumes on the first child and cancels the loser", "[whenAny]")
{
    auto waiters = std::vector<std::coroutine_handle<>> {};
    auto aDone = false;
    auto aCancelled = false;
    auto bDone = false;
    auto bCancelled = false;
    auto winner = coro::detail::WhenAnyNoWinner;

    auto root = raceTwo(&waiters, &aDone, &aCancelled, &bDone, &bCancelled, &winner);
    root.handle().resume();

    // Both children parked on their ManualEvent.
    REQUIRE(waiters.size() == 2);
    REQUIRE_FALSE(root.done());

    // Finish the first child: it wins and requests stop on the losers, but the parent
    // does NOT resume yet — whenAny joins until every child has finished, so the
    // awaiter (owning all child frames) outlives the still-parked loser.
    waiters[0].resume();
    REQUIRE(aDone);
    REQUIRE_FALSE(aCancelled);
    REQUIRE_FALSE(root.done()); // loser still parked; winner not yet reported to parent

    // Resume the loser: it observes the requested stop and unwinds via
    // OperationCancelled; being the last child, it tail-transfers back to the parent,
    // which then records the winner index.
    waiters[1].resume();
    REQUIRE(bCancelled);
    REQUIRE_FALSE(bDone);
    REQUIRE(root.done());
    REQUIRE(winner == 0);
}

TEST_CASE("whenAny completes synchronously when a child wins during start", "[whenAny]")
{
    auto waiters = std::vector<std::coroutine_handle<>> {};
    auto instantDone = false;
    auto parkedDone = false;
    auto parkedCancelled = false;
    auto winner = coro::detail::WhenAnyNoWinner;

    auto root = raceWithInstantWinner(&waiters, &instantDone, &parkedDone, &parkedCancelled, &winner);
    root.handle().resume();

    REQUIRE(root.done());
    REQUIRE(winner == 0);
    REQUIRE(instantDone);
    // The first child won synchronously and requested childStop before the second
    // started, so the second's ManualEvent sees the stop and resolves inline as
    // cancelled — it never parks (no entry in waiters) and never completes.
    REQUIRE(waiters.empty());
    REQUIRE(parkedCancelled);
    REQUIRE_FALSE(parkedDone);
}

TEST_CASE("whenAny over no tasks resolves to the no-winner sentinel", "[whenAny]")
{
    auto winner = std::size_t { 0 };
    auto root = [](std::size_t* w) -> Task<void> {
        *w = co_await whenAny(std::vector<Task<void>> {});
    }(&winner);
    root.handle().resume();

    REQUIRE(root.done());
    REQUIRE(winner == coro::detail::WhenAnyNoWinner);
}

#ifndef _WIN32
// Exception propagation through a coroutine frame crashes the Catch2 harness on
// Windows (the same MSVC coroutine-unwind interaction Task_test.cpp documents).
TEST_CASE("whenAny surfaces the winning child's exception", "[whenAny]")
{
    auto waiters = std::vector<std::coroutine_handle<>> {};
    auto threw = false;

    auto root = raceWithFailingWinner(&waiters, &threw);
    root.handle().resume();
    REQUIRE(waiters.size() == 1);

    waiters[0].resume(); // the only child fails → whenAny rethrows it

    REQUIRE(root.done());
    REQUIRE(threw);
}

TEST_CASE("whenAny throws OperationCancelled when the awaiting flow is cancelled", "[whenAny]")
{
    auto waiters = std::vector<std::coroutine_handle<>> {};
    auto aDone = false;
    auto aCancelled = false;
    auto bDone = false;
    auto bCancelled = false;
    auto winner = coro::detail::WhenAnyNoWinner;

    auto root = raceTwo(&waiters, &aDone, &aCancelled, &bDone, &bCancelled, &winner);
    auto source = coro::StopSource {};
    root.handle().promise().setStopToken(source.get_token());
    root.handle().resume();

    // Both children parked on their ManualEvent.
    REQUIRE(waiters.size() == 2);
    REQUIRE_FALSE(root.done());

    // Cancel the AWAITING flow (not a sibling winner): the parent→child bridge
    // requests the shared child stop, which both parked children observe on their
    // next resume. The first to unwind must NOT latch itself as the winner.
    source.request_stop();
    waiters[0].resume();
    waiters[1].resume();

    REQUIRE(aCancelled);
    REQUIRE(bCancelled);
    REQUIRE(root.done());
    // No child won, so await_resume throws OperationCancelled (surfaced through
    // the root task) instead of returning a cancelled loser's index.
    REQUIRE(winner == coro::detail::WhenAnyNoWinner);
    REQUIRE_THROWS_AS(root.result(), OperationCancelled);
}
#endif
