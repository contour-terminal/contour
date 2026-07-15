#pragma once

#include <contour/CommandHistory.h>
#include <contour/CommandHistoryStore.h>
#include <contour/LayoutStore.h>
#include <contour/SessionFactory.h>
#include <contour/TerminalSession.h>
#include <contour/WindowController.h>
#include <contour/display/TerminalDisplay.h>
#include <contour/helper.h>

#include <QtCore/QObject>
#include <QtGui/QColor>
#include <QtQml/QQmlEngine>

#include <algorithm>
#include <expected>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <ranges>
#include <span>
#include <unordered_map>
#include <vector>

#include <vtmux/ModelEvents.h>
#include <vtmux/Pane.h>
#include <vtmux/PaneLayout.h>
#include <vtmux/Primitives.h>
#include <vtmux/SessionModel.h>
#include <vtmux/Tab.h>

class QScreen;

namespace contour
{

class PaneProxy;
class WindowController;

/**
 * Session-lifetime service, SessionModel host, and per-window ModelEvents router.
 *
 * The manager owns the single Qt-free vtmux::SessionModel (the authoritative window/tab/pane tree,
 * including per-tab title and color, so a future daemon and its network clients share the same
 * state) and the SessionId <-> TerminalSession* registry, and it creates/tears down the backing
 * TerminalSessions. It is NOT the GUI adapter: each OS window has its own WindowController (a
 * QAbstractListModel + PaneProxy tree + window services) that the QML binds to. The manager mints
 * those controllers (createWindowController) and, as the vtmux::ModelEvents implementer, ROUTES each
 * model change to the owning window's controller (tab events by WindowId; pane events resolve
 * TabId -> WindowId). Session -> display ownership lives solely on the pane tree (the QML `session:`
 * binding -> TerminalDisplay::setSession); the manager holds no per-display session map.
 *
 * It is NOT a list model: the GUI tab strip renders from the per-window WindowController's
 * QAbstractListModel (WindowController::Roles). Every tab/pane operation here is routed by an
 * explicit window identity — either the calling controller's vtmux::WindowId or the acting
 * session's hosting tab — never by an implicit "the" window.
 */
class TerminalSessionManager: public QObject, public vtmux::ModelEvents
{
    Q_OBJECT
    Q_PROPERTY(bool multimediaReady READ isMultimediaReady NOTIFY multimediaReadyChanged)
    Q_PROPERTY(int splitHandleThickness READ splitHandleThickness CONSTANT)
    QML_ELEMENT

  public:
    /// @param app     The owning application (spawn context, app-wide services).
    /// @param factory The PTY factory backing new sessions (injected: production wires
    ///                AppSessionFactory, tests an in-memory MockPty factory). Must outlive this
    ///                manager (both live on ContourGuiApp, factory declared first).
    /// @param layouts Persistence for named layouts, for SaveLayout (injected: production wires
    ///                FileLayoutStore, tests an in-memory store). Must outlive this manager.
    /// @param commands Persistence for the command palette's most-recently-used list (injected:
    ///                production wires FileCommandHistoryStore, tests an in-memory store). Must
    ///                outlive this manager.
    TerminalSessionManager(ContourGuiApp& app,
                           SessionFactory& factory,
                           LayoutStore& layouts,
                           CommandHistoryStore& commands);

    /// The owning application (spawn context, app-wide services). For the window controllers.
    [[nodiscard]] ContourGuiApp& app() noexcept { return _app; }

    /// Thickness of the split divider handle in logical pixels, single-sourced from
    /// vtmux::DefaultSplitHandleThickness. PaneNode.qml's explicit SplitView `handle:` binds its
    /// implicit size to this property so the rendered handle and the pane-layout solver (which
    /// passes the same constant to vtmux::contentSizeForLeaf) cannot diverge.
    [[nodiscard]] constexpr int splitHandleThickness() const noexcept
    {
        return vtmux::DefaultSplitHandleThickness;
    }

    /// Creates a backing session + its model tab in @p window (the calling controller's window).
    contour::TerminalSession* createSessionInBackground(vtmux::WindowId window);

    /// Creates a new tab in @p window (the GUI "+" button entry point, via WindowController).
    contour::TerminalSession* createSession(vtmux::WindowId window);

    /// Creates and activates a new tab in @p window.
    void createNewTab(vtmux::WindowId window) { createSession(window); }

    /// Creates and activates a new tab in the window hosting @p acting (the CreateNewTab keybinding).
    void createNewTab(TerminalSession* acting);

    /// Looks up @p name in the app's configured layouts and appends its tabs to the window hosting
    /// @p acting (the LaunchLayout action). Logs and no-ops if the layout is unknown or @p acting has
    /// no hosting window.
    /// @param name   The layout's key in config::Config::layouts.
    /// @param acting The session that triggered the action; its hosting window is the target.
    void launchLayout(std::string const& name, TerminalSession* acting);

