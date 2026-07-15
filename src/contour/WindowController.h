// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <contour/CommandCatalog.h>
#include <contour/CommandPaletteModel.h>
#include <contour/display/TerminalDisplay.h>

#include <vtbackend/Color.h>

#include <QtCore/QAbstractListModel>
#include <QtGui/QColor>
#include <QtQml/QtQml>

#include <cstdint>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include <vtmux/Primitives.h>

namespace vtmux
{
class Tab;
class Window;
} // namespace vtmux

namespace contour
{

class TerminalSessionManager;
class TerminalSession;
class PaneProxy;
class SettingsController;

/// Per-OS-window Qt/QML adapter over one vtmux::Window.
///
/// Clean-architecture role: this is the ADAPTER between the pure, Qt-free domain core (vtmux::Window /
/// SessionModel) and the QML window (ApplicationWindow / main.qml). One instance exists per OS window.
/// It owns everything window-scoped and Qt-shaped:
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
/// TerminalSessionManager SERVICE, tagged with this controller's WindowId, so the model stays the single
/// source of truth and session ownership stays in one place. The manager also routes each vtmux
/// ModelEvent to the owning window's controller (the on*() hooks below).
///
/// Created only by TerminalSessionManager::createWindowController() (which mints the backing
/// vtmux::Window) and owned via QQmlEngine CppOwnership; destroyed when its ApplicationWindow closes.
class WindowController: public QAbstractListModel, public TabTitleProvider
{
    Q_OBJECT
    Q_PROPERTY(int count READ count NOTIFY countChanged)
    /// This window's command palette list. Per-window (not per-app) because the filter text and the
    /// selection are things the user is doing IN this window; the recently-used list behind it is
    /// app-wide and lives on the manager.
    Q_PROPERTY(contour::CommandPaletteModel* commandPalette READ commandPalette CONSTANT)
    Q_PROPERTY(QVariantList contextMenuModel READ contextMenuModel NOTIFY contextMenuModelChanged)
    Q_PROPERTY(int activeTabIndex READ activeTabIndex NOTIFY activeTabIndexChanged)
    Q_PROPERTY(bool multimediaReady READ isMultimediaReady NOTIFY multimediaReadyChanged)
    Q_PROPERTY(contour::PaneProxy* activeTabRootPane READ activeTabRootPane NOTIFY activeTabRootPaneChanged)
    Q_PROPERTY(contour::TerminalSession* activeSession READ activeSession NOTIFY activeSessionChanged)
    Q_PROPERTY(bool titleBarVisible READ titleBarVisible NOTIFY titleBarVisibleChanged)
    // Tab-strip (tab bar) placement + visibility, exposed to main.qml. `tabBarPosition` is an int
    // (0 = Top, 1 = Bottom) matching the config::TabBarPosition enumerator order. `tabBarShouldShow`
    // is the resolved gate (mode + live tab count) the QML binds its `visible` to.
    Q_PROPERTY(int tabBarPosition READ tabBarPosition NOTIFY tabBarPositionChanged)
    Q_PROPERTY(int tabBarVisibility READ tabBarVisibility NOTIFY tabBarVisibilityChanged)
    Q_PROPERTY(bool tabBarShouldShow READ tabBarShouldShow NOTIFY tabBarShouldShowChanged)
    Q_PROPERTY(int chromeHeight READ chromeHeight WRITE setChromeHeight NOTIFY chromeHeightChanged)
    Q_PROPERTY(bool settingsActive READ settingsActive NOTIFY settingsActiveChanged)
    Q_PROPERTY(contour::SettingsController* settingsController READ settingsController CONSTANT)
    QML_ELEMENT
    QML_UNCREATABLE("Created by the session manager")

  public:
    /// Tab-strip roles (one row per tab). Mirrors the old TerminalSessionManager::Roles so the delegates
    /// bind the same role names. The values are small (Qt::UserRole + n), so a 16-bit base suffices and
    /// they still convert cleanly to the `int role` of the QAbstractItemModel API.
    enum Roles : std::uint16_t
    {
        TitleRole = Qt::UserRole + 1, //!< Resolved (expanded) tab label; what the tab strip displays.
        ColorRole,                    //!< Tab accent color as QColor (transparent if uncolored).
        IsActiveRole,                 //!< Whether this tab is the active tab of this window.
        PaneCountRole,                //!< Number of panes in this tab.
        SessionIdRole,                //!< The session id of the tab's active leaf.
        RawTitleRole,                 //!< Un-expanded runtime rename template (empty if never renamed).
        ZoomedRole,                   //!< Whether this tab's active pane is zoomed (see vtmux::Tab).
    };

