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
/// there runs in the service's window station, where any GUI program it launches is invisible).

#include <array>
#include <cstdint>
#include <expected>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

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
};

/// How each run state reads in `daemon-service status`. One table rather than a chain of
/// conditionals, so a fourth state is a row here and not an edit at every report site.
inline constexpr auto ServiceRunStateNames = std::array {
    std::pair { std::string_view { "not installed" }, ServiceRunState::NotInstalled },
    std::pair { std::string_view { "installed, stopped" }, ServiceRunState::Stopped },
    std::pair { std::string_view { "running" }, ServiceRunState::Running },
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
};

/// Builds the backend that hosts @p mode for the registration named @p name.
///
/// @param mode Which mechanism to address.
/// @param name The registration name (a task name, or an SCM service name).
/// @return The backend; on a platform with no implementation, one whose every operation
///         reports @c ServiceErrorCode::Unsupported rather than a null pointer, so callers
///         have one error path instead of two.
[[nodiscard]] std::unique_ptr<ServiceBackend> makeServiceBackend(ServiceStartMode mode,
                                                                 std::string_view name);

/// The registration name for a daemon serving socket label @p label.
///
/// Labels already distinguish daemon instances, so they distinguish their registrations too;
/// without that, installing a second labelled daemon would silently overwrite the first.
/// @param label The socket label ("default" for the unnamed one).
/// @return The registration name.
[[nodiscard]] std::string serviceNameForLabel(std::string_view label);

} // namespace vthost
