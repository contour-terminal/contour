// SPDX-License-Identifier: Apache-2.0
#pragma once

/// @file
/// `whenAll` — run several `Task<void>`s concurrently on one runtime and
/// complete when all of them have completed.
///
/// The tasks are started together (each runs to its first suspension) rather
/// than awaited one-after-another, so on the single UI thread their suspensions
/// overlap — wall-clock is the slowest task, not the sum. This is how the
/// agent-mode loop runs its input pump and its agent-message pump together.
///
/// Cancellation: each child inherits the awaiting coroutine's @c StopToken, so
/// cancelling that token unwinds all children. `whenAll` does not itself cancel
/// siblings when one throws; the first exception is captured and rethrown once
/// every child has finished. Pair it with a shared token when you need
/// one-fails-all-stop semantics.

#include <coroutine>
#include <cstddef>
#include <exception>
#include <utility>
#include <vector>

#include <coro/Cancellation.hpp>
#include <coro/Task.hpp>
#include <coro/UniqueCoroHandle.hpp>

namespace coro
{

namespace detail
{

    /// Shared join state for a `whenAll`: how many children are still running,
    /// the awaiting coroutine to resume when the count reaches zero, and the
    /// first exception (if any) observed across the children.
    struct WhenAllState
    {
        std::size_t remaining = 0;            ///< Live children plus one for the start phase.
        std::coroutine_handle<> continuation; ///< The `whenAll` awaiter's coroutine.
        std::exception_ptr exception;         ///< First child exception, rethrown to the awaiter.
    };

    /// A child wrapper coroutine. It awaits one task, records any exception, and
    /// at its final suspension decrements the shared counter — the last child to
    /// finish tail-transfers to the awaiting coroutine. Decrementing at
    /// `final_suspend` (rather than calling `resume()` inline) means the child
    /// that resumes the parent is already suspended, so the parent may safely
    /// destroy the child frames when it resumes.
    class WhenAllRunner
    {
      public:
        struct PromiseType
        {
            WhenAllState* state = nullptr; ///< Borrowed; outlives every runner.
            StopToken token;               ///< Inherited from the awaiting coroutine.

            WhenAllRunner get_return_object() noexcept
            {
                return WhenAllRunner { std::coroutine_handle<PromiseType>::from_promise(*this) };
            }

            [[nodiscard]] std::suspend_always initial_suspend() const noexcept { return {}; }

            /// Final awaiter that decrements the join counter and tail-transfers
            /// to the awaiting coroutine when this is the last child to finish.
            struct FinalAwaiter
            {
                [[nodiscard]] bool await_ready() const noexcept { return false; }

                [[nodiscard]] std::coroutine_handle<> await_suspend(
                    std::coroutine_handle<PromiseType> self) const noexcept
                {
                    auto* const state = self.promise().state;
                    if (--state->remaining == 0 && state->continuation)
                        return state->continuation;
                    return std::noop_coroutine();
                }

                void await_resume() const noexcept {}
            };

            [[nodiscard]] FinalAwaiter final_suspend() const noexcept { return {}; }

            /// A runner body never lets an exception escape (it try/catches the
            /// awaited task), so this is unreachable in practice; capture defensively.
            void unhandled_exception() const noexcept
            {
                if (state && !state->exception)
                    state->exception = std::current_exception();
            }

            void return_void() const noexcept {}

            /// @return The cancellation token observed by this runner (and the task it awaits).
            [[nodiscard]] StopToken const& stopToken() const noexcept { return token; }
        };

        using promise_type = PromiseType;
        using handle_type = std::coroutine_handle<PromiseType>;

        explicit WhenAllRunner(handle_type handle) noexcept: _handle(handle) {}

        WhenAllRunner(WhenAllRunner&&) noexcept = default;
        WhenAllRunner& operator=(WhenAllRunner&&) noexcept = default;
        WhenAllRunner(WhenAllRunner const&) = delete;
        WhenAllRunner& operator=(WhenAllRunner const&) = delete;
        ~WhenAllRunner() = default;

        [[nodiscard]] handle_type handle() const noexcept { return _handle.get(); }

      private:
        UniqueCoroHandle<PromiseType> _handle;
    };

    /// Wraps one task so it participates in the join (records exceptions, does not
    /// rethrow into the runner so the counter is always decremented).
    /// @param task The work to run.
    /// @param state The shared join state.
    inline WhenAllRunner makeWhenAllRunner(Task<void> task, WhenAllState* state)
    {
        try
        {
            co_await std::move(task);
        }
        catch (...)
        {
            if (!state->exception)
                state->exception = std::current_exception();
        }
    }

    /// Awaitable that starts every runner and resumes the awaiting coroutine once
    /// all of them complete.
    class WhenAllAwaiter
    {
      public:
        explicit WhenAllAwaiter(std::vector<Task<void>> tasks): _tasks(std::move(tasks)) {}

        [[nodiscard]] bool await_ready() const noexcept { return _tasks.empty(); }

        /// Builds and starts a runner per task; keeps the awaiting coroutine
        /// suspended unless every child completes synchronously.
        /// @param awaiting The coroutine performing the `co_await whenAll(...)`.
        /// @return False if all children finished synchronously (resume immediately).
        template <typename Promise>
        [[nodiscard]] bool await_suspend(std::coroutine_handle<Promise> awaiting)
        {
            _state.continuation = awaiting;
            _state.remaining = _tasks.size() + 1; // +1 guards the start phase.

            _runners.reserve(_tasks.size());
            for (auto& task: _tasks)
                _runners.push_back(makeWhenAllRunner(std::move(task), &_state));

            for (auto& runner: _runners)
            {
                // The runner body holds `state` for exception capture; its final
                // awaiter reads it through the promise, so wire both to the same
                // join state before starting the runner.
                runner.handle().promise().state = &_state;
                if constexpr (requires { awaiting.promise().stopToken(); })
                    runner.handle().promise().token = awaiting.promise().stopToken();
                runner.handle().resume();
            }

            // Release the start-phase guard; if every child already finished, do
            // not suspend (resume the awaiting coroutine immediately).
            return --_state.remaining != 0;
        }

        /// Rethrows the first child exception, if any.
        void await_resume() const
        {
            if (_state.exception)
                std::rethrow_exception(_state.exception);
        }

      private:
        std::vector<Task<void>> _tasks;      ///< Moved into runners on suspend.
        std::vector<WhenAllRunner> _runners; ///< Kept alive until the join completes.
        WhenAllState _state {};              ///< Shared with the runners.
    };

} // namespace detail

/// Runs all given tasks concurrently and completes when every task completes.
/// @param tasks The tasks to run together (moved in).
/// @return An awaitable; `co_await` it to suspend until all tasks finish.
[[nodiscard]] inline auto whenAll(std::vector<Task<void>> tasks) -> detail::WhenAllAwaiter
{
    return detail::WhenAllAwaiter { std::move(tasks) };
}

/// Convenience overload: runs the given tasks concurrently.
/// @param tasks The tasks to run together (moved in).
/// @return An awaitable that completes when all tasks finish.
template <typename... Tasks>
    requires(sizeof...(Tasks) > 0 && (std::is_same_v<std::remove_cvref_t<Tasks>, Task<void>> && ...))
[[nodiscard]] auto whenAll(Tasks&&... tasks) -> detail::WhenAllAwaiter
{
    auto vec = std::vector<Task<void>> {};
    vec.reserve(sizeof...(Tasks));
    (vec.push_back(std::forward<Tasks>(tasks)), ...);
    return detail::WhenAllAwaiter { std::move(vec) };
}

} // namespace coro
