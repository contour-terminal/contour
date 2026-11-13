// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <contour/command/SidebarMenus.hpp>
#include <contour/window/DropZonePolicy.hpp>
#include <contour/window/SidebarTree.hpp>
#include <contour/window/WorkspaceDirectory.hpp>

#include <core/Utils.hpp>

#include <algorithm>
#include <cstdint>
#include <format>
#include <optional>
#include <ranges>
#include <span>
#include <vector>

namespace contour::window
{

/// The workspaces a tab of @p directory's selected workspace can move to: every other one.
/// @param directory What the window lists of its daemon's workspaces.
/// @return One choice per workspace but the selected one, in list order; each is titled with the
///         workspace's title (@see workspaceTitleOf).
[[nodiscard]] inline std::vector<command::WorkspaceChoice> moveTargetsOf(WorkspaceDirectory const& directory)
{
    auto choices = std::vector<command::WorkspaceChoice> {};
    for (auto const [index, workspace]: core::views::enumerate(directory.workspaces))
    {
        if (directory.selected == workspace.id)
            continue;
        choices.push_back(command::WorkspaceChoice {
            .position = static_cast<std::uint32_t>(index + 1),
            .title = workspaceTitleOf(workspace, static_cast<std::size_t>(index)) });
    }
    return choices;
}

/// The tabs a pane, a tab or a one-tab workspace can join: every tab of @p directory but @p excluded.
/// @param directory What the window lists.
/// @param excluded The tab the node is, or is in: joining it would move nothing anywhere.
/// @return One group per workspace with a tab to join, in list order, each headed by the workspace's title
///         (@see workspaceTitleOf); a tab is titled with its label, else "Tab N".
[[nodiscard]] inline std::vector<command::TabChoiceGroup> joinTargetsOf(WorkspaceDirectory const& directory,
                                                                        std::optional<TabRef> excluded)
{
    auto groups = std::vector<command::TabChoiceGroup> {};
    for (auto const [workspaceIndex, workspace]: core::views::enumerate(directory.workspaces))
    {
        auto group = command::TabChoiceGroup {
            .workspace = workspaceTitleOf(workspace, static_cast<std::size_t>(workspaceIndex)), .tabs = {}
        };
        for (auto const [tabIndex, tab]: core::views::enumerate(workspace.tabs))
        {
            if (excluded == TabRef { .workspace = workspace.id, .tab = tab.id })
                continue;
            group.tabs.push_back(command::TabChoice {
                .workspace = static_cast<std::uint32_t>(workspaceIndex + 1),
                .tab = static_cast<std::uint32_t>(tabIndex + 1),
                .title = tab.label.empty() ? std::format("Tab {}", tabIndex + 1) : tab.label });
        }
        if (!group.tabs.empty())
            groups.push_back(std::move(group));
    }
    return groups;
}

/// The world, as the workspace row's menu of @p workspace sees it: the sidebar's, and the switcher's for a
/// workspace of the window's own daemon.
/// @param directory What the window lists.
/// @param workspace The workspace's id.
/// @param moves What the window's host moves.
/// @return Its 0-based place among the workspaces and their count; for a workspace of one tab, the tabs that
///         tab can merge into. Place 0 for a workspace the directory does not list.
[[nodiscard]] inline command::WorkspaceMenuState workspaceMenuStateOf(WorkspaceDirectory const& directory,
                                                                      uint64_t workspace,
                                                                      command::MenuMoves moves)
{
    auto const holder = std::ranges::find(directory.workspaces, workspace, &DirectoryWorkspace::id);
    if (holder == directory.workspaces.end())
        return { .position = 0, .count = directory.workspaces.size(), .joinable = {}, .moves = moves };
    // A workspace of one tab merges into another tab as that tab does.
    return { .position = static_cast<std::size_t>(std::distance(directory.workspaces.begin(), holder)),
             .count = directory.workspaces.size(),
             .joinable = holder->tabs.size() == 1 ? joinTargetsOf(directory,
                                                                  TabRef { .workspace = holder->id,
                                                                           .tab = holder->tabs.front().id })
                                                  : std::vector<command::TabChoiceGroup> {},
             .moves = moves };
}

/// The tab @p workspace position @p tab position names, 1-based as MoveToTab names them.
/// @return The tab; none where either place is out of range.
[[nodiscard]] inline std::optional<TabRef> tabAtPlaces(WorkspaceDirectory const& directory,
                                                       std::uint32_t workspace,
                                                       std::uint32_t tab)
{
    if (workspace == 0 || workspace > directory.workspaces.size())
        return std::nullopt;
    auto const& holder = directory.workspaces[workspace - 1];
    if (tab == 0 || tab > holder.tabs.size())
        return std::nullopt;
    return TabRef { .workspace = holder.id, .tab = holder.tabs[tab - 1].id };
}

/// A tab a menu offers, by the 1-based places its MoveToTab row names and by its id: what the row meant when
/// the menu opened, whatever closes or moves while it is open.
struct OfferedTab
{
    std::uint32_t workspace = 0; ///< Its workspace's 1-based place in the list.
    std::uint32_t tab = 0;       ///< Its 1-based place among that workspace's tabs.
    TabRef ref;                  ///< The tab.
};

/// @return Every tab of @p directory, by its places and its id (@see OfferedTab).
[[nodiscard]] inline std::vector<OfferedTab> offeredTabsOf(WorkspaceDirectory const& directory)
{
    auto offered = std::vector<OfferedTab> {};
    for (auto const [workspaceIndex, workspace]: core::views::enumerate(directory.workspaces))
        for (auto const [tabIndex, tab]: core::views::enumerate(workspace.tabs))
            offered.push_back(OfferedTab { .workspace = static_cast<std::uint32_t>(workspaceIndex + 1),
                                           .tab = static_cast<std::uint32_t>(tabIndex + 1),
                                           .ref = TabRef { .workspace = workspace.id, .tab = tab.id } });
    return offered;
}

/// @return The tab @p offered lists at those 1-based places; none where it lists none.
[[nodiscard]] inline std::optional<TabRef> offeredTabAt(std::span<OfferedTab const> offered,
                                                        std::uint32_t workspace,
                                                        std::uint32_t tab)
{
    auto const found = std::ranges::find_if(
        offered, [&](OfferedTab const& entry) { return entry.workspace == workspace && entry.tab == tab; });
    return found != offered.end() ? std::optional { found->ref } : std::nullopt;
}

/// @param host What a window's host moves.
/// @return The same, as the sidebar's menus offer it.
[[nodiscard]] constexpr command::MenuMoves menuMovesOf(HostMoves host) noexcept
{
    switch (host)
    {
        case HostMoves::Nodes: break;
        case HostMoves::Tabs: return command::MenuMoves::Tabs;
        case HostMoves::Nothing: return command::MenuMoves::Nothing;
    }
    return command::MenuMoves::Nodes;
}

} // namespace contour::window
