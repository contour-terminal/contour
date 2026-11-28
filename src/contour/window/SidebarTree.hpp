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
/// A window that lists several daemons passes their sections (DaemonSection): each opens with a Daemon row, a
/// section heading at depth 0, and its workspaces follow it at depth 0 too, so the tree gains no column.
/// Every key that names a workspace, tab or pane carries its daemon and that daemon's instance: ids are
/// daemon-local and start over when a daemon restarts.
///
/// What a pane asks is decided by paneUrgency() (AttentionJump.hpp), in the one order of StatusUrgency.hpp;
/// a parent row shows the most urgent of its panes, with that pane's glyph, reason and time. A Failed or
/// DoneUnseen pane whose verdict is a finished command has a reason (`exit 1 · 18s`), and the time since it
/// ended where its daemon stated that (DirectoryPane::finishedAt); without it, no time, as
/// vthost::proto::FinishedCommand carries no timestamp to measure from.
#include <contour/config/TabBarMode.hpp>
#include <contour/window/CloseDialogTable.hpp>
#include <contour/window/DaemonSection.hpp>
#include <contour/window/PaneKey.hpp>
#include <contour/window/StatusKinds.hpp>
#include <contour/window/StatusUrgency.hpp>
#include <contour/window/TabAttention.hpp>
#include <contour/window/TabBadge.hpp>
#include <contour/window/TabIcon.hpp>
#include <contour/window/WorkspaceDirectory.hpp>

#include <vtbackend/core/Color.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <compare>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <ranges>
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
    Daemon,        ///< A daemon: the heading of its section, its workspaces beneath it.
    Hidden,        ///< The daemons the user chose not to list, last.
};

/// Number of @c SidebarRowKind enumerators, derived from the last one.
inline constexpr size_t SidebarRowKindCount = static_cast<size_t>(SidebarRowKind::Hidden) + 1;

/// Identifies a row across rebuilds, so a view keeps its selection and focus through a refresh.
struct SidebarRowKey
{
    SidebarRowKind kind = SidebarRowKind::Workspace; ///< What the row stands for.
    DaemonRef daemon {};                             ///< The daemon of its section; empty for none.
    /// That daemon's run, which its ids belong to; none on a Daemon row, which names no id and keeps its
    /// identity as its daemon connects (SidebarRow::daemonInstance holds the run it lists).
    DaemonInstance instance {};
    uint64_t workspace = 0; ///< The workspace's daemon id.
    uint64_t tab = 0;       ///< The tab's id; 0 for a workspace row.
    PaneKey pane;           ///< The pane's key; zero for a workspace or tab row.

    auto operator<=>(SidebarRowKey const&) const = default;
};

/// What a close confirmation asks about: the kind of close, and the row it was asked on, whole. It travels
/// through the dialog and back into the commit as an opaque value, so the close acts on exactly that node --
/// on its own daemon and run -- whatever the window shows or whichever daemon it is on by the time the user
/// answers. Workspace ids start at 1 on every daemon, so the ids alone never say whose node it is.
struct CloseNode
{
    CloseKind kind = CloseKind::Workspace; ///< What closes: the row's workspace, its tab, or its pane.
    SidebarRowKey row;                     ///< The row, with its daemon and that daemon's run.

    auto operator<=>(CloseNode const&) const = default;
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

/// Number of @c RowSelection enumerators, derived from the last one.
inline constexpr size_t RowSelectionCount = static_cast<size_t>(RowSelection::ContainsSelected) + 1;

/// Whether the window holding the sidebar has the user's attention: when a background pane that starts to
/// need them is worth saying aloud, and how much of a coloured row's colour it keeps.
enum class WindowActivity : uint8_t
{
    Inactive = 0, ///< Another window or application has focus.
    Active,       ///< This window has focus.
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

/// A workspace, named by its daemon and that daemon's run: a workspace id is unique only within one run of
/// one daemon.
struct WorkspaceKey
{
    DaemonRef daemon {};        ///< The daemon; empty for a window with no daemon.
    DaemonInstance instance {}; ///< Its run.
    uint64_t workspace = 0;     ///< The workspace's id on it.

    auto operator<=>(WorkspaceKey const&) const = default;
};

/// A tab, named by its workspace.
struct TabKey
{
    WorkspaceKey workspace {}; ///< The workspace holding it.
    uint64_t tab = 0;          ///< The tab's id within it.

