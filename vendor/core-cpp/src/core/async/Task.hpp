// SPDX-License-Identifier: Apache-2.0
#pragma once

/// @file
/// `Task<T>` — a lazy, awaitable C++23 coroutine type.
///
/// `Task<T>` is the awaitable counterpart to a pull-style generator: where a
/// generator is a synchronous pull-range (`co_yield`, no `co_await`), a `Task`
/// is an asynchronous unit of work that may `co_await` other tasks and runtime
/// awaitables and eventually produces a single value (or `void`).
///
/// Design :
///  - **Lazy**: a freshly created task is suspended at `initial_suspend`, so a
///    `co_await` can attach its continuation before the body runs. This makes
///    composition deterministic on the single UI thread and lets an un-started
///    task be destroyed without ever running.
///  - **Symmetric transfer**: `final_suspend` tail-transfers to the awaiting
///    coroutine, so deep `co_await` chains (the event loop awaits the next event
///    thousands of times) do not grow the stack.
///  - **Single resumption**: a task is awaited (or driven by the runtime) once.
///  - **The awaiter owns what it awaits.** `operator co_await` is rvalue-qualified
///    and moves the frame out of the `Task` value into the awaiter, which lives in
///    the awaiting coroutine's frame for the whole suspension and destroys the frame
///    at the end of the `co_await` expression. So a named local awaited with
///    `std::move` is empty afterwards, and a chain can park inside the awaited task
///    while the name that produced it is somewhere the frame is not reachable from.
///  - **Ownership runs downward**, which is what lets an executor free an abandoned
///    chain from its root (`ParkedWork`): each frame's awaiter owns the frame it
///    awaits, and `unownedRoot` — set at every `await_suspend` — says whether the
///    chain bottoms out in a `DetachedTask`, which nobody owns.
///
/// Unlike a generator there is no `std::`-provided fallback to prefer: no
/// shipping standard library provides a usable `std::task`, so `Task` is always
/// hand-rolled. Only the core `<coroutine>` language support is required.

#include <core/async/Awaitable.hpp>
#include <core/async/Cancellation.hpp>
#include <core/async/ParkedWork.hpp>
#include <core/async/UniqueCoroHandle.hpp>

#include <coroutine>
#include <exception>
#include <optional>
#include <stdexcept>
#include <utility>

#if !defined(__cpp_impl_coroutine) || __cpp_impl_coroutine < 201902L
    #error "core::async::Task requires C++20 coroutine language support (__cpp_impl_coroutine)."
#endif

namespace core::async
{

namespace detail
{

    /// Awaiter returned from a task promise's `final_suspend` that tail-transfers
    /// control to the awaiting coroutine (or to `noop_coroutine` for a root task
    /// driven directly by the runtime).
    struct FinalAwaiter
    {
        // Coroutine awaiter/promise hooks stay non-static instance methods: their
        // names are fixed snake_case by the language, and the promise hooks must
        // be instance methods (a static initial_suspend/final_suspend would make
        // the compiler-generated promise.hook() calls trip static-accessed-through-
        // instance). readability-convert-member-functions-to-static would flag the
        // stateless ones anyway; it is disabled tree-wide in the root .clang-tidy.
        [[nodiscard]] bool await_ready() const noexcept { return false; }

        /// @return The continuation to resume via symmetric transfer.
        template <typename Promise>
        [[nodiscard]] std::coroutine_handle<> await_suspend(
            std::coroutine_handle<Promise> self) const noexcept
        {
            auto const continuation = self.promise().continuation;
            return continuation ? continuation : std::noop_coroutine();
        }

        void await_resume() const noexcept {}
    };

    /// State shared by every task promise regardless of result type: the awaiting
    /// continuation, a captured exception, and the cancellation token inherited
    /// from the awaiting coroutine.
    struct TaskPromiseBase
    {
        std::coroutine_handle<> continuation; ///< Resumed on completion (empty for a root).
        std::exception_ptr exception;         ///< Captured body exception, rethrown to the awaiter.
        StopToken token;                      ///< Inherited from the awaiting coroutine.

        /// The root of the await chain this coroutine belongs to, when that chain is owned by
        /// NOBODY — otherwise empty.
        ///
        /// Propagated downward at each `co_await`, so every frame in a chain carries the same
        /// answer and a parked coroutine can state it without walking a continuation chain whose
        /// links are type-erased. It is non-empty exactly where the chain bottoms out in a
        /// @c DetachedTask; a chain rooted in a `Task` object somebody holds keeps it empty,
        /// because that object's destructor is what frees the frame and a second owner would
        /// double-free. See [`ParkedWork`](ParkedWork.hpp).
        std::coroutine_handle<> unownedRoot;

