// SPDX-License-Identifier: Apache-2.0
#include <vthost/DaemonWorldExtensions.hpp>

#include <core/log/LogStore.hpp>

#include <utility>

#include <vthost/DaemonExtensions.hpp>

namespace vthost
{

namespace
{
    /// @return The daemon world @p config declares, its problems logged.
    [[nodiscard]] std::unique_ptr<vtextension::DaemonExtensionRuntime> compose(
        vtextension::DaemonExtensionRuntimeConfig config,
        DaemonWorldServices const& services,
        vtextension::IExtensionPresence& presence)
    {
        auto composed = vtextension::DaemonExtensionRuntime::create(
            std::move(config),
            vtextension::DaemonExtensionRuntimeServices { .backend = services.backend,
                                                          .runner = services.runner,
                                                          .directories = services.directories,
                                                          .fileSystem = services.fileSystem,
                                                          .locker = services.locker,
                                                          .wallClock = services.wallClock,
                                                          .localTime = services.localTime,
                                                          .presence = presence,
                                                          .onLog = &logExtensionMessage });
        for (auto const& problem: composed.problems)
            errorLog()("{}", problem.message);
        return std::move(composed.runtime);
    }
} // namespace

DaemonWorldExtensions::DaemonWorldExtensions(vtextension::DaemonExtensionRuntimeConfig config,
                                             DaemonWorldServices const& services,
                                             DaemonWorldEvents events):
    _events { std::move(events) }, _runtime { compose(std::move(config), services, *this) }
{
}

void DaemonWorldExtensions::startOnDemand(std::string_view name)
{
    _runtime->start(name);
}

void DaemonWorldExtensions::stop() noexcept
{
    _runtime->stop();
}

std::optional<DaemonExtensionSummary> DaemonWorldExtensions::summary(std::string_view name) const
{
    auto const lock = std::scoped_lock { _mutex };
    auto const found = _summaries.find(name);
    if (found == _summaries.end())
        return std::nullopt;
    return found->second;
}

void DaemonWorldExtensions::publishSummary(std::string_view extension,
                                           std::string_view shortText,
                                           std::string_view detail)
{
    {
        auto const lock = std::scoped_lock { _mutex };
        _summaries.insert_or_assign(
            std::string(extension),
            DaemonExtensionSummary { .shortText = std::string(shortText), .detail = std::string(detail) });
    }
    if (_events.summaryChanged)
        _events.summaryChanged();
}

} // namespace vthost
