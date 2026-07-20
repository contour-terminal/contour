// SPDX-License-Identifier: Apache-2.0
#pragma once

/// @file
/// Cancellation primitives for @c coro coroutines.
///
/// Cancellation uses the standard C++20 `<stop_token>` facility
/// (`std::stop_token` / `std::stop_source` / `std::stop_callback`). This header
/// prefers the standard types and exposes them under the @c coro namespace so
/// call sites are stable. Every standard library we target (libstdc++, MSVC STL,
/// and libc++ from LLVM 18) ships `<stop_token>`; if a future target platform
/// does not, add a minimal fallback here rather than changing call sites.

#include <coroutine>
#include <version>

#if defined(__cpp_lib_jthread) && __cpp_lib_jthread >= 201911L

    #include <stop_token>

namespace coro
{

/// Observes a cancellation request. Cheap to copy; shares state with its source.
using StopToken = std::stop_token;

/// Requests cancellation of all @c StopToken instances obtained from it.
using StopSource = std::stop_source;

/// Invokes a callback when cancellation is requested on the associated token.
/// @tparam Callback The invocable to run on cancellation.
template <typename Callback>
using StopCallback = std::stop_callback<Callback>;

} // namespace coro

#else
    #error "coro cancellation requires C++20 <stop_token> (__cpp_lib_jthread). " \
        "Add a minimal fallback in Cancellation.hpp if a target platform lacks it."
#endif

namespace coro
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
/// token. Usage: `auto token = co_await coro::thisCoroStopToken();`.
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
        if constexpr (requires { awaiting.promise().stopToken(); })
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

} // namespace coro