    /// Saves the window hosting @p acting as a named layout, persisted to layouts.yml (the SaveLayout
    /// action). Thin wrapper around saveWindowLayout(); no-ops if @p acting has no hosting window.
    /// @param name   The key to save the layout under in config::Config::layouts.
    /// @param acting The session that triggered the action; its hosting window is serialized.
    void saveLayout(std::string const& name, TerminalSession* acting);

    /// Opens the save-as name prompt over the window hosting @p acting (a nameless `SaveLayout` action),
    /// routed exactly like beginTabColorPick(). The window then calls back into saveLayout() with the
    /// name the user typed. No-ops if @p acting has no hosting window.
    /// @param acting The session that triggered the action; its hosting window shows the prompt.
    void beginSaveLayoutPrompt(TerminalSession* acting);

    /// Serializes @p window's live tab/pane tree into a config::Layout, persists it through the
    /// injected LayoutStore, and — only once the store has accepted it, so runtime state can never
    /// claim more than what is actually saved — stores it under @p name in the app's in-memory config.
    /// @param window The window whose tabs/panes to serialize.
    /// @param name   The key to save the layout under.
    /// @return Nothing on success, or the reason the layout could not be saved.
    [[nodiscard]] std::expected<void, LayoutSaveError> saveWindowLayout(vtmux::WindowId window,
                                                                        std::string const& name);

    /// Where layouts are persisted: the `layouts.yml` sibling of the loaded config file — which is
    /// exactly where loadConfigFromFile() merges it back from, so saving and loading can never
    /// disagree (a custom `--config` path moves both together).
    /// @return The layout store's path for this run's configuration.
    [[nodiscard]] std::filesystem::path layoutsFilePath() const;

    /// Opens the command palette over the window hosting @p acting (the OpenCommandPalette action).
    /// No-ops if @p acting has no hosting window.
    /// @param acting The session that triggered the action; its hosting window shows the palette.
    void openCommandPalette(TerminalSession* acting);

    /// Opens the terminal context menu over the pane of @p acting (the OpenContextMenu action).
    ///
    /// Makes that pane active first, so the menu is built from the state of the pane the user actually
    /// right-clicked and every row it offers runs against that same pane — not against whichever pane
    /// happened to be active beforehand. No-ops if @p acting has no hosting window.
    ///
    /// @param acting The session that was right-clicked.
    void openContextMenu(TerminalSession* acting);

    /// Records that the command @p id was just run, and persists the updated list.
    ///
    /// App-wide, not per-window: "recently used" is a fact about the USER, so a command run in one
    /// window is recent in all of them.
    ///
    /// Persisted on every run rather than at exit, so the list survives a crash or a kill -9 — the
    /// file is a few hundred bytes and the write is atomic (see FileCommandHistoryStore).
    ///
    /// @param id The command id that was run (see commandId()).
    void recordCommand(std::string const& id);

    /// The app-wide most-recently-used command list, for the palette models to read.
    [[nodiscard]] CommandHistory const& commandHistory() const noexcept { return _commandHistory; }

    /// Brings the history into a usable state: re-applies the configured
    /// `command_palette_recent_count` (so editing it and reloading the config takes effect without a
    /// restart), and seeds the list from the store on first use.
    ///
    /// Idempotent, and called from BOTH entry points (openCommandPalette and recordCommand) rather than
    /// only the first — see recordCommand() for why relying on the ordering would fail silently.
    void ensureCommandHistoryReady();

    /// Where the command palette's MRU list is persisted: the `command-history.yml` sibling of the
    /// loaded config file, sited exactly like layoutsFilePath() so a custom `--config` moves it too.
    /// @return The command-history store's path for this run's configuration.
    [[nodiscard]] std::filesystem::path commandHistoryFilePath() const;

    /// The path of @p fileName as a sibling of the loaded config file (the config home when no file was
    /// loaded). The one place the machine-written stores' siting rule lives.
    /// @param fileName The file's name, e.g. "layouts.yml".
    [[nodiscard]] std::filesystem::path configSiblingPath(std::string_view fileName) const;

    /// Appends every tab of @p layout to @p window, building a real PTY-backed session for each leaf
    /// pane (via createBackingSession) before handing the pane tree to realizeLayoutTab. Used by both
    /// launchLayout and startup (--layout / default_layout).
    /// @param window   The target window.
    /// @param layout   The layout to realize (must have at least one tab).
    /// @param pageSize The total page size each pane's grid and child PTY are born at. Pass the live
    ///                 window's running size (LaunchLayout into an existing window) so commands that
    ///                 read the terminal size at startup see the real one; @c std::nullopt (a
    ///                 brand-new window at startup) uses the profile's configured terminalSize.
    /// @return false if @p layout has no tabs (nothing to apply); true otherwise.
    bool applyLayoutToWindow(vtmux::WindowId window,
                             config::Layout const& layout,
                             std::optional<vtbackend::PageSize> pageSize = std::nullopt);

