// SPDX-License-Identifier: Apache-2.0
#pragma once

/// @file
/// Starting the daemon: through the service that is registered for it when there is one, by spawning it on
/// demand otherwise; and what `daemon-service status` makes of the registration and the live socket.

#include <vtpty/ProcessPlacement.hpp>

#include <core/platform/FileSystem.hpp>

#include <chrono>
#include <expected>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>

#include <vthost/DaemonBinary.hpp>
#include <vthost/DaemonSpawn.hpp>
#include <vthost/InstalledExecutable.hpp>
#include <vthost/ServiceControl.hpp>

namespace vthost
{

/// What a registration reports once the socket has been asked: the live probe is the truth. A Task
/// Scheduler task that ran `--background` reads "Ready" while its daemon serves.
/// @param registered What the service manager reports.
/// @param live Whether a daemon accepts connections.
/// @return NotInstalled and Disabled stay as they are; otherwise Serving is Running and Absent is Stopped.
[[nodiscard]] constexpr ServiceRunState effectiveRunState(ServiceRunState registered,
                                                          DaemonPresence live) noexcept
{
    if (registered == ServiceRunState::NotInstalled || registered == ServiceRunState::Disabled)
        return registered;
    return live == DaemonPresence::Serving ? ServiceRunState::Running : ServiceRunState::Stopped;
}

/// Who stops a registered daemon.
enum class StopRoute : uint8_t
{
    ServiceManager = 0, ///< The OS supervises the daemon's process, so stopping the service stops it.
    DaemonControl,      ///< The OS only ran a launcher; the daemon is asked to stop over its socket.
};

/// @param style How the registered command relates to the daemon process.
/// @return Who can stop that daemon: a Foreground one is the service manager's process, but a
///         SelfDetaching one outlives the task that started it.
[[nodiscard]] constexpr StopRoute stopVia(DaemonLaunchStyle style) noexcept
{
    return style == DaemonLaunchStyle::SelfDetaching ? StopRoute::DaemonControl : StopRoute::ServiceManager;
}

/// How the backend for a start mode and registration name is built; production wraps makeServiceBackend().
using ServiceBackendFactory =
    std::function<std::unique_ptr<ServiceBackend>(ServiceStartMode mode, std::string_view name)>;

/// @param services What the backends reach the host through; must outlive the factory and its backends.
/// @return The factory that builds the platform's backends.
[[nodiscard]] ServiceBackendFactory nativeServiceBackendFactory(ServiceBackendServices const& services);

/// A registered daemon service, and the name it is registered under.
struct RegisteredService
{
    std::unique_ptr<ServiceBackend> backend; ///< The mechanism that holds the registration.
    std::string name;                        ///< The registration name (an older release's, maybe).
    /// What the registration reported when it was found: one round trip to the service manager, which what is
    /// decided from it (repairing, choosing a starter) reuses. Kept current by repairRegistration().
    ServiceStatus status {};
};

/// @param label The socket label.
/// @param makeBackend Builds the backend for one mode and name.
/// @return The first registration, over every name of registrationNamesForLabel() and every start mode,
///         whose status can be had and is not NotInstalled; nullopt when none is.
[[nodiscard]] std::optional<RegisteredService> findRegisteredService(
    std::string_view label, ServiceBackendFactory const& makeBackend);

/// What repairRegistration() did.
enum class RegistrationRepair : uint8_t
{
    Unchanged = 0, ///< Nothing needed doing.
    Retargeted,    ///< The registration was pointed at the installed executable.
};

/// What repairing a registration needs to know of the host.
struct RegistrationRepairInputs
{
    core::platform::FileSystem const& fileSystem; ///< Where the programs are looked for.
    /// The executable an installation provides now (installedExecutableOf()); empty for none, which repairs
    /// nothing.
    std::filesystem::path installedExecutable;
};

/// Points a registration whose program an upgrade removed at the installed executable, so the Task Scheduler
/// does not start nothing at the next logon.
/// @param service The registration, as found; its status is updated when it is retargeted.
/// @param inputs What exists on the host.
/// @return Retargeted iff the registration names a program that does not exist and the installed executable
///         does; Unchanged otherwise (also when it names no program, as systemd's and launchd's do not); the
///         backend's error when it refuses the change.
[[nodiscard]] std::expected<RegistrationRepair, ServiceError> repairRegistration(
    RegisteredService& service, RegistrationRepairInputs const& inputs);

/// What a registration is looked for to do.
enum class RegistrationUse : uint8_t
{
    Manage = 0, ///< The `daemon-service` verbs: any registration, a stale or disabled one included, for it to
                ///< be removed.
    Start,      ///< A client that wants a daemon serving: a registration that cannot start one is none.
};

/// @param label The socket label.
/// @param makeBackend Builds the backend for one mode and name.
/// @param inputs What repairRegistration() needs.
/// @param use What the registration is for.
/// @return findRegisteredService(), the registration repaired by repairRegistration(); a repair that fails is
///         logged. For Start, a registration whose program is still missing after that is none (nullopt), so
///         that the daemon is spawned on demand; logged once, with the repair's error where there was one.
[[nodiscard]] std::optional<RegisteredService> findRepairedRegisteredService(
    std::string_view label,
    ServiceBackendFactory const& makeBackend,
    RegistrationRepairInputs const& inputs,
    RegistrationUse use);

/// Something that gets a daemon serving.
class DaemonStarter
{
  public:
    virtual ~DaemonStarter() = default;

