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

/// Polls @p ready one loop tick (1ms) at a time, bounded by @p maxTicks.
/// @return True when @p ready held before the budget ran out.
template <typename Predicate>
async::Task<bool> waitUntil(EventLoop* loop, Predicate ready, int maxTicks = 1000)
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

} // namespace core::net::testing
