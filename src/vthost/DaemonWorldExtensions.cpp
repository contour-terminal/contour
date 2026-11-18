// SPDX-License-Identifier: Apache-2.0
#include <vthost/DaemonWorldExtensions.hpp>

#include <core/log/LogStore.hpp>

#include <algorithm>
#include <array>
#include <cstdint>
#include <format>
#include <string_view>
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
        PhaseRow { .phase = vtextension::DaemonExtensionPhase::Failed,
                   .state = DaemonExtensionState::Failed },
    };

    /// @return @p phase as vthost names it.
    [[nodiscard]] DaemonExtensionState stateOf(vtextension::DaemonExtensionPhase phase) noexcept
    {
        auto const row = std::ranges::find(PhaseRows, phase, &PhaseRow::phase);
        return row != PhaseRows.end() ? row->state : DaemonExtensionState::Stopped;
    }

    /// What the status line shows of an extension, by where it stands.
    enum class StatusShows : uint8_t
    {
        Nothing = 0, ///< Nothing: it does not run, and says nothing of itself.
        Summary,     ///< The summary it last published.
        Notice,      ///< A notice naming it, StatusText::notice.
    };

    /// One row of what the status line shows for a phase.
    struct StatusText
    {
        vtextension::DaemonExtensionPhase phase;
        StatusShows shows;
        std::string_view notice; ///< A format of the extension's name, for StatusShows::Notice.
    };

    inline constexpr auto StatusTexts = std::array {
        StatusText { .phase = vtextension::DaemonExtensionPhase::Stopped,
                     .shows = StatusShows::Nothing,
                     .notice = {} },
        StatusText { .phase = vtextension::DaemonExtensionPhase::Pending,
                     .shows = StatusShows::Nothing,
                     .notice = {} },
        StatusText {
            .phase = vtextension::DaemonExtensionPhase::Denied, .shows = StatusShows::Nothing, .notice = {} },
        StatusText { .phase = vtextension::DaemonExtensionPhase::Running,
                     .shows = StatusShows::Summary,
                     .notice = {} },
        StatusText { .phase = vtextension::DaemonExtensionPhase::Disabled,
                     .shows = StatusShows::Notice,
                     .notice = "{} disabled" },
        StatusText { .phase = vtextension::DaemonExtensionPhase::Failed,
                     .shows = StatusShows::Notice,
                     .notice = "{} failed" },
    };

    /// One hint vthost raises, as vtextension names it.
    struct HintRow
    {
        ServedHint served;
        vtextension::DaemonHint hint;
    };

    inline constexpr auto HintRows = std::array {
        HintRow { .served = ServedHint::Tabs, .hint = vtextension::DaemonHint::Tabs },
        HintRow { .served = ServedHint::Actions, .hint = vtextension::DaemonHint::Actions },
    };

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
    if (pendingRequests() == before)
        return;
    if (_events.requestsChanged)
        _events.requestsChanged();
    // Waiting for a grant, or no longer, is a state the palette shows.
    if (_events.presenceChanged)
        _events.presenceChanged();
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
        auto listing = DaemonExtensionListing { .name = entry.name,
                                                .state = stateOf(entry.phase),
                                                .grantDigest = entry.grantDigest,
                                                .pending = {},
                                                .problem = entry.problem };
        if (auto const asked = std::ranges::find(pending, entry.name, &DaemonExtensionRequest::name);
            asked != pending.end())
            listing.pending = *asked;
        listings.push_back(std::move(listing));
    }
    return listings;
}

void DaemonWorldExtensions::answer(std::string_view name, std::string_view grantDigest, GrantState answer)
{
    // The host tells the windows the question is settled; where the extension then stands, it reports.
    _runtime->answer(name, grantDigest, grantOf(answer));
}

void DaemonWorldExtensions::revoke(std::string_view name)
{
    // The runtime starts a new run, which drops the old one's summary and whatever it still publishes; once
    // it returned, the summary is forgotten here too, whatever reached us meanwhile.
    changingRequests([&] { _runtime->revoke(name); });
    auto const lock = std::scoped_lock { _mutex };
    if (auto const found = _shown.find(name); found != _shown.end())
        found->second.summary.reset();
}