    /// @param manager  The session-lifetime service + model host (must outlive this controller).
    /// @param windowId The backing vtmux::Window this controller adapts.
    WindowController(TerminalSessionManager& manager, vtmux::WindowId windowId);
    ~WindowController() override;

    WindowController(WindowController const&) = delete;
    WindowController& operator=(WindowController const&) = delete;
    WindowController(WindowController&&) = delete;
    WindowController& operator=(WindowController&&) = delete;

    [[nodiscard]] vtmux::WindowId windowId() const noexcept { return _windowId; }

    // {{{ QAbstractListModel (tab strip)
    [[nodiscard]] QVariant data(QModelIndex const& index, int role = Qt::DisplayRole) const override;
    [[nodiscard]] int rowCount(QModelIndex const& parent = QModelIndex()) const override;
    [[nodiscard]] QHash<int, QByteArray> roleNames() const override;
    // }}}

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
    Q_INVOKABLE void moveTab(int fromIndex, int toIndex);
    /// The raw id of the window this controller adapts, so a QML drag payload can name its source
    /// window and a DropArea can target the destination window across the shared engine.
    Q_INVOKABLE [[nodiscard]] quint64 windowIdValue() const noexcept { return _windowId.value; }
    /// Moves the tab at @p fromIndex of the window identified by @p sourceWindowId into THIS window at
    /// @p toIndex — the drop of a tab dragged from another window's strip onto this one.
    /// @param sourceWindowId The dragged tab's source window id (from windowIdValue()).
    /// @param fromIndex      The tab's row in the source window.
    /// @param toIndex        The destination row in this window.
    Q_INVOKABLE void moveTabIntoThisWindow(quint64 sourceWindowId, int fromIndex, int toIndex);
    /// Tears the tab at @p index of THIS window out into a brand-new OS window (drop on empty desktop).
    /// @param index The tab's row in this window.
    Q_INVOKABLE void tearOffTab(int index);
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
    /// Asks the QML to open this window's "save layout as" name prompt (the keyboard/palette entry point
    /// for a nameless SaveLayout action). Emits saveLayoutRequested(); the dialog calls saveLayoutAs()
    /// back with the name the user types.
    Q_INVOKABLE void beginSaveLayoutPrompt();
    /// Saves this window's tabs as the layout named @p name (the "save layout as" prompt's accept), via
    /// the manager's saveLayout(). A blank name or a window with no active session is a no-op, so an
    /// empty prompt never writes a nameless layout.
    /// @param name The layout name the user typed.
    Q_INVOKABLE void saveLayoutAs(QString const& name);
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

    /// Closes this OS window: tears down its tabs/sessions and drops the controller. main.qml onClosing.
    Q_INVOKABLE void closeWindow();
    /// Whether this window may close now (its last pane exited). TerminalPane.onTerminated.
    Q_INVOKABLE [[nodiscard]] bool canCloseWindow() const noexcept;
    // }}}

    // {{{ Command palette
    /// This window's palette list model (never null; owned by this controller).
    [[nodiscard]] CommandPaletteModel* commandPalette() const noexcept { return _commandPalette.get(); }

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
    // }}}

    // {{{ Settings page
    /// Whether this window's content area currently shows the in-app settings page instead of the
    /// active tab's terminal pane tree. The content Loader in main.qml switches on this, so the settings
    /// page and the terminal are mutually-exclusive views of the same region (Windows-Terminal style).
    [[nodiscard]] bool settingsActive() const noexcept { return _settingsActive; }

    /// This window's editable settings bridge (never null; owned by this controller). The QML settings
    /// page binds to it for the profile/color-scheme/default-profile models and the save/delete actions.
    [[nodiscard]] SettingsController* settingsController() const noexcept
    {
        return _settingsController.get();
    }

    /// Shows the settings page over this window (the OpenConfiguration action's default, and the
    /// new-tab dropdown's "Settings" entry). Idempotent: showing it while already shown does nothing.
    Q_INVOKABLE void openSettings();

    /// Returns from the settings page to the active tab's terminal content. A no-op if not showing it.
    Q_INVOKABLE void closeSettings();

    /// Toggles between the settings page and the terminal content.
    Q_INVOKABLE void toggleSettings();
    // }}}

