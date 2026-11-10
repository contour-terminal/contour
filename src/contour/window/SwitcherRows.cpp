// SPDX-License-Identifier: Apache-2.0
#include <contour/cli/ConnectList.hpp>
#include <contour/window/SwitcherRows.hpp>

#include <algorithm>
#include <array>
#include <format>
#include <iterator>
#include <ranges>

namespace contour::window
{

namespace
{
    using vthost::proto::SwitchDaemonKind;

    /// Joins the non-empty parts of a row's detail with ` · `.
    [[nodiscard]] std::string joined(std::initializer_list<std::string_view> parts)
    {
        auto text = std::string {};
        for (auto const part: parts)
        {
            if (part.empty())
                continue;
            if (!text.empty())
                text += " · ";
            text += part;
        }
        return text;
    }

    /// @return `1 tab`, `4 tabs`.
    [[nodiscard]] std::string countOf(std::size_t count, std::string_view one, std::string_view many)
    {
        return std::format("{} {}", count, count == 1 ? one : many);
    }

    /// @return The group @p address is listed under.
    [[nodiscard]] SwitcherGroup groupOf(session::SwitchTarget const& address)
    {
        return address.kind == SwitchDaemonKind::Host ? SwitcherGroup::SavedHosts
                                                      : SwitcherGroup::ThisComputer;
    }

    /// @return Whether @p address names the daemon the window is on.
    [[nodiscard]] bool isWindowDaemon(SwitcherInputs const& inputs, session::SwitchTarget const& address)
    {
        return address.kind == inputs.windowDaemon.kind && address.daemon == inputs.windowDaemon.daemon;
    }

    /// What a saved host's row says while it is not listed, per SwitcherHostLink.
    constexpr auto HostLinkTexts = std::array<std::string_view, 2> { "not yet connected", "not connected" };
    static_assert(HostLinkTexts.size() == static_cast<std::size_t>(SwitcherHostLink::Disconnected) + 1,
                  "every SwitcherHostLink needs its text");

    /// @return Whether a daemon runs (a local one) or is reached (a saved host).
    [[nodiscard]] std::string_view reachOf(SwitcherInputs const& inputs, SwitcherDaemon const& daemon)
    {
        if (daemon.address.kind != SwitchDaemonKind::Host)
            return "running";
        if (isWindowDaemon(inputs, daemon.address))
            return "connected";
        if (daemon.listing == SwitcherListing::NotListed)
            return HostLinkTexts[static_cast<std::size_t>(daemon.link)];
        return {};
    }

    /// @return How a daemon's listing stands, when it is not simply listed.
    [[nodiscard]] std::string listingOf(SwitcherDaemon const& daemon)
    {
        switch (daemon.listing)
        {
            case SwitcherListing::Listing: return "listing…";
            case SwitcherListing::Failed: return std::format("could not list: {}", daemon.failure);
            case SwitcherListing::NotListed:
            case SwitcherListing::Listed: break;
        }
        return {};
    }

    /// @return What a daemon's row says: whether it runs or is reached, how many workspaces, whether this
    ///         window is on it, and how its listing stands.
    [[nodiscard]] std::string daemonDetail(SwitcherInputs const& inputs, SwitcherDaemon const& daemon)
    {
        auto const count = daemon.listing == SwitcherListing::Listed
                               ? std::optional { daemon.workspaces.size() }
                               : daemon.workspaceCount;
        return joined({ reachOf(inputs, daemon),
                        count ? countOf(*count, "workspace", "workspaces") : std::string {},
                        isWindowDaemon(inputs, daemon.address) ? std::string_view { "this window" }
                                                               : std::string_view {},
                        listingOf(daemon) });
    }

    /// @return The target choosing @p workspace of @p daemon goes to.
    [[nodiscard]] session::SwitchTarget targetOf(SwitcherDaemon const& daemon,
                                                 session::WorkspaceTarget workspace)
    {
        return session::SwitchTarget { .kind = daemon.address.kind,
                                       .daemon = daemon.address.daemon,
                                       .workspace = std::move(workspace) };
    }

