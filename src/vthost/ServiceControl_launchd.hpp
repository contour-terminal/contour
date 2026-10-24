// SPDX-License-Identifier: Apache-2.0
#pragma once

/// @file
/// The macOS backend of `contour daemon-service`: a launchd LaunchAgent, driven through `launchctl`.
///
/// Compiled on every platform, so its plist text and its command lines are tested everywhere; only the
/// factory in ServiceControl.cpp picks it, and only on macOS.

#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

#include <vthost/ServiceControl.hpp>

namespace vthost
{

/// @param registrationName The registration name, e.g. `contour-daemon`.
/// @return Its launchd label, `org.contourterminal.<name>`.
[[nodiscard]] std::string launchdLabelFor(std::string_view registrationName);

/// @param home The user's home directory.
/// @param label The launchd label.
/// @return `<home>/Library/LaunchAgents/<label>.plist`.
[[nodiscard]] std::filesystem::path launchAgentPath(std::filesystem::path const& home,
                                                    std::string_view label);

/// @param userId The invoking user's numeric id.
/// @return The launchd domain of that user's GUI session, `gui/<uid>`.
[[nodiscard]] std::string launchdGuiDomain(unsigned userId);

/// @param label The launchd label.
/// @param request What to run.
/// @return A plist that runs the request at load and restarts it only after a crash: `contour daemon
///         stop` ends in a clean exit, which must stay stopped.
[[nodiscard]] std::string renderLaunchAgentPlist(std::string_view label,
                                                 ServiceInstallRequest const& request);

/// @param plist The text of a plist rendered by renderLaunchAgentPlist.
/// @return Its `ProgramArguments`, unescaped.
[[nodiscard]] std::vector<std::string> parseLaunchAgentArguments(std::string_view plist);

/// @param exitCode The exit status of `launchctl print`.
/// @param output What it printed.
/// @return Stopped if it failed (registered but not loaded) or reports a state other than running.
[[nodiscard]] ServiceRunState parseLaunchctlPrint(int exitCode, std::string_view output);

/// Which agent a LaunchdBackend manages.
struct LaunchdAgentConfig
{
    std::string label {};               ///< The launchd label.
    std::filesystem::path plistPath {}; ///< Where its plist lives.
    std::string domain {};              ///< The launchd domain, `gui/<uid>`.
};

/// Hosts the daemon as a launchd LaunchAgent.
class LaunchdBackend final: public ServiceBackend
{
  public:
    /// @param config The agent to manage.
    /// @param services What the backend reaches the host through; must outlive the backend.
    LaunchdBackend(LaunchdAgentConfig config, ServiceBackendServices const& services);

    [[nodiscard]] std::expected<void, ServiceError> install(ServiceInstallRequest const& request) override;
    [[nodiscard]] std::expected<void, ServiceError> uninstall() override;
    [[nodiscard]] std::expected<void, ServiceError> start() override;
    [[nodiscard]] std::expected<void, ServiceError> stop() override;
    [[nodiscard]] std::expected<ServiceStatus, ServiceError> status() const override;
    [[nodiscard]] DaemonLaunchStyle launchStyle() const noexcept override;

  private:
    /// Runs `launchctl <arguments>`; a non-zero exit is a Backend error naming the verb.
    [[nodiscard]] std::expected<CommandOutcome, ServiceError> launchctl(
        std::vector<std::string> arguments) const;
    /// @return `<domain>/<label>`, how launchctl addresses a loaded service.
    [[nodiscard]] std::string target() const;

    LaunchdAgentConfig _config;
    core::platform::FileSystem& _fileSystem;
    CommandRunner& _commands;
};

} // namespace vthost
