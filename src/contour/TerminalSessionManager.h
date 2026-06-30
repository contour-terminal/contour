#pragma once

#include <contour/DisplayState.h>
#include <contour/SessionFactory.h>
#include <contour/TerminalSession.h>
#include <contour/display/TerminalDisplay.h>
#include <contour/helper.h>

#include <QtCore/QAbstractListModel>
#include <QtGui/QColor>
#include <QtQml/QQmlEngine>

#include <algorithm>
#include <functional>
#include <memory>
#include <ranges>
#include <span>
#include <unordered_map>
#include <vector>

#include <vtmux/ModelEvents.h>
#include <vtmux/Pane.h>
#include <vtmux/Primitives.h>
#include <vtmux/SessionModel.h>
#include <vtmux/Tab.h>

namespace contour
{

class PaneProxy;

/**
 * Manages terminal sessions.
 *
 * Besides the legacy display/session multiplexing it still performs, this is now also the Qt
 * adapter over the Qt-free vtmux session/layout model: it owns a vtmux::SessionModel, keeps a
 * SessionId <-> TerminalSession* registry, and implements vtmux::ModelEvents to turn model changes
 * into Qt model-reset/dataChanged + signals for the GUI tab strip. The model is the authoritative
 * source for the per-tab title and color (so a future daemon and its network clients share the same
 * state); the manager merely projects it into QML roles.
 */
class TerminalSessionManager: public QAbstractListModel, public vtmux::ModelEvents
{
    Q_OBJECT
    Q_PROPERTY(int count READ count)
    Q_PROPERTY(int activeTabIndex READ activeTabIndex NOTIFY activeTabIndexChanged)
    Q_PROPERTY(bool multimediaReady READ isMultimediaReady NOTIFY multimediaReadyChanged)
    QML_ELEMENT

  public:
    /// Roles exposed to the QML tab strip for each tab (row).
    enum Roles : int
    {
        TitleRole = Qt::UserRole + 1, //!< Resolved (expanded) tab label; what the tab strip displays.
        ColorRole,                    //!< Tab accent color as QColor (transparent if uncolored).
        IsActiveRole,                 //!< Whether this tab is the active tab of its window.
        PaneCountRole,                //!< Number of panes in this tab.
        SessionIdRole,                //!< The session id of the tab's active leaf.
        RawTitleRole,                 //!< Un-expanded runtime rename template (empty if never renamed);
                                      //!< pre-fills the inline rename editor so editing keeps the template.
    };

    TerminalSessionManager(ContourGuiApp& app);

    contour::TerminalSession* createSessionInBackground();

    Q_INVOKABLE contour::TerminalSession* createSession();

    /// Creates and activates a new tab (the GUI "+" button / CreateNewTab action entry point).
    Q_INVOKABLE void createNewTab()
    {
        allowCreation();
        createSession();
    }

    void switchToPreviousTab();
    void switchToTabLeft();
    void switchToTabRight();
    void switchToTab(int position);
    Q_INVOKABLE void closeTab();
    Q_INVOKABLE void closeWindow();
    void moveTabTo(int position);
    void moveTabToLeft(TerminalSession* session);
    void moveTabToRight(TerminalSession* session);

    void removeSession(TerminalSession&);
    void currentSessionIsTerminated();

    Q_INVOKABLE [[nodiscard]] QVariant data(const QModelIndex& index,
                                            int role = Qt::DisplayRole) const override;
    Q_INVOKABLE [[nodiscard]] int rowCount(const QModelIndex& parent = QModelIndex()) const override;
    [[nodiscard]] QHash<int, QByteArray> roleNames() const override;

    /// The number of tabs in the active window (one row per tab in the tab strip). A tab with
    /// multiple split panes is still a single row, so this is the model's tab count, not the
    /// number of backing sessions.
    [[nodiscard]] int count() const noexcept
    {
        return _modelWindow != nullptr ? _modelWindow->tabCount() : 0;
    }

    /// The 0-based row (tab index) of the active tab in the active window, or -1.
    [[nodiscard]] int activeTabIndex() const noexcept;

    // {{{ GUI tab-strip invokables (model-backed)
    Q_INVOKABLE void activateTab(int index);
    Q_INVOKABLE void moveTab(int fromIndex, int toIndex);
    Q_INVOKABLE void setTabTitle(int index, QString const& title);
    Q_INVOKABLE void resetTabTitle(int index);
    Q_INVOKABLE void setTabColor(int index, QColor const& color);
    Q_INVOKABLE void resetTabColor(int index);
    Q_INVOKABLE void closeTabAtIndex(int index);
    Q_INVOKABLE void closeOtherTabs(int index);
    Q_INVOKABLE void closeTabsToRight(int index);