    // {{{ Terminal context menu
    /// Rebuilds the context menu against the CURRENT state of the active pane and asks the QML to pop it.
    ///
    /// Called by the manager for the OpenContextMenu action, which has already made the right-clicked
    /// pane the active one — so "the active pane" here IS the pane the user clicked.
    void openContextMenu();

    /// The context menu, as QML consumes it: a list of rows, each a QVariantMap of
    /// { kind, title, enabled, checkable, checked, actionId, children }.
    ///
    /// `actionId` indexes into this controller's own action list rather than naming a command, so a click
    /// runs the exact action the row was built with. Nothing has to be looked up by name at click time,
    /// and nothing depends on some other model having been populated first.
    [[nodiscard]] QVariantList contextMenuModel() const { return _contextMenuModel; }

    /// Runs the context-menu action with id @p actionId against the pane the menu was opened over.
    /// Out-of-range ids are ignored.
    /// @param actionId The row's `actionId`.
    Q_INVOKABLE void triggerContextMenuAction(int actionId);
    // }}}

    /// The titles of this window's tabs, in tab order (TabTitleProvider, for TabCommandSource).
    [[nodiscard]] std::vector<std::string> tabTitles() const override;

    // {{{ PaneProxy tree + window-service reads
    [[nodiscard]] PaneProxy* activeTabRootPane() const noexcept { return _activeTabRootProxy; }
    [[nodiscard]] TerminalSession* activeSession() const noexcept;
    [[nodiscard]] bool titleBarVisible() const noexcept;
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
    void seedTitleBarVisible(bool visible);

    /// Sets the window's title-bar visibility: stores the window-scoped state, applies the native
    /// window-frame decoration (Qt::FramelessWindowHint on the adopted OS window — shown => native
    /// frame, hidden => frameless so the custom client-side TitleBar is the only decoration; the C++
    /// counterpart of main.qml's `flags` binding), and notifies the QML window bindings.
    /// @param visible The new visibility.
    void setTitleBarVisible(bool visible);

    /// Flips the window's title-bar visibility (the ToggleTitleBar action, routed here by the display).
    void toggleTitleBar();
    // }}}

    // {{{ Tab strip (tab bar) placement + visibility
    // Like title-bar visibility, these are WINDOW state seeded once from the profile (first-write-wins),
    // so later session rebinds (tab switch, split collapse) never clobber them.

    /// The tab strip position as an int for QML (0 = Top, 1 = Bottom; see config::TabBarPosition).
    [[nodiscard]] int tabBarPosition() const noexcept;

    /// The tab strip visibility mode as an int for QML (see config::TabBarVisibility).
    [[nodiscard]] int tabBarVisibility() const noexcept;

    /// Whether the tab strip should currently be shown, resolving the visibility mode against the live
    /// tab count: Always => true, Never => false, Multiple => count() > 1. main.qml binds the tab
    /// strip's `visible` to this.
    /// @return True if the tab strip should be shown.
    [[nodiscard]] bool tabBarShouldShow() const noexcept;

    /// Seeds this window's tab strip position from the profile's tab_bar_position setting.
    /// First-write-wins (see seedTitleBarVisible).
    /// @param position The profile's tab_bar_position value.
    void seedTabBarPosition(config::TabBarPosition position);

    /// Seeds this window's tab strip visibility mode from the profile's tab_bar_visibility setting.
    /// First-write-wins (see seedTitleBarVisible).
    /// @param visibility The profile's tab_bar_visibility value.
    void seedTabBarVisibility(config::TabBarVisibility visibility);
    // }}}

    /// This window's currently focused display (for window services + status-line targeting).
    [[nodiscard]] display::TerminalDisplay* activeDisplay() const noexcept { return _activeDisplay; }

    // {{{ Window-geometry authority
    // The controller is the ONLY component that mutates QWindow geometry (WM size hints, window show
    // modes, content-driven resizes). Displays route their requests here.

    /// Adopts the QML window early (before any display focuses in), assigns the spawn target screen
    /// (the pre-show DPR predictor: spawning window's screen, else the cursor's screen on non-Wayland,
    /// else primary) and installs the DPR/screen-change hooks for the scale-settlement handler.
    /// Called from main.qml's Component.onCompleted, before the first tab exists.
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

    /// Sizes the still-unmapped window from the active pane's REAL cell metrics (FreeType math run
    /// headlessly during session attach) plus the declared chrome, applies the WM size hints, then maps
    /// the window directly in the profile's state (normal/maximized/fullscreen) — the first map is the
    /// final geometry, eliminating the map-then-correct spawn flicker. Falls back to showing at the
    /// default size (never leaves the user windowless) when no metrics are available.
    /// Called from main.qml's Component.onCompleted, after the first tab was created.
    Q_INVOKABLE void showInitial();

