// SPDX-License-Identifier: Apache-2.0
#pragma once

/// @file
/// The current-executor context: which executor the calling thread is running a task for.
///
/// An awaitable whose resumption another thread triggers -- a queue a producer pushes to, a stop
/// callback -- has to hand the parked coroutine to SOME executor. Before this context existed each
/// one named its own (`AsyncQueue` the executor it was constructed over), so a coroutine running
/// on a strand that awaited such a thing came back on that foreign executor: off the strand, with
/// nothing to say so, racing whatever the strand serialises. morph found it in review of its
/// PR #806, where only morph's own awaiters knew to come back.
///
/// The fix is a fact the executor states while it runs a task, and that an awaitable reads once,
/// in `await_suspend`: `ExecutorScope` marks the thread, `ResumeTarget::currentOr` reads the mark
/// and holds on to it across the suspension. See `docs/design/strands.md`.

#include <core/async/IExecutor.hpp>
#include <core/async/ParkedWork.hpp>

#include <cassert>
#include <memory>
#include <utility>

namespace core::async
{

class ExecutorScope;

namespace detail
{
    /// The innermost @c ExecutorScope on the calling thread, or null.
    ///
    /// A function-local `thread_local` with a constant initialiser, so reading it is a TLS load and
    /// nothing more: no guard, no initialisation call. That is what lets an event loop state the
    /// context every turn at the cost of two stores
    /// ([`.agent/rules/async-and-net.md`](https://github.com/contour-terminal/core-cpp/blob/master/.agent/rules/async-and-net.md),
    /// "The resume context").
    /// @return A reference to the calling thread's slot.
    [[nodiscard]] inline ExecutorScope*& innermostExecutorScope() noexcept
    {
        constinit thread_local ExecutorScope* innermost = nullptr;
        return innermost;
    }
} // namespace detail

/// Marks the calling thread as running a task for an executor, for as long as it lives.
///
/// An executor that resumes coroutines constructs one around the resumption, or around a batch of
/// them: `core::net::EventLoop` once per turn, `ThreadPoolExecutor` once per worker thread,
/// `Strand` once per batch. Scopes nest -- a strand's batch runs inside the loop's turn that
/// drives it -- and each restores the one it replaced, on every exit including a throw.
///
/// **Never across a `co_await`.** It is a stack-shaped, thread-local guard, like a profiling zone:
/// a coroutine that suspends inside one resumes later, possibly on another thread, and the
/// destructor then restores a scope that belongs to somebody else's stack. Construct it in a
/// function that resumes coroutines, never in a coroutine body. The destructor asserts it.
///
/// Costs two thread-local stores and no allocation, whichever constructor is used.
///
/// The slot is an `inline` thread-local, so there is one per linked image: a program that compiles
/// these headers into two shared libraries with hidden visibility has two chains, and a scope stated
/// in one is not seen by an awaitable compiled into the other. core-cpp's own targets are static.
class ExecutorScope final
{
  public:
    /// Marks the thread as running tasks of @p executor.
    /// @param executor What `currentExecutor()` answers inside this scope. Must outlive it.
    explicit ExecutorScope(IExecutor& executor) noexcept: ExecutorScope(executor, nullptr, nullptr) {}

    /// Marks the thread as running tasks of @p executor, whose lifetime is shared.
    ///
    /// For an executor that can be released while a coroutine that ran on it is suspended
    /// somewhere else -- `KeyedStrands` reclaims an idle key's strand -- so that a
    /// @c ResumeTarget taken here keeps it alive until the coroutine has been handed back.
    /// @param executor What `currentExecutor()` answers inside this scope.
    /// @param keepAlive A reference a @c ResumeTarget taken here copies, or null for an executor
    ///        whose owner guarantees it outlives every coroutine that ran on it. Must outlive
    ///        this scope.
    /// @param family An address identifying a group of executors, for a query such as
    ///        `KeyedStrands::runningAnyHere`; null where there is none. Never dereferenced.
    ExecutorScope(IExecutor& executor, std::shared_ptr<void> const* keepAlive, void const* family) noexcept:
        _executor(&executor),
        _keepAlive(keepAlive),
        _family(family),
        _previous(std::exchange(detail::innermostExecutorScope(), this))
    {
    }

    ExecutorScope(ExecutorScope const&) = delete;
    ExecutorScope(ExecutorScope&&) = delete;
    ExecutorScope& operator=(ExecutorScope const&) = delete;
    ExecutorScope& operator=(ExecutorScope&&) = delete;

    /// Restores the scope this one replaced.
    ~ExecutorScope()
    {
        assert(detail::innermostExecutorScope() == this
               && "ExecutorScope destroyed out of order: it was held across a co_await, or moved "
                  "to another thread");
        detail::innermostExecutorScope() = _previous;
    }

    /// @return The innermost scope on the calling thread, or null outside every executor's task.
    [[nodiscard]] static ExecutorScope const* innermost() noexcept
    {
        return detail::innermostExecutorScope();
    }

    /// @return The executor this scope names.
    [[nodiscard]] IExecutor& executor() const noexcept { return *_executor; }

