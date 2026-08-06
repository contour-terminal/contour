// SPDX-License-Identifier: Apache-2.0
#pragma once

/// @file
/// `IClock` — the monotonic-clock dependency-injection seam.
///
/// Production code uses @c SteadyClock; tests inject @c ManualClock so timer,
/// timeout, and delay behaviour is deterministic without real wall-clock waits.
/// Every component that schedules against a deadline — the reactor's timer heap,
/// async socket timeouts — takes an @c IClock by reference rather than calling
/// @c std::chrono::steady_clock::now() directly, so its time source is
/// controllable in tests.

#include <chrono>
#include <mutex>

namespace net
{

/// Monotonic time point used for every deadline and timeout in this module.
/// Wall-clock time is never used for internal scheduling, so system-clock skew
/// (NTP adjustments, manual changes) cannot perturb timer semantics.
using SteadyTimePoint = std::chrono::steady_clock::time_point;

/// Duration matching @c SteadyTimePoint's clock.
using SteadyDuration = std::chrono::steady_clock::duration;

/// Monotonic time provider. Inject by reference into any component that
/// schedules against a deadline.
class IClock
{
  public:
    IClock() = default;
    virtual ~IClock() = default;

    IClock(IClock const&) = delete;
    IClock& operator=(IClock const&) = delete;
    IClock(IClock&&) = delete;
    IClock& operator=(IClock&&) = delete;

    /// @return The current monotonic instant. Must be monotonic and thread-safe.
    [[nodiscard]] virtual SteadyTimePoint now() const noexcept = 0;
};

/// Production clock delegating to @c std::chrono::steady_clock::now().
class SteadyClock final: public IClock
{
  public:
    /// @return The current steady-clock instant.
    [[nodiscard]] SteadyTimePoint now() const noexcept override { return std::chrono::steady_clock::now(); }
};

/// Test clock whose value only changes on explicit @c advance() / @c setNow().
/// Thread-safe so a runtime test driving several coroutines (or a coroutine plus
/// a background producer) can share one instance without a data race.
class ManualClock final: public IClock
{
  public:
    /// @param start The initial value returned by @c now().
    explicit ManualClock(SteadyTimePoint start = SteadyTimePoint {}) noexcept: _now { start } {}

    /// @return The current (manually controlled) instant.
    [[nodiscard]] SteadyTimePoint now() const noexcept override
    {
        auto const lock = std::scoped_lock { _mutex };
        return _now;
    }

    /// Moves the clock forward by @p delta.
    /// @param delta A non-negative duration to advance the clock by.
    void advance(SteadyDuration delta) noexcept
    {
        auto const lock = std::scoped_lock { _mutex };
        _now += delta;
    }

    /// Hard-sets the value returned by @c now().
    /// @param when The new value @c now() will return.
    void setNow(SteadyTimePoint when) noexcept
    {
        auto const lock = std::scoped_lock { _mutex };
        _now = when;
    }

  private:
    mutable std::mutex _mutex;
    SteadyTimePoint _now;
};

/// Process-wide @c SteadyClock for callers that legitimately want a default time
/// source without threading an injection through every constructor (e.g. the
/// default argument of the EventLoop constructor). Prefer explicit constructor
/// injection where a test needs deterministic time.
/// @return A reference to a function-local-static @c SteadyClock.
[[nodiscard]] inline IClock& defaultSteadyClock() noexcept
{
    static SteadyClock instance;
    return instance;
}

} // namespace net
