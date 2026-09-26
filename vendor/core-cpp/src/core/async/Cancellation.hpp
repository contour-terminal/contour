// SPDX-License-Identifier: Apache-2.0
#pragma once

/// @file
/// Cancellation primitives for @c core::async coroutines.
///
/// Cancellation uses @c StopToken, @c StopSource and @c StopCallback from
/// `<core/async/StopToken.hpp>`: the standard `<stop_token>` facility where the
/// standard library has it, and core-cpp's fallback with the same semantics where
/// it does not. This header adds what a coroutine needs on top: the exception a
/// cancelled frame unwinds with, and an awaitable that yields the awaiting
/// coroutine's own token.

#include <core/async/Awaitable.hpp>
#include <core/async/StopToken.hpp>

#include <coroutine>

namespace core::async
{

/// Exception thrown into an awaiting coroutine frame when its operation is
/// cancelled, so the frame unwinds through ordinary RAII (restoring focus,
/// clearing prompts, etc.). Runtime awaitables throw this from @c await_resume
/// when their associated @c StopToken has @c stop_requested().
struct OperationCancelled
{
};

/// Awaitable that yields the @c StopToken of the awaiting coroutine without
/// suspending it. Lets a coroutine body observe its own cancellation token (e.g. to
/// poll @c stop_requested() inside a loop) when the promise carries one; coroutines
/// whose promise has no @c stopToken() accessor receive a default (never-stopped)
/// token. Usage: `auto token = co_await core::async::thisCoroStopToken();`.
struct ThisCoroStopToken
{
    StopToken token; ///< Filled from the awaiting promise in await_suspend.

    /// Never ready: await_suspend runs to capture the token, then resumes immediately.
    [[nodiscard]] bool await_ready() const noexcept { return false; }

    /// Captures the awaiting coroutine's stop token (if its promise exposes one) and
    /// resumes without actually suspending.
    /// @param awaiting The coroutine performing the co_await.
    /// @return false — do not suspend; resume @p awaiting immediately.
    template <typename Promise>
    bool await_suspend(std::coroutine_handle<Promise> awaiting) noexcept
    {
        if constexpr (HasStopToken<Promise>)
            token = awaiting.promise().stopToken();
        return false;
    }

    /// @return The captured stop token.
    [[nodiscard]] StopToken await_resume() const noexcept { return token; }
};

/// @return An awaitable yielding the awaiting coroutine's @c StopToken.
[[nodiscard]] inline ThisCoroStopToken thisCoroStopToken() noexcept
{
    return ThisCoroStopToken {};
}

} // namespace core::async
