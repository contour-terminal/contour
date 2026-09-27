// SPDX-License-Identifier: Apache-2.0
#pragma once

/// @file
/// The one runner, join state and awaiter that @c whenAll and @c whenAny are both written over.
///
/// The two combinators differ in exactly one step — what a child finishing does to the shared
/// state before the counter is decremented — and in what the awaiting coroutine is resumed with.
/// Everything else is the same: a wrapper coroutine per task, a count of live children plus a
/// start-phase guard, a token and a chain root pushed down into each child, and a tail transfer
/// from the last child to finish. This header holds that, parameterised by a *policy*; `WhenAll`
/// and `WhenAny` hold their policy, their `await_resume` and their public functions.
///
/// It is a public header whose whole content is `core::async::detail`, like
/// `UniqueCoroHandle.hpp`: a consumer includes `WhenAll.hpp` or `WhenAny.hpp`, which include this,
/// so it has to ship — but nothing in it is API.
///
/// A policy provides:
///
///   - `State`, derived from @c JoinState, holding whatever else the combinator latches;
///   - `ParentRegistration`, the storage for a callback on the awaiting flow's own token (an empty
///     struct where there is none);
///   - `armParentBridge(ParentRegistration&, std::shared_ptr<State> const&, StopToken const&)`;
///   - `childToken(State&, StopToken const& parent)`, the token every child observes;
///   - `onChildFinished(State&, ChildOutcome const&)`, the latch step.

#include <core/async/Awaitable.hpp>
#include <core/async/Cancellation.hpp>
#include <core/async/ParkedWork.hpp>
#include <core/async/StopToken.hpp>
#include <core/async/Task.hpp>
#include <core/async/UniqueCoroHandle.hpp>

#include <atomic>
#include <coroutine>
#include <cstddef>
#include <exception>
#include <memory>
#include <ranges>
#include <utility>
#include <vector>

namespace core::async::detail
{

/// What every join counts, whatever its policy latches on top.
///
/// **A join may span threads**, because this module ships what makes one: a child that awaits
/// `ResumeOn { pool }` finishes on a pool thread, so `remaining` is decremented from whichever
/// thread resumed that child. `IExecutor` says as much at its declaration, and a precondition
/// contradicted by the header beside it is one nobody keeps (controller ruling R98).
///
/// The memory ordering, which is what makes the two plain members below safe:
///
/// - `remaining` is `acq_rel` on every decrement, so the thread whose decrement returns 1 — the
///   last child to finish — acquires everything every other child released before its own.
/// - `continuation` is written once, before any child starts, and read only by that last thread.
///   It cannot be read early: the start phase holds a `+1` of its own, so no child's decrement can
///   reach zero until the starting thread has released it.
/// - `exception` is written only by whichever finisher claims `latched`, and read by the awaiting
///   coroutine, which the last decrement resumes.
struct JoinState
{
    std::atomic<std::size_t> remaining { 0 }; ///< Live children plus one for the start phase.
    std::coroutine_handle<> continuation;     ///< The join awaiter's coroutine.
    std::exception_ptr exception;             ///< Rethrown to the awaiting coroutine.

    /// Claimed once, by the first finisher whose policy has one-time work to do: `whenAll`'s first
    /// escape, `whenAny`'s first child to complete. Two children finishing on two threads would
    /// otherwise both read "nothing latched yet" and both write.
    std::atomic<bool> latched { false };

    /// @return Whether this call is the one that claimed the latch.
    [[nodiscard]] bool claimLatch() noexcept { return !latched.exchange(true, std::memory_order_acq_rel); }
};

/// What one child leaves behind for the policy to act on.
struct ChildOutcome
{
    std::size_t index = 0;      ///< The child's position in the input list.
    std::exception_ptr escaped; ///< Whatever left its task, cancellation included.
    bool cancelled = false;     ///< That escape was an @c OperationCancelled.
};

/// Tells a cancellation from a failure, for a join that has to act differently on each.
///
/// The classification happens where the exception is captured rather than in the runner's body: a
/// body that caught its own @c OperationCancelled would reach the final awaiter looking exactly
/// like a child that ran to completion, which is the difference @c whenAny decides a winner on.
/// @param escaped What left the child's task, or an empty pointer.
/// @return Whether that was an @c OperationCancelled.
[[nodiscard]] inline bool isCancellation(std::exception_ptr const& escaped) noexcept
{
    if (!escaped)
        return false;

    auto cancelled = false;
    try
    {
        std::rethrow_exception(escaped);
    }
    catch (OperationCancelled const&)
    {
        cancelled = true;
    }
    catch (...)
    {
        cancelled = false;
    }
    return cancelled;
}

/// The child wrapper coroutine: it awaits one task and, at its final suspension, lets the policy
/// act before decrementing the join counter.
///
/// Decrementing at `final_suspend` rather than resuming inline means the child that resumes the
/// awaiting coroutine is already suspended, so the awaiter — which owns every child frame — may
/// safely destroy them when it resumes.
/// @tparam Policy The combinator's policy; see the file comment.
template <typename Policy>
class JoinRunner
{
  public:
    using StateType = Policy::State;