        /// Start suspended so a continuation can be attached before the body runs.
        [[nodiscard]] std::suspend_always initial_suspend() const noexcept { return {}; }

        /// Suspend at the end and tail-transfer to the continuation.
        [[nodiscard]] FinalAwaiter final_suspend() const noexcept { return {}; }

        /// Captures an exception escaping the coroutine body for later rethrow.
        void unhandled_exception() noexcept { exception = std::current_exception(); }

        /// @return The cancellation token observed by this coroutine.
        [[nodiscard]] StopToken const& stopToken() const noexcept { return token; }

        /// Sets the cancellation token this coroutine (and tasks it awaits) observes.
        /// @param newToken The token to inherit.
        void setStopToken(StopToken newToken) noexcept { token = std::move(newToken); }
    };

    /// Refuses a result asked of a task that owns no coroutine frame.
    ///
    /// **This reports a PRECONDITION VIOLATION, not a recoverable error.** The caller asked a
    /// question with no true answer, and the only correct response is to fix the call. Do not
    /// catch it: a `try`/`catch` around `result()` turns "this task owns no frame" into a
    /// supported path, and the empty state then becomes something callers rely on rather than
    /// something they have got wrong. Nothing in `core::async` is declared to throw it, and no
    /// caller should handle it — it is an assertion that survives a Release build, which is the
    /// whole of why it is an exception.
    ///
    /// One function, so `Task<T>` and `Task<void>` and their awaiters refuse in the same words.
    /// It is a throw rather than `assert` because the empty state is one the type admits by
    /// design — default-constructed, moved from, released — and `done()` answers true for it, so
    /// the question is reachable through the documented guard rather than only through undefined
    /// behaviour; an `assert` would answer it with a silently wrong value in every Release build,
    /// which is the defect this removes. `std::optional::value()` answers the same question the
    /// same way, and for the same reason it is not `core::async::OperationCancelled`: that one is
    /// cancellation, a condition a flow is written to unwind through.
    /// @throws std::logic_error always, naming the condition.
    [[noreturn]] inline void refuseEmptyTask()
    {
        throw std::logic_error {
            "core::async::Task: the result of a task owning no coroutine frame was asked for. A "
            "default-constructed, moved-from or released task answers true to done() as well as a "
            "completed one, so done() alone is not the guard; such a task has no result, and a "
            "default-constructed value would be one the coroutine never produced."
        };
    }

} // namespace detail

/// A lazy, awaitable coroutine producing a single value of type @c T.
/// @tparam T The result type produced via `co_return` (use @c Task<void> for none).
template <typename T>
class [[nodiscard]] Task
{
  public:
    /// The coroutine promise; the standard looks up `Task<T>::promise_type`.
    struct PromiseType: detail::TaskPromiseBase
    {
        std::optional<T> result; ///< The produced value, populated by `co_return`.

        /// @return The owning task wrapping this coroutine.
        Task get_return_object() noexcept
        {
            return Task { std::coroutine_handle<PromiseType>::from_promise(*this) };
        }

        /// Stores the value produced by `co_return value;`.
        /// @param value The produced value, perfect-forwarded into storage.
        template <typename U = T>
            requires std::convertible_to<U&&, T>
        void return_value(U&& value)
        {
            result.emplace(std::forward<U>(value));
        }
    };

    using promise_type = PromiseType;
    using HandleType = std::coroutine_handle<PromiseType>;

    /// Awaiter produced by `co_await`-ing a task; owns the child, starts it and yields its result.
    ///
    /// It takes the frame from the rvalue `Task` that produced it and destroys it when the
    /// `co_await` expression ends, so nothing else can tear the child down while the awaiting
    /// coroutine is suspended inside it.
    class Awaiter
    {
      public:
        /// @param child The frame this awaiter takes over; may be empty.
        ///
        /// Whether there is anything to suspend for is decided HERE, and @c await_ready reads the
        /// answer: see @c await_ready for why neither of the other two places will do.
        explicit Awaiter(HandleType child) noexcept: _child(child), _ready(!child || child.done()) {}

        Awaiter(Awaiter&&) noexcept = default;
        Awaiter(Awaiter const&) = delete;
        Awaiter& operator=(Awaiter const&) = delete;
        Awaiter& operator=(Awaiter&&) = delete;
        ~Awaiter() = default;