    /// @return What keeps the executor alive, or null where its owner does.
    [[nodiscard]] std::shared_ptr<void> const* keepAlive() const noexcept { return _keepAlive; }

    /// @return The family address this scope was given, or null.
    [[nodiscard]] void const* family() const noexcept { return _family; }

    /// @return The scope this one replaced, which is still in force below it; null for the
    ///         outermost.
    [[nodiscard]] ExecutorScope const* previous() const noexcept { return _previous; }

    /// Asks whether any scope in force on the calling thread, innermost first, satisfies
    /// @p matches.
    ///
    /// Every scope in force, not only the innermost: a task of a strand that resumes something
    /// synchronously is still inside the strand's task, and the strand still serialises it.
    /// @tparam Predicate Callable as `bool(ExecutorScope const&)`.
    /// @param matches The question.
    /// @return Whether one scope answered yes.
    template <typename Predicate>
    [[nodiscard]] static bool anyInForce(Predicate matches) noexcept(
        noexcept(matches(std::declval<ExecutorScope const&>())))
    {
        auto const* scope = innermost();
        while (scope != nullptr)
        {
            if (matches(*scope))
                return true;
            scope = scope->_previous;
        }
        return false;
    }

  private:
    IExecutor* _executor;
    std::shared_ptr<void> const* _keepAlive;
    void const* _family;
    ExecutorScope* _previous;
};

/// The executor the calling thread is running a task for, or null.
///
/// Valid for the synchronous part of the current task only: a coroutine that holds on to it
/// across a suspension holds a pointer to something that may be gone when it resumes -- an idle
/// `KeyedStrands` key's strand is reclaimed. Hold a @c ResumeTarget instead.
/// @return The innermost @c ExecutorScope's executor, or null outside every executor's task.
[[nodiscard]] inline IExecutor* currentExecutor() noexcept
{
    auto const* const scope = ExecutorScope::innermost();
    return scope != nullptr ? &scope->executor() : nullptr;
}

/// Where a parked coroutine is to be resumed: an executor, and what keeps it alive.
///
/// What an awaitable holds between `await_suspend` and the resumption another thread triggers.
/// Taken with @c currentOr, it names the executor the awaiting coroutine was running on, so the
/// coroutine comes back where it was rather than wherever the resumption was triggered; outside
/// every executor's task it names the fallback, which is the behaviour an awaitable had before the
/// context existed.
///
/// Copying one is a pointer copy, plus one reference-count increment where the executor's lifetime
/// is shared (a `KeyedStrands` key's strand) and none otherwise.
class ResumeTarget final
{
  public:
    /// An empty target, naming nothing.
    ResumeTarget() noexcept = default;

    /// A target naming @p executor, whose owner keeps it alive.
    /// @param executor Where to resume; must outlive every @c submit through this target.
    explicit ResumeTarget(IExecutor& executor) noexcept: _executor(&executor) {}

    ResumeTarget(ResumeTarget const&) = default;
    ResumeTarget& operator=(ResumeTarget const&) = default;

    /// Takes @p other's executor and what keeps it alive, leaving @p other empty: a target that
    /// kept the pointer without the keep-alive would name an executor it no longer holds.
    /// @param other The target to take.
    ResumeTarget(ResumeTarget&& other) noexcept:
        _executor(std::exchange(other._executor, nullptr)), _keepAlive(std::move(other._keepAlive))
    {
    }

    /// As the move constructor.
    /// @param other The target to take.
    /// @return This.
    ResumeTarget& operator=(ResumeTarget&& other) noexcept
    {
        if (this != &other)
        {
            _executor = std::exchange(other._executor, nullptr);
            _keepAlive = std::move(other._keepAlive);
        }
        return *this;
    }

    ~ResumeTarget() = default;

    /// @return The target of the calling thread's innermost @c ExecutorScope, or an empty one
    ///         outside every executor's task.
    [[nodiscard]] static ResumeTarget current() noexcept
    {
        auto target = ResumeTarget {};
        if (auto const* const scope = ExecutorScope::innermost())
        {
            target._executor = &scope->executor();
            if (auto const* const keepAlive = scope->keepAlive())
                target._keepAlive = *keepAlive;
        }
        return target;
    }

    /// @param fallback Where to resume when the calling thread is running no executor's task.
    /// @return The current target, or @p fallback.
    [[nodiscard]] static ResumeTarget currentOr(IExecutor& fallback) noexcept
    {
        auto target = current();
        if (!target)
            target._executor = &fallback;
        return target;
    }

    /// @return The executor this target names, or null.
    [[nodiscard]] IExecutor* executor() const noexcept { return _executor; }

    /// @return Whether this target names an executor.
    explicit operator bool() const noexcept { return _executor != nullptr; }

    /// Hands @p work to the executor this target names.
    /// @param work The coroutine to resume, and its claim on the chain root.
    void submit(ParkedWork work) const
    {
        assert(_executor != nullptr && "ResumeTarget::submit on an empty target");
        _executor->submit(std::move(work));
    }

  private:
    IExecutor* _executor { nullptr };
    std::shared_ptr<void> _keepAlive;
};

} // namespace core::async
