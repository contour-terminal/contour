// SPDX-License-Identifier: Apache-2.0
#pragma once

/// @file
/// Running an external command and reading what it printed: the seam `systemctl` and `launchctl`
/// are reached through, so the service backends are tested without touching the host's init system.

#include <chrono>
#include <expected>
#include <memory>
#include <span>
#include <string>
#include <system_error>
#include <vector>

namespace vthost
{

/// What a finished command left behind.
struct CommandOutcome
{
    int exitCode = 0;              ///< The exit status; 128 + signal number if it was killed by a signal.
    std::string standardOutput {}; ///< Everything it wrote to standard output.
};

/// Runs one external command to completion.
class CommandRunner
{
  public:
    CommandRunner() = default;
    virtual ~CommandRunner() = default;

    CommandRunner(CommandRunner const&) = delete;
    CommandRunner& operator=(CommandRunner const&) = delete;
    CommandRunner(CommandRunner&&) = delete;
    CommandRunner& operator=(CommandRunner&&) = delete;

    /// Runs @p argv and waits for it.
    /// @param argv The program (looked up on PATH) followed by its arguments.
    /// @return What it printed and how it ended, or why it could not be started at all, e.g.
    ///         `std::errc::no_such_file_or_directory` for a program that is not installed.
    [[nodiscard]] virtual std::expected<CommandOutcome, std::error_code> run(
        std::span<std::string const> argv) = 0;

    /// Runs @p argv and waits for it, bounded by @p deadline: a command still running then is ended with its
    /// whole process group, and the run fails with `std::errc::timed_out`. The native runner sends its group
    /// SIGTERM at @p deadline, and SIGKILL as soon as the command has ended -- at once for one that ends on
    /// SIGTERM -- or 2 s later for one that does not (ProcessRunnerTuning::terminateGrace); it answers once
    /// the command is reaped. Before the deadline, a command whose leader exited while something it started
    /// holds its output is given 1 s (orphanedOutputGrace). A runner that cannot bound a command runs it to
    /// completion, as run() does.
    /// @param argv The program (looked up on PATH) followed by its arguments.
    /// @param deadline How long the command may take.
    /// @return As run(), or `std::errc::timed_out` for a command that outlived @p deadline.
    [[nodiscard]] virtual std::expected<CommandOutcome, std::error_code> runWithin(
        std::span<std::string const> argv, std::chrono::milliseconds deadline)
    {
        (void) deadline;
        return run(argv);
    }
};

/// @return A runner that starts real processes through a vtpty::ThreadedProcessRunner of its own, under this
///         process's environment as it stands at each run and looked up on that environment's PATH: standard
///         output captured, standard error drained and dropped, and a command that outlives runWithin()'s
///         deadline terminated with its whole process group. A runner whose process runner could not be made
///         fails every run with an error code that names why (and compares equal to
///         `errc::resource_unavailable_try_again`). On Windows every run fails with
///         `errc::not_supported`.
[[nodiscard]] std::unique_ptr<CommandRunner> makeNativeCommandRunner();

/// A native runner for a caller whose other threads may change the environment while it runs a command:
/// every command runs under @p environment, its program looked up on that environment's PATH, and running
/// one reads nothing of this process's environment.
/// @param environment What every command runs under ("NAME=VALUE").
/// @return The runner. On Windows every run fails with `errc::not_supported`.
[[nodiscard]] std::unique_ptr<CommandRunner> makeNativeCommandRunner(std::vector<std::string> environment);

} // namespace vthost
