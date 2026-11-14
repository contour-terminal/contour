// SPDX-License-Identifier: Apache-2.0
#pragma once
/// @file
/// The workspace sidebar's list model: a window's WorkspaceDirectory, flattened by sidebarRows() and handed
/// to QML one row per index. What the rows say is decided in SidebarTree.hpp, and the Qt-free tests of it
/// stand there; this adapter owns what only Qt can: the role names, the change signals, the coalescing of a
/// burst of events into one rebuild, the one-second timer behind the running times, and the announcement of
/// a pane that starts to need the user in a workspace the window is not showing.
#include <contour/config/TabBarMode.hpp>
#include <contour/platform/Announcer.hpp>
#include <contour/window/CloseDialogTable.hpp>
#include <contour/window/NodeDrag.hpp>
#include <contour/window/PaneKey.hpp>
#include <contour/window/SidebarDropPolicy.hpp>
#include <contour/window/SidebarTree.hpp>
#include <contour/window/TabAttention.hpp>
#include <contour/window/WorkspaceDirectorySource.hpp>

#include <core/platform/Clock.hpp>

#include <QtCore/QAbstractListModel>
#include <QtCore/QObject>
#include <QtCore/QPointF>
#include <QtCore/QString>
#include <QtCore/QTimer>
#include <QtCore/QVariantMap>

#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
#include <span>
#include <string>
#include <utility>
#include <vector>

