// SPDX-License-Identifier: Apache-2.0
#include <vthost/DaemonWorldExtensions.hpp>

#include <core/log/LogStore.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <format>
#include <optional>
#include <ranges>
#include <span>
#include <string_view>
#include <utility>

#include <vtextension/ExtensionConsent.hpp>
#include <vtextension/ExtensionError.hpp>
#include <vtextension/PaneService.hpp>
#include <vthost/DaemonExtensions.hpp>

namespace vthost
{

namespace
{
    /// One reason the host refused a served-tab call, as the guest is told it.
    struct ServedTabErrorRow
    {
        ServedTabError error;
        vtextension::HostError hostError;
    };

    /// One row per ServedTabError, indexed by value.
    inline constexpr auto ServedTabErrorRows = std::array {
        ServedTabErrorRow { .error = ServedTabError::NotOwned,
                            .hostError = vtextension::HostError::NotOwned },
        ServedTabErrorRow { .error = ServedTabError::Gone, .hostError = vtextension::HostError::Gone },
        ServedTabErrorRow { .error = ServedTabError::Limit, .hostError = vtextension::HostError::Limit },
        ServedTabErrorRow { .error = ServedTabError::BadArgument,
                            .hostError = vtextension::HostError::BadArgument },
    };

    static_assert(ServedTabErrorRows.size() == std::to_underlying(ServedTabError::BadArgument) + 1U,
                  "every ServedTabError has a row");
    static_assert(std::ranges::all_of(std::views::iota(std::size_t { 0 }, ServedTabErrorRows.size()),
                                      [](std::size_t index) {
                                          return std::to_underlying(ServedTabErrorRows.at(index).error)
                                                 == index;
                                      }),
                  "ServedTabErrorRows is indexed by the enumerator's value");

    /// @return @p error as the guest is told it.
    [[nodiscard]] constexpr vtextension::HostError hostErrorOf(ServedTabError error) noexcept
    {
        return ServedTabErrorRows.at(std::to_underlying(error)).hostError;
    }

    /// One extension's tab and action imports, on the host's served tabs and invoked actions.
    class ServedTabAdapter final: public vtextension::IServedTabService
    {
      public:
        ServedTabAdapter(vtextension::ServedTabOwner const& owner, DaemonWorldWiring wiring):
            _extension { owner.extension },
            _title { owner.title },
            _scheme { owner.scheme.transform([](std::string_view scheme) { return std::string(scheme); }) },
            _moduleDigest { owner.moduleDigest },
            _wiring { wiring }
        {
        }

        [[nodiscard]] std::expected<std::string, vtextension::HostError> tabOpen(
            std::string_view scheme, vtextension::Origin origin) override
        {
            // Only the scheme the manifest declares, which the user granted it to serve.
            if (!_scheme || *_scheme != scheme)
                return std::unexpected(vtextension::HostError::BadArgument);
            return _wiring.tabs.open(_extension, _title, _moduleDigest, scheme, origin)
                .transform_error(hostErrorOf);
        }

        [[nodiscard]] std::expected<void, vtextension::HostError> tabClose(std::string_view tab) override
        {
            return _wiring.tabs.close(_extension, tab).transform_error(hostErrorOf);
        }

        [[nodiscard]] std::expected<void, vtextension::HostError> tabReveal(
            std::string_view tab, vtextension::Origin origin) override
        {
            return _wiring.tabs.reveal(_extension, tab, origin).transform_error(hostErrorOf);
        }

        [[nodiscard]] std::expected<std::vector<vtextension::TabInfo>, vtextension::HostError> tabs() override
        {
            return std::ranges::to<std::vector<vtextension::TabInfo>>(
                _wiring.tabs.list(_extension) | std::views::transform([](ServedTabInfo const& info) {
                    return vtextension::TabInfo {
                        .tab = info.tab, .scheme = info.scheme, .cols = info.cols, .rows = info.rows
                    };
                }));
        }

        [[nodiscard]] std::expected<vtextension::ByteList, vtextension::HostError> tabRead(
            std::string_view tab) override
        {
            return _wiring.tabs.read(_extension, tab)
                .transform([](std::string const& bytes) {
                    return vtextension::ByteList(bytes.begin(), bytes.end());
                })
                .transform_error(hostErrorOf);
        }

        [[nodiscard]] std::expected<void, vtextension::HostError> tabWrite(
            std::string_view tab, std::span<uint8_t const> bytes) override
        {
            auto const text = std::string(bytes.begin(), bytes.end());
            return _wiring.tabs.write(_extension, tab, text).transform_error(hostErrorOf);
        }

        [[nodiscard]] std::expected<std::vector<vtextension::ActionInfo>, vtextension::HostError>
        actionsTake() override
        {
            return std::ranges::to<std::vector<vtextension::ActionInfo>>(
                _wiring.actions.take(_extension) | std::views::transform([](auto const& action) {
                    return vtextension::ActionInfo { .id = action.first, .origin = action.second };
                }));
        }

      private:
        std::string _extension;
        std::string _title;
        std::optional<std::string> _scheme;
        std::string _moduleDigest;
        DaemonWorldWiring _wiring;
    };

