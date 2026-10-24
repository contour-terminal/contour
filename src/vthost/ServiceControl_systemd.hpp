// SPDX-License-Identifier: Apache-2.0
#pragma once

/// @file
/// The Linux backend of `contour daemon-service`: a systemd USER unit, driven through `systemctl --user`.
///
/// Compiled on every platform, so its unit text and its command lines are tested everywhere; only the
/// factory in ServiceControl.cpp picks it, and only on Linux.

#include <filesystem>
#include <map>
#include <string>
#include <string_view>
#include <vector>

#include <vthost/ServiceControl.hpp>

namespace vthost
{

/// Quotes one word of an `ExecStart=` line.
/// @param argument The word.
/// @return It in double quotes with `\` and `"` escaped, and `%` and `$` doubled (systemd expands both).
[[nodiscard]] std::string systemdQuote(std::string_view argument);

/// @param request What to run and how to describe it.
/// @return The text of the unit that runs exactly @p request's command line.
[[nodiscard]] std::string renderSystemdUnit(ServiceInstallRequest const& request);

/// @param environment Where `XDG_CONFIG_HOME` and `HOME` are read from.
/// @return `$XDG_CONFIG_HOME/systemd/user`, else `~/.config/systemd/user`; empty if neither can be
/// determined.
[[nodiscard]] std::filesystem::path systemdUserUnitDirectory(core::Environment const& environment);

/// @param output The text `systemctl show` printed: one `Key=Value` per line.
/// @return Those pairs; lines without `=` are skipped.
[[nodiscard]] std::map<std::string, std::string, std::less<>> parseSystemctlShow(std::string_view output);

/// Which unit a SystemdUserBackend manages.
struct SystemdUnitConfig
{
    std::string unitName {};           ///< The unit as systemctl names it, e.g. `contour-daemon.service`.
    std::filesystem::path unitPath {}; ///< Where its unit file lives.
};

/// Hosts the daemon as a systemd user unit.
class SystemdUserBackend final: public ServiceBackend
{
  public:
    /// @param config The unit to manage.
    /// @param services What the backend reaches the host through; must outlive the backend.
    SystemdUserBackend(SystemdUnitConfig config, ServiceBackendServices const& services);

    [[nodiscard]] std::expected<void, ServiceError> install(ServiceInstallRequest const& request) override;
    [[nodiscard]] std::expected<void, ServiceError> uninstall() override;
    [[nodiscard]] std::expected<void, ServiceError> start() override;
    [[nodiscard]] std::expected<void, ServiceError> stop() override;
    [[nodiscard]] std::expected<ServiceStatus, ServiceError> status() const override;
    [[nodiscard]] DaemonLaunchStyle launchStyle() const noexcept override;

  private:
    /// Runs `systemctl --user <arguments>`; a non-zero exit is a Backend error naming the verb.
    [[nodiscard]] std::expected<CommandOutcome, ServiceError> systemctl(
        std::vector<std::string> arguments) const;
    /// @return The argv of `systemctl --user show` for the unit's load and active state.
    [[nodiscard]] std::vector<std::string> showArgv() const;
    /// Runs `systemctl --user show` for the unit's load and active state.
    [[nodiscard]] std::expected<CommandOutcome, ServiceError> show() const;

    SystemdUnitConfig _config;
    core::platform::FileSystem& _fileSystem;
    CommandRunner& _commands;
};

} // namespace vthost