    auto operator<=>(TabKey const&) const = default;
};

/// What a jump goes to, or what a window displays: a workspace on a daemon, and within it a tab and a pane
/// where they are named.
struct SidebarTarget
{
    DaemonRef daemon {};            ///< The daemon; empty for a window with no daemon.
    DaemonInstance instance {};     ///< Its run.
    uint64_t workspace = 0;         ///< The workspace's id on it.
    std::optional<uint64_t> tab {}; ///< The tab within it, if one is named.
    std::optional<PaneKey> pane {}; ///< The pane within that tab, if one is named.

    bool operator==(SidebarTarget const&) const = default;
};

/// @param key A row of a workspace, a tab, a pane or a daemon.
/// @return What the row names, as a jump goes there: its workspace, and the tab and pane a tab or pane row
///         names; a daemon row names its daemon's default workspace (workspace 0, which no workspace is), in
///         no particular run, as its key carries none (sidebarTargetOf(SidebarRow const&) names the listed
///         one).
[[nodiscard]] inline SidebarTarget sidebarTargetOf(SidebarRowKey const& key)
{
    auto target = SidebarTarget {
        .daemon = key.daemon, .instance = key.instance, .workspace = key.workspace, .tab = {}, .pane = {}
    };
    if (key.kind == SidebarRowKind::Tab || key.kind == SidebarRowKind::Pane)
        target.tab = key.tab;
    if (key.kind == SidebarRowKind::Pane)
        target.pane = key.pane;
    return target;
}

/// Where a jump to a row lands.
enum class JumpMode : uint8_t
{
    Here = 0,  ///< In this window.
    NewWindow, ///< In a window of its own.
};

/// Every JumpMode, in order.
inline constexpr auto JumpModes = std::array { JumpMode::Here, JumpMode::NewWindow };

/// The jumps a window's sidebar has under way: one per JumpMode, neither ending the other.
struct PendingJumps
{
    /// In this window: what the location chip shows, and what Escape and CancelSwitch end.
    std::optional<SidebarTarget> here;
    /// Into a window of its own: ended from its own row.
    std::optional<SidebarTarget> newWindow;

    /// @param mode A jump's mode.
    /// @return The jump of @p mode under way, if any.
    [[nodiscard]] std::optional<SidebarTarget> const& of(JumpMode mode) const noexcept
    {
        return mode == JumpMode::Here ? here : newWindow;
    }

