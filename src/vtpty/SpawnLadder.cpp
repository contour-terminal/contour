// SPDX-License-Identifier: Apache-2.0
#include <vtpty/SpawnLadder.h>

#include <format>
#include <string>
#include <utility>
#include <vector>

namespace vtpty
{

std::vector<SpawnAttempt> buildSpawnLadder(std::string const& commandLine,
                                           std::string const& workingDirectory,
                                           std::string const& loginShell)
{
    auto attempts = std::vector<SpawnAttempt> {};

    // Rung 1: exactly what was asked for.
    attempts.emplace_back(
        SpawnAttempt { .commandLine = commandLine, .workingDirectory = workingDirectory, .diagnostic = {} });

    // Rung 2: the same command, but in our own directory. This is the rung that recovers the reported
    // failure -- an inherited working directory that has since been removed, or that a WSL/SSH shell
    // advertised but Windows cannot enter -- and it keeps the user's chosen shell.
    if (!workingDirectory.empty())
        attempts.emplace_back(SpawnAttempt {
            .commandLine = commandLine,
            .workingDirectory = {},
            .diagnostic = std::format("Failed to start in \"{}\". Using the current directory instead.",
                                      workingDirectory) });

    // Rung 3: give up on the command itself. Skipped when it would only repeat a rung above -- the
    // command already being the login shell makes this identical to rung 1 or 2.
    if (!loginShell.empty() && loginShell != commandLine)
        attempts.emplace_back(SpawnAttempt {
            .commandLine = loginShell,
            .workingDirectory = {},
            .diagnostic =
                workingDirectory.empty()
                    ? std::format(
                          "Failed to spawn {}. Using login shell {} instead.", commandLine, loginShell)
                    : std::format("Failed to spawn {} in \"{}\". Using login shell {} in the current "
                                  "directory instead.",
                                  commandLine,
                                  workingDirectory,
                                  loginShell) });

    return attempts;
}

} // namespace vtpty
