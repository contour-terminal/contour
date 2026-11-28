// SPDX-License-Identifier: Apache-2.0
#pragma once

/// @file Clock.hpp
/// @brief `IClock` — the monotonic clock seam, with its production clock and its test double.
///
/// Production code uses @c SteadyClock; tests inject @c ManualClock, so timer, timeout, delay and
/// animation behaviour is deterministic without real waits. Every component that measures time
/// against a deadline — an event loop's timer heap, a socket timeout, a hover delay, a spinner's
/// frame — takes an @c IClock by reference rather than calling
/// @c std::chrono::steady_clock::now() directly, so its time source is controllable in tests.
///
/// It is in @c core::base rather than @c core::platform so that code which links nothing of the
/// platform layer — the headless TUI (@c core::tui_core) — takes the same interface the event loop
/// and @c core::tui::Terminal do (docs/design/tui-headless.md). `<core/platform/Clock.hpp>`
/// includes this header and names these types in @c core::platform too, beside the caching and
/// wall clocks that stay there.

#include <core/BaseExport.hpp>

#include <chrono>
#include <mutex>

namespace core
{

/// Monotonic time point used for every deadline, timeout and TTL. Wall-clock time is never
/// used for internal scheduling, so system-clock skew (NTP adjustments, manual changes)
/// cannot perturb timer semantics or expire what is still live. Aliased to
/// @c std::chrono::steady_clock::time_point so existing deadline fields need no type churn
/// when migrated onto @c IClock.
using SteadyTimePoint = std::chrono::steady_clock::time_point;

/// Duration matching @c SteadyTimePoint's clock.
using SteadyDuration = std::chrono::steady_clock::duration;

/// Monotonic time provider. Inject by reference into any component that schedules against a
/// deadline.
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

    /// Re-samples the underlying time source, if this clock caches one.
    ///
    /// Called by whoever owns an event loop, which is the only place that knows time may have
    /// passed: once right after the blocking wait returns, so every handler and timer that turn
    /// resumes sees the instant the wait ended at, and once before the next wait's timeout is
    /// computed, so that timeout is not overstated by however long the batch took to process.
    /// Implementations that read the OS clock on every `now()` (@c SteadyClock) and those driven
    /// by a test (@c ManualClock) ignore it, which is why this defaults to a no-op rather than
    /// being pure virtual.
    ///
    /// Must be safe to call from any thread and from several at once: event loops share one
    /// clock.
    virtual void refresh() noexcept {}
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
/// default argument of an event loop's constructor). Prefer explicit constructor
/// injection where a test needs deterministic time.
/// The instance is defined in the library, so every image linking core-cpp shares one.
/// @return A reference to a function-local-static @c SteadyClock.
[[nodiscard]] CORE_BASE_API IClock& defaultSteadyClock() noexcept;

} // namespace core