    /// The coroutine promise; the standard looks up `JoinRunner<Policy>::promise_type`.
    struct PromiseType
    {
        std::shared_ptr<StateType> state; ///< Shared, so the state outlives every call on it.
        std::size_t index = 0;            ///< This runner's position in the input list.
        StopToken token;                  ///< What this child (and its task) observes.
        std::exception_ptr escaped;       ///< Whatever left the task, cancellation included.

        /// The root of the await chain, where that chain belongs to nobody; otherwise empty.
        /// Inherited from the awaiting coroutine like @c token: a runner is a coroutine type of
        /// its own between the awaiting chain and the task that parks, and one that did not carry
        /// the answer would make every park underneath a combinator read as *somebody owns this*
        /// (see [`ParkedWork`](ParkedWork.hpp)).
        std::coroutine_handle<> unownedRoot;

        bool cancelled = false; ///< Its task unwound on @c OperationCancelled.

        JoinRunner get_return_object() noexcept
        {
            return JoinRunner { std::coroutine_handle<PromiseType>::from_promise(*this) };
        }

        [[nodiscard]] std::suspend_always initial_suspend() const noexcept { return {}; }

        /// Final awaiter: the policy's latch step, then the counter, then — for the LAST child to
        /// finish — a tail transfer to the awaiting coroutine.
        struct FinalAwaiter
        {
            [[nodiscard]] bool await_ready() const noexcept { return false; }

            [[nodiscard]] std::coroutine_handle<> await_suspend(
                std::coroutine_handle<PromiseType> self) const noexcept
            {
                auto& promise = self.promise();
                // A copy, not a reference to the promise's: a policy that requests stop can unwind
                // the whole join and destroy this frame's owner, and the state has to outlive the
                // rest of this function.
                auto const join = promise.state;
                Policy::onChildFinished(*join,
                                        ChildOutcome { .index = promise.index,
                                                       .escaped = promise.escaped,
                                                       .cancelled = promise.cancelled });
                if (join->remaining.fetch_sub(1, std::memory_order_acq_rel) == 1 && join->continuation)
                    return join->continuation;
                return std::noop_coroutine();
            }

            void await_resume() const noexcept {}
        };

        [[nodiscard]] FinalAwaiter final_suspend() const noexcept { return {}; }

        /// Records what escaped the child's task, and sorts it into the two things it can be.
        ///
        /// **The one source.** A runner body that caught and recorded as well would give the same
        /// fact two owners, and a body that swallowed its @c OperationCancelled would reach the
        /// final awaiter looking exactly like a child that ran to completion — which is the
        /// difference `whenAny` decides a winner on.
        void unhandled_exception() noexcept
        {
            escaped = std::current_exception();
            cancelled = isCancellation(escaped);
        }

        void return_void() const noexcept {}

        /// @return The cancellation token observed by this runner (and by the task it awaits).
        [[nodiscard]] StopToken const& stopToken() const noexcept { return token; }
    };

    using promise_type = PromiseType;
    using HandleType = std::coroutine_handle<PromiseType>;

    explicit JoinRunner(HandleType handle) noexcept: _handle(handle) {}

    JoinRunner(JoinRunner&&) noexcept = default;
    JoinRunner& operator=(JoinRunner&&) noexcept = default;
    JoinRunner(JoinRunner const&) = delete;
    JoinRunner& operator=(JoinRunner const&) = delete;
    ~JoinRunner() = default;

    /// @return This runner's coroutine handle; owned here.
    [[nodiscard]] HandleType handle() const noexcept { return _handle.get(); }

  private:
    UniqueCoroHandle<PromiseType> _handle;
};

/// Wraps one task so it participates in a join.
/// @tparam Policy The combinator's policy.
/// @param task The work to run.
/// @return The runner, suspended at its initial suspension.
template <typename Policy>
JoinRunner<Policy> makeJoinRunner(Task<void> task)
{
    co_await std::move(task);
}

/// Awaitable that starts a runner per task and resumes the awaiting coroutine once the policy's
/// join is complete.
///
/// The state is held by @c shared_ptr — by this awaiter, by every runner promise and by whatever
/// bridge the policy arms — so that it outlives this awaiter wherever a stop callback of its own
/// brings the join to an end. Requesting stop runs the children's stop callbacks, and a runtime
/// awaitable resumes its coroutine from inside one: the losers unwind there and then, the last
/// transfers to the awaiting coroutine, and this awaiter is destroyed before that `request_stop()`
/// returns. Keeping a stop state alive across one's own `request_stop()` is the caller's job, and
/// neither `std::stop_source` nor core-cpp's fallback promises to do it.
/// @tparam Policy The combinator's policy; see the file comment.
template <typename Policy>
class JoinAwaiter
{
  public:
    using StateType = Policy::State;

