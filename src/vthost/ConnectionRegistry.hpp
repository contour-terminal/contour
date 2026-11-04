// SPDX-License-Identifier: Apache-2.0
#pragma once

/// @file
/// `ConnectionRegistry` -- which connections a daemon serves right now, so that it can close them all: what a
/// live upgrade does before it execs, for the connections nothing else is told to end (tmux control mode,
/// imsg, the compat endpoint).

#include <cstddef>
#include <memory>
#include <unordered_set>

#include <vthost/ConnectionAcceptor.hpp>

namespace core::net
{
class ISocket;
}

namespace vthost
{

/// Tracks the sockets of the connections its tracked() handlers serve. Loop thread only.
///
/// The set lives in state shared with every tracked handler and registration, so a registry may be destroyed
/// before the connection flows it tracked are reaped (the loop reaps them last).
class ConnectionRegistry
{
  public:
    ConnectionRegistry(): _state(std::make_shared<State>()) {}

    /// Wraps @p handler so that each connection it serves is tracked for its whole life.
    /// @param handler The connection handler to wrap.
    /// @return The tracking handler.
    [[nodiscard]] ConnectionHandler tracked(ConnectionHandler handler) const;

    /// Closes every tracked connection's socket at once: a parked read or write ends as cancelled, and the
    /// connection's flow unwinds on the loop. Whatever is queued unwritten is lost, so a connection that
    /// ends in an orderly way of its own does not belong here.
    void closeAll() const;

    /// @return How many connections are tracked.
    [[nodiscard]] std::size_t size() const noexcept { return _state->sockets.size(); }

  private:
    /// The sockets of the tracked connections.
    struct State
    {
        std::unordered_set<core::net::ISocket*> sockets;
    };

    std::shared_ptr<State> _state;
};

} // namespace vthost
