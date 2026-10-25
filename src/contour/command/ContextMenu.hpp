// SPDX-License-Identifier: Apache-2.0
#pragma once

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

/// Whether a block has an output part yet: its command started (OSC 133;C).
enum class OutputPresence : std::uint8_t
{
    Absent = 0, ///< Still at its prompt.
    Present,    ///< Running or finished.
};

/// Whether a block's command line is known, which is what "Copy Command" copies.
enum class CommandLinePresence : std::uint8_t
{
    Absent = 0, ///< Neither reported nor recovered.
    Present,    ///< Reported by the shell (OSC 133;C cmdline_url), or recovered from the grid.
};

/// Whether the shell shows a prompt that a command typed now would run at.
enum class PromptPresence : std::uint8_t
{
    Absent = 0, ///< A command is running, or the shell marks no prompts at all.
    Present,    ///< The shell waits at its prompt.
};

/// The command block a right-click landed on, with what its rows may act on.
struct PointedBlock
{
    vtbackend::CommandBlockId id {};                               ///< The block; never zero.
    OutputPresence output = OutputPresence::Absent;                ///< Whether it has output to act on.
    CommandLinePresence commandLine = CommandLinePresence::Absent; ///< Whether its command line is known.
};

/// The world, as the terminal pane's context menu needs to see it.
///
/// A plain snapshot, taken once under the terminal lock at the moment of the right-click. It is the
/// dependency-injection seam of the menu: buildContextMenu() below is a pure function of it, so every row
/// the menu offers and every rule about when that row is shown or grayed out is decided — and tested —
/// with no terminal, no window, no clipboard and no Qt behind it.
struct ContextMenuState
{
    bool hasSelection = false;             ///< Some grid cells are selected.
    bool clipboardHasText = false;         ///< The system clipboard holds text.
    bool hasLastCommand = false;           ///< A finished OSC 133 command block sits in the scrollback.
    bool hasLocalWorkingDirectory = false; ///< OSC 7 reported a working directory on THIS host (openable).
    bool hasSplits = false;                ///< This tab holds more than one pane.
    bool inputProtected = false; ///< The user's read-only lock is on for this pane (not the host's KAM).
    /// Whether this build can speak, and this platform has a voice to speak with.
    ///
    /// A fact about the machine rather than about the pane, but it belongs in the snapshot for the same
    /// reason everything else does: the menu is a pure function of this struct, so asking the world
    /// from inside it would put an untestable branch in the one place that has none.
    bool canSpeak = false;

    /// The OSC 8 hyperlink the user right-clicked, empty if there was none.
    ///
    /// The URI itself and not merely a bool, because the rows it gates must also CARRY it. The terminal's
    /// idea of "the hyperlink under the cursor" tracks the live mouse position, and the mouse leaves the
    /// clicked cell the instant it travels to the menu row the user is reaching for — so an action that
    /// asked again at click time would answer about a cell nobody clicked.
    std::string hyperlinkUnderCursor;

    /// The detected link the user right-clicked, resolved far enough to open (a path only once confirmed);
    /// nullopt where there was none, or an OSC 8 link was there. Carried by the rows, for the reason
    /// hyperlinkUnderCursor is.
    std::optional<vtbackend::LinkTarget> linkUnderCursor {};

    /// The right-clicked grid line, when a fold covers it -- nullopt means there is nothing to toggle.
    ///
    /// One optional rather than a marker plus a line, so "there is a fold" and "which line it is on"
    /// cannot disagree. Captured at click time for the same reason the hyperlink above is: the pointer
    /// leaves that cell the moment the user reaches for the menu row, so a row that asked again would
    /// answer about a line nobody clicked.
    std::optional<vtbackend::LineOffset> foldLine;

    /// The command block whose row was right-clicked; nullopt when the click was on no block's row.
    ///
    /// One optional rather than an id with a zero sentinel beside its facts, for the reason foldLine is
    /// one. Captured at click time and carried by the block rows, because the pointer leaves the block
    /// for the menu the moment the user reaches for a row.
    std::optional<PointedBlock> blockUnderCursor {};
    /// Whether the shell waits at its prompt, where a command it is handed would run.
    PromptPresence livePrompt = PromptPresence::Absent;
    /// Whether the last finished block's command line is known, which is what "Copy Last Command" copies.
    CommandLinePresence lastCommandLine = CommandLinePresence::Absent;

    std::string activeProfile;             ///< The profile this session runs under.
    std::vector<std::string> profileNames; ///< Every configured profile, in a stable order.
    std::vector<std::string> hostNames {}; ///< Saved hosts (`hosts:`), sorted.
};

/// What a menu row IS: something to run, a divider, or a submenu that holds more rows.
enum class ContextMenuEntryKind : std::uint8_t
{
    Command,
    Separator,
    Submenu,
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

/// One row of the menu, already resolved against a ContextMenuState.
///
/// This is what the GUI renders. The action travels WITH the row rather than being looked up again by
/// name later, which is what keeps the menu from depending on any registry being populated first, and
/// what lets a click act on the pane it was opened over rather than on whichever pane happens to be
/// active by the time the click lands.
struct ContextMenuEntry
{
    ContextMenuEntryKind kind = ContextMenuEntryKind::Command;
    std::string title;                                       ///< Display text.
    actions::Action action {};                               ///< Command rows: what to run.
    RowAvailability availability = RowAvailability::Enabled; ///< Command rows: whether it can be picked.
    RowCheck check = RowCheck::None;        ///< Command rows: the check column, and whether it is ticked.
    MenuItemRole role = MenuItemRole::None; ///< Command rows: the role a native menu gives it.
    std::optional<vtbackend::RGBColor> swatch {}; ///< Command rows drawn as a colour swatch.
    std::vector<ContextMenuEntry> children;       ///< Submenu rows: what is inside.
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
