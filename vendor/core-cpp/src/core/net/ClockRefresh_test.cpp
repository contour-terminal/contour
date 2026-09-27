// SPDX-License-Identifier: Apache-2.0
//
// The two `clock.refresh()` calls in the turn, one apiece.
//
// `IClock::refresh()` is a no-op on the clocks a test usually injects — `SteadyClock` reads the
// OS on every `now()`, and `ManualClock` is driven by hand — so nothing in the rest of the suite
// notices if a turn stops making either call. Deleting them left fastcached's daemon serving a
// cache whose clock was frozen at the value `CachedClock` sampled in its constructor: TTLs that
// never expired, `expired_unfetched` stuck at zero, and every test green.
//
// **A case that passes with either call removed is not testing them**, so there is one case per
// call and they fail for different reasons:
//
//   - step 3's refresh decides the TIMEOUT the backend is given, so its case asserts that value;
//   - step 5's refresh decides which deadlines are due in THIS turn rather than the next, so its
//     case asserts that the flow is queued by the turn whose wait reached the deadline.
//
// Ported from fastcached's `Async/ReactorClockRefresh_test.cpp` at `0708dd54`, which asserted
// only the second half and did so through a real platform reactor.
#include <core/async/ParkedWork.hpp>
#include <core/async/Task.hpp>
#include <core/net/EventLoop.hpp>
#include <core/net/testing/ScriptedBackend.hpp>
#include <core/net/testing/TestLoop.hpp>
#include <core/platform/Clock.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <chrono>
#include <coroutine>
#include <cstddef>
#include <optional>
#include <ranges>
#include <tuple>
#include <vector>

using core::async::Task;
using core::net::EventLoop;
using core::net::testing::ScriptedBackend;
using core::platform::CachedClock;
using core::platform::ManualClock;
using namespace std::chrono_literals;

namespace
{

/// @param timeout A timeout a backend recorded.
/// @return The milliseconds it names, or -1 for an indefinite wait.
[[nodiscard]] long long timeoutMs(std::optional<core::platform::SteadyDuration> const& timeout)
{
    return timeout.has_value() ? std::chrono::duration_cast<std::chrono::milliseconds>(*timeout).count() : -1;
}

/// A scripted backend whose wait SPENDS exactly the timeout it was given, on an injected clock.
///
/// The deterministic stand-in for a kernel wait that ran to its timeout: no real sleeping, and the
/// clock moves by the amount the loop asked to wait rather than by whatever the runner took.
class SpendingBackend: public ScriptedBackend
{
  public:
    /// @param source The clock a wait advances (not owned).
    explicit SpendingBackend(ManualClock& source) noexcept: _source(source) {}

    /// @param timeout How long the loop asked to wait; the clock moves by exactly this much.
    /// @return What the next scripted step dispatched.
    core::net::WaitResult wait(std::optional<core::platform::SteadyDuration> timeout) override
    {
        if (timeout.has_value())
            _source.advance(*timeout);
        return ScriptedBackend::wait(timeout);
    }

  private:
    ManualClock& _source;
};

/// Parks on a deadline and records that it came back.
/// @param loop The loop to park on.
/// @param duration How far out the deadline is.
/// @param fired Set once the flow resumes.
Task<void> delayThenFlag(EventLoop* loop, core::platform::SteadyDuration duration, bool* fired)
{
    co_await loop->delay(duration);
    *fired = true;
}

} // namespace

TEST_CASE("The turn refreshes its clock before it computes the wait's timeout", "[EventLoop][clock]")
{
    // A CachedClock serves the instant of its last refresh(). The post below spends 100ms of the
    // turn's own time BEFORE the deadline's remainder is computed -- which is what a batch of work
    // costs in a real loop -- and only a refresh at step 3 counts it. Without one the loop asks
    // the kernel to wait the full 500ms it armed, having already spent a fifth of it, and every
    // deadline in the process runs late by however long its turn took.
    auto source = ManualClock {};
    auto cached = CachedClock { source }; // samples 0
    auto backend = SpendingBackend { source };
    backend.pushTimeout();
    // Declared BEFORE the loop, so it outlives it: ~EventLoop resumes every borrowed
    // park, and the flow's unwinding runs on what it was given.
    auto fired = false;
    auto loop = EventLoop { backend, cached };

    loop.spawn(delayThenFlag(&loop, 500ms, &fired));
    loop.post([&source] { source.advance(100ms); });

    std::ignore = loop.runOnce();

    REQUIRE(backend.waitCount() == 1);
    CHECK(timeoutMs(backend.recordedTimeouts().back()) == 400);
    CHECK_FALSE(fired);
}

