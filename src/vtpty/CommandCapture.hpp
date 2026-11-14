// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <core/Environment.hpp>
#include <core/async/StopToken.hpp>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>

namespace vtpty
{

/// Why a captured command produced no output.
struct CommandCaptureError
{
    /// What went wrong.
    enum class Kind : uint8_t
    {
        SpawnFailed, ///< The shell could not be started; @ref cause says why.
        TimedOut,    ///< The run exceeded its timeout and was killed.
        Cancelled,   ///< A stop was requested and the run was killed.
    };

    Kind kind {};          ///< What went wrong.
    std::error_code cause; ///< The system error behind SpawnFailed; empty otherwise.
};

/// How many bytes of a command's standard output are kept; the rest is read and discarded.
inline constexpr std::size_t MaxCapturedOutput = std::size_t { 64 } * 1024;

/// How many bytes a single read from the command's output pipe takes.
inline constexpr std::size_t CaptureReadChunkSize = MaxCapturedOutput;

/// Runs a command and captures the first line of its standard output.
class CommandCapture
{
  public:
    virtual ~CommandCapture() = default;

    /// Runs @p command and waits for it, at most @p timeout.
    /// @param command The command line, UTF-8: a shell command for SystemCommandCapture, a program's own
    ///                command line for ProgramCommandCapture.
    /// @param timeout Upper bound on the run; the command and its children are killed when it elapses.
    /// @param stop    A stop request kills the command and its children at once.
    /// @return The first line of standard output (without its line break and one trailing '\r'),
    ///         or why there is none. The exit status is ignored.
    [[nodiscard]] virtual std::expected<std::string, CommandCaptureError> run(
        std::string_view command, std::chrono::milliseconds timeout, core::async::StopToken stop) = 0;
};

/// Runs commands through the platform shell: `<shell> -c <command>` on Unix,
/// `"<shell>" /c <command>` on Windows (the command line `popen` / `_popen` build).
class SystemCommandCapture final: public CommandCapture
{
  public:
    /// @param shell    Path of the shell to run commands with, UTF-8.
    /// @param shutdown A stop request kills every run in flight and makes later runs return Cancelled
    ///                 at once. Pass a default-constructed token for a policy of never stopping.
    /// @param descriptorLimit The soft descriptor limit each command runs under, set by its shell before the
    ///                 command (`ulimit -S -n`), never in this process; nullopt for this process's own. What
    ///                 a daemon behind a PTY keeper, whose own limit the keeper raised, gives its commands
    ///                 back. POSIX only.
    SystemCommandCapture(std::string shell,
                         core::async::StopToken shutdown,
                         std::optional<std::uint64_t> descriptorLimit = std::nullopt);

    /// @param env The environment to resolve the shell from.
    /// @return The shell this platform's `popen` uses: `/bin/sh` on Unix; `%COMSPEC%`, else `cmd.exe`, on
    /// Windows.
    [[nodiscard]] static std::string defaultShell(core::Environment const& env);

    /// @copydoc CommandCapture::run
    [[nodiscard]] std::expected<std::string, CommandCaptureError> run(std::string_view command,
                                                                      std::chrono::milliseconds timeout,
                                                                      core::async::StopToken stop) override;

  private:
    std::string _shell;
    core::async::StopToken _shutdown;
#ifndef _WIN32
    std::optional<std::uint64_t> _descriptorLimit; ///< Only a POSIX shell sets one.
#endif
};

#ifdef _WIN32
/// Runs a program by its own command line, with no shell between. What a GUI-subsystem program that attaches
/// to its parent's console needs, as a release build of Contour is: attaching replaces each standard handle
/// the program was not started with as such (STARTF_USESTDHANDLES), and `cmd.exe` hands its children its own
/// by inheritance alone, so through SystemCommandCapture the program writes to `cmd.exe`'s console instead.
class ProgramCommandCapture final: public CommandCapture
{
  public:
    /// @param shutdown A stop request kills every run in flight and makes later runs return Cancelled
    ///                 at once. Pass a default-constructed token for a policy of never stopping.
    explicit ProgramCommandCapture(core::async::StopToken shutdown);

    /// @copydoc CommandCapture::run
    /// @p command is what CreateProcess() runs: the program, then its arguments, as
    /// buildWindowsCommandLine() spells them.
    [[nodiscard]] std::expected<std::string, CommandCaptureError> run(std::string_view command,
                                                                      std::chrono::milliseconds timeout,
                                                                      core::async::StopToken stop) override;

  private:
    core::async::StopToken _shutdown;
};
#endif

/// @param output Raw standard output.
/// @return The first line of @p output: everything before the first '\n', minus one trailing '\r'.
[[nodiscard]] std::string firstLineOf(std::string_view output);

} // namespace vtpty
