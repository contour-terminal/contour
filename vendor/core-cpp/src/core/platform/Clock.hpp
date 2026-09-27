// SPDX-License-Identifier: Apache-2.0
#pragma once

/// @file Clock.hpp
/// @brief `IClock` and `IWallClock` — the clock dependency-injection seams.
///
/// Production code uses @c SteadyClock (or a @c CachedClock over it) and @c SystemWallClock;
/// tests inject @c ManualClock and @c ManualWallClock, so timer, timeout, TTL and `sleep`
/// behaviour is deterministic without real waits. Every component that schedules against a
/// deadline — an event loop's timer heap, socket timeouts, a cache's expiry — takes an
/// @c IClock by reference rather than calling @c std::chrono::steady_clock::now() directly, so
/// its time source is controllable in tests. It mirrors the provider pattern of the rest of
/// this module: an abstract interface, a native implementation, and a test double.

#include <atomic>
#include <chrono>
#include <mutex>

namespace core::platform
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

/// An @c IClock that serves a value sampled once per event-loop turn instead of reading the OS
/// clock on every call.
///
/// Reading the clock is not free: on Windows `steady_clock::now()` is a
/// `QueryPerformanceCounter`, measured at ~16 ns on a Ryzen 9 9950X3D. A cache that reads the
/// clock once per command paid ~32% of the cost of serving a cached GET for it — more than the
/// hash, the lock and the lookup put together, and none of it cache work.
///
/// A request cannot observe a difference: every operation in one loop turn is answered with the
/// time at which that turn's I/O became ready, and TTLs are expressed in seconds. The staleness
/// is bounded by the time the loop spends processing one batch, not by how long it sleeps — the
/// refresh happens *after* the blocking wait returns, so an idle process's clock is current the
/// moment work arrives.
///
/// Thread-safe: several event loops may share one instance. Publication is a compare-and-swap
/// that only ever moves the value forward, so `now()` stays monotonic as @c IClock requires even
/// when loops refresh out of order.
class CachedClock final: public IClock
{
  public:
    /// Wraps @p source, taking an initial sample so `now()` is valid before any event loop has
    /// started (configuration reload, startup logging, a test).
    /// @param source Upstream clock to sample; must outlive this object.
    explicit CachedClock(IClock& source) noexcept: _source { source } { refresh(); }

    /// @return The instant sampled by the last @c refresh(), or by the constructor.
    [[nodiscard]] SteadyTimePoint now() const noexcept override
    {
        return SteadyTimePoint { SteadyDuration { _ticks.load(std::memory_order_relaxed) } };
    }

    /// Samples the upstream clock and publishes the sample if it is newer than what is already
    /// there.
    void refresh() noexcept override
    {
        auto const sampled = _source.now().time_since_epoch().count();
        auto current = _ticks.load(std::memory_order_relaxed);
        while (sampled > current)
        {
            // On success the value moved forward; on failure `current` is reloaded and the loop
            // re-tests, so a concurrent refresh that published something newer simply wins and
            // this one gives up.
            if (_ticks.compare_exchange_weak(
                    current, sampled, std::memory_order_relaxed, std::memory_order_relaxed))
                return;
        }
    }

  private:
    IClock& _source;
    std::atomic<SteadyDuration::rep> _ticks { 0 };
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
/// @return A reference to a function-local-static @c SteadyClock.
[[nodiscard]] inline IClock& defaultSteadyClock() noexcept
{
    static SteadyClock instance;
    return instance;
}

/// Wall-clock time provider, distinct from @c IClock.
///
/// Internal scheduling never reads the wall clock (see @c SteadyTimePoint), but some inputs are
/// wall-clock instants: an absolute UNIX timestamp in a protocol (Redis `EXPIREAT`), a log
/// line's date. Code that translates such an instant against "now" takes an @c IWallClock,
/// usually as a @c WallClockRef. Production injects @c SystemWallClock; tests inject
/// @c ManualWallClock.
class IWallClock
{
  public:
    IWallClock() = default;
    // These deletions are LOAD-BEARING for a guarantee stated on `WallClockRef` below, and
    // nothing else enforces it. Because neither a copy nor a move exists, no function anywhere
    // can return an `IWallClock` BY VALUE -- so the one shape `WallClockRef` cannot refuse, a
    // borrow of something that was never an lvalue to begin with, is impossible by
    // construction rather than merely absent from today's tree. Restoring either constructor
    // reopens that hole SILENTLY: no test fails, no scan fires, nothing stops compiling. If you
    // need one, read `WallClockRef`'s note first and give that guarantee a different reader.
    IWallClock(IWallClock const&) = delete;
    IWallClock(IWallClock&&) = delete;
    IWallClock& operator=(IWallClock const&) = delete;
    IWallClock& operator=(IWallClock&&) = delete;
    virtual ~IWallClock() = default;