    /// @param tasks The children, moved in.
    explicit JoinAwaiter(std::vector<Task<void>> tasks):
        _tasks(std::move(tasks)), _state(std::make_shared<StateType>())
    {
    }

    /// Movable, not copyable — the join state and the runners' frames have one owner.
    ///
    /// A move is only expressible *before* the awaiter suspends, because from `await_suspend` on
    /// it is a temporary of one `co_await` expression that nothing else can name. Nothing points
    /// back at it either: the runners hold the shared state, and the state holds the awaiting
    /// coroutine, so no member is an address of `*this`. Deleting the move made anything that
    /// passes one on ill-formed for no benefit — `auto h() { auto a = whenAll(…); return a; }`,
    /// or a container of them — while guaranteed copy-elision kept `co_await whenAll(…)` and
    /// `auto a = whenAll(…)` compiling, which is every call this repository writes. That is why
    /// nothing noticed.
    JoinAwaiter(JoinAwaiter&&) noexcept = default;
    JoinAwaiter& operator=(JoinAwaiter&&) noexcept = default;
    JoinAwaiter(JoinAwaiter const&) = delete;
    JoinAwaiter& operator=(JoinAwaiter const&) = delete;
    ~JoinAwaiter() = default;

    /// @return False. Whether there is anything to wait for is @c await_suspend's question:
    ///         asking a container is a call, and MSVC 19.44's ARM64 code generator drops the
    ///         enclosing `try` of a `co_await` on a temporary awaiter whose `await_ready` makes one
    ///         ([fastcached#1546](https://github.com/LASTRADA-Software/fastcached/issues/1546)).
    [[nodiscard]] constexpr bool await_ready() const noexcept { return false; }

    /// Builds and starts a runner per task, keeping the awaiting coroutine suspended unless the
    /// join completed during the start phase.
    ///
    /// No tasks declines to park before anything else is touched -- no continuation, no parent
    /// bridge, no token read -- which is exactly what `await_ready` answering true used to skip.
    /// @tparam Promise The awaiting coroutine's promise type.
    /// @param awaiting The coroutine performing the `co_await`.
    /// @return False where there are no tasks or every child finished synchronously, so the
    ///         awaiting coroutine resumes through the normal path.
    template <typename Promise>
    [[nodiscard]] bool await_suspend(std::coroutine_handle<Promise> awaiting)
    {
        if (_tasks.empty())
            return false;
        _state->continuation = awaiting;
        // Relaxed: no child has started, so no other thread can see this yet, and the guard's own
        // release below is what publishes it.
        _state->remaining.store(_tasks.size() + 1, std::memory_order_relaxed); // +1 guards the start

        if constexpr (HasStopToken<Promise>)
            _parentToken = awaiting.promise().stopToken();
        Policy::armParentBridge(_parentRegistration, _state, _parentToken);

        _runners.reserve(_tasks.size());
        for (auto& task: _tasks)
            _runners.push_back(makeJoinRunner<Policy>(std::move(task)));

        auto const childToken = Policy::childToken(*_state, _parentToken);
        auto const chainRoot = unownedRootOf(awaiting);
        for (auto const i: std::views::iota(std::size_t { 0 }, _runners.size()))
        {
            auto& promise = _runners[i].handle().promise();
            promise.state = _state;
            promise.index = i;
            promise.token = childToken;
            promise.unownedRoot = chainRoot;
            _runners[i].handle().resume();
        }

        // Release the start-phase guard. Where every child already finished, this is what reaches
        // zero, and the awaiting coroutine resumes inline rather than being transferred to.
        return _state->remaining.fetch_sub(1, std::memory_order_acq_rel) != 1;
    }

  protected:
    /// @return The shared join state. Non-const through a `shared_ptr`, like any pointee.
    [[nodiscard]] StateType& state() const noexcept { return *_state; }

    /// @return The awaiting flow's own token; empty where its promise carries none.
    [[nodiscard]] StopToken const& parentToken() const noexcept { return _parentToken; }

  private:
    std::vector<Task<void>> _tasks;           ///< Moved into runners on suspend.
    std::vector<JoinRunner<Policy>> _runners; ///< Kept alive until the join completes.
    std::shared_ptr<StateType> _state;        ///< Shared with the runners and any bridge.
    StopToken _parentToken;                   ///< The awaiting flow's own token.

    /// Declared LAST, so it is destroyed FIRST: a registered callback must be deregistered before
    /// what it reads goes away.
    Policy::ParentRegistration _parentRegistration;
};

/// The policy's answer where there is no callback on the awaiting flow's own token to arm.
struct NoParentRegistration
{
};

} // namespace core::async::detail
