// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <contour/command/ContextMenuState.hpp>
#include <contour/config/Actions.hpp>

#include <vtbackend/core/Color.hpp>
#include <vtbackend/core/CommandBlockId.hpp>
#include <vtbackend/core/Primitives.hpp>
#include <vtbackend/links/LinkTypes.hpp>

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace contour::command
{

/// What a menu row IS: something to run, a divider, a submenu that holds more rows, or a line of text that
/// heads a menu.
enum class ContextMenuEntryKind : std::uint8_t
{
    Command,
    Separator,
    Submenu,
    Header, ///< A disabled line of text with no action: a block menu's facts.
};

/// Whether a command row can be picked, or is shown grayed out.
enum class RowAvailability : std::uint8_t
{
    Enabled = 0, ///< The row can be picked.
    Disabled,    ///< The row is in the menu but grayed out.
};

/// Whether a command row draws a check column, and if so whether the check is set.
enum class RowCheck : std::uint8_t
{
    None = 0,  ///< No check column.
    Unchecked, ///< A check column, not ticked.
    Checked,   ///< A check column, ticked.
};

/// The role a native menu gives a row, so the platform can move it where its conventions put it.
enum class MenuItemRole : std::uint8_t
{
    None = 0,    ///< An ordinary row.
    About,       ///< The "About" row.
    Preferences, ///< The "Preferences" / "Settings" row.
    Quit,        ///< The "Quit" row.
    Minimize,    ///< The Window menu's "Minimize" row, which answers to the platform's minimize key.
};

/// What a command row acts on, which decides where it runs when no window holds focus.
///
/// Only the menu bar asks: it outlives every window and stays open with none focused (after ⌘M, or with
/// another application in front), where a pane's or a tab's menu always has the pane or tab it opened over.
enum class ActionScope : std::uint8_t
{
    Window = 0,  ///< Acts on a window or its terminal: nothing to act on without a focused window.
    Application, ///< Acts on the application: runs in some window if one is left, or without one.
};

/// One row of the menu, already resolved against a ContextMenuState.
///
/// This is what the GUI renders. The action travels WITH the row rather than being looked up again by
/// name later, which is what keeps the menu from depending on any registry being populated first, and
/// what lets a click act on the pane it was opened over rather than on whichever pane happens to be
/// active by the time the click lands.
struct ContextMenuEntry
{
    ContextMenuEntryKind kind = ContextMenuEntryKind::Command;
    std::string title {};                                    ///< Display text.
    actions::Action action {};                               ///< Command rows: what to run.
    RowAvailability availability = RowAvailability::Enabled; ///< Command rows: whether it can be picked.
    RowCheck check = RowCheck::None;        ///< Command rows: the check column, and whether it is ticked.
    MenuItemRole role = MenuItemRole::None; ///< Command rows: the role a native menu gives it.
    std::optional<vtbackend::RGBColor> swatch {}; ///< Command rows drawn as a color swatch.
    std::vector<ContextMenuEntry> children {};    ///< Submenu rows: what is inside.
    ActionScope scope = ActionScope::Window;      ///< Command rows: what the row acts on.
};

/// The context menu for @p state.
///
/// Filters the menu template against the state: rows whose precondition is not met are dropped (a
/// "Copy Last Command Output" with no shell integration behind it would only ever mislead), rows whose
/// precondition is merely unsatisfied right now are grayed out, and the separators that dropping rows
/// left stranded are collapsed away.
///
/// @param state The pane the menu was opened over.
/// @return The menu, top to bottom.
[[nodiscard]] std::vector<ContextMenuEntry> buildContextMenu(ContextMenuState const& state);

} // namespace contour::command
