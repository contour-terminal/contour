// SPDX-License-Identifier: Apache-2.0
#include <contour/remote/SwitchPlan.hpp>

#include <core/Utils.hpp>

#include <algorithm>
#include <array>
#include <cctype>
#include <charconv>
#include <expected>
#include <format>
#include <ranges>
#include <string>
#include <string_view>
#include <type_traits>
#include <variant>
#include <vector>

#include <vtcontrol/Target.hpp>
#include <vtworkspace/TabName.hpp>

namespace contour::remote
{

namespace
{
    /// The control id of a workspace a daemon stated none for.
    constexpr auto NoControlId = std::array<uint8_t, 16> {};

    /// @return A reference to the daemon's workspace @p id that only this process uses, to tell apart the
    /// rows
    ///         of a directory whose daemon stated no control ids (@see resolvesFromDirectory()).
    [[nodiscard]] vtcontrol::ObjectRef localRefOf(uint64_t id)
    {
        auto ref = vtcontrol::ObjectRef { .kind = vtcontrol::ObjectKind::Workspace, .uuid = {} };
        for (auto const index: std::views::iota(0, 8))
            ref.uuid.bytes.at(static_cast<std::size_t>(15 - index)) = static_cast<uint8_t>(id >> (8 * index));
        return ref;
    }

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
                return SwitchDecision { SwitchTo { .workspace = 0 } };
            else if constexpr (std::is_same_v<T, session::AnyWorkspace>)
            {
                if (daemonWorkspaces.size() > 1)
                    return SwitchDecision { SwitchPick {} };
                if (daemonWorkspaces.empty())
                    return SwitchDecision { SwitchTo { .workspace = 0 } };
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
    return SwitchTo { .workspace = row->id };
}

std::vector<WorkspaceRow> workspaceRowsOf(std::span<vtcontrol::WorkspaceInfo const> workspaces,
                                          ShownHere const& shownHere)
{
    using vthost::WorkspaceVisibility;
    static constexpr auto Visibilities =
        std::array { WorkspaceVisibility::Shown, WorkspaceVisibility::Hidden };
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
            .name = workspaceDisplayName(
                info.label,
                active != info.tabs.end() ? vtworkspace::tabNameOf({ .label = active->label }, active->title)
                                          : "",
                info.position),
            .visibility = visibility != Visibilities.end() ? *visibility : WorkspaceVisibility::Hidden,
            .tabs = info.tabs.size(),
            .shownByWindowHere = shownHere(id),
            .status = window::switcherStatusOf(info),
        });
    }
    return rows;
}

std::vector<WorkspaceRow> workspaceRowsOf(window::WorkspaceDirectory const& directory,
                                          ShownHere const& shownHere)
{
    auto rows = std::vector<WorkspaceRow> {};
    for (auto const [index, workspace]: core::views::enumerate(directory.workspaces))
    {
        // As a listing's: one holding no tab has nothing for a window to show.
        if (workspace.tabs.empty())
            continue;
        auto const position = static_cast<uint32_t>(index + 1);
        auto shown = shownHere(workspace.id);
        rows.push_back(WorkspaceRow {
            .id = workspace.id,
            .ref = workspace.controlId != NoControlId
                       ? vtcontrol::ObjectRef { .kind = vtcontrol::ObjectKind::Workspace,
                                                .uuid = vtcontrol::Uuid { .bytes = workspace.controlId } }
                       : localRefOf(workspace.id),
            .label = workspace.label,
            .position = position,
            .name = workspaceDisplayName(workspace.label, workspace.tabs.front().label, position),
            .visibility = shown ? vthost::WorkspaceVisibility::Shown : vthost::WorkspaceVisibility::Hidden,
            .tabs = workspace.tabs.size(),
            .shownByWindowHere = shown,
            .status = {},
        });
    }
    return rows;
}

bool namesControlId(session::WorkspaceTarget const& workspace)
{
    if (std::holds_alternative<vtcontrol::ObjectRef>(workspace))
        return true;
    auto const* const named = std::get_if<session::WorkspaceNamed>(&workspace);
    if (named == nullptr)
        return false;
    // A full id, or what may be the end of one: four hex digits or more that are no position.
    auto const& spec = named->spec;
    auto const isHex = [](char c) {
        return std::isxdigit(static_cast<unsigned char>(c)) != 0;
    };
    auto const isDigit = [](char c) {
        return std::isdigit(static_cast<unsigned char>(c)) != 0;
    };
    return vtcontrol::parseObjectRef(spec).has_value()
           || (spec.size() >= 4 && std::ranges::all_of(spec, isHex) && !std::ranges::all_of(spec, isDigit));
}

bool resolvesFromDirectory(session::WorkspaceTarget const& workspace,
                           window::WorkspaceDirectory const& directory)
{
    if (!namesControlId(workspace))
        return true;
    // The workspaces a switch could go to whose control id the daemon stated: one holding no tab is no row.
    auto candidates = std::vector<vtcontrol::TargetCandidate> {};
    for (auto const [index, listed]: core::views::enumerate(directory.workspaces))
        if (!listed.tabs.empty() && listed.controlId != NoControlId)
            candidates.push_back(vtcontrol::TargetCandidate {
                .ref = vtcontrol::ObjectRef { .kind = vtcontrol::ObjectKind::Workspace,
                                              .uuid = vtcontrol::Uuid { .bytes = listed.controlId } },
                .label = listed.label,
                .position = static_cast<uint32_t>(index + 1),
            });
    // Only an id the directory carries resolves here; one it does not yet (a reconnect's empty directory, a
    // workspace created before its batch settled) is the listing's to find.
    if (auto const* const ref = std::get_if<vtcontrol::ObjectRef>(&workspace))
        return std::ranges::contains(candidates, *ref, &vtcontrol::TargetCandidate::ref);
    return vtcontrol::resolveTarget(vtcontrol::ObjectKind::Workspace,
                                    std::get<session::WorkspaceNamed>(workspace).spec,
                                    candidates)
        .has_value();
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
        return SwitcherWorkspaceState::Hidden;
    };
    return std::ranges::to<std::vector>(rows | std::views::transform([&](WorkspaceRow const& row) {
                                            return window::SwitcherWorkspace {
                                                .ref = row.ref,
                                                .position = row.position,
                                                // Another daemon's name is the user's text: made fit for a
                                                // row as the sidebar makes its own (bidi disarmed, capped).
                                                .name = window::rowTextOf(row.name),
                                                .tabs = row.tabs,
                                                .state = stateOf(row),
                                                .status = row.status,
                                                .id = row.id,
                                            };
                                        }));
}

std::vector<window::SwitcherWorkspace> windowWorkspacesOf(
    std::span<WorkspaceRow const> rows,
    std::span<window::SidebarRow const> sidebarWorkspaces,
    vtworkspace::WorkspaceId window)
{
    auto workspaces = switcherWorkspacesOf(rows, window);
    for (auto& workspace: workspaces)
        if (auto const found = std::ranges::find_if(sidebarWorkspaces,
                                                    [&](window::SidebarRow const& row) {
                                                        return row.key.kind
                                                                   == window::SidebarRowKind::Workspace
                                                               && row.key.workspace == workspace.id;
                                                    });
            found != sidebarWorkspaces.end())
            workspace.status = window::switcherStatusOf(*found);
    return workspaces;
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
