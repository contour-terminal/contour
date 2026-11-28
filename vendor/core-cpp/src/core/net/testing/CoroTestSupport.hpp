// SPDX-License-Identifier: Apache-2.0
#pragma once

/// @file
/// Small coroutine helpers shared by the net/mux test suites: blockOn-able
/// adapters around EventLoop::delay and whenAll/whenAny (whose awaiters are not
/// Tasks), plus bounded readiness polling.

#include <core/async/Task.hpp>
#include <core/async/WhenAll.hpp>
#include <core/async/WhenAny.hpp>
#include <core/net/EventLoop.hpp>

#include <chrono>
#include <utility>

namespace core::net::testing
{

/// Sleeps on the loop — a blockOn-able wrapper around EventLoop::delay.
inline async::Task<void> sleepFor(EventLoop* loop, std::chrono::milliseconds duration)
{
    co_await loop->delay(duration);
}

/// Awaits every task — a blockOn-able adapter around async::whenAll, so tests
/// compose their concurrent parties inline at the blockOn site.
template <typename... Tasks>
async::Task<void> allOf(Tasks... tasks)
{
    co_await async::whenAll(std::move(tasks)...);
}

/// Awaits the first task to finish (the losers are cancelled) — a blockOn-able
/// adapter around async::whenAny; the winner's index is discarded.
template <typename... Tasks>
async::Task<void> anyOf(Tasks... tasks)
{
    std::ignore = co_await async::whenAny(std::move(tasks)...);
}

/// Runs @p task on @p loop and gives up once @p limit has passed, stopping the task then: what
/// bounds a test's kernel wait, so a socket that never becomes ready fails one case rather than
/// hanging the binary into its TIMEOUT.
/// @param loop The loop.
/// @param task The flow to run.
/// @param limit How long it may take.
/// @return Whether @p task finished within @p limit.
[[nodiscard]] inline bool blockOnWithin(EventLoop& loop,
                                        async::Task<void> task,
                                        std::chrono::milliseconds limit)
{
    auto finished = false;
    auto run = [](async::Task<void> inner, bool* done) -> async::Task<void> {
        co_await std::move(inner);
        *done = true;
    };
    loop.blockOn(anyOf(run(std::move(task), &finished), sleepFor(&loop, limit)));
    return finished;
}

/// Polls @p ready about once a millisecond, bounded by @p maxMilliseconds of the loop's clock rather
/// than by a count of polls: a 1 ms delay lasts a timer tick, 15.6 ms on Windows, and a thousand of
/// them outlasted the bounds the callers run under.
/// @return True when @p ready held before the budget ran out.
template <typename Predicate>
async::Task<bool> waitUntil(EventLoop* loop, Predicate ready, int maxMilliseconds = 1000)
{
    auto const deadline = loop->clock().now() + std::chrono::milliseconds { maxMilliseconds };
    while (!ready())
    {
        if (loop->clock().now() >= deadline)
            co_return false;
        co_await loop->delay(std::chrono::milliseconds { 1 });
    }
    co_return true;
}

} // namespace core::net::testing
