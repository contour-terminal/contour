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
/// siblings when one throws; the first escape is captured and rethrown once
/// every child has finished. Pair it with a shared token when you need
/// one-fails-all-stop semantics.
///
/// The runner, the counter and the start phase are
/// [`Join.hpp`](Join.hpp)'s, shared with @c whenAny. What is here is the one step that differs:
/// the latch (there is none — every escape is the join's), the token each child observes, and
/// what the awaiting coroutine is resumed with.

#include <core/async/Awaitable.hpp>
#include <core/async/Cancellation.hpp>
#include <core/async/Join.hpp>
#include <core/async/StopToken.hpp>
#include <core/async/Task.hpp>

#include <exception>
#include <memory>
#include <type_traits>
#include <utility>
#include <vector>

namespace core::async
{

namespace detail
{

    /// `whenAll`'s policy: wait for every child, latch nothing, cancel nobody.
    struct WhenAllPolicy
    {
        /// The join needs nothing of its own: a `whenAll` has no winner and no child source.
        struct State: JoinState
        {
        };

        using ParentRegistration = NoParentRegistration;

        /// Nothing to arm: `whenAll` cancels no child of its own, and each child already observes
        /// the awaiting coroutine's token directly.
        static void armParentBridge(ParentRegistration& /*registration*/,
                                    std::shared_ptr<State> const& /*state*/,
                                    StopToken const& /*parent*/) noexcept
        {
        }

        /// @return The awaiting coroutine's own token, so cancelling that flow unwinds every child.
        [[nodiscard]] static StopToken childToken(State& /*state*/, StopToken const& parent) noexcept
        {
            return parent;
        }

        /// Records the FIRST escape, whatever it was.
        ///
        /// A cancellation counts: a child whose inherited token was stopped unwinds into the join
        /// like any other failure, and the awaiting coroutine sees it rethrown. `whenAny` is the
        /// one that has to tell the two apart, because it decides a winner on the difference.
        ///
        /// "First" is the first to CLAIM the latch, not the first in finish order, because two
        /// children can escape on two threads at once — `if (!state.exception)` on both of them
        /// reads "nothing yet" twice and writes twice. A child that escaped nothing claims
        /// nothing, or the first child to succeed would keep a later failure out.
        /// @param state The shared join state.
        /// @param outcome What the child left behind.
        static void onChildFinished(State& state, ChildOutcome const& outcome) noexcept
        {
            if (!outcome.escaped)
                return;
            if (state.claimLatch())
                state.exception = outcome.escaped;
        }
    };

    /// Awaitable that starts every runner and resumes the awaiting coroutine once all of them
    /// complete.
    class WhenAllAwaiter final: public JoinAwaiter<WhenAllPolicy>
    {
      public:
        using JoinAwaiter<WhenAllPolicy>::JoinAwaiter;

        /// Rethrows the first child escape, if any.
        void await_resume() const
        {
            if (state().exception)
                std::rethrow_exception(state().exception);
        }
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
    requires(sizeof...(Tasks) > 0 && (std::is_same_v<Tasks, Task<void>> && ...))
[[nodiscard]] auto whenAll(Tasks&&... tasks) -> detail::WhenAllAwaiter
{
    auto vec = std::vector<Task<void>> {};
    vec.reserve(sizeof...(Tasks));
    (vec.push_back(std::forward<Tasks>(tasks)), ...);
    return detail::WhenAllAwaiter { std::move(vec) };
}

} // namespace core::async
