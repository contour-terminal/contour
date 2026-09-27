// SPDX-License-Identifier: Apache-2.0
#include <core/async/Awaitable.hpp>
#include <core/async/Cancellation.hpp>
#include <core/async/Task.hpp>
#include <core/async/WhenAll.hpp>

#include <catch2/catch_test_macros.hpp>

#include <coroutine>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

using core::async::StopSource;
using core::async::Task;
using core::async::whenAll;

/// Satisfied where `whenAll(task)` is a call, @p T being the value category of the argument. A
/// template, so that a call that does not resolve is a substitution failure rather than an error.
template <typename T>
concept WhenAllTakes = requires(T&& task) { whenAll(std::forward<T>(task)); };

// The variadic overload takes its tasks by rvalue. With the constraint written over
// std::remove_cvref_t an lvalue satisfied it too, and the diagnostic then came out of
// std::vector::push_back on Task's deleted copy constructor: a wall of errors from <vector> where
// "no matching overload" is what happened.
static_assert(WhenAllTakes<Task<void>>, "whenAll takes its tasks by rvalue");
static_assert(!WhenAllTakes<Task<void>&>,
              "an lvalue Task is not a whenAll argument: whenAll moves its tasks in");
static_assert(!WhenAllTakes<Task<void> const>,
              "a const Task is not a whenAll argument: it cannot be moved from");

// The awaiter moves. Upstream's did, `whenAll()` returns one by value, and only guaranteed
// copy-elision hid the loss -- `auto const a = whenAll(...)` compiles against an immovable type,
// so nothing in the suite noticed. What does not compile is passing one on, which is what a
// consumer wrapping whenAll() in a helper of its own writes first.
static_assert(std::is_move_constructible_v<decltype(whenAll(std::vector<Task<void>> {}))>,
              "the whenAll awaiter is movable");
static_assert(!std::is_copy_constructible_v<decltype(whenAll(std::vector<Task<void>> {}))>,
              "and not copyable: the join state and the runners' frames have one owner");

// fastcached#1546: MSVC 19.44's ARM64 code generator drops the enclosing `try` of a `co_await` on
// a temporary awaiter whose `await_ready` makes a call, so an `OperationCancelled` from
// `await_resume` passes every handler. An `await_ready` here answers a constant and the decision
// is `await_suspend`'s (.agent/rules/async-and-net.md); a call put back fails to compile wherever
// the question can be asked at compile time (core::async::awaitReadyIsConstantFalse).
static_assert(core::async::awaitReadyIsConstantFalse<decltype(whenAll(std::vector<Task<void>> {}))>());

namespace
{

/// An awaitable the test completes by hand (mirrors Task_test's ManualEvent).
struct ManualEvent
{
    std::vector<std::coroutine_handle<>>* waiters;

    [[nodiscard]] bool await_ready() const noexcept { return false; }

    void await_suspend(std::coroutine_handle<> waiting) const { waiters->push_back(waiting); }