    // Keyboard tab navigation/reordering. Every entry point takes the ACTING session (the one that
    // received the keybinding) and targets that session's hosting window, so a keybinding in any
    // OS window operates on that window's tabs.
    void switchToPreviousTab(TerminalSession* acting);
    void switchToTabLeft(TerminalSession* acting);
    void switchToTabRight(TerminalSession* acting);
    void switchToTab(int position, TerminalSession* acting);
    void closeTab(TerminalSession* acting);
    void moveTabTo(int position, TerminalSession* acting);
    void moveTabToLeft(TerminalSession* session);
    void moveTabToRight(TerminalSession* session);
    /// Opens the inline title editor for the active tab of the window hosting @p acting (the
    /// SetTabTitle keybinding), routed like the other tab ops via the acting session's window.
    /// @param acting The session that received the keybinding; its hosting window is the target.
    void beginTabTitleEdit(TerminalSession* acting);
    /// Opens the color picker for the active tab of the window hosting @p acting (a `SetTabColor`
    /// keybinding that names no color), routed exactly like beginTabTitleEdit().
    /// @param acting The session that received the keybinding; its hosting window is the target.
    void beginTabColorPick(TerminalSession* acting);
    /// Colors the active tab of the window hosting @p acting (a `SetTabColor` keybinding that names a
    /// color) as the user's own choice, so it outranks an application's DECAC color.
    /// @param color The color to apply.
    /// @param acting The session that received the keybinding; its hosting window is the target.
    void setActiveTabColor(vtbackend::RGBColor color, TerminalSession* acting);
    /// Drops the user's color from the active tab of the window hosting @p acting (the `ResetTabColor`
    /// keybinding), letting an application-assigned DECAC color resurface.
    /// @param acting The session that received the keybinding; its hosting window is the target.
    void resetActiveTabColor(TerminalSession* acting);

    void removeSession(TerminalSession&);
    void currentSessionIsTerminated();

    // {{{ Tab-strip operations (window-routed; called by WindowController with its own WindowId)
    void activateTab(vtmux::WindowId window, int index);
    void moveTab(vtmux::WindowId window, int fromIndex, int toIndex);
    /// Moves the tab at row @p fromIndex of window @p from into window @p to at row @p toIndex,
    /// transplanting it intact (its sessions survive). Drives the drag of a tab between windows.
    /// @param from      The source window's id.
    /// @param fromIndex The tab's row in @p from.
    /// @param to        The destination window's id.
    /// @param toIndex   The destination row.
    void moveTabToWindow(vtmux::WindowId from, int fromIndex, vtmux::WindowId to, int toIndex);
    /// Tears the tab at row @p fromIndex of window @p from into a brand-new OS window. The new window
    /// is spawned (on @p targetScreen if given) and adopts the tab as its sole tab; if that empties the
    /// source window, it is closed. Drives dropping a dragged tab onto empty desktop.
    /// @param from         The source window's id.
    /// @param fromIndex    The tab's row in @p from.
    /// @param targetScreen The screen the new window should open on (may be nullptr).
    void tearOffTabToNewWindow(vtmux::WindowId from, int fromIndex, QScreen* targetScreen);
    void closeTabAtIndex(vtmux::WindowId window, int index);
    void closeOtherTabs(vtmux::WindowId window, int index);
    void closeTabsToRight(vtmux::WindowId window, int index);

    /// The predefined tab-color palette (a grid of swatches) the user picks from, as QColors.
    [[nodiscard]] QVariantList tabColorPalette() const;
    // }}}

