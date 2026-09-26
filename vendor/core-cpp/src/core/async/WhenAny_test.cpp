// SPDX-License-Identifier: Apache-2.0
#include <core/async/Awaitable.hpp>
#include <core/async/Cancellation.hpp>
#include <core/async/Task.hpp>
#include <core/async/WhenAny.hpp>

#include <catch2/catch_test_macros.hpp>

#include <coroutine>
#include <optional>
#include <stdexcept>
#include <type_traits>
#include <utility>
#include <vector>

using core::async::HasStopToken;
using core::async::OperationCancelled;
using core::async::StopCallback;
using core::async::StopToken;
using core::async::Task;
using core::async::whenAny;

/// Satisfied where `whenAny(task)` is a call, @p T being the value category of the argument. A
/// template, so that a call that does not resolve is a substitution failure rather than an error.
template <typename T>
concept WhenAnyTakes = requires(T&& task) { whenAny(std::forward<T>(task)); };

// The variadic overload takes its tasks by rvalue. With the constraint written over
// std::remove_cvref_t an lvalue satisfied it too, and the diagnostic then came out of
// std::vector::push_back on Task's deleted copy constructor: a wall of errors from <vector> where
// "no matching overload" is what happened.
static_assert(WhenAnyTakes<Task<void>>, "whenAny takes its tasks by rvalue");
static_assert(!WhenAnyTakes<Task<void>&>,
              "an lvalue Task is not a whenAny argument: whenAny moves its tasks in");
static_assert(!WhenAnyTakes<Task<void> const>,
              "a const Task is not a whenAny argument: it cannot be moved from");

// whenAny's awaiter is pinned where whenAll's moves, and the asymmetry is a property of the
// standard rather than a choice: this one owns a live `StopCallback` bridging the parent's
// cancellation into the children, and a registered stop callback is neither movable nor copyable
// -- the stop state holds its address. The shared base defaults its move, so each awaiter gets
// exactly what its own members allow; asserted here so that the day a bridge is redesigned, the
// question "may this move now?" is asked rather than answered by silence.
static_assert(!std::is_move_constructible_v<decltype(whenAny(std::vector<Task<void>> {}))>,
              "the whenAny awaiter is pinned by its registered StopCallback");
static_assert(!std::is_copy_constructible_v<decltype(whenAny(std::vector<Task<void>> {}))>,
              "and not copyable: the join state and the runners' frames have one owner");

namespace
{

/// An awaitable the test resumes by hand to control which child of a whenAny
/// finishes first. It captures the awaiting coroutine's stop token; on resume it
/// throws @c OperationCancelled if a cancel was requested while parked, so a losing
/// child unwinds exactly as a real runtime awaitable would.
struct ManualEvent
{
    std::vector<std::coroutine_handle<>>* waiters = nullptr;
    StopToken token {}; // filled in await_suspend; defaulted so call sites name only waiters

    [[nodiscard]] bool await_ready() const noexcept { return false; }

    template <typename Promise>
    [[nodiscard]] bool await_suspend(std::coroutine_handle<Promise> awaiting) noexcept
    {
        if constexpr (HasStopToken<Promise>)
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
///
/// A cancelled racer notes it and lets the OperationCancelled out, which is the contract whenAny
/// states for a loser: a child that swallows its cancellation and returns has, as far as the race
/// can tell, completed, and a completion is a win.
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
        throw;
    }
}

/// Resumes a parked coroutine, from wherever it is invoked. @c StopCallbackEvent registers one on
/// its token, so a stop request runs it and the coroutine is resumed from inside that request.
struct ResumeOnStop
{
    std::coroutine_handle<>* parked = nullptr;

    void operator()() const
    {
        if (auto const handle = std::exchange(*parked, {}))
            handle.resume();
    }
};

/// An awaitable that parks and delivers cancellation the way the runtime awaitables do: through a
/// @c StopCallback that resumes the parked coroutine then and there, rather than on a later manual
/// resume. The awaiter is a temporary in the parked coroutine's frame, so unwinding that frame
/// destroys the registration from inside its own callback — again what the real ones do.
///
/// @c ManualEvent above cannot stand in for this: it registers no callback, so every cancellation
/// it delivers is observed on a resume the test makes itself, with no callback on the stack.
struct StopCallbackEvent
{
    std::vector<std::coroutine_handle<>>* waiters = nullptr;
    StopToken token {};                // filled in await_suspend
    std::coroutine_handle<> parked {}; // the coroutine this awaitable holds, while it holds it
    std::optional<StopCallback<ResumeOnStop>> registration {};

    [[nodiscard]] bool await_ready() const noexcept { return false; }