    void await_resume() const noexcept {}
};

/// Throws synchronously the moment the child is started.
Task<void> throwImmediately(std::string message)
{
    throw std::runtime_error(message);
    co_return; // unreachable; makes this a coroutine
}

/// Unwinds cancelled the moment it is started, the way a child whose inherited token was already
/// stopped does.
Task<void> cancelImmediately()
{
    throw core::async::OperationCancelled {};
    co_return; // unreachable; makes this a coroutine
}

/// Parks on a ManualEvent, then increments a counter once resumed.
Task<void> waitThenIncrement(std::vector<std::coroutine_handle<>>* waiters, int* counter)
{
    co_await ManualEvent { waiters };
    ++*counter;
}

/// Parks on a ManualEvent, then records whether its inherited token was cancelled.
Task<void> waitThenRecordStop(std::vector<std::coroutine_handle<>>* waiters, std::vector<bool>* observed)
{
    auto const token = co_await core::async::thisCoroStopToken();
    co_await ManualEvent { waiters };
    observed->push_back(token.stop_requested());
}

// Drivers are free functions with pointer parameters, matching Task_test's rationale:
// lambda-coroutines dangle their closure, and reference coroutine parameters are
// forbidden by cppcoreguidelines-avoid-reference-coroutine-parameters.

/// Awaits a throwing child joined with a parked one; records what the join rethrows.
Task<void> joinThrowerAndWaiter(std::vector<std::coroutine_handle<>>* waiters,
                                int* counter,
                                std::string* caught)
{
    try
    {
        co_await whenAll(throwImmediately("boom"), waitThenIncrement(waiters, counter));
    }
    catch (std::runtime_error const& error)
    {
        *caught = error.what();
    }
}

/// Awaits two children that both throw synchronously; records the surviving message.
Task<void> joinTwoThrowers(std::string* caught)
{
    try
    {
        co_await whenAll(throwImmediately("first"), throwImmediately("second"));
    }
    catch (std::runtime_error const& error)
    {
        *caught = error.what();
    }
}

/// Awaits a cancelled child joined with a parked one; records whether the join rethrew the
/// cancellation.
Task<void> joinCancelledAndWaiter(std::vector<std::coroutine_handle<>>* waiters, int* counter, bool* caught)
{
    try
    {
        co_await whenAll(cancelImmediately(), waitThenIncrement(waiters, counter));
    }
    catch (core::async::OperationCancelled const&)
    {
        *caught = true;
    }
}

/// Awaits two parked children that observe their inherited cancellation token.
Task<void> joinTwoStopObservers(std::vector<std::coroutine_handle<>>* waiters, std::vector<bool>* observed)
{
    co_await whenAll(waitThenRecordStop(waiters, observed), waitThenRecordStop(waiters, observed));
}

} // namespace

TEST_CASE("whenAll rethrows a child exception only after every child finished", "[WhenAll]")
{
    auto waiters = std::vector<std::coroutine_handle<>> {};
    auto counter = 0;
    auto caught = std::string {};
    auto root = joinThrowerAndWaiter(&waiters, &counter, &caught);

    root.handle().resume();

    // The first child already threw, but the join must NOT resume the parent while
    // the second child is still parked: whenAll does not cancel siblings on failure.
    REQUIRE(waiters.size() == 1);
    REQUIRE_FALSE(root.done());
    REQUIRE(caught.empty());

    waiters[0].resume(); // the surviving sibling completes -> join resumes the parent

    REQUIRE(root.done());
    REQUIRE(counter == 1); // the sibling ran to completion despite the earlier throw
    REQUIRE(caught == "boom");
}

TEST_CASE("whenAll surfaces the FIRST child exception when several throw", "[WhenAll]")
{
    auto caught = std::string {};
    auto root = joinTwoThrowers(&caught);

    root.handle().resume();

    REQUIRE(root.done());
    REQUIRE(caught == "first");
}

TEST_CASE("whenAll rethrows a child's cancellation like any other escape", "[WhenAll]")
{
    // `whenAny` tells a cancelled child from a failed one, because it decides a winner on the
    // difference; `whenAll` does not, and must not start: a child whose inherited token was
    // stopped unwinds into the join, and the awaiting coroutine sees it. One runner serves both
    // combinators, so the case that says which of them classifies belongs here.
    auto waiters = std::vector<std::coroutine_handle<>> {};
    auto counter = 0;
    auto caught = false;
    auto root = joinCancelledAndWaiter(&waiters, &counter, &caught);

    root.handle().resume();

    REQUIRE(waiters.size() == 1);
    REQUIRE_FALSE(root.done());
    REQUIRE_FALSE(caught);

    waiters[0].resume(); // the surviving sibling completes -> the join resumes the parent

    REQUIRE(root.done());
    CHECK(counter == 1);
    CHECK(caught);
}

TEST_CASE("whenAll children inherit the awaiting coroutine's stop token", "[WhenAll]")
{
    auto waiters = std::vector<std::coroutine_handle<>> {};
    auto observed = std::vector<bool> {};
    auto source = StopSource {};

    auto root = joinTwoStopObservers(&waiters, &observed);
    root.handle().promise().setStopToken(source.get_token());

    root.handle().resume();
    REQUIRE(waiters.size() == 2);

    // Cancel while both children are parked: each child's own token (obtained via
    // thisCoroStopToken BEFORE parking) must observe the request, proving the token
    // flowed root -> whenAll runner -> child task rather than defaulting.
    source.request_stop();
    waiters[0].resume();
    waiters[1].resume();

    REQUIRE(root.done());
    REQUIRE(observed == std::vector<bool> { true, true });
}
