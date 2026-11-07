// SPDX-License-Identifier: Apache-2.0
#pragma once

/// @file
/// Telling the operating system how to start this application again. On Windows, Restart Manager closes a
/// running application to replace its files (an upgrade by the installer) and then starts it again with the
/// command line it registered; elsewhere nothing asks. Qt-free, so that the decision is testable anywhere.

#include <cstddef>
#include <expected>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <system_error>

namespace contour::platform
{

/// The longest command line RegisterApplicationRestart() takes, in UTF-16 code units: RESTART_MAX_CMD_LINE
/// (1024) less its terminator.
inline constexpr std::size_t MaxRestartCommandLine = 1023;

/// The arguments of a process's command line that a restart repeats.
/// @param commandLine The whole command line, the program name first; may be empty.
/// @return Everything after the program name: empty for an empty command line and for the program name alone.
///         A view into @p commandLine.
[[nodiscard]] std::span<std::string const> restartArguments(std::span<std::string const> commandLine);

/// The command line a restarted application is given.
/// @param arguments The arguments this process was started with, the program name left out.
/// @return The arguments as Windows spells them, or empty (a plain launch) where they are longer than
///         MaxRestartCommandLine UTF-16 code units: starting with fewer arguments beats not starting.
[[nodiscard]] std::string restartCommandLine(std::span<std::string const> arguments);

/// Registers with the operating system how to start this application again after it was closed to be
/// updated. An interface because it reaches the operating system; tests inject a recording one.
class ApplicationRestart
{
  public:
    ApplicationRestart() = default;
    ApplicationRestart(ApplicationRestart const&) = delete;
    ApplicationRestart& operator=(ApplicationRestart const&) = delete;
    ApplicationRestart(ApplicationRestart&&) = delete;
    ApplicationRestart& operator=(ApplicationRestart&&) = delete;
    virtual ~ApplicationRestart() = default;

    /// Registers the command line to start this application with again.
    /// @param commandLine The arguments, UTF-8, without the program name; may be empty.
    /// @return Nothing, or why the operating system refused.
    [[nodiscard]] virtual std::expected<void, std::error_code> registerRestart(
        std::string_view commandLine) = 0;
};

/// The ApplicationRestart this platform offers: Restart Manager's on Windows, a no-op elsewhere.
[[nodiscard]] std::unique_ptr<ApplicationRestart> makeApplicationRestart();

} // namespace contour::platform
