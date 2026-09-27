// SPDX-License-Identifier: Apache-2.0
#pragma once

/// @file
/// `syncRun` — drive a `Task` to completion on the calling thread.
///
/// For tests, and for a `main()` body before an event loop exists. The task must be
/// self-driving: it may only await things that complete at once. A task still suspended when
/// `resume()` returns has no result to read, and — worse — destroying its frame then tears down
/// storage whatever parked the coroutine is still pointing into, which arrives as a SIGSEGV, a
/// heap corruption or an abort naming nothing. So the precondition is checked rather than
/// assumed, and @c syncRunWith exists for the one case where the park can be taken back first.
///
/// It is a header of its own, although fastcached keeps both in `Task.hpp`, for the reason
/// `DetachedTask.hpp` is: every piece of this module's vocabulary is a header of its own.

#include <core/async/Task.hpp>

#include <concepts>
#include <stdexcept>
#include <tuple>
#include <utility>

namespace core::async
{

/// Drives @p task to completion on this thread and returns its result.
/// @tparam T The task's result type.
/// @param task The task to drive; must own a frame.
/// @return The task's result (or void), rethrowing anything its body threw.
/// @throws std::logic_error if @p task owns no frame, or is still suspended after being resumed
///         — it awaited something this function cannot complete.
template <typename T>
T syncRun(Task<T> task)
{
    if (!task.handle())
        detail::refuseEmptyTask();

    task.handle().resume();
    if (!task.done())
        throw std::logic_error {
            "core::async::syncRun: the task is still suspended after resume(). It awaited "
            "something syncRun cannot complete (a socket read with no data and no closed peer, "
            "typically). Reading its result would be undefined behaviour, and freeing its frame "
            "would tear down storage whatever parked it still points into; syncRunWith() takes "
            "the park back first."
        };
    return task.result();
}

/// `syncRun` for a task that may PARK on something whose owner can take the park back — a socket
/// read that `cancelRead()` completes — with @p retrieve being how.
///
/// **The plain overload's refusal is only half of what such a task needs.** It throws, and the
/// throw is legible; but `~Task` then frees a frame the parked read still points into, the
/// resource's next completion writes into freed memory, and the case ends in a crash that names
/// no assertion at all — a red turned into a SIGSEGV. So this one retrieves the park FIRST, while
/// the frame is alive, and throws afterwards, when nothing points into it any more.
///
/// A callable rather than a resource type, because `core::async` names no socket.
///
/// A park @p retrieve did not wake is one this function still cannot finish, and freeing it would
/// be the defect this exists to remove, so its frame is deliberately LEAKED instead: a leak report
/// names the coroutine that parked, and a use-after-free names nothing.
/// @tparam T The task's result type.
/// @tparam Retrieve A callable taking no arguments.
/// @param task The task to drive; must own a frame.
/// @param retrieve What completes the task's park; called only where the task parked.
/// @return The task's result (or void), rethrowing anything its body threw.
/// @throws std::logic_error if @p task owns no frame, or parked — whether or not @p retrieve woke
///         it. The reasoning above is the whole of its origin: no upstream issue records it
///         (core-cpp#37).
template <typename T, std::invocable Retrieve>
T syncRunWith(Task<T> task, Retrieve&& retrieve)
{
    if (!task.handle())
        detail::refuseEmptyTask();

    task.handle().resume();
    if (task.done())
        return task.result();

    std::forward<Retrieve>(retrieve)();
    if (task.done())
        throw std::logic_error {
            "core::async::syncRunWith: the task parked on something syncRun cannot complete. The "
            "park was retrieved and the task ran to its end before it was freed, but its answer "
            "is not the one the caller asked for."
        };

    std::ignore = task.release();
    throw std::logic_error {
        "core::async::syncRunWith: the task parked, and what was to retrieve the park did not "
        "wake it. Its frame is leaked rather than freed while something still points into it."
    };
}

} // namespace core::async
