// SPDX-License-Identifier: Apache-2.0
#pragma once

/// @file
/// Installing the daemon as an OS-managed background service, so it starts without anyone
/// typing `contour daemon`.
///
/// Two mechanisms hide behind one set of verbs, because on Windows no single one does both
/// halves of what is wanted:
///
/// - **A Scheduled Task** starts at LOGON, as the invoking user, in that user's own session,
///   and needs neither elevation nor a stored password (TASK_LOGON_INTERACTIVE_TOKEN).
/// - **An SCM service** starts at BOOT (or on demand), but the public service-trigger set has
///   no logon trigger at all, and registering one under a named account requires that
///   account's password plus SeServiceLogonRight.
///
/// So @ref ServiceStartMode selects the backend rather than merely configuring one, and
/// `Logon` is the default: a terminal multiplexer is a per-user, per-session thing, and the
/// session-0 isolation a boot service lives under is the wrong shape for it (a shell spawned
/// there runs in the service's workspace station, where any GUI program it launches is invisible).

#include <core/Environment.hpp>
#include <core/platform/FileSystem.hpp>

#include <array>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <vthost/CommandRunner.hpp>

namespace vthost
{

/// When an installed daemon starts, and thereby WHICH mechanism hosts it.
enum class ServiceStartMode : std::uint8_t
{
    /// A Scheduled Task triggered by the installing user's logon. The default; the only mode
    /// that starts with a user session, and the only one needing no elevation.
    Logon = 0,
    /// An SCM service started at boot (SERVICE_AUTO_START), in session 0.
    Boot,
    /// An SCM service started only when asked (SERVICE_DEMAND_START), in session 0.
    Manual,
};

/// The spelling of each start mode, for the CLI and for diagnostics. One table: a mode added
/// here is parseable, printable and listable in the help without touching anything else.
inline constexpr auto ServiceStartModeNames = std::array {
    std::pair { std::string_view { "logon" }, ServiceStartMode::Logon },
    std::pair { std::string_view { "boot" }, ServiceStartMode::Boot },
    std::pair { std::string_view { "manual" }, ServiceStartMode::Manual },
};

/// @param mode The start mode.
/// @return Its CLI spelling.
[[nodiscard]] std::string_view nameOf(ServiceStartMode mode) noexcept;

/// @param text A CLI spelling, e.g. "logon".
/// @return The mode it names, or nullopt if it names none.
[[nodiscard]] std::optional<ServiceStartMode> serviceStartModeFrom(std::string_view text);

/// Why a service operation could not be carried out.
enum class ServiceErrorCode : std::uint8_t
{
    AlreadyInstalled, ///< Install found an existing registration under the same name.
    NotInstalled,     ///< An operation needing a registration found none.
    AccessDenied,     ///< The caller lacks the rights (an SCM install needs elevation).
    NotRunning,       ///< Stop found nothing to stop.
    Unsupported,      ///< No backend exists for this platform.
    Backend,          ///< The OS refused; see the context and system code.
    NotUtf8,          ///< A name, program or argument is not UTF-8; the context says which.
};

/// A service-control failure: the category, the raw OS status, and what was being attempted.
struct ServiceError
{
    ServiceErrorCode code = ServiceErrorCode::Backend; ///< The category.
    long systemCode = 0;                               ///< GetLastError()/HRESULT, or 0.
    std::string context {};                            ///< The failing call, for diagnostics.