    /// The predefined tab-color palette (a grid of swatches) the user picks from, as QColors.
    Q_INVOKABLE [[nodiscard]] QVariantList tabColorPalette() const;
    // }}}

    // {{{ Split-pane operations (drive the vtmux model; Phase 2)
    /// Splits the target tab's active pane along @p vertical (true: side-by-side; false: stacked),
    /// creating a backing TerminalSession for the new leaf.
    /// @param vertical Split orientation.
    /// @param acting The session that received the keybinding; its hosting tab is the target. When
    ///        null (QML callers), the model's active tab is used.
    void splitActivePane(bool vertical, TerminalSession* acting = nullptr);
    /// Closes the active pane of the target tab (closing the tab if it was the last pane).
    /// @param acting The session that received the keybinding; its hosting tab is the target. When
    ///        null, the model's active tab is used.
    void closeActivePane(TerminalSession* acting = nullptr);
    /// Moves pane focus within the target tab in the given direction.
    /// @param direction The direction to move pane focus.
    /// @param acting The session that received the keybinding; its hosting tab is the target. When
    ///        null, the model's active tab is used.
    void focusPane(vtmux::FocusDirection direction, TerminalSession* acting = nullptr);
    // }}}

    // {{{ PaneProxy support (used by PaneProxy + the recursive split QML)
    /// The PaneProxy tree root for the active tab, or nullptr. Rebound whenever the active tab or its
    /// tree changes; the recursive PaneNode.qml renders from it.
    Q_PROPERTY(contour::PaneProxy* activeTabRootPane READ activeTabRootPane NOTIFY activeTabRootPaneChanged)
    [[nodiscard]] PaneProxy* activeTabRootPane() const noexcept { return _activeTabRootProxy; }

    /// The TerminalSession of the active tab's active pane (for window-level bindings: title,
    /// implicit size, opacity). Falls back to the active display's current session.
    ///
    /// Emits activeSessionChanged() whenever the active pane's session may have changed: a tab switch
    /// or tree rebuild (activeTabRootPaneChanged) AND a pane-focus change within a tab (which does not
    /// rebuild the tree). Binding window properties to this — rather than to the single display's
    /// session — keeps e.g. the window title following the focused pane in a split.
    Q_PROPERTY(contour::TerminalSession* activeSession READ activeSession NOTIFY activeSessionChanged)
    [[nodiscard]] TerminalSession* activeSession() const noexcept;

    /// The TerminalSession backing @p id, or nullptr. (Public for PaneProxy.)
    [[nodiscard]] TerminalSession* sessionForId(vtmux::SessionId id) const noexcept;
    /// Whether @p id is the active leaf of the active tab.
    [[nodiscard]] bool isActivePane(vtmux::PaneId id) const noexcept;
    /// Sets the split ratio of node @p id in the active tab.
    void setPaneRatio(vtmux::PaneId id, double ratio);
    /// Makes leaf @p id the active pane of the active tab.
    void activatePane(vtmux::PaneId id);
    // }}}

    Q_INVOKABLE [[nodiscard]] bool canCloseWindow() const noexcept;

    void updateColorPreference(vtbackend::ColorPreference const& preference);

    void FocusOnDisplay(display::TerminalDisplay* display);

    void update() { updateStatusLine(); }

    /// Invalidates TitleRole on every tab row so the tab strip recomputes every label. Needed when a
    /// change can shift more than one label at once: a structural reorder/close/insert renumbers the
    /// {TabPosition} of later tabs, and a config reload may change the tab-label template. No-op when
    /// there are no rows.
    void refreshAllTabTitles();

    /// Invalidates TitleRole for the tab hosting @p session, so the tab strip recomputes that tab's
    /// label after a runtime window-title / tab-name change. The label is derived from the tab's active
    /// leaf, so this is correct even when @p session is a background pane (the active-leaf label is
    /// recomputed; if @p session is not the active leaf the label simply stays the same). No-op if no
    /// tab hosts @p session. MUST be called on the GUI thread (it emits dataChanged).
    /// @param session The session whose hosting tab should refresh its label.
    void refreshTabForSession(vtmux::SessionId session);

    /// Clears the manager's record that @p display currently shows @p session, when the session is
    /// taken over by another display (TerminalSession::attachDisplay). Keeps _displayStates in sync
    /// with the display's own _session so a later FocusOnDisplay(@p display) does not re-activate a
    /// session that display no longer owns.
    /// @param display The display that has just released the session.
    /// @param session The session being taken over by another display.
    void onSessionDetachedFromDisplay(display::TerminalDisplay* display, TerminalSession* session);

