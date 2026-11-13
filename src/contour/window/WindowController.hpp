// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <contour/command/CommandCatalog.hpp>
#include <contour/command/MenuBarTable.hpp>
#include <contour/display/WindowHost.hpp>
#include <contour/input/HorizontalWheelGesture.hpp>
#include <contour/platform/Announcer.hpp>
#include <contour/platform/CaptionHitTest.hpp>
#include <contour/platform/NativeCaptionButtons.hpp>
#include <contour/platform/NativeWindowFrame.hpp>
#include <contour/platform/TitleBandMaterial.hpp>
#include <contour/platform/WindowResizeGrid.hpp>
#include <contour/platform/WindowShadowController.hpp>
#include <contour/session/InProcessDirectory.hpp>
#include <contour/session/NodeDragSession.hpp>
#include <contour/session/PaneProxy.hpp>
#include <contour/window/ChromeFocus.hpp>
#include <contour/window/ChromeKeys.hpp>
#include <contour/window/CloseDialogTable.hpp>
#include <contour/window/CommandPaletteModel.hpp>
#include <contour/window/ContextMenuModel.hpp>
#include <contour/window/HeldModifierReveal.hpp>
#include <contour/window/MenuRouting.hpp>
#include <contour/window/NodeDrag.hpp>
#include <contour/window/NoticeModel.hpp>
#include <contour/window/PaneHeaderPolicy.hpp>
#include <contour/window/PaneKey.hpp>
#include <contour/window/ProgramStatusBadge.hpp>
#include <contour/window/SaveLayoutCheck.hpp>
#include <contour/window/SecureInputPolicy.hpp>
#include <contour/window/SidebarMenuState.hpp>
#include <contour/window/SidebarModel.hpp>
#include <contour/window/SidebarToggle.hpp>
#include <contour/window/TabAttention.hpp>
#include <contour/window/TabBadge.hpp>
#include <contour/window/TabBarDropPolicy.hpp>
#include <contour/window/TabBarHitValue.hpp>
#include <contour/window/TitleBandGeometry.hpp>
#include <contour/window/TitleBandLayout.hpp>
#include <contour/window/TitleBandSurface.hpp>
#include <contour/window/WindowId.hpp>
#include <contour/window/WindowPositionStore.hpp>
#include <contour/window/WindowRole.hpp>
#include <contour/window/WorkspaceDirectorySource.hpp>

#include <vtbackend/core/Color.hpp>

#include <core/platform/Clock.hpp>

#include <QtCore/QAbstractListModel>
#include <QtCore/QCoreApplication>
#include <QtCore/QPointer>
#include <QtCore/QRectF>
#include <QtCore/QTimer>
#include <QtCore/QVariant>
#include <QtGui/QColor>
#include <QtQml/qqmlregistration.h>
#include <QtQuick/QQuickWindow>

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <unordered_map>
#include <vector>

#include <vtworkspace/Primitives.hpp>

class QScreen;

namespace vtworkspace
{
class Tab;
class Window;
} // namespace vtworkspace

namespace contour::display
{
class TerminalDisplay;
} // namespace contour::display

namespace contour::session
{
class TerminalSessionManager;
class TerminalSession;
} // namespace contour::session