    bool operator==(PendingJumps const&) const = default;
};

/// Whether a row says what its daemon says now, or what it said last.
enum class RowStaleness : uint8_t
{
    Live = 0, ///< Its daemon is connected, or there is nothing older to show.
    /// Its daemon is not connected, whatever its link is doing: the row is what was last known of it, drawn
    /// dimmer, with no running mark, and counted by neither the footer nor the summary.
    LastKnown,
};

/// Who holds the link a daemon row's verbs (Connect, Start, Cancel) act on.
enum class DaemonRowLink : uint8_t
{
    Observer = 0, ///< An observer link the sidebar's process keeps: the daemon sections connect it.
    OwnWindow,    ///< This window's own link: it is sought again as the chip and a pane's footer do.
    OtherWindow,  ///< Another window's link: that window's chip and footers act on it, not this row.
};

/// Whether another window of this process shows a workspace.
enum class ShownElsewhere : uint8_t
{
    No = 0, ///< No other window shows it.
    Yes,    ///< Another window shows it; activating it raises that window.
};

/// Whether a jump to a row is under way.
enum class RowPending : uint8_t
{
    No = 0, ///< Nothing is under way.
    Yes,    ///< The pending jump goes to this row, or, on a daemon row, into its section.
};

/// What a row's one trailing status slot shows.
enum class StatusSlot : uint8_t
{
    None = 0, ///< Nothing.
    Link,     ///< The daemon's link state: its section's daemon is not connected.
    Urgency,  ///< The row's rolled-up urgency.
    Pending,  ///< A jump to the row, or into its section, is under way.
};

/// One row of the sidebar.
struct SidebarRow
{
    SidebarRowKey key;                           ///< Identifies it across rebuilds.
    uint8_t depth = 0;                           ///< 0 daemon, workspace or hidden, 1 tab, 2 pane.
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
    std::string fullFolder;                      ///< The hover card's folder: whole, to MaxFullReasonLength.
    std::string gitText;                         ///< Workspace rows: `feat/billing ↑2 ↓1`.
    TimeText time;                               ///< The right-hand side.
    Urgency urgency = Urgency::Idle;             ///< Rolled up over the panes beneath a parent row.
    TabBadgeKind glyph = TabBadgeKind::None;     ///< Which badge look draws it (chromeStyle.badgeLook).
    RowExpansion expansion = RowExpansion::None; ///< Whether its children are shown.
    RowSelection selection = RowSelection::None; ///< Whether this window displays it, or a fold hides that.
    /// Its real place among its siblings in the directory, whatever a filter or a fold hides: a workspace's
    /// among the workspaces, a tab's among its workspace's tabs, a pane's among its tab's panes. A drop
    /// resolves to these, never to an index among the rows shown.
    std::size_t position = 0;
    TreeLines lines; ///< The Terminal look's tree connectors.
    /// The connectors on the row's second line (its reason): every line that passes the row goes on through
    /// it, and so does the line down to the row's own children. Empty columns on a workspace row.
    TreeLines continued;
    LeafCounts leaves; ///< The panes beneath it, for the footer.
    /// The colour of the tab a tab or pane row belongs to (DirectoryTab::color); none when the tab is
    /// uncoloured, and always none on a workspace row, folded or not.
    std::optional<vtbackend::RGBColor> tabColor {};
    /// The colour of the workspace the row belongs to (DirectoryWorkspace::color), on every row of it: the
    /// workspace row, folded or not, its tab rows and their pane rows. None when the workspace is uncoloured.
    std::optional<vtbackend::RGBColor> workspaceColor {};
    /// A tab row's icon as the window's tab strip shows it (window::tabIconOf); none for a workspace or pane
    /// row, and for a tab the window does not hold, whose profile and program it does not know.
    std::optional<TabIcon> icon {};
    /// A Daemon row: where its link stands. Every other row leaves it at its default.
    DaemonLinkState linkState = DaemonLinkState::NotConnected;
    /// A Daemon row: the run of its daemon its section lists now, which a jump from the row names; none while
    /// not connected. Every other row carries its run in its key.
    DaemonInstance daemonInstance {};
    std::string linkReason;                             ///< A Daemon row: why its link failed; plain text.
    DaemonLocality locality = DaemonLocality::Remote;   ///< A Daemon row: whether it runs on this machine.
    DaemonRowLink link = DaemonRowLink::Observer;       ///< A Daemon row: who holds its link.
    RowStaleness staleness = RowStaleness::Live;        ///< Whether the row is what was last known.
    ShownElsewhere shownElsewhere = ShownElsewhere::No; ///< A Workspace row: whether another window shows it.
    /// Whether a pending jump goes to it (or into a Daemon row's section).
    RowPending pending = RowPending::No;
    StatusSlot slot = StatusSlot::None; ///< What its trailing status slot shows (statusSlotOf()).
    std::string daemonName; ///< The name of its section's daemon; empty for a window with no daemon.
    std::vector<HiddenDaemon> hidden; ///< A Hidden row: the daemons it names; empty on every other row.

    bool operator==(SidebarRow const&) const = default;
};

/// @param row A row of a workspace, a tab, a pane or a daemon.
/// @return What the row names, as a jump goes there (sidebarTargetOf(SidebarRowKey const&)), a daemon row's
/// in
///         the run its section lists now.
[[nodiscard]] inline SidebarTarget sidebarTargetOf(SidebarRow const& row)
{
    auto target = sidebarTargetOf(row.key);
    if (row.key.kind == SidebarRowKind::Daemon)
        target.instance = row.daemonInstance;
    return target;
}

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
    /// The daemon the window opened on: its workspaces start unfolded, every other daemon's start folded.
    DaemonRef openedOn {};
    /// The workspaces the user folded or unfolded against their daemon's default: a workspace is folded iff
    /// its daemon is not openedOn, or it is toggled, but not both. A workspace that appears later follows its
    /// daemon's default, and so does every workspace of a daemon's new run.
    std::set<WorkspaceKey> toggled {};
    std::set<TabKey> collapsedTabs {};                      ///< Tabs whose panes are hidden.
    std::set<DaemonRef> foldedDaemons {};                   ///< Daemons whose rows hide their workspaces.
    config::SidebarScope scope = config::SidebarScope::All; ///< Which rows are listed.
    /// A case-insensitive (ASCII) substring a row must hold in its title, reason or git text, or have a
    /// descendant that does; a daemon row's name counts too. While it is set, every ancestor of a match is
    /// expanded. Empty: no filter.
    std::string filter {};

