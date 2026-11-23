// SPDX-License-Identifier: Apache-2.0
#pragma once

/// @file
/// Which process is in front of a console that has no foreground process group to ask (Windows): a decision
/// over a snapshot of the process table, dependency-free so that it is tested on every platform.

#include <cstdint>
#include <optional>
#include <span>
#include <string>

namespace vtpty
{

/// One process of a process-table snapshot, as far as picking the foreground one cares.
struct ProcessEntry
{
    uint32_t pid = 0;          ///< Its process id.
    uint32_t parentPid = 0;    ///< The id its parent had when it was created; that id may since be reused.
    uint64_t creationTime = 0; ///< When it was created, in any monotonic unit; 0 when it could not be told.
    std::string name;          ///< Its image name (`pwsh.exe`), UTF-8.
};

/// Picks the process in front of a console whose first process is @p rootPid: the job leader, as a POSIX
/// terminal's foreground process group names it. A console has no process group, but a shell waits for the
/// command it runs: from the root down, while the current process is an interactive shell (cmd, pwsh, bash,
/// ...; matched as isSameProgram() does), go to its newest child; the first process that is no shell is the
/// leader, and its own children are not looked at -- an `ssh` running a `ProxyCommand` is `ssh`, not the
/// proxy. A shell without a child is idle, and itself in front. A batch-file wrapper runs in `cmd`, which is
/// a shell, so `npm` (`npm.cmd`) is named by the `node` it runs, where POSIX names it `npm`.
///
/// A process whose recorded parent id is the current one's but which was not created after it is no child:
/// the parent id was reused after its real parent ended. One whose creation time could not be told is not
/// followed either.
/// @param processes The snapshot, in any order.
/// @param rootPid The console's first process: the shell the terminal started.
/// @return The foreground process's name; nullopt when @p rootPid is not in @p processes (it has ended).
[[nodiscard]] std::optional<std::string> foregroundProcessOf(std::span<ProcessEntry const> processes,
                                                             uint32_t rootPid);

} // namespace vtpty
