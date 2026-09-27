// SPDX-License-Identifier: Apache-2.0
#pragma once

/// @file
/// `PlatformLoop` — an @c EventLoop that owns this platform's default backend.
///
/// Every program that wants a loop wants the same two lines: make the backend this platform
/// prefers, then build a loop over it. Written out per consumer they drift — one forgets that the
/// backend must OUTLIVE the loop, and the resulting teardown reads a detached handler through a
/// destroyed backend. Here the base-class order says it once.
///
/// It replaces fastcached's `Async/PlatformReactor.hpp`, which was an `#if` ladder over three
/// concrete reactor types. There is one loop type here and the choice is `makeDefaultBackend()`'s,
/// so the ladder lives in one `DefaultBackend.cpp` per platform and no header has to ask what
/// platform it is on.

#include <core/net/EventLoop.hpp>
#include <core/net/IoBackend.hpp>
#include <core/platform/Clock.hpp>

#include <memory>

namespace core::net
{

namespace detail
{

    /// Holds the backend a @c PlatformLoop owns, so it is constructed before the loop and
    /// destroyed after it.
    ///
    /// A private base rather than a member, because a member is initialised AFTER every base —
    /// and @c EventLoop is a base, so it would be handed a reference to a `unique_ptr` that has
    /// not been filled in yet. Bases initialise in declaration order, which is the ordering this
    /// needs and the only one the language will give.
    class OwnedBackend
    {
      public:
        OwnedBackend(OwnedBackend const&) = delete;
        OwnedBackend(OwnedBackend&&) = delete;
        OwnedBackend& operator=(OwnedBackend const&) = delete;
        OwnedBackend& operator=(OwnedBackend&&) = delete;

      protected:
        OwnedBackend(): _ownedBackend { makeDefaultBackend() } {}
        ~OwnedBackend() = default;

        /// Never null: @c makeDefaultBackend throws instead. Named apart from @c EventLoop's own
        /// `_backend`, because a name found in two base classes is ambiguous whatever its access.
        std::unique_ptr<IoBackend> _ownedBackend;
    };

} // namespace detail

/// The loop a program gets when it has no reason to choose a backend.
///
/// On Linux that is epoll, on macOS and the BSDs kqueue, on Windows the completion port, and
/// under single-threaded WebAssembly the host-driven backend over the
/// browser's own timer — which means a `PlatformLoop` there must be PUMPED and neither `run()`
/// nor `blockOn()` may be called on it. Both assert.
class PlatformLoop final: private detail::OwnedBackend, public EventLoop
{
  public:
    /// @param clock The monotonic time source for deadlines (not owned; outlives the loop).
    /// @param options The loop's configuration; see @c EventLoopOptions.
    explicit PlatformLoop(platform::IClock& clock = platform::defaultSteadyClock(),
                          EventLoopOptions const& options = {}):
        detail::OwnedBackend {}, EventLoop { *_ownedBackend, clock, options }
    {
    }

    /// @return The backend this loop owns, for a caller that has to ask what it is (a test's
    ///         section label, a diagnostic) or that registers a handler of its own with it.
    [[nodiscard]] IoBackend& backend() noexcept { return *_ownedBackend; }
};

} // namespace core::net