    /// @return A descriptive string combining the category, context and OS status.
    [[nodiscard]] std::string toString() const;
};

/// Whether an installed daemon is currently running.
enum class ServiceRunState : std::uint8_t
{
    NotInstalled, ///< No registration exists.
    Stopped,      ///< Registered, not running.
    Running,      ///< Registered and running.
    Disabled,     ///< Registered, but switched off: it cannot start (the Task Scheduler's disabled task).
};

/// How each run state reads in `daemon-service status`. One table rather than a chain of
/// conditionals, so a fourth state is a row here and not an edit at every report site.
inline constexpr auto ServiceRunStateNames = std::array {
    std::pair { std::string_view { "not installed" }, ServiceRunState::NotInstalled },
    std::pair { std::string_view { "installed, stopped" }, ServiceRunState::Stopped },
    std::pair { std::string_view { "running" }, ServiceRunState::Running },
    std::pair { std::string_view { "installed, disabled" }, ServiceRunState::Disabled },
};

/// @param state The run state to name.
/// @return How it reads in a status report.
[[nodiscard]] constexpr std::string_view nameOf(ServiceRunState state) noexcept
{
    for (auto const& [name, value]: ServiceRunStateNames)
        if (value == state)
            return name;
    return "not installed";
}

/// What an installed registration reports about itself.
struct ServiceStatus
{
    ServiceRunState state = ServiceRunState::NotInstalled; ///< Installed and/or running.
    ServiceStartMode mode = ServiceStartMode::Logon;       ///< Which backend holds it.
    std::string commandLine {};                            ///< What it was registered to run.
    /// The program the registration starts; empty where the backend reports it only inside its command line
    /// (systemd, launchd).
    std::filesystem::path program {};
};

/// Everything an install must pin down.
///
/// The command line is supplied whole rather than rebuilt here for the same reason
/// runDaemonDetached takes one: the CLI surface lives in `src/contour`, and a registration
/// that outlives the shell which created it must not depend on the environment that shell
/// happened to have. Both the socket and the log file are therefore absolute by the time
/// they reach this struct — a service's %TEMP% and %USERNAME% need not be the interactive
/// user's, so a path left to be derived later would resolve somewhere else entirely.
struct ServiceInstallRequest
{
    std::vector<std::string> commandLine {};         ///< Full argv for the daemon (argv[0] first).
    ServiceStartMode mode = ServiceStartMode::Logon; ///< Which backend, and when it starts.
    std::string displayName {};                      ///< Human-readable name in the OS UI.
    std::string description {};                      ///< Longer text for the OS UI.
};

/// How a registered daemon relates to the process the OS starts for it.
enum class DaemonLaunchStyle : std::uint8_t
{
    /// The OS supervises the process it started, so the daemon must stay in the foreground (systemd,
    /// launchd).
    Foreground = 0,
    /// The OS only starts a launcher, so the daemon backgrounds itself (`--background`; the Task Scheduler).
    SelfDetaching,
};

/// One OS mechanism for hosting the daemon. An interface so the verb layer is testable
/// against a fake: registering a real service needs elevation, and registering a real task
/// mutates the developer's own account — neither belongs in a unit test.
class ServiceBackend
{
  public:
    ServiceBackend() = default;
    virtual ~ServiceBackend() = default;

    ServiceBackend(ServiceBackend const&) = delete;
    ServiceBackend& operator=(ServiceBackend const&) = delete;
    ServiceBackend(ServiceBackend&&) = delete;
    ServiceBackend& operator=(ServiceBackend&&) = delete;

    /// Registers the daemon so the OS starts it per @p request.
    [[nodiscard]] virtual std::expected<void, ServiceError> install(ServiceInstallRequest const& request) = 0;

    /// Removes the registration, stopping it first if it is running.
    [[nodiscard]] virtual std::expected<void, ServiceError> uninstall() = 0;

    /// Starts the registered daemon now, without waiting for its trigger.
    [[nodiscard]] virtual std::expected<void, ServiceError> start() = 0;

    /// Stops the running daemon, leaving the registration in place.
    [[nodiscard]] virtual std::expected<void, ServiceError> stop() = 0;

    /// @return What the registration reports, or why it could not be queried.
    [[nodiscard]] virtual std::expected<ServiceStatus, ServiceError> status() const = 0;

    /// Points the registration at @p program, keeping its arguments, trigger and settings. How a registration
    /// whose program an upgrade moved or removed is made to start the installed one again.
    /// @param program The executable the registration is to start.
    /// @return Nothing, or why the registration could not be changed; Unsupported where the backend names its
    ///         program only inside a command line it does not take apart.
    [[nodiscard]] virtual std::expected<void, ServiceError> retarget(
        std::filesystem::path const& program) = 0;

