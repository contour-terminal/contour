// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <core/cli/App.hpp>

#include <cstdint>
#include <expected>
#include <filesystem>
#include <string>
#include <vector>

namespace vthost
{
enum class DaemonLaunchStyle : std::uint8_t;
}

namespace contour::cli
{

enum class DaemonVerb : std::uint8_t; // DaemonVerbs.hpp

/// What `contour daemon-service <verb>` was asked to do.
enum class DaemonServiceAction : std::uint8_t
{
    Install,
    Uninstall,
    Start,
    Stop,
    Restart,
    Status,
};

/// Contour CLI application base.
///
/// TODO: provide special installable targets in debian packages (cmake and PPA)
class ContourApp: public core::cli::App
{
  public:
    /// @param env The process environment every part of the application reads through.
    explicit ContourApp(core::Environment const& env);

    [[nodiscard]] core::cli::Command parameterDefinition() const override;

  private:
    int captureAction();
    int listDebugTagsAction();
    int parserTableAction();
    int profileAction();
    int terminfoAction();
    int configAction();
    int integrationAction();
    int infoVT();
    int documentationVT();
    int documentationKeyMapping();
    int documentationGlobalConfig();
    int documentationProfileConfig();
    /// Displays an image in the terminal via GIP oneshot sequence.
    int catAction();
    /// Runs the headless terminal multiplexer daemon.
    int daemonAction();
    /// Runs one `contour daemon <sub-verb>`, which talks to a running daemon instead of becoming one.
    /// @param verb Which sub-verb was invoked.
    /// @param prefix That sub-verb's dotted flag prefix, e.g. "contour.daemon.status".
    /// @return The process exit code.
    int daemonVerbAction(DaemonVerb verb, std::string const& prefix);
    /// Relaunches this process detached (`--background`) and waits until it is serving.
    /// @param socketPath The control socket the daemon binds; also derives its default log file.
    /// @return EXIT_SUCCESS once the background daemon accepts connections.
    int runDaemonInBackground(std::filesystem::path const& socketPath);
    /// Runs one `contour daemon-service` verb against the configured backend.
    /// @param action Which verb was invoked.
    /// @param prefix That verb's dotted flag prefix, e.g. "contour.daemon-service.install".
    /// @return EXIT_SUCCESS if the operation succeeded.
    int daemonServiceAction(DaemonServiceAction action, std::string const& prefix);
    /// The argv an installed registration is given, with every path already absolute.
    /// @param prefix The invoking verb's dotted flag prefix.
    /// @param label The socket label the daemon serves.
    /// @param launchStyle Whether the supervising backend wants the daemon in the foreground or detached.
    /// @return The argv (argv[0] first), or why the options could not be resolved.
    [[nodiscard]] std::expected<std::vector<std::string>, std::string> daemonServiceCommandLine(
        std::string const& prefix, std::string const& label, vthost::DaemonLaunchStyle launchStyle) const;
};

} // namespace contour::cli