    /// Window chrome height in logical pixels OUTSIDE the terminal content area, DECLARED by main.qml
    /// (bound to the title bar's effective height: 0 when hidden). Geometry math must never measure
    /// window-minus-item deltas — those are transient during relayout and structurally wrong in splits.
    [[nodiscard]] int chromeHeight() const noexcept { return _chromeHeight; }
    /// Sets the declared chrome height (QML binding write) and refreshes the WM size hints.
    void setChromeHeight(int height);

    /// How much of the WM size-hint set @ref updateSizeHintsFor may (re)write.
    ///
    /// The character-cell resize grid (base + increment, the X11 @c PResizeInc pair) is deliberately
    /// CLEARED while the window is maximized/fullscreen (see @c showWithoutSizeIncrements) so the WM
    /// fills the screen exactly rather than snapping to the nearest cell multiple. An INCIDENTAL hint
    /// refresh (a split's font reconcile, a DPR settle, a title-bar toggle) must therefore not re-arm
    /// the increment while the window is non-normal — on WMs that honor @c PResizeInc that re-writes a
    /// sub-cell gap around the maximized window, and can even drop the maximized state. The @c minimum
    /// hint is always safe (it never disturbs the maximized state) and is written unconditionally.
    enum class HintApplyMode : uint8_t
    {
        /// Incidental refresh: write the resize grid only while the window is @c Windowed; write
        /// @c minimum always. Used by font/DPI/chrome refreshes that may fire while maximized.
        RespectWindowState,
        /// Establishing the normal-state hints: write the full set unconditionally. Used by the
        /// restore-into-normal paths, which call this BEFORE @c showNormal() settles @c visibility()
        /// (so a live @c visibility() read would still see the old maximized value); they KNOW the
        /// window is becoming normal, so intent — not the not-yet-settled state — drives the write.
        Full,
    };

    /// Recomputes and applies the WM size hints (minimum/base/increment) for this window from
    /// @p requester's cell geometry, profile margins, content scale and the declared chrome.
    /// Refresh triggers: font/DPI reconfiguration (via the display), chrome changes, and
    /// restore-from-fullscreen/maximize. NEVER called from a resize path.
    /// @param requester The display whose cell geometry defines the hints (the active pane).
    /// @param mode Whether to gate the resize grid on the live window state (@ref HintApplyMode).
    void updateSizeHintsFor(display::TerminalDisplay& requester,
                            HintApplyMode mode = HintApplyMode::RespectWindowState);

    /// Shows this window fullscreen (size increments cleared while non-normal).
    /// @param requester The display forwarding the request (resolves the OS window).
    void setWindowFullScreen(display::TerminalDisplay& requester);
    /// Shows this window maximized (size increments cleared while non-normal).
    /// @param requester The display forwarding the request (resolves the OS window).
    void setWindowMaximized(display::TerminalDisplay& requester);
    /// Restores this window to normal state and re-applies the WM size hints.
    /// @param requester The display forwarding the request (resolves the OS window).
    void setWindowNormal(display::TerminalDisplay& requester);
    /// Toggles fullscreen, restoring the previous maximized state on exit.
    /// @param requester The display forwarding the request (resolves the OS window).
    void toggleFullScreen(display::TerminalDisplay& requester);

    /// THE grid->window choke point: every programmatic window resize (DECSLPP / CSI 8 t, font zoom's
    /// grid restore, profile switch, DPR settlement) lands here; nothing else may resize the window.
    /// Computes the window size so that @p requester's pane receives @p totalPageSize — for a split
    /// window by solving the pane-tree ratios (vtmux::contentSizeForLeaf), identity for a single pane —
    /// plus the declared chrome, and issues ONE resize. The resulting WM resize event drives the grid
    /// through the normal window->grid path; the WM is free to refuse (the reflowed grid then stands).
    /// Refused (logged) when fullscreen/maximized or the requester has no window/session/pane.
    /// @param requester     The display whose pane the grid request targets.
    /// @param totalPageSize The requested total page (main page + status line).
    /// @return True if a window resize was issued.
    bool resizeWindowForPage(display::TerminalDisplay& requester, vtbackend::PageSize totalPageSize);