    // {{{ Split-pane operations (drive the vtmux model; Phase 2)
    /// Splits the acting session's tab's active pane along @p vertical (true: side-by-side;
    /// false: stacked), creating a backing TerminalSession for the new leaf.
    /// @param vertical Split orientation.
    /// @param acting The session that received the keybinding; its hosting tab is the target.
    ///        Null or unknown to the model is a no-op.
    void splitActivePane(bool vertical, TerminalSession* acting);
    /// Closes the active pane of the acting session's tab (closing the tab if it was the last pane).
    /// @param acting The session that received the keybinding; its hosting tab is the target.
    void closeActivePane(TerminalSession* acting);
    /// Moves pane focus within the acting session's tab in the given direction.
    /// @param direction The direction to move pane focus.
    /// @param acting The session that received the keybinding; its hosting tab is the target.
    void focusPane(vtmux::FocusDirection direction, TerminalSession* acting);
    /// Swaps the acting session's active pane with its neighbor in @p direction (see
    /// vtmux::SessionModel::swapActivePane).
    /// @param direction The direction of the neighbor to swap with.
    /// @param acting The session that received the keybinding; its hosting tab is the target.
    void swapPane(vtmux::FocusDirection direction, TerminalSession* acting);
    /// Moves (re-parents) the acting session's active pane across its neighbor in @p direction (see
    /// vtmux::SessionModel::moveActivePane).
    /// @param direction The direction to move the active pane.
    /// @param acting The session that received the keybinding; its hosting tab is the target.
    void movePane(vtmux::FocusDirection direction, TerminalSession* acting);
    /// Flips the orientation of the acting session's active pane's split.
    /// @param acting The session that received the keybinding; its hosting tab is the target.
    void toggleActivePaneOrientation(TerminalSession* acting);
    /// Grows or shrinks the acting session's active pane in @p direction by @p fraction.
    /// @param direction The side the active pane grows toward.
    /// @param fraction The ratio delta magnitude in (0, 1).
    /// @param acting The session that received the keybinding; its hosting tab is the target.
    void resizeActivePane(vtmux::FocusDirection direction, double fraction, TerminalSession* acting);
    /// Toggles zoom on the acting session's active pane (see vtmux::SessionModel::toggleActivePaneZoom).
    /// @param acting The session that received the keybinding; its hosting tab is the target.
    void toggleActivePaneZoom(TerminalSession* acting);
    // }}}

    // {{{ Model service used by PaneProxy + WindowController
    /// The TerminalSession backing @p id, or nullptr. (Public for PaneProxy.)
    [[nodiscard]] TerminalSession* sessionForId(vtmux::SessionId id) const noexcept;
    /// Whether @p id is the active leaf of tab @p tab.
    [[nodiscard]] bool isActivePane(vtmux::TabId tab, vtmux::PaneId id) const noexcept;
    /// Sets the split ratio of node @p id in tab @p tab.
    void setPaneRatio(vtmux::TabId tab, vtmux::PaneId id, double ratio);
    /// Makes leaf @p id the active pane of tab @p tab.
    void activatePane(vtmux::TabId tab, vtmux::PaneId id);
    // }}}

    void updateColorPreference(vtbackend::ColorPreference const& preference);

    void FocusOnDisplay(display::TerminalDisplay* display);

    /// Single authority for terminal (VT) focus: sends a focus-OUT to the previously focused session
    /// and a focus-IN to @p next (nullptr = nothing focused), so exactly one session is ever focused.
    /// Every focus source (Qt focus events, model tab/pane changes, cross-window moves) routes here.
    /// @param next The session that should now hold terminal focus, or nullptr for none.
    void setFocusedSession(TerminalSession* next);

    /// Clears terminal focus iff @p session currently holds it (a display losing Qt focus). A no-op
    /// when @p session is not the focused one, so an outgoing pane's focus-out cannot cancel the
    /// focus-in the model already delivered to the incoming pane.
    /// @param session The session whose display just lost Qt focus.
    void clearFocusIfCurrent(TerminalSession* session);

    void update() { updateStatusLine(); }

    /// Invalidates TitleRole on every tab row of EVERY window's tab strip (each WindowController
    /// re-emits for its own rows). Needed when a change can shift more than one label at once: a
    /// structural reorder/close/insert renumbers the {TabPosition} of later tabs, and a config
    /// reload may change the tab-label template.
    void refreshAllTabTitles();

    /// Invalidates TitleRole for the tab hosting @p session ON ITS OWNING WINDOW's tab strip, so
    /// the strip recomputes that tab's label after a runtime window-title / tab-name change. The
    /// label is derived from the tab's active leaf, so this is correct even when @p session is a
    /// background pane (the active-leaf label is recomputed; if @p session is not the active leaf
    /// the label simply stays the same). No-op if no tab hosts @p session. MUST be called on the
    /// GUI thread (it emits dataChanged on the owning controller).
    /// @param session The session whose hosting tab should refresh its label.
    void refreshTabForSession(vtmux::SessionId session);

    /// Assigns @p color to the tab hosting @p session (DECAC item 2 "window frame"), routing through
    /// the authoritative SessionModel so the existing tab-color pipeline repaints the tab strip. The
    /// color is recorded under vtmux::TabColorSource::Application, so it stays hidden behind a color the
    /// user picked themselves and surfaces once the user clears theirs. No-op if no tab hosts
    /// @p session. MUST be called on the GUI thread (it mutates the GUI-facing model).
    /// @param session The session (by model id) whose hosting tab should be colored.
    /// @param color The color to assign.
    void setTabColorForSession(vtmux::SessionId session, vtbackend::RGBColor color);