std::optional<DaemonExtensionStatus> DaemonWorldExtensions::status(std::string_view name) const
{
    auto const listings = list();
    auto const listed = std::ranges::find(listings, name, &DaemonExtensionListing::name);
    if (listed == listings.end())
        return std::nullopt;
    auto status =
        DaemonExtensionStatus { .state = listed->state, .summary = {}, .detail = {}, .problem = {} };
    if (status.state == DaemonExtensionState::Failed)
        status.problem = listed->problem;
    if (auto published = summary(name))
    {
        status.summary = std::move(published->shortText);
        status.detail = std::move(published->detail);
    }
    return status;
}

vtbackend::IDaemonExtensionSummaries const& DaemonWorldExtensions::summaries() const noexcept
{
    return *this;
}

std::string DaemonWorldExtensions::summaryOf(std::string_view name) const
{
    auto const lock = std::scoped_lock { _mutex };
    auto const found = _shown.find(name);
    if (found == _shown.end())
        return {};
    auto const& shown = found->second;
    auto const row = std::ranges::find(StatusTexts, shown.phase, &StatusText::phase);
    if (row == StatusTexts.end())
        return {};
    switch (row->shows)
    {
        case StatusShows::Nothing: return {};
        case StatusShows::Summary: return shown.summary ? shown.summary->shortText : std::string {};
        case StatusShows::Notice: return std::vformat(row->notice, std::make_format_args(name));
    }
    return {};
}

std::vector<ExtensionActionRows> DaemonWorldExtensions::actionRows() const
{
    auto all = std::vector<ExtensionActionRows> {};
    for (auto const& entry: _runtime->entries())
    {
        auto rows = ExtensionActionRows {
            .extension = entry.name, .title = entry.title, .state = stateOf(entry.phase), .rows = {}
        };
        for (auto const& action: entry.actions)
            rows.rows.emplace_back(action.id, action.title);
        all.push_back(std::move(rows));
    }
    return all;
}

void DaemonWorldExtensions::hint(std::string_view name, ServedHint hint)
{
    auto const row = std::ranges::find(HintRows, hint, &HintRow::served);
    if (row != HintRows.end())
        _runtime->hint(name, row->hint);
}

void DaemonWorldExtensions::restart(std::string_view name)
{
    _runtime->restart(name);
}

void DaemonWorldExtensions::stop() noexcept
{
    _runtime->stop();
}

std::optional<DaemonExtensionSummary> DaemonWorldExtensions::summary(std::string_view name) const
{
    auto const lock = std::scoped_lock { _mutex };
    auto const found = _shown.find(name);
    if (found == _shown.end())
        return std::nullopt;
    return found->second.summary;
}

DaemonWorldExtensions::Shown* DaemonWorldExtensions::shownAs(std::string_view name,
                                                             vtextension::DaemonRunGeneration generation)
{
    auto found = _shown.find(name);
    if (found == _shown.end())
        found = _shown
                    .emplace(std::string(name),
                             Shown { .generation = generation,
                                     .phase = vtextension::DaemonExtensionPhase::Stopped,
                                     .summary = {} })
                    .first;
    auto& shown = found->second;
    if (generation < shown.generation)
        return nullptr;
    if (generation > shown.generation)
        shown = Shown { .generation = generation,
                        .phase = vtextension::DaemonExtensionPhase::Stopped,
                        .summary = {} };
    return &shown;
}

void DaemonWorldExtensions::publishSummary(std::string_view extension,
                                           vtextension::DaemonRunGeneration generation,
                                           std::string_view shortText,
                                           std::string_view detail)
{
    {
        auto const lock = std::scoped_lock { _mutex };
        auto* const shown = shownAs(extension, generation);
        if (shown == nullptr)
            return; // A run revoked since: what it says is no longer shown.
        shown->summary =
            DaemonExtensionSummary { .shortText = std::string(shortText), .detail = std::string(detail) };
    }
    if (_events.summaryChanged)
        _events.summaryChanged();
}

void DaemonWorldExtensions::phaseChanged(std::string_view extension,
                                         vtextension::DaemonRunGeneration generation,
                                         vtextension::DaemonExtensionPhase phase)
{
    {
        auto const lock = std::scoped_lock { _mutex };
        auto* const shown = shownAs(extension, generation);
        if (shown == nullptr)
            return;
        shown->phase = phase;
    }
    // Every phase change repaints: what the status line and the palette show follows from it.
    if (_events.summaryChanged)
        _events.summaryChanged();
    if (_events.presenceChanged)
        _events.presenceChanged();
}

} // namespace vthost
