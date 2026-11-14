// SPDX-License-Identifier: Apache-2.0
#include <contour/command/FuzzyFilter.hpp>
#include <contour/window/LocationChip.hpp>
#include <contour/window/SwitcherRows.hpp>

#include <algorithm>
#include <format>
#include <iterator>
#include <ranges>
#include <tuple>

#include <vthost/SocketPath.hpp>

namespace contour::window
{

namespace
{
    using vthost::proto::SwitchDaemonKind;

    /// @return The target choosing @p workspace of @p daemon goes to.
    [[nodiscard]] session::SwitchTarget targetOf(SwitcherDaemon const& daemon,
                                                 session::WorkspaceTarget workspace)
    {
        return session::SwitchTarget { .kind = daemon.address.kind,
                                       .daemon = daemon.address.daemon,
                                       .workspace = std::move(workspace) };
    }

    /// @return `Workspaces on work`.
    [[nodiscard]] std::string workspacesOnTitle(SwitcherDaemon const& daemon)
    {
        return std::format("Workspaces on {}", switcherDaemonName(daemon));
    }

    /// @return `New Workspace`, or `New Workspace “api”` once something is typed.
    [[nodiscard]] std::string newWorkspaceTitle(std::string_view filter)
    {
        return filter.empty() ? std::string { "New Workspace" } : std::format("New Workspace “{}”", filter);
    }

    /// The pill a workspace row of its own daemon's group wears: where else it is shown.
    struct StatePill
    {
        SwitcherWorkspaceState state;
        SwitcherPill pill;
        std::string_view text;
    };

    /// One row per SwitcherWorkspaceState, in enumerator order.
    constexpr auto StatePills = std::array {
        StatePill { .state = SwitcherWorkspaceState::Hidden, .pill = SwitcherPill::None, .text = {} },
        StatePill { .state = SwitcherWorkspaceState::ShownElsewhere,
                    .pill = SwitcherPill::OtherWindow,
                    .text = "Other window" },
        StatePill {
            .state = SwitcherWorkspaceState::ThisWindow, .pill = SwitcherPill::Current, .text = "Current" },
    };
    static_assert(std::ranges::all_of(std::views::iota(std::size_t { 0 }, StatePills.size()),
                                      [](std::size_t index) {
                                          return StatePills[index].state
                                                 == static_cast<SwitcherWorkspaceState>(index);
                                      }),
                  "StatePills lists every SwitcherWorkspaceState once, in enumerator order");

    /// A candidate row and what it is ranked by while filtering.
    struct Ranked
    {
        SwitcherRow row;
        int score = 0;
        std::size_t daemon = 0;
        uint32_t position = 0;
    };

    /// Sorts @p ranked best score first, ties by daemon then position, and appends the rows to @p rows.
    void appendBestFirst(std::vector<Ranked> ranked, std::vector<SwitcherRow>& rows)
    {
        std::ranges::stable_sort(ranked, [](Ranked const& a, Ranked const& b) {
            return std::tuple { -a.score, a.daemon, a.position }
                   < std::tuple { -b.score, b.daemon, b.position };
        });
        for (auto& each: ranked)
            rows.push_back(std::move(each.row));
    }

    /// Builds the rows of one view; one instance per switcherRows() call.
    class RowBuilder
    {
      public:
        RowBuilder(SwitcherInputs const& inputs, SwitcherView const& view): _inputs { inputs }, _view { view }
        {
        }

        [[nodiscard]] std::vector<SwitcherRow> build() &&
        {
            switch (_view.level)
            {
                case SwitcherLevel::Top:
                    if (listsOneDaemon(_view.scope))
                        addOneDaemon(_view.daemon);
                    else
                        addEverything();
                    break;
                case SwitcherLevel::Daemon: addOneDaemon(_view.daemon); break;
                case SwitcherLevel::NewDaemon: addLabelAction(); break;
            }
            return std::move(_rows);
        }

      private:
        [[nodiscard]] bool filtering() const noexcept { return !_view.filter.empty(); }

        [[nodiscard]] bool hasDaemon(std::size_t index) const noexcept
        {
            return index < _inputs.daemons.size();
        }

