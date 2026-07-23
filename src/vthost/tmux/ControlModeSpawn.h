// SPDX-License-Identifier: Apache-2.0
#pragma once

/// @file
/// Spawns a real `tmux -C attach-session` as a child on its own PTY — tmux's
/// client insists on a terminal — and adopts the master side as a reactor
/// transport (net::adoptFd handles PTY masters: ENOTSOCK fallback, EIO=EOF).
/// This is the production promotion of the gateway oracle test's harness.

#include <expected>
#include <memory>
#include <string>

#include <net/EventLoop.h>
#include <net/ISocket.h>

namespace vthost::tmux
{

/// A spawned control-mode tmux client: reap the pid after the transport EOFs.
struct SpawnedControlMode
{
    int pid = -1;
    std::unique_ptr<net::ISocket> transport;
};

/// Forks `tmux -C attach-session` (against @p tmuxSocket via -S when
/// non-empty) on a fresh PTY.
/// @param loop The reactor that will drive the transport.
/// @param tmuxSocket Optional tmux server socket path (-S).
/// @return The child and its adopted transport, or a reason on failure
///         (POSIX only; unsupported on Windows).
[[nodiscard]] std::expected<SpawnedControlMode, std::string> spawnControlMode(
    net::EventLoop& loop, std::string const& tmuxSocket = {});

/// Reaps the spawned client: bounded wait, then SIGKILL. No-op for pid < 0.
void reapControlMode(int pid);

} // namespace vthost::tmux
