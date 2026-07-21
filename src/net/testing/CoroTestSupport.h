// SPDX-License-Identifier: Apache-2.0
#pragma once

/// @file
/// Small coroutine helpers shared by the net/mux test suites: blockOn-able
/// adapters around EventLoop::delay and whenAll/whenAny (whose awaiters are not
/// Tasks), plus bounded readiness polling.

#include <chrono>
#include <utility>

#include <coro/Task.hpp>
#include <coro/WhenAll.hpp>
#include <coro/WhenAny.hpp>
#include <net/EventLoop.h>

namespace net::testing
{

/// Sleeps on the loop — a blockOn-able wrapper around EventLoop::delay.
inline coro::Task<void> sleepFor(EventLoop* loop, std::chrono::milliseconds duration)
{
    co_await loop->delay(duration);
}

/// Awaits every task — a blockOn-able adapter around coro::whenAll, so tests
/// compose their concurrent parties inline at the blockOn site.
template <typename... Tasks>
coro::Task<void> allOf(Tasks... tasks)
{
    co_await coro::whenAll(std::move(tasks)...);
}

/// Awaits the first task to finish (the losers are cancelled) — a blockOn-able
/// adapter around coro::whenAny; the winner's index is discarded.
template <typename... Tasks>
coro::Task<void> anyOf(Tasks... tasks)
{
    std::ignore = co_await coro::whenAny(std::move(tasks)...);
}

/// Polls @p ready one loop tick (1ms) at a time, bounded by @p maxTicks.
/// @return True when @p ready held before the budget ran out.
template <typename Predicate>
coro::Task<bool> waitUntil(EventLoop* loop, Predicate ready, int maxTicks = 1000)
{
    auto ticks = 0;
    while (!ready())
    {
        if (++ticks > maxTicks)
            co_return false;
        co_await loop->delay(std::chrono::milliseconds { 1 });
    }
    co_return true;
}

} // namespace net::testing
