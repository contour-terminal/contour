// SPDX-License-Identifier: Apache-2.0
#pragma once

/// @file
/// Spawns a real `tmux -C attach-session` as a child on its own PTY — tmux's
/// client insists on a terminal — and adopts the master side as a reactor
/// transport (core::net::adoptFd handles PTY masters: ENOTSOCK fallback, EIO=EOF).
/// This is the production promotion of the gateway oracle test's harness.

#include <core/net/EventLoop.hpp>
#include <core/net/ISocket.hpp>

#include <expected>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace vthost::tmux
{

/// A spawned control-mode tmux client: reap the pid after the transport EOFs.
struct SpawnedControlMode
{
    int pid = -1;
    std::unique_ptr<core::net::ISocket> transport;
};

/// @param tmuxSocket Optional tmux server socket path (-S).
/// @return The argument vector of a control-mode client: `tmux -C [-S <socket>] attach-session`.
[[nodiscard]] std::vector<std::string> controlModeArguments(std::string_view tmuxSocket);

/// Forks `tmux -C attach-session` (against @p tmuxSocket via -S when
/// non-empty) on a fresh PTY. tmux is looked up on @p searchPath and its
/// argument vector built before the fork, so the child calls only
/// async-signal-safe functions until it execs.
/// @param loop The reactor that will drive the transport.
/// @param tmuxSocket Optional tmux server socket path (-S).
/// @param searchPath The PATH tmux is looked for on.
/// @return The child and its adopted transport, or a reason on failure -- tmux not
///         found among them (POSIX only; unsupported on Windows).
[[nodiscard]] std::expected<SpawnedControlMode, std::string> spawnControlMode(core::net::EventLoop& loop,
                                                                              std::string const& tmuxSocket,
                                                                              std::string_view searchPath);

/// Reaps the spawned client: bounded wait, then SIGKILL. No-op for pid < 0.
void reapControlMode(int pid);

} // namespace vthost::tmux
