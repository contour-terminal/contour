// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <contour/command/ContextMenu.hpp>
#include <contour/config/TabBarMode.hpp>

#include <string>
#include <vector>

namespace contour::command
{

/// The world, as the title bar's context menu needs to see it.
///
/// A separate snapshot from ContextMenuState, not a widening of it. The two menus answer to different
/// worlds: the terminal pane's asks about selections, clipboards and hyperlinks; this one asks about
/// tab counts and tab bar modes. One struct covering both would make every test of either populate the
/// other's fields, and would turn each doc comment into a lie about half its members.
struct TitleBarContextMenuState
{
    int tabCount = 0; ///< Tabs in THIS window.

    /// The window's live tab bar mode and placement, so the menu can tick the one in force.
    ///
    /// The WINDOW's, not the configuration's: the menu sets a runtime override, so a window that has
    /// been changed since startup must show what it is actually doing.
    config::TabBarVisibility tabBarVisibility = config::TabBarVisibility::Always;
    config::TabBarPosition tabBarPosition = config::TabBarPosition::Top;

    std::string activeProfile;             ///< The profile the active session runs under.
    std::vector<std::string> profileNames; ///< Every configured profile, in a stable order.
    std::vector<std::string> hostNames {}; ///< Saved hosts (`hosts:`), sorted.
};

/// The world, as the tab bar menu needs to see it: the window's live mode and placement.
struct TabBarMenuState
{
    config::TabBarVisibility visibility = config::TabBarVisibility::Always; ///< The mode in force.
    config::TabBarPosition position = config::TabBarPosition::Top;          ///< The placement in force.
};

/// The tab bar menu for @p state: a checkable row per visibility mode, a separator, then "Position".
/// @param state The modes in force.
/// @return The menu, top to bottom.
[[nodiscard]] std::vector<ContextMenuEntry> buildTabBarMenu(TabBarMenuState const& state);

/// The title bar's context menu for @p state.
///
/// @param state The window the menu was opened over.
/// @return The menu, top to bottom.
[[nodiscard]] std::vector<ContextMenuEntry> buildTitleBarContextMenu(TitleBarContextMenuState const& state);

} // namespace contour::command
