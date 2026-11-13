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
#include <string_view>
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

/// Where the keyboard goes once a row has been activated (Enter on it).
enum class ActivationFocus : uint8_t
{
    Stay = 0, ///< It stays on the tree, so the next key moves the row cursor.
    Terminal, ///< It goes to the terminal the activation shows, ready to type, as a click on the row leaves
              ///< it.
};

/// @param kind What the activated row stands for.
/// @return Where the keyboard goes after the activation. Every row kind shows something in the window --
///         a workspace selects it, a tab or a pane shows its pane -- so each hands the keyboard to the
///         terminal, which is what a pointer press on the row does already (the chrome focus ends on it).
[[nodiscard]] constexpr ActivationFocus focusAfterActivation(SidebarRowKind kind) noexcept
{
    switch (kind)
    {
        case SidebarRowKind::Workspace:
        case SidebarRowKind::Tab:
        case SidebarRowKind::Pane: return ActivationFocus::Terminal;
    }
    return ActivationFocus::Stay;
}

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
    Selected,         ///< The tab this window displays; panes are not marked.
    ContainsSelected, ///< A folded workspace holding that tab: the selection moves to the nearest row shown.
};

/// A tree connector in one column of the Terminal look's tree.
enum class TreeGlyph : uint8_t
{
    None = 0, ///< Blank.
    Tee,      ///< `├`: a child with later siblings.
    Last,     ///< `└`: the last child.
    Bar,      ///< `│`: an ancestor's later siblings pass by.
};

/// A reason in its two parts: the words, set in the proportional face, and the command, set in the
/// monospace face after them. A plain reason (`All tests pass`) is all label.
struct ReasonParts
{
    std::string label;     ///< The words (`exit 1 · 2s`, `Permission · Bash: `).
    std::string separator; ///< Between the words and the command, and gone with it: ` · `, or empty.
    std::string command;   ///< The command the words are about; empty for a plain reason.

    /// @return The reason as one line, label then command.
    [[nodiscard]] std::string joined() const { return label + separator + command; }

    bool operator==(ReasonParts const&) const = default;
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
    std::string reasonLabel;                     ///< Its words (the proportional part).
    std::string reasonSeparator;                 ///< Between the words and the command; empty for none.
    std::string reasonCommand;                   ///< Its command (the monospace part); often empty.
    std::string fullReason;                      ///< The hover card's reason: whole, to MaxFullReasonLength.
    std::string fullReasonLabel;                 ///< Its words.
    std::string fullReasonCommand;               ///< Its command.
    std::string program;                         ///< The program the reason is about; empty when unknown.
    std::string folder;                          ///< The pane's working directory; empty when unknown.
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
    TreeLines lines; ///< The Terminal look's tree connectors.
    /// The connectors on the row's second line (its reason): every line that passes the row goes on through
    /// it, and so does the line down to the row's own children. Empty columns on a workspace row.
    TreeLines continued;
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

/// The longest full reason a row holds for its hover card (SidebarRow::fullReason), in characters, the
/// ellipsis included: far above any reason a program means to show, well below one that floods the card.
inline constexpr std::size_t MaxFullReasonLength = 2000;

/// What one pass over a directory gives the sidebar: the rows it draws, and every workspace's row whatever
/// the filter hides, which the footer and the summary count over.
struct SidebarListing
{
    std::vector<SidebarRow> rows;       ///< The rows drawn, depth-first (sidebarRows()).
    std::vector<SidebarRow> workspaces; ///< Every workspace's row, in order, filtered or not.
};

/// Flattens @p directory into the rows the sidebar draws and the workspace rows a count needs, in one pass.
/// @param directory The workspaces, tabs and panes.
/// @param attention This window's seen-state.
/// @param state What is collapsed, and the filter.
/// @param now The wall clock the times are measured against.
/// @param displayedTab The tab this window shows, if any.
/// @return The listing; its rows are what sidebarRows() returns.
[[nodiscard]] SidebarListing sidebarListing(WorkspaceDirectory const& directory,
                                            TabAttention const& attention,
                                            SidebarState const& state,
                                            std::chrono::system_clock::time_point now,
                                            std::optional<TabRef> displayedTab);

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
/// @param rows Rows from sidebarRows() built with an empty filter, or SidebarListing::workspaces, for a count
///             of everything.
/// @return The panes that need the user and that work, counted over the leaves under each workspace row
///         (LeafCounts), not over the rolled-up urgencies.
[[nodiscard]] SidebarFooter sidebarFooter(std::span<SidebarRow const> rows);

/// What the window's chrome says of the workspaces around the rows: which one it shows, and what the others
/// ask of the user.
struct SidebarSummary
{
    std::string currentWorkspace;            ///< The shown workspace's name, as its row titles it.
    Urgency otherWorkspaces = Urgency::Idle; ///< The most urgent roll-up of every other workspace.
    TabBadgeKind otherWorkspacesGlyph = TabBadgeKind::None; ///< That roll-up's glyph, the first among equals.

