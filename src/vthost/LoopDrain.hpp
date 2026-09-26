// SPDX-License-Identifier: Apache-2.0
#pragma once

/// @file
/// Ends the flows a loop spawned while what they reference is still alive.

#include <core/net/EventLoop.hpp>

#include <cassert>
#include <chrono>
#include <tuple>

namespace vthost
{

/// Cancels and drains every flow @p loop spawned, when it goes out of scope.
///
/// A connection flow that ConnectionAcceptor spawns references the SessionHost it serves: it
/// subscribes to it for the life of the connection. ~EventLoop ends the flows still parked, but it
/// runs last, after the host is gone, because the host cannot be declared before the loop it takes
/// in its constructor -- and must not be, since its pump threads post onto the loop until
/// ~SessionHost has joined them. So an owner declares one of these right after the host: it is
/// destroyed first, and every spawned flow unwinds while the host it points into still exists.
///
/// Which backend reports a peer's close in time for the loop's last turn decides whether any flow is
/// still parked there, so without this the order is right on one backend and a use-after-free on
/// another.
class [[nodiscard]] LoopDrain
{
  public:
    /// The most turns the drain runs, and the longest each may wait: two seconds in all.
    static constexpr auto MaxPasses = 200;
    static constexpr auto PassWait = std::chrono::milliseconds { 10 };

    /// @param loop The loop to drain on destruction (not owned; outlives this object). It must not
    ///        be running a turn at that point.
    explicit LoopDrain(core::net::EventLoop& loop) noexcept: _loop(loop) {}

    /// Cancels every flow, then turns the loop until it holds no spawned flow -- not merely until
    /// a turn finds nothing to do: on an I/O completion port a cancelled read stays parked until its
    /// completion packet comes back, which can take a turn that waits. Bounded, because a
    /// SessionHost's pump threads keep posting onto the loop, so "idle" may never come.
    ///
    /// spawnedCount() is exact from core-cpp 0.3.0 on: the turn that ends a flow releases it, even
    /// when the flow ends inside a sub-task's resume.
    ~LoopDrain()
    {
        _loop.requestStop();
        for (auto pass = 0; _loop.spawnedCount() > 0 && pass < MaxPasses; ++pass)
            std::ignore = _loop.runOnce(PassWait);
        assert(_loop.spawnedCount() == 0 && "a spawned flow is still alive after the drain");
    }

    LoopDrain(LoopDrain const&) = delete;
    LoopDrain& operator=(LoopDrain const&) = delete;
    LoopDrain(LoopDrain&&) = delete;
    LoopDrain& operator=(LoopDrain&&) = delete;

  private:
    core::net::EventLoop& _loop;
};

} // namespace vthost