        /// @return Whether there is nothing to suspend for -- no child, or one already finished --
        ///         as the constructor found it.
        ///
        /// **A member read, decided in the constructor**, because MSVC 19.44's ARM64 code generator
        /// loses the awaiting coroutine's handler in both of the other places
        /// ([fastcached#1546](https://github.com/LASTRADA-Software/fastcached/issues/1546)). Asked
        /// here -- a call in `await_ready` -- it dropped the enclosing `try` of a `co_await` on a
        /// temporary awaiter, and every `co_await task()` is that shape. Asked in @c await_suspend,
        /// which then transferred straight back to the awaiting coroutine, the `std::logic_error`
        /// @c await_resume throws for a task owning no frame passed that coroutine's `catch` all the
        /// same: the `windows (cl-release-arm64)` leg's `Task_test.cpp` case, with `await_ready` a
        /// constant `false`. Decided in the constructor, the awaiting coroutine never suspends,
        /// and @c await_resume throws inline, which is the shape fastcached measured keeping its
        /// handler. Nothing runs between the two: the awaiter is made by the `co_await` itself.
        [[nodiscard]] bool await_ready() const noexcept { return _ready; }

        /// Records the awaiting coroutine as the child's continuation, propagates the
        /// cancellation token and the chain's ownership down, and starts the child via
        /// symmetric transfer.
        ///
        /// Reached only where @c await_ready answered false, so there is a child and it has not
        /// finished; it never transfers back to @p awaiting.
        /// @param awaiting The coroutine performing the `co_await`.
        /// @return The child handle to resume.
        template <typename Promise>
        [[nodiscard]] std::coroutine_handle<> await_suspend(std::coroutine_handle<Promise> awaiting) noexcept
        {
            auto& promise = _child.get().promise();
            promise.continuation = awaiting;
            promise.unownedRoot = detail::unownedRootOf(awaiting);
            if constexpr (HasStopToken<Promise>)
                promise.setStopToken(awaiting.promise().stopToken());
            return _child.get();
        }

        /// @return The value produced by the child, or rethrows its exception.
        /// @throws std::logic_error if the awaited task owned no frame — a precondition
        ///         violation, not a recoverable error; see `detail::refuseEmptyTask()`.
        T await_resume()
        {
            if (!_child)
                detail::refuseEmptyTask();
            auto& promise = _child.get().promise();
            if (promise.exception)
                std::rethrow_exception(promise.exception);
            return std::move(*promise.result);
        }

      private:
        detail::UniqueCoroHandle<PromiseType> _child; ///< Owned: taken from the awaited rvalue Task.
        bool _ready; ///< No child, or one already finished, when the `co_await` began.
    };

    Task() noexcept = default;

    explicit Task(HandleType handle) noexcept: _handle(handle) {}

    Task(Task&&) noexcept = default;
    Task& operator=(Task&&) noexcept = default;
    Task(Task const&) = delete;
    Task& operator=(Task const&) = delete;
    ~Task() = default;

    /// Awaiting a task consumes it: the frame moves into the awaiter, which keeps it alive
    /// across the suspension (in the awaiting coroutine's frame) and destroys it at the end of
    /// the `co_await` expression. This value is empty afterwards.
    [[nodiscard]] Awaiter operator co_await() && noexcept { return Awaiter { _handle.release() }; }

    /// @return The underlying coroutine handle (for the runtime/driver to start
    /// and inspect a root task). Prefer `co_await` for composition.
    [[nodiscard]] HandleType handle() const noexcept { return _handle.get(); }

    /// Gives the frame up, so the caller owns it: this task is empty afterwards and destroys
    /// nothing. What is returned must be destroyed, or resumed to completion by something that
    /// frees it.
    /// @return The handle that was owned, or an empty one.
    [[nodiscard]] HandleType release() noexcept { return _handle.release(); }

    /// @return True once the coroutine has run to completion.
    [[nodiscard]] bool done() const noexcept { return !_handle || _handle.get().done(); }

    /// @return The result of a completed root task, rethrowing any body exception.
    /// @pre `done()` is true AND a frame is owned.
    /// @throws std::logic_error if no frame is owned. It reports the violated precondition and
    ///         is not to be caught; see `detail::refuseEmptyTask()`.
    ///
    /// The two halves of that precondition are separate on purpose: `done()` also answers true for
    /// a task owning NO frame (default-constructed, moved from, or released), so
    /// `if (t.done()) t.result();` is not by itself a safe guard. Such a task is refused by name
    /// rather than answered with a default-constructed value, which would be one the coroutine
    /// never produced — and which required every `T` to be default-constructible.
    [[nodiscard]] T result()
    {
        if (!_handle)
            detail::refuseEmptyTask();
        auto& promise = _handle.get().promise();
        if (promise.exception)
            std::rethrow_exception(promise.exception);
        return std::move(*promise.result);
    }

