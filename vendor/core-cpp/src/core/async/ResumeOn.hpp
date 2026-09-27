// SPDX-License-Identifier: Apache-2.0
#pragma once

/// @file
/// `ResumeOn` — an awaitable that continues the awaiting coroutine somewhere else.

#include <core/async/IExecutor.hpp>
#include <core/async/ParkedWork.hpp>

#include <coroutine>

namespace core::async
{

/// Awaitable that continues the awaiting coroutine on another executor.
///
/// `co_await ResumeOn { executor }` suspends and posts the handle through
/// @c IExecutor::submit (thread-safe); the coroutine resumes wherever that executor runs things.
/// It is how a freshly accepted connection is handed from an acceptor thread to the
/// single-threaded loop that will own it — so the connection's coroutine only ever runs on that
/// one thread — and how a blocking, seconds-long job is moved off a loop onto a pool, which is
/// the same move written the same way.
struct ResumeOn
{
    IExecutor& target; ///< Where the coroutine should continue.

    /// Never ready: always suspend, so the resumption happens on the target.
    [[nodiscard]] bool await_ready() const noexcept { return false; }

    /// Posts the handle to the target for resumption.
    ///
    /// Templated on the promise because an executor destroyed before it runs this has to be told
    /// whether anything else can free the chain, and only the parking coroutine's own promise type
    /// knows ([fastcached#1025](https://github.com/LASTRADA-Software/fastcached/issues/1025)).
    /// @tparam Promise The suspending coroutine's promise type.
    /// @param handle The suspended coroutine to resume there.
    template <typename Promise>
    void await_suspend(std::coroutine_handle<Promise> handle) const
    {
        target.submit(detail::parkedWorkFor(handle));
    }

    void await_resume() const noexcept {}
};

} // namespace core::async
