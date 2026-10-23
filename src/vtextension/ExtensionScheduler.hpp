// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <core/platform/Clock.hpp>

#include <chrono>
#include <cstdint>
#include <functional>

namespace vtextension
{

/// One pinned thread of an IExtensionScheduler. Every job and timer posted to a lane runs on the same
/// OS thread, one at a time, in order.
enum class LaneId : uint32_t
{
};

/// A timer scheduled on an IExtensionScheduler, by which it can be cancelled.
enum class TimerId : uint64_t
{
};

/// Where extension runners run, and how they wait.
///
/// WebAssembly execution environments remember the native stack of the thread that created them, so
/// everything a runner does with its instance (create, load, update, render, destroy) has to happen on
/// one OS thread. A runner therefore asks for a lane once, and posts all its work there; the work of
/// one runner never overlaps, and a slow extension delays only the runners that share its lane.
///
/// Timers are part of the scheduler rather than a separate service because a timer's job runs on its
/// lane like any other, and because the scheduler's clock is the one its timers are measured by: a
/// runner reads the time from clock(), so that it and its timers agree on what time it is. Production
/// uses real time; a test advances a manual clock and fires the timers that fall due.
///
/// post(), schedule() and cancel() may be called from any thread.
class IExtensionScheduler
{
  public:
    IExtensionScheduler() = default;
    IExtensionScheduler(IExtensionScheduler const&) = delete;
    IExtensionScheduler(IExtensionScheduler&&) = delete;
    IExtensionScheduler& operator=(IExtensionScheduler const&) = delete;
    IExtensionScheduler& operator=(IExtensionScheduler&&) = delete;
    virtual ~IExtensionScheduler() = default;

    /// Picks the lane a new runner is pinned to for its whole life.
    /// @return The lane.
    [[nodiscard]] virtual LaneId assignLane() = 0;

    /// Runs @p job on @p lane, after the jobs posted there before it. Never runs it inline.
    /// @param lane A lane assignLane() returned.
    /// @param job The work; it must not throw.
    virtual void post(LaneId lane, std::function<void()> job) = 0;

    /// Runs @p job on @p lane once @p after has passed by clock(), unless cancelled first.
    /// @param lane A lane assignLane() returned.
    /// @param after The delay.
    /// @param job The work; it must not throw.
    /// @return The timer, for cancel().
    [[nodiscard]] virtual TimerId schedule(LaneId lane,
                                           std::chrono::milliseconds after,
                                           std::function<void()> job) = 0;

    /// Cancels a timer. A timer that already ran, or is running, is not affected; one that is
    /// cancelled from its own lane before it ran never runs.
    /// @param timer The timer.
    virtual void cancel(TimerId timer) noexcept = 0;

    /// @return The clock the timers are measured by, which every runner reads the time from.
    [[nodiscard]] virtual core::platform::IClock& clock() noexcept = 0;
};

} // namespace vtextension