TEST_CASE("The turn refreshes its clock after the wait, so a deadline it reached fires in that turn",
          "[EventLoop][clock]")
{
    // The wait runs to its full 500ms timeout, so by the time it returns the deadline HAS been
    // reached. Only a refresh at step 5 lets the turn see that: without one the cached clock still
    // reads the instant this turn started at, step 5 finds nothing due, and the flow waits a whole
    // further turn -- which on an idle loop is a whole further blocking wait.
    auto source = ManualClock {};
    auto cached = CachedClock { source };
    auto backend = SpendingBackend { source };
    backend.pushTimeout();
    // Declared BEFORE the loop, so it outlives it: ~EventLoop resumes every borrowed
    // park, and the flow's unwinding runs on what it was given.
    auto fired = false;
    auto loop = EventLoop { backend, cached };

    loop.spawn(delayThenFlag(&loop, 500ms, &fired));

    auto const first = loop.runOnce();

    CHECK(first.drained == 1);            // the spawned flow ran to its park
    CHECK(loop.pendingTimerCount() == 0); // and its deadline was found due by THIS turn
    CHECK(loop.readyCount() == 1);        // queued by step 5, for the next turn's step 2
    CHECK_FALSE(fired);                   // which has not happened yet: resumption is step 2's

    std::ignore = loop.runOnce();
    CHECK(fired);
    CHECK(backend.waitCount() == 1); // the second turn had nothing left to wait on
}

namespace
{

/// Hands the awaiting coroutine straight back to the loop.
struct YieldToLoop
{
    EventLoop* loop; ///< Where to hand it back.

    /// @return False: always suspend, so the sample below is taken once per turn.
    [[nodiscard]] bool await_ready() const noexcept { return false; }

    /// @tparam Promise The awaiting coroutine's promise type.
    /// @param awaiting The coroutine to re-queue.
    template <typename Promise>
    void await_suspend(std::coroutine_handle<Promise> awaiting) const
    {
        loop->resumeSoon(core::async::detail::parkedWorkFor(awaiting));
    }

    void await_resume() const noexcept {}
};

/// Reads the loop's clock once per turn, @p turns times.
/// @param loop The loop to yield to.
/// @param samples Receives one reading per turn.
/// @param turns How many readings to take.
Task<void> sampleClockEachTurn(EventLoop* loop,
                               std::vector<core::platform::SteadyTimePoint>* samples,
                               int turns)
{
    for ([[maybe_unused]] auto const turn: std::views::iota(0, turns))
    {
        samples->push_back(loop->clock().now());
        co_await YieldToLoop { loop };
    }
}

} // namespace

TEST_CASE("A loop given a plain steady clock is unaffected by the refresh calls", "[EventLoop][clock]")
{
    // The refresh is unconditional -- the loop cannot know which IClock it was handed -- so the
    // default no-op has to be safe on the turn's hot path as well as in isolation. A refresh that
    // disturbed a direct-reading clock would show up as a non-monotonic sample, so the samples are
    // taken from inside the flow, one per turn, and compared: eight turns, eight refreshes apiece.
    //
    // Deliberately not written as "park on a deadline and wait for it": a real clock reaches a
    // real deadline after some unknown number of turns, which in a Release build is more than a
    // bound can honestly be set to.
    constexpr auto Turns = 8;

    auto clock = core::platform::SteadyClock {};
    // Declared BEFORE the loop, so it outlives it: ~EventLoop resumes every borrowed
    // park, and the flow's unwinding runs on what it was given.
    auto samples = std::vector<core::platform::SteadyTimePoint> {};
    auto loop = core::net::testing::TestLoop { clock };

    auto const before = clock.now();
    loop.spawn(sampleClockEachTurn(&loop, &samples, Turns));
    std::ignore = loop.drain();

    REQUIRE(samples.size() == static_cast<std::size_t>(Turns));
    CHECK(std::ranges::is_sorted(samples)); // monotonic across every refresh the turns made
    CHECK(samples.front() >= before);
    CHECK(clock.now() >= samples.back());
}
