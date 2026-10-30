// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <contour/command/BlockMenu.hpp>

#include <vtbackend/core/Primitives.hpp>
#include <vtbackend/links/LinkTypes.hpp>

#include <optional>
#include <string>
#include <vector>

namespace contour::command
{

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

    /// The command block whose row was right-clicked; nullopt when the click was on no block's row. Its
    /// rows and header head the menu (buildBlockMenu()).
    ///
    /// One optional rather than an id with a zero sentinel beside its facts, for the reason foldLine is
    /// one. Captured at click time and carried by the block rows, because the pointer leaves the block
    /// for the menu the moment the user reaches for a row.
    std::optional<BlockMenuState> blockUnderCursor {};
    /// Whether the last finished block's command line is known, which is what "Copy Last Command" copies.
    CommandLinePresence lastCommandLine = CommandLinePresence::Absent;

    std::string activeProfile;             ///< The profile this session runs under.
    std::vector<std::string> profileNames; ///< Every configured profile, in a stable order.
    std::vector<std::string> hostNames {}; ///< Saved hosts (`hosts:`), sorted.
};

} // namespace contour::command
