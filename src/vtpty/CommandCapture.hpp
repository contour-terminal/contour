// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <core/Environment.hpp>
#include <core/async/StopToken.hpp>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <expected>
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

    Kind kind;             ///< What went wrong.
    std::error_code cause; ///< The system error behind SpawnFailed; empty otherwise.
};

/// How many bytes of a command's standard output are kept; the rest is read and discarded.
inline constexpr std::size_t MaxCapturedOutput = std::size_t { 64 } * 1024;

/// How many bytes a single read from the command's output pipe takes.
inline constexpr std::size_t CaptureReadChunkSize = MaxCapturedOutput;

/// Runs a shell command and captures the first line of its standard output.
class CommandCapture
{
  public:
    virtual ~CommandCapture() = default;

    /// Runs @p command through the shell and waits for it, at most @p timeout.
    /// @param command Shell command line, UTF-8.
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
    SystemCommandCapture(std::string shell, core::async::StopToken shutdown);

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
};

/// @param output Raw standard output.
/// @return The first line of @p output: everything before the first '\n', minus one trailing '\r'.
[[nodiscard]] std::string firstLineOf(std::string_view output);

} // namespace vtpty