/// The enumerators a sidebar row's kind, time and expansion roles carry, as QML names them. Their own
/// namespace, because Q_ENUM_NS needs Q_NAMESPACE in the namespace that declares the enums, and
/// SidebarTree.hpp -- where the rows are decided -- stays Qt-free; each enum here mirrors that one value for
/// value. QML names them as SidebarRows.SidebarRowKind.Workspace, ... once registerSidebarQmlTypes() has
/// run. A row compares its roles with these, never with its model's properties: a row can outlive its model
/// for as long as its window's teardown takes, and an enumerator needs no model to be read.
namespace contour::window::sidebarrows
{
Q_NAMESPACE

/// What a sidebar row stands for (@see window::SidebarRowKind, which it mirrors).
enum class SidebarRowKind : uint8_t
{
    Workspace = 0, ///< A workspace.
    Tab,           ///< A tab of a workspace.
    Pane,          ///< One pane of a tab with more than one.
};
Q_ENUM_NS(SidebarRowKind)

/// What a row's time is (@see window::TimeKind, which it mirrors).
enum class TimeKind : uint8_t
{
    None = 0, ///< Nothing.
    Live,     ///< Time spent in the current state.
    Relative, ///< Time since it ended.
    Progress, ///< The percentage the program reports.
};
Q_ENUM_NS(TimeKind)

/// Whether a row's children show (@see window::RowExpansion, which it mirrors).
enum class RowExpansion : uint8_t
{
    None = 0,  ///< A row with nothing to expand.
    Collapsed, ///< Its children are hidden.
    Expanded,  ///< Its children follow it.
};
Q_ENUM_NS(RowExpansion)

/// Whether a row is what the window shows (@see window::RowSelection, which it mirrors).
enum class RowSelection : uint8_t
{
    None = 0,         ///< Neither.
    Selected,         ///< The tab the window displays.
    ContainsSelected, ///< A folded row holding it.
};
Q_ENUM_NS(RowSelection)

/// Whether activating a row showed anything. Not exposed to QML: only C++ asks.
enum class RowActivation : uint8_t
{
    Nothing = 0, ///< The row is not there.
    Shown,       ///< What the row names is shown.
};

/// What a close confirmation is about (@see window::CloseKind, which it mirrors).
enum class CloseKind : uint8_t
{
    Workspace = 0, ///< A workspace.
    Tab,           ///< A tab.
    Pane,          ///< One pane.
};
Q_ENUM_NS(CloseKind)

/// Whether the sidebar's footer has room for the chord that jumps to the next pane that needs the user.
enum class FooterChord : uint8_t
{
    Dropped = 0, ///< No: the counts keep the room.
    Shown,       ///< Yes: it follows the counts.
};
Q_ENUM_NS(FooterChord)

/// Which wording of its counts the sidebar's footer shows.
enum class FooterCounts : uint8_t
{
    Full = 0, ///< `1 needs you · 1 working`.
    Compact,  ///< `1 waiting · 1 busy`: the same counts in fewer letters, for the narrowest sidebar.
};
Q_ENUM_NS(FooterCounts)

/// What a drag hovering a row or the New Workspace area shows (@see window::DropIndicator, which it mirrors).
enum class DropIndicator : uint8_t
{
    Refused = 0, ///< Nothing; the drop is not taken.
    LineAbove,   ///< A line at the row's upper edge.
    LineBelow,   ///< A line at the row's lower edge.
    Highlight,   ///< The row, or the area, highlighted.
};
Q_ENUM_NS(DropIndicator)

static_assert(std::to_underlying(DropIndicator::Refused)
              == std::to_underlying(window::DropIndicator::Refused));
static_assert(std::to_underlying(DropIndicator::LineAbove)
              == std::to_underlying(window::DropIndicator::LineAbove));
static_assert(std::to_underlying(DropIndicator::LineBelow)
              == std::to_underlying(window::DropIndicator::LineBelow));
static_assert(std::to_underlying(DropIndicator::Highlight)
              == std::to_underlying(window::DropIndicator::Highlight));
static_assert(std::to_underlying(SidebarRowKind::Workspace)
              == std::to_underlying(window::SidebarRowKind::Workspace));
static_assert(std::to_underlying(SidebarRowKind::Tab) == std::to_underlying(window::SidebarRowKind::Tab));
static_assert(std::to_underlying(SidebarRowKind::Pane) == std::to_underlying(window::SidebarRowKind::Pane));
static_assert(std::to_underlying(TimeKind::None) == std::to_underlying(window::TimeKind::None));
static_assert(std::to_underlying(TimeKind::Live) == std::to_underlying(window::TimeKind::Live));
static_assert(std::to_underlying(TimeKind::Relative) == std::to_underlying(window::TimeKind::Relative));
static_assert(std::to_underlying(TimeKind::Progress) == std::to_underlying(window::TimeKind::Progress));
static_assert(std::to_underlying(RowExpansion::None) == std::to_underlying(window::RowExpansion::None));
static_assert(std::to_underlying(RowExpansion::Collapsed)
              == std::to_underlying(window::RowExpansion::Collapsed));
static_assert(std::to_underlying(RowExpansion::Expanded)
              == std::to_underlying(window::RowExpansion::Expanded));
static_assert(std::to_underlying(CloseKind::Workspace) == std::to_underlying(window::CloseKind::Workspace));
static_assert(std::to_underlying(CloseKind::Tab) == std::to_underlying(window::CloseKind::Tab));
static_assert(std::to_underlying(CloseKind::Pane) == std::to_underlying(window::CloseKind::Pane));
static_assert(std::to_underlying(RowSelection::None) == std::to_underlying(window::RowSelection::None));
static_assert(std::to_underlying(RowSelection::Selected)
              == std::to_underlying(window::RowSelection::Selected));
static_assert(std::to_underlying(RowSelection::ContainsSelected)
              == std::to_underlying(window::RowSelection::ContainsSelected));

} // namespace contour::window::sidebarrows

namespace contour::window
{

/// Registers the sidebar rows' enums with QML under Contour.Terminal, as SidebarRows. The first call
/// registers; later ones return.
void registerSidebarQmlTypes();

/// What the sidebar asks of the window it sits in. The window implements it; a test answers it with a fake.
/// Every operation is the window's own (the same ones its actions reach), so the sidebar and the keyboard
/// cannot disagree about what, say, closing a workspace does.
class ISidebarHost
{
  public:
    ISidebarHost() = default;
    ISidebarHost(ISidebarHost const&) = delete;
    ISidebarHost& operator=(ISidebarHost const&) = delete;
    ISidebarHost(ISidebarHost&&) = delete;
    ISidebarHost& operator=(ISidebarHost&&) = delete;
    virtual ~ISidebarHost() = default;

    /// @return The window's source of workspaces; none while it has none. Asked on every rebuild, because a
    ///         window's source is replaced when it switches daemons.
    [[nodiscard]] virtual IWorkspaceDirectorySource* directorySource() const = 0;

