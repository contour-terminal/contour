// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <contour/command/ContextMenu.hpp>
#include <contour/command/TabMenus.hpp>

#include <vtbackend/core/Color.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
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

/// Whether the window's host keeps a workspace's colour, as the workspace menu offers it: the "Workspace
/// Color" submenu is grayed out where a colour would do nothing.
enum class MenuColoring : uint8_t
{
    Unavailable = 0, ///< No: a window listed by a daemon that predates the colour.
    Available,       ///< Yes: a window that lists its own workspaces, or a daemon that keeps the colour.
};

/// Whether a row's menu offers "Open in New Window".
enum class OpenOffer : uint8_t
{
    NotOffered = 0, ///< Not offered: a row of a `link:` daemon, or of no daemon, which no switch can name.
    Offered,        ///< Offered.
};

/// Whose row a menu was opened over.
enum class RowOrigin : uint8_t
{
    Own = 0,     ///< The window's own daemon, or a window with no daemon.
    OtherDaemon, ///< Another listed daemon: verbs go through its section, or jump there first.
};

/// How a sidebar menu verb runs on another daemon's row (spec §9).
enum class OtherDaemonRoute : uint8_t
{
    NotOffered = 0, ///< Not on another daemon's row (moves, merges, the tab strip's bulk closes).
    InPlace,        ///< Through that daemon's section, the window staying where it is.
    JumpFirst,      ///< Jump to the row, then run it there.
};

/// What a verb on another daemon's row needs of that daemon's section.
enum class SectionNeed : uint8_t
{
    None = 0, ///< Nothing: a jump, which reaches the daemon itself.
    Live,     ///< A connected section, which carries it to the daemon: on a last-known row it is disabled.
};

/// Whether another daemon's row says what its daemon says now, or what was last known of it.
enum class RowFreshness : uint8_t
{
    Live = 0,  ///< Its daemon's section is connected.
    LastKnown, ///< It is not: the row is what was last known, and nothing carries a verb through its section.
};

/// One row of the route table: an action, by its alternative, and how it runs on another daemon's row.
struct OtherDaemonRouteRow
{
    actions::Action action;                                ///< The action; only its alternative counts.
    OtherDaemonRoute route = OtherDaemonRoute::NotOffered; ///< How it runs there.
    SectionNeed need = SectionNeed::None;                  ///< What it needs of the daemon's section.
};

/// @return The route table, one row per action a sidebar menu offers, each action once.
[[nodiscard]] std::span<OtherDaemonRouteRow const> otherDaemonRoutes();

/// @param action A sidebar menu's action.
/// @return How @p action runs on another daemon's row; NotOffered for any action the table does not name.
[[nodiscard]] OtherDaemonRoute otherDaemonRouteOf(actions::Action const& action) noexcept;

/// @param action A sidebar menu's action.
/// @return What @p action needs of another daemon's section; None for any action the table does not name.
[[nodiscard]] SectionNeed sectionNeedOf(actions::Action const& action) noexcept;

/// Whether a daemon row's menu offers "New Workspace".
enum class NewWorkspaceOffer : uint8_t
{
    NotOffered = 0, ///< Not offered: a `link:` daemon, which no switch can name, or one not reachable now.
    Offered, ///< Offered: on the window's own daemon, a new workspace; on another, made there, then jumped
             ///< to.
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
    /// The colours the "Workspace Color" submenu offers, in order (the tab menu's palette).
    std::vector<vtbackend::RGBColor> palette {};
    /// The colour the user gave the workspace; none while it is uncoloured.
    std::optional<vtbackend::RGBColor> color {};
    MenuColoring coloring = MenuColoring::Available; ///< Whether the window's host keeps the colour.
    OpenOffer open = OpenOffer::Offered;             ///< Whether "Open in New Window" is offered.
    RowOrigin origin = RowOrigin::Own; ///< Whose row: another daemon's offers only what routes there.
    /// Whether another daemon's row is what was last known of it: what goes through its section is disabled.
    RowFreshness freshness = RowFreshness::Live;
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
    MenuMoves moves = MenuMoves::Nodes;  ///< What the window's host moves.
    OpenOffer open = OpenOffer::Offered; ///< Whether "Open in New Window" is offered.
    RowOrigin origin = RowOrigin::Own;   ///< Whose row: another daemon's offers only what routes there.
    /// Whether another daemon's row is what was last known of it: what goes through its section is disabled.
    RowFreshness freshness = RowFreshness::Live;
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
    MenuMoves moves = MenuMoves::Nodes;  ///< What the window's host moves.
    OpenOffer open = OpenOffer::Offered; ///< Whether "Open in New Window" is offered.
    RowOrigin origin = RowOrigin::Own;   ///< Whose row: another daemon's offers only what routes there.
    /// Whether another daemon's row is what was last known of it: what goes through its section is disabled.
    RowFreshness freshness = RowFreshness::Live;
};