    /// Evicts @p display from the per-display bookkeeping when the display itself is destroyed (its
    /// QML item / pane is torn down, e.g. closing one pane of a split or the whole window). Without
    /// this, _displayStates kept a dangling TerminalDisplay* key and a dangling currentSession that
    /// updateStatusLine()/activeTabIndex() would later dereference. Resets _activeDisplay when it
    /// pointed at @p display so no stale active display survives the teardown. Idempotent and safe to
    /// call for a display never registered.
    /// @param display The display being destroyed.
    void detachDisplay(display::TerminalDisplay* display) noexcept;

    void allowCreation() { _allowCreation = true; }

    void doNotSwitchToNewSession() { _allowSwitchOfTheSession = false; }

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

    // DisplayState (and detachSessionFromState) now live in DisplayState.h so the pure
    // session-bookkeeping logic is unit-testable without this Qt/app-heavy header.

    // {{{ vtmux::ModelEvents — turn model changes into Qt model/signal notifications
    void tabAdded(vtmux::WindowId window, vtmux::TabId tab, int index) override;
    void tabClosed(vtmux::WindowId window, vtmux::TabId tab, int index) override;
    void tabMoved(vtmux::WindowId window, vtmux::TabId tab, int fromIndex, int toIndex) override;
    void activeTabChanged(vtmux::WindowId window, vtmux::TabId tab, int index) override;
    void paneSplit(vtmux::TabId tab, vtmux::PaneId splitNode, vtmux::PaneId newLeaf) override;
    void paneClosed(vtmux::TabId tab, vtmux::PaneId closed, vtmux::PaneId survivor) override;
    void activePaneChanged(vtmux::TabId tab, vtmux::PaneId leaf) override;
    void paneRatioChanged(vtmux::TabId tab, vtmux::PaneId splitNode, double ratio) override;
    void tabTitleChanged(vtmux::TabId tab) override;
    void tabColorChanged(vtmux::TabId tab) override;
    // }}}

  signals:
    void multimediaReadyChanged();
    void activeTabIndexChanged();
    void activeTabRootPaneChanged();
    /// Emitted when the active tab's active-pane session may have changed (tab switch, tree rebuild, or
    /// a pane-focus change). Drives window-level bindings (e.g. the window title) bound to activeSession.
    void activeSessionChanged();

  private:
    contour::TerminalSession* activateSession(TerminalSession* session, bool isNewSession = false);

    /// Creates a TerminalSession backing the given vtmux session id, registers it in the
    /// SessionId<->TerminalSession maps and the session list, and claims C++ ownership. Does NOT
    /// touch the model (the caller has already created the corresponding tab or split leaf).
    contour::TerminalSession* createBackingSession(vtmux::SessionId sessionId,
                                                   std::optional<std::string> cwd);

    void tryFindSessionForDisplayOrClose();

    /// Emits dataChanged for the row of @p tab so the QML tab strip refreshes that tab.
    void notifyTabRowChanged(vtmux::TabId tab, QList<int> const& roles);

    /// Resolves the label shown on @p row's tab. A template string is chosen by precedence and then
    /// expanded (see expandTabLabel) with {TabPosition} = @p row + 1 and {WindowTitle} = the active
    /// leaf's title:
    ///   1. a user rename (the tab's runtime title) — the rename itself acts as the template, so a
    ///      rename of "{WindowTitle}" tracks the title while a plain "deploy" passes through;
    ///   2. "Multiple panes" when the tab holds a split;
    ///   3. otherwise the active leaf session's profile tab-label template.
    /// @param tab The tab to label (may be null).
    /// @param session The session backing @p tab's active leaf (may be null).
    /// @param row The tab's 0-based list-model row, used for the 1-based {TabPosition}.
    /// @return The resolved label (empty when @p tab and @p session are both null).
    [[nodiscard]] QString resolvedTabLabel(vtmux::Tab* tab, TerminalSession* session, int row) const;

    /// Invalidates IsActiveRole on every tab row so the tab strip repaints its active highlight.
    /// The highlight is painted per row from the isActive role (TabItem.qml), so a tab switch must
    /// re-emit that role for BOTH the previously-active and newly-active rows. The active-tab callback
    /// carries only the new tab, so this refreshes the whole (single-digit) row range rather than
    /// tracking the previous row. No-op when there are no rows.
    void refreshActiveTabHighlight();