    /// What "New daemon…" is found by when filtering.
    constexpr auto NewDaemonTitle = std::string_view { "New daemon…" };
} // namespace

std::string_view describe(SwitcherWorkspaceState state) noexcept
{
    switch (state)
    {
        case SwitcherWorkspaceState::Left: return "not shown";
        case SwitcherWorkspaceState::Hidden: return "hidden";
        case SwitcherWorkspaceState::ShownElsewhere: return "shown elsewhere";
        case SwitcherWorkspaceState::ThisWindow: return "this window";
    }
    return "not shown";
}

std::string switcherDaemonName(session::SwitchTarget const& address)
{
    return address.kind == SwitchDaemonKind::Same ? std::string { "this daemon" } : address.daemon;
}

std::vector<SwitcherRow> switcherRows(SwitcherInputs const& inputs, std::string_view filter)
{
    auto rows = std::vector<SwitcherRow> {};
    auto const filtering = !filter.empty();
    auto const picking = inputs.scope == SwitcherScope::OneDaemon;

    auto const addDaemon = [&](std::size_t index, SwitcherDaemon const& daemon) {
        auto const name = switcherDaemonName(daemon.address);
        auto const group = picking ? SwitcherGroup::Pick : groupOf(daemon.address);
        auto const daemonMatches = !filtering || cli::matchesFilter(name, filter);
        auto const expanded = picking || daemon.expansion == SwitcherExpansion::Expanded;
        auto workspaceRows = std::vector<SwitcherRow> {};
        for (auto const& workspace: daemon.workspaces)
        {
            auto const title = std::format("{} \"{}\"", workspace.position, workspace.name);
            auto const shows =
                (expanded && daemonMatches) || (filtering && cli::matchesFilter(title, filter));
            if (!shows)
                continue;
            workspaceRows.push_back(SwitcherRow {
                .kind = SwitcherRowKind::Workspace,
                .group = group,
                .daemon = index,
                .title = title,
                .detail = joined({ countOf(workspace.tabs, "tab", "tabs"), describe(workspace.state) }),
                .tag = {},
                .expansion = SwitcherExpansion::Collapsed,
                .target = targetOf(daemon, workspace.ref),
            });
        }
        if (!daemonMatches && workspaceRows.empty())
            return;
        if (!picking)
            rows.push_back(SwitcherRow {
                .kind = SwitcherRowKind::Daemon,
                .group = group,
                .daemon = index,
                .title = name,
                .detail = daemonDetail(inputs, daemon),
                .tag = daemon.tag,
                .expansion = expanded ? SwitcherExpansion::Expanded : SwitcherExpansion::Collapsed,
                .target = targetOf(daemon, session::AnyWorkspace {}),
            });
        rows.insert(rows.end(), workspaceRows.begin(), workspaceRows.end());
        if (expanded && daemonMatches)
            rows.push_back(SwitcherRow {
                .kind = SwitcherRowKind::NewWorkspace,
                .group = group,
                .daemon = index,
                .title = std::format("+ new workspace on {}", name),
                .detail = {},
                .tag = {},
                .expansion = SwitcherExpansion::Collapsed,
                .target = targetOf(daemon, session::NewWorkspace {}),
            });
    };

    for (auto const& [index, daemon]: std::views::zip(std::views::iota(std::size_t { 0 }), inputs.daemons))
    {
        if (picking && index > 0)
            break;
        if (!picking && groupOf(daemon.address) != SwitcherGroup::ThisComputer)
            continue;
        addDaemon(index, daemon);
    }
    if (picking)
        return rows;
    if (!filtering || cli::matchesFilter(NewDaemonTitle, filter))
        rows.push_back(SwitcherRow {
            .kind = SwitcherRowKind::NewDaemon,
            .group = SwitcherGroup::ThisComputer,
            .daemon = 0,
            .title = std::string { NewDaemonTitle },
            .detail = "Start a daemon of its own, under a label",
            .tag = {},
            .expansion = SwitcherExpansion::Collapsed,
            .target = {},
        });
    for (auto const& [index, daemon]: std::views::zip(std::views::iota(std::size_t { 0 }), inputs.daemons))
        if (groupOf(daemon.address) == SwitcherGroup::SavedHosts)
            addDaemon(index, daemon);
    return rows;
}

std::size_t switcherSelection(std::span<SwitcherRow const> rows,
                              std::optional<session::SwitchTarget> const& keep)
{
    if (!keep)
        return 0;
    auto const found = std::ranges::find(rows, *keep, &SwitcherRow::target);
    return found != rows.end() ? static_cast<std::size_t>(std::distance(rows.begin(), found)) : 0;
}

std::size_t switcherInitialSelection(std::span<SwitcherRow const> rows,
                                     session::SwitchTarget const& windowDaemon)
{
    auto const isDaemonRow = [](SwitcherRow const& row) {
        return row.kind == SwitcherRowKind::Daemon;
    };
    auto const isOther = [&](SwitcherRow const& row) {
        return isDaemonRow(row)
               && (row.target.kind != windowDaemon.kind || row.target.daemon != windowDaemon.daemon);
    };
    auto const index = [&](auto found) {
        return static_cast<std::size_t>(std::distance(rows.begin(), found));
    };
    if (auto const other = std::ranges::find_if(rows, isOther); other != rows.end())
        return index(other);
    if (auto const own = std::ranges::find_if(rows, isDaemonRow); own != rows.end())
        return index(own);
    return 0;
}

} // namespace contour::window