    /// @return Who hosts the window's sessions, as a drag names it: the id space its pane keys and workspace
    ///         ids are in. A drop from another host names ids of that host's, which mean nothing here.
    [[nodiscard]] virtual HostIdentity hostIdentity() const = 0;

    /// @return What the window's host can move: a drop it would not make is refused up front, so the drag
    ///         shows the refusal (window::sidebarHostTakes()).
    [[nodiscard]] virtual HostMoves hostMoves() const = 0;

    /// @param directory What the window lists now, as directorySource() gave it: read once per rebuild.
    /// @return The tab the window shows, if any, by @p directory's ids.
    [[nodiscard]] virtual std::optional<TabRef> displayedTab(WorkspaceDirectory const& directory) const = 0;

    /// Shows @p pane: selects its workspace, activates its tab, focuses it and counts it as seen.
    /// @param pane The pane to show.
    virtual void showPane(PaneKey pane) = 0;

    /// Shows a workspace the user picked: selects it, and gives up what waited for an earlier look.
    /// @param workspace The workspace's id.
    virtual void showWorkspace(uint64_t workspace) = 0;

    /// Counts @p pane as seen.
    /// @param pane The pane the user has looked at.
    virtual void markSeen(PaneKey pane) = 0;

    /// Renames a workspace.
    /// @param workspace The workspace's id.
    /// @param name The new label.
    virtual void renameWorkspace(uint64_t workspace, std::string const& name) = 0;

    /// Sets the title a tab shows, as the strip's inline rename does.
    /// @param tab The tab.
    /// @param title Its title; empty clears the one the user set.
    virtual void renameTab(TabRef tab, std::string const& title) = 0;

    /// Moves a workspace in the list.
    /// @param workspace The workspace's id.
    /// @param to The position among all the workspaces to insert before (ReorderWorkspace::to).
    virtual void moveWorkspace(uint64_t workspace, std::size_t to) = 0;

    /// Moves a pane or a whole tab: into a workspace as a tab (MoveTab), into a tab as its splits (JoinTab),
    /// or into a workspace of its own (IntoNewWorkspace). The sessions keep running; a tab or a workspace the
    /// move empties closes.
    /// @param node The pane, by its key, or the tab.
    /// @param destination Where it goes; a MoveTab's position is among the tabs as they are now, AppendTab
    ///        for the end.
    virtual void moveNode(SidebarNode const& node, NodeDestination const& destination) = 0;