    /// One row per OwnedPaneError, indexed by value: the policy's word for it.
    inline constexpr auto OwnedPaneErrorRows = std::array {
        vtextension::PaneError::NotOwned,        // NotOwned
        vtextension::PaneError::TooMany,         // TooMany
        vtextension::PaneError::HostRefused,     // Upgrading
        vtextension::PaneError::NotReconciled,   // NotReconciled
        vtextension::PaneError::NotSupportedYet, // NotSupported (held directories, here)
        vtextension::PaneError::CwdMissing,      // DirectoryGone
        vtextension::PaneError::ProgramMissing,  // ProgramMissing
        vtextension::PaneError::Gone,            // Gone
    };
    static_assert(OwnedPaneErrorRows.size() == std::to_underlying(OwnedPaneError::Gone) + 1U,
                  "every OwnedPaneError has a row");

    [[nodiscard]] constexpr vtextension::PaneError paneErrorOf(OwnedPaneError error) noexcept
    {
        return OwnedPaneErrorRows.at(std::to_underlying(error));
    }

    /// The status a pane reports, as the world names it, indexed by vtbackend::ProgramState.
    inline constexpr auto AgentStatusOf = std::array {
        vtextension::AgentStatus::Idle,    // Idle
        vtextension::AgentStatus::Working, // Working
        vtextension::AgentStatus::Done,    // Done
        vtextension::AgentStatus::Blocked, // Blocked
        vtextension::AgentStatus::Error,   // Error
    };
    static_assert(AgentStatusOf.size() == std::to_underlying(vtbackend::ProgramState::Error) + 1U,
                  "every ProgramState has a row");

    /// One extension's agent panes, on the host's owned panes.
    class PaneHostAdapter final: public vtextension::IPaneHost
    {
      public:
        PaneHostAdapter(std::string_view extension, IOwnedPanes& panes):
            _extension { extension }, _panes { panes }
        {
        }

        [[nodiscard]] std::expected<std::string, vtextension::PaneError> open(
            vtextension::PaneSpawn spawn) override
        {
            return _panes
                .open(OwnedPaneSpawn { .extension = _extension,
                                       .argv = std::move(spawn.argv),
                                       .environment = std::move(spawn.environment),
                                       .removedEnvironment = std::move(spawn.removedEnvironment),
                                       .directory = std::move(spawn.directory),
                                       .cwd = std::move(spawn.cwd),
                                       .title = std::move(spawn.title),
                                       .tag = std::move(spawn.tag),
                                       .nearTab = std::move(spawn.nearTab) })
                .transform_error(paneErrorOf);
        }

        [[nodiscard]] std::expected<void, vtextension::PaneError> close(std::string_view pane) override
        {
            return _panes.close(_extension, pane).transform_error(paneErrorOf);
        }

        [[nodiscard]] std::expected<std::vector<vtextension::PaneInfo>, vtextension::PaneError> list()
            override
        {
            return _panes.list(_extension)
                .transform([](std::vector<OwnedPaneInfo> const& panes) {
                    return std::ranges::to<std::vector<vtextension::PaneInfo>>(
                        panes | std::views::transform([](OwnedPaneInfo const& info) {
                            return vtextension::PaneInfo {
                                .pane = info.pane,
                                .tag = info.tag,
                                .status = info.status.transform([](PaneStatus const& status) {
                                    return AgentStatusOf.at(std::to_underlying(status.state));
                                }),
                                .message = info.status ? info.status->message : std::string {},
                                .program = info.program == PaneLiveness::Alive
                                               ? vtextension::PaneProgram::Alive
                                               : vtextension::PaneProgram::Gone,
                            };
                        }));
                })
                .transform_error(paneErrorOf);
        }

        [[nodiscard]] std::expected<void, vtextension::PaneError> reveal(std::string_view pane,
                                                                         vtextension::Origin origin) override
        {
            return _panes.reveal(_extension, pane, origin).transform_error(paneErrorOf);
        }

      private:
        std::string _extension;
        IOwnedPanes& _panes;
    };

