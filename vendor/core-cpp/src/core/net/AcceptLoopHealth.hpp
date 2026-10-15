// SPDX-License-Identifier: Apache-2.0
#pragma once

/// @file
/// Which of a process's accept loops are degraded, and which have given up.
///
/// **A listening port is not a serving one.** An accept loop that ends leaves its listening socket
/// open unless something closes it, so the port still shows `LISTENING` and the kernel goes on
/// completing handshakes into a backlog nobody drains -- and then refuses every connect.
/// fastcached's health endpoint answered `200` for nine hours over a node whose compile surface was
/// in exactly that state, because nothing it read could tell. A loop reports here when it becomes
/// degraded (@c AcceptLoopEventKind::Degraded), when it recovers, and when it gives up on a dead
/// listener, and a liveness probe that answers from @c AcceptLoopHealth::snapshot answers the
/// question it is asked. fastcached's health probe is the consumer this was graduated for.
///
/// Origin: fastcached `src/FastCache/Transport/AcceptLoopHealth.hpp`.

#include <core/net/AcceptPolicy.hpp>

#include <functional>
#include <mutex>
#include <string>
#include <string_view>
#include <vector>

namespace core::net
{

/// One accept loop that is not serving as it should, as @c AcceptLoopHealth::snapshot reports it.
struct SurfaceCondition
{
    std::string surface;      ///< What the loop serves, as its log lines name it.
    std::string reason;       ///< The line it reported, in words.
    AcceptLoopEventKind kind; ///< @c AcceptLoopEventKind::Degraded or @c AcceptLoopEventKind::GaveUp.
};

/// The accept loops of one process that are degraded or have given up, and who to tell when that
/// changes.
///
/// Thread-safe: an accept loop reports from its own thread, a probe reads from another.
class AcceptLoopHealth
{
  public:
    /// Told of each change -- degraded, recovered, gave up -- outside the lock, on the reporting
    /// loop's thread.
    using Listener = std::function<void(AcceptLoopEvent const&)>;

    AcceptLoopHealth() = default;
    AcceptLoopHealth(AcceptLoopHealth const&) = delete;
    AcceptLoopHealth(AcceptLoopHealth&&) = delete;
    AcceptLoopHealth& operator=(AcceptLoopHealth const&) = delete;
    AcceptLoopHealth& operator=(AcceptLoopHealth&&) = delete;
    ~AcceptLoopHealth() = default;

    /// Names who is told of each change: a later call replaces the listener for the changes that
    /// follow it. Called while the process is being assembled, before any accept loop runs.
    /// @param listener Told of each change.
    void subscribe(Listener listener);

    /// Passes every change recorded here on to @p target as well, the conditions already recorded
    /// included.
    ///
    /// For a component that runs a loop of its own and owns its own registry, so a process still has
    /// ONE registry its liveness probe reads. **Called once, while the process is being assembled,
    /// before any loop reports:** the replay runs outside this registry's lock, so a change recorded
    /// concurrently with it may reach @p target ahead of the replayed ones, and two registries that
    /// forward to each other pass every change round without end.
    /// @param target Where changes are passed on to; must outlive this registry, and must not forward
    ///        back to it.
    void forward(AcceptLoopHealth& target);

    /// Records one report of an accept loop: a degraded loop is added, one that recovered or stopped
    /// is removed, and one that gave up is added for good. A @c AcceptLoopEventKind::Warning changes nothing
    /// here and is not passed on. What `serve`'s @c AcceptLoopReporting::onEvent is written to feed.
    /// @param event The report.
    void record(AcceptLoopEvent const& event);

    /// @return Every surface that is degraded or has given up, in the order it was reported; empty
    ///         while all serve.
    [[nodiscard]] std::vector<SurfaceCondition> snapshot() const;

  private:
    mutable std::mutex _mutex;
    std::vector<SurfaceCondition> _conditions; ///< Guarded by `_mutex`.
    Listener _listener;                        ///< Guarded by `_mutex`; copied out before it is called.
    AcceptLoopHealth* _forward {};             ///< Guarded by `_mutex`; see `forward`.
};

} // namespace core::net