    /// Opens the context menu of a row.
    /// @param row The row, as the model lists it.
    /// @param windowPosition Where the pointer is, in window coordinates.
    virtual void openContextMenu(SidebarRow const& row, QPointF windowPosition) = 0;
};

/// Whether the user can see the sidebar's rows, which decides whether their times need to run.
enum class SidebarPresence : uint8_t
{
    Hidden = 0, ///< The sidebar is hidden, or the window is not exposed.
    Shown,      ///< The sidebar is shown in an exposed window.
};

/// Whether the window is on screen.
enum class WindowExposure : uint8_t
{
    Hidden = 0, ///< Minimized, or not mapped yet.
    Exposed,    ///< On screen.
};

/// Whether the switcher popup is presented over the window: the chip's dropdown shows the sidebar's rows,
/// times and all, whether or not the sidebar is shown.
enum class SwitcherOverWindow : uint8_t
{
    No = 0,
    Yes,
};

/// @param visibility Whether the window shows its sidebar.
/// @param exposure Whether the window is on screen.
/// @param switcher Whether the switcher popup is presented over it.
/// @return Shown when the window is on screen and its sidebar or the switcher over it shows the rows.
[[nodiscard]] constexpr SidebarPresence sidebarPresenceOf(config::SidebarVisibility visibility,
                                                          WindowExposure exposure,
                                                          SwitcherOverWindow switcher) noexcept
{
    if (exposure == WindowExposure::Hidden)
        return SidebarPresence::Hidden;
    return visibility == config::SidebarVisibility::Shown || switcher == SwitcherOverWindow::Yes
               ? SidebarPresence::Shown
               : SidebarPresence::Hidden;
}

/// Whether the window has the user's attention, which is when a background pane that starts to need them
/// is worth saying aloud.
enum class WindowActivity : uint8_t
{
    Inactive = 0, ///< Another window or application has focus.
    Active,       ///< This window has focus.
};

/// What a screen reader names a row by: its title, which is what the row shows. Its kind, fold and status are
/// the description's (accessibleDescriptionOf()), its selection the item's own state.
/// @param row The row.
/// @return The title.
[[nodiscard]] QString accessibleNameOf(SidebarRow const& row);

/// What a screen reader says of a row after its name, in the user's language: its kind, whether it is folded
/// (Qt Quick's Accessible has no expanded state to carry it) and what it asks of the user, counted with the
/// right plural form.
/// @param row The row.
/// @return For example `Workspace, expanded, 1 pane needs you`; a row that asks nothing and folds nothing
/// says
///         its kind alone.
[[nodiscard]] QString accessibleDescriptionOf(SidebarRow const& row);

/// What the sidebar's footer says of every pane the window lists: the counts that are not zero, in the user's
/// language, joined by ` · ` (`1 needs you · 2 working`).
/// @param footer The counts.
/// @return The text; empty when no pane needs the user or works.
[[nodiscard]] QString footerTextOf(SidebarFooter const& footer);

/// footerTextOf() in its shorter wording (`1 waiting · 2 busy`), for a footer too narrow for the full one.
/// @param footer The counts.
/// @return The text; empty when no pane needs the user or works.
[[nodiscard]] QString footerCompactTextOf(SidebarFooter const& footer);

/// What the sidebar's footer has to place, in logical pixels: the footer's text parts and the room they
/// share.
struct FooterRoom
{
    double available = 0;   ///< The width the counts and the chord share.
    double countsWidth = 0; ///< The counts' whole text; 0 while there are none.
    double chordWidth = 0;  ///< The chord's text; 0 while none is bound.
    double gap = 0;         ///< The space kept between the counts and the chord.
};

/// Decides whether the footer's chord stays: the counts are what the footer is for and the chord only a
/// shortcut to act on them, so the chord is dropped before the counts lose a word to an ellipsis.
/// @param room The text widths and the room for them.
/// @return Shown when the counts and the chord both fit whole, Dropped otherwise.
[[nodiscard]] constexpr sidebarrows::FooterChord footerChordFor(FooterRoom const& room) noexcept
{
    if (room.chordWidth <= 0)
        return sidebarrows::FooterChord::Dropped;
    auto const needed =
        room.countsWidth > 0 ? room.countsWidth + room.gap + room.chordWidth : room.chordWidth;
    return needed <= room.available ? sidebarrows::FooterChord::Shown : sidebarrows::FooterChord::Dropped;
}

/// Decides which wording of the counts the footer shows: the full one whenever it fits, so the counts are
/// never cut short by an ellipsis while a shorter wording of them would fit.
/// @param available The width the counts have, with no chord beside them.
/// @param fullWidth The full wording's width.
/// @return Full when it fits, Compact otherwise.
[[nodiscard]] constexpr sidebarrows::FooterCounts footerCountsFor(double available, double fullWidth) noexcept
{
    return fullWidth <= available ? sidebarrows::FooterCounts::Full : sidebarrows::FooterCounts::Compact;
}

/// The sidebar tree's own commands for QML (SidebarCommandNames): each name to its window::ChromeCommand
/// value, as sidebarCommandRequested() carries it.
/// @return The map.
[[nodiscard]] QVariantMap sidebarCommandMap();

/// The sidebar's rows as a QAbstractListModel.
///
/// After the first build the model never resets: a rebuild is diffed against the rows it replaces by
/// SidebarRowKey and applied as removals, moves, insertions and per-row dataChanged, so QML keeps its scroll
/// position and its delegates. Whatever raises a change -- the directory, the seen-state, a burst of output
/// -- only marks the model stale; one rebuild runs per event-loop turn.
class SidebarModel: public QAbstractListModel
{
    Q_OBJECT
    Q_PROPERTY(int footerNeedYou READ footerNeedYou NOTIFY footerChanged)
    Q_PROPERTY(int footerWorking READ footerWorking NOTIFY footerChanged)
    /// The footer's counts as words, zeros left out (footerTextOf()); empty while nothing is pending.
    Q_PROPERTY(QString footerText READ footerText NOTIFY footerChanged)
    /// The same counts in fewer letters (footerCompactTextOf()), for the narrowest sidebar.
    Q_PROPERTY(QString footerCompactText READ footerCompactText NOTIFY footerChanged)
    Q_PROPERTY(int currentIndex READ currentIndex NOTIFY currentIndexChanged)
    /// The shown workspace's name, as its row titles it: the breadcrumb's and the workspace chip's.
    Q_PROPERTY(QString currentWorkspaceTitle READ currentWorkspaceTitle NOTIFY summaryChanged)
    /// The row of the shown workspace, or -1: where the RenameWorkspace action edits.
    Q_PROPERTY(int currentWorkspaceIndex READ currentWorkspaceIndex NOTIFY summaryChanged)
    /// The most urgent roll-up of the workspaces the window does not show, an Urgency's underlying value.
    Q_PROPERTY(int otherWorkspacesUrgency READ otherWorkspacesUrgency NOTIFY summaryChanged)
    /// Whether the workspace chip carries its marker (chipMarkerOf()). A bool by the QML-facing carve-out:
    /// the rule's answer, which QML only shows.
    Q_PROPERTY(bool workspaceChipMarked READ workspaceChipMarked NOTIFY summaryChanged)
    /// The marker's glyph, a TabBadgeKind's underlying value: the other workspaces' roll-up while the chip is
    /// marked, TabBadgeKind::None otherwise.
    Q_PROPERTY(int workspaceChipGlyph READ workspaceChipGlyph NOTIFY summaryChanged)
    /// What a screen reader says of the workspace chip (workspaceChipAccessibleName()).
    Q_PROPERTY(QString workspaceChipAccessibleName READ workspaceChipAccessibleName NOTIFY summaryChanged)