    /// @return The current wall-clock time (system_clock). Need not be monotonic, and may jump
    ///         under NTP adjustments.
    [[nodiscard]] virtual std::chrono::system_clock::time_point now() const noexcept = 0;
};

/// Production @c IWallClock delegating to @c std::chrono::system_clock::now().
class SystemWallClock final: public IWallClock
{
  public:
    /// @return The current system-clock instant.
    [[nodiscard]] std::chrono::system_clock::time_point now() const noexcept override
    {
        return std::chrono::system_clock::now();
    }
};

/// Test @c IWallClock with a manually driven value. Mirrors @c ManualClock for the steady seam:
/// deterministic, and thread-safe so tests driving several coroutines can share one.
class ManualWallClock final: public IWallClock
{
  public:
    /// @param start The initial value returned by @c now().
    explicit ManualWallClock(std::chrono::system_clock::time_point start = {}) noexcept: _now { start } {}

    /// @return The current (manually controlled) wall-clock instant.
    [[nodiscard]] std::chrono::system_clock::time_point now() const noexcept override
    {
        auto const lock = std::scoped_lock { _mutex };
        return _now;
    }

    /// Moves the wall clock forward by @p delta.
    /// @param delta A non-negative duration to advance the wall clock by.
    void advance(std::chrono::system_clock::duration delta) noexcept
    {
        auto const lock = std::scoped_lock { _mutex };
        _now += delta;
    }

    /// Hard-sets the value returned by @c now().
    /// @param when The new value @c now() will return.
    void setNow(std::chrono::system_clock::time_point when) noexcept
    {
        auto const lock = std::scoped_lock { _mutex };
        _now = when;
    }

  private:
    mutable std::mutex _mutex;
    std::chrono::system_clock::time_point _now;
};

/// Process-wide @c SystemWallClock for callers that want a wall-clock source without requiring
/// every test to inject one (e.g. a default constructor argument). Tests that need determinism
/// construct a @c ManualWallClock locally and pass it in.
/// @return A reference to a function-local-static @c SystemWallClock.
[[nodiscard]] inline IWallClock& defaultSystemWallClock() noexcept
{
    static SystemWallClock instance;
    return instance;
}

/// A borrowed @c IWallClock that cannot be built from a temporary.
///
/// Every type that keeps a wall clock keeps a BORROWED one -- it is injected and outlives its
/// holder -- and nothing said so in a way a compiler could read. A temporary bound to such a
/// parameter dies at the end of the declaration and leaves the holder reading freed stack:
/// fastcached saw five release legs answer SIGSEGV while every debug leg passed, because at
/// `-O0` the dead frame slot still held a usable vptr and at `-O2` the locals declared after it
/// reuse the slot (LASTRADA-Software/fastcached#1028).
///
/// **The guard is on the PARAMETER, not on the storing type, and that is the point.** A deleted
/// `Retainer(IWallClock const&&)` overload rejects a DIRECT temporary and accepts a FORWARDED one
/// -- measured, gcc and clang alike -- because inside a forwarding constructor the parameter is a
/// named lvalue and binds to the ordinary overload. fastcached#1028 was exactly that shape: the
/// temporary went to a type that stores no clock at all and hands the reference to three objects
/// that do. Carrying the borrow as a VALUE means forwarding copies it rather than re-binding a
/// reference, so the refusal survives every hop.
///
/// Implicitly constructible on purpose: a call site passing a named clock is unchanged, so
/// adopting the guard costs nothing and there is no incentive to route around it.
///
/// It refuses an rvalue, and the reason there is no gap left over is @c IWallClock's own deleted
/// copy and move constructors: with neither, nothing can return an @c IWallClock by value, so
/// there is no legitimate rvalue for this to be too strict about. That is a property of the
/// interface, not a census of the callers -- and those deletions carry this guarantee, which is
/// why they say so.
class WallClockRef
{
  public:
    /// Borrows a wall clock.
    /// @param wall The clock to borrow. It must outlive every holder of this reference;
    ///        @c defaultSystemWallClock() has static storage and any named object will do,
    ///        while a temporary is refused by the overload below.
    WallClockRef(IWallClock const& wall) noexcept: _wall { &wall } {}

    /// Refuses a temporary, which is the entire reason this type exists.
    WallClockRef(IWallClock const&&) = delete;

    /// @return The borrowed clock, for a caller that needs the interface itself.
    [[nodiscard]] IWallClock const& get() const noexcept { return *_wall; }

    /// @return What the borrowed clock says the time is, so a holder need not unwrap it.
    [[nodiscard]] std::chrono::system_clock::time_point now() const noexcept { return _wall->now(); }

  private:
    IWallClock const* _wall;
};

} // namespace core::platform
