// SPDX-License-Identifier: Apache-2.0
#pragma once

/// @file
/// `DetachedTask` — a fire-and-forget coroutine whose frame nobody owns.
///
/// The body runs to its first suspension on construction and the frame frees itself on final
/// return, so the caller keeps no handle: a server spawns one per connection and the chain of
/// I/O awaitables drives it from there.
///
/// It is a header of its own, although fastcached keeps it in `Task.hpp`, because every other
/// piece of this module's vocabulary is (`Awaitable.hpp`, `Cancellation.hpp`,
/// `UniqueCoroHandle.hpp`) and because it is what makes an await chain UNOWNED — the one fact
/// `core::async::detail::unownedRootOf` has to be able to name. Every other coroutine frame here
/// is owned by something: a `Task` value, or the awaiter of whatever awaits it.

#include <core/async/StopToken.hpp>

#include <coroutine>
#include <exception>
#include <memory>
#include <mutex>
#include <type_traits>

namespace core::async
{

namespace detail
{
    class AbandonState;
}

/// A coroutine started for its effects, owned by nobody, freeing its own frame when it ends.
///
/// An exception escaping the body terminates the process: there is no caller to hand it to, and
/// no frame left to unwind into. A detached flow catches what it can act on — a connection error
/// becomes a response — before reaching its final suspension.
///
/// It is the one shape an executor may free at teardown, because nothing else can
/// ([`ParkedWork`](ParkedWork.hpp)).
///
/// The destructor is user-provided, and does nothing, on purpose: see @c ~DetachedTask.
struct DetachedTask
{
    DetachedTask() noexcept = default;
    DetachedTask(DetachedTask const&) noexcept = default;
    DetachedTask(DetachedTask&&) noexcept = default;
    DetachedTask& operator=(DetachedTask const&) noexcept = default;
    DetachedTask& operator=(DetachedTask&&) noexcept = default;

    /// Does nothing, and is defaulted out of line to make this type non-trivially destructible:
    /// defaulted where it is declared, it would be trivial.
    ///
    /// A trivial empty class is returned in a register, and clang-cl without optimisation keeps
    /// the ramp's copy of it in the coroutine frame and reloads it from there on the way out --
    /// after the first suspension. By then whoever the body suspended into (a pool thread, an
    /// event loop, an `await_suspend` that resumes inline) may have run it to its end and freed
    /// the frame, so the reload read freed memory. A non-trivial destructor makes every ABI return
    /// the object through a pointer the caller passes, which the ramp keeps on its own stack
    /// ([core-cpp#51](https://github.com/contour-terminal/core-cpp/issues/51)).
    ~DetachedTask();

    /// The coroutine promise; the standard looks up `DetachedTask::promise_type`.
    struct promise_type
    {
        /// What every park of this chain agrees on: which frame to free, and whether it is still
        /// theirs to free. Created by the first park (`core::async::detail::claimOn`) and held
        /// here so the second finds it rather than making a second one — a fan-out hands the same
        /// root to N children, and N claims would free it N times. It is held STRONGLY and frees
        /// nothing itself, so it is not a cycle: a park's claim is what frees the frame, and the
        /// state outlives the frame because every park holds a reference to it.
        std::shared_ptr<detail::AbandonState> abandonState;

        /// Guards the one creation of @c abandonState. A chain whose children run on a pool can
        /// park two of them at once, and `if (!state) state = make()` on two threads makes two.
        std::once_flag abandonOnce;

        /// @return The (empty) handle-less object that represents this coroutine.
        [[nodiscard]] DetachedTask get_return_object() noexcept { return {}; }

        /// Runs the body at once: a detached flow has no caller to attach a continuation.
        [[nodiscard]] std::suspend_never initial_suspend() const noexcept { return {}; }

        /// Frees the frame at the end: there is no owner to do it.
        [[nodiscard]] std::suspend_never final_suspend() const noexcept { return {}; }

        void return_void() const noexcept {}

        /// Ends the process: there is nowhere for an exception to go from here.
        void unhandled_exception() const noexcept { std::terminate(); }

        /// @return A token that never reports a stop.
        ///
        /// A detached flow has no awaiting coroutine to inherit cancellation from, and it carries
        /// no source of its own: what cancels it is whatever it awaits, through that awaitable's
        /// own token. Answering with an empty token rather than not answering at all is what lets
        /// a `Task` awaited from here take the ordinary inheritance path (@c HasStopToken) instead
        /// of a second, silent one.
        [[nodiscard]] StopToken stopToken() const noexcept { return StopToken {}; }
    };
};

inline DetachedTask::~DetachedTask() = default;

static_assert(!std::is_trivially_destructible_v<DetachedTask>,
              "a trivial DetachedTask is one clang-cl at -O0 reads back from a freed frame (core-cpp#51)");

} // namespace core::async
