// SPDX-License-Identifier: Apache-2.0
#pragma once

/// @file
/// `testing::TestLoop` — a deterministic @c EventLoop with no I/O behind it.
///
/// Pairs with @c platform::ManualClock so deadline-driven behaviour is reproducible: a case
/// submits its entry point, advances the clock and calls @c tick() or @c drain(). Nothing waits
/// on anything real, so nothing is timing-dependent.
///
/// It is an @c EventLoop over @c NullBackend with @c IdlePolicy::Return, and being the real loop
/// rather than a second implementation is the point: fastcached's `TestReactor` was a separate
/// reactor, so every rule the platform reactors held — the turn order, the teardown order, the
/// ownership of parked work — had to be written twice and could differ. Here a case that passes
/// on a `TestLoop` and fails on a `PlatformLoop` is a backend difference and nothing else.
///
/// Ported from fastcached's `Async/TestReactor.{hpp,cpp}` at `0708dd54`.

#include <core/net/EventLoop.hpp>
#include <core/net/testing/NullBackend.hpp>
#include <core/platform/Clock.hpp>

#include <cstddef>

namespace core::net::testing
{

namespace detail
{

    /// Holds the @c NullBackend a @c TestLoop owns, so it outlives the loop over it. A private
    /// base for @c core::net::detail::OwnedBackend's reason: a member is initialised after every
    /// base, and @c EventLoop is a base.
    class OwnedNullBackend
    {
      public:
        OwnedNullBackend(OwnedNullBackend const&) = delete;
        OwnedNullBackend(OwnedNullBackend&&) = delete;
        OwnedNullBackend& operator=(OwnedNullBackend const&) = delete;
        OwnedNullBackend& operator=(OwnedNullBackend&&) = delete;

      protected:
        OwnedNullBackend() = default;
        ~OwnedNullBackend() = default;

        /// Accepts registrations, reports nothing, never blocks. Named apart from @c EventLoop's
        /// own `_backend`, because a name found in two base classes is ambiguous whatever its access.
        NullBackend _ownedBackend;
    };

} // namespace detail

/// A single-threaded loop driven entirely by hand.
class TestLoop final: private detail::OwnedNullBackend, public EventLoop
{
  public:
    /// @param clock The clock deadlines are measured against (not owned) — typically a
    ///        @c platform::ManualClock.
    /// @param options The loop's configuration. The idle policy is forced to
    ///        @c IdlePolicy::Return whatever is passed: this loop has no thread of its own to
    ///        block, and a turn that blocked would block the case driving it.
    explicit TestLoop(platform::IClock& clock, EventLoopOptions const& options = {}):
        detail::OwnedNullBackend {}, EventLoop { _ownedBackend, clock, returning(options) }
    {
    }

    /// Runs exactly one turn.
    /// @return How much this turn drained — coroutines resumed plus timer callbacks run. Zero
    ///         means the loop had nothing to do, which is what @c drain stops on.
    std::size_t tick() { return runOnce(platform::SteadyDuration::zero()).drained; }

    /// Runs turns until one of them does nothing.
    /// @return How much every turn drained in all — coroutines resumed plus timer callbacks run.
    std::size_t drain() { return runUntilIdle(); }

    /// @return How many coroutines are waiting to be resumed by a drain: the ready queue, plus what
    ///         was submitted between turns. A case's own thread is not the loop's worker outside a
    ///         turn, so a `submit` from it goes to the inbound queue; counting the ready queue alone
    ///         answered 0 right after that submit, which made the answer depend on which thread
    ///         submitted (the reason @c EventLoop::cancelPending searches the inbound queue too).
    [[nodiscard]] std::size_t pendingSubmissions() const { return readyCount() + inboundSubmissionCount(); }

    /// @return How many deadlines are waiting: the parks armed on one, plus what was scheduled
    ///         between turns and is not armed yet, for the reason @c pendingSubmissions gives.
    [[nodiscard]] std::size_t pendingTimers() const { return pendingTimerCount() + inboundScheduledCount(); }

    /// @return The backend this loop owns, for a case that asserts what it was asked.
    [[nodiscard]] NullBackend& backend() noexcept { return _ownedBackend; }

  private:
    /// @param options What the caller asked for.
    /// @return The same options with the idle policy forced; see the constructor.
    [[nodiscard]] static EventLoopOptions returning(EventLoopOptions const& options) noexcept
    {
        auto forced = options;
        forced.idle = IdlePolicy::Return;
        return forced;
    }
};

} // namespace core::net::testing
