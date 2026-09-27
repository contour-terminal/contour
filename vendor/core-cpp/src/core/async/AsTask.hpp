// SPDX-License-Identifier: Apache-2.0
#pragma once

/// @file
/// `asTask` — wraps an awaiter in a @c Task, for the one caller shape an awaitable cannot serve.
///
/// `core::net`'s socket operations are awaitables rather than tasks on purpose: a `co_await` on
/// one allocates no coroutine frame, which is what lets a server hold a parked read per connection
/// without a frame per connection. The cost is that an awaitable is a **one-shot temporary bound
/// to its `co_await` expression** — it cannot be stored, moved into a container, handed to
/// @c whenAny, or kept across a suspension point, because nothing but the awaiting frame is
/// keeping it alive.
///
/// This is the escape hatch for exactly those callers, and it is deliberately an explicit call
/// rather than an implicit conversion: it costs a frame, and a caller should have to say so.

#include <core/async/Awaitable.hpp>
#include <core/async/Task.hpp>

#include <type_traits>
#include <utility>

namespace core::async
{

namespace detail
{

    /// What awaiting @p Aw resolves to.
    ///
    /// Read off `await_resume` rather than off a member typedef, so any awaiter works and none has
    /// to opt in by declaring one.
    /// @tparam Aw The awaiter type.
    template <typename Aw>
    using AwaitResultOf = decltype(std::declval<Aw&>().await_resume());

} // namespace detail

/// Wraps @p awaitable in a @c Task resolving to whatever awaiting it resolves to.
///
/// **The awaitable is stored in the returned task's frame, by value.** That is the whole point:
/// a caller that needs to keep an operation alive across a suspension point, put it in a
/// container, or hand it to a combinator gets an object whose lifetime it controls. Passing by
/// value rather than by reference is therefore load-bearing and not a style choice — a reference
/// parameter here would dangle the moment the caller's full-expression ended, which is the defect
/// this function exists to make impossible.
///
/// **The awaiting flow's stop token still reaches the operation.** The task is a pass-through: it
/// suspends on @p awaitable, and `Task`'s own awaiter hands the token of whoever awaits the task
/// down into the task's promise before resuming it, so a templated `await_suspend` reading
/// `promise().stopToken()` sees the token of the flow that awaits the TASK. A conversion that
/// broke that would leave a socket read un-cancellable, and the symptom is a read that never
/// unwinds rather than an error.
///
/// **It costs a coroutine frame**, which is what the awaitables exist to avoid, so it is spelled
/// as a call at the site that needs it rather than offered as a conversion every site gets.
///
/// @tparam Aw The awaiter type; taken by value, so a temporary is moved into the frame.
/// @param awaitable The operation to await. Its own lifetime contracts still apply — a socket
///        read's destination buffer must outlive the returned task, not merely this call.
/// @return A task resolving to `awaitable.await_resume()`'s type, `Task<void>` where that is
///         `void`, and propagating any exception `await_resume` throws (notably
///         @c OperationCancelled).
template <Awaiter Aw>
[[nodiscard]] Task<detail::AwaitResultOf<Aw>> asTask(Aw awaitable)
{
    // Two spellings because `co_return co_await x` does not compile for a void result, and a
    // `Task<void>` promise has `return_void()` rather than `return_value()`.
    if constexpr (std::is_void_v<detail::AwaitResultOf<Aw>>)
        co_await std::move(awaitable);
    else
        co_return co_await std::move(awaitable);
}

} // namespace core::async
