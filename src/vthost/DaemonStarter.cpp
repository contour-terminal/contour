// SPDX-License-Identifier: Apache-2.0
#include <vthost/DaemonStarter.hpp>

#include <core/Utf8Path.hpp>

#include <cstdlib>
#include <format>
#include <ranges>
#include <utility>

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
                if (auto const found = backend->status();
                    found && found->state != ServiceRunState::NotInstalled)
                    return RegisteredService { .backend = std::move(backend), .name = name };
    return std::nullopt;
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
        auto const status = registered->backend->status();
        auto const served =
            (status ? registeredSocketPath(status->commandLine) : std::nullopt).value_or(labelDefaultSocket);
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