        /// Appends @p row under @p group, titling the group on its first row.
        void add(SwitcherRow row, SwitcherGroup group, std::string const& groupTitle)
        {
            row.group = group;
            if (_rows.empty() || _rows.back().group != group)
                row.groupTitle = groupTitle;
            _rows.push_back(std::move(row));
        }

        [[nodiscard]] SwitcherGroup groupOfDaemon(std::size_t index) const noexcept
        {
            return index == _inputs.windowDaemon ? SwitcherGroup::WindowDaemon
                                                 : SwitcherGroup::DaemonWorkspaces;
        }

        /// @return @p workspace of daemon @p index as a row, with the pill of where it is shown.
        [[nodiscard]] SwitcherRow workspaceRow(std::size_t index, SwitcherWorkspace const& workspace) const
        {
            auto const& pill = StatePills.at(static_cast<std::size_t>(workspace.state));
            return SwitcherRow { .kind = SwitcherRowKind::Workspace,
                                 .group = SwitcherGroup::None,
                                 .groupTitle = {},
                                 .daemon = index,
                                 .title = workspace.name,
                                 .titleMatches = {},
                                 .status = workspace.status,
                                 .pill = pill.pill,
                                 .pillText = std::string { pill.text },
                                 .tabs = workspace.tabs,
                                 .state = workspace.state,
                                 .target = targetOf(_inputs.daemons[index], workspace.ref) };
        }

        /// @return A row of no daemon's but the window's: an action, or a row that only says something.
        [[nodiscard]] SwitcherRow plainRow(SwitcherRowKind kind,
                                           std::string title,
                                           session::SwitchTarget target = {}) const
        {
            return SwitcherRow { .kind = kind,
                                 .group = SwitcherGroup::None,
                                 .groupTitle = {},
                                 .daemon = _inputs.windowDaemon,
                                 .title = std::move(title),
                                 .titleMatches = {},
                                 .status = {},
                                 .pill = SwitcherPill::None,
                                 .pillText = {},
                                 .tabs = 0,
                                 .state = SwitcherWorkspaceState::Hidden,
                                 .target = std::move(target) };
        }

        /// @return The New Workspace row creating one on daemon @p index.
        [[nodiscard]] SwitcherRow newWorkspaceRow(std::size_t index) const
        {
            auto row = plainRow(SwitcherRowKind::NewWorkspace,
                                newWorkspaceTitle(_view.filter),
                                targetOf(_inputs.daemons[index], session::NewWorkspace {}));
            row.daemon = index;
            return row;
        }

        /// The workspaces of daemon @p index (matching, best first, while filtering), or what stands in for
        /// them while it is listed; then its New Workspace.
        void addOneDaemon(std::size_t index)
        {
            if (!hasDaemon(index))
                return;
            auto const& daemon = _inputs.daemons[index];
            auto const group = groupOfDaemon(index);
            auto const title = workspacesOnTitle(daemon);
            if (daemon.listing == SwitcherListing::Failed)
            {
                // What was listed before is stale now: the row says why there is nothing.
                auto row = plainRow(
                    SwitcherRowKind::Failure,
                    std::format("Could not list {}: {}", switcherDaemonName(daemon), daemon.failure));
                row.daemon = index;
                add(std::move(row), group, title);
            }
            else if (daemon.workspaces.empty() && daemon.listing != SwitcherListing::Listed)
            {
                // Listed for the first time; a refresh keeps the workspaces it knows instead.
                auto row = plainRow(SwitcherRowKind::Connecting,
                                    std::format("Connecting to {}…", switcherDaemonName(daemon)));
                row.daemon = index;
                add(std::move(row), group, title);
            }
            else
            {
                auto ranked = std::vector<Ranked> {};
                for (auto const& workspace: daemon.workspaces)
                    if (auto matched = rankedWorkspace(index, workspace))
                        ranked.push_back(std::move(*matched));
                auto matching = std::vector<SwitcherRow> {};
                if (filtering())
                    appendBestFirst(std::move(ranked), matching);
                else
                    for (auto& each: ranked)
                        matching.push_back(std::move(each.row));
                for (auto& row: matching)
                    add(std::move(row), group, title);
            }
            add(newWorkspaceRow(index), SwitcherGroup::Actions, "Actions");
        }