    /// @return Whether @p daemon's workspaces start folded: every daemon's but openedOn's.
    [[nodiscard]] bool foldsByDefault(DaemonRef const& daemon) const { return daemon != openedOn; }
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
    std::vector<SidebarRow> rows; ///< The rows drawn, depth-first (sidebarRows()).
    std::vector<SidebarRow>
        workspaces; ///< Every workspace's row of every section, in order, filtered or not.
};

/// The sections a window lists, and which one is its own.
struct SidebarSources
{
    std::span<DaemonSection const> sections; ///< In the order they are listed.
    DaemonRef own;                           ///< The window's own daemon; its section's panes use ownMarks.
    /// The window's own seen-state, for its own section's panes; null reads as nothing seen. Every other
    /// section's panes are read as their daemon states them.
    TabAttention const* ownMarks = nullptr;
    std::span<HiddenDaemon const>
        hidden; ///< The daemons the user chose not to list; none lists no Hidden row.
};

/// Flattens @p sources into the rows the sidebar draws, section after section, each headed by its Daemon row.
/// @param sources The sections, the window's own daemon and its seen-state, and the hidden daemons.
/// @param state What is folded, the scope and the filter.
/// @param now The wall clock the times are measured against.
/// @param displayed What this window shows, if anything: its tab is Selected.
/// @param pending Where the jumps under way go: each one's row and its daemon's row are Pending, and the
///                Needs-me scope keeps them.
/// @return The listing; its workspaces span every section.
[[nodiscard]] SidebarListing sidebarListing(SidebarSources const& sources,
                                            SidebarState const& state,
                                            std::chrono::system_clock::time_point now,
                                            std::optional<SidebarTarget> displayed,
                                            PendingJumps pending);

/// Flattens @p directory into the rows the sidebar draws and the workspace rows a count needs, in one pass: a
/// window with no daemon, one section with an empty DaemonRef and no Daemon row.
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
///         (LeafCounts), not over the rolled-up urgencies; last-known workspaces are not counted.
[[nodiscard]] SidebarFooter sidebarFooter(std::span<SidebarRow const> rows);

/// The footer's counts in the status glyphs @p look's rows draw for them (`! 1 · ◑ 2`), for a footer too
/// narrow for any wording in words; the words stay its accessible name.
/// @param footer The counts.
/// @param look The chrome row, as its sidebar look.
/// @return The text, counts that are zero left out; empty when no pane needs the user or works.
[[nodiscard]] std::string sidebarFooterGlyphText(SidebarFooter const& footer, config::SidebarLook look);

/// What the window's chrome says of the workspaces around the rows: which one it shows, and what the others
/// ask of the user.
struct SidebarSummary
{
    std::string currentWorkspace;            ///< The shown workspace's name, as its row titles it.
    Urgency otherWorkspaces = Urgency::Idle; ///< The most urgent roll-up of every other workspace.
    TabBadgeKind otherWorkspacesGlyph = TabBadgeKind::None; ///< That roll-up's glyph, the first among equals.