    template <typename Promise>
    [[nodiscard]] bool await_suspend(std::coroutine_handle<Promise> awaiting)
    {
        if constexpr (HasStopToken<Promise>)
            token = awaiting.promise().stopToken();
        // A token already stopped resolves inline: registering would run the callback here, which
        // would resume a coroutine that has not finished suspending.
        if (token.stop_requested())
            return false;
        parked = awaiting;
        waiters->push_back(awaiting);
        registration.emplace(token, ResumeOnStop { .parked = &parked });
        return true;
    }

    void await_resume() const
    {
        if (token.stop_requested())
            throw OperationCancelled {};
    }
};

/// Records that the flow it is registered on was asked to stop.
struct NoteStop
{
    bool* ran = nullptr;

    void operator()() const noexcept { *ran = true; }
};

/// Parks on a StopCallbackEvent while holding a registration of its own on its token — the shape
/// of a cancellable operation that has cleanup to run when asked to stop.
///
/// That guard is what makes this a regression case rather than a re-run of the one above. It is a
/// local of the body and is registered BEFORE the event's, so it sits behind the event's in the
/// source's callback list: when the stop request takes the event's callback, the guard's is still
/// registered, and the request therefore comes back to its own state after that callback instead
/// of stopping at its last one. The guard itself never runs — the event's callback, registered
/// later and so run first, unwinds this body, which deregisters the guard on its way out.
Task<void> stopCallbackRacer(std::vector<std::coroutine_handle<>>* waiters, bool* cancelled, bool* guardRan)
{
    auto const token = co_await core::async::thisCoroStopToken();
    auto const guard = StopCallback<NoteStop> { token, NoteStop { .ran = guardRan } };
    try
    {
        co_await StopCallbackEvent { .waiters = waiters };
    }
    catch (OperationCancelled const&)
    {
        *cancelled = true;
        throw;
    }
}

/// Races two children that each deliver their cancellation from inside a stop callback, and
/// catches the whenAny cancellation in its own frame (so nothing is thrown through a coroutine
/// frame, which the Catch2 harness cannot take on Windows).
Task<void> raceTwoStopCallbackRacers(std::vector<std::coroutine_handle<>>* waiters,
                                     bool* aCancelled,
                                     bool* aGuardRan,
                                     bool* bCancelled,
                                     bool* bGuardRan,
                                     bool* parentCancelled)
{
    try
    {
        static_cast<void>(co_await whenAny(stopCallbackRacer(waiters, aCancelled, aGuardRan),
                                           stopCallbackRacer(waiters, bCancelled, bGuardRan)));
    }
    catch (OperationCancelled const&)
    {
        *parentCancelled = true;
    }
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
                   std::optional<std::size_t>* winner)
{
    *winner = co_await whenAny(racer(waiters, aDone, aCancelled), racer(waiters, bDone, bCancelled));
}

/// Drives whenAny over two manual racers, catching a cancellation in its own frame rather than
/// letting it out of the coroutine (which the Catch2 harness cannot take on Windows).
Task<void> raceTwoCatching(std::vector<std::coroutine_handle<>>* waiters,
                           bool* aDone,
                           bool* aCancelled,
                           bool* bDone,
                           bool* bCancelled,
                           std::optional<std::size_t>* winner,
                           bool* threwCancelled)
{
    try
    {
        *winner = co_await whenAny(racer(waiters, aDone, aCancelled), racer(waiters, bDone, bCancelled));
    }
    catch (OperationCancelled const&)
    {
        *threwCancelled = true;
    }
}

/// Drives whenAny where the first child completes synchronously.
Task<void> raceWithInstantWinner(std::vector<std::coroutine_handle<>>* waiters,
                                 bool* instantDone,
                                 bool* parkedDone,
                                 bool* parkedCancelled,
                                 std::optional<std::size_t>* winner)
{
    *winner = co_await whenAny(instantRacer(instantDone), racer(waiters, parkedDone, parkedCancelled));
}

} // namespace