        /// @return @p workspace's row ranked by how well it matches the filter; none when it does not.
        [[nodiscard]] std::optional<Ranked> rankedWorkspace(std::size_t index,
                                                            SwitcherWorkspace const& workspace) const
        {
            auto matched = command::fuzzyMatch(_view.filter, workspace.name);
            if (!matched)
                return std::nullopt;
            auto row = workspaceRow(index, workspace);
            row.titleMatches = std::move(matched->positions);
            return Ranked { .row = std::move(row),
                            .score = matched->score,
                            .daemon = index,
                            .position = workspace.position };
        }

        /// Every daemon: the window's daemon's workspaces (or every match), the other daemons, the actions.
        void addEverything()
        {
            if (filtering())
                addMatches();
            else if (hasDaemon(_inputs.windowDaemon))
                for (auto const& workspace: _inputs.daemons[_inputs.windowDaemon].workspaces)
                    add(workspaceRow(_inputs.windowDaemon, workspace),
                        SwitcherGroup::WindowDaemon,
                        workspacesOnTitle(_inputs.daemons[_inputs.windowDaemon]));
            addOtherDaemons();
            if (hasDaemon(_inputs.windowDaemon))
                add(newWorkspaceRow(_inputs.windowDaemon), SwitcherGroup::Actions, "Actions");
            if (!filtering())
                add(plainRow(SwitcherRowKind::NewDaemon, "New Daemon…"), SwitcherGroup::Actions, "Actions");
        }

        /// The matching workspaces of the window's daemon and every listed local daemon, best first, each
        /// with its daemon's pill. A saved host's are left out: it is matched by its name.
        void addMatches()
        {
            auto ranked = std::vector<Ranked> {};
            for (auto const& [index, daemon]:
                 std::views::zip(std::views::iota(std::size_t { 0 }), _inputs.daemons))
            {
                if (index != _inputs.windowDaemon && daemon.address.kind == SwitchDaemonKind::Host)
                    continue;
                for (auto const& workspace: daemon.workspaces)
                    if (auto matched = rankedWorkspace(index, workspace))
                    {
                        matched->row.pill = SwitcherPill::Daemon;
                        matched->row.pillText = switcherDaemonName(daemon);
                        ranked.push_back(std::move(*matched));
                    }
            }
            auto matching = std::vector<SwitcherRow> {};
            appendBestFirst(std::move(ranked), matching);
            for (auto& row: matching)
                add(std::move(row), SwitcherGroup::Matches, "Workspaces");
        }

        /// Every daemon but the window's (those whose name matches, best first, while filtering), and the
        /// looking row while discovery runs.
        void addOtherDaemons()
        {
            auto ranked = std::vector<Ranked> {};
            for (auto const& [index, daemon]:
                 std::views::zip(std::views::iota(std::size_t { 0 }), _inputs.daemons))
            {
                if (index == _inputs.windowDaemon)
                    continue;
                auto const name = switcherDaemonName(daemon);
                auto matched = command::fuzzyMatch(_view.filter, name);
                if (!matched)
                    continue;
                auto row =
                    plainRow(SwitcherRowKind::Daemon, name, targetOf(daemon, session::AnyWorkspace {}));
                row.daemon = index;
                row.titleMatches = std::move(matched->positions);
                row.status = daemonStatusOf(daemon);
                if (!daemon.tag.empty())
                {
                    row.pill = SwitcherPill::Transport;
                    row.pillText = daemon.tag;
                }
                ranked.push_back(Ranked {
                    .row = std::move(row), .score = matched->score, .daemon = index, .position = 0 });
            }
            auto daemons = std::vector<SwitcherRow> {};
            appendBestFirst(std::move(ranked), daemons);
            for (auto& row: daemons)
                add(std::move(row), SwitcherGroup::OtherDaemons, "Other daemons");
            if (_inputs.discovery == SwitcherDiscovery::Running && !filtering())
                add(plainRow(SwitcherRowKind::Discovering, "Looking for other daemons…"),
                    SwitcherGroup::OtherDaemons,
                    "Other daemons");
        }

