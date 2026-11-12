// SPDX-License-Identifier: Apache-2.0
#pragma once
/// @file
/// The workspace sidebar's rows, as pure data. sidebarRows() flattens what a window lists of its workspaces
/// (WorkspaceDirectory), this window's seen ledger (TabAttention), the sidebar's own state (what is
/// collapsed, the filter) and the wall clock into the rows a view draws: workspaces, their tabs, and the
/// panes of the split ones, each with its title, reason line, time, status glyph, rolled-up urgency,
/// expansion, selection and the tree lines the Terminal look draws. Qt-free: every row's text is plain
/// text, and a view must render it as such, never as markup.
///
/// What a pane asks is decided by paneUrgency() (AttentionJump.hpp), in the one order of StatusUrgency.hpp;
/// a parent row shows the most urgent of its panes, with that pane's glyph, reason and time. A Failed or
/// DoneUnseen pane whose verdict is a finished command has a reason (`exit 1 · 18s`) but no time:
/// vthost::proto::FinishedCommand carries no timestamp to measure from.
#include <contour/window/PaneKey.hpp>
#include <contour/window/StatusKinds.hpp>
#include <contour/window/StatusUrgency.hpp>
#include <contour/window/TabAttention.hpp>
#include <contour/window/WorkspaceDirectory.hpp>

#include <array>
#include <chrono>
#include <compare>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <set>
#include <span>
#include <string>
#include <vector>

namespace contour::window
{

/// What a sidebar row stands for.
enum class SidebarRowKind : uint8_t
{
    Workspace = 0, ///< A workspace.
    Tab,           ///< A tab of a workspace.
    Pane,          ///< One pane of a tab with more than one.
};

/// Identifies a row across rebuilds, so a view keeps its selection and focus through a refresh.
struct SidebarRowKey
{
    SidebarRowKind kind = SidebarRowKind::Workspace; ///< What the row stands for.
    uint64_t workspace = 0;                          ///< The workspace's daemon id.
    uint64_t tab = 0;                                ///< The tab's id; 0 for a workspace row.
    PaneKey pane;                                    ///< The pane's key; zero for a workspace or tab row.

    auto operator<=>(SidebarRowKey const&) const = default;
};

/// What a row's right-hand side shows, which decides how often it is re-formatted.
enum class TimeKind : uint8_t
{
    None = 0, ///< Nothing.
    Live,     ///< Time spent in the current state (`4:12`); re-formatted every second.
    Relative, ///< Time since it ended (`2m ago`); re-formatted every second.
    Progress, ///< The percentage the program reports; wins over Live.
};

/// The right-hand side of a row.
struct TimeText
{
    TimeKind kind = TimeKind::None; ///< How the text is kept current.
    std::string text;               ///< What to draw.
    std::optional<uint8_t> percent; ///< Progress: the percent done, for the meter.

    bool operator==(TimeText const&) const = default;
};

/// Whether a row folds its children away.
enum class RowExpansion : uint8_t
{
    None = 0,  ///< A row with nothing to expand.
    Collapsed, ///< Its children are hidden.
    Expanded,  ///< Its children follow it.
};

/// Whether a row is what the window shows.
enum class RowSelection : uint8_t
{
    None = 0,
    Selected, ///< The tab this window displays; panes are not marked.
};

/// A tree connector in one column of the Terminal look's tree.
enum class TreeGlyph : uint8_t
{
    None = 0, ///< Blank.
    Tee,      ///< `├`: a child with later siblings.
    Last,     ///< `└`: the last child.
    Bar,      ///< `│`: an ancestor's later siblings pass by.
};

/// The connectors of a row: column @c i belongs to depth @c i+1. A row of depth @c d fills columns 0..d-1:
/// the ancestors' columns hold Bar or None, the row's own (the last) holds Tee or Last. A workspace row
/// has none.
struct TreeLines
{
    std::array<TreeGlyph, 2> columns {};

    bool operator==(TreeLines const&) const = default;
};

/// How many panes under a row ask for something, counted before any filter or folding hides them.
struct LeafCounts
{
    uint32_t needYou = 0; ///< Panes at Urgency::NeedsYou.
    uint32_t working = 0; ///< Panes at Urgency::Working.

