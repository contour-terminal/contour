// SPDX-License-Identifier: Apache-2.0
#pragma once

/// @file
/// `whenAny` — run several `Task<void>`s concurrently on one runtime and complete
/// as soon as the FIRST of them completes, cancelling the rest.
///
/// This is the select-style counterpart to @c whenAll: where `whenAll` waits for
/// every child and never cancels siblings, `whenAny` resumes the awaiting
/// coroutine with the index of the first child to finish and requests stop on a
/// shared child @c StopSource so the losing children unwind via
/// @c OperationCancelled. The losers must therefore be cancellation-safe (RAII
/// cleanup on @c OperationCancelled) — every runtime awaitable already is.
///
/// Cancellation propagates both ways: the children observe the shared child token
/// (so the winner cancels the losers), and a @c StopCallback on the awaiting
/// coroutine's own token chains into the child source (so cancelling the parent
/// cancels every child). The first child to reach its final suspension latches the
/// result and tail-transfers to the awaiting coroutine; later finishers are
/// no-ops, so the parent is resumed exactly once.

#include <coroutine>
#include <cstddef>
#include <exception>
#include <functional>
#include <limits>
#include <optional>
#include <ranges>
#include <type_traits>
#include <utility>
#include <vector>

#include <coro/Cancellation.hpp>
#include <coro/Task.hpp>
#include <coro/UniqueCoroHandle.hpp>

namespace coro
{

namespace detail
{

    /// Sentinel "no winner yet" index.
    inline constexpr std::size_t WhenAnyNoWinner = std::numeric_limits<std::size_t>::max();

    /// Shared join state for a `whenAny`. The first child to finish latches the
    /// winner index and requests stop so the losers unwind; the awaiting coroutine
    /// is resumed only once EVERY child has finished (winner completed + losers
    /// unwound), so the awaiter — which owns the child frames — outlives them all.
    /// @c remaining counts live children plus one start-phase guard.
    struct WhenAnyState
    {
        std::size_t remaining = 0;            ///< Live children plus the start-phase guard.
        std::size_t winner = WhenAnyNoWinner; ///< Index of the first child to finish.
        bool decided = false;                 ///< Latch: only the first finisher claims the win.
        std::coroutine_handle<> continuation; ///< The `whenAny` awaiter's coroutine.
        std::exception_ptr exception;         ///< Winner's exception, rethrown to the awaiter.
        StopSource childStop;                 ///< request_stop() cancels the losing children.
    };

    /// A child wrapper coroutine. It awaits one task, records the winner index and
    /// any exception at its final suspension, and — if it is the first to finish —
    /// cancels its siblings and tail-transfers to the awaiting coroutine.
    class WhenAnyRunner
    {
      public:
        struct PromiseType
        {
            WhenAnyState* state = nullptr; ///< Borrowed; outlives every runner.
            std::size_t index = 0;         ///< This runner's position in the input list.
            StopToken token;               ///< The shared child token (cancels losers).
            std::exception_ptr failure;    ///< This child's failure, if its task threw.

            WhenAnyRunner get_return_object() noexcept
            {
                return WhenAnyRunner { std::coroutine_handle<PromiseType>::from_promise(*this) };
            }

            [[nodiscard]] std::suspend_always initial_suspend() const noexcept { return {}; }

            /// Final awaiter: the first child to finish claims the win, propagates its
            /// failure (if any) to the shared state, and requests stop so the losers
            /// unwind. The LAST child to finish (winner or unwound loser) tail-transfers
            /// to the awaiting coroutine — so the awaiter, which owns every child frame,
            /// is not destroyed until no child is still parked.
            struct FinalAwaiter
            {
                [[nodiscard]] bool await_ready() const noexcept { return false; }

                [[nodiscard]] std::coroutine_handle<> await_suspend(
                    std::coroutine_handle<PromiseType> self) const noexcept
                {
                    auto& promise = self.promise();
                    auto* const state = promise.state;
                    if (!state->decided)
                    {
                        state->decided = true;
                        state->winner = promise.index;
                        state->exception = promise.failure; // surface the winner's failure, if any
                        state->childStop.request_stop();    // unwind the losing siblings
                    }
                    if (--state->remaining == 0 && state->continuation)
                        return state->continuation;
                    return std::noop_coroutine();
                }

                void await_resume() const noexcept {}
            };

            [[nodiscard]] FinalAwaiter final_suspend() const noexcept { return {}; }

            /// The runner body try/catches its task, so nothing should escape; capture
            /// defensively into this child's failure slot.
            void unhandled_exception() noexcept { failure = std::current_exception(); }

            void return_void() const noexcept {}

            /// @return The cancellation token observed by this runner (and its task).
            [[nodiscard]] StopToken const& stopToken() const noexcept { return token; }
        };

        using promise_type = PromiseType;
        using handle_type = std::coroutine_handle<PromiseType>;

        explicit WhenAnyRunner(handle_type handle) noexcept: _handle(handle) {}

        WhenAnyRunner(WhenAnyRunner&&) noexcept = default;
        WhenAnyRunner& operator=(WhenAnyRunner&&) noexcept = default;
        WhenAnyRunner(WhenAnyRunner const&) = delete;
        WhenAnyRunner& operator=(WhenAnyRunner const&) = delete;
        ~WhenAnyRunner() = default;