    /// @return Nothing once a daemon serves, or why none does, in words for the user.
    [[nodiscard]] virtual std::expected<void, std::string> start() = 0;
};

/// Starts the registered service, then waits for the daemon to accept connections.
class ServiceDaemonStarter final: public DaemonStarter
{
  public:
    /// @param service The registration to start.
    /// @param spawner Asks whether the daemon serves (not owned; outlives this).
    /// @param socketPath The control socket the daemon binds.
    /// @param timeout How long to wait for it to serve.
    ServiceDaemonStarter(RegisteredService service,
                         DaemonSpawner& spawner,
                         std::filesystem::path socketPath,
                         std::chrono::milliseconds timeout);

    [[nodiscard]] std::expected<void, std::string> start() override;

  private:
    RegisteredService _service;
    DaemonSpawner& _spawner;
    std::filesystem::path _socketPath;
    std::chrono::milliseconds _timeout;
};

/// What spawning the daemon on demand needs; the references outlive the starter.
struct OnDemandSpawn
{
    UnixEndpoint endpoint;              ///< Where the daemon listens.
    DaemonSpawner& spawner;             ///< Starts the process and asks whether it serves.
    DaemonBinarySource& binary;         ///< What the daemon is started from.
    vtpty::ProcessPlacement& placement; ///< Places the spawned daemon. POSIX only.
    DaemonSpawnOptions options;         ///< Forwarded to the daemon's command line.
    std::chrono::seconds timeout;       ///< How long to wait for it to serve.
};

/// Spawns the daemon on demand (ensureDaemon()); the fallback when no service is registered.
class OnDemandDaemonStarter final: public DaemonStarter
{
  public:
    /// @param spawn What the spawn needs.
    explicit OnDemandDaemonStarter(OnDemandSpawn spawn);

    [[nodiscard]] std::expected<void, std::string> start() override;

  private:
    OnDemandSpawn _spawn;
};

/// @param lhs One socket path.
/// @param rhs Another.
/// @return Whether both name the same socket file: compared after weak canonicalization (symlinks, `..`,
///         a trailing separator), falling back to lexical normalization where the file system cannot say.
[[nodiscard]] bool isSameSocketPath(std::filesystem::path const& lhs, std::filesystem::path const& rhs);

/// @param registered The registered service, if any.
/// @param labelDefaultSocket The socket the label serves by default (muxSocketPath with no explicit path and
///        no `$CONTOUR_MUX`); what a registration serves when its command line pins no `--socket`.
/// @param onDemand What spawning on demand needs, should there be none.
/// @return A registration wins only when it serves the endpoint asked for: the socket its command line
///         names, else @p labelDefaultSocket. Any other endpoint (`$CONTOUR_MUX`, `--socket`) and the
///         absence of a registration both spawn on demand, since starting a service that serves another
///         socket would never produce a daemon at the requested one.
[[nodiscard]] std::unique_ptr<DaemonStarter> chooseDaemonStarter(
    std::optional<RegisteredService> registered,
    std::filesystem::path const& labelDefaultSocket,
    OnDemandSpawn onDemand);

} // namespace vthost
