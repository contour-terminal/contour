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
/// argument assembly (config/profile/working-directory flags) is unit-testable without starting a process.
/// @param programPath The contour executable path.
/// @param configPath The active config file (added as `config <path>` when non-empty).
/// @param profileName The profile to open (added as `profile <name>` when non-empty).
/// @param workingDirectory The directory the new process starts in, already resolved to a path on this
///                         machine (TerminalSession::workingDirectory()); forwarded as it is when non-empty.
/// @return The program + arguments to hand to a detached-process launcher.
[[nodiscard]] SpawnTerminalCommand buildSpawnTerminalCommand(std::string const& programPath,
                                                             std::string const& configPath,
                                                             std::string const& profileName,
                                                             std::string const& workingDirectory);

/// Builds `contour client --host NAME [--config PATH]`, which opens a window attached to a saved host (pure;
/// no launching). A process of its own, as a GUI process is attached to exactly one daemon.
/// @param programPath The contour executable path.
/// @param configPath The active config file, which names the host (added as `--config <path>` when
/// non-empty).
/// @param hostName The host's name under `hosts:`.
/// @return The program + arguments to hand to a detached-process launcher.
[[nodiscard]] SpawnTerminalCommand buildAttachHostCommand(std::string const& programPath,
                                                          std::string const& configPath,
                                                          std::string const& hostName);

} // namespace contour::session