    bool operator==(LeafCounts const&) const = default;
};

/// One row of the sidebar.
struct SidebarRow
{
    SidebarRowKey key;                           ///< Identifies it across rebuilds.
    uint8_t depth = 0;                           ///< 0 workspace, 1 tab, 2 pane.
    std::string title;                           ///< The name; plain text.
    std::string reason;                          ///< The second line; empty when there is nothing to say.
    std::string gitText;                         ///< Workspace rows: `feat/billing ↑2 ↓1`.
    TimeText time;                               ///< The right-hand side.
    Urgency urgency = Urgency::Idle;             ///< Rolled up over the panes beneath a parent row.
    TabBadgeKind glyph = TabBadgeKind::None;     ///< Which badge look draws it (chromeStyle.badgeLook).
    RowExpansion expansion = RowExpansion::None; ///< Whether its children are shown.
    RowSelection selection = RowSelection::None; ///< Whether this window displays it (tab rows only).
    /// Its real place among its siblings in the directory, whatever a filter or a fold hides: a workspace's
    /// among the workspaces, a tab's among its workspace's tabs, a pane's among its tab's panes. A drop
    /// resolves to these, never to an index among the rows shown.
    std::size_t position = 0;
    TreeLines lines;   ///< The Terminal look's tree connectors.
    LeafCounts leaves; ///< The panes beneath it, for the footer.

    bool operator==(SidebarRow const&) const = default;
};

/// A tab, named by its workspace: a tab id is unique only within its workspace.
struct TabRef
{
    uint64_t workspace = 0; ///< The workspace's daemon id.
    uint64_t tab = 0;       ///< The tab's id within it.

    auto operator<=>(TabRef const&) const = default;
};

/// What the sidebar itself remembers.
struct SidebarState
{
    std::set<uint64_t> collapsedWorkspaces {}; ///< Workspace ids whose tabs are hidden.
    std::set<TabRef> collapsedTabs {};         ///< Tabs whose panes are hidden.
    /// A case-insensitive (ASCII) substring a row must hold in its title, reason or git text, or have a
    /// descendant that does. While it is set, every ancestor of a match is expanded. Empty: no filter.
    std::string filter {};
};

/// The sidebar's footer: `N need you · M working`.
struct SidebarFooter
{
    uint32_t needYou = 0; ///< Panes that need the user (blocked or a bell).
    uint32_t working = 0; ///< Panes at work.

    bool operator==(SidebarFooter const&) const = default;
};

/// The longest text a row holds (title or reason), in characters, the ellipsis included.
inline constexpr std::size_t MaxRowTextLength = 120;

/// Flattens @p directory into the rows the sidebar draws, depth-first.
/// @param directory The workspaces, tabs and panes.
/// @param attention This window's seen-state.
/// @param state What is collapsed, and the filter.
/// @param now The wall clock the times are measured against.
/// @param displayedTab The tab this window shows, if any.
/// @return The rows. A tab has pane rows only when it has more than one pane.
[[nodiscard]] std::vector<SidebarRow> sidebarRows(WorkspaceDirectory const& directory,
                                                  TabAttention const& attention,
                                                  SidebarState const& state,
                                                  std::chrono::system_clock::time_point now,
                                                  std::optional<TabRef> displayedTab);

/// The footer over @p rows.
/// @param rows Rows from sidebarRows(); build them with an empty filter for a count of everything.
/// @return The panes that need the user and that work, counted over the leaves under each workspace row
///         (LeafCounts), not over the rolled-up urgencies.
[[nodiscard]] SidebarFooter sidebarFooter(std::span<SidebarRow const> rows);

/// Time spent in a state.
/// @param elapsed How long; below zero reads as zero.
/// @return `0:42`, `4:12` or `1:02:03`.
[[nodiscard]] std::string formatLive(std::chrono::seconds elapsed);

/// Time since something ended.
/// @param elapsed How long ago; below zero reads as zero.
/// @return `now` (under 10 s), `42s ago`, `2m ago`, `3h ago` or `2d ago`.
[[nodiscard]] std::string formatRelative(std::chrono::seconds elapsed);

} // namespace contour::window