  public:
    /// What each row of the model says; QML reads them by the names in roleNames().
    enum Roles : uint16_t
    {
        KindRole = Qt::UserRole + 1, ///< SidebarRowKind.
        DepthRole,                   ///< 0 workspace, 1 tab, 2 pane.
        TitleRole,                   ///< The name; plain text.
        ReasonRole,                  ///< The second line.
        GitTextRole,                 ///< Workspace rows: the branch and its drift.
        TimeTextRole,                ///< The right-hand side.
        TimeKindRole,                ///< TimeKind.
        ProgressRole,                ///< The percent the program reports, or -1.
        UrgencyRole,                 ///< Urgency, rolled up over the panes beneath a parent row.
        GlyphRole,                   ///< TabBadgeKind: which badge to draw.
        ExpansionRole,               ///< RowExpansion.
        SelectedRole,                ///< Whether the window shows this row's tab.
        TreeLinesRole,               ///< The Terminal look's connectors, one TreeGlyph per column.
        AccessibleNameRole,          ///< Kind, state and status as text, for a screen reader.
        ProgramRole,                 ///< The program the reason is about; empty when unknown.
        FolderRole,                  ///< The pane's working directory; empty when unknown.
        ReasonLabelRole,             ///< The reason's words and its separator, set proportional.
        ReasonCommandRole,           ///< The reason's command, set monospace; empty for a plain reason.
        TreeTextRole,                ///< The Terminal look's connectors as text (treeLinesText()).
        MeterTextRole,         ///< The Terminal look's block meter (meterText()), empty without progress.
        HoverDetailRole,       ///< The hover card's `program · folder · time` (hoverDetailOf()).
        ProgressFractionRole,  ///< The progress as 0 to 1, clamped; 0 without one.
        FullReasonRole,        ///< The hover card's reason, whole (SidebarRow::fullReason).
        FullReasonLabelRole,   ///< Its words.
        FullReasonCommandRole, ///< Its command.
        /// The row's kind and status as words, for a screen reader (accessibleDescriptionOf()).
        AccessibleDescriptionRole,
        SelectionRole,         ///< RowSelection: the displayed tab, or the folded row that holds it.
        TreeTextContinuedRole, ///< The Terminal look's connectors on the second line
                               ///< (treeContinuationText()).
    };

