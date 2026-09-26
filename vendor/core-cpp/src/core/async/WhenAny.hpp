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
///
/// The runner, the counter and the start phase are [`Join.hpp`](Join.hpp)'s, shared with
/// @c whenAll — including the reason the join state is reference-counted, which this combinator
/// is what pays for: requesting stop on the child source can unwind the whole race and destroy
/// this awaiter while that `request_stop()` is still on the stack.

#include <core/async/Awaitable.hpp>
#include <core/async/Cancellation.hpp>
#include <core/async/Join.hpp>
#include <core/async/StopToken.hpp>
#include <core/async/Task.hpp>

#include <cstddef>
#include <exception>
#include <memory>
#include <optional>
#include <type_traits>
#include <utility>
#include <vector>

namespace core::async
{

namespace detail
{

    /// What `whenAny` latches on top of a join: the winner, and the source that unwinds the rest.
    struct WhenAnyState: JoinState
    {
        std::optional<std::size_t> winner; ///< The first child to complete; the latch as well.
        StopSource childStop;              ///< request_stop() cancels the losing children.
    };

    /// The parent→child cancellation bridge: the callback registered on the awaiting coroutine's
    /// own token, which requests stop on the shared child source.
    ///
    /// A named functor rather than a lambda in a `StopCallback<std::function<void()>>`: one
    /// pointer of state needs neither an allocation nor an indirect call. It holds the race state
    /// by @c shared_ptr and takes a copy of that pointer before it requests stop, because the very
    /// request can end the race, unwind the awaiting coroutine and destroy this callback — the
    /// copy on this stack frame is then all that keeps the child source alive until
    /// @c request_stop() returns.
    class WhenAnyCancelBridge
    {
      public:
        /// @param state The race state to request stop on.
        explicit WhenAnyCancelBridge(std::shared_ptr<WhenAnyState> state) noexcept: _state(std::move(state))
        {
        }

        /// Requests stop on the child source, holding the state alive across the call.
        void operator()() const noexcept
        {
            auto const held = _state;
            held->childStop.request_stop();
        }

      private:
        std::shared_ptr<WhenAnyState> _state;
    };

    /// `whenAny`'s policy: the first child to COMPLETE wins and cancels the rest.
    struct WhenAnyPolicy
    {
        using State = WhenAnyState;
        using ParentRegistration = std::optional<StopCallback<WhenAnyCancelBridge>>;

        /// Chains the awaiting flow's cancellation into the child source, so cancelling the parent
        /// cancels every child. A token with no stop state never runs the callback, so this is
        /// armed unconditionally.
        /// @param registration Where the callback lives.
        /// @param state The race state the callback stops.
        /// @param parent The awaiting flow's own token.
        static void armParentBridge(ParentRegistration& registration,
                                    std::shared_ptr<State> const& state,
                                    StopToken const& parent)
        {
            registration.emplace(parent, WhenAnyCancelBridge { state });
        }

        /// @return The shared child token, which the winner stops so the losers unwind.
        [[nodiscard]] static StopToken childToken(State& state, StopToken const& /*parent*/)
        {
            return state.childStop.get_token();
        }

        /// The first child to COMPLETE claims the win, propagates its failure (if any) to the
        /// shared state, and requests stop so the losers unwind.
        ///
        /// A child that unwound cancelled claims nothing: it is a loser, whether the winner
        /// cancelled it or the awaiting flow did. Telling the two apart is why the runner promise
        /// classifies what escaped its task instead of the body swallowing it — a body that
        /// swallowed its @c OperationCancelled would arrive here looking exactly like a child
        /// that ran to completion.
        /// @param state The shared race state.
        /// @param outcome What the child left behind.
        /// "First" is the first to CLAIM the latch rather than the first to find `winner` empty:
        /// two children completing on two threads would otherwise both read it empty, both write
        /// it, and both request stop.
        static void onChildFinished(State& state, ChildOutcome const& outcome) noexcept
        {
            if (outcome.cancelled || !state.claimLatch())
                return;
            state.winner = outcome.index;
            state.exception = outcome.escaped; // surface the winner's failure, if any
            state.childStop.request_stop();    // unwind the losing siblings
        }
    };

    /// Awaitable that starts every runner and resumes the awaiting coroutine once the first child
    /// completes, returning that child's index.
    class WhenAnyAwaiter final: public JoinAwaiter<WhenAnyPolicy>
    {
      public:
        using JoinAwaiter<WhenAnyPolicy>::JoinAwaiter;

        /// @return The index of the first task to complete, or nothing where none did:
        ///         an empty input, or every child unwound cancelled.
        /// @throws The winner's exception, if it failed; @c OperationCancelled if the
        ///         awaiting flow itself was cancelled and no child completed.
        [[nodiscard]] std::optional<std::size_t> await_resume() const
        {
            // Only where nothing won: a child that completed did so, and a cancellation
            // that arrives after it cannot undo it. `whenAny(readSocket(), timeout())`
            // whose read consumed bytes has nowhere to put them back, and
            // .agent/rules/async-and-net.md is explicit that the data wins. Where no
            // child completed, `winner` is empty -- a cancelled loser latches nothing --
            // and a stopped parent token is what says why.
            if (!state().winner.has_value() && parentToken().stop_requested())
                throw OperationCancelled {};
            if (state().exception)
                std::rethrow_exception(state().exception);
            return state().winner;
        }
    };

} // namespace detail

/// Runs all given tasks concurrently and completes when the FIRST completes,
/// cancelling the rest.
/// @param tasks The tasks to race (moved in). Losers are cancelled via a shared
///        child stop source, so each must unwind cleanly on @c OperationCancelled.
/// @return An awaitable; `co_await` it to suspend until the first task finishes.
///         It resolves to the winner's index, or to @c std::nullopt where no child
///         completed at all (an empty input, or every child unwound cancelled).
[[nodiscard]] inline auto whenAny(std::vector<Task<void>> tasks) -> detail::WhenAnyAwaiter
{
    return detail::WhenAnyAwaiter { std::move(tasks) };
}

/// Convenience overload: races the given tasks.
/// @param tasks The tasks to race (moved in).
/// @return An awaitable resolving to the index of the first task to complete, or to
///         @c std::nullopt where none did.
template <typename... Tasks>
    requires(sizeof...(Tasks) > 0 && (std::is_same_v<Tasks, Task<void>> && ...))
[[nodiscard]] auto whenAny(Tasks&&... tasks) -> detail::WhenAnyAwaiter
{
    auto vec = std::vector<Task<void>> {};
    vec.reserve(sizeof...(Tasks));
    (vec.push_back(std::forward<Tasks>(tasks)), ...);
    return detail::WhenAnyAwaiter { std::move(vec) };
}

} // namespace core::async
