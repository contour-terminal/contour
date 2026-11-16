// SPDX-License-Identifier: Apache-2.0
#include <vthost/DaemonWorldExtensions.hpp>

#include <core/log/LogStore.hpp>

#include <algorithm>
#include <array>
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

    /// @return @p consent as vthost carries it.
    [[nodiscard]] DaemonExtensionRequest requestOf(vtextension::PendingConsent const& consent)
    {
        auto request = DaemonExtensionRequest { .name = consent.name,
                                                .grantDigest = consent.grantDigest,
                                                .title = consent.question.title,
                                                .lines = {} };
        for (auto const& line: consent.question.lines)
            request.lines.emplace_back(line.text, static_cast<uint8_t>(line.weight));
        return request;
    }

    /// One phase of vtextension's, as vthost names it.
    struct PhaseRow
    {
        vtextension::DaemonExtensionPhase phase;
        DaemonExtensionState state;
    };

    inline constexpr auto PhaseRows = std::array {
        PhaseRow { .phase = vtextension::DaemonExtensionPhase::Stopped,
                   .state = DaemonExtensionState::Stopped },
        PhaseRow { .phase = vtextension::DaemonExtensionPhase::Pending,
                   .state = DaemonExtensionState::Pending },
        PhaseRow { .phase = vtextension::DaemonExtensionPhase::Denied,
                   .state = DaemonExtensionState::Denied },
        PhaseRow { .phase = vtextension::DaemonExtensionPhase::Running,
                   .state = DaemonExtensionState::Running },
        PhaseRow { .phase = vtextension::DaemonExtensionPhase::Disabled,
                   .state = DaemonExtensionState::Disabled },
    };

    /// @return @p phase as vthost names it.
    [[nodiscard]] DaemonExtensionState stateOf(vtextension::DaemonExtensionPhase phase) noexcept
    {
        auto const row = std::ranges::find(PhaseRows, phase, &PhaseRow::phase);
        return row != PhaseRows.end() ? row->state : DaemonExtensionState::Stopped;
    }

    /// @return vthost's answer as vtextension's.
    [[nodiscard]] vtextension::GrantState grantOf(GrantState answer) noexcept
    {
        switch (answer)
        {
            case GrantState::Granted: return vtextension::GrantState::Granted;
            case GrantState::Denied: return vtextension::GrantState::Denied;
            case GrantState::Pending: break;
        }
        return vtextension::GrantState::Pending;
    }
} // namespace

DaemonWorldExtensions::DaemonWorldExtensions(vtextension::DaemonExtensionRuntimeConfig config,
                                             DaemonWorldServices const& services,
                                             DaemonWorldEvents events):
    _events { std::move(events) }, _runtime { compose(std::move(config), services, *this) }
{
}

template <typename Change>
void DaemonWorldExtensions::changingRequests(Change change)
{
    auto const before = pendingRequests();
    change();
    if (pendingRequests() != before && _events.requestsChanged)
        _events.requestsChanged();
}

void DaemonWorldExtensions::startOnDemand(std::string_view name)
{
    changingRequests([&] { _runtime->start(name); });
}

std::vector<DaemonExtensionRequest> DaemonWorldExtensions::pendingRequests() const
{
    auto requests = std::vector<DaemonExtensionRequest> {};
    for (auto const& consent: _runtime->pending())
        requests.push_back(requestOf(consent));
    return requests;
}

std::vector<DaemonExtensionListing> DaemonWorldExtensions::list() const
{
    auto const pending = pendingRequests();
    auto listings = std::vector<DaemonExtensionListing> {};
    for (auto const& entry: _runtime->entries())
    {
        auto listing = DaemonExtensionListing {
            .name = entry.name, .state = stateOf(entry.phase), .grantDigest = entry.grantDigest, .pending = {}
        };
        if (auto const asked = std::ranges::find(pending, entry.name, &DaemonExtensionRequest::name);
            asked != pending.end())
            listing.pending = *asked;
        listings.push_back(std::move(listing));
    }
    return listings;
}

void DaemonWorldExtensions::answer(std::string_view name, std::string_view grantDigest, GrantState answer)
{
    _runtime->answer(name, grantDigest, grantOf(answer));
}

void DaemonWorldExtensions::revoke(std::string_view name)
{
    changingRequests([&] { _runtime->revoke(name); });
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
