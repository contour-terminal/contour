// SPDX-License-Identifier: Apache-2.0
#pragma once

/// @file
/// The switcher's row menu (right-click, Menu, Shift+F10 on a workspace row): "Switch to ‹name›" and "Open in
/// New Window" (only the latter where Return opens a new window), then, for a workspace of the window's own
/// daemon, the sidebar's workspace rows, acting on it through the sidebar's menu path. Another daemon's
/// workspaces are not changed from here. Pure and Qt-free.

#include <contour/command/ContextMenu.hpp>
#include <contour/command/Shortcut.hpp>
#include <contour/command/SidebarMenus.hpp>

#include <cstdint>
#include <optional>
#include <string>
#include <variant>
#include <vector>

namespace contour::command
{

/// What one of the menu's leading rows does to the workspace.
enum class SwitcherMenuChoice : uint8_t
{
    Switch = 0,      ///< Switch this window to it, as Return does.
    OpenInNewWindow, ///< Open it in a new window, as Shift+Return does.
};

/// Whose workspace the menu is about.
enum class SwitcherMenuOwner : uint8_t
{
    WindowDaemon = 0, ///< The daemon the window is on: the sidebar's rows apply.
    OtherDaemon,      ///< Another daemon: switch and open only.
};

/// What Return does to a workspace where the menu was opened.
enum class SwitcherMenuReturn : uint8_t
{
    Switch = 0, ///< Switch this window to it.
    Open,       ///< Open it in a new window: a pick an Open asked for.
};

/// Whether the window the switcher is over shows the workspace already.
enum class SwitcherMenuShown : uint8_t
{
    Elsewhere = 0, ///< Hidden, or shown by another window.
    ThisWindow,    ///< This window shows it: switching to it would do nothing.
};

/// The world, as a switcher row's menu needs to see it.
struct SwitcherRowMenuState
{
    std::string name {};                                       ///< The workspace's name.
    SwitcherMenuOwner owner = SwitcherMenuOwner::WindowDaemon; ///< Whose it is.
    WorkspaceMenuState workspace {}; ///< The sidebar's view of it; read for the window's daemon only.
    SwitcherMenuReturn returnMode = SwitcherMenuReturn::Switch; ///< What Return does.
    SwitcherMenuShown shown = SwitcherMenuShown::Elsewhere;     ///< Whether this window shows it.
};

/// One of the menu's leading rows: what Return or Shift+Return would do. It carries no action, so nothing but
/// the switcher can run it.
struct SwitcherChoiceRow
{
    SwitcherMenuChoice choice = SwitcherMenuChoice::Switch;  ///< What it does.
    std::string title {};                                    ///< What it says.
    std::string shortcut {};                                 ///< The key that does the same in the switcher.
    RowAvailability availability = RowAvailability::Enabled; ///< Whether it can be picked.

    bool operator==(SwitcherChoiceRow const&) const = default;
};

/// One row of the menu: a leading choice, or one of the sidebar's rows (with their action) or a separator.
using SwitcherMenuRow = std::variant<SwitcherChoiceRow, ContextMenuEntry>;

/// The switcher's row menu.
/// @param state The workspace the menu was opened over.
/// @param style How to spell the shortcuts.
/// @return The menu, top to bottom.
[[nodiscard]] std::vector<SwitcherMenuRow> buildSwitcherRowMenu(SwitcherRowMenuState const& state,
                                                                ShortcutStyle style);

} // namespace contour::command
