// SPDX-License-Identifier: Apache-2.0
#pragma once

/// @file
/// `sleepUntil(EventLoop*, tp)` and `nextWakeStep` — the free forms of the loop's deadline.
///
/// `EventLoop::sleepUntil` is the member a caller with a loop uses. This is the form for a caller
/// that may not have one: an in-memory transport, a test double, a decorator handed a null loop
/// because there is no deadline mechanism behind it. A null loop, or a deadline already gone,
/// resolves INLINE — `await_suspend()` declines to park and the flow resumes before `co_await`
/// returns, so nothing is filed with a loop that could not take it back. It is `await_suspend`
/// rather than `await_ready` that decides, for the reason @c DelayAwaiter gives
/// ([fastcached#1546](https://github.com/LASTRADA-Software/fastcached/issues/1546)).
///
/// Ported from fastcached's `Async/SleepUntil.hpp` at `0708dd54`, where it was the aggregate
/// `SleepUntil{&reactor, deadline}`. It is a call here because the awaitable it produces is the
/// loop's own @c DelayAwaiter, which already carries the cancellation registration an aggregate
/// would have had to grow.

#include <core/net/EventLoop.hpp>
#include <core/platform/Clock.hpp>

#include <algorithm>

namespace core::net
{

/// Suspends until @p deadline on @p loopOrNull's clock, or resolves inline where it cannot.
/// @param loopOrNull The loop whose clock and deadline heap to park on, or null to resolve
///        inline. Not owned; it must outlive the await.
/// @param deadline The absolute instant to resume at.
/// @return An awaitable that resumes at @p deadline; it never parks when @p loopOrNull is null
///         or @p deadline has already passed.
[[nodiscard]] inline DelayAwaiter sleepUntil(EventLoop* loopOrNull,
                                             platform::SteadyTimePoint deadline) noexcept
{
    return DelayAwaiter { loopOrNull, deadline };
}

/// The next instant a bounded wait should sleep to on its way to @p deadline.
///
/// **core-cpp itself no longer calls this, and that is the point of Task B5.** It is the
/// arithmetic of a wait that sleeps in steps and re-reads its own condition at each one, which is
/// what a caller had to do when a scheduled resumption could not be taken back. Here it can be
/// (@c EventLoop::cancelTimer, @c EventLoop::cancelPending, @c EventLoop::requestCancel), so
/// @c interruptibleSleepUntil and @c DeadlineTimer park once and are woken, and neither steps.
///
/// It is kept because a consumer migrating a bounded wait of its own — fastcached's `RaftDriver`
/// and `ExpiryReaper` each have one — needs the arithmetic while it is still a poll, and because
/// the deprecated four-argument @c interruptibleSleepUntil names the same parameter. It goes when
/// they do.
///
/// A non-positive @p wakeBound means "do not step", not "step by nothing": a zero-length step
/// resolves as already-ready and would spin the loop, so it sleeps straight through instead.
/// @param now The current instant on the relevant clock.
/// @param deadline The absolute instant the wait ends at.
/// @param wakeBound The longest single uninterruptible sleep; non-positive for one sleep straight
///        through.
/// @return The instant to sleep to next; never past @p deadline.
[[nodiscard]] constexpr platform::SteadyTimePoint nextWakeStep(platform::SteadyTimePoint now,
                                                               platform::SteadyTimePoint deadline,
                                                               platform::SteadyDuration wakeBound) noexcept
{
    return wakeBound > platform::SteadyDuration::zero() ? std::min(deadline, now + wakeBound) : deadline;
}

} // namespace core::net