    /// @return Whether the registered command must run in the foreground or detach itself.
    [[nodiscard]] virtual DaemonLaunchStyle launchStyle() const noexcept = 0;
};

/// What a service backend reaches the host through, so tests substitute fakes for all of it.
struct ServiceBackendServices
{
    core::platform::FileSystem& fileSystem; ///< Where unit files and plists are written.
    CommandRunner& commands;                ///< How `systemctl` and `launchctl` are run.
    core::Environment const& environment;   ///< Where the home and config directories come from.
};

/// Builds the backend that hosts @p mode for the registration named @p name.
///
/// @param mode Which mechanism to address.
/// @param name The registration name (a task name, or an SCM service name).
/// @param services What the backend reaches the host through; must outlive the backend.
/// @return The backend; on a platform with no implementation, one whose every operation
///         reports @c ServiceErrorCode::Unsupported rather than a null pointer, so callers
///         have one error path instead of two.
[[nodiscard]] std::unique_ptr<ServiceBackend> makeServiceBackend(ServiceStartMode mode,
                                                                 std::string_view name,
                                                                 ServiceBackendServices const& services);

/// Runs a service-management tool such as `systemctl` or `launchctl`.
/// @param commands What runs it.
/// @param argv The program and its arguments.
/// @param context What a failure is attributed to, e.g. `systemctl --user show`.
/// @return What it printed; a Backend error if the program could not be started ("<program> not found") or
///         exited non-zero (@p context, the exit code as the system code).
[[nodiscard]] std::expected<CommandOutcome, ServiceError> runServiceCommand(
    CommandRunner& commands, std::vector<std::string> const& argv, std::string_view context);

/// The registration name for a daemon serving socket label @p label.
///
/// Labels already distinguish daemon instances, so they distinguish their registrations too;
/// without that, installing a second labelled daemon would silently overwrite the first.
/// @param label The socket label (DefaultSocketLabel for the unnamed one).
/// @return `contour-daemon` for the default label, else `contour-daemon-<label>`.
[[nodiscard]] std::string serviceNameForLabel(std::string_view label);

/// The names a Task Scheduler task for the registration @p name may carry.
///
/// Task names share one folder for the whole machine, so a second account's `daemon-service install` would be
/// refused, or, elevated, replace the first account's task. A task is named for its user, with @p userKey
/// (the account's SID) appended; the machine-wide name of the releases before that is still looked up, so
/// such a task is found, started and removed.
/// @param name The registration name (@see serviceNameForLabel).
/// @param userKey The account the task belongs to; empty for none (every other backend is per-user already).
/// @return The name to create first, then the machine-wide one where it differs.
[[nodiscard]] std::vector<std::string> taskNamesFor(std::string_view name, std::string_view userKey);

/// Expands the `%VARIABLE%` references of a text, as the Task Scheduler does for a task's program.
using VariableExpander = std::function<std::string(std::string_view)>;

/// The program a Task Scheduler action starts, as a path that can be looked for.
///
/// The Task Scheduler takes the program as the user typed it: in quotes where it holds spaces (the UI writes
/// it so), and with `%VARIABLES%` that it expands itself. Compared as stored, either form would look like a
/// program that does not exist, although the task works.
/// @param raw The action's path as stored.
/// @param expand Expands the variables of the text.
/// @return The path with surrounding quotes dropped and its variables expanded; empty for an empty @p raw.
[[nodiscard]] std::filesystem::path programOfTaskAction(std::string_view raw, VariableExpander const& expand);

/// Brings the arguments of a registered daemon command to how its backend launches it.
///
/// A registration made by an earlier release may lack what a later one needs of the launch style: the Task
/// Scheduler only starts a launcher, so its daemon must detach itself (`--background`), which a 0.8
/// registration does not say. Left as it was, a repaired task would run its daemon in the foreground, from
/// the folder the repair points it at, and hold that folder at every logon.
/// @param arguments The registered arguments, as a command line.
/// @param style How the backend launches the command.
/// @return @p arguments with what the style needs added; unchanged where it needs nothing or has it.
[[nodiscard]] std::string argumentsForLaunchStyle(std::string_view arguments, DaemonLaunchStyle style);

/// Every name a daemon serving @p label may be registered under.
/// @param label The socket label.
/// @return The current name first, then the pre-phase-2 `contour-daemon-<label>` where it differs, so a
///         registration made by an older release can still be found, stopped and uninstalled.
[[nodiscard]] std::vector<std::string> registrationNamesForLabel(std::string_view label);

} // namespace vthost
