// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <contour/Actions.h>

#include <cstdint>
#include <string>
#include <vector>

namespace contour
{

/// The world, as the terminal pane's context menu needs to see it.
///
/// A plain snapshot, taken once under the terminal lock at the moment of the right-click. It is the
/// dependency-injection seam of the menu: buildContextMenu() below is a pure function of it, so every row
/// the menu offers and every rule about when that row is shown or grayed out is decided — and tested —
/// with no terminal, no window, no clipboard and no Qt behind it.
struct ContextMenuState
{
    bool hasSelection = false;        ///< Some grid cells are selected.
    bool clipboardHasText = false;    ///< The system clipboard holds text.
    bool hasLastCommand = false;      ///< A finished OSC 133 command block sits in the scrollback.
    bool hasWorkingDirectory = false; ///< The shell reported a working directory (OSC 7).
    bool hasSplits = false;           ///< This tab holds more than one pane.

    /// The OSC 8 hyperlink the user right-clicked, empty if there was none.
    ///
    /// The URI itself and not merely a bool, because the rows it gates must also CARRY it. The terminal's
    /// idea of "the hyperlink under the cursor" tracks the live mouse position, and the mouse leaves the
    /// clicked cell the instant it travels to the menu row the user is reaching for — so an action that
    /// asked again at click time would answer about a cell nobody clicked.
    std::string hyperlinkUnderCursor;

    std::string activeProfile;             ///< The profile this session runs under.
    std::vector<std::string> profileNames; ///< Every configured profile, in a stable order.
};

/// What a menu row IS: something to run, a divider, or a submenu that holds more rows.
enum class ContextMenuEntryKind : std::uint8_t
{
    Command,
    Separator,
    Submenu,
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
    std::string title;                      ///< Display text.
    actions::Action action {};              ///< Command rows: what to run.
    bool enabled = true;                    ///< Command rows: whether it can be picked.
    bool checkable = false;                 ///< Command rows: whether it draws a check column.
    bool checked = false;                   ///< Checkable rows: whether the check is set.
    std::vector<ContextMenuEntry> children; ///< Submenu rows: what is inside.
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

} // namespace contour