        /// The New Daemon level's one action, for a label it can act on.
        void addLabelAction()
        {
            auto const running = switcherRunningLabels(_inputs);
            auto const verdict = switcherLabelVerdict(_view.filter, running);
            if (verdict != LabelVerdict::Available && verdict != LabelVerdict::Running)
                return;
            add(plainRow(SwitcherRowKind::LabelAction,
                         switcherLabelActionTitle(_view.filter, verdict),
                         session::SwitchTarget { .kind = SwitchDaemonKind::Label,
                                                 .daemon = std::string { _view.filter },
                                                 .workspace = session::AnyWorkspace {} }),
                SwitcherGroup::None,
                {});
        }

        SwitcherInputs const& _inputs;
        SwitcherView const& _view;
        std::vector<SwitcherRow> _rows;
    };
} // namespace

std::string_view describe(SwitcherWorkspaceState state) noexcept
{
    switch (state)
    {
        case SwitcherWorkspaceState::Hidden: return "hidden";
        case SwitcherWorkspaceState::ShownElsewhere: return "shown elsewhere";
        case SwitcherWorkspaceState::ThisWindow: return "this window";
    }
    return "hidden";
}

LabelVerdict switcherLabelVerdict(std::string_view label, std::span<std::string const> runningLabels)
{
    if (label.empty())
        return LabelVerdict::Empty;
    if (!vthost::validateSocketLabel(label))
        return LabelVerdict::Invalid;
    return std::ranges::find(runningLabels, label) != runningLabels.end() ? LabelVerdict::Running
                                                                          : LabelVerdict::Available;
}

std::vector<std::string> switcherRunningLabels(SwitcherInputs const& inputs)
{
    auto labels = std::vector<std::string> {};
    for (auto const& daemon: inputs.daemons)
        if (daemon.address.kind == SwitchDaemonKind::Label)
            labels.push_back(daemon.address.daemon);
    return labels;
}

std::string switcherLabelActionTitle(std::string_view label, LabelVerdict verdict)
{
    switch (verdict)
    {
        case LabelVerdict::Available:
            return std::format("Start daemon “{}” and switch this window to it", label);
        case LabelVerdict::Running: return std::format("Switch this window to daemon “{}”", label);
        case LabelVerdict::Empty:
        case LabelVerdict::Invalid: break;
    }
    return {};
}

std::vector<SwitcherRow> switcherRows(SwitcherInputs const& inputs, SwitcherView const& view)
{
    return RowBuilder { inputs, view }.build();
}

SwitcherStatus daemonStatusOf(SwitcherDaemon const& daemon)
{
    auto statuses = std::vector<SwitcherStatus> {};
    statuses.reserve(daemon.workspaces.size());
    for (auto const& workspace: daemon.workspaces)
        statuses.push_back(workspace.status);
    return rollUpStatus(statuses);
}

std::string switcherDaemonName(session::SwitchTarget const& address, std::string_view hostLabel)
{
    // The switcher's own rows say "this daemon" of an unnameable one the window has no label for; elsewhere
    // it is named as the chip names it.
    return address.kind == SwitchDaemonKind::Same && hostLabel.empty()
               ? std::string { "this daemon" }
               : locationDaemonName(address, hostLabel);
}

std::string switcherDaemonName(SwitcherDaemon const& daemon)
{
    return switcherDaemonName(daemon.address, daemon.hostLabel);
}

SwitcherRowIdentity switcherRowIdentity(SwitcherRow const& row)
{
    return SwitcherRowIdentity { .kind = row.kind, .target = row.target };
}

std::size_t switcherSelection(std::span<SwitcherRow const> rows,
                              std::optional<SwitcherRowIdentity> const& keep)
{
    if (!keep)
        return 0;
    auto const found = std::ranges::find(rows, *keep, switcherRowIdentity);
    return found != rows.end() ? static_cast<std::size_t>(std::distance(rows.begin(), found)) : 0;
}

std::size_t switcherInitialSelection(std::span<SwitcherRow const> rows)
{
    auto const found = std::ranges::find_if(rows, [](SwitcherRow const& row) {
        return row.kind == SwitcherRowKind::Workspace && row.state != SwitcherWorkspaceState::ThisWindow;
    });
    return found != rows.end() ? static_cast<std::size_t>(std::distance(rows.begin(), found)) : 0;
}

} // namespace contour::window
