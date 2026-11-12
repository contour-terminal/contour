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

/// The world, as a workspace row's context menu needs to see it.
struct WorkspaceMenuState
{
    std::size_t position = 0; ///< The workspace's place in the list, 0-based.
    std::size_t count = 1;    ///< How many workspaces the list holds.
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
    TabContextMenuState tab {};             ///< The tab, as the tab strip's own menu sees it.
    std::vector<WorkspaceChoice> others {}; ///< The workspaces the tab can move to: every one but its own.
};

/// The tab row's context menu in the sidebar: the tab strip's menu, and "Move to Workspace".
/// @param state The tab the menu was opened over.
/// @return The menu, top to bottom.
[[nodiscard]] std::vector<ContextMenuEntry> buildSidebarTabMenu(SidebarTabMenuState const& state);

/// The world, as a pane row's context menu needs to see it. A pane's menu has no conditional rows today; the
/// state exists so that one can be added as a row of the table, not as a signature change.
struct SidebarPaneMenuState
{
};

/// The pane row's context menu.
/// @param state The pane the menu was opened over.
/// @return The menu, top to bottom.
[[nodiscard]] std::vector<ContextMenuEntry> buildSidebarPaneMenu(SidebarPaneMenuState const& state);

} // namespace contour::command