    bool operator==(SidebarSummary const&) const = default;
};

/// The summary over @p rows.
/// @param rows Rows from sidebarRows() built with an empty filter, or SidebarListing::workspaces, so no
///             workspace row is left out.
/// @param current The workspace the window shows; none leaves the name empty and counts every workspace.
/// @return The summary.
[[nodiscard]] SidebarSummary sidebarSummary(std::span<SidebarRow const> rows,
                                            std::optional<uint64_t> current);

/// Whether the workspace chip marks that another workspace asks for the user.
enum class ChipMarker : uint8_t
{
    None = 0, ///< Nothing elsewhere waits for the user.
    Shown,    ///< Another workspace needs the user, or has a result they have not seen.
};

/// @param otherWorkspaces The most urgent roll-up of the workspaces the window does not show.
/// @return Shown from DoneUnseen up: a result not seen, a failure not seen, or a request for the user. Work
///         still running and output alone do not mark it; the chip is not a progress indicator.
[[nodiscard]] constexpr ChipMarker chipMarkerOf(Urgency otherWorkspaces) noexcept
{
    return otherWorkspaces >= Urgency::DoneUnseen ? ChipMarker::Shown : ChipMarker::None;
}

/// What a status says, as words: `needs you`, `failed`, `done`, `new output`, `working`.
/// @param urgency The status.
/// @return The words; empty for Idle.
[[nodiscard]] std::string_view urgencyText(Urgency urgency) noexcept;

/// What a screen reader says of the workspace chip: the workspace it names and, while it is marked, what the
/// other workspaces ask in the words of their roll-up (`Workspace docs, another workspace failed`).
/// @param summary The summary the chip shows.
/// @return The name.
[[nodiscard]] std::string workspaceChipAccessibleName(SidebarSummary const& summary);

/// The detail line of a row's hover card: what it is about, where, and how long (`claude · ~/src · 4:12`).
/// @param row The row.
/// @return The parts it has, joined by ` · `; empty when it has none.
[[nodiscard]] std::string hoverDetailOf(SidebarRow const& row);

/// The index of the row @p row hangs under.
/// @param rows The rows as shown.
/// @param row A row's index.
/// @return The nearest row above it of a lesser depth; -1 for a workspace row or an index out of range.
[[nodiscard]] int parentRowOf(std::span<SidebarRow const> rows, int row) noexcept;

/// The Terminal look's connectors of a row as text: two cells per column of @p lines that the row's depth
/// fills, the connector and a blank (`│ ├ `).
/// @param lines The row's connectors.
/// @param depth The row's depth; a workspace row (0) has none.
/// @return The text, empty for a workspace row.
[[nodiscard]] std::string treeLinesText(TreeLines const& lines, uint8_t depth);

/// The Terminal look's connectors of a row's second line as text: two cells per column, both columns of a
/// tab or pane row, so the line to a tab's panes reaches them (`│ │ `).
/// @param row The row.
/// @return The text, empty for a workspace row.
[[nodiscard]] std::string treeContinuationText(SidebarRow const& row);

/// The cells of the Terminal look's progress meter.
inline constexpr int MeterCells = 5;

/// The Terminal look's progress meter as block characters: @ref MeterCells cells, full blocks for what is
/// done, a partial block for the cell it is in, shade for the rest (`██▌░░`).
/// @param percent The percent done; clamped to 0..100.
/// @return The meter.
[[nodiscard]] std::string meterText(int percent);

/// A workspace's git state as a row shows it: the branch, then how far it is ahead of and behind its upstream
/// where that is not zero (`feat/billing ↑2 ↓1`).
/// @param git The git state.
/// @return The text, the branch sanitized to plain text.
[[nodiscard]] std::string gitTextOf(DirectoryGit const& git);

/// Time spent in a state.
/// @param elapsed How long; below zero reads as zero.
/// @return `0:42`, `4:12` or `1:02:03`.
[[nodiscard]] std::string formatLive(std::chrono::seconds elapsed);

/// Time since something ended.
/// @param elapsed How long ago; below zero reads as zero.
/// @return `now` (under 10 s), `42s ago`, `2m ago`, `3h ago` or `2d ago`.
[[nodiscard]] std::string formatRelative(std::chrono::seconds elapsed);

/// Between the words of a reason and what follows them: `exit 1 · 18s`, `Permission · Bash: `.
inline constexpr std::string_view LabelSeparator = " · ";

/// Text from outside, made fit for a row's line: no controls, no invisible or bidi formatting, at most
/// MaxRowTextLength characters.
/// @param text The text.
/// @return The text as a row may show it.
[[nodiscard]] std::string rowTextOf(std::string_view text);

/// What a program-status record says it is doing or waits for: the reason a row shows of it. A permission
/// asks for a command the program wants to run, which is the reason's command part; whatever leads it, the
/// kind and the tool that would run it (`Permission · Bash: `), is its label.
/// @param record The record that decides the row.
/// @param kind The badge it earned.
/// @param maxLength The most characters the reason holds.
/// @return The reason, sanitized.
[[nodiscard]] ReasonParts statusReasonOf(vtbackend::ProgramStatusRecord const& record,
                                         BadgeKind kind,
                                         std::size_t maxLength);

/// How a status's time is kept: its progress while working or blocked when it reports one, else the time
/// spent while working or blocked, else the time since.
/// @param progress The percent done the record reports, if any.
/// @param kind The badge it earned.
/// @return Progress, Live or Relative.
[[nodiscard]] TimeKind statusTimeKind(std::optional<uint8_t> progress, BadgeKind kind) noexcept;

/// A row's time, formatted.
/// @param kind How it is kept (statusTimeKind).
/// @param since When the status began or ended; none when unknown.
/// @param progress The percent done, for Progress.
/// @param now The wall clock.
/// @return `42%`, `4:12`, `2m ago`; empty for None, or for Live and Relative without @p since.
[[nodiscard]] TimeText formatTimeText(TimeKind kind,
                                      std::optional<std::chrono::system_clock::time_point> since,
                                      std::optional<uint8_t> progress,
                                      std::chrono::system_clock::time_point now);

/// What a finished command says of itself: `exit 1 · 18s`, or `exit 1 · 18s · ` and the command line as its
/// command part when it is known.
/// @param finished The command.
/// @return The reason, the command line sanitized.
[[nodiscard]] ReasonParts finishedText(vthost::proto::FinishedCommand const& finished);

} // namespace contour::window
