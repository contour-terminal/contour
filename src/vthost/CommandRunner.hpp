// SPDX-License-Identifier: Apache-2.0
#pragma once

/// @file
/// Running an external command and reading what it printed: the seam `systemctl` and `launchctl`
/// are reached through, so the service backends are tested without touching the host's init system.

#include <expected>
#include <memory>
#include <span>
#include <string>
#include <system_error>

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
};

/// @return A runner that starts real processes: `posix_spawnp` with standard output captured through a
///         pipe and standard error discarded. On Windows every run fails with `errc::not_supported`.
[[nodiscard]] std::unique_ptr<CommandRunner> makeNativeCommandRunner();

} // namespace vthost
