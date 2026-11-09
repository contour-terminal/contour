// SPDX-License-Identifier: Apache-2.0
#pragma once

/// @file
/// Starting a local daemon the user named ("New daemon…", spec §Command palette): one that lives like a
/// daemon `contour daemon` started -- never ends on its own -- rather than like one a client auto-started.

#include <vtpty/ProcessPlacement.hpp>

#include <chrono>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <string>

#include <vthost/DaemonBinary.hpp>
#include <vthost/DaemonSpawn.hpp>

namespace contour::cli
{

/// What starting a named daemon found.
enum class NamedDaemonStart : uint8_t
{
    Started = 0,    ///< None served there; one was started, and serves now.
    AlreadyRunning, ///< One serves there already: nothing was started.
};

/// What spawning a named daemon needs; the references outlive the call.
struct NamedDaemonSpawn
{
    vthost::DaemonSpawner& spawner;     ///< Starts the process and asks whether a daemon serves.
    vthost::DaemonBinarySource& binary; ///< What the daemon is started from.
    vtpty::ProcessPlacement& placement; ///< Places the spawned daemon. POSIX only.
    vthost::DaemonSpawnOptions options; ///< Forwarded to the daemon's command line.
    std::chrono::seconds timeout;       ///< How long to wait for it to serve.
};

/// Starts a persistent daemon (vthost::DaemonLifecycle::Persistent) at @p socket, unless one serves there.
/// Blocks for up to the spawn's timeout: call it off the GUI thread.
/// @param socket The control socket of the label asked for.
/// @param spawn What the spawn needs.
/// @return Whether one was started or already served; why none serves otherwise.
[[nodiscard]] std::expected<NamedDaemonStart, std::string> startNamedDaemon(
    std::filesystem::path const& socket, NamedDaemonSpawn const& spawn);

} // namespace contour::cli
