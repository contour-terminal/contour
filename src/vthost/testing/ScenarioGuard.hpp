// SPDX-License-Identifier: Apache-2.0
#pragma once

/// @file
/// Keeps a test scenario that fails, or never finishes, from hanging the suite.
///
/// A scenario runs beside the parties it drives -- a client's and a daemon's run() -- under whenAll, which
/// waits for every child and cancels none when one throws. A scenario that leaves early, by a REQUIRE that
/// throws or a lookup of a session that never arrived, skips its own detach(), and the parties then wait for
/// each other forever with blockOn parked under them: the suite hangs instead of failing, and names nothing.

#include <core/async/Task.hpp>
#include <core/async/WhenAll.hpp>
#include <core/net/EventLoop.hpp>

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <exception>
#include <functional>
#include <utility>

namespace vthost::testing
{

/// The longest a scenario may take before the parties beside it are ended under it.
inline constexpr auto ScenarioDeadline = std::chrono::milliseconds { std::chrono::seconds { 30 } };

namespace detail
{
    /// Runs @p scenario; when it throws, ends the parties and rethrows, so whenAll can return and report it.
    inline core::async::Task<void> endPartiesOnFailure(core::async::Task<void> scenario,
                                                       std::function<void()> const* endParties,
                                                       bool* finished)
    {
        auto failure = std::exception_ptr {};
        try
        {
            co_await std::move(scenario);
        }
        catch (...)
        {
            failure = std::current_exception();
        }
        *finished = true;
        if (!failure)
            co_return;
        (*endParties)();
        std::rethrow_exception(failure);
    }

    /// Ends the parties once @p deadline passes with the scenario still running: the bound on a scenario that
    /// awaits something that never comes.
    inline core::async::Task<void> scenarioWatchdog(core::net::EventLoop* loop,
                                                    std::function<void()> const* endParties,
                                                    bool const* finished,
                                                    std::chrono::milliseconds deadline)
    {
        auto const expiry = loop->clock().now() + deadline;
        while (!*finished && loop->clock().now() < expiry)
            co_await loop->delay(std::chrono::milliseconds { 10 });
        if (*finished)
            co_return;
        FAIL_CHECK("the scenario did not finish in time");
        (*endParties)();
    }
} // namespace detail

/// Runs @p scenario so that, however it ends, the parties beside it end too.
/// @param loop The loop everything runs on; its clock times the deadline.
/// @param endParties Ends the parties the scenario runs beside, so that their run()s return -- a client's
///        detach(). Called when the scenario throws, or when it outlives @p deadline.
/// @param scenario The scenario, which ends the parties itself when it succeeds.
/// @param deadline How long the scenario may run before it is failed and the parties are ended under it.
/// @return A task that completes with the scenario, rethrowing what it threw.
inline core::async::Task<void> guardScenario(core::net::EventLoop* loop,
                                             std::function<void()> endParties,
                                             core::async::Task<void> scenario,
                                             std::chrono::milliseconds deadline = ScenarioDeadline)
{
    auto finished = false;
    co_await core::async::whenAll(detail::endPartiesOnFailure(std::move(scenario), &endParties, &finished),
                                  detail::scenarioWatchdog(loop, &endParties, &finished, deadline));
}

} // namespace vthost::testing
