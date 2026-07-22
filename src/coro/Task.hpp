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
///  - The awaiter does **not** own the child handle; the owning `Task` value
///    does and destroys it on scope exit, so awaiting a task — whether a named
///    local or a temporary that lives across the suspend point in the awaiting
///    frame — never double-frees.
///
/// Unlike a generator there is no `std::`-provided fallback to prefer: no
/// shipping standard library provides a usable `std::task`, so `Task` is always
/// hand-rolled. Only the core `<coroutine>` language support is required.

#include <coroutine>
#include <exception>
#include <optional>
#include <utility>

#include <coro/Cancellation.hpp>
#include <coro/UniqueCoroHandle.hpp>

#if !defined(__cpp_impl_coroutine) || __cpp_impl_coroutine < 201902L
    #error "coro::Task requires C++20 coroutine language support (__cpp_impl_coroutine)."
#endif

namespace coro
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
        // instance). readability-convert-member-functions-to-static is therefore
        // disabled for this directory (see src/coro/.clang-tidy).
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
    using handle_type = std::coroutine_handle<PromiseType>;

    /// Awaiter produced by `co_await`-ing a task; starts the child and yields its result.
    class Awaiter
    {
      public:
        explicit Awaiter(handle_type child) noexcept: _child(child) {}

        /// @return True if there is nothing to suspend for (no child, or already done).
        [[nodiscard]] bool await_ready() const noexcept { return !_child || _child.done(); }

        /// Records the awaiting coroutine as the child's continuation, propagates
        /// the cancellation token down, and starts the child via symmetric transfer.
        /// @param awaiting The coroutine performing the `co_await`.
        /// @return The child handle to resume.
        template <typename Promise>
        [[nodiscard]] std::coroutine_handle<> await_suspend(std::coroutine_handle<Promise> awaiting) noexcept
        {
            _child.promise().continuation = awaiting;
            if constexpr (requires { awaiting.promise().stopToken(); })
                _child.promise().setStopToken(awaiting.promise().stopToken());
            return _child;
        }

        /// @return The value produced by the child, or rethrows its exception.
        T await_resume()
        {
            if (_child.promise().exception)
                std::rethrow_exception(_child.promise().exception);
            return std::move(*_child.promise().result);
        }

      private:
        handle_type _child; ///< Borrowed; owned and destroyed by the awaited Task value.
    };

    Task() noexcept = default;

    explicit Task(handle_type handle) noexcept: _handle(handle) {}

    Task(Task&&) noexcept = default;
    Task& operator=(Task&&) noexcept = default;
    Task(Task const&) = delete;
    Task& operator=(Task const&) = delete;
    ~Task() = default;

    /// Awaiting a task consumes it; the owning value keeps the frame alive across
    /// the suspension (for a temporary, in the awaiting coroutine's frame).
    [[nodiscard]] Awaiter operator co_await() && noexcept { return Awaiter { _handle.get() }; }

    /// @return The underlying coroutine handle (for the runtime/driver to start
    /// and inspect a root task). Prefer `co_await` for composition.
    [[nodiscard]] handle_type handle() const noexcept { return _handle.get(); }

    /// @return True once the coroutine has run to completion.
    [[nodiscard]] bool done() const noexcept { return !_handle || _handle.get().done(); }

    /// @return The result of a completed root task, rethrowing any body exception.
    /// @pre `done()` is true.
    [[nodiscard]] T result()
    {
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
    using handle_type = std::coroutine_handle<PromiseType>;

    /// Awaiter produced by `co_await`-ing a `Task<void>`.
    class Awaiter
    {
      public:
        explicit Awaiter(handle_type child) noexcept: _child(child) {}

        [[nodiscard]] bool await_ready() const noexcept { return !_child || _child.done(); }

        template <typename Promise>
        [[nodiscard]] std::coroutine_handle<> await_suspend(std::coroutine_handle<Promise> awaiting) noexcept
        {
            _child.promise().continuation = awaiting;
            if constexpr (requires { awaiting.promise().stopToken(); })
                _child.promise().setStopToken(awaiting.promise().stopToken());
            return _child;
        }

        /// Rethrows any exception escaping the child body.
        void await_resume() const
        {
            if (_child.promise().exception)
                std::rethrow_exception(_child.promise().exception);
        }

      private:
        handle_type _child; ///< Borrowed; owned and destroyed by the awaited Task value.
    };

    Task() noexcept = default;

    explicit Task(handle_type handle) noexcept: _handle(handle) {}

    Task(Task&&) noexcept = default;
    Task& operator=(Task&&) noexcept = default;
    Task(Task const&) = delete;
    Task& operator=(Task const&) = delete;
    ~Task() = default;

    [[nodiscard]] Awaiter operator co_await() && noexcept { return Awaiter { _handle.get() }; }

    [[nodiscard]] handle_type handle() const noexcept { return _handle.get(); }

    [[nodiscard]] bool done() const noexcept { return !_handle || _handle.get().done(); }

    /// Rethrows any exception escaping a completed root task's body.
    /// @pre `done()` is true.
    void result()
    {
        auto& promise = _handle.get().promise();
        if (promise.exception)
            std::rethrow_exception(promise.exception);
    }

  private:
    detail::UniqueCoroHandle<PromiseType> _handle;
};

} // namespace coro