TEST_CASE("whenAny resumes on the first child and cancels the loser", "[whenAny]")
{
    auto waiters = std::vector<std::coroutine_handle<>> {};
    auto aDone = false;
    auto aCancelled = false;
    auto bDone = false;
    auto bCancelled = false;
    auto winner = std::optional<std::size_t> {};

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
    auto winner = std::optional<std::size_t> {};

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

TEST_CASE("whenAny keeps a winner that already ran when the flow is cancelled after it", "[whenAny]")
{
    auto waiters = std::vector<std::coroutine_handle<>> {};
    auto aDone = false;
    auto aCancelled = false;
    auto bDone = false;
    auto bCancelled = false;
    auto threwCancelled = false;
    auto winner = std::optional<std::size_t> {};

    auto root = raceTwoCatching(&waiters, &aDone, &aCancelled, &bDone, &bCancelled, &winner, &threwCancelled);
    auto source = core::async::StopSource {};
    root.handle().promise().setStopToken(source.get_token());
    root.handle().resume();
    REQUIRE(waiters.size() == 2);

    // The first child completes — `whenAny(readSocket(), timeout())` where the read consumed
    // bytes. It wins and requests stop on the loser, which is still parked.
    waiters[0].resume();
    REQUIRE(aDone);
    REQUIRE_FALSE(root.done());

    // Only now is the awaiting flow cancelled, before the loser has unwound.
    source.request_stop();
    waiters[1].resume();

    REQUIRE(root.done());
    REQUIRE(bCancelled);
    // A cancellation that arrives after a child has completed does not undo that completion:
    // there is no way to hand the bytes back, so the winner is reported and the flow decides.
    REQUIRE_FALSE(threwCancelled);
    REQUIRE(winner == 0);
}

TEST_CASE("whenAny survives children resumed from inside the cancel bridge's own callback", "[whenAny]")
{
    auto waiters = std::vector<std::coroutine_handle<>> {};
    auto aCancelled = false;
    auto aGuardRan = false;
    auto bCancelled = false;
    auto bGuardRan = false;
    auto parentCancelled = false;

    auto root = raceTwoStopCallbackRacers(
        &waiters, &aCancelled, &aGuardRan, &bCancelled, &bGuardRan, &parentCancelled);
    auto source = core::async::StopSource {};
    root.handle().promise().setStopToken(source.get_token());
    root.handle().resume();

    REQUIRE(waiters.size() == 2);
    REQUIRE_FALSE(root.done());

    // Cancelling the awaiting flow runs whenAny's parent→child bridge, which requests stop on the
    // shared child source. Each child is resumed from inside THAT request's own callback and
    // unwinds; the last one transfers to the awaiting coroutine, whose await_resume() throws, so
    // the awaiter is destroyed — the bridge registration, the child frames and the child stop
    // source whose request_stop() is still on the stack — before the request returns. The race
    // state is reference-counted so that it outlives the call; with it a plain member, the rest of
    // request_stop() ran on freed memory (heap-use-after-free under AddressSanitizer wherever
    // StopToken is std::stop_token, whose state a raw pointer reaches).
    source.request_stop();

    // Each child's own registration was deregistered by its unwinding body, never run.
    REQUIRE_FALSE(aGuardRan);
    REQUIRE_FALSE(bGuardRan);
    REQUIRE(aCancelled);
    REQUIRE(bCancelled);
    REQUIRE(parentCancelled);
    REQUIRE(root.done());
}

TEST_CASE("whenAny over no tasks resolves to no winner", "[whenAny]")
{
    auto winner = std::optional<std::size_t> { 7 };
    auto root = [](std::optional<std::size_t>* w) -> Task<void> {
        *w = co_await whenAny(std::vector<Task<void>> {});
    }(&winner);
    root.handle().resume();

    REQUIRE(root.done());
    REQUIRE_FALSE(winner.has_value());
}

#ifndef _WIN32
// Exception propagation through a coroutine frame crashes the Catch2 harness on
// Windows (the same MSVC coroutine-unwind interaction Task_test.cpp documents).
namespace
{

/// A racer whose task throws (not cancellation) once resumed, to prove the winner's
/// failure surfaces through whenAny.
Task<void> failingRacer(std::vector<std::coroutine_handle<>>* waiters)
{
    co_await ManualEvent { .waiters = waiters };
    throw std::runtime_error("boom");
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
    auto winner = std::optional<std::size_t> {};

    auto root = raceTwo(&waiters, &aDone, &aCancelled, &bDone, &bCancelled, &winner);
    auto source = core::async::StopSource {};
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
    REQUIRE_FALSE(winner.has_value());
    REQUIRE_THROWS_AS(root.result(), OperationCancelled);
}
#else
// The case above cannot be compiled here, so it SAYS so rather than vanishing. A compiled-out
// case reports nothing at all: the Windows binary would list three fewer cases than the Linux one
// with nothing to explain the gap, and `.agent/rules/testing.md` asks for a `SKIP` precisely so
// that a case which could not run names itself and why. The `#ifndef` stays -- the reason for it
// is sound -- and only the silence goes.
TEST_CASE("whenAny surfaces the winning child's exception", "[whenAny]")
{
    SKIP("exception propagation through a coroutine frame crashes the Catch2 harness on MSVC");
}

TEST_CASE("whenAny throws OperationCancelled when the awaiting flow is cancelled", "[whenAny]")
{
    SKIP("exception propagation through a coroutine frame crashes the Catch2 harness on MSVC");
}
#endif
