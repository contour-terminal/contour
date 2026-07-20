// SPDX-License-Identifier: Apache-2.0
#pragma once

/// @file
/// The daemon entry points behind `contour daemon` / `contour attach` — kept in
/// this Qt-free module so the whole serving path never touches the GUI stack.

#include <vtbackend/Settings.h>

#include <vtpty/Process.h>

#include <filesystem>

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
};

/// Runs the daemon: binds the hardened control socket, serves connections until
/// SIGINT/SIGTERM, then shuts down cleanly. Blocks the calling thread.
/// @param config The daemon configuration.
/// @return The process exit code (EXIT_SUCCESS on clean shutdown).
[[nodiscard]] int runDaemon(DaemonConfig const& config);

/// Connects to a running daemon's control socket and reports reachability —
/// the attach handshake proper lands with the client protocol phases.
/// @param socketPath The control-socket file to probe.
/// @return EXIT_SUCCESS if a daemon accepted the connection.
[[nodiscard]] int runAttachProbe(std::filesystem::path const& socketPath);

} // namespace muxserver