    /// Pixel-flavored choke-point entry (CSI 4 t): requests the content area of @p requester's pane in
    /// device pixels; otherwise identical to resizeWindowForPage().
    /// @param requester       The display whose pane the request targets.
    /// @param contentDevicePx The requested pane content extent in device pixels.
    /// @return True if a window resize was issued.
    bool resizeWindowForContentPixels(display::TerminalDisplay& requester,
                                      vtbackend::ImageSize contentDevicePx);
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
    [[nodiscard]] PaneProxy* getProxy(vtmux::PaneId id);

    // {{{ Called by the manager's ModelEvents router (dispatched here by WindowId)
    void onTabAboutToBeAdded(int index);
    void onTabAdded(int index);
    void onTabAboutToBeRemoved(int index);
    void onTabClosed();
    void onTabAboutToBeMoved(int fromIndex, int toIndex);
    void onTabMoved();
    void onActiveTabChanged();
    void notifyTabRowChanged(vtmux::TabId tab, QList<int> const& roles);
    void refreshAllTabTitles();
    void refreshActiveTabHighlight();
    void rebuildActiveTabPaneProxies();
    /// Re-points activeTabRootPane at the active tab's layout root (vtmux::Tab::layoutRoot), which zoom
    /// moves without reshaping the tree. O(1): the proxies stay valid, so no walk or rebuild is needed.
    void refreshActiveTabLayoutRoot();
    /// Notifies every proxy's active state changed (active-pane focus moved, no tree rebuild).
    void notifyActivePaneChanged();
    /// Notifies the split node proxy @p splitNode that its ratio changed.
    void notifyRatioChanged(vtmux::PaneId splitNode);
    /// Emits activeSessionChanged (the focused pane's session may have changed).
    void emitActiveSessionChanged() { emit activeSessionChanged(); }
    /// Rebuilds and republishes this window's status line (tab list + per-pane marker).
    void updateStatusLine();
    // }}}

  signals:
    void countChanged();
    void activeTabIndexChanged();
    /// Requests that the tab at @p index open its inline title editor. Per-window (this controller
    /// is the tab-strip model), so it only reaches this window's TabItems; the delegate whose row
    /// matches @p index starts editing.
    void tabTitleEditRequested(int index);
    /// Requests that the tab at @p index open its color picker. Per-window, exactly like
    /// tabTitleEditRequested: the delegate whose row matches @p index opens its TabColorFlyout.
    void tabColorPickRequested(int index);
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
    void multimediaReadyChanged();
    void activeTabRootPaneChanged();
    void titleBarVisibleChanged();
    void tabBarPositionChanged();
    void tabBarVisibilityChanged();
    void tabBarShouldShowChanged();
    void activeSessionChanged();
    void chromeHeightChanged();
    /// The content area switched between the settings page and the terminal (settingsActive changed).
    void settingsActiveChanged();

  private:
    /// Sets whether the settings page is shown, emitting settingsActiveChanged only on a real change.
    /// Centralizes the notify so every entry point (openSettings/closeSettings/toggleSettings and the
    /// tab-activation reset) stays consistent.
    /// @param active The new settings-page visibility.
    void setSettingsActive(bool active);

    /// The backing vtmux::Window, or nullptr if it has been removed.
    [[nodiscard]] vtmux::Window* window() const noexcept;
    /// The active tab of this window's vtmux::Window, or nullptr.
    [[nodiscard]] vtmux::Tab* activeModelTab() const noexcept;
    /// The model tab backing list-model row @p index, or nullptr.
    [[nodiscard]] vtmux::Tab* tabAtRow(int index) const noexcept;
    /// The row index of @p tab in this window, or -1.
    [[nodiscard]] int rowOfTab(vtmux::TabId tab) const noexcept;
    /// Resolves the label shown on @p row's tab.
    [[nodiscard]] QString resolvedTabLabel(vtmux::Tab* tab, TerminalSession* session, int row) const;

    /// Resolves the OS window to operate on for a geometry request from @p requester, adopting the
    /// requester's QQuickWindow as _osWindow when none is recorded yet (same rule as focusDisplay).
    /// @return The window to operate on, or nullptr when none exists.
    [[nodiscard]] QQuickWindow* osWindowFor(display::TerminalDisplay& requester) noexcept;

    /// The declared chrome as the geometry module's type (width is structurally 0 in this layout).
    [[nodiscard]] geometry::Chrome chrome() const noexcept { return { .width = 0, .height = _chromeHeight }; }