    /// Resets the tab hosting @p session back to no application-assigned color (DECAC item 2 with no
    /// colors, or a hard reset). A color the user picked is left alone. No-op if no tab hosts
    /// @p session. MUST be called on the GUI thread.
    /// @param session The session (by model id) whose hosting tab color should be cleared.
    void resetTabColorForSession(vtmux::SessionId session);

    /// Clears the destroyed @p display from any controller that held it as its focused display, and from
    /// the manager's _activeDisplay. Called when the display's QML item / pane is torn down. Session->
    /// display ownership lives on the pane tree, so there is no per-display session map to scrub.
    /// @param display The display being destroyed.
    void detachDisplay(display::TerminalDisplay* display) noexcept;

    // {{{ Per-OS-window controllers (Stage 2 of the per-window refactor)
    /// Mints a fresh vtmux::Window, creates its per-OS-window WindowController and registers it. The
    /// controller is the QML-facing adapter for one ApplicationWindow: it owns that window's tab-strip
    /// list-model, PaneProxy tree and window services, and delegates structural / session-lifetime
    /// operations back to this manager tagged with its WindowId. Owned via QQmlEngine CppOwnership
    /// (like sessions and proxies). main.qml calls this from Component.onCompleted before creating its
    /// first tab.
    Q_INVOKABLE contour::WindowController* createWindowController();

    /// If a tab tear-off is pending (staged by tearOffTabToNewWindow before this window was spawned),
    /// transplants that tab into @p newController's window and returns true; the window must NOT then
    /// create its own first tab. Returns false when there is no pending transplant, in which case
    /// main.qml creates the usual fresh first tab. Consumes the staged request (fires at most once).
    /// @param newController The freshly-created controller of the just-spawned window.
    /// @return true if a torn-off tab was adopted; false if the window should create its own first tab.
    Q_INVOKABLE bool consumePendingTransplant(contour::WindowController* newController);

    /// Looks up the app's startup layout (ContourGuiApp::layoutName(), from `--layout` or the config's
    /// `default_layout`) and, if found, applies it to @p controller's window. Called by main.qml right
    /// after consumePendingTransplant() when no tab was transplanted, so a freshly-spawned window can
    /// open pre-populated instead of falling back to a single blank tab. One-shot: only the FIRST
    /// window of the process consumes the startup layout — every later window (NewTerminalWindow)
    /// gets its usual single default tab instead of re-running all of the layout's commands.
    /// @param controller The freshly-created controller of the just-spawned window.
    /// @return true if a startup layout was found and applied; false if there is none configured, it is
    ///         unknown, it has no tabs, or it was already consumed by an earlier window, in which case
    ///         the window should create its usual first tab.
    Q_INVOKABLE bool consumeDefaultLayout(contour::WindowController* controller);

    /// The controller adapting @p window, or nullptr. Used by the ModelEvents router to forward each Qt
    /// row/signal emission to the owning window's controller.
    [[nodiscard]] WindowController* controllerFor(vtmux::WindowId window) const noexcept;

    /// The controller owning @p display's OS window, for routing focus. Matches the controller that has
    /// adopted this display's QQuickWindow (ownsOSWindow); falls back to the sole controller before any
    /// window has recorded its QQuickWindow (first focus-in). Null if there are no controllers.
    /// @param display The display that just focused in (may be null).
    /// @return The owning controller, or nullptr.
    [[nodiscard]] WindowController* controllerForDisplay(display::TerminalDisplay* display) const noexcept;

    /// Drops the controller for a closed OS window (WindowController::closeWindow calls this last). "Last
    /// window" is _controllersByWindow.size() == 1; when it is, the manager clears residual session
    /// registries. deleteLater()s the controller.
    /// @param windowId The window whose controller is being removed.
    void removeWindowController(vtmux::WindowId windowId);

    // Narrow SERVICE interface the WindowController reads through (the manager stays the SessionModel host
    // + SessionId<->TerminalSession registry). These expose the shared model/registry without moving
    // session lifetime out of the manager.
    [[nodiscard]] vtmux::SessionModel& model() const noexcept { return *_model; }
    /// The backing sessions of every leaf pane in @p tab (public for WindowController::closeTab paths).
    [[nodiscard]] std::vector<TerminalSession*> sessionsOfTab(vtmux::Tab* tab) const
    {
        return sessionsInTab(tab);
    }
    /// Terminates each of @p sessions (public whole-tab close primitive for WindowController).
    void terminate(std::span<TerminalSession* const> sessions) { terminateSessions(sessions); }
    // }}}

    /// Returns whether the Qt multimedia backend has been initialized.
    [[nodiscard]] bool isMultimediaReady() const noexcept { return _multimediaReady; }

    /// Sets the multimedia readiness flag and emits multimediaReadyChanged().
    void setMultimediaReady(bool ready)
    {
        if (_multimediaReady != ready)
        {
            _multimediaReady = ready;
            emit multimediaReadyChanged();
        }
    }

