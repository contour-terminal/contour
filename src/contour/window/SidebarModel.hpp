// SPDX-License-Identifier: Apache-2.0
#pragma once
/// @file
/// The workspace sidebar's list model: a window's WorkspaceDirectory, flattened by sidebarRows() and handed
/// to QML one row per index. What the rows say is decided in SidebarTree.hpp, and the Qt-free tests of it
/// stand there; this adapter owns what only Qt can: the role names, the change signals, the coalescing of a
/// burst of events into one rebuild, the one-second timer behind the running times, and the announcement of
/// a pane that starts to need the user in a workspace the window is not showing.
#include <contour/platform/Announcer.hpp>
#include <contour/window/PaneKey.hpp>
#include <contour/window/SidebarTree.hpp>
#include <contour/window/TabAttention.hpp>
#include <contour/window/WorkspaceDirectorySource.hpp>

#include <core/platform/Clock.hpp>

#include <QtCore/QAbstractListModel>
#include <QtCore/QPointF>
#include <QtCore/QString>
#include <QtCore/QTimer>

#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace contour::window
{

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

    /// @return The tab the window shows, if any.
    [[nodiscard]] virtual std::optional<TabRef> displayedTab() const = 0;

    /// Shows @p pane: selects its workspace, activates its tab, focuses it and counts it as seen.
    /// @param pane The pane to show.
    virtual void showPane(PaneKey pane) = 0;

    /// Counts @p pane as seen.
    /// @param pane The pane the user has looked at.
    virtual void markSeen(PaneKey pane) = 0;

    /// Renames a workspace.
    /// @param workspace The workspace's id.
    /// @param name The new label.
    virtual void renameWorkspace(uint64_t workspace, std::string const& name) = 0;

    /// Moves a workspace in the list.
    /// @param workspace The workspace's id.
    /// @param to The position among all the workspaces to insert before (ReorderWorkspace::to).
    virtual void moveWorkspace(uint64_t workspace, std::size_t to) = 0;

    /// Moves a tab into a workspace.
    /// @param tab The tab.
    /// @param toWorkspace The target workspace's id.
    /// @param toPosition The position among its tabs; AppendTab for the end.
    virtual void moveTab(TabRef tab, uint64_t toWorkspace, std::size_t toPosition) = 0;

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

/// Whether the window has the user's attention, which is when a background pane that starts to need them
/// is worth saying aloud.
enum class WindowActivity : uint8_t
{
    Inactive = 0, ///< Another window or application has focus.
    Active,       ///< This window has focus.
};

/// What a screen reader says of a row: its title, kind, whether it is expanded and what it asks of the user.
/// @param row The row.
/// @return For example `billing, workspace, expanded, 1 needs you`; a row that asks nothing leaves the status
///         out, one with nothing to fold leaves the expansion out.
[[nodiscard]] QString accessibleNameOf(SidebarRow const& row);

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
    Q_PROPERTY(int currentIndex READ currentIndex NOTIFY currentIndexChanged)

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
    /// @return Panes that need the user, over every pane the window lists (a filter hides none of them).
    [[nodiscard]] int footerNeedYou() const noexcept { return static_cast<int>(_footer.needYou); }
    /// @return Panes at work, over every pane the window lists.
    [[nodiscard]] int footerWorking() const noexcept { return static_cast<int>(_footer.working); }
    /// @return The row of the tab the window shows, or -1.
    [[nodiscard]] int currentIndex() const noexcept { return _currentIndex; }
    /// @return Whether anyone can see the rows, as setPresence() last said.
    [[nodiscard]] SidebarPresence presence() const noexcept { return _presence; }
    /// @return Whether the window has the user's attention, as setActivity() last said.
    [[nodiscard]] WindowActivity activity() const noexcept { return _activity; }
    /// @return Whether the one-second timer that keeps the running times current is armed.
    [[nodiscard]] bool isTimerActive() const noexcept { return _timer.isActive(); }
    /// @return Whether a rebuild waits for the event loop.
    [[nodiscard]] bool isRebuildPending() const noexcept { return _rebuildPending; }

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
    /// Expands a collapsed row, collapses an expanded one.
    /// @param row The row.
    Q_INVOKABLE void toggle(int row);
    /// Expands @p row's children.
    /// @param row The row.
    Q_INVOKABLE void expand(int row);
    /// Hides @p row's children.
    /// @param row The row.
    Q_INVOKABLE void collapse(int row);
    /// Renames the workspace of @p row.
    /// @param row A workspace row; others have no name of their own to change.
    /// @param name The new label.
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

  signals:
    /// The footer's counts changed.
    void footerChanged();
    /// The row of the tab the window shows changed.
    void currentIndexChanged();

  private:
    struct Rebuilt
    {
        std::vector<SidebarRow> rows;
        SidebarFooter footer;
        std::optional<TabRef> displayed;
    };

    [[nodiscard]] SidebarRow const* rowAt(int row) const noexcept;
    [[nodiscard]] WorkspaceDirectory directory() const;
    [[nodiscard]] Rebuilt build(WorkspaceDirectory const& directory) const;
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
    SidebarFooter _footer;
    int _currentIndex = -1;
    bool _built = false;
    bool _rebuildPending = false;
    SidebarPresence _presence = SidebarPresence::Hidden;
    WindowActivity _activity = WindowActivity::Inactive;
    /// The source the last rebuild read; a different one is a new baseline, not news.
    IWorkspaceDirectorySource const* _source = nullptr;
    /// Each pane's urgency at the last rebuild, to see which ones just began to need the user.
    std::map<PaneKey, Urgency> _paneUrgency;
    QTimer _timer;
};

} // namespace contour::window