namespace contour::window
{

/// The random token that tells this process's drags from any other's: generated once, on first use, and the
/// same for the rest of the process's life.
/// @return The token, which a node drag's payload carries as its host's id
/// (TerminalSessionManager::hostIdentity).
[[nodiscard]] QString const& processToken();

class PromptHost;

/// A place a context menu opens from. Each holds its own published menu, so opening one never rebuilds
/// -- or re-targets -- another the user may still have open.
enum class MenuSurface : uint8_t
{
    Pane = 0, ///< The terminal pane's menu; acts on the pane it was opened over.
    TitleBar, ///< The title bar's menu; acts on the active session.
    Tab,      ///< A tab's menu; acts on the tab it was opened over.
    NewTab,   ///< The new-tab button's profile menu; acts on the active session.
    MainMenu, ///< The header bar's main menu: the menu bar's menus; acts on the active session.
    Sidebar,  ///< A sidebar row's menu (workspace, tab or pane); acts on the row it was opened over.
};

/// How many MenuSurface values there are.
inline constexpr std::size_t MenuSurfaceCount = static_cast<std::size_t>(MenuSurface::Sidebar) + 1;

/// @return The production NoticeAnnouncerFactory: Qt's accessibility announcements (platform::QtAnnouncer).
[[nodiscard]] NoticeAnnouncerFactory qtNoticeAnnouncer();

/// Per-OS-window Qt/QML adapter over one vtworkspace::Workspace.
///
/// Clean-architecture role: this is the ADAPTER between the pure, Qt-free domain core (vtworkspace::Workspace
/// / SessionModel) and the QML window (ApplicationWindow / Main.qml). One instance exists per OS window. It
/// owns everything window-scoped and Qt-shaped:
///   - the tab-strip QAbstractListModel (rows = this window's tabs) + the tab invokables the strip calls;
///   - the PaneProxy tree for the active tab (the recursive PaneNode.qml renders from activeTabRootPane);
///   - this window's focused display (_activeDisplay) and the window services derived from it
///     (titleBarVisible), bridged from the display's own signals;
///   - the WINDOW-GEOMETRY AUTHORITY: initial pre-show sizing, WM size hints, show modes and the
///     grid->window choke point (see the "Window-geometry authority" section below). Nothing else
///     mutates QWindow geometry.
///
/// It holds NO session lifetime and NO authority over the model tree. Structural mutations
/// (create/close/move tab, split/close/focus pane, terminate) are delegated to the
/// TerminalSessionManager SERVICE, tagged with this controller's WorkspaceId, so the model stays the single
/// source of truth and session ownership stays in one place. The manager also routes each vtworkspace
/// ModelEvent to the owning window's controller (the on*() hooks below).
///
/// Created only by TerminalSessionManager::createWindowController() (which mints the backing
/// vtworkspace::Workspace) and owned via QQmlEngine CppOwnership; destroyed when its ApplicationWindow
/// closes.
///
/// It is also the window half of the display/window seam: it implements @c display::WindowHost, the
/// interface the display layer declares for "the OS window I am shown in". Everything a display asks of
/// its window arrives through those overrides, so the display names no window type at all.
class WindowController:
    public QAbstractListModel,
    public command::TabTitleProvider,
    public command::DaemonRowsProvider,
    public display::WindowHost,
    public session::InProcessWindow
{
    Q_OBJECT
    Q_PROPERTY(int count READ count NOTIFY countChanged)
    /// The pane whose header carries the node drag this window owns: it shows the placeholder where its card
    /// was. Null while no pane of this window is carried (a tab's drag carries none).
    Q_PROPERTY(QObject* carriedPane READ carriedPane NOTIFY carriedPaneChanged)
    /// This window's command palette list. Per-window (not per-app) because the filter text and the
    /// selection are things the user is doing IN this window; the recently-used list behind it is
    /// app-wide and lives on the manager.
    Q_PROPERTY(contour::window::CommandPaletteModel* commandPalette READ commandPalette CONSTANT)
    // This window's notices: the toast and the banner (never null; owned by this controller).
    Q_PROPERTY(contour::window::NoticeModel* notices READ noticesForQml CONSTANT)
    /// The workspace sidebar's rows (SidebarModel).
    Q_PROPERTY(QObject* sidebar READ sidebarForQml CONSTANT)
    Q_PROPERTY(QVariantList sidebarContextMenuModel READ sidebarContextMenuModel NOTIFY
                   sidebarContextMenuModelChanged)
    Q_PROPERTY(QVariantList contextMenuModel READ contextMenuModel NOTIFY contextMenuModelChanged)
    Q_PROPERTY(QVariantList titleBarContextMenuModel READ titleBarContextMenuModel NOTIFY
                   titleBarContextMenuModelChanged)
    Q_PROPERTY(QVariantList tabContextMenuModel READ tabContextMenuModel NOTIFY tabContextMenuModelChanged)
    Q_PROPERTY(QVariantList newTabMenuModel READ newTabMenuModel NOTIFY newTabMenuModelChanged)
    Q_PROPERTY(QVariantList mainMenuModel READ mainMenuModel NOTIFY mainMenuModelChanged)
    /// The radius of the window's top corners, which the title band rounds: the chrome row's
    /// @c radii.window while the window is windowed under client-side decorations, 0 otherwise.
    Q_PROPERTY(int windowCornerRadius READ windowCornerRadius NOTIFY windowCornerRadiusChanged)
    Q_PROPERTY(int activeTabIndex READ activeTabIndex NOTIFY activeTabIndexChanged)
    // Keyboard focus in the chrome: a window::ChromeArea as its underlying value (Terminal means the chrome
    // has no focus, so the focus ring is hidden), and the focused item of the tab well and of the toolbar
    // (-1 outside its area). Set only by the focus actions and by keys inside the chrome, never by a pointer
    // press, so a ring that shows is a ring for keyboard focus.
    Q_PROPERTY(int chromeFocusArea READ chromeFocusArea NOTIFY chromeFocusChanged)
    Q_PROPERTY(int focusedTabIndex READ focusedTabIndex NOTIFY chromeFocusChanged)
    Q_PROPERTY(int focusedToolbarIndex READ focusedToolbarIndex NOTIFY chromeFocusChanged)
    Q_PROPERTY(bool multimediaReady READ isMultimediaReady NOTIFY multimediaReadyChanged)
    Q_PROPERTY(
        contour::session::PaneProxy* activeTabRootPane READ activeTabRootPane NOTIFY activeTabRootPaneChanged)
    Q_PROPERTY(
        contour::session::TerminalSession* activeSession READ activeSession NOTIFY activeSessionChanged)
    // The active session's sticky header as the title band shows it (TerminalSession::stickyHeader): empty
    // unless that session's header lives in the band or a pane header, and has a block to name.
    Q_PROPERTY(QVariantMap activeStickyHeader READ activeStickyHeader NOTIFY activeStickyHeaderChanged)
    Q_PROPERTY(bool titleBarVisible READ titleBarVisible NOTIFY titleBarVisibleChanged)
    // All four follow titleBarVisible, so they re-evaluate on the same signal.
    Q_PROPERTY(bool needsFramelessHint READ needsFramelessHint NOTIFY titleBarVisibleChanged)
    // What the window is for: its WindowRole as an int, and the Qt window flags its WindowLevel adds to the
    // frame policy's. Both are fixed at construction, so CONSTANT; ints because QML names no C++ enum here.
    Q_PROPERTY(int windowRole READ windowRoleForQml CONSTANT)
    Q_PROPERTY(int roleFlags READ roleFlags CONSTANT)
    Q_PROPERTY(bool needsExpandedClientArea READ needsExpandedClientArea NOTIFY titleBarVisibleChanged)
    Q_PROPERTY(
        bool needsClientFrameAffordances READ needsClientFrameAffordances NOTIFY titleBarVisibleChanged)
    // NOT on titleBarVisibleChanged: this one is measured from the OS's own buttons, which only
    // exist once the window is realized -- long after the title-bar value is seeded.
    Q_PROPERTY(qreal nativeControlsInset READ nativeControlsInset NOTIFY nativeControlsInsetChanged)
    // The title band's measured geometry and, on macOS, whether Qt clears the title bar's background
    // behind it -- all three re-decided by refreshTitleBand().
    Q_PROPERTY(qreal titleBandHeight READ titleBandHeight NOTIFY titleBandChanged)
    Q_PROPERTY(qreal titleBandRevealTop READ titleBandRevealTop NOTIFY titleBandChanged)
    Q_PROPERTY(bool clearsTitleBarBackground READ clearsTitleBarBackground NOTIFY titleBandChanged)
    // What this window's band paints, a config::BandFill as its underlying value: the row's surface, unless
    // this window's frame hosts no material or its material failed.
    Q_PROPERTY(int titleBandFill READ titleBandFill NOTIFY titleBandChanged)
    // How the maximise button looks while the OS drives it (Snap Layouts): two QML-facing predicates rather
    // than the enum's number, so WindowControls.qml names no enumerator value.
    Q_PROPERTY(bool nativeMaximizeHovered READ nativeMaximizeHovered NOTIFY nativeMaximizeVisualChanged)
    Q_PROPERTY(bool nativeMaximizePressed READ nativeMaximizePressed NOTIFY nativeMaximizeVisualChanged)
    // Tab-strip (tab bar) placement + visibility, exposed to Main.qml. `tabBarPosition` is an int
    // (0 = Top, 1 = Bottom) matching the TabBarPosition enumerator order. `tabBarShouldShow`
    // is the resolved gate (mode + live tab count) the QML binds its `visible` to.
    Q_PROPERTY(int tabBarPosition READ tabBarPosition NOTIFY tabBarPositionChanged)
    Q_PROPERTY(int tabBarVisibility READ tabBarVisibility NOTIFY tabBarVisibilityChanged)
    Q_PROPERTY(bool tabBarShouldShow READ tabBarShouldShow NOTIFY tabBarShouldShowChanged)
    // The workspace sidebar, which Main.qml lays out at the window's left edge. `sidebarVisibility` is a
    // config::SidebarVisibility's underlying value (0 = Hidden, 1 = Shown); `sidebarWidth` is the width it
    // takes in logical pixels, 0 while hidden -- the horizontal chrome the window declares (chrome()).
    // `tabPlacement` is a config::TabPlacement's (0 = Top, 1 = Sidebar).
    Q_PROPERTY(int sidebarVisibility READ sidebarVisibility NOTIFY sidebarVisibilityChanged)
    Q_PROPERTY(int sidebarWidth READ sidebarWidth NOTIFY sidebarWidthChanged)
    Q_PROPERTY(int tabPlacement READ tabPlacement NOTIFY tabPlacementChanged)
    /// What the workspaces this window does not show ask of the user: the most urgent of their roll-ups, an
    /// Urgency's underlying value (SidebarModel::otherWorkspacesUrgency).
    Q_PROPERTY(int otherWorkspacesUrgency READ otherWorkspacesUrgency NOTIFY otherWorkspacesUrgencyChanged)
    /// Whether the keyboard is in the sidebar's tree (the chrome area is ChromeArea::Sidebar). A bool by the
    /// QML-facing carve-out: Main.qml moves the real focus by it.
    Q_PROPERTY(bool sidebarHasFocus READ sidebarHasFocus NOTIFY chromeFocusChanged)
    /// The sidebar commands sidebarCommandRequested() carries, by the name QML handles each with
    /// (`previous`, `next`, `first`, `last`, `activate`, `collapse`, `expand`, `rename`, `filter`).
    Q_PROPERTY(QVariantMap sidebarCommands READ sidebarCommands CONSTANT)
    Q_PROPERTY(int chromeHeight READ chromeHeight WRITE setChromeHeight NOTIFY chromeHeightChanged)
    Q_PROPERTY(QString hostingNotice READ hostingNotice NOTIFY hostingNoticeChanged)
    // The host of the window's own binding: it changes as the window is bound, and when its binding stops.
    Q_PROPERTY(QString hostLabel READ hostLabel NOTIFY hostLabelChanged)
    // The window's own daemon's drift (window::DaemonDriftModel), which its drift badge shows; null for a
    // local window. Read anew whenever the window is bound to another host.
    Q_PROPERTY(QObject* daemonDrift READ daemonDrift NOTIFY hostLabelChanged)
    // The window's host for ssh's login prompts (window::PromptHost), which its AskpassDialog shows.
    Q_PROPERTY(QObject* promptHost READ promptHost CONSTANT)
    // A window::HostChipState as its underlying value: where the connection to that host stands.
    Q_PROPERTY(int hostChipState READ hostChipState NOTIFY hostChipStateChanged)
    Q_PROPERTY(QString applicationVersion READ applicationVersion CONSTANT)
    // {{{ Panes
    // The panes the active tab shows: the leaves under its layout root (1 while zoomed or unsplit, 0 without
    // a tab), which decides whether the panes wear headers.
    Q_PROPERTY(int visiblePaneCount READ visiblePaneCount NOTIFY visiblePaneCountChanged)
    // A window::HeaderReveal as its underlying value: whether the reveal modifier, held alone long enough,
    // shows this window's pane headers where the density hides them.
    Q_PROPERTY(int headerReveal READ headerReveal NOTIFY headerRevealChanged)
    // }}}
    QML_ELEMENT
    QML_UNCREATABLE("Created by the session manager")

  public:
    /// Tab-strip roles (one row per tab). Mirrors the old TerminalSessionManager::Roles so the delegates
    /// bind the same role names. The values are small (Qt::UserRole + n), so a 16-bit base suffices and
    /// they still convert cleanly to the `int role` of the QAbstractItemModel API.
    enum class Roles : std::uint16_t
    {
        DisplayRole = Qt::DisplayRole,    //!< Qt's built-in display role; answered like SessionIdRole.
        TitleRole = Qt::UserRole + 1,     //!< Resolved (expanded) tab label; what the tab strip displays.
        ColorRole = Qt::UserRole + 2,     //!< Tab accent color as QColor (transparent if uncolored).
        IsActiveRole = Qt::UserRole + 3,  //!< Whether this tab is the active tab of this window.
        PaneCountRole = Qt::UserRole + 4, //!< Number of panes in this tab.
        SessionIdRole = Qt::UserRole + 5, //!< The session id of the tab's active leaf.
        RawTitleRole = Qt::UserRole + 6,  //!< Un-expanded runtime rename template (empty if never renamed).
        ZoomedRole = Qt::UserRole + 7, //!< Whether this tab's active pane is zoomed (see vtworkspace::Tab).
        //!< The active pane's OSC 9;4 progress state, as the vtbackend::ProgressState enumerator's
        //!< value (0 = inactive, i.e. nothing to paint). @see vtbackend::ProgressState.
        ProgressStateRole = Qt::UserRole + 8,
        ProgressPercentageRole = Qt::UserRole + 9, //!< The active pane's progress percentage, 0..100.
        //!< The tab's badge, as the TabBadgeKind enumerator's value (0 = none): the winner among its panes'
        //!< OSC 7501 records, their unseen finished commands and unseen output. @see TabBadge.hpp.
        BadgeRole = Qt::UserRole + 10,
        //!< The OSC 7501 tooltip, disarmed, when the program status is what the badge shows; else empty.
        BadgeTextRole = Qt::UserRole + 11,
        //!< The progress of a Working badge, 0..100, or -1 when the program gave none (a plain spinner).
        BadgeProgressRole = Qt::UserRole + 12,
        //!< The strongest window::SecureIndicator of the tab's panes, as its value (0 for no lock).
        SecureRole = Qt::UserRole + 13,
    };

    /// @param manager  The session-lifetime service + model host (must outlive this controller).
    /// @param windowId This window's identity, minted by the manager; it outlives any one workspace.
    /// @param workspace The workspace this window displays at first (and the first it owns).
    /// @param menuPresentation Whether this window's menus open natively or in the scene: the app's
    ///                 ContourGuiApp::menuPresentation() in production, fixed for the window's life.
    /// @param initialPosition The first window's remembered location, or where a window opened for a dropped
    ///                 pane or tab goes; absent for other windows.
    /// @param clock    What the held-modifier reveal measures its hold against.
    /// @param wallClock What the sidebar's running times are measured against.
    /// @param noticeAnnouncer Builds what the window's notices are said through; the window says them only
    ///                 while `accessibility_announcements` allows.
    /// @param role     What the window is for; fixed for its life.
    WindowController(session::TerminalSessionManager& manager,
                     WindowId windowId,
                     vtworkspace::WorkspaceId workspace,
                     config::MenuPresentation menuPresentation,
                     std::optional<WindowPosition> initialPosition = std::nullopt,
                     core::platform::IClock const& clock = core::platform::defaultSteadyClock(),
                     core::platform::IWallClock const& wallClock = core::platform::defaultSystemWallClock(),
                     NoticeAnnouncerFactory const& noticeAnnouncer = qtNoticeAnnouncer(),
                     WindowRole role = WindowRole::Normal);
    ~WindowController() override;

    WindowController(WindowController const&) = delete;
    WindowController& operator=(WindowController const&) = delete;
    WindowController(WindowController&&) = delete;
    WindowController& operator=(WindowController&&) = delete;

    /// @return This window's identity, stable while it displays one workspace after another.
    [[nodiscard]] WindowId windowId() const noexcept { return _windowId; }

    /// @return The workspace whose tabs the tab strip lists and whose active tab fills the window.
    [[nodiscard]] vtworkspace::WorkspaceId displayedWorkspace() const noexcept override { return _displayed; }

    /// @return Every workspace this window owns, in the order it took them; the displayed one is among them.
    ///         Sessions of the others keep running while hidden.
    [[nodiscard]] std::span<vtworkspace::WorkspaceId const> ownedWorkspaces() const noexcept override
    {
        return _owned;
    }

    /// Shows @p workspace in this window: the tab strip and the active-tab proxies are rebound to it, and the
    /// tab model announces a reset. A workspace this window does not own, and the one already shown, change
    /// nothing.
    /// @param workspace The owned workspace to show.
    void displayWorkspace(vtworkspace::WorkspaceId workspace) override;

    /// @return Where this window lists the workspaces it can show: its binding's (a daemon's) or its own
    ///         process's; null while it has none.
    [[nodiscard]] IWorkspaceDirectorySource* directorySource() const noexcept { return _directorySource; }

    /// Points the window at the source of the workspaces it lists. A rebinding seam, not configuration: a
    /// window moves between a daemon's workspaces and its own process's as it is bound, which the manager
    /// follows (TerminalSessionManager::bindWindow).
    /// @param source The source; borrowed, and it must outlive its use here, or be replaced first.
    /// A jump, a row action or a follow that waited for the old source is given up.
    void setDirectorySource(IWorkspaceDirectorySource* source) noexcept;

    /// When @p workspace is one of several this window owns and holds no tab, ends it: the window shows the
    /// next owned one first, if it was showing this, and the manager forgets the workspace
    /// (session::TerminalSessionManager::forgetWorkspace). Nothing otherwise (the last workspace is the
    /// window's to close). In-process, and for a daemon window the one it switched away from.
    /// @param workspace The workspace that may have become empty.
    void dropWorkspaceIfEmpty(vtworkspace::WorkspaceId workspace);

    /// Takes @p workspace among those this window owns, without showing it.
    /// @param workspace A workspace of the model no window owns yet.
    void adoptWorkspace(vtworkspace::WorkspaceId workspace);

    /// @return What this window has not yet seen of its panes' output and finished commands.
    [[nodiscard]] TabAttention& tabAttention() noexcept { return _attention; }
    /// @copydoc tabAttention()
    [[nodiscard]] TabAttention const& tabAttention() const noexcept { return _attention; }
    /// @return Where this window is placed as it is first shown -- its remembered place, or where a pane
    ///         dropped outside every window opened it -- until that first expose places it.
    [[nodiscard]] std::optional<WindowPosition> const& initialPosition() const noexcept
    {
        return _initialPosition;
    }

    // {{{ QAbstractListModel (tab strip)
    [[nodiscard]] QVariant data(QModelIndex const& index, int role = Qt::DisplayRole) const override;
    [[nodiscard]] int rowCount(QModelIndex const& parent = QModelIndex()) const override;
    [[nodiscard]] QHash<int, QByteArray> roleNames() const override;
    // }}}

    /// Watches the bound window for QEvent::DevicePixelRatioChange (no QWindow signal exists for it), and
    /// for the key and mouse input the held-modifier reveal follows. Consumes nothing.
    bool eventFilter(QObject* watched, QEvent* event) override;

    /// Tab count of this window (one row per tab).
    [[nodiscard]] int count() const noexcept;
    /// 0-based row of this window's active tab, or -1.
    [[nodiscard]] int activeTabIndex() const noexcept;

    // {{{ GUI tab-strip invokables (delegated to the manager, tagged with _windowId)
    /// Opens a new tab. With an empty @p profileName the app-default profile is used; otherwise the
    /// named profile is launched (the new-tab profile dropdown). The default argument keeps the bare
    /// `createNewTab()` call site (the "+" button) working.
    Q_INVOKABLE void createNewTab(QString const& profileName = {});
    Q_INVOKABLE void activateTab(int index);
    /// The working directory of the tab at @p index, for its hover tooltip.
    ///
    /// Queried on demand rather than carried as a model role: the directory changes with every `cd`, and
    /// a role would have to emit dataChanged for each one to stay honest. A tooltip is only read when it
    /// opens, so asking then is both simpler and always current.
    ///
    /// Written with `~` only when the directory is on this machine; any other is shown as reported,
    /// made safe to show.
    ///
    /// @param index The tab's row.
    /// @return The directory, or an empty string when none can be determined.
    Q_INVOKABLE [[nodiscard]] QString tabWorkingDirectory(int index) const;
    /// Routes a wheel event that occurred over the tab strip.
    ///
    /// Horizontal notches resolve through the same built-in fallback mouse mappings the terminal grid
    /// consults (@see TerminalSession::applyFallbackMouseBinding), so the tab-switch behaviour and its
    /// config gate live in exactly one table. The vertical component is ignored: the strip does not
    /// scroll.
    ///
    /// Takes the whole event rather than its horizontal angle alone: a trackpad reports pixel deltas and
    /// no angle at all, so a swipe judged only by angle would never move a tab, and the gesture phase and
    /// the platform's natural-scrolling flag are what decide which way it should move.
    ///
    /// @param pixelDeltaX Horizontal pixel-precise delta (trackpads), or 0.
    /// @param pixelDeltaY Vertical pixel-precise delta, used to tell a sideways gesture from a vertical one.
    /// @param angleDeltaX Horizontal angle delta, in the same units Qt reports (120 per notch).
    /// @param angleDeltaY Vertical angle delta, used to tell a sideways gesture from a vertical one.
    /// @param phase       The gesture phase, as a vtbackend::ScrollPhase (QML has no enum to pass).
    /// @param inverted    Whether the platform already flipped the delta for natural scrolling.
    Q_INVOKABLE void dispatchTabStripWheel(
        int pixelDeltaX, int pixelDeltaY, int angleDeltaX, int angleDeltaY, int phase, bool inverted);
    Q_INVOKABLE void moveTab(int fromIndex, int toIndex);
    /// The `application/x-contour-node` payload of a drag of the pane showing @p pane.
    /// @param pane The pane's TerminalSession.
    /// @return The JSON; empty for a pane that cannot be dragged (none, or one its host has no id for yet).
    [[nodiscard]] QString paneDragPayload(session::TerminalSession const* pane) const;
    /// The `application/x-contour-node` payload of a drag of the tab at @p tabIndex: a tab of one pane
    /// drags as that pane.
    /// @param tabIndex The tab's row in this window.
    /// @return The JSON; empty for no such tab, or one its host has no id for yet.
    [[nodiscard]] QString tabDragPayload(int tabIndex) const;
    /// What kind of node @p nodePayload drags, for the DropArea that has only the JSON in hand.
    /// @param nodePayload The drag's `application/x-contour-node` JSON.
    /// @return A DropPayload, as its value: Pane or Tab; None for JSON that is no node drag.
    Q_INVOKABLE [[nodiscard]] int dropPayloadOf(QString const& nodePayload) const;
    /// The name of what @p nodePayload drags, for the slot a tab strip shows where it would land: the title
    /// of the dragged pane, or of the first pane of the dragged tab.
    /// @param nodePayload The drag's JSON.
    /// @return The title; empty for a drag this process does not show, or JSON that is no node drag.
    Q_INVOKABLE [[nodiscard]] QString dragTitle(QString const& nodePayload) const;
    /// What dropping something over a pane would do, for the DropArea of a pane card to show and to act on.
    /// Decided by resolveDropZone() from what this window can do and what is dragged.
    ///
    /// Files and text insert into the pane unless its session has ended (typed text would be swallowed, or
    /// worse, restart it). A pane or tab is refused where it cannot move into this window
    /// (TerminalSessionManager::refusalFor), is a no-op over itself or when it has gone since the drag
    /// began; otherwise a pane splits in an edge band and swaps in the middle, and a tab splits only from
    /// another tab and only in an edge band (PassThrough elsewhere: not accepted).
    /// @param pane         The pane's TerminalSession; null resolves to DropZone::None.
    /// @param payload     What is dragged, as a DropPayload; out of range resolves to DropZone::None.
    /// @param x           The pointer's distance from the pane's left edge.
    /// @param y           The pointer's distance from the pane's top edge.
    /// @param width       The pane card's width.
    /// @param height      The pane card's height.
    /// @param nodePayload The drag's `application/x-contour-node` JSON; read only for a pane or tab. It comes
    /// from
    ///                    whatever started the drag, so one that does not parse resolves to DropZone::None.
    /// @return A DropZone, as its value.
    Q_INVOKABLE [[nodiscard]] int dropZoneAt(session::TerminalSession const* pane,
                                             int payload,
                                             qreal x,
                                             qreal y,
                                             qreal width,
                                             qreal height,
                                             QString const& nodePayload) const;
    /// The label of the ghost shown for @p zone when @p payload is dragged over @p target ("Pane → split
    /// right", "Swap with <pane>"); empty for a zone with no ghost.
    /// @param zone    A DropZone, as its value.
    /// @param payload A DropPayload, as its value.
    /// @param target  The pane hovered, whose title a swap names.
    Q_INVOKABLE [[nodiscard]] QString dropGhostLabel(int zone,
                                                     int payload,
                                                     session::TerminalSession const* target) const;
    /// The body of the refusal shown when @p nodePayload cannot move into this window (refusalTextOf()).
    /// @param nodePayload The drag's JSON.
    /// @return The text; empty when nothing refuses it, or it does not parse.
    Q_INVOKABLE [[nodiscard]] QString refusalText(QString const& nodePayload) const;
    /// Drops @p nodePayload on @p pane in @p zone: beside it in a split zone, onto it (trading places) in the
    /// swap zone. Any other zone changes nothing. Everything is decided again here, not taken from the QML
    /// caller: the zone must be one the drag can have here, and a refused drag moves nothing.
    /// @param pane        The pane's TerminalSession.
    /// @param zone        The zone dropZoneAt() answered, as its value.
    /// @param nodePayload The drag's JSON.
    Q_INVOKABLE void dropNodeOnPane(session::TerminalSession const* pane,
                                    int zone,
                                    QString const& nodePayload);
    /// Drops @p nodePayload on the tab at @p tabIndex: to the right of that tab's active pane.
    /// @param tabIndex    The tab's row in this window.
    /// @param nodePayload The drag's JSON.
    Q_INVOKABLE void dropNodeOnTab(int tabIndex, QString const& nodePayload);
    /// Drops @p nodePayload in the gap before the tab at @p index (@p index of the tab count: after the
    /// last): a tab of its own there; a tab of this window's is reordered to it.
    /// @param index       The gap, as resolveTabBarHit() names it.
    /// @param nodePayload The drag's JSON.
    Q_INVOKABLE void dropNodeInGap(int index, QString const& nodePayload);
    /// Records where the tab strip's tabs are, for tabBarHit(). The window lays the strip out, not this
    /// controller: externally-driven geometry, reported by the strip as a drag enters it and whenever it
    /// lays out while the drag is over it.
    /// @param tabs Each tab's rectangle in the strip, left to right; only the horizontal extent is read.
    Q_INVOKABLE void setTabSpans(QList<QRectF> const& tabs);
    /// Hit-tests the tab strip (resolveTabBarHit()) against the spans setTabSpans() last reported.
    /// @param x The pointer's position along the strip.
    /// @return What the pointer is on (`hit.spot`, a TabBarSpot) and which tab or gap (`hit.index`).
    Q_INVOKABLE [[nodiscard]] TabBarHitValue tabBarHit(qreal x) const;
    /// Whether resting a drag of @p nodePayload on the tab at @p tabIndex opens that tab
    /// (shouldSpringOpen()): never the tab dragged, nor the one the drag came from, nor in a window that
    /// refuses the drag.
    /// @param tabIndex    The hovered tab's row.
    /// @param nodePayload The drag's JSON.
    /// @return Whether it opens. A `bool` because this is the Qt boundary: QML reads it in a condition; the
    ///         decision itself is DwellOpens.
    Q_INVOKABLE [[nodiscard]] bool springOpens(int tabIndex, QString const& nodePayload) const;
    /// Starts the drag of @p pane by its header: from here until finishNodeDrag(), the window closing or the
    /// next drag beginning, the session manager holds the drag for this window -- a delegate only forwards
    /// -- and the moves its drops ask for wait until it has returned (the delegate the OS drag loop runs on
    /// must outlive the loop). The pane is carried (carriedPane()) for as long. A drag still running is
    /// ended first, in its own window.
    /// @param pane The pane's TerminalSession.
    /// @return The `application/x-contour-node` JSON the drag carries; empty for a pane that cannot be
    ///         dragged, which starts no drag.
    Q_INVOKABLE [[nodiscard]] QString beginPaneDrag(session::TerminalSession* pane);
    /// Starts the drag of the tab at @p tabIndex, as beginPaneDrag() does a pane's; no pane is carried.
    /// @param tabIndex The tab's row in this window.
    /// @return The JSON; empty for a tab that cannot be dragged, which starts no drag.
    Q_INVOKABLE [[nodiscard]] QString beginTabDrag(int tabIndex);
    /// Ends the drag this window began, exactly once; with none running it does nothing. A drop some
    /// Contour window took (@p dropAction is not Qt::IgnoreAction) did its work there -- every window takes
    /// every pane and tab dropped on it, as a no-op where nothing else did -- so one no window took was
    /// dropped outside or cancelled. The app's DragEndProbe tells which, as Qt reports Escape like a drop
    /// outside; a drop outside opens a window of its own where the probe says it ended.
    /// @param dropAction The Qt::DropAction the drag ended with, as its value.
    Q_INVOKABLE void finishNodeDrag(int dropAction);
    /// Ends the node drag this window owns, if any, without acting on how it ended: its carrier is gone
    /// before the OS drag returned (NodeDragSource's destruction), so finishNodeDrag() never comes. The
    /// moves its drops held are made then.
    Q_INVOKABLE void abandonNodeDrag();
    /// @return The pane whose header carries the drag this window began; null for none.
    [[nodiscard]] QObject* carriedPane() const;

    /// Says @p announcement through this window's screen reader. Public because it is the session
    /// manager's seam: the manager composes the text and calls the receiving window, which for a move into
    /// a new window does not exist yet when the drop is handled.
    /// @param announcement What to say.
    void announceMove(QString const& announcement);
    /// The ghost's rectangle for @p zone: the half of the pane on the zone's side (dropGhostOf()), in the
    /// pane's coordinates.
    /// @param zone   A DropZone, as its value.
    /// @param width  The pane card's width.
    /// @param height The pane card's height.
    /// @param inset  How far the ghost sits in from the pane's edges.
    /// @return The rectangle; a null one for a zone that is not a split.
    Q_INVOKABLE [[nodiscard]] QRectF dropGhostRect(int zone, qreal width, qreal height, qreal inset) const;
    Q_INVOKABLE void setTabTitle(int index, QString const& title);
    Q_INVOKABLE void resetTabTitle(int index);
    /// Asks the active tab's QML delegate to open its inline title editor (the keyboard entry point
    /// for the SetTabTitle action). No-op when this window has no active tab. Emits
    /// tabTitleEditRequested() with the active tab's row so exactly that TabItem starts editing.
    Q_INVOKABLE void beginActiveTabTitleEdit();
    /// Asks the active tab's QML delegate to open its color picker (the keyboard entry point for the
    /// SetTabColor action when it names no color). No-op when this window has no active tab. Emits
    /// tabColorPickRequested() with the active tab's row so exactly that TabItem opens its flyout.
    Q_INVOKABLE void beginActiveTabColorPick();

    // {{{ Keyboard focus in the chrome
    /// @return The focused window::ChromeArea as its underlying value.
    [[nodiscard]] int chromeFocusArea() const noexcept { return static_cast<int>(_chromeArea); }
    /// @return The row of the focused tab while the tab well has focus, else -1.
    [[nodiscard]] int focusedTabIndex() const noexcept;
    /// @return The focused tab while the tab well has focus, else nothing. The focus belongs to this tab, so
    /// an insert, a move or a close elsewhere in the strip never makes it name another one.
    [[nodiscard]] std::optional<vtworkspace::TabId> focusedTab() const noexcept { return _focusedTab; }
    /// @return The index of the focused toolbar item while the toolbar has focus, else -1.
    [[nodiscard]] int focusedToolbarIndex() const noexcept { return _focusedToolbarIndex; }
    /// Moves focus between the terminal, the tab well and the toolbar (the three focus actions), skipping
    /// an area that is not shown. Entering the tab well focuses the active tab; entering the toolbar, its
    /// first item; reaching the terminal is leaveChrome().
    /// @param action Where focus goes.
    /// @return Moved when focus changed area; Stayed when there was nowhere else to go, so the key that asked
    ///         can still be the terminal's.
    window::FocusMove moveChromeFocus(window::ChromeFocusAction action);
    /// Reports what the chrome shows, as QML knows it. If the focused area is no longer shown, focus
    /// returns to the terminal.
    /// Booleans are the carve-out for a QML-facing Q_INVOKABLE (AGENT.md, enum class over bool).
    /// @param tabsShown Whether the tab well is shown.
    /// @param toolbarItems How many items the toolbar holds; 0 when it is absent.
    Q_INVOKABLE void setChromePresence(bool tabsShown, int toolbarItems);
    /// Reports where the sidebar's toggle stands, as Main.qml placed it (UiStyleProvider::sidebarToggle): the
    /// pane's context menu offers the hidden sidebar back while it stands nowhere.
    /// @param host "none", "band" or "sidebar" (window::sidebarToggleHostName()); anything else is "none".
    Q_INVOKABLE void setSidebarToggleHost(QString const& host);
    /// Offers a key press in the chrome. The key is first matched against the active session's key
    /// bindings for the three focus actions (so Ctrl+F6 cycles from inside the chrome too, and a rebound
    /// chord follows). A key in ChromeConsumedKeys is looked up in ChromeKeyRows for the focused area. Any
    /// other chord runs its binding, so the command palette or a new tab work from inside the chrome, while
    /// plain typing reaches nothing. A toolbar item is pressed by QML itself, so Return and Space there are
    /// left unhandled.
    /// @param qtKey The Qt::Key of the event.
    /// @param qtModifiers The event's Qt::KeyboardModifiers.
    /// @param text The event's text, which tells a Windows AltGr level from a Ctrl+Alt chord; the chord a
    ///        binding is looked up under is the one the session's keyboard platform decodes.
    /// @return Whether the key was handled, so QML should accept the event (a QML-facing carve-out).
    Q_INVOKABLE [[nodiscard]] bool chromeKey(int qtKey, int qtModifiers, QString const& text = {});
    /// Returns focus to the terminal and hides the ring (Escape, a pointer press, the terminal regaining
    /// focus). No-op when the chrome has no focus.
    Q_INVOKABLE void leaveChrome();

    /// Gives keyboard focus to the active pane's display and ends the chrome's hold on it: what is due when a
    /// popup over this window closes, which leaves no window holding the keyboard.
    void focusActiveDisplay();
    /// Closes the tab the user confirmed closing, as closeTabConfirmationRequested() asked. A tab that is
    /// already gone closes nothing: its row may now hold another tab.
    /// @param tab The tab's vtworkspace::TabId value.
    Q_INVOKABLE void confirmCloseTab(quint64 tab);
    /// Closes the workspace the user confirmed closing, as closeWorkspaceConfirmationRequested() asked. One
    /// that is gone already closes nothing.
    /// @param workspace The workspace's id, as the window's directory lists it (DirectoryWorkspace::id).
    Q_INVOKABLE void confirmCloseWorkspace(quint64 workspace);
    /// What the close confirmation says for a node (window::closeDialogText): the wording of its kind's
    /// table row, with the count of what ends with it taken from the directory.
    /// @param kind A window::CloseKind as an int.
    /// @param workspace The node's workspace id.
    /// @param tab The node's tab id; 0 for a workspace.
    /// @param label The node's name; empty when it has none.
    /// @param program The running program, where known; empty otherwise.
    /// @return `{ heading, message, acceptText }`; an empty map for an unknown kind.
    [[nodiscard]] Q_INVOKABLE QVariantMap closeConfirmation(
        int kind, quint64 workspace, quint64 tab, QString const& label, QString const& program) const;
    /// Closes the node the user confirmed closing, as sidebarCloseConfirmationRequested() asked. The node is
    /// named by its ids, so a close acts on the row the menu was opened on, not on what the window shows; one
    /// that is gone already closes nothing.
    /// @param kind A window::CloseKind as an int.
    /// @param workspace The node's workspace id.
    /// @param tab The node's tab id; 0 for a workspace.
    /// @param pane The node's pane key; 0 for a workspace or a tab.
    Q_INVOKABLE void confirmClose(int kind, quint64 workspace, quint64 tab, quint64 pane);
    // }}}
    /// Asks the QML to open this window's "save layout as" name prompt (the keyboard/palette entry point
    /// for a nameless SaveLayout action). Emits saveLayoutRequested(); the dialog calls saveLayoutAs()
    /// back with the name the user types.
    Q_INVOKABLE void beginSaveLayoutPrompt();
    /// Saves this window's tabs as a new layout named @p name (Save Layout's Save). A blank name is a no-op,
    /// so an empty prompt never writes a nameless layout; a name a layout already has is refused, however
    /// recently it was taken. Either way the window says how it went.
    /// @param name The layout name the user typed.
    Q_INVOKABLE void saveLayoutAs(QString const& name);
    /// Saves this window's tabs as the layout named @p name, replacing the one that has it (Save Layout's
    /// Replace, once the user confirmed it). The window says how it went.
    /// @param name The layout name the user typed.
    Q_INVOKABLE void replaceLayout(QString const& name);
    /// What Save Layout makes of @p name against the layouts saved (checkLayoutName()): the configuration's,
    /// and the layouts file's as beginSaveLayoutPrompt() last read it.
    /// @param name The typed name, untrimmed.
    /// @param replace Whether the user confirmed replacing a taken name -- a bool for QML, the documented
    ///        carve-out (a LayoutReplace inside).
    /// @return `nameState` ("empty", "available" or "taken"), `button` ("disabled", "save" or "replace")
    ///         and `trimmed`, the name as it would be saved.
    [[nodiscard]] Q_INVOKABLE QVariantMap checkLayoutName(QString const& name, bool replace) const;
    /// @return What Save Layout says it saves: this window's tabs and panes (layoutSummaryText()).
    [[nodiscard]] Q_INVOKABLE QString layoutSummary() const;
    /// @p check as QML reads it, with the name it was made of: what checkLayoutName() returns.
    [[nodiscard]] static QVariantMap layoutCheckMap(SaveLayoutCheck const& check, QString const& trimmed);
    /// Colors the ACTIVE tab (the keyboard entry point for `SetTabColor` carrying a color). Recorded
    /// as the user's own choice, so it outranks any color the application assigned via DECAC.
    /// @param color The color to apply.
    void setActiveTabColor(vtbackend::RGBColor color);
    /// Returns the ACTIVE tab to its default color (the `ResetTabColor` action): drops the user's
    /// choice, letting an application-assigned DECAC color resurface. No-op without an active tab.
    void resetActiveTabColor();
    /// Colors the tab at @p index on the user's behalf. A user color outranks any color the application
    /// assigned via DECAC, so this always takes visible effect.
    /// @param index The tab's row in this window.
    /// @param color The color the user picked.
    Q_INVOKABLE void setTabColor(int index, QColor const& color);
    /// Returns the tab at @p index to its default color, meaning: the color the application assigned via
    /// DECAC if it assigned one, otherwise the host's default tab color. It never discards an
    /// application's color, only the user's own choice.
    /// @param index The tab's row in this window.
    Q_INVOKABLE void resetTabColor(int index);
    Q_INVOKABLE void closeTabAtIndex(int index);
    Q_INVOKABLE void closeOtherTabs(int index);
    Q_INVOKABLE void closeTabsToRight(int index);
    /// Runs @p action against the tab at row @p row: the one mapping from a tab action to a tab. The
    /// tab actions act on @p row; any other action runs on the active session.
    /// @param row The tab's row in this window.
    /// @param action The action to run.
    void runTabAction(int row, actions::Action const& action);
    /// Runs @p action against @p tab, resolved to its row. Does nothing when the tab has no row.
    /// @param tab The tab to act on.
    /// @param action The action to run.
    void runTabAction(vtworkspace::TabId tab, actions::Action const& action);
    /// Asks the window to show its About dialog; emits aboutRequested().
    void showAbout();
    Q_INVOKABLE [[nodiscard]] QVariantList tabColorPalette() const;

    /// The fill for a colored tab, faded per the focus flags (WT-style); math in TabColorScheme.h.
    /// @param tabColor The user's chosen tab color.
    /// @param rowBackground The tab-row / title-bar background the tab sits on.
    /// @param active Whether this is the active tab of its window.
    /// @param hovered Whether the pointer is over the (non-active) tab.
    /// @param windowActive Whether the window itself is focused.
    /// @return The background color to fill the tab with.
    Q_INVOKABLE [[nodiscard]] QColor tabBackgroundColor(QColor const& tabColor,
                                                        QColor const& rowBackground,
                                                        bool active,
                                                        bool hovered,
                                                        bool windowActive) const;

    /// A contrasting label color (black or white) for text over a tab background.
    /// @param tabBackground The (already composited) tab background.
    /// @return Opaque black for a light background, opaque white for a dark one.
    Q_INVOKABLE [[nodiscard]] QColor tabTextColor(QColor const& tabBackground) const;

    /// Closes this OS window: tears down its tabs/sessions and drops the controller. Main.qml onClosing.
    Q_INVOKABLE void closeWindow();
    /// Whether this window may close now (its last pane exited). TerminalPane.onTerminated.
    Q_INVOKABLE [[nodiscard]] bool canCloseWindow() const noexcept;
    // }}}

    // {{{ Command palette
    /// This window's palette list model (never null; owned by this controller).
    [[nodiscard]] CommandPaletteModel* commandPalette() const noexcept { return _commandPalette.get(); }
    // }}}

    // {{{ Notices
    /// This window's notices: what posts to its toast and banner.
    [[nodiscard]] NoticeModel& notices() noexcept { return _notices; }

    /// @return This window's notices, as the `notices` property hands them to QML.
    [[nodiscard]] NoticeModel* noticesForQml() noexcept { return &_notices; }

    /// Where this window's toast goes now (placeToast()), from the band QML shows and the active pane's
    /// cursor row: asked as a toast shows.
    /// @param band The band's presence, by the name UiStyleProvider::titleBand() gives it ("shown", ...).
    /// @param edge Its edge, by name ("top" or "bottom").
    /// @param bandHeight The band's height, in logical pixels.
    /// @param toastHeight The toast's height (size.toast), in logical pixels.
    /// @return Where it goes, by name (toastHostName(): "titleBand", "contentTop" or "contentBottom"). The
    ///         band comes in, and the host goes out, as the band's QML vocabulary has them: names.
    [[nodiscard]] Q_INVOKABLE QString toastHost(QString const& band,
                                                QString const& edge,
                                                int bandHeight,
                                                int toastHeight) const;

    /// Rebuilds the palette against the CURRENT state — live tabs, the configured profiles/layouts and
    /// the key bindings as they stand after any reload — and asks the QML to show it.
    ///
    /// Called by the manager for the OpenCommandPalette action. The refresh happens here, on open,
    /// rather than being cached: the tab list is the whole reason the palette's rows can go stale.
    void openCommandPalette();

    /// Runs the command with id @p id against this window's active session, and records it as recently
    /// used. Unknown ids are ignored (a row can go stale between a refresh and a click).
    /// @param id The command id (CommandPaletteModel's `commandId` role).
    Q_INVOKABLE void runCommand(QString const& id);

    /// Starts refreshing what the daemon rows list (the local daemons, this daemon's workspaces), without
    /// waiting: for a surface that is about to list them. daemonRows() reads the caches only. A window with
    /// no daemon has nothing to refresh.
    void refreshSwitching() const;

    /// Opens the daemon and workspace switcher over this window: what the ShowSwitcher action does, for
    /// the host chip's click.
    Q_INVOKABLE void showSwitcher() const;

    /// Opens the switcher as the workspace chip's dropdown: below the chip, listing this window's daemon's
    /// workspaces alone (TerminalSessionManager::showWorkspaceDropdown()).
    /// @param chip The chip in the window's scene coordinates, the gap below it included.
    Q_INVOKABLE void showWorkspaceDropdown(QRectF const& chip) const;
    // }}}

    // {{{ Terminal context menu
    /// Rebuilds the context menu against the CURRENT state of the active pane and asks the QML to pop it.
    ///
    /// Called by the manager for the OpenContextMenu action, which has already made the right-clicked
    /// pane the active one — so "the active pane" here IS the pane the user clicked.
    void openContextMenu();

    /// Shows the menu of @p block of @p session: its header and its rows, below @p anchor or at the pointer.
    /// Pinned to @p session and @p block: picking a row after either has gone does nothing.
    /// @param session The session the block is in.
    /// @param block The block, by id.
    /// @param anchor Where the menu opens, in this window's coordinates; at the pointer when absent.
    void openBlockMenu(session::TerminalSession& session,
                       vtbackend::CommandBlockId block,
                       std::optional<QPointF> anchor);

    /// Where the pane menu was asked to open, in the window's coordinates; a null point for the pointer.
    /// What the QML pops the menu at when it is shown in the scene.
    [[nodiscard]] Q_INVOKABLE QPointF contextMenuAnchor() const noexcept
    {
        return _contextMenuAnchor.value_or(QPointF {});
    }

    /// The context menu, as QML consumes it: a list of rows, each a QVariantMap of
    /// { kind, title, enabled, checkable, checked, actionId, children }.
    ///
    /// `actionId` indexes into this controller's own action list rather than naming a command, so a click
    /// runs the exact action the row was built with. Nothing has to be looked up by name at click time,
    /// and nothing depends on some other model having been populated first.
    [[nodiscard]] QVariantList contextMenuModel() const { return menu(MenuSurface::Pane).model; }

    /// The title bar's context menu rows. Its OWN model, not the pane menu's: publishing into that one
    /// would rebuild and re-target a menu the user may still have open.
    [[nodiscard]] QVariantList titleBarContextMenuModel() const { return menu(MenuSurface::TitleBar).model; }

    /// The tab menu's rows, built for the tab it was last opened over.
    [[nodiscard]] QVariantList tabContextMenuModel() const { return menu(MenuSurface::Tab).model; }

    /// The new-tab menu's rows: one per profile.
    [[nodiscard]] QVariantList newTabMenuModel() const { return menu(MenuSurface::NewTab).model; }
    /// @return The sidebar row menu's rows, for QML.
    [[nodiscard]] QVariantList sidebarContextMenuModel() const { return menu(MenuSurface::Sidebar).model; }
    /// Runs the sidebar row menu's action @p actionId against the row the menu was opened over: a row that
    /// is not the displayed one is shown first, so the action never lands on another.
    /// @param actionId The row's `actionId`.
    Q_INVOKABLE void triggerSidebarContextMenuAction(int actionId);
    /// @return The sidebar model, for QML.
    [[nodiscard]] QObject* sidebarForQml() noexcept { return _sidebar.get(); }
    /// @return The sidebar model.
    [[nodiscard]] SidebarModel& sidebar() noexcept { return *_sidebar; }
    /// Says the sidebar's rows may be stale (a seen-state changed); they are rebuilt once per event-loop
    /// turn.
    void noteSidebarChanged();

    /// The main menu's rows: one submenu per menu-bar menu (command::buildMenuBar), in MenuBarMenu order.
    [[nodiscard]] QVariantList mainMenuModel() const { return menu(MenuSurface::MainMenu).model; }

    /// Builds and publishes the title bar's context menu, then asks QML to pop it.
    Q_INVOKABLE void openTitleBarContextMenu();

    /// Runs the title-bar menu action with id @p actionId. Out-of-range ids are ignored.
    /// @param actionId The row's `actionId`.
    Q_INVOKABLE void triggerTitleBarContextMenuAction(int actionId);

    /// Runs the context-menu action with id @p actionId against the pane the menu was opened over.
    /// Out-of-range ids are ignored.
    /// @param actionId The row's `actionId`.
    Q_INVOKABLE void triggerContextMenuAction(int actionId);

    /// Builds the menu of the tab at row @p index and shows it, natively or in the scene.
    ///
    /// The menu is pinned to that tab, not to its row: a pick acts on it wherever it has moved, and does
    /// nothing once it has closed.
    /// @param index The right-clicked tab's row.
    /// @param windowPosition Where the click landed, in window coordinates. Unused: the menu opens at the
    ///        pointer, which is where the click landed.
    Q_INVOKABLE void openTabContextMenu(int index, QPointF windowPosition);

    /// Runs the tab-menu action with id @p actionId against the tab the menu was opened over.
    /// Out-of-range ids, and a tab that has gone, are ignored.
    /// @param actionId The row's `actionId`.
    Q_INVOKABLE void triggerTabContextMenuAction(int actionId);

    /// Builds the new-tab menu from the configured profiles and shows it, natively or in the scene.
    /// @param windowPosition Where the click landed, in window coordinates. Unused: the menu opens at the
    ///        pointer.
    Q_INVOKABLE void openNewTabMenu(QPointF windowPosition);

    /// Runs the new-tab menu action with id @p actionId against the active session.
    /// Out-of-range ids are ignored.
    /// @param actionId The row's `actionId`.
    Q_INVOKABLE void triggerNewTabMenuAction(int actionId);

    /// Runs the header-bar item @p item (a window::HeaderBarItem): a new tab, the find bar, or the main
    /// menu. An @p item out of range does nothing.
    ///
    /// An int rather than the enum: the QML-facing carve-out, whose caller passes the row's @c item from
    /// UiStyleProvider::headerBarItems.
    /// @param item           A window::HeaderBarItem's underlying value.
    /// @param windowPosition Where the button's menu would open, in window coordinates (the main menu).
    Q_INVOKABLE void activateHeaderBarItem(int item, QPointF windowPosition);

    /// Builds the main menu -- the menu bar's menus as submenus (command::buildMenuBar of menuBarState())
    /// -- and shows it, natively or in the scene, as the new-tab menu is.
    /// @param windowPosition Where the menu button sits, in window coordinates. Unused: the menu opens at
    ///        the pointer.
    void openMainMenu(QPointF windowPosition);

    /// Runs the main-menu action with id @p actionId against the active session.
    /// Out-of-range ids are ignored.
    /// @param actionId The row's `actionId`.
    Q_INVOKABLE void triggerMainMenuAction(int actionId);
    // }}}

    // {{{ Menu bar
    /// The world as the application's menu bar sees it while this window is focused.
    ///
    /// The configuration's part (menuBarStateOf()), then this window: its tabs, the active tab's pane
    /// count, the live tab bar modes, and the active session's selection, speech, read-only lock and
    /// profile, as its contextMenuState() reports them.
    /// @return The state.
    [[nodiscard]] command::MenuBarState menuBarState() const;

    /// Runs a menu bar row's @p action on the active session; does nothing when there is none.
    /// @param action The row's action.
    void runMenuBarAction(actions::Action const& action) const;
    // }}}

    /// The titles of this window's tabs, in tab order (TabTitleProvider, for TabCommandSource).
    [[nodiscard]] std::vector<std::string> tabTitles() const override;

    /// Whether this window has a daemon behind it (command::DaemonRowsProvider).
    [[nodiscard]] command::DaemonSwitching switchingAvailability() const override;

    /// The daemons and workspaces the palette lists for this window (command::DaemonRowsProvider). Starts
    /// refreshing the local daemon discovery behind it, and answers from what it knew before.
    [[nodiscard]] command::DaemonRowsState daemonRows() const override;

    /// The label the strip shows for one tab, read as tabTitles() reads each row. Resolving a label
    /// takes that tab's terminal lock, so naming one tab this way waits on no other tab's parser.
    /// @param tab The tab to name.
    /// @return Its label, or empty when @p tab is not in this window.
    [[nodiscard]] std::string tabTitleOf(vtworkspace::TabId tab) const;

    // {{{ PaneProxy tree + window-service reads
    [[nodiscard]] session::PaneProxy* activeTabRootPane() const noexcept { return _activeTabRootProxy; }
    /// @return The leaves under the active tab's layout root; 0 without a tab.
    [[nodiscard]] int visiblePaneCount() const noexcept { return _visiblePaneCount; }
    /// @return The window's window::HeaderReveal, as its underlying value.
    [[nodiscard]] int headerReveal() const noexcept { return static_cast<int>(_headerReveal); }

    /// Has the sessions whose pane header shows follow the program in front of them, and every other session
    /// of this window stop (session::ProcessNameFollower::Header). A header shows for a leaf under the
    /// active tab's layout root when resolvePaneHeader() says so for the density, the reveal and the
    /// visible pane count; its geometry is QML's and is not consulted. Called on active-tab, structure,
    /// zoom, density and reveal changes.
    void refreshHeaderFollowers();
    [[nodiscard]] session::TerminalSession* activeSession() const noexcept;
    /// @return The active session's sticky header for the title band (@see activeStickyHeader above); empty
    ///         without an active session.
    [[nodiscard]] QVariantMap activeStickyHeader() const;

    /// Brings the active session's sticky header's command to the top of its pane: what the title band's jump
    /// link does. Nothing without an active session or a header.
    Q_INVOKABLE void jumpToActiveStickyHeader() const;

    /// The badge every tab of this window shows, in row order, as the strip reads it. GUI thread.
    [[nodiscard]] std::vector<TabBadgeKind> tabBadgeKinds() const;
    [[nodiscard]] bool titleBarVisible() const noexcept;

    /// The radius of the window's top corners (platform::windowCornerRadius): the chrome row's
    /// @c radii.window for a windowed window under client-side decorations, 0 when it is maximised, full
    /// screen, or wears the window manager's frame.
    /// @return The radius, in logical pixels.
    [[nodiscard]] int windowCornerRadius() const;
    [[nodiscard]] bool isMultimediaReady() const noexcept;
    // }}}

    // {{{ Window decoration (title bar)
    // Title-bar visibility is WINDOW state and lives here (the window authority), not per display:
    // per-pane storage silently reverted a runtime ToggleTitleBar on the next pane-focus change or
    // tab switch, and re-seeding from the profile on every session rebind reset it too.

    /// Seeds this window's initial title-bar visibility from the profile's show_title_bar setting.
    /// First-write-wins: only the first seed after construction takes effect, so session rebinds
    /// (tab switches, split collapses) never reset a runtime toggle.
    /// @param visible The profile's show_title_bar value.
    void seedTitleBarVisible(bool visible) override;

    /// Sets the window's title-bar visibility: stores the window-scoped state, applies the native
    /// window-frame decoration (Qt::FramelessWindowHint on the adopted OS window — shown => native
    /// frame, hidden => frameless so the custom client-side TitleBar is the only decoration; the C++
    /// counterpart of Main.qml's `flags` binding), and notifies the QML window bindings.
    /// @param visible The new visibility.
    void setTitleBarVisible(bool visible);

    /// Flips the window's title-bar visibility (the ToggleTitleBar action, routed here by the display).
    void toggleTitleBar() override;

    // The three gates Main.qml used to derive by negating titleBarVisible. They come off the frame
    // POLICY instead (platform::framePolicyFor), because the answer is no longer the same on every
    // OS: Windows keeps a real frame and zeroes it, and macOS keeps a decorated NSWindow whose own
    // traffic lights ours must then yield to. Bools, as the QML-facing carve-out allows, but each
    // reads as the question its use site asks.

    /// Whether the window should carry Qt::FramelessWindowHint.
    ///
    /// False on Windows and macOS even with our own title bar: the hint is exactly what costs a
    /// window its system shadow and rounded corners, and both platforms have a way to keep the frame
    /// while still giving the client area the whole window.
    [[nodiscard]] bool needsFramelessHint() const noexcept;

    /// @return What the window is for.
    [[nodiscard]] WindowRole role() const noexcept { return _role; }

    /// @return role() as the int QML reads.
    [[nodiscard]] int windowRoleForQml() const noexcept { return std::to_underlying(_role); }

    /// The Qt window flags the role adds to the frame policy's, as the int Main.qml ORs into its `flags`:
    /// none for a Normal window, a frameless tool window for a Panel one.
    /// @return Qt::WindowFlags' value, 0 when the role's level adds nothing.
    [[nodiscard]] int roleFlags() const noexcept;

    /// Whether the window should carry Qt::ExpandedClientAreaHint, which lets the content fill the title
    /// bar and reports the bar as the window's safe area.
    ///
    /// True on macOS under our own title band alone (platform::ClientAreaExpansion::QtHint): Qt is the
    /// one owner of the full-size content view, and the Cocoa frame adapter leaves the style mask alone.
    [[nodiscard]] bool needsExpandedClientArea() const noexcept;

    /// Whether WE supply the window's frame affordances -- ResizeBorder.qml's hit zones and the tab
    /// strip's own min/max/close controls -- or the OS frame does.
    ///
    /// One question, not two: the frame that resizes the window is the frame that carries its
    /// buttons, which is why @ref platform::FrameAffordances is one field.
    [[nodiscard]] bool needsClientFrameAffordances() const noexcept;

    /// Leading space the tab strip must leave clear for the OS's own window controls.
    ///
    /// Zero unless the frame policy says the OS paints them OVER our chrome, which today is macOS:
    /// a full-size-content NSWindow keeps drawing the real traffic lights and its content view
    /// extends underneath them. The MAGNITUDE comes from the frame adapter rather than from our
    /// chrome tokens, because it is the operating system's number -- AppKit's buttons are a fixed
    /// size whatever font or `ui_style` Contour is using.
    [[nodiscard]] qreal nativeControlsInset() const noexcept;
    // }}}

    // {{{ The caption buttons the OS drives (Snap Layouts)

    /// Records where a caption button sits in the scene, for the caption adapter's hit test.
    /// @param owner  The control reporting it; only the control that reported a kind last may clear it.
    /// @param kind   int(config::WindowControlKind); an out-of-range kind is ignored.
    /// @param x      The button's leading edge, in the window's logical pixels.
    /// @param y      Its top edge.
    /// @param width  Its width.
    /// @param height Its height.
    Q_INVOKABLE void setCaptionButtonRect(
        QObject* owner, int kind, qreal x, qreal y, qreal width, qreal height);

    /// Forgets a caption button, which is no longer shown -- unless another control reported it since: a
    /// change of side builds the new controls before the old ones are gone.
    /// @param owner The control that no longer shows it.
    /// @param kind  int(config::WindowControlKind); an out-of-range kind is ignored.
    Q_INVOKABLE void clearCaptionButtonRect(QObject* owner, int kind);

    /// @return How the maximise button looks while the OS drives it; Rest until the adapter says otherwise.
    [[nodiscard]] platform::CaptionButtonVisual nativeMaximizeVisual() const noexcept
    {
        return _nativeMaximizeVisual;
    }

    /// @return Whether the OS reports the pointer over the maximise button (hovered or pressed). A
    ///         QML-facing predicate: the carve-out for Q_PROPERTY booleans.
    [[nodiscard]] bool nativeMaximizeHovered() const noexcept
    {
        return _nativeMaximizeVisual != platform::CaptionButtonVisual::Rest;
    }

    /// @return Whether the OS reports the maximise button pressed. A QML-facing predicate, as above.
    [[nodiscard]] bool nativeMaximizePressed() const noexcept
    {
        return _nativeMaximizeVisual == platform::CaptionButtonVisual::Pressed;
    }

    /// What this window lends the caption adapter: the adapter's seam, public so a test drives it on any OS.
    /// The layout carries the reported rectangles, the OS window's logical width and the resize border's
    /// thickness; the visual updates nativeMaximizeVisual(); the toggle maximises or restores.
    /// @return The binding; its callbacks capture this controller.
    [[nodiscard]] platform::CaptionBinding captionBinding();
    // }}}

    // {{{ The title band's material and geometry

    /// Re-decides the title band: applies its request through the app's title-band material when the
    /// window's frame hosts it (hostsTitleBandMaterial) and the request changed (or the platform window is
    /// new to it), then measures the band and emits titleBandChanged() when its geometry, request or fill
    /// moved. A request whose title-bar growth changed also moves the traffic lights, so it emits
    /// nativeControlsInsetChanged() as well. A material that fails is logged, and the window falls back to
    /// the opaque band for its lifetime (R-g). Cheap to repeat: every hook (expose, palette, scale, screen,
    /// window state, safe area, a chrome refresh) calls it. GUI thread.
    void refreshTitleBand();

    /// Where this window's band and tabs go now (ContourGuiApp::titleBandOf): the decision Main.qml asks
    /// UiStyleProvider::titleBand for, from the same inputs -- the frame, the mode, the tab-bar gate, its
    /// edge and the host chip's notice.
    /// @return The band.
    [[nodiscard]] TitleBand titleBand() const;

    /// @return What this window asks the OS to draw behind and around its band now: its surface
    ///         (titleBandSurface()) in the effective colour scheme, for a tabbed window.
    [[nodiscard]] platform::TitleBandRequest titleBandRequest() const;

    /// @return What this window's band paints now, a config::BandFill as its underlying value: the same
    ///         answer UiStyleProvider::bandFill gives, unless this window's frame hosts no material or its
    ///         material failed, where it is the opaque band's.
    [[nodiscard]] int titleBandFill() const { return static_cast<int>(titleBandSurface().fill); }

    /// @return The band's height and full-screen resting place, from the app's band tokens for the band's
    ///         centre (titleBand()) and, where the frame is the full-size content view, the native title bar
    ///         as measured now.
    [[nodiscard]] TitleBandGeometry titleBandGeometry() const;

    /// @return The band's height as last published by refreshTitleBand(), in logical pixels.
    [[nodiscard]] qreal titleBandHeight() const noexcept { return _titleBandGeometry.height; }

    /// @return Where a revealed full-screen band rests, window-relative, as last published; 0 when windowed.
    [[nodiscard]] qreal titleBandRevealTop() const noexcept { return _titleBandGeometry.revealTop; }

    /// Whether Main.qml sets Qt::NoTitleBarBackgroundHint: only under Qt::ExpandedClientAreaHint (macOS, our
    /// own band), and only while the published request's backdrop is the client's own paint. A bool for
    /// QML, the documented QML-facing carve-out, read as the question the `flags` binding asks.
    [[nodiscard]] bool clearsTitleBarBackground() const noexcept;
    // }}}

    // {{{ Tab strip (tab bar) placement + visibility
    // Like title-bar visibility, these are WINDOW state seeded once from the profile (first-write-wins),
    // so later session rebinds (tab switch, split collapse) never clobber them.

    /// The tab strip position as an int for QML (0 = Top, 1 = Bottom; see TabBarPosition).
    [[nodiscard]] int tabBarPosition() const noexcept;

    /// The tab strip visibility mode as an int for QML (see TabBarVisibility).
    [[nodiscard]] int tabBarVisibility() const noexcept;

    /// Whether the tab strip should currently be shown, resolving the visibility mode against the live
    /// tab count: Always => true, Never => false, Multiple => count() > 1. Main.qml binds the tab
    /// strip's `visible` to this.
    /// @return True if the tab strip should be shown.
    [[nodiscard]] bool tabBarShouldShow() const noexcept;

    /// Seeds this window's tab strip position from the global tab_bar_position setting.
    /// First-write-wins (see seedTitleBarVisible).
    /// @param position The configured tab_bar_position value.
    void seedTabBarPosition(config::TabBarPosition position) override;

    /// Seeds this window's tab strip visibility mode from the global tab_bar_visibility setting.
    /// First-write-wins (see seedTitleBarVisible).
    /// @param visibility The configured tab_bar_visibility value.
    void seedTabBarVisibility(config::TabBarVisibility visibility) override;

    /// Re-applies the configured tab strip placement and visibility to this window.
    ///
    /// Unlike the seed methods above this is NOT first-write-wins. It runs only on an explicit
    /// configuration reload, where the user has just asked for the configured value to take effect --
    /// so it supersedes a runtime override, which is what reloading a configuration means. Without it
    /// the seed latch swallows the change and the setting appears to do nothing until restart.
    ///
    /// @param position   The configured tab_bar_position value.
    /// @param visibility The configured tab_bar_visibility value.
    void applyTabBarFromConfig(config::TabBarPosition position, config::TabBarVisibility visibility);

    /// Re-applies the configured tab placement (`tab_placement`) to this window. Placement is configuration
    /// only -- never remembered as a last-used choice -- so a reload, which is how the Settings row's commit
    /// reaches an open window, is its one way to change.
    /// @param placement The configured tab_placement value.
    void applyTabPlacementFromConfig(config::TabPlacement placement);

    /// Sets this window's tab strip visibility mode at runtime.
    ///
    /// A view toggle, in the same spirit as toggleTitleBar(): it applies to THIS window for as long as
    /// it lives, and the settings page remains where the choice is made permanent. Marks the mode as
    /// seeded so a later session rebind cannot revert it.
    void setTabBarVisibility(config::TabBarVisibility visibility) override;

    /// Sets this window's tab strip placement at runtime. @see setTabBarVisibility.
    void setTabBarPosition(config::TabBarPosition position) override;
    // }}}

    /// This window's currently focused display (for window services + status-line targeting).
    [[nodiscard]] display::TerminalDisplay* activeDisplay() const noexcept { return _activeDisplay; }

    // {{{ Window-geometry authority
    // The controller is the ONLY component that mutates QWindow geometry (WM size hints, window show
    // modes, content-driven resizes). Displays route their requests here.

    /// Adopts the QML window early (before any display focuses in), assigns the spawn target screen
    /// (the pre-show DPR predictor: spawning window's screen, else the cursor's screen on non-Wayland,
    /// else primary) and installs the DPR/screen-change hooks for the scale-settlement handler.
    /// Called from Main.qml's Component.onCompleted, before the first tab exists.
    /// @param osWindow The ApplicationWindow backing this controller.
    Q_INVOKABLE void bindWindow(QQuickWindow* osWindow);

    /// Toggles maximize/restore through the show-mode protocol (size-increment clearing on maximize,
    /// hint re-application on restore) — the QML entry point for the custom window controls and the
    /// title-bar double-click. QML must not call QWindow::showMaximized()/showNormal() directly: the
    /// controller is the only window-geometry mutator, and the direct calls skip the protocol (leaving
    /// stale WM size increments applied while maximized, and none after restore).
    Q_INVOKABLE void toggleMaximized();

    /// Minimizes this window — routed here so every show-mode change stays on the controller.
    Q_INVOKABLE void minimizeWindow();

    /// Whether this window's OS window is minimised. False before a window is bound.
    ///
    /// Read from QWindow::windowStates(), which the platform keeps current. (Wayland compositors do not
    /// report minimising, so there a minimised window reads as shown.)
    [[nodiscard]] bool isMinimized() const noexcept;
    /// @return Whether the window is on screen for the user: shown, and not minimised.
    [[nodiscard]] bool isExposed() const noexcept;

    /// The screen this window's OS window is on: where the Settings window opens when this window asks.
    /// @return The bound window's screen, or nullptr before a window is bound.
    [[nodiscard]] QScreen* screen() const noexcept;

    /// Brings this window to the front: un-minimises it, keeping a maximised or full-screen state,
    /// raises it, and asks the window manager to activate it. What clicking a finish notification
    /// does after its tab was made active. A no-op before a window is bound.
    void raiseAndActivate();

    /// Asks this window's QML to show a tray message on behalf of one of its sessions.
    ///
    /// Routed through the WINDOW because the session may sit in a background tab, which has no
    /// SessionChrome to relay a signal of its own. The delivery for platforms without a desktop Notifier.
    /// @param title     The message title.
    /// @param body      The message body.
    /// @param onClicked What a click on the message does, besides bringing the window forward.
    void requestNotification(QString const& title, QString const& body, std::function<void()> onClicked);

    /// Asks this window's QML to show a tray message whose click only brings the window forward: a notice
    /// a pane on screen relays from its session.
    /// @param title The message title.
    /// @param body  The message body.
    Q_INVOKABLE void requestNotification(QString const& title, QString const& body)
    {
        requestNotification(title, body, {});
    }

    /// A click on this window's tray message: brings the window forward, and does what the message asked a
    /// click to do -- a finish notice's session revealed, an OSC 99 notification activated.
    ///
    /// Qt's tray reports a click with no word of which message it was on (QSystemTrayIcon::messageClicked
    /// carries nothing), and shows one message at a time, so the click is taken for one on the last
    /// message shown. A click on a message a later one replaced is lost to it.
    Q_INVOKABLE void trayMessageClicked();

    /// Asks this window's QML to play a bell sound, for the same reason.
    /// @param source The sound's URL; empty plays nothing.
    /// @param volume 0.0 (silent) .. 1.0 (loudest).
    void requestBell(QString const& source, double volume) { emit bellRequested(source, volume); }

    /// Asks this window's QML to flash its taskbar entry, for the same reason.
    void requestAlert() { emit alertRequested(); }

    /// Sizes the still-unmapped window from the active pane's REAL cell metrics (FreeType math run
    /// headlessly during session attach) plus the declared chrome, applies the WM size hints, then maps
    /// the window directly in the profile's state (normal/maximized/fullscreen) — the first map is the
    /// final geometry, eliminating the map-then-correct spawn flicker. Falls back to showing at the
    /// default size (never leaves the user windowless) when no metrics are available.
    /// Called from Main.qml's Component.onCompleted, after the first tab was created.
    Q_INVOKABLE void showInitial();

    /// @return Whether the workspace sidebar is shown, a config::SidebarVisibility as its underlying value.
    [[nodiscard]] int sidebarVisibility() const noexcept { return static_cast<int>(_sidebarVisibility); }

    /// @return The workspace sidebar's width in logical pixels, 0 while it is hidden: the horizontal chrome
    ///         this window declares (see chrome()).
    [[nodiscard]] int sidebarWidth() const;

    /// @return Where the tabs live, a config::TabPlacement as its underlying value.
    [[nodiscard]] int tabPlacement() const noexcept { return static_cast<int>(_tabPlacement); }

    /// Sets the workspace sidebar's width, as dragging its edge does. Call it when the drag is released, not
    /// on every move: each distinct width is written to the GUI state. The width is kept within the chrome
    /// row's bounds, on the Terminal row snapped to whole cells, and remembered (GuiState::sidebar) as the
    /// width the next window starts at. Takes effect at once, shown or not.
    /// @param width The requested width in logical pixels.
    Q_INVOKABLE void setSidebarWidth(int width);

    /// Shows the workspace sidebar when it is hidden, and hides it when it is shown, and remembers the
    /// choice (GuiState::sidebar) as the visibility the next window starts with.
    Q_INVOKABLE void toggleSidebar();

    /// Moves keyboard focus into the workspace sidebar, showing the sidebar first when it is hidden (what the
    /// FocusSidebar action does).
    Q_INVOKABLE void focusSidebar();

    /// Opens the sidebar's filter field with the keyboard in it, showing the sidebar first when it is hidden
    /// (sidebarFilterRequested()): what every filter button and the FilterWorkspaces action do.
    Q_INVOKABLE void openSidebarFilter();

    /// @return The other workspaces' roll-up, an Urgency's underlying value.
    [[nodiscard]] int otherWorkspacesUrgency() const noexcept { return _sidebar->otherWorkspacesUrgency(); }
    /// @return Whether the keyboard is in the sidebar's tree.
    [[nodiscard]] bool sidebarHasFocus() const noexcept { return _chromeArea == window::ChromeArea::Sidebar; }
    /// @return The commands sidebarCommandRequested() carries, by name, as window::ChromeCommand values.
    [[nodiscard]] static QVariantMap sidebarCommands();

    /// Creates a workspace and shows it in this window: the sidebar's "+" (the NewWorkspace action).
    Q_INVOKABLE void newWorkspace() const;

    /// Starts the drag of what a sidebar row names: a workspace (to reorder it), or a tab -- one of the
    /// shown workspace's drags as beginTabDrag() does a strip tab, another workspace's is named by the
    /// directory's pane keys, and tears off nowhere (finishNodeDrag()), having no tab here.
    /// @param row A workspace or tab row's index in the sidebar model.
    /// @return The drag's JSON; empty for a pane row, or a row that is gone.
    Q_INVOKABLE [[nodiscard]] QString beginSidebarDrag(int row);

    // {{{ Sidebar actions whose behaviour lands with the attention ledger (Task 7) and the workspace control
    // API (Task 6). Each has its final signature and does nothing yet.
    /// Shows the next pane that needs the user (the JumpToNextAttention action): the first after the active
    /// pane, in sidebar order, in the most urgent of the classes nextAttention() visits. Its workspace is
    /// selected first; a daemon's selection lands later, and the tab and pane are shown then
    /// (completePendingJump()). Nothing when no pane asks for the user.
    void jumpToNextAttention();
    /// Shows @p target wherever it is: its workspace is selected first (a daemon's selection lands later, and
    /// the tab and pane are shown then), and it is counted as seen once shown. Replaces a pending jump.
    /// @param target The pane to show.
    void jumpToPane(PaneKey target);
    /// Shows @p workspace because the user picked it (a sidebar row): selects it, and gives up a pending
    /// jump, row action or follow, as the user went elsewhere.
    /// @param workspace The workspace's id, as the window's directory lists it.
    void showWorkspace(uint64_t workspace);
    /// Shows the pane a jump selected a workspace for, once the displayed workspace holds it, and counts it
    /// as seen. Nothing when no jump is pending or the pane is not there (yet). Called when a daemon's layout
    /// lands, and by displayWorkspace(), which then ends the jump either way.
    void completePendingJump();
    /// Forgets the jump that waits for a workspace to be shown: it was refused, or is not wanted.
    void cancelPendingJump() noexcept;
    /// @return Whether a jump waits for its workspace to be shown.
    [[nodiscard]] bool jumpPending() const noexcept { return _pendingJump.has_value(); }
    /// @return Whether a sidebar action waits for a workspace to be shown, or for its pane to appear in it.
    [[nodiscard]] bool rowActionPending() const noexcept { return _pendingRowAction.has_value(); }
    /// Shows the workspace a daemon makes for @p pane, once its push lists it there: the first directory that
    /// lists @p pane in a workspace the directory does not list now. A move into a new workspace is followed
    /// so, as an in-process window shows the workspace it made. A jump elsewhere, a later follow, or
    /// session::TerminalSessionManager::RemoteMoveTimeout passing gives it up.
    /// @param pane The pane moved, by the daemon's key.
    void followIntoNewWorkspace(PaneKey pane);
    /// @return Whether a move into a new workspace waits to be followed there.
    [[nodiscard]] bool followPending() const noexcept { return _pendingFollow.has_value(); }
    /// Gives up every look that waits: a jump, a row action and a follow.
    void cancelPendingNavigation() noexcept;
    /// Marks the active pane as seen (the MarkSeen action).
    void markActivePaneSeen();
    /// Renames the displayed workspace inline (the RenameWorkspace action): shows the sidebar, puts the
    /// keyboard in it and opens the editor on the workspace's row; committing it calls renameWorkspace().
    void renameCurrentWorkspace();
    /// Moves the displayed workspace one place up in the list (the MoveWorkspaceUp action).
    void moveCurrentWorkspaceUp();
    /// Moves the displayed workspace one place down in the list (the MoveWorkspaceDown action).
    void moveCurrentWorkspaceDown();
    /// Closes the displayed workspace, after asking when it may be running a program (the CloseWorkspace
    /// action).
    void closeCurrentWorkspace();
    /// Shows the pane of the sidebar row a menu acts on and focuses it (the GoToPane action). Nothing when no
    /// sidebar row menu is acting.
    void goToSidebarPane();
    /// Moves the active tab to the end of another workspace (the MoveTabToWorkspace action).
    /// @param position The target workspace's 1-based place in the window's workspace list.
    void moveActiveTabToWorkspace(uint32_t position);
    /// Moves the active pane into another tab, as a split beside its active pane (the MoveToTab action).
    /// @param workspace The tab's workspace's 1-based place in the window's workspace list.
    /// @param tab The tab's 1-based place among that workspace's tabs.
    void moveActivePaneToTab(uint32_t workspace, uint32_t tab);
    /// Moves the active pane into a tab of its own at the end of its workspace (the MoveToNewTab action);
    /// nothing for a pane that already is its tab's only one.
    void moveActivePaneToNewTab();
    /// Moves the active pane into a workspace of its own (the MoveToNewWorkspace action).
    void moveActivePaneToNewWorkspace();

    /// @name The workspace operations, by the ids the window's directory lists (TerminalSessionManager's).
    /// @{
    /// Sets the title tab @p tab shows: a tab of the shown workspace as the strip's inline rename does, any
    /// other through TerminalSessionManager::renameTab().
    /// @param tab The tab, as the directory names it.
    /// @param title Its title; empty clears the one the user set.
    void renameTab(TabRef tab, std::string const& title);
    /// Renames workspace @p workspace.
    /// @param workspace The workspace's id.
    /// @param label Its new label; empty clears it.
    void renameWorkspace(uint64_t workspace, std::string label);
    /// Moves workspace @p workspace to place @p toPosition of the list.
    /// @param workspace The workspace's id.
    /// @param toPosition The 0-based place it ends at.
    void moveWorkspace(uint64_t workspace, int toPosition);
    /// Moves tab @p tab to place @p toPosition of workspace @p toWorkspace.
    /// @param tab The tab's id.
    /// @param toWorkspace The destination workspace's id.
    /// @param toPosition The 0-based place it ends at among the destination's tabs.
    void moveTabToWorkspace(uint64_t tab, uint64_t toWorkspace, int toPosition);
    /// Closes workspace @p workspace, after asking when it may be running a program.
    /// @param workspace The workspace's id.
    void closeWorkspace(uint64_t workspace);
    /// @}
    // }}}

    /// Window chrome height in logical pixels OUTSIDE the terminal content area, DECLARED by Main.qml
    /// (bound to the title bar's effective height: 0 when hidden). Geometry math must never measure
    /// window-minus-item deltas — those are transient during relayout and structurally wrong in splits.
    [[nodiscard]] int chromeHeight() const noexcept { return _chromeHeight; }
    /// Sets the declared chrome height (QML binding write) and refreshes the WM size hints.
    void setChromeHeight(int height);

    /// Recomputes and applies the WM size hints (minimum/base/increment) for this window from
    /// @p requester's cell geometry, profile margins, content scale and the declared chrome.
    /// Refresh triggers: font/DPI reconfiguration (via the display), chrome changes, and
    /// restore-from-fullscreen/maximize. NEVER called from a resize path.
    /// @param requester The display whose cell geometry defines the hints (the active pane).
    /// @param mode Whether to gate the resize grid on the live window state (@ref HintApplyMode).
    void updateSizeHintsFor(session::DisplaySurface& requester, HintApplyMode mode) override;

    /// Shows this window fullscreen (size increments cleared while non-normal).
    /// @param requester The display forwarding the request (resolves the OS window).
    void setWindowFullScreen(session::DisplaySurface& requester) override;
    /// Shows this window maximized (size increments cleared while non-normal).
    /// @param requester The display forwarding the request (resolves the OS window).
    void setWindowMaximized(session::DisplaySurface& requester) override;
    /// Restores this window to normal state and re-applies the WM size hints.
    /// @param requester The display forwarding the request (resolves the OS window).
    void setWindowNormal(session::DisplaySurface& requester) override;
    /// Toggles fullscreen, restoring the previous maximized state on exit.
    /// @param requester The display forwarding the request (resolves the OS window).
    void toggleFullScreen(session::DisplaySurface& requester) override;

    /// THE grid->window choke point: every programmatic window resize (DECSLPP / CSI 8 t, font zoom's
    /// grid restore, profile switch, DPR settlement) lands here; nothing else may resize the window.
    /// Computes the window size so that @p requester's pane receives @p totalPageSize — for a split
    /// window by solving the pane-tree ratios (vtworkspace::contentSizeForLeaf), identity for a single pane —
    /// plus the declared chrome, and issues ONE resize. The resulting WM resize event drives the grid
    /// through the normal window->grid path; the WM is free to refuse (the reflowed grid then stands).
    /// Refused (logged) when fullscreen/maximized or the requester has no window/session/pane.
    /// @param requester     The display whose pane the grid request targets.
    /// @param totalPageSize The requested total page (main page + status line).
    /// @return True if a window resize was issued.
    bool resizeWindowForPage(session::DisplaySurface& requester, vtbackend::PageSize totalPageSize) override;

    /// Pixel-flavored choke-point entry (CSI 4 t): requests the content area of @p requester's pane in
    /// device pixels; otherwise identical to resizeWindowForPage().
    /// @param requester       The display whose pane the request targets.
    /// @param contentDevicePx The requested pane content extent in device pixels.
    /// @return True if a window resize was issued.
    bool resizeWindowForContentPixels(session::DisplaySurface& requester,
                                      vtbackend::ImageSize contentDevicePx) override;
    // }}}

    /// Whether @p osWindow is this controller's OS window. A controller adopts the QQuickWindow of the
    /// first display that focuses into it; the manager uses this to route a focus/close to the right
    /// controller. Returns false until this controller has seen a display (its window is not yet known).
    [[nodiscard]] bool ownsOSWindow(QQuickWindow const* osWindow) const noexcept
    {
        return osWindow != nullptr && _osWindow == osWindow;
    }

    /// Makes @p display this window's focused display: re-points the window-service signal bridge and
    /// re-emits the window bindings. Called by the manager when a display in this window focuses in.
    void focusDisplay(display::TerminalDisplay* display);

    /// Clears _activeDisplay if it pointed at @p display (the display was destroyed). Called by the
    /// manager routing detachDisplay() by window.
    void onDisplayDetached(display::TerminalDisplay* display) noexcept;

    /// Get-or-create a PaneProxy for @p id (reused across rebuilds so QML items survive). Public so the
    /// PaneProxy can be constructed against the manager. (Used only by rebuildActiveTabPaneProxies.)
    [[nodiscard]] session::PaneProxy* getProxy(vtworkspace::PaneId id);

    // {{{ Called by the manager's ModelEvents router (dispatched here by WorkspaceId)
    void onTabAboutToBeAdded(int index);
    void onTabAdded(int index);
    void onTabAboutToBeRemoved(int index);
    void onTabClosed();
    void onTabAboutToBeMoved(int fromIndex, int toIndex);
    void onTabMoved();
    void onActiveTabChanged();
    void notifyTabRowChanged(vtworkspace::TabId tab, QList<Roles> const& roles);
    /// Announces @p roles changed on every tab row, for a change that is not one tab's (a reload).
    /// @param roles The roles to re-read.
    void notifyAllTabRowsChanged(QList<Roles> const& roles);
    void refreshAllTabTitles();
    void refreshActiveTabHighlight();
    void rebuildActiveTabPaneProxies();
    /// The leaf @p leaf of @p tab carries a new session (a restart in the same pane): re-announces that
    /// pane's proxy, so QML rebinds its display, and the tab row and active session that read it.
    /// @param tab  The tab holding the leaf.
    /// @param leaf The leaf whose session was replaced.
    void notifyPaneSessionReplaced(vtworkspace::TabId tab, vtworkspace::PaneId leaf);
    /// Re-points activeTabRootPane at the active tab's layout root (vtworkspace::Tab::layoutRoot), which zoom
    /// moves without reshaping the tree. O(1): the proxies stay valid, so no walk or rebuild is needed.
    void refreshActiveTabLayoutRoot();
    /// Notifies every proxy's active state changed (active-pane focus moved, no tree rebuild).
    void notifyActivePaneChanged();
    /// Notifies the split node proxy @p splitNode that its ratio changed.
    void notifyRatioChanged(vtworkspace::PaneId splitNode);
    /// Emits activeSessionChanged (the focused pane's session may have changed).
    void emitActiveSessionChanged() { emit activeSessionChanged(); }
    /// Rebuilds and republishes this window's status line (tab list + per-pane marker).
    void updateStatusLine();
    // }}}

    // {{{ Hosting notice
    /// What this window tells the user about where its sessions run: set when the daemon the launch wanted
    /// could not be had and the sessions run in this process instead (cli::hostingNotice), so they end
    /// with the window. Main.qml shows it as a bar while non-empty.
    [[nodiscard]] QString const& hostingNotice() const noexcept { return _hostingNotice; }

    /// Shows @p notice in this window's notice bar.
    /// @param notice The line to show; empty hides the bar.
    void showHostingNotice(QString notice);

    /// The user dismissed the notice bar: it is hidden for good.
    Q_INVOKABLE void dismissHostingNotice() { showHostingNotice({}); }
    // }}}

    // {{{ Remote chrome
    /// The remote host this window's sessions run on (`contour client --host`), which its title and tab
    /// strip name, so it is never mistaken for a local window.
    /// @return The host's label; empty for a local window.
    [[nodiscard]] QString hostLabel() const;

    /// What this window's drift badge shows: whether the daemon of its own link runs another release.
    /// @return Its window::DaemonDriftModel; null for a local window.
    [[nodiscard]] QObject* daemonDrift() const;

    /// @return The window's window::PromptHost: where the ssh logins this window started ask their questions.
    [[nodiscard]] QObject* promptHost() const noexcept;

    /// What the host chip shows: the manager's host connection, folded onto the chip's three states.
    /// @return A window::HostChipState as its underlying value; Connected for a local window.
    [[nodiscard]] int hostChipState() const noexcept;

    /// The manager's host connection changed what the chip shows. @see hostChipState
    void notifyHostConnectionChanged() { emit hostChipStateChanged(); }

    /// The manager bound the window to another host, or released it: its label and chip read anew.
    /// @see hostLabel, hostChipState
    void notifyHostBindingChanged()
    {
        emit hostLabelChanged();
        emit hostChipStateChanged();
    }

    /// The chord the configuration binds to a command, as this host spells shortcuts (an ended pane's
    /// footer names Close's). Each call reads the configuration as it is now, but nothing notifies a
    /// binding over it: a label already showing keeps its chord until it is evaluated again (the footer
    /// asks when it appears).
    /// @param commandName The command's id (command::commandId), such as "ClosePane".
    /// @return The chord, e.g. "Ctrl+Shift+X" or "⌃⇧X"; empty when nothing binds the command.
    [[nodiscard]] Q_INVOKABLE QString shortcutText(QString const& commandName) const;

    /// Drops what this window remembers of @p session, which was removed: its cached process name.
    /// @param session The removed session.
    void forgetSession(vtworkspace::SessionId session) { _processNames.erase(session.value); }

    /// The application's version string, shown by the About dialog.
    /// @return The build constant, reached through the composition root.
    [[nodiscard]] QString applicationVersion() const;

    /// The OS window title for the active session's @p sessionTitle: prefixed with the host for a remote
    /// window. @see remoteWindowTitle
    /// @param sessionTitle The active session's title.
    /// @return The window title.
    Q_INVOKABLE [[nodiscard]] QString windowTitle(QString const& sessionTitle) const;
    // }}}

  signals:
    void countChanged();
    /// carriedPane() changed: a node drag began or ended.
    void carriedPaneChanged();
    void activeTabIndexChanged();
    /// Requests that the tab at @p index open its inline title editor. Per-window (this controller
    /// is the tab-strip model), so it only reaches this window's TabItems; the delegate whose row
    /// matches @p index starts editing.
    void tabTitleEditRequested(int index);
    /// Requests that the tab at @p index open its color picker. Per-window, exactly like
    /// tabTitleEditRequested: the delegate whose row matches @p index opens its TabColorFlyout.
    void tabColorPickRequested(int index);
    /// Requests that this window show its About dialog.
    void aboutRequested();
    /// Requests that this window show its command palette. Per-window (like tabTitleEditRequested), so
    /// the popup opens over the window the user pressed the chord in — not over every open window.
    void commandPaletteRequested();
    /// Requests that this window open its "save layout as" name prompt. Per-window, exactly like
    /// commandPaletteRequested: the prompt opens over the window the SaveLayout action fired in.
    void saveLayoutRequested();
    /// The context menu's rows changed. Emitted before contextMenuRequested(), so the QML has already
    /// rebuilt the menu by the time it is asked to pop it.
    void contextMenuModelChanged();
    /// Requests that this window pop its terminal context menu, at the mouse cursor.
    void contextMenuRequested();
    void titleBarContextMenuModelChanged();
    void titleBarContextMenuRequested();
    /// The tab menu's rows changed. Emitted before tabContextMenuRequested().
    void tabContextMenuModelChanged();
    /// Requests that the tab at @p index pop its context menu in the scene.
    void tabContextMenuRequested(int index);
    /// The new-tab menu's rows changed. Emitted before newTabMenuRequested().
    void newTabMenuModelChanged();
    /// Requests that this window pop its new-tab menu in the scene.
    void newTabMenuRequested();
    /// The main menu's rows changed. Emitted before mainMenuRequested().
    void mainMenuModelChanged();
    /// Requests that this window pop its main menu in the scene.
    void mainMenuRequested();
    /// The sidebar row menu's rows changed. Emitted before sidebarContextMenuRequested().
    void sidebarContextMenuModelChanged();
    /// Requests that this window pop the sidebar row menu in the scene, at the row it was opened over.
    /// @param row The row's index in the sidebar model.
    /// @param windowPosition Where the pointer was, in window coordinates.
    void sidebarContextMenuRequested(int row, QPointF windowPosition);
    void windowCornerRadiusChanged();
    void multimediaReadyChanged();
    void activeTabRootPaneChanged();
    void titleBarVisibleChanged();
    void nativeControlsInsetChanged();
    /// The band's published geometry or request changed (@see refreshTitleBand).
    void titleBandChanged();
    /// The OS changed how the maximise button looks (@see nativeMaximizeVisual).
    void nativeMaximizeVisualChanged();
    void tabBarPositionChanged();
    void tabBarVisibilityChanged();
    void tabBarShouldShowChanged();
    /// The chrome focus moved: the area, the focused tab or the focused toolbar item changed.
    void chromeFocusChanged();
    /// A key of the focused sidebar tree that the tree itself answers (the collapse, expand, rename and
    /// filter commands, and the row movement and activation): the tree is QML's, so it carries the command
    /// out.
    /// @param command A window::ChromeCommand as its underlying value.
    /// @param text The typed text of a filter command, else empty.
    void sidebarCommandRequested(int command, QString const& text);
    /// The RenameWorkspace action: the sidebar edits the shown workspace's name inline, on the row
    /// SidebarModel::currentWorkspaceIndex names.
    void workspaceRenameRequested();
    /// A sidebar row's menu asked to rename it (Rename… on a workspace or a tab row): the sidebar opens its
    /// inline editor on that row, wherever the window is.
    /// @param row The row, as SidebarModel lists it.
    void sidebarRowRenameRequested(int row);
    /// The sidebar opens its filter field and puts the keyboard in it (openSidebarFilter()).
    void sidebarFilterRequested();
    /// @see otherWorkspacesUrgency
    void otherWorkspacesUrgencyChanged();
    /// Closing a tab may end a running program, so the window asks first; QML shows the dialog and calls
    /// confirmCloseTab() on "Close". The tab is named by its id, not its row: rows shift while the question
    /// is open.
    /// @param tab The tab's vtworkspace::TabId value.
    /// @param tabLabel The tab's resolved label.
    /// @param program The running program's name; empty when no pane could name one.
    void closeTabConfirmationRequested(quint64 tab, QString const& tabLabel, QString const& program);
    /// The tab a close question was asked about is going away (it closed, or moved to another window), so
    /// QML withdraws the question.
    /// @param tab The tab's vtworkspace::TabId value.
    void closeTabConfirmationWithdrawn(quint64 tab);
    /// A close from the sidebar's menu always asks first, whatever runs in the node: QML shows the one
    /// confirmation dialog, worded by closeConfirmation(), and calls confirmClose() on the destructive
    /// button.
    /// @param kind A window::CloseKind as an int.
    /// @param workspace The node's workspace id.
    /// @param tab The node's tab id; 0 for a workspace.
    /// @param pane The node's pane key; 0 for a workspace or a tab.
    /// @param label The row's title.
    /// @param program The program the row's status is about; empty when unknown.
    void sidebarCloseConfirmationRequested(
        int kind, quint64 workspace, quint64 tab, quint64 pane, QString const& label, QString const& program);
    /// Closing a workspace may end running programs, so the window asks first (the same question as for a
    /// tab, over every pane of the workspace); a dialog calls confirmCloseWorkspace() on "Close".
    /// @param workspace The workspace's id, as the window's directory lists it.
    /// @param workspaceLabel The workspace's label; empty when it has none.
    /// @param program The running program's name; empty when no pane could name one.
    void closeWorkspaceConfirmationRequested(quint64 workspace,
                                             QString const& workspaceLabel,
                                             QString const& program);
    void activeSessionChanged();
    /// activeStickyHeader() may have changed: the active session did, or its header.
    void activeStickyHeaderChanged();
    void chromeHeightChanged();
    /// @see sidebarVisibility
    void sidebarVisibilityChanged();
    /// @see sidebarWidth
    void sidebarWidthChanged();
    /// @see tabPlacement
    void tabPlacementChanged();
    /// @see hostingNotice
    void hostingNoticeChanged();
    /// @see hostLabel
    void hostLabelChanged();
    /// What the window's directorySource() lists changed, or the source is another. Raised for both kinds of
    /// window: by the session manager for an in-process source (InProcessDirectory), and by the window's link
    /// for a daemon's, whenever the connection's directory changes. Not coalesced: one per change.
    void workspaceDirectoryChanged();
    /// @see hostChipState
    void hostChipStateChanged();
    /// @see requestNotification.
    void notificationRequested(QString const& title, QString const& body);
    /// @see requestBell.
    void bellRequested(QString const& source, double volume);
    /// @see requestAlert.
    void alertRequested();
    // {{{ Panes
    /// @see visiblePaneCount
    void visiblePaneCountChanged();
    /// @see headerReveal
    void headerRevealChanged();
    // }}}

  private:
    /// Follows the active session's stickyHeaderChanged() for activeStickyHeaderChanged(), dropping the
    /// previous session's. Run on every activeSessionChanged(). GUI thread.
    void followActiveStickyHeader();

    /// The connection followActiveStickyHeader() made to the active session. GUI thread.
    QMetaObject::Connection _activeStickyHeaderConnection;

    /// The backing vtworkspace::Workspace, or nullptr if it has been removed.
    [[nodiscard]] vtworkspace::Workspace* window() const noexcept;
    /// The active tab of this window's vtworkspace::Workspace, or nullptr.
    [[nodiscard]] vtworkspace::Tab* activeModelTab() const noexcept;
    /// The model tab backing list-model row @p index, or nullptr.
    [[nodiscard]] vtworkspace::Tab* tabAtRow(int index) const noexcept;
    /// The row index of @p tab in this window, or -1.
    [[nodiscard]] int rowOfTab(vtworkspace::TabId tab) const noexcept;
    /// @return The id of the tab on row @p row; nothing when there is none.
    [[nodiscard]] std::optional<vtworkspace::TabId> tabIdAtRow(int row) const noexcept;
    /// The strip's label for list-model row @p row, through the model's own TitleRole.
    [[nodiscard]] std::string titleAtRow(int row) const;
    /// The program-status badge @p tab shows, from the records of all its panes, each read under its
    /// terminal's lock.
    [[nodiscard]] ProgramStatusBadge programStatusBadge(vtworkspace::Tab const& tab) const;
    /// programStatusBadge(), computed once until the row changes or another tab comes in front, which marks
    /// its panes' records seen (@ref _badges).
    [[nodiscard]] ProgramStatusBadge cachedProgramStatusBadge(vtworkspace::Tab const& tab) const;
    /// The badge @p tab shows: its program status against its panes' unseen output and finished
    /// commands (the manager's TabAttention), resolved by resolveTabBadge().
    /// @param tab The tab.
    /// @return The badge, with a Working one's progress.
    [[nodiscard]] TabBadge tabBadge(vtworkspace::Tab const& tab) const;
    /// @return Whether any pane of @p tab shows the lock.
    [[nodiscard]] SecureIndicator tabSecureIndicator(vtworkspace::Tab const& tab) const;
    /// @p session's foreground process name, asked once until the next TitleRole refresh (@ref
    /// _processNames). Only a TitleRole change re-reads it, so a tab keeps the name of the process it first
    /// found until its title changes; Step 2's foreground poll is what invalidates it on a process change.
    [[nodiscard]] std::string const& cachedProcessName(session::TerminalSession const& session) const;
    /// @return What names the other machine this window's daemon runs on (a saved host, or the chrome's label
    /// for
    ///         a `--connect-tcp` daemon); empty for a daemon on this machine and a local window.
    [[nodiscard]] std::string otherMachineName() const;

    /// @return What names a tab whose label came out empty: the pane's foreground process, else the other
    ///         machine its daemon runs on, else the shell its profile spawns (each asked only when the
    ///         one before named nothing: a daemon-hosted pane reports no process, and another machine's shell
    ///         is only guessed from this client's profile).
    [[nodiscard]] std::string blankTabName(session::TerminalSession const& session) const;

    /// Resolves the label shown on @p row's tab.
    [[nodiscard]] QString resolvedTabLabel(vtworkspace::Tab* tab,
                                           session::TerminalSession* session,
                                           int row) const;

    /// Resolves the OS window to operate on for a geometry request from @p requester, adopting the
    /// requester's QQuickWindow as _osWindow when none is recorded yet (same rule as focusDisplay).
    /// @return The window to operate on, or nullptr when none exists.
    [[nodiscard]] QQuickWindow* osWindowFor(session::DisplaySurface& requester) noexcept;

    /// The declared chrome as the geometry module's type: the workspace sidebar's width, and the height
    /// Main.qml declares.
    [[nodiscard]] geometry::Chrome chrome() const
    {
        return { .width = sidebarWidth(), .height = _chromeHeight };
    }

    /// Takes the sidebar's starting state: what the user last chose (GuiState::sidebar) where there is
    /// any, else the configuration's. Once, at construction.
    void restoreSidebar();

    /// Writes the sidebar's state to the GUI state, as the next window's starting point.
    void rememberSidebar();

    /// @return The id of the workspace the window shows, as its directory lists it; none without one.
    [[nodiscard]] std::optional<uint64_t> shownWorkspaceId() const;
    /// Moves the shown workspace @p by places in the list.
    void moveShownWorkspaceBy(int by);
    /// Moves the workspace @p workspace @p by places in the list; nothing past either end.
    /// @param workspace The workspace's id, as the directory lists it.
    void moveWorkspaceBy(uint64_t workspace, int by);
    /// Moves a pane or a tab as the sidebar asks (ISidebarHost::moveNode): through the manager.
    /// @param node The pane or the tab, as the directory names them.
    /// @param destination Where it goes; a MoveTab's position is the 0-based place it ends at.
    void moveSidebarNode(SidebarNode const& node, NodeDestination const& destination);
    /// @return What a menu on @p row moves: its pane, its tab, or a workspace's only tab; none for a
    ///         workspace of several tabs.
    [[nodiscard]] std::optional<SidebarNode> sidebarNodeOf(SidebarRowKey const& row) const;
    /// @return The active pane, as the directory names it; none without one.
    [[nodiscard]] std::optional<SidebarNode> activePaneNode() const;
    /// Moves @p node into a tab, by the 1-based places MoveToTab names; nothing where they name none.
    void moveNodeToTab(SidebarNode const& node, uint32_t workspace, uint32_t tab);
    /// Moves the pane @p node into a tab of its own at the end of its workspace; nothing for a tab, or for a
    /// pane that is its tab's only one.
    void moveNodeToNewTab(SidebarNode const& node);
    /// Moves tab @p tab to the end of another workspace, as MoveTabToWorkspace does.
    /// @param tab The tab, as the directory names it.
    /// @param position The target workspace's 1-based place in the window's workspace list.
    void moveTabToWorkspaceAt(TabRef tab, uint32_t position);

    /// The sidebar's bounds changed or it moved: the window's chrome changed with it.
    void refreshSidebarChrome();

    /// Current physical row grid, absent when disabled or more than one pane is visible.
    [[nodiscard]] std::optional<geometry::RowResizeGrid> rowResizeGrid() const;

    /// Shared tail of the choke-point entries: guards the window state, solves the pane tree for
    /// @p requester's leaf, adds the declared chrome and issues the single window resize.
    /// @param requester          The display whose pane the request targets.
    /// @param leafContentLogical The pane's required content extent in logical pixels.
    /// @return True if a window resize was issued.
    bool applyContentDrivenResize(session::DisplaySurface& requester,
                                  geometry::LogicalSize leafContentLogical);

    /// Scale-settlement handler (DevicePixelRatioChange / screenChanged): when the window's resolved
    /// content scale actually changed, re-derives the font DPI (grid reflows in place) and issues ONE
    /// queued grid-preserving corrective window resize (user decision: monitor moves and late Wayland
    /// fractional-scale arrival keep the grid, not the pixel size). Idempotent per scale value.
    void onWindowScaleMaybeChanged();

    /// Restores the captured grid after DPI processing and the native move/resize loop finish.
    void settlePendingWindowScaleChange();

    /// OS window activation handler (QWindow::activeChanged): grants focus ownership to this window
    /// when it becomes the active OS window and revokes it when it stops being active, so alt-tabbing
    /// away from Contour notifies the shell (DECSET 1004) even when no display holds Qt item focus.
    void onOSWindowActiveChanged();

    /// Feeds one input event of the bound window to the held-modifier reveal: a lone press of the reveal
    /// modifier arms it, a release or a window deactivation releases it, any other key or a wheel turn
    /// disarms it, and a button press disarms a hold still pending (not a reveal already shown, whose buttons
    /// it may be clicking). Auto-repeated presses are not presses: a held key repeats on some platforms.
    /// @param event The event the window is about to deliver; it is not consumed.
    void followHeldModifier(QEvent const& event);

    /// Asks the held-modifier reveal where it stands now, re-arms the timer for a hold still pending, and
    /// on a change publishes headerReveal and refreshes who follows the foreground process.
    void refreshHeaderReveal();

    /// Which decoration this window is currently showing, as the frame policy names it.
    [[nodiscard]] platform::WindowDecoration decoration() const noexcept;

    /// Re-decides windowCornerRadius() and notifies when it changed. Called with the shadow (presentation,
    /// decoration) and with the title band (window state, chrome row).
    void refreshWindowCornerRadius();

    /// Re-publishes the window's drop shadow for the state it is now in.
    ///
    /// A frameless window gets no server-side decoration, and on KWin the shadow is drawn as part of
    /// that decoration — so the window has to hand the compositor its own. Called from every place
    /// that changes an input to the decision: the first map, the presentation transitions, the
    /// title-bar toggle, and a window-manager-driven maximize.
    ///
    /// Creates the attachment on first use rather than in bindWindow(), because resolving it needs
    /// the platform window, and forcing that into existence before showInitial() has sized the
    /// still-unmapped window is exactly the premature realization WindowGeometry.hpp warns about.
    void refreshWindowShadow();

    /// Makes @p window this controller's OS window (_osWindow), and drops the helpers built from it once it
    /// is destroyed (@see dropWindowHelpers).
    /// @param window The window adopted; not null.
    void adoptOsWindow(QQuickWindow* window);

    /// Drops the helpers built from an OS window this controller adopted, once that window is destroyed: the
    /// shadow and the resize grid reach the window they were made for, and a caller after the window's end
    /// -- a daemon link released in the application's teardown, which re-reads the title band -- must find
    /// none rather than one that reaches freed memory. The shadow is built again from the next window on its
    /// next refresh, the resize grid by the next bindWindow().
    void dropWindowHelpers();

    /// QWindow::visibilityChanged handler.
    ///
    /// Every way the window changes presentation ends in this signal — our own setWindowMaximized
    /// and friends, the toggles, and the ones driven from outside the application entirely (a
    /// window-manager keybinding, a double click on the native frame) — so this one connection
    /// covers all of them, and the shadow reads the presentation back off the window rather than
    /// having it recorded at each site.
    void onWindowVisibilityChanged();

    /// QWindow::windowStateChanged handler: minimising and restoring, which a window hidden from the
    /// screen also goes through without its visibility changing.
    void onWindowStateChanged();

    /// @return The colour scheme the chrome shows: the one last forced onto it, else what Qt reports.
    [[nodiscard]] Qt::ColorScheme effectiveColorScheme() const;

    /// @return Whether this window's frame hosts the title-band material (platform::hostsTitleBandMaterial).
    [[nodiscard]] bool dressesTitleBand() const noexcept;

    /// @return The band's surface for the window's frame, mode and band centre (titleBand()): the chrome
    ///         row's material, made opaque where the frame hosts no material or the material failed.
    [[nodiscard]] TitleBandSurface titleBandSurface() const;

    /// @return FullScreen while the bound OS window is in full screen, else Windowed (also unbound).
    [[nodiscard]] WindowMode windowMode() const noexcept;

    /// The frame policy for this window's platform and current decoration.
    [[nodiscard]] platform::FramePolicy framePolicy() const noexcept;

    /// Attaches the bound OS window to the app's caption adapter while its frame is the kept Windows frame
    /// (FrameShadowStrategy::NativeFrameKept), and detaches it otherwise. Cheap to repeat. GUI thread.
    void refreshCaptionAttachment();

    /// Detaches the bound OS window from the caption adapter, if attached, and rests the button. GUI thread.
    void detachCaption();

    /// Stores @p visual and emits nativeMaximizeVisualChanged() when it differs. GUI thread.
    void setNativeMaximizeVisual(platform::CaptionButtonVisual visual);
    /// Makes a drop's move now, or once the node drag in flight has returned
    /// (TerminalSessionManager::afterNodeDrag).
    void placeNode(session::ResolvedDrag drag, vtworkspace::MoveTarget target);
    /// Starts the drag of @p drag (@see beginPaneDrag).
    [[nodiscard]] QString beginNodeDrag(std::optional<NodeDrag> drag, session::TerminalSession* carried);

    session::TerminalSessionManager& _manager;
    WindowId _windowId;
    vtworkspace::WorkspaceId _displayed;                   ///< The workspace the tab strip lists.
    IWorkspaceDirectorySource* _directorySource = nullptr; ///< @see directorySource
    /// The pane a jump waits to show, and the directory workspace it selected for it.
    struct PendingJump
    {
        PaneKey pane;       ///< The pane to show.
        uint64_t workspace; ///< The directory's id of the workspace holding it.
    };
    /// Counts the directory's entry for @p key as seen, as far as the daemon's flags go (the serials).
    void noteDirectoryPaneSeenByKey(PaneKey key);
    std::optional<PendingJump> _pendingJump; ///< The jump whose workspace is being selected.
    /// A pane moved into a new workspace of the daemon's, which the window shows once it is listed.
    struct PendingFollow
    {
        PaneKey pane;                 ///< The pane moved.
        std::vector<uint64_t> listed; ///< The workspaces listed when it was asked: none of them is new.
        core::platform::SteadyTimePoint giveUp; ///< When it is no longer waited for.
    };
    std::optional<PendingFollow> _pendingFollow; ///< @see followIntoNewWorkspace
    /// Shows the new workspace a pending follow waits for, if the directory lists it now.
    void followMovedPane();
    /// Whether a sidebar row's action ran, or waits for the pane it needs.
    enum class RowActionOutcome : uint8_t
    {
        Waiting = 0, ///< The pane it needs is not there yet; it stays pending.
        Done,        ///< It ran.
    };
    /// What waits for a workspace to be shown: a sidebar menu's action on one of its rows, or a move.
    struct PendingRowAction
    {
        uint64_t workspace; ///< The directory's id of the workspace that must be shown first.
        std::function<RowActionOutcome()> run; ///< What to do once it is.
    };
    std::optional<PendingRowAction> _pendingRowAction;
    /// The sidebar row an action is being run for, while it runs (goToSidebarPane() reads it).
    std::optional<SidebarRowKey> _sidebarActionRow;
    /// The row the sidebar's menu was last opened over.
    std::optional<SidebarRowKey> _sidebarMenuRow;
    /// The tabs that menu offered, by id: a Move to Tab row resolves by them, not by the places as they are
    /// now.
    std::vector<OfferedTab> _sidebarMenuTabs;
    QPointF _sidebarMenuAnchor; ///< Where that menu was asked for, in window coordinates.
    /// Runs @p run once the directory shows @p workspace: now when it does, else after selecting it. A @p run
    /// that is Waiting (its pane is not there yet) stays pending, as a jump does, until the layout lands;
    /// showing the workspace without it ends the wait (cancelPendingJump()).
    /// @param workspace The directory's id of the workspace.
    /// @param run What to do.
    void afterShowing(uint64_t workspace, std::function<RowActionOutcome()> run);
    /// Runs @p action for sidebar row @p row: by the row's id, leaving the window where it is, where the
    /// action has such a form (Mark seen, Rename, Move, Close Workspace), else on what the window shows, once
    /// the row is shown.
    void runSidebarAction(SidebarRowKey const& row, actions::Action const& action);
    /// Runs @p action on what the window shows, once @p row is shown (the tail of runSidebarAction()).
    void runSidebarActionShown(SidebarRowKey const& row, actions::Action const& action);
    /// Asks, through sidebarCloseConfirmationRequested(), whether to close the node @p key stands for.
    void askToClose(CloseKind kind, SidebarRowKey const& key);
    /// Builds and presents the menu of sidebar row @p row.
    void openSidebarContextMenu(SidebarRow const& row, QPointF windowPosition);
    /// Tells the sidebar whether anyone can see it: it is shown, in an exposed window.
    void refreshSidebarPresence();
    class SidebarHost;
    /// Builds the sidebar and its host, once the window exists.
    void initSidebar();
    std::vector<vtworkspace::WorkspaceId> _owned; ///< Every workspace of this window; @see ownedWorkspaces
    TabAttention _attention;                      ///< This window's seen-state, by PaneKey.
    /// Shows the ssh prompts of the attaches this window started; attached to the app's router for the
    /// window's whole life.
    std::unique_ptr<PromptHost> _promptHost;
    std::vector<TabSpan> _tabSpans; ///< Where the strip's tabs are, as setTabSpans() last reported.
    // Retained until the first normal expose so late native frame margins can be clamped too.
    WindowRole _role; ///< @see role
    std::optional<WindowPosition> _initialPosition;
    std::optional<WindowPosition> _lastWindowPosition;

    /// Records normal-window moves without persisting maximize/fullscreen coordinates.
    void rememberWindowPosition();
    /// Clamps and records the initial normal placement after the configured size has been calculated.
    void restoreWindowPosition();

    // {{{ Menu routing
    /// The published menu of @p surface.
    [[nodiscard]] PublishedContextMenu const& menu(MenuSurface surface) const noexcept
    {
        return _menus[static_cast<std::size_t>(surface)];
    }

    /// Shows @p tree as @p surface's menu: natively at the pointer, over this window, when the style asks
    /// for it and the platform can, otherwise in the scene, where alone it is published as the model. The
    /// native presentation is posted, so it runs once the press handler that asked for it has returned.
    /// @param surface The surface the menu opens from.
    /// @param tree The resolved menu.
    /// @param anchor Where it opens, in the window's coordinates; the pointer when absent. A position, not a
    ///        flag.
    void presentSurface(MenuSurface surface,
                        command::MenuTree tree,
                        std::optional<QPointF> anchor = std::nullopt);

    /// Publishes @p tree as @p surface's in-scene menu model, and asks the QML to pop it.
    /// @param surface The surface whose QML menu should open.
    /// @param tree The resolved menu.
    void showInScene(MenuSurface surface, command::MenuTree tree);

    /// What a menu acts on, pinned when it opens: the pane menu's session and the tab menu's tab.
    struct MenuTarget
    {
        QPointer<session::TerminalSession> session {}; ///< The pane menu's session; null once it has died.
        std::optional<vtworkspace::TabId> tab {};      ///< The tab menu's tab.
        std::optional<SidebarRowKey> sidebarRow {};    ///< The sidebar menu's row.
    };

    /// The targets the menus last opened were pinned to.
    [[nodiscard]] MenuTarget currentMenuTarget() const;

    /// Runs the action row @p actionId of @p surface's published menu against the current targets: the
    /// in-scene (QML) path. Out-of-range ids, and a target that has gone, are ignored.
    /// @param surface The surface the row belongs to.
    /// @param actionId The row's `actionId`.
    void triggerSurfaceAction(MenuSurface surface, int actionId);

    /// Runs @p action from @p surface's menu against @p target.
    /// @param surface The surface the action's menu opened from.
    /// @param target What that menu was opened over; a target that has gone makes this a no-op.
    /// @param action The action to run.
    void runSurfaceAction(MenuSurface surface, MenuTarget const& target, actions::Action const& action);

    /// Resolves @p entries into a tree with the current key bindings' chords.
    /// @param entries The menu rows.
    /// @return The resolved menu.
    [[nodiscard]] command::MenuTree resolveMenu(std::span<command::ContextMenuEntry const> entries) const;
    // }}}

    // Wheel state for the tab strip. Separate from the grid's (which lives on TerminalSession) so a
    // gesture over the strip is judged on its own, and so the accumulator does not jump when the
    // pointer crosses between strip and grid mid-scroll.
    input::HorizontalWheelGesture _tabStripWheelGesture;
    int _tabStripWheelAccumulator = 0;

    // {{{ Keyboard focus in the chrome (GUI thread only)
    /// Moves focus into @p area, seeding the focused item of the area it enters.
    void enterChromeArea(window::ChromeArea area);
    /// Ends the chrome's focus without touching the active focus item.
    /// @return Whether the chrome had focus.
    [[nodiscard]] bool dropChromeFocus();
    /// Keeps the focused item inside the area after the tab count or the toolbar changed.
    void clampChromeFocus();
    /// Runs @p command on the focused area.
    /// @param command What the key does in the focused area.
    /// @param text The typed text of a Filter command.
    /// @return Whether the area handled it.
    [[nodiscard]] window::ChromeKeyOutcome runChromeCommand(window::ChromeCommand command,
                                                            QString const& text = {});
    /// The actions the active session's key bindings give a key press while the chrome has focus.
    /// @return The bound actions; nullptr when nothing is bound or there is no active session.
    [[nodiscard]] actions::ActionList const* boundActionsFor(int qtKey,
                                                             Qt::KeyboardModifiers modifiers,
                                                             QString const& text) const;
    /// Closes the tab at @p row, asking first unless every pane is known to be idle (a pane whose program
    /// cannot be known asks too: window::closeConfirmationFor).
    void closeTabAskingIfBusy(int row);

    window::ChromeArea _chromeArea = window::ChromeArea::Terminal;
    window::ChromeAreaPresence _chromePresence {};
    /// The areas the window shows right now: what QML declared, and the sidebar from its visibility.
    [[nodiscard]] window::ChromeAreaPresence currentChromePresence() const noexcept;
    /// The tab the tab well's keyboard focus is on, by identity rather than by row: the model changes rows
    /// under it (an insert, a move in from another window, a reorder), but never which tab it is.
    std::optional<vtworkspace::TabId> _focusedTab;
    int _focusedToolbarIndex = -1;
    int _toolbarItemCount = 0;
    /// Where the sidebar's toggle stands, as Main.qml last reported it (setSidebarToggleHost()).
    window::SidebarToggleHost _sidebarToggleHost = window::SidebarToggleHost::None;
    /// Whether the tab holding the chrome focus is being removed.
    enum class FocusedTabRemoval : uint8_t
    {
        None,
        Pending, ///< onTabClosed() then ends the chrome focus.
    };
    FocusedTabRemoval _focusedTabRemoval = FocusedTabRemoval::None;
    // }}}

    // {{{ Command palette
    // The sources this window's palette draws from, held by value and in PRECEDENCE order (see
    // collectCommands(): first source wins a duplicate id). The live-state sources come first so their
    // richer rows beat the generic ones — "Switch To Tab 2: vim" from the tab source outranks the bare
    // "Switch To Tab 2" a key binding would contribute. The action catalog comes last: it is the floor
    // that guarantees an unbound action is still reachable. Declared before _commandPalette, which is
    // handed their addresses in the member-initializer list.
    command::TabCommandSource _tabCommands;
    command::ProfileCommandSource _profileCommands;
    command::LayoutCommandSource _layoutCommands;
    command::HostCommandSource _hostCommands;
    command::DaemonCommandSource _daemonCommands;
    command::BoundCommandSource _boundCommands;
    command::ActionCommandSource _actionCommands;
    // The two sources that offer actions without knowing the window, minus the switching actions a window
    // with no daemon behind it cannot run.
    command::SwitchingGatedSource _gatedBoundCommands;
    command::SwitchingGatedSource _gatedActionCommands;
    std::unique_ptr<CommandPaletteModel> _commandPalette;
    // }}}

    // {{{ Menu routing
    /// Whether menus open natively or in the scene, as the constructor was handed it.
    config::MenuPresentation _menuPresentation = config::MenuPresentation::InScene;
    /// Shows a menu natively; the app's, which outlives every window.
    platform::INativeMenuPresenter& _nativeMenus;
    // }}}

    QString _hostingNotice; ///< @see hostingNotice

    /// Each tab's badge, computed once for the three roles that show it. Mutable: data() fills it.
    mutable TabBadgeCache _badges;

    /// The foreground process name of each session whose tab label fell back to it, by session id: asking
    /// costs a process-table query, and the strip reads TitleRole on every repaint. Dropped whenever a
    /// TitleRole refresh is emitted, so it is as fresh as the label; nothing tells of a new foreground
    /// process yet. Mutable: data() fills it.
    mutable std::unordered_map<uint64_t, std::string> _processNames;

    // {{{ Terminal context menu
    /// Each surface's menu as QML sees it, rebuilt on every open, indexed by MenuSurface. The rows' actions
    /// are held here rather than in the QML so a row runs the exact action it was built with -- no lookup
    /// by name at click time.
    std::array<PublishedContextMenu, MenuSurfaceCount> _menus {};

    /// The tab the tab menu was opened over. Pinned by id, so a pick acts on that tab wherever it has
    /// moved and does nothing once it has closed.
    std::optional<vtworkspace::TabId> _tabContextMenuTab {};

    /// The session the menu was BUILT over — the pane the user right-clicked.
    ///
    /// Pinned, not looked up again when a row is picked. The active pane can change while the menu stands
    /// open (the clicked pane's shell exits, the model activates a sibling in its place), and "Close Pane"
    /// or "Paste" would then land on a terminal the user never pointed at. A QPointer, so a pane that dies
    /// under the open menu makes the row a no-op rather than a use-after-free.
    QPointer<session::TerminalSession> _contextMenuSession;

    /// Where the pane menu opens, when it was not at the pointer. @see contextMenuAnchor
    std::optional<QPointF> _contextMenuAnchor {};
    // }}}

    /// What a click on the tray message last shown does, besides bringing the window forward.
    std::function<void()> _trayMessageClicked;

    display::TerminalDisplay* _activeDisplay = nullptr;
    // The OS window (QQuickWindow) this controller adapts, adopted from the first display that focuses in.
    // Lets the manager route a focus/close to the controller that owns a given display's window. A QPointer:
    // the QML engine destroys the window before the controller, and a daemon link released after that (the
    // application's teardown stops its links once the windows are gone) still re-reads the title band. Set
    // by adoptOsWindow() only, which drops what was built from it as it goes (dropWindowHelpers()).
    QPointer<QQuickWindow> _osWindow;

    // Guards closeWindow() against re-entry: closeWindow() closes _osWindow, whose QML `onClosing`
    // calls closeWindow() again — the flag makes that second call a no-op so teardown runs exactly once,
    // whichever door (user close button vs. programmatic close of an emptied window) started it.
    bool _closing = false;

    // PaneProxy tree for this window's active tab. Owned here, reused by PaneId across rebuilds so a
    // surviving pane's QML item / display is not torn down when a sibling splits or closes.
    std::unordered_map<uint64_t, session::PaneProxy*> _paneProxies;
    session::PaneProxy* _activeTabRootProxy = nullptr;
    // @see visiblePaneCount. Recounted with the layout root (refreshActiveTabLayoutRoot()).
    int _visiblePaneCount = 0;
    // What, besides the density, reveals this window's pane headers (@see refreshHeaderFollowers): what
    // _heldModifier said when last asked (refreshHeaderReveal()).
    HeaderReveal _headerReveal = HeaderReveal::None;
    // The reveal modifier's hold, fed by eventFilter() and measured against _clock.
    core::platform::IClock const& _clock;
    core::platform::IWallClock const& _wallClock; ///< What the sidebar's times are measured against.
    HeldModifierReveal _heldModifier;
    // Fires when a hold that is still armed reaches its delay, for refreshHeaderReveal() to show the
    // headers then: nothing else would ask while the modifier is merely held.
    QTimer _headerRevealTimer;

    // The names of the layouts saved, as beginSaveLayoutPrompt() last read them (the layouts file's
    // included), for checkLayoutName(), which runs on every key typed.
    std::vector<std::string> _savedLayoutNames;

    // Says this window's notices to a screen reader, against the window while it is shown. Declared before
    // _notices, which speaks through it.
    std::unique_ptr<platform::Announcer> _noticeAnnouncer;
    // This window's toast and banner, timed by _clock.
    NoticeModel _notices;
    // The sidebar's window-facing half and its model; the model is declared last, as it uses the rest.
    std::unique_ptr<SidebarHost> _sidebarHost;
    std::unique_ptr<SidebarModel> _sidebar;

    // Carries beginMoveRows()'s result from onTabAboutToBeMoved() to onTabMoved().
    bool _tabMoveInProgress = false;

    // The window's drop shadow. Created lazily on the first refresh (see refreshWindowShadow) and
    // destroyed before the window closes, while its handle is still live enough to withdraw through.
    //
    // The presentation it is published for is DERIVED from the window on each refresh rather than
    // held here: a mirrored member needs a writer at every transition, and one refactor away from
    // having none it silently stops withdrawing the shadow at all. WindowPresentation::Tiled is
    // unreachable today — Qt reports no such visibility, so a KWin quick-tiled window keeps its
    // shadow, as every other client-decorated application does.
    std::unique_ptr<platform::WindowShadowController> _windowShadow;

    // The corner radius last announced through windowCornerRadiusChanged, so a refresh that changes
    // nothing stays silent; empty until the first refresh, which always announces.
    std::optional<int> _publishedCornerRadius {};

    // The OS-frame adapter. Never null: platforms with nothing to offer get a NullWindowFrame, so
    // every call site can apply the policy unconditionally rather than guarding on the platform.
    std::unique_ptr<platform::NativeWindowFrame> _nativeFrame;

    // The OS window attached to the app's caption adapter, if any. A QPointer, so a window destroyed while
    // attached is not detached through a dangling reference (the adapter forgets a destroyed window itself).
    QPointer<QWindow> _captionWindow;
    // The caption buttons' scene rectangles, as WindowControls.qml reports them (logical pixels).
    platform::CaptionLayout _captionLayout {};
    // The control that reported each of them last, by WindowControlKind: the only one whose clear counts.
    std::array<QPointer<QObject>, config::WindowControlCount> _captionOwners {};
    // How the maximise button looks while the OS drives it.
    platform::CaptionButtonVisual _nativeMaximizeVisual = platform::CaptionButtonVisual::Rest;

    // The title band request last applied to this window's platform window, if any; forgotten when the
    // decoration changes, so the next refresh applies again.
    std::optional<platform::TitleBandRequest> _appliedTitleBand;
    // The request and the geometry refreshTitleBand() last published (the properties read these).
    std::optional<platform::TitleBandRequest> _publishedTitleBand;
    std::optional<int> _publishedTitleBandFill;
    // Whether this window's title-band material still works; Failed for the rest of its life once it failed.
    platform::BandMaterialHealth _bandMaterialHealth = platform::BandMaterialHealth::Working;
    TitleBandGeometry _titleBandGeometry {};

    // Window-scoped native subscription; its geometry provider reads the live configuration.
    std::unique_ptr<QObject> _resizeGrid;

    // Window-scoped title-bar visibility (see the decoration block above). Seeded once from the
    // profile's show_title_bar (first display attach), flipped by ToggleTitleBar.
    bool _titleBarVisible = true;
    // First-write-wins latch for seedTitleBarVisible(): session rebinds must not reset a runtime toggle.
    bool _titleBarSeeded = false;

    // Window-scoped tab strip placement + visibility (see the tab-strip block above). Seeded once from
    // the profile's tab_bar_position / tab_bar_visibility (first display attach). Defaults mirror the
    // ConfigEntry defaults so a window with no seed yet behaves like the historical Top/Always.
    config::TabBarPosition _tabBarPosition = config::TabBarPosition::Top;
    bool _tabBarPositionSeeded = false;
    config::TabBarVisibility _tabBarVisibility = config::TabBarVisibility::Always;
    bool _tabBarVisibilitySeeded = false;

    // The workspace sidebar: whether it is shown, its width in the chrome row's units (restoreSidebar() takes
    // it; pixels or cells, as the row counts) and where the tabs live. The last two do not change over a
    // window's life yet: a configuration reload re-applies neither, as `ui_style` does not either.
    config::SidebarVisibility _sidebarVisibility = config::SidebarVisibility::Hidden;
    int _sidebarUnits = 0;
    config::TabPlacement _tabPlacement = config::TabPlacement::Top;

    // Declared title-bar chrome height in logical px (written by Main.qml's binding).
    int _chromeHeight = 0;
    // Whether the window was maximized before entering fullscreen (restored on toggle-out).
    bool _maximizedState = false;
    // The content scale the geometry was last derived for (settlement idempotence latch).
    double _lastAppliedScale = 0.0;
    // Page captured before native DPI resizing; also coalesces the queued settlement callback.
    struct PendingScalePage
    {
        vtbackend::PageSize page;
        int sessionId;
    };
    std::optional<PendingScalePage> _pendingScalePage;
    platform::WindowMoveResizePhase _nativeMoveResizePhase = platform::WindowMoveResizePhase::Idle;
    // True while our own content-driven resize is in flight (straddle guard, see onWindowScaleMaybeChanged).
    bool _inContentDrivenResize = false;
};

} // namespace contour::window
