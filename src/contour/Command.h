// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <contour/Actions.h>

#include <string>
#include <string_view>
#include <vector>

namespace contour
{

/// One entry the command palette can offer.
///
/// A Command is an action made CONCRETE and nameable: the action itself plus the identity, title and
/// description a user picks it by. Several Commands may share an action kind — "Switch To Tab 1" and
/// "Switch To Tab 2" are two Commands over one SwitchToTab — which is exactly why the palette deals
/// in Commands and not in actions.
struct Command
{
    std::string id;    ///< Stable identity, persisted in the MRU. "SplitVertical", "ChangeProfile:dark".
    std::string title; ///< Display name, e.g. "Split Vertical", "Change Profile: dark".
    std::string description; ///< What it does; from the action catalog.
    actions::Action action;  ///< What to run.
};

/// A contributor of commands to the palette.
///
/// This is the dependency-injection seam of the palette: the action catalog, the user's key bindings
/// and the live application state (configured profiles, saved layouts, open tabs) are each just a
/// source. The palette itself knows none of them — it merges whatever sources it was handed. A sixth
/// kind of command is therefore a new class, not an edit to the palette.
class CommandSource
{
  public:
    CommandSource() = default;
    CommandSource(CommandSource&&) = default;
    CommandSource(CommandSource const&) = default;
    CommandSource& operator=(CommandSource&&) = default;
    CommandSource& operator=(CommandSource const&) = default;
    virtual ~CommandSource() = default;

    /// The commands this source currently offers.
    ///
    /// Re-queried every time the palette opens, so a source backed by live state (tabs, profiles)
    /// reports what is true NOW rather than what was true at startup.
    [[nodiscard]] virtual std::vector<Command> commands() const = 0;
};

/// The arguments an action carries, rendered for both of the ways the palette needs them.
///
/// Two facets, one visitor: an id must DISTINGUISH ("SwitchToTab:3" is not "SwitchToTab:4") and a
/// title must READ ("Switch To Tab 3"). Producing them from a single traversal is what keeps the two
/// from drifting — a new argument that reaches the id but not the title would give the user two
/// identical-looking rows that behave differently.
struct CommandArguments
{
    std::string id;    ///< Appended to the action's name after a ':'. Empty when the action carries none.
    std::string title; ///< Appended to the display title verbatim. Empty when the action carries none.
};

/// The arguments @p action carries.
///
/// @param action The action to inspect.
/// @return Its arguments, or two empty strings when it has none (the common case: most actions are
///         empty structs, and their id is simply their name).
[[nodiscard]] CommandArguments commandArguments(actions::Action const& action);

/// The stable identity of the command that runs @p action — the string persisted in the MRU.
///
/// It is the action's catalog name, plus its arguments when it carries any: "SplitVertical",
/// "ChangeProfile:dark", "SwitchToTab:3". Two actions get the same id exactly when running either
/// would do the same thing.
///
/// @param action The action to identify.
/// @return Its command id.
[[nodiscard]] std::string commandId(actions::Action const& action);

/// The display title of the command that runs @p action, e.g. "Split Vertical", "Switch To Tab 3".
///
/// Derived from the catalog name rather than authored, so there is no separate title table to keep
/// in sync with the actions: the name is split at its camel-case humps ("SplitVertical" -> "Split
/// Vertical"), acronym runs are kept whole ("ScreenshotVT" -> "Screenshot VT"), and the action's
/// arguments are appended.
///
/// @param action The action to title.
/// @return Its display title.
[[nodiscard]] std::string commandTitle(actions::Action const& action);

/// Splits an upper-camel-case identifier into space-separated words.
///
/// A space is inserted before a capital that starts a word — one following a lower-case letter
/// ("SplitVertical" -> "Split Vertical"), or one that ends a run of capitals by being followed by a
/// lower-case letter ("VTMode" -> "VT Mode"). A capital in the middle of a run is left alone, which
/// is what keeps acronyms intact ("ScreenshotVT" -> "Screenshot VT", not "Screenshot V T").
///
/// @param identifier The identifier to split, e.g. "ToggleSplitOrientation".
/// @return Its words, space-separated, e.g. "Toggle Split Orientation".
[[nodiscard]] std::string splitCamelCase(std::string_view identifier);

} // namespace contour