    /// Rebuilds the PaneProxy tree for the active tab and rebinds activeTabRootPane. Proxies are
    /// reused by PaneId across rebuilds so a surviving pane's QML item is not recreated.
    void rebuildActiveTabPaneProxies();

    /// The row index of the tab carrying @p tab id in the (single) window, or -1.
    [[nodiscard]] int rowOfTab(vtmux::TabId tab) const noexcept;

    /// The 0-based row of the tab hosting @p session's active leaf, or -1 when @p session is null or
    /// has no model tab. The single session -> tab id (O(1)) -> row resolution shared by
    /// activeTabIndex() (which falls back to the model's active tab) and activeTabPositionForSession()
    /// (which converts to a 1-based marker) — previously the same two-step lookup was open-coded twice.
    /// @param session The session to locate (may be null).
    /// @return The 0-based tab row, or -1.
    [[nodiscard]] int rowOfSession(TerminalSession const* session) const noexcept
    {
        if (session == nullptr)
            return -1;
        if (auto const it = _tabBySession.find(session->modelSessionId().value); it != _tabBySession.end())
            return rowOfTab(it->second);
        return -1;
    }

    /// The model tab whose tree contains a leaf hosting @p session, or nullptr.
    [[nodiscard]] vtmux::Tab* findTabHostingSession(vtmux::SessionId session) const noexcept;

    /// The model tab backing list-model row @p index, or nullptr.
    [[nodiscard]] vtmux::Tab* tabAtRow(int index) const noexcept;

    /// Collects the backing TerminalSessions of every leaf pane in @p tab (empty if @p tab is null).
    /// @param tab The tab whose pane sessions to gather.
    /// @return The backing sessions, in pane-tree order.
    [[nodiscard]] std::vector<TerminalSession*> sessionsInTab(vtmux::Tab* tab) const;

    /// Gathers the backing sessions of every tab in the window matching @p predicate, in row order.
    /// Shared by the bulk-close operations (closeOtherTabs / closeTabsToRight), which snapshot the doomed
    /// sessions before the model mutates. Returns empty when there is no window.
    /// @param predicate Selects which tabs' sessions to gather.
    /// @return The matching tabs' backing sessions.
    [[nodiscard]] std::vector<TerminalSession*> gatherSessionsOfTabsWhere(
        std::function<bool(vtmux::Tab*)> const& predicate) const;

    /// Terminates each of @p sessions, the single whole-tab close primitive shared by all close
    /// paths (CloseTab, the tab ✕ button, "Close Other Tabs", "Close Tabs to the Right").
    /// terminate() drives sessionClosed -> removeSession, which collapses the model to the survivor on
    /// each pane close and tears the tab down with its last pane. The caller passes a stable snapshot
    /// (e.g. from sessionsInTab) so the model mutations terminate() triggers do not invalidate the
    /// iteration. Keeping this in one place ensures the close entry points stay consistent.
    /// @param sessions The backing sessions to terminate.
    void terminateSessions(std::span<TerminalSession* const> sessions);

    /// The active tab of the (single) model window, or nullptr if there is none.
    [[nodiscard]] vtmux::Tab* activeModelTab() const noexcept
    {
        return _modelWindow != nullptr ? _modelWindow->activeTab() : nullptr;
    }

    /// Whether @p tab is the active tab of the (single) model window. Used to skip work (e.g. the
    /// PaneProxy rebuild) that only matters for the tab currently bound to the QML.
    /// @param tab The tab id to test.
    /// @return true if @p tab is the active tab, false otherwise (including when there is none).
    [[nodiscard]] bool isActiveTab(vtmux::TabId tab) const noexcept
    {
        auto* active = activeModelTab();
        return active != nullptr && active->id() == tab;
    }

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

    /// The tab a pane action should target: the tab hosting @p acting (the session that received the
    /// keybinding) when known, else the model's active tab. Keyboard pane actions must act on the tab
    /// the user is typing in, which can differ from the model's active tab in a desynced/multi-display
    /// state.
    /// @param acting The acting session, or nullptr for callers without one (QML).
    /// @return The target tab, or nullptr.
    [[nodiscard]] vtmux::Tab* paneActionTargetTab(TerminalSession* acting) const noexcept
    {
        if (auto* tab = tabHostingSession(acting); tab != nullptr)
            return tab;
        return activeModelTab();
    }

    /// Activates the tab at list-model row @p row: makes it the model's active tab (so the rendered
    /// split tree and subsequent pane operations target it) and activates the display session of its
    /// active leaf. No-op if @p row is out of range.
    void activateModelTabByRow(int row);