    /// @param host The window the sidebar sits in; must outlive the model.
    /// @param attention The window's seen-state; must outlive the model.
    /// @param clock The wall clock the times are measured against; must outlive the model.
    /// @param announcer What a background pane that needs the user is said through; must outlive the model.
    /// @param parent The QObject parent.
    SidebarModel(ISidebarHost& host,
                 TabAttention const& attention,
                 core::platform::IWallClock const& clock,
                 platform::Announcer& announcer,
                 QObject* parent = nullptr);

    /// @name QAbstractListModel
    /// @{
    [[nodiscard]] int rowCount(QModelIndex const& parent = {}) const override;
    [[nodiscard]] QVariant data(QModelIndex const& index, int role) const override;
    [[nodiscard]] QHash<int, QByteArray> roleNames() const override;
    /// @}

    /// @return The rows as shown, in order.
    [[nodiscard]] std::vector<SidebarRow> const& rows() const noexcept { return _rows; }
    /// @return Every workspace's row as the last rebuild made it, in order, whatever the filter or a fold
    ///         hides (SidebarListing::workspaces): what the switcher shows of the window's own daemon.
    [[nodiscard]] std::span<SidebarRow const> workspaceRows() const noexcept { return _workspaceRows; }
    /// @return Panes that need the user, over every pane the window lists (a filter hides none of them).
    [[nodiscard]] int footerNeedYou() const noexcept { return static_cast<int>(_footer.needYou); }
    /// @return Panes at work, over every pane the window lists.
    [[nodiscard]] int footerWorking() const noexcept { return static_cast<int>(_footer.working); }
    /// @return The footer's counts as words; empty while nothing is pending.
    [[nodiscard]] QString footerText() const { return footerTextOf(_footer); }
    [[nodiscard]] QString footerCompactText() const { return footerCompactTextOf(_footer); }

    /// Decides the footer's counts wording for QML (footerCountsFor()), as a sidebarrows::FooterCounts.
    /// @param available The width the counts have alone.
    /// @param fullWidth The full wording's width.
    /// @return The FooterCounts value.
    [[nodiscard]] Q_INVOKABLE int footerCounts(qreal available, qreal fullWidth) const noexcept
    {
        return static_cast<int>(std::to_underlying(footerCountsFor(available, fullWidth)));
    }

    /// Decides the footer's chord for QML (footerChordFor()), as a sidebarrows::FooterChord.
    /// @param available The width the counts and the chord share.
    /// @param countsWidth The counts' whole width.
    /// @param chordWidth The chord's width.
    /// @param gap The space between them.
    /// @return The FooterChord value.
    [[nodiscard]] Q_INVOKABLE int footerChord(qreal available,
                                              qreal countsWidth,
                                              qreal chordWidth,
                                              qreal gap) const noexcept
    {
        return static_cast<int>(std::to_underlying(footerChordFor(FooterRoom {
            .available = available, .countsWidth = countsWidth, .chordWidth = chordWidth, .gap = gap })));
    }
    /// @return The row of the tab the window shows, or -1.
    [[nodiscard]] int currentIndex() const noexcept { return _currentIndex; }
    /// @return The shown workspace's name.
    [[nodiscard]] QString currentWorkspaceTitle() const
    {
        return QString::fromStdString(_summary.currentWorkspace);
    }
    /// @return The row of the shown workspace, or -1.
    [[nodiscard]] int currentWorkspaceIndex() const noexcept { return _currentWorkspaceIndex; }
    /// @return The other workspaces' roll-up, an Urgency's underlying value.
    [[nodiscard]] int otherWorkspacesUrgency() const noexcept
    {
        return static_cast<int>(_summary.otherWorkspaces);
    }
    /// @return Whether the workspace chip carries its marker.
    [[nodiscard]] bool workspaceChipMarked() const noexcept
    {
        return chipMarkerOf(_summary.otherWorkspaces) == ChipMarker::Shown;
    }
    /// @return The marker's glyph, a TabBadgeKind's underlying value.
    [[nodiscard]] int workspaceChipGlyph() const noexcept
    {
        return static_cast<int>(workspaceChipMarked() ? _summary.otherWorkspacesGlyph : TabBadgeKind::None);
    }
    /// @return What a screen reader says of the workspace chip.
    [[nodiscard]] QString workspaceChipAccessibleName() const
    {
        return QString::fromStdString(window::workspaceChipAccessibleName(_summary));
    }
    /// @return Whether anyone can see the rows, as setPresence() last said.
    [[nodiscard]] SidebarPresence presence() const noexcept { return _presence; }
    /// @return Whether the window has the user's attention, as setActivity() last said.
    [[nodiscard]] WindowActivity activity() const noexcept { return _activity; }
    /// @return Whether the one-second timer that keeps the running times current is armed.
    [[nodiscard]] bool isTimerActive() const noexcept { return _timer.isActive(); }
    /// @return Whether a rebuild waits for the event loop.
    [[nodiscard]] bool isRebuildPending() const noexcept { return _rowsState == RowsState::Stale; }
    /// @return Whether a filter narrows the rows.
    [[nodiscard]] bool isFiltered() const noexcept { return !_state.filter.empty(); }

