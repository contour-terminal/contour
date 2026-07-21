// SPDX-License-Identifier: Apache-2.0
#pragma once

/// @file
/// The daemon entry points behind `contour daemon` / `contour attach` — kept in
/// this Qt-free module so the whole serving path never touches the GUI stack.

#include <vtbackend/Settings.h>

#include <vtpty/Process.h>

#include <filesystem>
#include <optional>
#include <string>

namespace muxserver
{

/// Everything `contour daemon` needs to serve.
struct DaemonConfig
{
    /// The control-socket file (see muxSocketPath for derivation).
    std::filesystem::path socketPath;
    /// Factory settings for every hosted session's terminal.
    vtbackend::Settings settings;
    /// The shell each new session runs.
    vtpty::Process::ExecInfo shell;
    /// When set, ALSO binds tmux's own discovery path
    /// `/tmp/tmux-<uid>/<label>` for the imsg endpoint, so a plain
    /// `tmux -L <label> -C attach-session` finds this daemon. Opt-in only.
    std::optional<std::string> tmuxCompatLabel;
};

/// Runs the daemon: binds the hardened control socket, serves connections until
/// SIGINT/SIGTERM, then shuts down cleanly. Blocks the calling thread.
/// @param config The daemon configuration.
/// @return The process exit code (EXIT_SUCCESS on clean shutdown).
[[nodiscard]] int runDaemon(DaemonConfig const& config);

/// Attaches this terminal to a running daemon over the native cells+deltas
/// protocol: mirrors the remote screen onto the local TTY and forwards
/// keystrokes until the peer disconnects or Ctrl-\ detaches.
/// @param socketPath The daemon's control-socket file (the native socket
///        lives beside it).
/// @return The process exit code (EXIT_SUCCESS on clean detach).
[[nodiscard]] int runAttach(std::filesystem::path const& socketPath);

} // namespace muxserver
