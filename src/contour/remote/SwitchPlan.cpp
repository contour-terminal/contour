// SPDX-License-Identifier: Apache-2.0
#include <contour/remote/SwitchPlan.hpp>

#include <algorithm>
#include <array>
#include <charconv>
#include <expected>
#include <format>
#include <ranges>
#include <string>
#include <type_traits>
#include <variant>
#include <vector>

#include <vtcontrol/Target.hpp>

namespace contour::remote
{

namespace
{
    /// @return The row @p spec names among @p rows, or why none.
    [[nodiscard]] std::expected<WorkspaceRow const*, std::string> resolveSpec(
        std::string_view spec, std::span<WorkspaceRow const> rows)
    {
        auto const candidates =
            std::ranges::to<std::vector>(rows | std::views::transform([](auto const& row) {
                                             return vtcontrol::TargetCandidate {
                                                 .ref = row.ref,
                                                 .label = row.label,
                                                 .position = row.position,
                                             };
                                         }));
        auto const resolved = vtcontrol::resolveTarget(vtcontrol::ObjectKind::Workspace, spec, candidates);
        if (!resolved)
            return std::unexpected(resolved.error().message);
        auto const found = std::ranges::find(rows, *resolved, &WorkspaceRow::ref);
        if (found == rows.end())
            return std::unexpected(std::format("no workspace '{}'", spec));
        return &*found;
    }
} // namespace

SwitchDecision planSwitch(session::SwitchTarget const& target,
                          std::span<WorkspaceRow const> daemonWorkspaces,
                          std::optional<uint64_t> currentWorkspace,
                          SameDaemon sameDaemon)
{
    // Which row the target names; a decision of its own where it names none.
    auto const named = std::visit(
        [&](auto const& workspace) -> std::variant<WorkspaceRow const*, SwitchDecision> {
            using T = std::decay_t<decltype(workspace)>;
            if constexpr (std::is_same_v<T, session::NewWorkspace>)
                return SwitchDecision { SwitchTo { .workspace = 0,
                                                   .undo = vthost::proto::WorkspaceRelease::Closed } };
            else if constexpr (std::is_same_v<T, session::AnyWorkspace>)
            {
                if (daemonWorkspaces.size() > 1)
                    return SwitchDecision { SwitchPick {} };
                if (daemonWorkspaces.empty())
                    return SwitchDecision { SwitchTo { .workspace = 0,
                                                       .undo = vthost::proto::WorkspaceRelease::Closed } };
                return &daemonWorkspaces.front();
            }
            else if constexpr (std::is_same_v<T, session::WorkspaceAt>)
            {
                auto const found =
                    std::ranges::find(daemonWorkspaces, workspace.position, &WorkspaceRow::position);
                if (found == daemonWorkspaces.end())
                    return SwitchDecision { SwitchUnresolved {
                        .reason = std::format("no workspace at position {}", workspace.position) } };
                return &*found;
            }
            else if constexpr (std::is_same_v<T, vtcontrol::ObjectRef>)
            {
                auto const found = std::ranges::find(daemonWorkspaces, workspace, &WorkspaceRow::ref);
                if (found == daemonWorkspaces.end())
                    return SwitchDecision { SwitchUnresolved {
                        .reason = std::format("no workspace '{}'", vtcontrol::toString(workspace)) } };
                return &*found;
            }
            else
            {
                auto const resolved = resolveSpec(workspace.spec, daemonWorkspaces);
                if (!resolved)
                    return SwitchDecision { SwitchUnresolved { .reason = resolved.error() } };
                return *resolved;
            }
        },
        target.workspace);
    if (auto const* const decided = std::get_if<SwitchDecision>(&named))
        return *decided;
    auto const* const row = std::get<WorkspaceRow const*>(named);
    if (sameDaemon == SameDaemon::Yes && currentWorkspace == row->id)
        return SwitchNoop {};
    if (row->shownByWindowHere)
        return SwitchFocus { .window = *row->shownByWindowHere };
    return SwitchTo { .workspace = row->id, .undo = undoReleaseFor(row->visibility) };
}

std::vector<WorkspaceRow> workspaceRowsOf(std::span<vtcontrol::WorkspaceInfo const> workspaces,
                                          ShownHere const& shownHere)
{
    using vthost::WorkspaceVisibility;
    static constexpr auto Visibilities =
        std::array { WorkspaceVisibility::Left, WorkspaceVisibility::Shown, WorkspaceVisibility::Hidden };
    auto rows = std::vector<WorkspaceRow> {};
    for (auto const& info: workspaces)
    {
        // A workspace holding no tab has nothing to show -- a daemon no client attached to yet has one, its
        // own -- and a window switched to it would be emptied, and close.
        if (info.tabs.empty())
            continue;
        auto id = uint64_t {};
        auto const* const end = info.native.data() + info.native.size();
        if (auto const [last, error] = std::from_chars(info.native.data(), end, id);
            error != std::errc {} || last != end || id == 0)
            continue;
        auto const visibility = std::ranges::find(
            Visibilities, info.visibility, [](WorkspaceVisibility each) { return vthost::nameOf(each); });
        auto const active = std::ranges::find(info.tabs, true, &vtcontrol::TabInfo::isActive);
        rows.push_back(WorkspaceRow {
            .id = id,
            .ref = vtcontrol::parseObjectRef(info.id).value_or(vtcontrol::ObjectRef {}),
            .label = info.label,
            .position = info.position,
            .name = workspaceDisplayName(info.label,
                                         active != info.tabs.end() ? std::string_view { active->title } : "",
                                         info.position),
            .visibility = visibility != Visibilities.end() ? *visibility : WorkspaceVisibility::Left,
            .tabs = info.tabs.size(),
            .shownByWindowHere = shownHere(id),
        });
    }
    return rows;
}

std::vector<window::SwitcherWorkspace> switcherWorkspacesOf(std::span<WorkspaceRow const> rows,
                                                            vtworkspace::WorkspaceId window)
{
    auto const stateOf = [window](WorkspaceRow const& row) {
        using window::SwitcherWorkspaceState;
        if (row.shownByWindowHere == window)
            return SwitcherWorkspaceState::ThisWindow;
        // One a window here shows is shown, whether or not the daemon counted that window yet.
        if (row.shownByWindowHere || row.visibility == vthost::WorkspaceVisibility::Shown)
            return SwitcherWorkspaceState::ShownElsewhere;
        return row.visibility == vthost::WorkspaceVisibility::Hidden ? SwitcherWorkspaceState::Hidden
                                                                     : SwitcherWorkspaceState::Left;
    };
    return std::ranges::to<std::vector>(rows | std::views::transform([&](WorkspaceRow const& row) {
                                            return window::SwitcherWorkspace {
                                                .ref = row.ref,
                                                .position = row.position,
                                                .name = row.name,
                                                .tabs = row.tabs,
                                                .state = stateOf(row),
                                            };
                                        }));
}

std::string workspaceDisplayName(std::string_view label, std::string_view activeTabTitle, uint32_t position)
{
    if (!label.empty())
        return std::string { label };
    if (!activeTabTitle.empty())
        return std::string { activeTabTitle };
    return std::format("workspace {}", position);
}

} // namespace contour::remote