    /// @param key A row's key.
    /// @return The row's index, or -1 when it is not listed.
    [[nodiscard]] int rowIndexOf(SidebarRowKey const& key) const noexcept;

    /// Tells the model whether anyone can see its rows. The one-second timer runs only while they can.
    /// @param presence The sidebar's presence.
    void setPresence(SidebarPresence presence);
    /// Tells the model whether the window has the user's attention.
    /// @param activity The window's activity.
    void setActivity(WindowActivity activity) noexcept { _activity = activity; }

    /// Marks the rows stale: they are rebuilt once, when control returns to the event loop, however many
    /// times this is called before. What a directory change, a seen-state change or a burst of output calls.
    Q_INVOKABLE void noteChanged();
    /// Rebuilds the rows now, applying the difference to the model.
    void refreshNow();

    /// Shows what a row names: a workspace is selected, a tab is shown, a pane is shown and focused; the
    /// pane is counted as seen.
    /// @param row The row.
    Q_INVOKABLE void activate(int row);
    /// What activate() does, said back.
    /// @param row The row.
    /// @return Shown when the row named something that was shown; Nothing for a row the tree does not have
    ///         (an empty or filtered-out tree), which changes nothing.
    [[nodiscard]] sidebarrows::RowActivation activateRow(int row);
    /// Expands a collapsed row, collapses an expanded one.
    /// @param row The row.
    Q_INVOKABLE void toggle(int row);
    /// Expands @p row's children.
    /// @param row The row.
    Q_INVOKABLE void expand(int row);
    /// Hides @p row's children.
    /// @param row The row.
    Q_INVOKABLE void collapse(int row);
    /// The Collapse key of the tree: collapses @p row when it is expanded, else names its parent.
    /// @param row The focused row.
    /// @return The row the focus is on afterwards: @p row, or its parent; @p row when it has none.
    Q_INVOKABLE int collapseOrParent(int row);
    /// The Expand key of the tree: expands @p row when it is collapsed, else names its first child.
    /// @param row The focused row.
    /// @return The row the focus is on afterwards: @p row, or the row after it when that is its child.
    Q_INVOKABLE int expandOrChild(int row);
    /// Renames what @p row names: a workspace's label, or the title a tab shows.
    /// @param row A workspace or tab row; a pane has no name of its own to change.
    /// @param name The new name.
    Q_INVOKABLE void rename(int row, QString const& name);
    /// Counts every pane under @p row as seen.
    /// @param row The row.
    Q_INVOKABLE void markSeen(int row);
    /// Opens the context menu of @p row.
    /// @param row The row.
    /// @param windowPosition Where the pointer is, in window coordinates.
    Q_INVOKABLE void openContextMenu(int row, QPointF windowPosition);
    /// Narrows the rows to those matching @p text (and the ancestors of a match); empty shows everything.
    /// @param text The typed filter.
    Q_INVOKABLE void setFilter(QString const& text);
    /// Drops what a node drag carries on @p row.
    /// @param row The row under the pointer.
    /// @param y The pointer's position within the row, 0 (top) to 1 (bottom).
    /// @param payload The drag's data of type NodeDragMime (parseNodeDrag()); decides what is dragged. A
    ///        payload that does not read as a node drag is refused.
    Q_INVOKABLE void drop(int row, double y, QString const& payload);
    /// What a drag hovering @p row shows (SidebarDropPolicy): where it would land, or that it is refused.
    /// @param row The row under the pointer.
    /// @param y The pointer's position within the row, 0 (top) to 1 (bottom).
    /// @param payload The drag's data of type NodeDragMime.
    /// @return A sidebarrows::DropIndicator.
    [[nodiscard]] Q_INVOKABLE int dropIndicator(int row, double y, QString const& payload) const;
    /// @param payload The drag's data of type NodeDragMime.
    /// @return Whether dropping it on the tree's empty area, or on the New Workspace control, moves it into a
    ///         workspace of its own (a tab or a pane that is not all its workspace holds).
    [[nodiscard]] Q_INVOKABLE bool takesNewWorkspaceDrop(QString const& payload) const;
    /// Drops what a node drag carries on the tree's empty area or the New Workspace control: a workspace of
    /// its own for it, if takesNewWorkspaceDrop().
    /// @param payload The drag's data of type NodeDragMime.
    Q_INVOKABLE void dropOnNewWorkspace(QString const& payload);