        [[nodiscard]] handle_type handle() const noexcept { return _handle.get(); }

      private:
        UniqueCoroHandle<PromiseType> _handle;
    };

    /// Wraps one task so it participates in the race. A child cancelled by the
    /// winner swallows its @c OperationCancelled (a loser, not a failure) and
    /// returns normally; any other exception escapes to the runner promise's
    /// @c unhandled_exception, which records it in this child's failure slot so the
    /// final awaiter can surface it if this child is the winner.
    /// @param task The work to run.
    inline WhenAnyRunner makeWhenAnyRunner(Task<void> task)
    {
        try
        {
            co_await std::move(task);
        }
        catch (OperationCancelled const&)
        {
            // A losing child cancelled by the winner: expected control flow, not a
            // failure. Swallow it so the runner returns normally and its final
            // awaiter runs (as a no-op, since the winner already latched).
            co_return;
        }
    }

    /// Awaitable that starts every runner and resumes the awaiting coroutine once
    /// the first child completes, returning that child's index.
    class WhenAnyAwaiter
    {
      public:
        explicit WhenAnyAwaiter(std::vector<Task<void>> tasks): _tasks(std::move(tasks)) {}

        [[nodiscard]] bool await_ready() const noexcept { return _tasks.empty(); }

        /// Builds and starts a runner per task; keeps the awaiting coroutine
        /// suspended unless a child completes synchronously during start.
        /// @param awaiting The coroutine performing `co_await whenAny(...)`.
        /// @return False if a child already won synchronously (resume immediately).
        template <typename Promise>
        [[nodiscard]] bool await_suspend(std::coroutine_handle<Promise> awaiting)
        {
            _state.continuation = awaiting;
            _state.remaining = _tasks.size() + 1; // +1 start-phase guard

            // Chain parent cancellation into the child source so cancelling the
            // awaiting flow cancels every child.
            if constexpr (requires { awaiting.promise().stopToken(); })
            {
                _parentToken = awaiting.promise().stopToken();
                _parentReg.emplace(_parentToken, [state = &_state] { state->childStop.request_stop(); });
            }

            _runners.reserve(_tasks.size());
            for (auto& task: _tasks)
                _runners.push_back(makeWhenAnyRunner(std::move(task)));

            auto const childToken = _state.childStop.get_token();
            for (auto const i: std::views::iota(std::size_t { 0 }, _runners.size()))
            {
                auto& promise = _runners[i].handle().promise();
                promise.state = &_state;
                promise.index = i;
                promise.token = childToken;
                _runners[i].handle().resume();
            }

            // Release the start-phase guard. If every child already finished
            // synchronously (the winner ran and the losers saw the stop and unwound
            // immediately), remaining hits zero here and we resume the parent inline.
            return --_state.remaining != 0;
        }

        /// @return The index of the first task to finish.
        /// @throws The winner's exception, if it failed; @c OperationCancelled if the
        ///         awaiting flow itself was cancelled before any child won.
        [[nodiscard]] std::size_t await_resume() const
        {
            // A cancelled awaiting flow sees NO winner: the child that latched
            // `decided` did so while unwinding through its own (swallowed)
            // OperationCancelled, so returning its index would report a cancelled
            // loser as a successful result and let the flow continue on its
            // success path. Cancellation dominates even a latched winner.
            if (_parentToken.stop_requested())
                throw OperationCancelled {};
            if (_state.exception)
                std::rethrow_exception(_state.exception);
            return _state.winner;
        }

      private:
        std::vector<Task<void>> _tasks;      ///< Moved into runners on suspend.
        std::vector<WhenAnyRunner> _runners; ///< Kept alive until the race completes.
        WhenAnyState _state {};              ///< Shared with the runners.
        StopToken _parentToken;              ///< The awaiting flow's own token (empty when it has none).
        std::optional<StopCallback<std::function<void()>>> _parentReg; ///< Parent→child cancel bridge.
    };

} // namespace detail

/// Runs all given tasks concurrently and completes when the FIRST completes,
/// cancelling the rest.
/// @param tasks The tasks to race (moved in). Losers are cancelled via a shared
///        child stop source, so each must unwind cleanly on @c OperationCancelled.
/// @return An awaitable; `co_await` it to suspend until the first task finishes.
///         It resolves to the winner's index (or @c detail::WhenAnyNoWinner if the
///         input was empty).
[[nodiscard]] inline auto whenAny(std::vector<Task<void>> tasks) -> detail::WhenAnyAwaiter
{
    return detail::WhenAnyAwaiter { std::move(tasks) };
}

/// Convenience overload: races the given tasks.
/// @param tasks The tasks to race (moved in).
/// @return An awaitable resolving to the index of the first task to finish.
template <typename... Tasks>
    requires(sizeof...(Tasks) > 0 && (std::is_same_v<std::remove_cvref_t<Tasks>, Task<void>> && ...))
[[nodiscard]] auto whenAny(Tasks&&... tasks) -> detail::WhenAnyAwaiter
{
    auto vec = std::vector<Task<void>> {};
    vec.reserve(sizeof...(Tasks));
    (vec.push_back(std::forward<Tasks>(tasks)), ...);
    return detail::WhenAnyAwaiter { std::move(vec) };
}

} // namespace coro