    void updateStatusLine()
    {
        // Build the tab list once: it is identical for every display. Iterate the model's tabs
        // directly (one status-line entry per tab, not per pane) so the resolved title and the
        // user-chosen color come straight from the tab in hand — no per-session _tabBySession ->
        // findTab hash-lookup chain on this hot path, and a split tab no longer contributes two
        // entries.
        auto tabs = [&]() {
            std::vector<vtbackend::TabsInfo::Tab> result;
            if (_modelWindow == nullptr)
                return result;
            auto const tabCount = _modelWindow->tabCount();
            result.reserve(static_cast<size_t>(tabCount));
            auto const& resolver = _model->sessionTitleResolver();
            for (auto const row: std::views::iota(0, tabCount))
            {
                auto* tab = _modelWindow->tabAt(row);
                if (tab == nullptr)
                    continue;
                auto const color = tab->color().value_or(vtbackend::RGBColor { 0, 0, 0 });
                result.push_back({ .name = tab->title(resolver), .color = color });
            }
            return result;
        }();

        // Publish the tab info to EVERY display's current session, not just the active display's. A
        // title/tab-name change in a non-focused window's foreground session refreshes through here too
        // (refreshGuiTabInfoForStatusLine -> _manager->update()); updating only the active display would
        // leave that window's indicator status line showing the stale label until it regained focus.
        //
        // The active-tab marker must be computed PER DISPLAY: each display shows its own current
        // session, so the highlighted tab is the row of the tab hosting THAT display's session, not the
        // focused display's active tab. Broadcasting a single activeTabIndex() (the focused display's)
        // made every other display highlight the wrong tab.
        for (auto& [display, state]: _displayStates)
        {
            if (auto* session = state.currentSession; session)
                session->terminal().setGuiTabInfoForStatusLine(vtbackend::TabsInfo {
                    .tabs = tabs,
                    .activeTabPosition = activeTabPositionForSession(session),
                });
        }
    }

    /// The 1-based position of the tab hosting @p session's active leaf, for that session's status
    /// line {Tabs} marker. Resolves session -> tab id (O(1)) -> row; falls back to 1 when the session
    /// has no model tab (so the marker stays in range). Per-display: each display passes its own
    /// current session so its status line highlights its own tab, not the focused display's.
    /// @param session The display's current session (must be non-null).
    /// @return The 1-based tab position.
    [[nodiscard]] size_t activeTabPositionForSession(TerminalSession* session) const noexcept
    {
        if (auto const row = rowOfSession(session); row >= 0)
            return static_cast<size_t>(row) + 1;
        return 1;
    }

    ContourGuiApp& _app;
    SessionFactory _sessionFactory;
    std::chrono::seconds _earlyExitThreshold;
    std::unordered_map<display::TerminalDisplay*, DisplayState> _displayStates;
    std::vector<TerminalSession*> _sessions;
    display::TerminalDisplay* _activeDisplay = nullptr;

    // on windows qt tries to create a new session
    // twice on qml file loading, this bool is used to
    // prevent that, and to allow creation of new session
    // user have to call allowCreation() method first
    std::atomic<bool> _allowCreation { true };
    bool _multimediaReady = false;

    // When we spawn a new window and share multiple windows within the same process,
    // we first create a new session and then attempt to "activateSession".
    // However, we do not want to immediately switch to this session from the old display,
    // since a new display will be created, and the session should appear on that new display.
    // To handle this, we set a flag, and once the new display is created, we switch to this session.
    std::atomic<bool> _allowSwitchOfTheSession { true };

    // {{{ vtmux model integration
    // The Qt-free layout model. The manager keeps it in sync 1:1 with _sessions (one tab per
    // session, single pane) for now; splits (Phase 2) add pane nodes within a tab. The model owns
    // the authoritative tab title and color.
    std::unique_ptr<vtmux::SessionModel> _model;
    vtmux::Window* _modelWindow = nullptr;
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

    // PaneProxy tree for the active tab (the recursive split QML renders from it). Proxies are
    // owned here and reused by PaneId across rebuilds so a surviving pane's QML item / display is
    // not torn down when a sibling splits or closes.
    std::unordered_map<uint64_t, PaneProxy*> _paneProxies;
    PaneProxy* _activeTabRootProxy = nullptr;

    // Cached QVariantList of the (immutable) tab-color palette, built lazily on first request.
    mutable QVariantList _tabColorPaletteCache;
    // }}}
};

} // namespace contour

Q_DECLARE_INTERFACE(contour::TerminalSessionManager, "org.contour.TerminalSessionManager")