    // {{{ vtmux::ModelEvents — turn model changes into Qt model/signal notifications
    void tabAboutToBeAdded(vtmux::WindowId window, int index) override;
    void tabAdded(vtmux::WindowId window, vtmux::TabId tab, int index) override;
    void tabAboutToBeRemoved(vtmux::WindowId window, int index) override;
    void tabClosed(vtmux::WindowId window, vtmux::TabId tab, int index) override;
    void tabAboutToBeMoved(vtmux::WindowId window, int fromIndex, int toIndex) override;
    void tabMoved(vtmux::WindowId window, vtmux::TabId tab, int fromIndex, int toIndex) override;
    void tabAboutToBeMovedToWindow(vtmux::WindowId from,
                                   int fromIndex,
                                   vtmux::WindowId to,
                                   int toIndex) override;
    void tabMovedToWindow(
        vtmux::WindowId from, vtmux::TabId tab, int fromIndex, vtmux::WindowId to, int toIndex) override;
    void activeTabChanged(vtmux::WindowId window, vtmux::TabId tab, int index) override;
    void paneSplit(vtmux::TabId tab, vtmux::PaneId splitNode, vtmux::PaneId newLeaf) override;
    void paneClosed(vtmux::TabId tab, vtmux::PaneId closed, vtmux::PaneId survivor) override;
    void activePaneChanged(vtmux::TabId tab, vtmux::PaneId leaf) override;
    void paneRatioChanged(vtmux::TabId tab, vtmux::PaneId splitNode, double ratio) override;
    void paneOrientationChanged(vtmux::TabId tab,
                                vtmux::PaneId splitNode,
                                vtmux::SplitState newState) override;
    void paneSwapped(vtmux::TabId tab, vtmux::PaneId a, vtmux::PaneId b) override;
    void paneZoomChanged(vtmux::TabId tab, std::optional<vtmux::PaneId> zoomedLeaf) override;
    void paneTreeRestructured(vtmux::TabId tab) override;
    void tabTitleChanged(vtmux::TabId tab) override;
    void tabColorChanged(vtmux::TabId tab) override;
    // }}}

  signals:
    void multimediaReadyChanged();

    /// A pane asked for its context menu, before the menu is routed to the window that hosts it.
    ///
    /// The routing itself no-ops without a window, so this is the one point at which "the right-click
    /// reached the OpenContextMenu action" is observable independently of there being a GUI to show it in.
    /// @param acting The session that was right-clicked.
    void contextMenuRequested(TerminalSession* acting);

  private:
    /// Closes @p window if a tab transplant just emptied it — an empty window (no tabs) is not a valid
    /// state. Called after a cross-window tab move; MUST run after the transplant so removeWindow (via
    /// closeWindow) never tears down the moved tab's now-relocated sessions.
    /// @param window The source window a tab was just moved out of.
    void closeWindowIfEmpty(vtmux::WindowId window);

    /// Re-syncs terminal focus after an active-tab/-pane change in @p controller: if it owns the
    /// focused display, moves focus to its new active-leaf session (symmetric out/in). A background
    /// window's change does not steal focus.
    /// @param controller The window whose active tab/pane just changed.
    void syncFocusForWindow(WindowController* controller);

    /// Creates a TerminalSession backing the given vtmux session id, registers it in the
    /// SessionId<->TerminalSession maps, and claims C++ ownership. Does NOT touch the model (the
    /// caller has already created the corresponding tab or split leaf).
    /// @param sessionId The pre-minted vtmux session id to back.
    /// @param cwd       Working directory the new shell inherits, if any.
    /// @param pageSize  Initial grid size for the child PTY, if the caller inherits the live window's
    ///                  page size (a new tab/split); @c std::nullopt lets the factory use the profile
    ///                  default (a brand-new window).
    /// @param commandOverride Command to launch instead of the profile's configured shell, if any.
    /// @param profileName Profile to run this session under, if any; @c std::nullopt (the default)
    ///                    selects the application's default profile.
    contour::TerminalSession* createBackingSession(
        vtmux::SessionId sessionId,
        std::optional<std::string> cwd,
        std::optional<vtbackend::PageSize> pageSize = std::nullopt,
        std::optional<vtpty::Process::ExecInfo> const& commandOverride = std::nullopt,
        std::optional<std::string> const& profileName = std::nullopt);

    /// Looks up a layout by name in the app's (inline + file) layout map, logging a miss.
    /// @param name    The layout name to find.
    /// @param context What is looking it up (e.g. "LaunchLayout"), for the log line.
    /// @return The layout, or nullptr when no layout carries that name.
    [[nodiscard]] config::Layout const* findLayout(std::string const& name, std::string_view context) const;

