// SPDX-License-Identifier: Apache-2.0
#include <vthost/DaemonStarter.hpp>

#include <core/Utf8Path.hpp>
#include <core/log/LogStore.hpp>

#include <cstdlib>
#include <format>
#include <ranges>
#include <utility>

#include <vthost/Logging.hpp>

namespace vthost
{

ServiceBackendFactory nativeServiceBackendFactory(ServiceBackendServices const& services)
{
    return [&services](ServiceStartMode mode, std::string_view name) {
        return makeServiceBackend(mode, name, services);
    };
}

std::optional<RegisteredService> findRegisteredService(std::string_view label,
                                                       ServiceBackendFactory const& makeBackend)
{
    // A backend whose status cannot be had (a Boot/Manual one on a POSIX host answers with an error) is
    // not a registration.
    for (auto const& name: registrationNamesForLabel(label))
        for (auto const& mode: ServiceStartModeNames | std::views::values)
            if (auto backend = makeBackend(mode, name))
                if (auto found = backend->status(); found && found->state != ServiceRunState::NotInstalled)
                    return RegisteredService { .backend = std::move(backend),
                                               .name = name,
                                               .status = *std::move(found) };
    return std::nullopt;
}

std::expected<RegistrationRepair, ServiceError> repairRegistration(RegisteredService& service,
                                                                   RegistrationRepairInputs const& inputs)
{
    auto const& program = service.status.program;
    if (program.empty() || inputs.installedExecutable.empty() || inputs.fileSystem.exists(program)
        || !inputs.fileSystem.exists(inputs.installedExecutable))
        return RegistrationRepair::Unchanged;
    if (auto const retargeted = service.backend->retarget(inputs.installedExecutable); !retargeted)
        return std::unexpected(retargeted.error());
    service.status.program = inputs.installedExecutable;
    return RegistrationRepair::Retargeted;
}

std::optional<RegisteredService> findRepairedRegisteredService(std::string_view label,
                                                               ServiceBackendFactory const& makeBackend,
                                                               RegistrationRepairInputs const& inputs,
                                                               RegistrationUse use)
{
    auto found = findRegisteredService(label, makeBackend);
    if (!found)
        return found;
    auto repairFailure = std::string {};
    if (auto const repaired = repairRegistration(*found, inputs); !repaired)
        repairFailure = std::format(" (it could not be pointed at {}: {})",
                                    core::pathToUtf8(inputs.installedExecutable),
                                    repaired.error().toString());
    auto const& program = found->status.program;
    if (use == RegistrationUse::Start && !program.empty() && !inputs.fileSystem.exists(program))
    {
        errorLog()("the daemon task {} starts {}, which does not exist{}; starting the daemon on demand",
                   found->name,
                   core::pathToUtf8(program),
                   repairFailure);
        return std::nullopt;
    }
    if (!repairFailure.empty())
        errorLog()("the daemon task {} starts {}, which does not exist{}",
                   found->name,
                   core::pathToUtf8(program),
                   repairFailure);
    return found;
}

ServiceDaemonStarter::ServiceDaemonStarter(RegisteredService service,
                                           DaemonSpawner& spawner,
                                           std::filesystem::path socketPath,
                                           std::chrono::milliseconds timeout):
    _service { std::move(service) },
    _spawner { spawner },
    _socketPath { std::move(socketPath) },
    _timeout { timeout }
{
}

std::expected<void, std::string> ServiceDaemonStarter::start()
{
    if (auto const started = _service.backend->start(); !started)
        return std::unexpected(
            std::format("cannot start service {}: {}", _service.name, started.error().toString()));
    if (awaitDaemon(_spawner, _socketPath, _timeout) != DaemonPresence::Serving)
        return std::unexpected(std::format("service {} was started, but no daemon serves on {}",
                                           _service.name,
                                           core::pathToUtf8(_socketPath)));
    return {};
}

OnDemandDaemonStarter::OnDemandDaemonStarter(OnDemandSpawn spawn): _spawn { std::move(spawn) }
{
}

std::expected<void, std::string> OnDemandDaemonStarter::start()
{
    auto const endpoint = AttachEndpoint { _spawn.endpoint };
    if (ensureDaemon(
            endpoint, _spawn.binary, _spawn.spawner, _spawn.placement, _spawn.options, _spawn.timeout)
        != EXIT_SUCCESS)
        return std::unexpected(std::format("daemon did not start on {} within {} seconds",
                                           core::pathToUtf8(_spawn.endpoint.socketPath),
                                           _spawn.timeout.count()));
    return {};
}

bool isSameSocketPath(std::filesystem::path const& lhs, std::filesystem::path const& rhs)
{
    auto const normalize = [](std::filesystem::path const& path) {
        auto ec = std::error_code {};
        auto result = std::filesystem::weakly_canonical(path, ec);
        if (ec)
            result = path;
        result = result.lexically_normal();
        // "dir/" normalizes to "dir/." -- drop the empty filename a trailing separator leaves.
        if (!result.has_filename())
            result = result.parent_path();
        return result;
    };
    return normalize(lhs) == normalize(rhs);
}

std::unique_ptr<DaemonStarter> chooseDaemonStarter(std::optional<RegisteredService> registered,
                                                   std::filesystem::path const& labelDefaultSocket,
                                                   OnDemandSpawn onDemand)
{
    if (registered)
    {
        // A registration the user disabled cannot start anything: spawn on demand rather than run it.
        if (registered->status.state == ServiceRunState::Disabled)
            return std::make_unique<OnDemandDaemonStarter>(std::move(onDemand));
        auto const served = registeredSocketPath(registered->status.commandLine).value_or(labelDefaultSocket);
        if (isSameSocketPath(served, onDemand.endpoint.socketPath))
            return std::make_unique<ServiceDaemonStarter>(
                std::move(*registered),
                onDemand.spawner,
                onDemand.endpoint.socketPath,
                std::chrono::duration_cast<std::chrono::milliseconds>(onDemand.timeout));
    }
    return std::make_unique<OnDemandDaemonStarter>(std::move(onDemand));
}

} // namespace vthost