  private:
    detail::UniqueCoroHandle<PromiseType> _handle;
};

/// Specialization for tasks producing no value.
template <>
class [[nodiscard]] Task<void>
{
  public:
    /// The coroutine promise; the standard looks up `Task<void>::promise_type`.
    struct PromiseType: detail::TaskPromiseBase
    {
        /// @return The owning task wrapping this coroutine.
        Task get_return_object() noexcept
        {
            return Task { std::coroutine_handle<PromiseType>::from_promise(*this) };
        }

        void return_void() const noexcept {}
    };

    using promise_type = PromiseType;
    using HandleType = std::coroutine_handle<PromiseType>;

    /// Awaiter produced by `co_await`-ing a `Task<void>`; owns the child for the suspension.
    class Awaiter
    {
      public:
        /// @param child The frame this awaiter takes over; may be empty.
        explicit Awaiter(HandleType child) noexcept: _child(child), _ready(!child || child.done()) {}

        Awaiter(Awaiter&&) noexcept = default;
        Awaiter(Awaiter const&) = delete;
        Awaiter& operator=(Awaiter const&) = delete;
        Awaiter& operator=(Awaiter&&) = delete;
        ~Awaiter() = default;

        /// @return Whether there is nothing to suspend for, as the constructor found it -- for the
        ///         reason @c Task<T>::Awaiter::await_ready gives.
        [[nodiscard]] bool await_ready() const noexcept { return _ready; }

        /// Reached only where @c await_ready answered false; it never transfers back to @p awaiting.
        /// @param awaiting The coroutine performing the `co_await`.
        /// @return The child handle to resume.
        template <typename Promise>
        [[nodiscard]] std::coroutine_handle<> await_suspend(std::coroutine_handle<Promise> awaiting) noexcept
        {
            auto& promise = _child.get().promise();
            promise.continuation = awaiting;
            promise.unownedRoot = detail::unownedRootOf(awaiting);
            if constexpr (HasStopToken<Promise>)
                promise.setStopToken(awaiting.promise().stopToken());
            return _child.get();
        }

        /// Rethrows any exception escaping the child body.
        /// @throws std::logic_error if the awaited task owned no frame — a precondition
        ///         violation, not a recoverable error; see `detail::refuseEmptyTask()`.
        void await_resume() const
        {
            if (!_child)
                detail::refuseEmptyTask();
            if (_child.get().promise().exception)
                std::rethrow_exception(_child.get().promise().exception);
        }

      private:
        detail::UniqueCoroHandle<PromiseType> _child; ///< Owned: taken from the awaited rvalue Task.
        bool _ready; ///< No child, or one already finished, when the `co_await` began.
    };

    Task() noexcept = default;

    explicit Task(HandleType handle) noexcept: _handle(handle) {}

    Task(Task&&) noexcept = default;
    Task& operator=(Task&&) noexcept = default;
    Task(Task const&) = delete;
    Task& operator=(Task const&) = delete;
    ~Task() = default;

    /// Awaiting a task consumes it: the frame moves into the awaiter, which destroys it at the
    /// end of the `co_await` expression. This value is empty afterwards.
    [[nodiscard]] Awaiter operator co_await() && noexcept { return Awaiter { _handle.release() }; }

    [[nodiscard]] HandleType handle() const noexcept { return _handle.get(); }

    /// Gives the frame up, so the caller owns it: this task is empty afterwards.
    /// @return The handle that was owned, or an empty one.
    [[nodiscard]] HandleType release() noexcept { return _handle.release(); }

    [[nodiscard]] bool done() const noexcept { return !_handle || _handle.get().done(); }

    /// Rethrows any exception escaping a completed root task's body.
    /// @pre `done()` is true AND a frame is owned — `done()` alone is not that guard, since it also
    ///      answers true for a task owning no frame (default-constructed, moved from, released).
    /// @throws std::logic_error if no frame is owned, for the reason `Task<T>::result()` gives:
    ///         a task with no frame ran no body, so "it threw nothing" is not an answer about it.
    ///         It reports the violated precondition and is not to be caught.
    void result()
    {
        if (!_handle)
            detail::refuseEmptyTask();
        auto& promise = _handle.get().promise();
        if (promise.exception)
            std::rethrow_exception(promise.exception);
    }

  private:
    detail::UniqueCoroHandle<PromiseType> _handle;
};

} // namespace core::async