    /// The 0-based row of @p tab within its OWNING window, or -1. Window-agnostic: the owning
    /// window is resolved through the model (windowOfTab), so this is correct for any tab of any
    /// window.
    [[nodiscard]] int rowOfTab(vtmux::TabId tab) const noexcept;

    /// The model tab whose tree contains a leaf hosting @p session, or nullptr.
    [[nodiscard]] vtmux::Tab* findTabHostingSession(vtmux::SessionId session) const noexcept;

    /// The model tab backing tab-strip row @p index of @p window, or nullptr.
    [[nodiscard]] vtmux::Tab* tabAtRow(vtmux::WindowId window, int index) const noexcept;

    /// Collects the backing TerminalSessions of every leaf pane in @p tab (empty if @p tab is null).
    /// @param tab The tab whose pane sessions to gather.
    /// @return The backing sessions, in pane-tree order.
    [[nodiscard]] std::vector<TerminalSession*> sessionsInTab(vtmux::Tab* tab) const;

    /// Gathers the backing sessions of every tab in @p window matching @p predicate, in row order.
    /// Shared by the bulk-close operations (closeOtherTabs / closeTabsToRight), which snapshot the doomed
    /// sessions before the model mutates.
    /// @param window The window whose tabs to scan.
    /// @param predicate Selects which tabs' sessions to gather. Receives the tab's row (already known to
    ///                  the row-ordered scan) so a positional predicate need not re-derive it via a linear
    ///                  rowOfTab() lookup, and the tab itself.
    /// @return The matching tabs' backing sessions.
    [[nodiscard]] std::vector<TerminalSession*> gatherSessionsOfTabsWhere(
        vtmux::Window& window, std::function<bool(int row, vtmux::Tab*)> const& predicate) const;

    /// Terminates each of @p sessions, the single whole-tab close primitive shared by all close
    /// paths (CloseTab, the tab ✕ button, "Close Other Tabs", "Close Tabs to the Right").
    /// terminate() drives sessionClosed -> removeSession, which collapses the model to the survivor on
    /// each pane close and tears the tab down with its last pane. The caller passes a stable snapshot
    /// (e.g. from sessionsInTab) so the model mutations terminate() triggers do not invalidate the
    /// iteration. Keeping this in one place ensures the close entry points stay consistent.
    /// @param sessions The backing sessions to terminate.
    void terminateSessions(std::span<TerminalSession* const> sessions);

    /// Resolves the model tab that hosts @p session (any of its split panes), or nullptr if @p
    /// session is unknown to the registries or has no model tab.
    /// @param session The backing session whose hosting tab to locate.
    /// @return The hosting tab, or nullptr.
    [[nodiscard]] vtmux::Tab* tabHostingSession(TerminalSession* session) const noexcept
    {
        if (session == nullptr)
            return nullptr;
        return findTabHostingSession(session->modelSessionId());
    }

    /// Resolves the model window hosting @p session (through its tab), or nullptr.
    /// @param session The backing session whose hosting window to locate.
    /// @return The hosting window, or nullptr.
    [[nodiscard]] vtmux::Window* windowHostingSession(TerminalSession* session) const noexcept
    {
        auto* tab = tabHostingSession(session);
        return tab != nullptr ? _model->window(_model->windowOfTab(tab->id())) : nullptr;
    }

    /// Resolves the QML-facing controller of the window hosting @p session, or nullptr.
    ///
    /// The routing every keybinding that targets "my window's UI" needs: a session receives the chord,
    /// but the tab strip / palette / context menu it must act on belongs to the window that session
    /// happens to sit in.
    /// @param session The backing session that received the keybinding.
    /// @return The hosting window's controller, or nullptr when the session has no window (or no
    ///         controller has been registered for it).
    [[nodiscard]] WindowController* controllerHostingSession(TerminalSession* session) const noexcept
    {
        auto* win = windowHostingSession(session);
        return win != nullptr ? controllerFor(win->id()) : nullptr;
    }

    /// Shared move primitive for the public move-tab entry points: reorders the tab hosting @p session to
    /// model row @p targetRow through the authoritative model and refreshes the status line. The callers
    /// own their distinct target-row computation and bounds checks; this resolves the session to its tab
    /// then defers to moveTabByTab() for the common move / updateStatusLine mechanism.
    /// @param session   The backing session whose tab to move (nullptr or unknown is a no-op).
    /// @param targetRow The destination row in 0-based model tab-space.
    void moveTabToRow(TerminalSession* session, int targetRow);

    /// The single move mechanism: reorders @p tab to row @p targetRow of its OWNING window (resolved
    /// through the model) and refreshes the status line. Every move entry point funnels through here
    /// so the post-move refresh lives in exactly one place.
    /// @param tab       The tab to move (nullptr is a no-op).
    /// @param targetRow The destination row in 0-based model tab-space.
    void moveTabByTab(vtmux::Tab* tab, int targetRow);

