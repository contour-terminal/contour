// SPDX-License-Identifier: Apache-2.0
#pragma once

/// @file
/// `DeadlineTimer` — a deadline that can be disarmed, as an object with a lifetime.
///
/// The shape an operation needs when a timeout has to **tear the operation down** rather than
/// merely stop waiting for it. That is the reason this is a timer with a callback and not a race
/// between two tasks: a dial that only stopped waiting would leave the connect attempt in flight
/// for the kernel's own retry schedule — minutes — and a caller that redials on a backoff then
/// accumulates descriptors. A timeout that does not close what it abandoned is a leak with a retry
/// loop behind it. Where "stop waiting" IS the whole answer, `withTimeout` is the cheaper shape.
///
/// Ported from fastcached's `Async/DeadlineTimer.{hpp,cpp}` at `0708dd54`, and **most of it is
/// gone**. Upstream's `IReactor::Schedule` could not be taken back, so the timer was a detached
/// coroutine that slept in 50ms steps re-reading a flag, over a `shared_ptr<State>` that outlived
/// the timer so a late fire would read live memory. `EventLoop::cancelTimer` retires the park by
/// id, so there is no late fire to survive: no coroutine frame, no shared state, no allocation, no
/// poll interval, and the only wake-up an armed timer costs is the one at its deadline.
///
/// Threading: the callback runs on the loop's thread, in turn step 2. Destroying the timer
/// disarms it, **and doing so from inside its own callback is safe** — the timer is marked settled
/// before the callback runs, so the destructor it triggers finds nothing left to retire.

#include <core/net/EventLoop.hpp>
#include <core/platform/Clock.hpp>

namespace core::net
{

/// A one-shot deadline on an @c EventLoop, disarmed by @c disarm() or by destruction.
class DeadlineTimer
{
  public:
    /// Invoked at most once, on the loop's thread, when the deadline elapses without a
    /// @c disarm().
    using Callback = TimerCallback;

    /// Arms a deadline on @p loop.
    /// @param loop The loop whose clock gates the deadline and whose thread runs the callback.
    ///        Not owned, and it must outlive this timer — every user satisfies that by
    ///        construction, being a local or a member of something running on that loop.
    /// @param deadline The absolute instant at or after which @p onExpired runs, unless disarmed
    ///        first. A deadline already in the past fires on a later turn rather than inline, so a
    ///        caller is never re-entered from its own constructor.
    /// @param onExpired What to do when it elapses. Must not be null.
    /// @param state An opaque pointer handed to @p onExpired; it must outlive the timer, or the
    ///        callback must tolerate it not doing so.
    DeadlineTimer(EventLoop& loop, platform::SteadyTimePoint deadline, Callback onExpired, void* state);

    DeadlineTimer(DeadlineTimer const&) = delete;
    DeadlineTimer(DeadlineTimer&&) = delete;
    DeadlineTimer& operator=(DeadlineTimer const&) = delete;
    DeadlineTimer& operator=(DeadlineTimer&&) = delete;

    /// Disarms, so a timer never outlives the object its callback would reach.
    ~DeadlineTimer();

    /// Prevents the callback from running.
    ///
    /// Idempotent, and safe after the callback has already run. Loop thread only, which is where
    /// every user of this type lives.
    void disarm() noexcept;

    /// @return True once the callback has run or @c disarm() has been called.
    [[nodiscard]] bool settled() const noexcept { return _settled; }

  private:
    /// What the loop calls: marks this timer settled, then runs the user's callback.
    ///
    /// **Settled and the id dropped BEFORE the call**, because the callback is allowed to destroy
    /// this timer — the destructor it triggers must then find nothing to retire, and nothing here
    /// may touch `*self` afterwards, which is why the callback and its state are read into locals
    /// first.
    /// Named apart from the constructor's `onExpired` parameter and the `_onExpired` member: a
    /// parameter that shadows a member function is `-Wshadow`, which is fatal here and which only
    /// GCC diagnoses.
    /// @param state The timer, as a `void*`.
    static void fire(void* state);

    EventLoop* _loop;      ///< The loop the deadline is armed on; not owned.
    Callback _onExpired;   ///< What to run when it elapses.
    void* _state;          ///< The opaque pointer handed to @c _onExpired.
    TimerId _timer {};     ///< The armed park, while there is one.
    bool _settled = false; ///< Whether the callback has run or the timer has been disarmed.
};

} // namespace core::net
