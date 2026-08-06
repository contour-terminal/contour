// SPDX-License-Identifier: Apache-2.0
#pragma once

/// @file
/// The daemon's binary tmux endpoint: a REAL tmux client binary
/// (`tmux -S <sock> -C attach-session`) connects here speaking the
/// rewritten imsg protocol. After the MSG_IDENTIFY handshake (which passes
/// the client's STDIN/STDOUT via SCM_RIGHTS) and the MSG_COMMAND startup
/// command, the oracle-verified control-mode line protocol runs over the
/// PASSED descriptors — the imsg socket carries only session lifecycle
/// (MSG_EXITING → MSG_EXITED, and our MSG_EXIT on detach).
///
/// POSIX only: SCM_RIGHTS has no Windows equivalent.

#include <functional>
#include <memory>

#include <coro/Task.hpp>
#include <net/EventLoop.hpp>
#include <net/ISocket.hpp>
#include <vthost/ConnectionAcceptor.hpp>
#include <vthost/SessionHost.hpp>

namespace vthost::tmux
{

/// The daemon's connection-handler factory for imsg (binary tmux) clients.
/// @param loop The event loop.
/// @param host The session host (not owned; must outlive the daemon's serving).
/// @return A handler suitable for ConnectionAcceptor's constructor.
[[nodiscard]] ConnectionHandler makeTmuxImsgHandler(net::EventLoop& loop, SessionHost& host);

} // namespace vthost::tmux