/// The pane row's context menu.
/// @param state The pane the menu was opened over.
/// @return The menu, top to bottom.
[[nodiscard]] std::vector<ContextMenuEntry> buildSidebarPaneMenu(SidebarPaneMenuState const& state);

/// What one of a daemon row's buttons does, as its menu offers it: the command layer's mirror of
/// window::DaemonRowButton, which this layer cannot name (window::sidebarDaemonMenuStateOf() ties the two).
enum class DaemonRowVerb : uint8_t
{
    None = 0, ///< No button in this place.
    Connect,  ///< Connects to the daemon (ConnectDaemon).
    Start,    ///< Starts the local daemon (StartDaemon).
    Cancel,   ///< Ends what is under way there: a pending jump into it, or a connect (CancelSwitch).
    Remove,   ///< Unlists the daemon for the rest of the run (RemoveDaemon).
};

/// Whether a daemon row's menu offers "Hide from Sidebar".
enum class HideOffer : uint8_t
{
    NotOffered = 0, ///< Not offered: a window of this process is on the daemon, or it is a `link:` daemon.
    Offered,        ///< Offered: the daemon is only observed, and a choice kept for it names it next run too.
};

/// What a daemon row's Cancel ends, which its menu row names.
enum class DaemonCancelTarget : uint8_t
{
    Connect = 0, ///< The connect under way.
    Jump,        ///< The pending jump into the daemon's section.
};

/// The world, as a daemon row's context menu needs to see it.
struct SidebarDaemonMenuState
{
    /// The row's buttons as verbs, in their order on the row; None for a place with no button.
    std::array<DaemonRowVerb, 2> verbs {};
    /// Not offered for a `link:` daemon, which no switch can name, nor for one that cannot be reached now.
    OpenOffer open = OpenOffer::Offered;
    DaemonCancelTarget cancels = DaemonCancelTarget::Connect; ///< What its Cancel ends, if it offers one.
    /// "New Workspace", above the button verbs: on the window's own daemon, or made on another and jumped to.
    NewWorkspaceOffer newWorkspace = NewWorkspaceOffer::Offered;
    /// "Hide from Sidebar", set apart with "Remove from Sidebar".
    HideOffer hide = HideOffer::NotOffered;
};

/// What a daemon row verb's menu row says: the verb in full, as the button's tooltip says it too.
/// @param verb    The verb.
/// @param cancels What a Cancel ends, which it names.
/// @return Its title; empty for None.
[[nodiscard]] std::string_view daemonRowVerbTitle(DaemonRowVerb verb, DaemonCancelTarget cancels) noexcept;

/// The daemon row's context menu: "Open in New Window", "New Workspace", then what the row's buttons do, in
/// their order, and "Hide from Sidebar" and "Remove from Sidebar" set apart.
/// @param state The daemon row the menu was opened over.
/// @return The menu, top to bottom.
[[nodiscard]] std::vector<ContextMenuEntry> buildSidebarDaemonMenu(SidebarDaemonMenuState const& state);

/// The Hidden row's context menu: one "Show ‹name›" per hidden daemon, in order, each running
/// ShowHiddenDaemon with its 1-based index.
/// @param names The hidden daemons' names, in the Hidden row's order.
/// @return The menu, top to bottom.
[[nodiscard]] std::vector<ContextMenuEntry> buildSidebarHiddenMenu(std::span<std::string const> names);

} // namespace contour::command