  signals:
    /// The footer's counts changed.
    void footerChanged();
    /// The row of the tab the window shows changed.
    void currentIndexChanged();
    /// The shown workspace, its row or what the other workspaces ask changed.
    void summaryChanged();
    /// A rebuild changed a workspace's row (workspaceRows()).
    void workspaceRowsChanged();

  private:
    /// A node drag as the sidebar reads it: what it is to the drop policy, and what moves.
    struct Dragged
    {
        SidebarDragged dragged; ///< Its subject, its tab and its share of its workspace.
        SidebarNode node;       ///< What moves when it lands among the tabs; a OneTabWorkspace's tab.
        uint64_t workspace = 0; ///< A workspace drag's workspace id; 0 otherwise.
    };

    /// @return What @p payload drags, read against the directory; none for one this sidebar does not take
    ///         (unreadable, another host's, or no node).
    [[nodiscard]] std::optional<Dragged> draggedOf(QString const& payload) const;

    /// Carries out @p drop for @p dragged.
    void apply(Dragged const& dragged, SidebarDrop const& drop);

    /// Where the rows stand against what they list.
    enum class RowsState : uint8_t
    {
        Unbuilt = 0, ///< Never built: the first build resets the model instead of diffing against nothing.
        Current,     ///< Built, and nothing has changed since.
        Stale,       ///< Something changed; the rebuild waits for the event loop.
    };

    struct Rebuilt
    {
        std::vector<SidebarRow> rows;
        std::vector<SidebarRow> workspaces; ///< Every workspace's row (SidebarListing::workspaces).
        SidebarFooter footer;
        SidebarSummary summary;
        std::optional<TabRef> displayed;
        std::optional<uint64_t> currentWorkspace; ///< The workspace the window shows.
    };

    [[nodiscard]] SidebarRow const* rowAt(int row) const noexcept;
    [[nodiscard]] WorkspaceDirectory directory() const;
    [[nodiscard]] Rebuilt build(WorkspaceDirectory const& directory) const;
    /// Replaces the rows with @p next in one model reset: the first build's.
    void resetRows(std::vector<SidebarRow> next);
    /// Diffs @p next against the rows and applies it as removals, moves, insertions and dataChanged.
    void apply(std::vector<SidebarRow> next);
    void announceNewNeeds(WorkspaceDirectory const& directory);
    void updateTimer();
    void setExpansion(int row, RowExpansion wanted);

    ISidebarHost& _host;
    TabAttention const& _attention;
    core::platform::IWallClock const& _clock;
    platform::Announcer& _announcer;
    SidebarState _state;
    std::vector<SidebarRow> _rows;
    std::vector<SidebarRow> _workspaceRows; ///< @see workspaceRows()
    SidebarFooter _footer;
    SidebarSummary _summary;
    int _currentIndex = -1;
    int _currentWorkspaceIndex = -1;
    RowsState _rowsState = RowsState::Unbuilt;
    SidebarPresence _presence = SidebarPresence::Hidden;
    WindowActivity _activity = WindowActivity::Inactive;
    /// The source the last rebuild read; a different one is a new baseline, not news.
    IWorkspaceDirectorySource const* _source = nullptr;
    /// Each pane's urgency at the last rebuild, to see which ones just began to need the user.
    std::map<PaneKey, Urgency> _paneUrgency;
    QTimer _timer;
};

} // namespace contour::window
