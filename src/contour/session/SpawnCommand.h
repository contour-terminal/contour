// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <QtCore/QString>
#include <QtCore/QStringList>

#include <string>

namespace contour::session
{

/// A resolved "spawn a new contour process" command: the program to run plus its argument list.
struct SpawnTerminalCommand
{
    QString program;
    QStringList arguments;
};

/// Builds the command line for spawning a new detached contour process (pure; no launching), so the
/// argument assembly (config/profile/working-directory flags and the cwd-URL host filtering) is
/// unit-testable without starting a process.
/// @param programPath The contour executable path.
/// @param configPath The active config file (added as `config <path>` when non-empty).
/// @param profileName The profile to open (added as `profile <name>` when non-empty).
/// @param cwdUrl The working directory as a file URL; only a local-host path is forwarded.
/// @return The program + arguments to hand to a detached-process launcher.
[[nodiscard]] SpawnTerminalCommand buildSpawnTerminalCommand(std::string const& programPath,
                                                             std::string const& configPath,
                                                             std::string const& profileName,
                                                             std::string const& cwdUrl);

} // namespace contour::session
