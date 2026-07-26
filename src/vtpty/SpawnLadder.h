// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <string>
#include <vector>

namespace vtpty
{

/// One attempt at spawning a child process: what to run, and where.
struct SpawnAttempt
{
    std::string commandLine;
    std::string workingDirectory; ///< Empty means "inherit the parent's".
    std::string diagnostic;       ///< Empty on the first rung; else what this rung gave up, for the user.
};

/// Builds the ordered attempts for spawning a child process, most-faithful first.
///
/// POSIX discovers a bad working directory or a missing program in the CHILD, between fork() and
/// exec(), where it walks a ladder of its own: chdir, then the requested program, then the login
/// shell (see Process_unix.cpp). Windows has no fork — CreateProcess() fails in the PARENT — so the
/// same ladder has to be walked there explicitly. It was not: both attempts passed the same working
/// directory, so ERROR_DIRECTORY ("The directory name is invalid") failed twice by construction and
/// a new tab could not be opened at all. See issue #1711.
///
/// Rungs that would duplicate an earlier one are elided, so the result never retries the same thing
/// twice: no working directory means no directory-dropping rung, and a command that already IS the
/// login shell gets no login-shell rung.
///
/// @param commandLine      the command line the caller asked for.
/// @param workingDirectory the requested working directory; empty to inherit the parent's.
/// @param loginShell       the last-resort shell; empty for none.
///
/// @return the attempts in the order they should be tried; never empty for a non-empty command line.
[[nodiscard]] std::vector<SpawnAttempt> buildSpawnLadder(std::string const& commandLine,
                                                         std::string const& workingDirectory,
                                                         std::string const& loginShell);

} // namespace vtpty