    bool operator==(SidebarSummary const&) const = default;
};

/// How much of where the window is its breadcrumb says.
enum class BreadcrumbDepth : uint8_t
{
    WorkspaceTab = 0,   ///< `workspace › tab`: one daemon, or none, so the daemon goes without saying.
    DaemonWorkspaceTab, ///< `daemon › workspace › tab`: several daemons listed beside each other.
};

/// @param sections How many daemon sections the sidebar lists (0 for a window with no daemon).
/// @return `daemon › workspace › tab` only with more than one section.
[[nodiscard]] constexpr BreadcrumbDepth breadcrumbDepthOf(std::size_t sections) noexcept
{
    return sections > 1 ? BreadcrumbDepth::DaemonWorkspaceTab : BreadcrumbDepth::WorkspaceTab;
}

/// The summary over @p rows.
/// @param rows Rows from sidebarRows() built with an empty filter, or SidebarListing::workspaces, so no
///             workspace row is left out.
/// @param current The workspace the window shows; none leaves the name empty and counts every workspace.
/// @return The summary, over the workspace rows of every section; a last-known workspace still names the
///         current one, but asks nothing of the others' roll-up.
[[nodiscard]] SidebarSummary sidebarSummary(std::span<SidebarRow const> rows,
                                            std::optional<WorkspaceKey> const& current);

/// @param urgency A pane's urgency.
/// @return Whether it asks for the user: NeedsYou, Failed or DoneUnseen (the classes an attention jump
///         visits), which is what the Needs-me scope keeps.
[[nodiscard]] bool needsMe(Urgency urgency) noexcept;

/// @param rows The rows as listed.
/// @return How many of them need the user (needsMe() of their rolled-up urgency).
[[nodiscard]] std::size_t rowsNeedingMe(std::span<SidebarRow const> rows) noexcept;

/// @param scope The scope in force.
/// @return The other one: what a second press of the Needs-me control, or of its action, brings back.
[[nodiscard]] constexpr config::SidebarScope toggledScope(config::SidebarScope scope) noexcept
{
    return scope == config::SidebarScope::NeedsMe ? config::SidebarScope::All : config::SidebarScope::NeedsMe;
}

/// @param state What the sidebar remembers.
/// @return Whether neither a filter nor the Needs-me scope narrows the tree: the filter's Return then has no
///         match to go to.
[[nodiscard]] bool isUnnarrowed(SidebarState const& state) noexcept;

/// The row the filter's Return goes to: the first tab or pane row that itself matches (the filter's text in
/// its own title, reason or git text; under the Needs-me scope, its own urgency, and on a tab row only while
/// its panes are not listed beneath it), not an ancestor listed for a match beneath it; else the first
/// workspace row that itself matches; else the first tab or pane row; else the first workspace row.
/// @param rows The rows as listed under @p state.
/// @param state What the sidebar remembers: its filter and scope.
/// @return The row's index; none for a tree with neither tab, pane nor workspace rows.
[[nodiscard]] std::optional<std::size_t> firstMatchOf(std::span<SidebarRow const> rows,
                                                      SidebarState const& state);

/// What the tree says where it lists nothing the user asked for.
enum class SidebarEmptyState : uint8_t
{
    None = 0,        ///< Nothing: the rows speak for themselves.
    NothingNeedsYou, ///< "Nothing needs you", and a way back to every row ("Show all").
};

/// @param scope   The scope in force.
/// @param needing How many of the listed rows need the user (rowsNeedingMe()). The Needs-me scope keeps the
///                displayed tab's path and a pending jump's besides, so an empty list is not the test.
/// @return NothingNeedsYou under the Needs-me scope while no row needs the user; None otherwise, and always
///         under All, whose empty tree has nothing to undo.
[[nodiscard]] constexpr SidebarEmptyState sidebarEmptyStateOf(config::SidebarScope scope,
                                                              std::size_t needing) noexcept
{
    return scope == config::SidebarScope::NeedsMe && needing == 0 ? SidebarEmptyState::NothingNeedsYou
                                                                  : SidebarEmptyState::None;
}

/// What a row's status slot shows while its link is in one state and no jump is under way.
struct LinkSlotRow
{
    DaemonLinkState state; ///< The link state.
    StatusSlot slot;       ///< Link while the daemon is not connected; Urgency once it is.
    RowStaleness tree;     ///< What a section whose link is in this state lists of a directory it has.
};

/// The slot and the staleness of every link state, indexed by @c DaemonLinkState.
inline constexpr auto LinkSlotRows = std::array {
    LinkSlotRow {
        .state = DaemonLinkState::NotConnected, .slot = StatusSlot::Link, .tree = RowStaleness::LastKnown },
    LinkSlotRow {
        .state = DaemonLinkState::Connecting, .slot = StatusSlot::Link, .tree = RowStaleness::LastKnown },
    LinkSlotRow {
        .state = DaemonLinkState::Connected, .slot = StatusSlot::Urgency, .tree = RowStaleness::Live },
    LinkSlotRow {
        .state = DaemonLinkState::Reconnecting, .slot = StatusSlot::Link, .tree = RowStaleness::LastKnown },
    LinkSlotRow {
        .state = DaemonLinkState::NeedsInput, .slot = StatusSlot::Link, .tree = RowStaleness::LastKnown },
    LinkSlotRow {
        .state = DaemonLinkState::NotRunning, .slot = StatusSlot::Link, .tree = RowStaleness::LastKnown },
    LinkSlotRow {
        .state = DaemonLinkState::TooOld, .slot = StatusSlot::Link, .tree = RowStaleness::LastKnown },
    LinkSlotRow {
        .state = DaemonLinkState::Failed, .slot = StatusSlot::Link, .tree = RowStaleness::LastKnown },
};

static_assert(LinkSlotRows.size() == DaemonLinkStateCount, "LinkSlotRows must list every DaemonLinkState");
static_assert(std::ranges::all_of(std::views::iota(size_t { 0 }, LinkSlotRows.size()),
                                  [](size_t i) { return LinkSlotRows[i].state == DaemonLinkState(i); }),
              "LinkSlotRows must list the states in enumerator order");

/// What a row's one trailing status slot shows.
/// @param state Where the link of the row's daemon stands; Connected for a row with no daemon.
/// @param pending Whether a jump to the row, or into a Daemon row's section, is under way.
/// @param rolledUp The row's rolled-up urgency.
/// @return Pending while a jump is under way; else Link while the daemon is not connected; else Urgency when
///         the row asks for something, None when it is idle.
[[nodiscard]] constexpr StatusSlot statusSlotOf(DaemonLinkState state,
                                                RowPending pending,
                                                Urgency rolledUp) noexcept
{
    if (pending == RowPending::Yes)
        return StatusSlot::Pending;
    auto const slot = LinkSlotRows[static_cast<size_t>(state)].slot;
    return slot == StatusSlot::Urgency && rolledUp == Urgency::Idle ? StatusSlot::None : slot;
}

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

/// The detail line of a row's hover card: what it is about, where, and how long (`claude · ~/src · 4:12`).
/// @param row The row.
/// @return The parts it has, joined by ` · `; empty when it has none.
[[nodiscard]] std::string hoverDetailOf(SidebarRow const& row);

/// What a row's second line holds.
enum class SecondLine : uint8_t
{
    None = 0, ///< Nothing to show: a workspace row, or a tab or pane row that knows neither its folder nor
              ///< its program, whose name the row then centres in its two lines.
    Idle,     ///< The idle line (idleLineOf()): where the pane is and what runs there.
    Status,   ///< The reason: what the pane does or waits for.
};

/// @param row The row.
/// @return Status when the row has a reason; else Idle when it is a tab or pane row that knows its folder
///         or its program; None for a workspace row and for one that knows neither.
[[nodiscard]] constexpr SecondLine secondLineOf(SidebarRow const& row) noexcept
{
    if (row.depth == 0)
        return SecondLine::None;
    if (!row.reason.empty())
        return SecondLine::Status;
    return row.folder.empty() && row.program.empty() ? SecondLine::None : SecondLine::Idle;
}

/// The second line of a tab or pane row with no reason to show (`~/src · claude`).
/// @param row The row.
/// @return The idle line: folder, then program, joined by LabelSeparator; a missing part is left out
///         with its separator; empty for a workspace row and when both are empty.
[[nodiscard]] std::string idleLineOf(SidebarRow const& row);

/// The index of the row @p row hangs under.
/// @param rows The rows as shown.
/// @param row A row's index.
/// @return The nearest row above it of a lesser depth; for a workspace row, its section's Daemon row; -1 for
///         a Daemon or Hidden row, a workspace row with no Daemon row above it, or an index out of range.
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

/// What the Terminal look's workspace rail (`sidebar.workspace_color: group`) draws in a row's first cell, on
/// each of the row's lines.
struct RailCells
{
    std::string first;  ///< On the row's first line; empty where the workspace's disclosure is the cell.
    std::string second; ///< On its second line; empty for a workspace row, which has one line.

    bool operator==(RailCells const&) const = default;
};

/// The cells of a workspace's rail on @p row: the tree's own connector in its first column, made to run on,
/// so the rail is one unbroken line from the workspace's row to the bottom of its last row. A blank (a pane
/// under the last tab, the last tab's second line) becomes `│`, and a `└`, whose stroke stops half way down
/// the cell, becomes `├`. The tree's own text (treeLinesText()) is left as it is.
/// @param row The row.
/// @return The cells. A workspace row's first is `│` when it folds nothing, so has no disclosure to draw it,
///         and empty otherwise. A Daemon or Hidden row belongs to no workspace's rail: none.
[[nodiscard]] RailCells workspaceRailCellsOf(SidebarRow const& row);

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
/// @p maxLength characters.
/// @param text The text.
/// @param maxLength The most characters it holds: a row's line, or MaxFullReasonLength for its hover card.
/// @return The text as a row may show it.
[[nodiscard]] std::string rowTextOf(std::string_view text, std::size_t maxLength = MaxRowTextLength);

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
