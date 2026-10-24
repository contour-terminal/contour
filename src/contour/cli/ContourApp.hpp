// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <core/cli/App.hpp>

#include <cstdint>
#include <expected>
#include <filesystem>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace vthost
{
enum class DaemonLaunchStyle : std::uint8_t;
enum class ClientSizePolicy : std::uint8_t;
class IByteStream;
} // namespace vthost

namespace contour::cli
{

enum class DaemonVerb : std::uint8_t; // DaemonVerbs.hpp
struct ControlVerb;                   // ControlVerbs.hpp

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

/// How an option says it was given.
enum class GivenAs : std::uint8_t
{
    Text = 0, ///< A string option, given when it is not empty.
    Flag,     ///< A boolean option, given when it is true.
};

/// An option of a verb, named for a check on whether it was given.
struct OptionPresence
{
    std::string_view name; ///< The option, without its leading dashes.
    GivenAs givenAs;       ///< How it says it was given.
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

  protected:
    /// Finds the first of @p options that the command line gave.
    /// @param verbPrefix The verb's dotted parameter prefix, e.g. "contour.daemon".
    /// @param options The options to look for.
    /// @return The name of the first one given, if any.
    [[nodiscard]] std::optional<std::string_view> firstGivenOption(std::string_view verbPrefix,
                                                                   std::span<OptionPresence const> options);

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
    /// Writes the agent skill: `contour agent setup`.
    /// @return The process exit code.
    int agentSetupAction();
    /// Runs one control verb (`contour ls`, `contour send`, ...) against the daemon its flags name.
    /// @param verb The verb's row.
    /// @return The process exit code.
    int controlVerbAction(ControlVerb const& verb);
    /// Runs one `contour daemon <sub-verb>`, which talks to a running daemon instead of becoming one.
    /// @param verb Which sub-verb was invoked.
    /// @param prefix That sub-verb's dotted flag prefix, e.g. "contour.daemon.status".
    /// @return The process exit code.
    int daemonVerbAction(DaemonVerb verb, std::string const& prefix);
    /// Checks that `contour daemon --stdio` names no option only a daemon this process becomes can honour.
    /// @return The refusal, naming the option, when one is given.
    [[nodiscard]] std::expected<void, std::string> checkStdioCombinations();
    /// Runs `contour daemon --stdio`: relays the native protocol between this process's standard
    /// input/output and the daemon serving @p socketPath, starting that daemon when none serves.
    /// @param client This process's stdio, taken (makeProcessStdioStream()) before the configuration loaded.
    /// @param socketPath The daemon's control socket.
    /// @param sizePolicy The policy a daemon this relay starts runs with.
    /// @return EXIT_SUCCESS once the daemon side ended; EXIT_FAILURE for a daemon that could not be
    ///         reached (as an error preamble on stdout).
    int stdioRelayAction(std::shared_ptr<vthost::IByteStream> client,
                         std::filesystem::path const& socketPath,
                         vthost::ClientSizePolicy sizePolicy);
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
