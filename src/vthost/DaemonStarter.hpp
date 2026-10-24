// SPDX-License-Identifier: Apache-2.0
#pragma once

/// @file
/// Starting the daemon: through the service that is registered for it when there is one, by spawning it on
/// demand otherwise; and what `daemon-service status` makes of the registration and the live socket.

#include <vtpty/ProcessPlacement.hpp>

#include <chrono>
#include <expected>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>

#include <vthost/Daemon.hpp>
#include <vthost/DaemonBinary.hpp>
#include <vthost/ServiceControl.hpp>

namespace vthost
{

/// What a registration reports once the socket has been asked: the live probe is the truth. A Task
/// Scheduler task that ran `--background` reads "Ready" while its daemon serves.
/// @param registered What the service manager reports.
/// @param live Whether a daemon accepts connections.
/// @return NotInstalled stays NotInstalled; otherwise Serving is Running and Absent is Stopped.
[[nodiscard]] constexpr ServiceRunState effectiveRunState(ServiceRunState registered,
                                                          DaemonPresence live) noexcept
{
    if (registered == ServiceRunState::NotInstalled)
        return ServiceRunState::NotInstalled;
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
};

/// @param label The socket label.
/// @param makeBackend Builds the backend for one mode and name.
/// @return The first registration, over every name of registrationNamesForLabel() and every start mode,
///         whose status can be had and is not NotInstalled; nullopt when none is.
[[nodiscard]] std::optional<RegisteredService> findRegisteredService(
    std::string_view label, ServiceBackendFactory const& makeBackend);

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

/// @param registered The registered service, if any.
/// @param onDemand What spawning on demand needs, should there be none.
/// @return A registration wins; spawning on demand is the fallback for when no service is installed.
[[nodiscard]] std::unique_ptr<DaemonStarter> chooseDaemonStarter(std::optional<RegisteredService> registered,
                                                                 OnDemandSpawn onDemand);

} // namespace vthost