    /// Shared tail of the choke-point entries: guards the window state, solves the pane tree for
    /// @p requester's leaf, adds the declared chrome and issues the single window resize.
    /// @param requester          The display whose pane the request targets.
    /// @param leafContentLogical The pane's required content extent in logical pixels.
    /// @return True if a window resize was issued.
    bool applyContentDrivenResize(display::TerminalDisplay& requester,
                                  geometry::LogicalSize leafContentLogical);

    /// Scale-settlement handler (DevicePixelRatioChange / screenChanged): when the window's resolved
    /// content scale actually changed, re-derives the font DPI (grid reflows in place) and issues ONE
    /// grid-preserving corrective window resize (user decision: monitor moves and late Wayland
    /// fractional-scale arrival keep the grid, not the pixel size). Idempotent per scale value.
    void onWindowScaleMaybeChanged();

    /// Watches the bound window for QEvent::DevicePixelRatioChange (no QWindow signal exists for it).
    bool eventFilter(QObject* watched, QEvent* event) override;

    TerminalSessionManager& _manager;
    vtmux::WindowId _windowId;

    // {{{ Command palette
    // The sources this window's palette draws from, held by value and in PRECEDENCE order (see
    // collectCommands(): first source wins a duplicate id). The live-state sources come first so their
    // richer rows beat the generic ones — "Switch To Tab 2: vim" from the tab source outranks the bare
    // "Switch To Tab 2" a key binding would contribute. The action catalog comes last: it is the floor
    // that guarantees an unbound action is still reachable.
    TabCommandSource _tabCommands;
    ProfileCommandSource _profileCommands;
    LayoutCommandSource _layoutCommands;
    BoundCommandSource _boundCommands;
    ActionCommandSource _actionCommands;
    std::unique_ptr<CommandPaletteModel> _commandPalette;
    // }}}

    // {{{ Settings page
    /// Whether the content area currently shows the settings page instead of the terminal pane tree.
    bool _settingsActive = false;
    /// The editable settings bridge for this window (created in the constructor; never null).
    std::unique_ptr<SettingsController> _settingsController;
    // }}}

    // {{{ Terminal context menu
    /// The menu as QML sees it, rebuilt on every right-click.
    QVariantList _contextMenuModel;

    /// The actions those rows run, in the order the rows carry as `actionId`. Held here rather than in
    /// the QML so a row runs the exact action it was built with — no lookup by name at click time.
    std::vector<actions::Action> _contextMenuActions;

    /// The session the menu was BUILT over — the pane the user right-clicked.
    ///
    /// Pinned, not looked up again when a row is picked. The active pane can change while the menu stands
    /// open (the clicked pane's shell exits, the model activates a sibling in its place), and "Close Pane"
    /// or "Paste" would then land on a terminal the user never pointed at. A QPointer, so a pane that dies
    /// under the open menu makes the row a no-op rather than a use-after-free.
    QPointer<TerminalSession> _contextMenuSession;
    // }}}

    display::TerminalDisplay* _activeDisplay = nullptr;
    // The OS window (QQuickWindow) this controller adapts, adopted from the first display that focuses in.
    // Lets the manager route a focus/close to the controller that owns a given display's window.
    QQuickWindow* _osWindow = nullptr;

    // Guards closeWindow() against re-entry: closeWindow() closes _osWindow, whose QML `onClosing`
    // calls closeWindow() again — the flag makes that second call a no-op so teardown runs exactly once,
    // whichever door (user close button vs. programmatic close of an emptied window) started it.
    bool _closing = false;

    // PaneProxy tree for this window's active tab. Owned here, reused by PaneId across rebuilds so a
    // surviving pane's QML item / display is not torn down when a sibling splits or closes.
    std::unordered_map<uint64_t, PaneProxy*> _paneProxies;
    PaneProxy* _activeTabRootProxy = nullptr;

    // Carries beginMoveRows()'s result from onTabAboutToBeMoved() to onTabMoved().
    bool _tabMoveInProgress = false;

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

    // Declared title-bar chrome height in logical px (written by main.qml's binding).
    int _chromeHeight = 0;
    // Whether the window was maximized before entering fullscreen (restored on toggle-out).
    bool _maximizedState = false;
    // The content scale the geometry was last derived for (settlement idempotence latch).
    double _lastAppliedScale = 0.0;
    // True while our own content-driven resize is in flight (straddle guard, see onWindowScaleMaybeChanged).
    bool _inContentDrivenResize = false;
};

} // namespace contour
