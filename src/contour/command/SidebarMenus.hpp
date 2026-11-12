// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <contour/command/ContextMenu.hpp>
#include <contour/command/TabMenus.hpp>

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace contour::command
{

/// One tab a pane, a tab or a one-tab workspace can join, by the 1-based places MoveToTab names.
struct TabChoice
{
    std::uint32_t workspace = 0; ///< Its workspace's 1-based place in the list.
    std::uint32_t tab = 0;       ///< Its 1-based place among that workspace's tabs.
    std::string title;           ///< What the row says: the tab's label, or "Tab N".
};

/// The tabs of one workspace that can be joined, under that workspace's name.
struct TabChoiceGroup
{
    std::string workspace;       ///< The workspace's title, which heads its tabs.
    std::vector<TabChoice> tabs; ///< Its tabs that can be joined, in order.
};

/// What the window's host moves, as the sidebar's menus offer it: a row for a move the host would not make
/// is grayed out.
enum class MenuMoves : uint8_t
{
    Nodes = 0, ///< Panes and tabs, anywhere: this process, or a daemon serving MoveNode.
    Tabs,      ///< No node either, as every menu move goes by MoveNode: a daemon older than MoveNode.
    Nothing,   ///< Nothing: a tmux server.
};

/// The world, as a workspace row's context menu needs to see it.
struct WorkspaceMenuState
{
    std::size_t position = 0; ///< The workspace's place in the list, 0-based.
    std::size_t count = 1;    ///< How many workspaces the list holds.
    /// The tabs its only tab can join ("Merge into Tab"): empty for a workspace of several tabs, which joins
    /// nothing.
    std::vector<TabChoiceGroup> joinable {};
    MenuMoves moves = MenuMoves::Nodes; ///< What the window's host moves.
};

/// The workspace row's context menu for @p state.
/// @param state The workspace the menu was opened over.
/// @return The menu, top to bottom.
[[nodiscard]] std::vector<ContextMenuEntry> buildWorkspaceMenu(WorkspaceMenuState const& state);

/// One workspace a tab can be moved to.
struct WorkspaceChoice
{
    std::uint32_t position =
        0;             ///< The workspace's 1-based place in the list (what MoveTabToWorkspace names).
    std::string title; ///< What the row says: the workspace's label, or "Workspace N".
};

/// The world, as a tab row's context menu in the sidebar needs to see it.
struct SidebarTabMenuState
{
    TabContextMenuState tab {};              ///< The tab, as the tab strip's own menu sees it.
    std::vector<WorkspaceChoice> others {};  ///< The workspaces the tab can move to: every one but its own.
    std::vector<TabChoiceGroup> joinable {}; ///< The tabs it can merge into: every one but itself.
    /// How many tabs its workspace holds: a new workspace for its only one would only rebuild the one it has.
    std::size_t workspaceTabs = 1;
    MenuMoves moves = MenuMoves::Nodes; ///< What the window's host moves.
};

/// The tab row's context menu in the sidebar: the tab strip's menu, "Move to Workspace", "Merge into Tab" and
/// "Move to New Workspace".
/// @param state The tab the menu was opened over.
/// @return The menu, top to bottom.
[[nodiscard]] std::vector<ContextMenuEntry> buildSidebarTabMenu(SidebarTabMenuState const& state);

/// The world, as a pane row's context menu needs to see it.
struct SidebarPaneMenuState
{
    std::vector<TabChoiceGroup> joinable {}; ///< The tabs it can move to: every one but its own.
    /// How many panes its tab holds: a tab of its own for its only one would only rebuild the one it has.
    std::size_t tabPanes = 1;
    /// How many tabs its workspace holds: with tabPanes, whether the pane is all its workspace holds, which a
    /// new workspace for it would only rebuild.
    std::size_t workspaceTabs = 1;
    MenuMoves moves = MenuMoves::Nodes; ///< What the window's host moves.
};

/// The pane row's context menu.
/// @param state The pane the menu was opened over.
/// @return The menu, top to bottom.
[[nodiscard]] std::vector<ContextMenuEntry> buildSidebarPaneMenu(SidebarPaneMenuState const& state);

} // namespace contour::command