    /// The tab a pane action should target: the tab hosting @p acting (the session that received
    /// the keybinding). Keyboard pane actions act on the tab the user is typing in — there is no
    /// "the active tab" fallback, since with several OS windows that would be ambiguous.
    /// @param acting The acting session (nullptr or unknown yields nullptr).
    /// @return The target tab, or nullptr.
    [[nodiscard]] vtmux::Tab* paneActionTargetTab(TerminalSession* acting) const noexcept
    {
        return tabHostingSession(acting);
    }

    /// Activates the tab at tab-strip row @p row of @p window: makes it that window's active tab
    /// (so the rendered split tree and subsequent pane operations target it). No-op if @p row is
    /// out of range.
    void activateModelTabByRow(vtmux::WindowId window, int row);

    /// Refreshes every window's indicator status line. The per-window tab list + per-pane marker fan-out
    /// lives on each WindowController (WindowController::updateStatusLine), so this simply routes to every
    /// controller — the single publish path, replacing the old _displayStates iteration. A rename/recolor
    /// in a background window reaches here (refreshGuiTabInfoForStatusLine -> update()) and refreshes that
    /// window's status line because its controller republishes to its own tabs' sessions.
    void updateStatusLine();

    ContourGuiApp& _app;
    SessionFactory& _sessionFactory;
    LayoutStore& _layoutStore;
    CommandHistoryStore& _commandHistoryStore;
    /// The app-wide most-recently-used command list. Seeded from the store on the first palette open
    /// (see openCommandPalette()); every window's palette model reads it, and recordCommand() writes it
    /// back through the store.
    CommandHistory _commandHistory;
    /// Whether _commandHistory has been seeded from the store yet. The seeding is deferred rather than
    /// done in the constructor because the configured capacity is not known that early — see there.
    bool _commandHistoryLoaded = false;
    std::chrono::seconds _earlyExitThreshold;
    // The process-wide "focused display". Session->display ownership lives on the pane tree; this is only
    // the currently-focused display, routed to its owning WindowController for window services.
    display::TerminalDisplay* _activeDisplay = nullptr;

    // The single session that currently holds terminal (VT) focus across all windows, or nullptr.
    // setFocusedSession() is the sole mutator and emits the symmetric focus-out/focus-in pair.
    TerminalSession* _focusedSession = nullptr;

    bool _multimediaReady = false;

    // {{{ vtmux model integration
    // The Qt-free layout model: the authoritative window/tab/pane tree. The model owns the
    // authoritative tab title and color; _sessionsById maps each leaf's SessionId to its backing
    // TerminalSession.
    std::unique_ptr<vtmux::SessionModel> _model;
    // Per-OS-window QML adapters, keyed by vtmux::WindowId. The ModelEvents router forwards each Qt
    // row/signal emission to the controller for the event's window. Owned via QQmlEngine CppOwnership.
    std::unordered_map<uint64_t, WindowController*> _controllersByWindow;
    // Registry mapping a vtmux session id to the Qt TerminalSession that backs it. The reverse
    // (TerminalSession -> SessionId) needs no map: each TerminalSession stores its own
    // modelSessionId().
    std::unordered_map<uint64_t, TerminalSession*> _sessionsById;
    // SessionId -> the tab hosting that session's leaf, so updateStatusLine() and
    // findTabHostingSession() resolve a session's tab in O(1) instead of walking every tab's tree.
    std::unordered_map<uint64_t, vtmux::TabId> _tabBySession;
    uint64_t _nextSessionId { 1 };
    // The id pre-minted for the next tab the model is about to create, consumed by the model's
    // SessionAllocator so a model tab and its backing TerminalSession share one id.
    std::optional<vtmux::SessionId> _pendingSessionId;
    // Whether a window already consumed the startup layout (--layout / default_layout). The app's
    // layout name is stable for the whole process, but the layout must apply to the FIRST window
    // only — not to every window spawned later (see consumeDefaultLayout()).
    bool _startupLayoutConsumed = false;

    // A tab tear-off staged by tearOffTabToNewWindow(): the (source window, tab) the next spawned
    // window should adopt as its sole tab instead of creating a fresh one. Consumed exactly once by
    // consumePendingTransplant() from that window's main.qml. Mirrors ContourGuiApp::_pendingSpawnScreen.
    std::optional<std::pair<vtmux::WindowId, vtmux::TabId>> _pendingTransplant;

    // Cached QVariantList of the (immutable) tab-color palette, built lazily on first request.
    mutable QVariantList _tabColorPaletteCache;
    // }}}
};

} // namespace contour

Q_DECLARE_INTERFACE(contour::TerminalSessionManager, "org.contour.TerminalSessionManager")
