// SPDX-License-Identifier: Apache-2.0
#include <catch2/catch_test_macros.hpp>

#include <coroutine>
#include <stdexcept>
#include <string>
#include <vector>

#include <coro/Cancellation.hpp>
#include <coro/Task.hpp>
#include <coro/WhenAll.hpp>

using coro::StopSource;
using coro::Task;
using coro::whenAll;

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

/// Parks on a ManualEvent, then increments a counter once resumed.
Task<void> waitThenIncrement(std::vector<std::coroutine_handle<>>* waiters, int* counter)
{
    co_await ManualEvent { waiters };
    ++*counter;
}

/// Parks on a ManualEvent, then records whether its inherited token was cancelled.
Task<void> waitThenRecordStop(std::vector<std::coroutine_handle<>>* waiters, std::vector<bool>* observed)
{
    auto const token = co_await coro::thisCoroStopToken();
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
