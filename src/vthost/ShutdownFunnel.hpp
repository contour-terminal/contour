// SPDX-License-Identifier: Apache-2.0
#pragma once

/// @file
/// `ShutdownFunnel` — the one shutdown action every trigger of a daemon's shutdown shares.

#include <cstdint>
#include <functional>
#include <vector>

#include <vthost/DaemonLifecycle.hpp>

namespace core::net
{
class EventLoop;
}

namespace vthost
{

class ConnectionAcceptor;

/// Whether a ShutdownFunnel has shut its daemon down yet.
enum class ShutdownState : std::uint8_t
{
    Serving = 0,  ///< No shutdown was requested yet.
    ShuttingDown, ///< One was, and has run; every later request is ignored.
};

/// Shuts a daemon down, once: runs the `beforeShutdown` hook, stops accepting on every endpoint, then
/// unwinds the loop so each connection flow cancels.
///
/// Loop-thread only — @c EventLoop::requestStop is documented loop-thread-only. A trigger on another thread
/// (the signal watch, the console-control handler) or inside a connection's PDU handler (a StopDaemon) posts
/// its request; the last-session trigger is already on the loop and posts only to leave the observer
/// fan-out (see LastSessionWatcher).
///
/// Idempotent, because the triggers race: a StopDaemon and a SIGTERM may both arrive before the loop
/// unwinds, and a request may still be queued when it does. The first request is the shutdown; every later
/// one logs nothing and does nothing.
///
/// This skips the connections' graceful epilogues by design (see the "Daemon lifetime" section of
/// docs/internals/vthost.md); peers see a clean FIN rather than a goodbye.
class ShutdownFunnel
{
  public:
    /// @param loop The loop to unwind (not owned; outlives this).
    /// @param servers Every bound endpoint, read when the shutdown comes rather than now, so an endpoint
    ///        bound after construction is closed too (not owned; outlives this).
    /// @param beforeShutdown Runs once, with the cause, before any listener closes and while every session
    ///        is still alive — the seam a later phase hands sessions over through. May be empty.
    ShutdownFunnel(core::net::EventLoop& loop,
                   std::vector<ConnectionAcceptor*> const& servers,
                   std::function<void(ShutdownCause)> beforeShutdown);

    /// Shuts the daemon down, unless that has happened already.
    /// @param cause What asked for it, for the single log line and for `beforeShutdown`.
    void request(ShutdownCause cause);

    /// @return Whether the shutdown has run.
    [[nodiscard]] ShutdownState state() const noexcept { return _state; }

  private:
    core::net::EventLoop& _loop;
    std::vector<ConnectionAcceptor*> const& _servers;
    std::function<void(ShutdownCause)> _beforeShutdown;
    ShutdownState _state = ShutdownState::Serving;
};

} // namespace vthost