    /// @return The daemon world @p config declares, its problems logged.
    [[nodiscard]] std::unique_ptr<vtextension::DaemonExtensionRuntime> compose(
        vtextension::DaemonExtensionRuntimeConfig config,
        DaemonWorldServices const& services,
        vtextension::IExtensionPresence& presence,
        DaemonWorldWiring wiring)
    {
        auto composed = vtextension::DaemonExtensionRuntime::create(
            std::move(config),
            vtextension::DaemonExtensionRuntimeServices {
                .backend = services.backend,
                .runner = services.runner,
                .directories = services.directories,
                .files = services.files,
                .fileSystem = services.fileSystem,
                .locker = services.locker,
                .wallClock = services.wallClock,
                .localTime = services.localTime,
                .presence = presence,
                .tabsFor =
                    [wiring](vtextension::ServedTabOwner const& owner) {
                        return servedTabServiceOf(owner, wiring);
                    },
                .panesFor =
                    [wiring](vtextension::ServedTabOwner const& owner) { return paneHostOf(owner, wiring); },
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

    /// Who draws an extension's served tabs, by where it stands.
    enum class TabsDrawn : uint8_t
    {
        Unchanged = 0, ///< Whoever did before.
        ByHost,        ///< The host's frame: it stopped after repeated faults (IServedTabs::drawStopped).
        ByGuest,       ///< The extension again: it runs (IServedTabs::resumeDrawing).
    };

    /// One phase of vtextension's, as vthost names it, and who draws its tabs once it is there.
    struct PhaseRow
    {
        vtextension::DaemonExtensionPhase phase;
        DaemonExtensionState state;
        TabsDrawn tabs = TabsDrawn::Unchanged;
    };

    inline constexpr auto PhaseRows = std::array {
        PhaseRow { .phase = vtextension::DaemonExtensionPhase::Stopped,
                   .state = DaemonExtensionState::Stopped },
        PhaseRow { .phase = vtextension::DaemonExtensionPhase::Pending,
                   .state = DaemonExtensionState::Pending },
        PhaseRow { .phase = vtextension::DaemonExtensionPhase::Denied,
                   .state = DaemonExtensionState::Denied },
        PhaseRow { .phase = vtextension::DaemonExtensionPhase::Running,
                   .state = DaemonExtensionState::Running,
                   .tabs = TabsDrawn::ByGuest },
        PhaseRow { .phase = vtextension::DaemonExtensionPhase::Disabled,
                   .state = DaemonExtensionState::Disabled,
                   .tabs = TabsDrawn::ByHost },
        PhaseRow { .phase = vtextension::DaemonExtensionPhase::Failed,
                   .state = DaemonExtensionState::Failed },
    };

    /// @return @p phase as vthost names it.
    [[nodiscard]] DaemonExtensionState stateOf(vtextension::DaemonExtensionPhase phase) noexcept
    {
        auto const row = std::ranges::find(PhaseRows, phase, &PhaseRow::phase);
        return row != PhaseRows.end() ? row->state : DaemonExtensionState::Stopped;
    }

    /// @return Who draws the served tabs of an extension that reached @p phase.
    [[nodiscard]] TabsDrawn tabsDrawnIn(vtextension::DaemonExtensionPhase phase) noexcept
    {
        auto const row = std::ranges::find(PhaseRows, phase, &PhaseRow::phase);
        return row != PhaseRows.end() ? row->tabs : TabsDrawn::Unchanged;
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
        HintRow { .served = ServedHint::Panes, .hint = vtextension::DaemonHint::Panes },
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

std::unique_ptr<vtextension::IPaneHost> paneHostOf(vtextension::ServedTabOwner const& owner,
                                                   DaemonWorldWiring wiring)
{
    return std::make_unique<PaneHostAdapter>(owner.extension, wiring.panes);
}

std::unique_ptr<vtextension::IServedTabService> servedTabServiceOf(vtextension::ServedTabOwner const& owner,
                                                                   DaemonWorldWiring wiring)
{
    return std::make_unique<ServedTabAdapter>(owner, wiring);
}

DaemonWorldExtensions::DaemonWorldExtensions(vtextension::DaemonExtensionRuntimeConfig config,
                                             DaemonWorldServices const& services,
                                             DaemonWorldEvents events,
                                             OwnerPost post,
                                             DaemonWorldWiring wiring):
    _events { std::move(events) },
    _post { std::move(post) },
    _wiring { wiring },
    _runtime { compose(std::move(config), services, *this, wiring) }
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
    // Its dashboards would sit frozen: they close with its grant.
    _wiring.tabs.closeAll(name);
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
    status.lastFault = lastFaultOf(name);
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

std::string DaemonWorldExtensions::lastFaultOf(std::string_view name) const
{
    auto const status = _runtime->status(name);
    if (!status || !status->lastFault)
        return {};
    return vtextension::neutralizedForDisplay(vtextension::errorText(*status->lastFault));
}

void DaemonWorldExtensions::drawStoppedIfDisabled(std::string const& name)
{
    // Restarted since the run was disabled: the extension draws again.
    auto const status = _runtime->status(name);
    if (!status || status->state != vtextension::DaemonRunnerState::Disabled)
        return;
    auto const entries = _runtime->entries();
    auto const entry = std::ranges::find(entries, name, &vtextension::DaemonExtensionEntry::name);
    if (entry == entries.end())
        return;
    _wiring.tabs.drawStopped(name, entry->title, lastFaultOf(name));
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
    switch (tabsDrawnIn(phase))
    {
        case TabsDrawn::Unchanged: break;
        // The runtime, which knows its title and fault, is asked on its owner's thread.
        case TabsDrawn::ByHost:
            _post([this, name = std::string(extension)] { drawStoppedIfDisabled(name); });
            break;
        case TabsDrawn::ByGuest: _wiring.tabs.resumeDrawing(extension); break;
    }
    // Every phase change repaints: what the status line and the palette show follows from it.
    if (_events.summaryChanged)
        _events.summaryChanged();
    if (_events.presenceChanged)
        _events.presenceChanged();
}

} // namespace vthost
