// SPDX-License-Identifier: Apache-2.0
#include <contour/ColorConversion.h>
#include <contour/ContourGuiApp.h>
#include <contour/PaneProxy.h>
#include <contour/TabLabel.h>
#include <contour/TerminalSession.h>
#include <contour/TerminalSessionManager.h>

#include <vtbackend/primitives.h>

#include <vtpty/Process.h>
#if defined(VTPTY_LIBSSH2)
    #include <vtpty/SshSession.h>
#endif

#include <QtQml/QQmlEngine>
#include <QtQuick/QQuickWindow>

#include <algorithm>
#include <filesystem>
#include <string>

using namespace std::string_literals;

using std::make_unique;
using std::nullopt;

namespace contour
{

TerminalSessionManager::TerminalSessionManager(ContourGuiApp& app):
    _app { app }, _sessionFactory { app }, _earlyExitThreshold {}
{
    // The model allocates a fresh vtmux session id whenever it needs one (a new tab or a new split
    // pane). For tabs we pre-mint the id in createSessionInBackground() and hand it back here; for
    // splits (Phase 2) there is no pre-minted id, so we mint a fresh one. Either way the manager
    // then maps the id to a backing TerminalSession.
    _model = std::make_unique<vtmux::SessionModel>(*this, [this]() -> vtmux::SessionId {
        if (_pendingSessionId.has_value())
        {
            auto const id = *_pendingSessionId;
            _pendingSessionId.reset();
            return id;
        }
        return vtmux::SessionId { _nextSessionId++ };
    });
    _model->setSessionTitleResolver([this](vtmux::SessionId id) -> std::string {
        if (auto* session = sessionForId(id))
            if (auto name = session->name())
                return *name;
        return {};
    });
    _modelWindow = _model->createWindow();
}

TerminalSession* TerminalSessionManager::sessionForId(vtmux::SessionId id) const noexcept
{
    auto const it = _sessionsById.find(id.value);
    return it != _sessionsById.end() ? it->second : nullptr;
}

int TerminalSessionManager::rowOfTab(vtmux::TabId tab) const noexcept
{
    return _modelWindow != nullptr ? _modelWindow->indexOf(tab) : -1;
}

TerminalSession* TerminalSessionManager::createSessionInBackground()
{
    // TODO: Remove dependency on app-knowledge and pass shell / terminal-size instead.
    // The GuiApp *or* (Global)Config could be made a global to be accessible from within QML.

    if (!_activeDisplay)
    {
        managerLog()("No active display found. something went wrong.");
    }

    if (!_allowCreation)
    {
        managerLog()("Session creation is disabled.");
        // try to find for the selected display a session to use

        for (auto& session: _sessions)
        {
            if (_displayStates[_activeDisplay].currentSession == session)
            {
                managerLog()("Found suitable session Returning it.");
                return session;
            }
        }
    }

#if !defined(_WIN32)
    auto ptyPath = [this]() -> std::optional<std::string> {
        if (_sessions.empty())
            return std::nullopt;
        auto& terminal = _sessions[0]->terminal();
        if (auto const* ptyProcess = dynamic_cast<vtpty::Process const*>(&terminal.device()))
            return ptyProcess->workingDirectory();
        return std::nullopt;
    }();
#else
    std::optional<std::string> ptyPath = std::nullopt;
    if (!_sessions.empty())
    {
        auto& terminal = _sessions[0]->terminal();
        {
            auto _l = std::scoped_lock { terminal };
            ptyPath = terminal.currentWorkingDirectory();
        }
    }
#endif

    // Pre-mint the session id so the model's allocator (see ctor) hands it back, keeping the model
    // tab/pane and the Qt session on one id.
    auto const sessionId = vtmux::SessionId { _nextSessionId++ };
    auto* session = createBackingSession(sessionId, ptyPath);

    // Mirror this new session into the vtmux model as a new single-pane tab.
    if (_modelWindow != nullptr)
        if (auto* tab = _model->createTab(_modelWindow->id()))
            _tabBySession[sessionId.value] = tab->id();

    _allowCreation = false;
    return session;
}

TerminalSession* TerminalSessionManager::createBackingSession(vtmux::SessionId sessionId,
                                                              std::optional<std::string> cwd)
{
    auto* session = new TerminalSession(this, _sessionFactory.createPty(std::move(cwd)), _app);
    managerLog()("Create backing session with ID {}({}) at index {}",
                 session->id(),
                 (void*) session,
                 _sessions.size());

    _sessions.insert(_sessions.end(), session);
    _pendingSessionId = sessionId;
    _sessionsById[sessionId.value] = session;
    session->setModelSessionId(sessionId);

    connect(session, &TerminalSession::sessionClosed, [this, session]() { removeSession(*session); });

    // Claim ownership of this object, so that it will be deleted automatically by the QML's GC.
    //
    // QQmlEngine falsely assumed that the object would not be needed anymore at random times in active
    // sessions. This will work around it, by explicitly claiming ownership of the object.
    QQmlEngine::setObjectOwnership(session, QQmlEngine::CppOwnership);

    return session;
}

void TerminalSessionManager::setSession(size_t index)
{
    Require(index <= _sessions.size());
    managerLog()(std::format("SET SESSION: index: {}, _sessions.size(): {}", index, _sessions.size()));

    if (index < _sessions.size())
        activateSession(_sessions[index]);
    else
        activateSession(createSessionInBackground());
}

TerminalSession* TerminalSessionManager::activateSession(TerminalSession* session, bool isNewSession)
{
    if (!session)
        return nullptr;

    managerLog()("Activating session ID {} {} at index {}",
                 session->id(),
                 (void*) session,
                 getSessionIndexOf(session).value_or(-1));

    // iterate over _displayStates to see if this session is already active
    for (auto& [display, state]: _displayStates)
    {
        managerLog()("display: {}, session: {}\n", (void*) display, (void*) state.currentSession);
        if (display && state.currentSession == session)
        {
            if (!display->hasSession())
            {
                managerLog()("Display does not have a session will set it to another session.");
                continue;
            }
            managerLog()("Session is already active : (display {}, ID {} {})",
                         (void*) display,
                         session->id(),
                         (void*) session);
            return session;
        }
    }

    if (!_activeDisplay)
    {
        managerLog()("No active display found. something went wrong.");
    }

    if (!_allowSwitchOfTheSession)
    {
        _displayStates[nullptr].currentSession = session;
        _allowSwitchOfTheSession = true;
        return session;
    }

    auto& displayState = _displayStates[_activeDisplay];
    displayState.previousSession = displayState.currentSession;
    displayState.currentSession = session;
    updateStatusLine();

    if (_activeDisplay)
    {

        auto const pixels = _activeDisplay->pixelSize();
        auto const totalPageSize = _activeDisplay->calculatePageSize();

        // Ensure that the existing session is resized to the display's size.
        if (!isNewSession)
        {
            managerLog()("Resize existing session to display size: {}x{}.",
                         _activeDisplay->width(),
                         _activeDisplay->height());
            auto const _ = std::scoped_lock { displayState.currentSession->terminal() };
            displayState.currentSession->terminal().resizeScreen(totalPageSize, pixels);
        }

        managerLog()(
            "Set display {} to session: {}({}).", (void*) _activeDisplay, session->id(), (void*) session);
        // resize terminal session before display is attached to it
        _activeDisplay->setSession(displayState.currentSession);

        // Resize active session after display is attached to it
        // to return a lost line
        {
            auto const _ = std::scoped_lock { displayState.currentSession->terminal() };
            displayState.currentSession->terminal().resizeScreen(totalPageSize, pixels);
        }

        // These resizeScreen() calls go straight to the terminal, bypassing the renderer's geometry
        // staging, so the renderer's published/live grid metrics would stay stale (diagnostics and any
        // gridMetrics() reader would see the wrong size/margin). Push the full geometry (page size, pixel
        // size and margin) into the renderer so its grid metrics stay consistent with the terminal — a
        // page-size-only sync would leave the previous session's margin live until a later resize.
        _activeDisplay->syncRendererGeometry(totalPageSize, pixels);
    }

    // The active-tab index published to QML is the row of THIS display's currentSession (see
    // activeTabIndex()). Switch paths that reach the display without going through
    // _model->activateTab() — setSession, FocusOnDisplay, tryFindSessionForDisplayOrClose,
    // createSession, the no-tab switchToPreviousTab fallback — change currentSession here but never
    // fire the model's activeTabChanged, so notify directly. Guarded on an actual session change to
    // avoid spurious focus-restore churn (main.qml's onActiveTabIndexChanged); the strip highlight is
    // refreshed too so it follows on these paths as well.
    if (displayState.previousSession != displayState.currentSession)
    {
        emit activeTabIndexChanged();
        refreshActiveTabHighlight();
    }

    return session;
}

void TerminalSessionManager::onSessionDetachedFromDisplay(display::TerminalDisplay* display,
                                                          TerminalSession* session)
{
    if (display == nullptr)
        return;
    auto const it = _displayStates.find(display);
    if (it == _displayStates.end())
        return;
    // Only clear if this display still references the taken-over session; never disturb a mapping
    // the display has already moved on to. The boolean "did anything change" result is irrelevant
    // here.
    std::ignore = detachSessionFromState(it->second, session);
}

void TerminalSessionManager::detachDisplay(display::TerminalDisplay* display) noexcept
{
    if (display == nullptr)
        return;

    // The display (a window's single-pane terminal, or one pane of a split) is being destroyed. Drop
    // its _displayStates entry so the map never holds a dangling TerminalDisplay* key or a dangling
    // currentSession that a later updateStatusLine()/activeTabIndex() would dereference. This is the
    // ONLY eviction path for a destroyed display: ~TerminalDisplay -> detachDisplay; closeWindow()
    // only ever ran for the active display and so leaked every split pane's entry.
    if (_displayStates.erase(display) != 0)
        managerLog()("Detached destroyed display {} from per-display state.", (void*) display);

    // Never leave _activeDisplay pointing at a freed display. A surviving display will reclaim focus
    // (FocusOnDisplay) on the next focus-in.
    if (_activeDisplay == display)
        _activeDisplay = nullptr;
}

void TerminalSessionManager::FocusOnDisplay(display::TerminalDisplay* display)
{
    managerLog()("Setting active display to {}", (void*) display);
    _activeDisplay = display;

    // if we have a session in nullptr display, set it to this one
    if (_displayStates[nullptr].currentSession != nullptr)
    {
        _displayStates[_activeDisplay] = _displayStates[nullptr];
        _displayStates[nullptr].currentSession = nullptr;
    }

    // if this is new display, find a session to attach to
    if (_displayStates[_activeDisplay].currentSession == nullptr)
    {
        tryFindSessionForDisplayOrClose();
        return;
    }

    updateStatusLine();
    activateSession(_displayStates[_activeDisplay].currentSession);
}

TerminalSession* TerminalSessionManager::createSession()
{
    return activateSession(createSessionInBackground(), true /*force resize on before display-attach*/);
}

void TerminalSessionManager::switchToPreviousTab()
{
    auto* previous = _displayStates[_activeDisplay].previousSession;
    if (previous == nullptr)
        return;

    // Activate the tab hosting the previously-focused session through the model so the model's
    // active tab follows, falling back to a plain display activation if the session has no tab.
    if (auto* tab = findTabHostingSession(previous->modelSessionId()); tab != nullptr)
    {
        managerLog()("switch to previous tab: row {}", rowOfTab(tab->id()));
        activateModelTabByRow(rowOfTab(tab->id()));
        return;
    }

    activateSession(previous);
}

void TerminalSessionManager::switchToTabLeft()
{
    auto const tabCount = count();
    if (tabCount <= 0)
        return;
    auto const current = activeTabIndex();
    // Move one tab to the left in tab-space (rows are tabs), wrapping at the start.
    auto const target = current > 0 ? current - 1 : tabCount - 1;
    managerLog()("switch to tab left: current row {}, target row {}, tabs {}", current, target, tabCount);
    activateModelTabByRow(target);
}

void TerminalSessionManager::switchToTabRight()
{
    auto const tabCount = count();
    if (tabCount <= 0)
        return;
    auto const current = activeTabIndex();
    // Move one tab to the right in tab-space (rows are tabs), wrapping at the end.
    auto const target = current >= 0 && current < tabCount - 1 ? current + 1 : 0;
    managerLog()("switch to tab right: current row {}, target row {}, tabs {}", current, target, tabCount);
    activateModelTabByRow(target);
}

void TerminalSessionManager::switchToTab(int position)
{
    // position is 1-based (keyboard "go to tab N"); rows are tabs.
    managerLog()("switchToTab to position {} (out of {} tabs)", position, count());
    if (1 <= position && position <= count())
        activateModelTabByRow(position - 1);
}

void TerminalSessionManager::closeWindow()
{
    if (!_activeDisplay)
    {
        managerLog()("No active display found. Cannot close window.");
        return;
    }

    // This manager is a process-wide singleton shared by every in-process OS window (NewWindow with
    // the default spawn_new_process:false just loads another main.qml against the same manager and the
    // same _modelWindow / _sessions / registries). So the global state must only be torn down when the
    // LAST OS window closes. If a display of ANOTHER OS window is still alive, removing the shared
    // model window or clearing the registries here would wipe that window's tabs and sessions.
    auto* closingDisplay = _activeDisplay;

    // Group by OS window, not by display: a split tab in THIS window owns several displays (one per
    // pane), and those must NOT be mistaken for other open windows — otherwise closing the only
    // window when it has split panes takes the "one of several windows" branch and skips teardown,
    // leaking its sessions/shells and leaving dangling display keys behind. Two displays belong to the
    // same OS window iff they share a QQuickWindow.
    auto const windowOf = [](display::TerminalDisplay* display) -> QQuickWindow* {
        return display != nullptr ? display->window() : nullptr;
    };

    if (!isLastActiveDisplay(_displayStates, closingDisplay, windowOf))
    {
        managerLog()("Closing one of several windows: releasing only this window's displays.");
        // Drop the closing window's display bookkeeping (every display that shares its QQuickWindow,
        // i.e. all panes of the closing window), but leave the shared model window, sessions and
        // registries intact for the surviving windows. Erasing only _activeDisplay would leak the
        // closing window's other pane displays as dangling keys in _displayStates.
        auto* const closingWindow = windowOf(closingDisplay);
        std::erase_if(_displayStates, [&](auto const& entry) {
            return entry.first != nullptr && windowOf(entry.first) == closingWindow;
        });
        _activeDisplay = nullptr;
        return;
    }

    managerLog()("Closing the last window: tearing down {} session(s) and the model window.",
                 _sessions.size());

    // Last window: tear down the whole shared state. removeWindow fires tabClosed per tab
    // (-> Qt beginRemoveRows); it does NOT fire activeTabChanged when the final (active, index-0) tab
    // is erased, so the PaneProxy pruning that activeTabChanged would have driven never runs. Clear the
    // proxy tree explicitly below rather than relying on that signal, or _activeTabRootProxy would keep
    // a cached Pane* into the just-freed model tree (a dangling pointer a QML binding could deref).
    if (_modelWindow != nullptr)
        _model->removeWindow(_modelWindow->id());
    _modelWindow = nullptr;

    // Explicitly prune the PaneProxy tree (see above): clear each proxy's cached Pane* first so a
    // getter that runs before deleteLater() takes effect sees no stale pointer, then drop the root.
    for (auto const& [id, proxy]: _paneProxies)
    {
        proxy->setPane(nullptr);
        proxy->deleteLater();
    }
    _paneProxies.clear();
    if (_activeTabRootProxy != nullptr)
    {
        _activeTabRootProxy = nullptr;
        emit activeTabRootPaneChanged();
    }

    // The model events do not touch the SessionId registries or the legacy session list; clear them
    // so no stale entry resolves a session whose display is gone. The QML-owned TerminalSessions are
    // reclaimed by the engine's GC once the window's QML is torn down.
    _sessionsById.clear();
    _tabBySession.clear();
    _sessions.clear();
    _displayStates.erase(closingDisplay);
    _activeDisplay = nullptr;
}

void TerminalSessionManager::closeTab()
{
    if (!_activeDisplay || !_displayStates[_activeDisplay].currentSession)
    {
        managerLog()("Failed to close tab: no active display or no session in active display.");
        return;
    }

    // CloseTab is documented "Closes current tab." — close every pane of the tab hosting the active
    // display's session, not just that one leaf. removeSession() alone closes a single pane, which
    // would leave a split tab open with its surviving panes. Terminate each of the tab's pane
    // sessions; the model collapses to the survivor on each close and tears the tab down with the
    // last pane (same whole-tab path as closeTabAtIndex / the GUI context menu).
    auto* tab = tabHostingSession(_displayStates[_activeDisplay].currentSession);
    if (tab == nullptr)
    {
        // No model tab (single legacy session): fall back to removing just that session.
        removeSession(*_displayStates[_activeDisplay].currentSession);
        return;
    }

    managerLog()("Close tab: current session ID {}, tab row {}",
                 _displayStates[_activeDisplay].currentSession->id(),
                 rowOfTab(tab->id()));

    terminateSessions(sessionsInTab(tab));
}

void TerminalSessionManager::moveTabTo(int position)
{
    // position is 1-based (the keybinding's argument); the model is 0-based tab-space. Reorder the
    // active display's tab through the authoritative model so the tab strip and status line — both
    // model-driven — actually move. (The legacy _sessions swap touched neither.)
    if (_modelWindow == nullptr)
        return;
    auto* tab = tabHostingSession(_displayStates[_activeDisplay].currentSession);
    if (tab == nullptr)
        return;
    if (position < 1 || position > _modelWindow->tabCount())
        return;
    _model->moveTab(_modelWindow->id(), tab->id(), position - 1);
    updateStatusLine();
}

void TerminalSessionManager::moveTabToLeft(TerminalSession* session)
{
    if (_modelWindow == nullptr)
        return;
    auto* tab = tabHostingSession(session);
    if (tab == nullptr)
        return;
    auto const row = rowOfTab(tab->id());
    if (row > 0)
    {
        _model->moveTab(_modelWindow->id(), tab->id(), row - 1);
        updateStatusLine();
    }
}

void TerminalSessionManager::moveTabToRight(TerminalSession* session)
{
    if (_modelWindow == nullptr)
        return;
    auto* tab = tabHostingSession(session);
    if (tab == nullptr)
        return;
    auto const row = rowOfTab(tab->id());
    if (row >= 0 && row + 1 < _modelWindow->tabCount())
    {
        _model->moveTab(_modelWindow->id(), tab->id(), row + 1);
        updateStatusLine();
    }
}

void TerminalSessionManager::currentSessionIsTerminated()
{
    managerLog()("got notified that session is terminated, number of existing sessions: _sessions.size(): {}",
                 _sessions.size());
}

void TerminalSessionManager::removeSession(TerminalSession& thatSession)
{
    managerLog()("remove session: session: {}, _sessions.size(): {}", (void*) &thatSession, _sessions.size());

    auto const i = std::ranges::find(_sessions, &thatSession);
    if (i == _sessions.end())
    {
        managerLog()("Session not found in session list.");
        return;
    }

    // Mirror the removal into the vtmux model FIRST, while the registry still resolves this session
    // to its tab/leaf, then erase the local bookkeeping. A session is one *pane*; closing it must
    // close only that pane. closePane() absorbs the surviving sibling when the tab still has other
    // panes and only tears down the whole tab (firing tabClosed -> beginRemoveRows) when this was
    // the tab's last pane — so closing one split pane no longer destroys its siblings.
    auto const sessionId = thatSession.modelSessionId();
    if (_modelWindow != nullptr)
        if (auto* tab = findTabHostingSession(sessionId); tab != nullptr)
            if (auto* leaf = tab->rootPane()->findLeaf(sessionId); leaf != nullptr)
                _model->closePane(_modelWindow->id(), tab->id(), leaf->id());
    _sessionsById.erase(sessionId.value);
    _tabBySession.erase(sessionId.value);

    _sessions.erase(i);

    // Scrub the removed session from every per-display state. The session object is about to be
    // reclaimed (QML CppOwnership GC), so any DisplayState still naming it as current/previous would
    // hold a dangling pointer that updateStatusLine()/activeSession() would dereference. (closeWindow
    // only ever cleared the active display's entry; a closed split pane left its sibling displays'
    // states pointing at the freed session.)
    for (auto& [display, state]: _displayStates)
        std::ignore = detachSessionFromState(state, &thatSession);

    tryFindSessionForDisplayOrClose();
}

void TerminalSessionManager::tryFindSessionForDisplayOrClose()
{
    managerLog()("Trying to find session for display: {}", (void*) _activeDisplay);
    for (auto& session: _sessions)
    {
        bool saveToSwitch { true };
        // check if session is not used by any display and then switch
        for (auto& [display, state]: _displayStates)
        {
            if (display && (state.currentSession == session))
            {
                saveToSwitch = false;
                break;
            }
        }

        if (saveToSwitch)
        {
            managerLog()("Switching to session: {}", (void*) session);
            activateSession(session);
            return;
        }
    }
    updateStatusLine();
    // No free session to hand this display: close it. Guard against a null active display — it may
    // have just been evicted by detachDisplay() (a destroyed pane), in which case there is nothing
    // left to close here.
    if (_activeDisplay != nullptr)
        _activeDisplay->closeDisplay();
}

void TerminalSessionManager::updateColorPreference(vtbackend::ColorPreference const& preference)
{
    for (auto& session: _sessions)
        session->updateColorPreference(preference);
}

// {{{ QAbstractListModel
QHash<int, QByteArray> TerminalSessionManager::roleNames() const
{
    return {
        { Qt::DisplayRole, "display" }, { TitleRole, "title" },         { ColorRole, "accentColor" },
        { IsActiveRole, "isActive" },   { PaneCountRole, "paneCount" }, { SessionIdRole, "sessionId" },
        { RawTitleRole, "rawTitle" },
    };
}

QVariant TerminalSessionManager::data(const QModelIndex& index, int role) const
{
    auto const row = index.row();
    if (row < 0 || row >= rowCount())
        return {};

    // Rows are tabs. Resolve the tab for this row and the session backing its active leaf; do not
    // index _sessions by the row, which is pane-space and diverges from tab-space after a split.
    auto* tab = tabAtRow(row);
    auto* session = tab != nullptr ? sessionForId(tab->activePane()->session()) : nullptr;

    switch (role)
    {
        case Qt::DisplayRole:
        case SessionIdRole: return session != nullptr ? QVariant(session->id()) : QVariant {};
        case TitleRole: return resolvedTabLabel(tab, session, row);
        case RawTitleRole:
            // The un-expanded rename template, for the inline editor. Empty for a never-renamed tab so
            // the editor opens blank (rather than freezing the displayed expansion as a literal name).
            return tab != nullptr ? QString::fromStdString(tab->runtimeTitle().value_or("")) : QString {};
        case ColorRole:
            if (tab != nullptr && tab->color().has_value())
                return toQColor(*tab->color());
            return QColor(Qt::transparent);
        case IsActiveRole: return row == activeTabIndex();
        case PaneCountRole: return tab != nullptr ? tab->paneCount() : 1;
        default: return {};
    }
}

int TerminalSessionManager::rowCount(const QModelIndex& parent) const
{
    crispy::ignore_unused(parent);

    // One row per tab in the active window. A tab may host several split panes (and thus several
    // backing sessions), but it is still a single row in the tab strip, so the row count must track
    // the model's tabs — not _sessions, which counts every pane.
    return _modelWindow != nullptr ? _modelWindow->tabCount() : 0;
}
// }}}

vtmux::Tab* TerminalSessionManager::tabAtRow(int index) const noexcept
{
    return _modelWindow != nullptr ? _modelWindow->tabAt(index) : nullptr;
}

vtmux::Tab* TerminalSessionManager::findTabHostingSession(vtmux::SessionId session) const noexcept
{
    if (_modelWindow == nullptr)
        return nullptr;
    // O(1) via the SessionId -> TabId index.
    if (auto const it = _tabBySession.find(session.value); it != _tabBySession.end())
        return _model->findTab(it->second);
    return nullptr;
}

int TerminalSessionManager::activeTabIndex() const noexcept
{
    // Rows are tabs, so the active tab index is the row of the tab hosting the active display's
    // current session (rowOfSession resolves session -> tab id (O(1)) -> row in one tab scan); fall
    // back to the model's own active-tab index when no display session is known yet or the session
    // has no model tab.
    auto const stateIt = _displayStates.find(_activeDisplay);
    if (stateIt != _displayStates.end())
        if (auto const row = rowOfSession(stateIt->second.currentSession); row >= 0)
            return row;

    return _modelWindow != nullptr ? _modelWindow->activeTabIndex() : -1;
}

// {{{ vtmux::ModelEvents
void TerminalSessionManager::tabAdded(vtmux::WindowId, vtmux::TabId, int index)
{
    // tabAdded is a structural row insertion only. The active-tab work (activeTabIndexChanged + the
    // PaneProxy tree rebuild) belongs to activeTabChanged, which createTab always fires right after
    // tabAdded because a new tab becomes active. Doing it here too made every new tab/window walk
    // the active-tab pane tree and re-trigger the QML focus-restore binding twice. Adding a tab that
    // does NOT become active also must not rebuild the *current* active tab's proxies, so deferring
    // is the more correct behavior, not just the cheaper one.
    beginInsertRows(QModelIndex(), index, index);
    endInsertRows();
    // A middle insert shifts the {TabPosition} of every later tab; refresh all labels so positional
    // templates renumber. An append leaves earlier positions unchanged, but the whole-column refresh
    // is single-digit cheap, so handle both the same way.
    refreshAllTabTitles();
}

void TerminalSessionManager::tabClosed(vtmux::WindowId, vtmux::TabId, int index)
{
    beginRemoveRows(QModelIndex(), index, index);
    endRemoveRows();
    emit activeTabIndexChanged();
    // Closing a tab renumbers every tab after it; refresh all labels so positional templates update.
    refreshAllTabTitles();
}

void TerminalSessionManager::tabMoved(vtmux::WindowId, vtmux::TabId, int fromIndex, int toIndex)
{
    // Project the reorder as a real row move so persistent indices, the view's currentIndex and the
    // tab strip's move animation follow the tab — a dataChanged() over the range would only
    // re-decorate rows in place and leave the view's row mapping wrong. Qt's beginMoveRows wants the
    // destination row *before* which the row is inserted: for a downward move that is toIndex + 1.
    auto const destination = toIndex > fromIndex ? toIndex + 1 : toIndex;
    if (beginMoveRows(QModelIndex(), fromIndex, fromIndex, QModelIndex(), destination))
        endMoveRows();
    emit activeTabIndexChanged();
    // A reorder changes the {TabPosition} of every tab between the source and destination; refresh
    // all labels so positional templates renumber.
    refreshAllTabTitles();
}

void TerminalSessionManager::activeTabChanged(vtmux::WindowId, vtmux::TabId, int)
{
    emit activeTabIndexChanged();

    // The tab-strip highlight is painted per row from IsActiveRole (row == activeTabIndex()), not from
    // ListView.currentIndex, so the switch only becomes visible once the previously-active and
    // newly-active rows both re-read that role. activeTabIndexChanged() above remains the single
    // window-level focus-restore signal (main.qml); this is purely additive.
    refreshActiveTabHighlight();

    rebuildActiveTabPaneProxies();
}

void TerminalSessionManager::paneSplit(vtmux::TabId tab, vtmux::PaneId, vtmux::PaneId)
{
    notifyTabRowChanged(tab, { TitleRole, PaneCountRole });
    // Only the active tab's pane tree is bound to the QML; a split routed to a background tab (via a
    // keybinding's acting session) leaves the active tree untouched and is rebuilt when that tab is
    // next activated.
    if (isActiveTab(tab))
        rebuildActiveTabPaneProxies();
}

void TerminalSessionManager::paneClosed(vtmux::TabId tab, vtmux::PaneId, vtmux::PaneId)
{
    notifyTabRowChanged(tab, { TitleRole, PaneCountRole });
    if (isActiveTab(tab))
        rebuildActiveTabPaneProxies();
}

void TerminalSessionManager::activePaneChanged(vtmux::TabId tab, vtmux::PaneId)
{
    notifyTabRowChanged(tab, { TitleRole });
    // Update the active-pane focus borders without rebuilding the tree.
    for (auto const& [id, proxy]: _paneProxies)
        proxy->notifyActiveChanged();
    // The focused pane (and thus the active-pane session) changed, but the tree did not rebuild, so
    // activeTabRootPaneChanged does not fire. Notify activeSession consumers (e.g. the window title)
    // directly so window-level bindings follow the newly focused pane within the same tab.
    emit activeSessionChanged();
}

void TerminalSessionManager::paneRatioChanged(vtmux::TabId, vtmux::PaneId splitNode, double)
{
    // A model-driven ratio change (Snapshot restore, a programmatic setPaneRatio, or a future daemon
    // client) must reach the split node's proxy so PaneNode.qml's preferredWidth/Height binding —
    // which reads node.ratio with NOTIFY ratioChanged — re-evaluates and moves the divider. Without
    // this, only ratio changes that originate from the QML drag handle itself would be reflected.
    if (auto const it = _paneProxies.find(splitNode.value); it != _paneProxies.end())
        it->second->notifyRatioChanged();
}

void TerminalSessionManager::tabTitleChanged(vtmux::TabId tab)
{
    // A rename (or its reset) changes both the displayed label and the raw template the editor reads,
    // so refresh both roles. Without RawTitleRole here a re-opened editor would show a stale template.
    notifyTabRowChanged(tab, { TitleRole, RawTitleRole });
    // The indicator status line's {Tabs} entry is built from the same titles, so republish it too
    // (mirroring tabColorChanged); otherwise a renamed tab keeps its old name in the status line until
    // an unrelated event (color change, tab switch, move) next refreshes it.
    updateStatusLine();
}

void TerminalSessionManager::tabColorChanged(vtmux::TabId tab)
{
    notifyTabRowChanged(tab, { ColorRole });
    updateStatusLine();
}

void TerminalSessionManager::notifyTabRowChanged(vtmux::TabId tab, QList<int> const& roles)
{
    if (auto const row = rowOfTab(tab); row >= 0)
        emit dataChanged(index(row), index(row), roles);
}

void TerminalSessionManager::refreshActiveTabHighlight()
{
    if (auto const rows = rowCount(); rows > 0)
        emit dataChanged(index(0), index(rows - 1), { IsActiveRole });
}

void TerminalSessionManager::refreshAllTabTitles()
{
    if (auto const rows = rowCount(); rows > 0)
        emit dataChanged(index(0), index(rows - 1), { TitleRole });
}

void TerminalSessionManager::refreshTabForSession(vtmux::SessionId session)
{
    if (auto* tab = findTabHostingSession(session))
        notifyTabRowChanged(tab->id(), { TitleRole });
}

QString TerminalSessionManager::resolvedTabLabel(vtmux::Tab* tab, TerminalSession* session, int row) const
{
    // Without a tab (transient model states) fall back to the session's own title, matching the prior
    // behavior of the TitleRole accessor.
    if (tab == nullptr)
        return session != nullptr ? QString::fromStdString(session->name().value_or("")) : QString {};

    // {WindowTitle} is the RAW OS-window title, independent of TabsNamingMode. Do NOT route through
    // sessionTitleResolver() here: that resolver is shared with the indicator status line's {Tabs}
    // (Tab::title -> resolvedTabName), which gates the title on TabsNamingMode and yields nothing
    // under the default Indexing mode. Sourcing the placeholder from the session's raw title fixes the
    // empty {WindowTitle} under a default status line while leaving the status-line semantics intact.
    auto const windowTitle = session != nullptr ? session->resolvedWindowTitle() : std::string {};

    // Pick the template by precedence, then expand it once. All three sources are owned strings that
    // outlive this call, so binding a string_view over the chosen one is safe (expandTabLabel's parse
    // fragments borrow from it but never escape the call):
    //   1. a user rename — the rename string IS the template for this tab, so typing "{WindowTitle}"
    //      tracks the title while a plain rename like "deploy" (no placeholders) passes through;
    //   2. "Multiple panes" when the tab holds a split (no meaningful {WindowTitle} across panes);
    //   3. otherwise the active leaf's profile tab-label template (default "{WindowTitle}", which
    //      reproduces the pre-template behavior).
    auto templ = std::string_view {};
    if (auto const& renamed = tab->runtimeTitle(); renamed.has_value())
        templ = *renamed;
    else if (tab->hasMultiplePanes())
        templ = vtmux::Tab::MultiplePanesLabel;
    else if (session != nullptr)
        templ = session->profile().tabLabel.value();
    else
        // No rename, single pane, and no backing session (so no profile and no window title): nothing
        // to show. windowTitle is empty here, since it too is sourced from the (absent) session.
        return QString::fromStdString(windowTitle);

    return QString::fromStdString(
        expandTabLabel(templ, TabLabelContext { .position = row + 1, .windowTitle = windowTitle }));
}
// }}}

// {{{ GUI tab-strip invokables
void TerminalSessionManager::activateModelTabByRow(int row)
{
    auto* tab = tabAtRow(row);
    if (tab == nullptr || _modelWindow == nullptr)
        return;

    // Update the model's active tab FIRST so the rendered split tree (activeTabRootPane) and any
    // subsequent split/close/focus-pane operation — all of which resolve through activeModelTab() —
    // target the tab the user actually selected, not whichever tab was active at creation time.
    _model->activateTab(_modelWindow->id(), tab->id());

    // Then bring the display in sync by activating the session backing the tab's active leaf.
    if (auto* session = sessionForId(tab->activePane()->session()))
        activateSession(session);
}

void TerminalSessionManager::activateTab(int index)
{
    activateModelTabByRow(index);
}

void TerminalSessionManager::moveTab(int fromIndex, int toIndex)
{
    // fromIndex/toIndex are tab-strip rows (tab-space). _sessions is pane-space — it has one entry
    // per backing session, so once any tab is split it is longer than the tab list and indexing it
    // by a row erases/inserts the wrong pane, scrambling _sessions against the model. The tab order
    // is owned by the model (rowCount/data/updateStatusLine all read it), so reorder purely there
    // and bounds-check against the tab count.
    if (_modelWindow == nullptr)
        return;
    auto const tabCount = _modelWindow->tabCount();
    if (fromIndex < 0 || fromIndex >= tabCount || toIndex < 0 || toIndex >= tabCount)
        return;
    if (auto* tab = tabAtRow(fromIndex); tab != nullptr)
    {
        _model->moveTab(_modelWindow->id(), tab->id(), toIndex);
        updateStatusLine();
    }
}

void TerminalSessionManager::setTabTitle(int index, QString const& title)
{
    if (auto* tab = tabAtRow(index); tab != nullptr)
        _model->setTabTitle(tab->id(), title.toStdString());
}

void TerminalSessionManager::resetTabTitle(int index)
{
    if (auto* tab = tabAtRow(index); tab != nullptr)
        _model->resetTabTitle(tab->id());
}

void TerminalSessionManager::setTabColor(int index, QColor const& color)
{
    if (auto* tab = tabAtRow(index); tab != nullptr)
        _model->setTabColor(tab->id(), toRGBColor(color));
}

void TerminalSessionManager::resetTabColor(int index)
{
    if (auto* tab = tabAtRow(index); tab != nullptr)
        _model->resetTabColor(tab->id());
}

void TerminalSessionManager::closeTabAtIndex(int index)
{
    // index is a tab row (rows are tabs). Close the whole tab by terminating each of its panes'
    // sessions; the model collapses to the survivor on each close and tears the tab down with the
    // last pane.
    terminateSessions(sessionsInTab(tabAtRow(index)));
}

std::vector<TerminalSession*> TerminalSessionManager::sessionsInTab(vtmux::Tab* tab) const
{
    std::vector<TerminalSession*> sessions;
    if (tab == nullptr)
        return sessions;
    tab->rootPane()->walkTree([&](vtmux::Pane& pane) {
        if (pane.isLeaf())
            if (auto* session = sessionForId(pane.session()))
                sessions.push_back(session);
    });
    return sessions;
}

void TerminalSessionManager::terminateSessions(std::span<TerminalSession* const> sessions)
{
    // The single whole-tab close primitive shared by every close entry point. The caller has already
    // gathered the doomed sessions into a stable vector, so terminate() -> removeSession is free to
    // mutate the model tab tree (collapsing to the survivor and finally erasing the tab) without
    // invalidating what we iterate. terminate() now closes the PTY even for a display-less background
    // pane, so panes of a non-active tab are torn down too (previously a silent no-op that leaked the
    // session and its shell process).
    for (auto* session: sessions)
        session->terminate();
}

void TerminalSessionManager::closeOtherTabs(int index)
{
    auto* keep = tabAtRow(index);
    if (keep == nullptr || _modelWindow == nullptr)
        return;

    // Gather the backing sessions of every tab that will be closed BEFORE mutating the model, then
    // let the authoritative (and unit-tested) SessionModel::closeOtherTabs do the structural close,
    // and finally terminate the gathered sessions. terminate() -> removeSession then only cleans up
    // the Qt bookkeeping (the model tab is already gone), so we close in tab-space, not pane-space.
    std::vector<TerminalSession*> doomed;
    for (auto const row: std::views::iota(0, _modelWindow->tabCount()))
        if (auto* tab = _modelWindow->tabAt(row); tab != nullptr && tab->id() != keep->id())
            for (auto* session: sessionsInTab(tab))
                doomed.push_back(session);

    _model->closeOtherTabs(_modelWindow->id(), keep->id());
    terminateSessions(doomed);
}

void TerminalSessionManager::closeTabsToRight(int index)
{
    auto* anchor = tabAtRow(index);
    if (anchor == nullptr || _modelWindow == nullptr)
        return;

    // Same strategy as closeOtherTabs: collect the sessions of the tabs to the right, close them in
    // tab-space through the unit-tested model method, then terminate the gathered sessions.
    std::vector<TerminalSession*> doomed;
    for (auto const row: std::views::iota(index + 1, _modelWindow->tabCount()))
        if (auto* tab = _modelWindow->tabAt(row); tab != nullptr)
            for (auto* session: sessionsInTab(tab))
                doomed.push_back(session);

    _model->closeTabsToRight(_modelWindow->id(), anchor->id());
    terminateSessions(doomed);
}

QVariantList TerminalSessionManager::tabColorPalette() const
{
    // The palette is immutable for the model's lifetime; build the QVariantList once and cache it.
    if (_tabColorPaletteCache.isEmpty())
        for (auto const& color: _model->colorPalette())
            _tabColorPaletteCache.append(toQColor(color));
    return _tabColorPaletteCache;
}
// }}}

// {{{ Split-pane operations
void TerminalSessionManager::splitActivePane(bool vertical, TerminalSession* acting)
{
    auto* tab = paneActionTargetTab(acting);
    if (tab == nullptr)
        return;

    // Inherit the working directory of the pane we are splitting from. Use the session's own
    // accessor so the split pane inherits the cwd on every platform — the previous inline extraction
    // lacked the Windows currentWorkingDirectory() fallback, so a split pane silently did not inherit
    // the cwd on Windows while a new tab/window did.
    std::optional<std::string> cwd;
    if (auto* activeSession = sessionForId(tab->activePane()->session()))
        cwd = activeSession->workingDirectory();

    // Create the backing Qt session BEFORE the model split. _model->splitActivePane synchronously
    // fires paneSplit/activePaneChanged -> rebuildActiveTabPaneProxies, which binds the new pane's
    // QML to sessionForId(newSessionId). If the session were registered only afterwards (the old
    // order), that binding would resolve to nullptr and the pane would render permanently blank with
    // nothing re-notifying the proxy. createBackingSession registers the id and stages it as the
    // _pendingSessionId, so the model allocator (invoked inside splitActivePane) hands back exactly
    // this id — same backing-session-first order as createSessionInBackground/createTab.
    auto const newSessionId = vtmux::SessionId { _nextSessionId++ };
    createBackingSession(newSessionId, std::move(cwd));

    auto const direction = vertical ? vtmux::SplitState::Vertical : vtmux::SplitState::Horizontal;
    auto* newLeaf = _model->splitActivePane(tab->id(), direction);
    _pendingSessionId.reset(); // consumed by the allocator; clear any leftover
    if (newLeaf == nullptr)
    {
        // The split did not happen; the backing session we created has no model pane, so terminate
        // it to drop the orphaned session/registry entries rather than leak them.
        if (auto* orphan = sessionForId(newSessionId))
            orphan->terminate();
        return;
    }

    // The new leaf joins the same tab as the pane it split from.
    _tabBySession[newSessionId.value] = tab->id();
}

void TerminalSessionManager::closeActivePane(TerminalSession* acting)
{
    auto* tab = paneActionTargetTab(acting);
    if (tab == nullptr)
        return;
    auto* active = tab->activePane();
    if (active == nullptr)
        return;
    auto const sessionId = active->session();
    // Close the model pane FIRST, then terminate the backing session. terminate() fires
    // sessionClosed -> removeSession, but by then the leaf is already gone from the model, so
    // removeSession's findLeaf() returns nullptr and it does not close the pane a second time — it
    // only drops the local bookkeeping.
    _model->closePane(_modelWindow->id(), tab->id(), active->id());
    if (auto* session = sessionForId(sessionId))
        session->terminate();
}

void TerminalSessionManager::focusPane(vtmux::FocusDirection direction, TerminalSession* acting)
{
    if (auto* tab = paneActionTargetTab(acting))
        _model->focusDirection(tab->id(), direction);
}
// }}}

// {{{ PaneProxy support
TerminalSession* TerminalSessionManager::activeSession() const noexcept
{
    if (auto* tab = activeModelTab())
        if (auto* active = tab->activePane())
            if (auto* session = sessionForId(active->session()))
                return session;
    // Fallback to the active display's current session (legacy path).
    auto const it = _displayStates.find(_activeDisplay);
    return it != _displayStates.end() ? it->second.currentSession : nullptr;
}

bool TerminalSessionManager::isActivePane(vtmux::PaneId id) const noexcept
{
    if (auto* tab = activeModelTab())
        return tab->activePane() != nullptr && tab->activePane()->id() == id;
    return false;
}

void TerminalSessionManager::setPaneRatio(vtmux::PaneId id, double ratio)
{
    if (auto* tab = activeModelTab())
        _model->setPaneRatio(tab->id(), id, ratio);
}

void TerminalSessionManager::activatePane(vtmux::PaneId id)
{
    if (auto* tab = activeModelTab())
        _model->setActivePane(tab->id(), id);
}

void TerminalSessionManager::rebuildActiveTabPaneProxies()
{
    // Get-or-create a proxy for a PaneId, reusing existing proxies so a surviving pane's QML item
    // (and its ContourTerminal) is not torn down across a sibling split/close.
    auto getProxy = [this](vtmux::PaneId id) -> PaneProxy* {
        auto it = _paneProxies.find(id.value);
        if (it != _paneProxies.end())
            return it->second;
        auto* proxy = new PaneProxy(*this, id);
        QQmlEngine::setObjectOwnership(proxy, QQmlEngine::CppOwnership);
        _paneProxies[id.value] = proxy;
        return proxy;
    };

    vtmux::Tab* tab = _modelWindow != nullptr ? _modelWindow->activeTab() : nullptr;

    // Collect the set of live pane ids so we can prune proxies for panes that no longer exist.
    std::unordered_map<uint64_t, vtmux::Pane*> live;
    if (tab != nullptr)
        tab->rootPane()->walkTree([&](vtmux::Pane& p) { live[p.id().value] = &p; });

    // Wire up the cached backing pane + child pointers and emit change notifications for the
    // surviving proxies. Caching the Pane* here (we already hold it) lets every PaneProxy getter read
    // it directly instead of re-walking the tab tree on each QML binding evaluation.
    for (auto const& [idValue, p]: live)
    {
        auto* proxy = getProxy(vtmux::PaneId { idValue });
        proxy->setPane(p);
        if (p->isLeaf())
            proxy->setChildren(nullptr, nullptr);
        else
            proxy->setChildren(getProxy(p->first()->id()), getProxy(p->second()->id()));
        proxy->notifyChanged();
        proxy->notifyActiveChanged();
    }

    // Prune proxies whose panes are gone. Clear the cached pane first so a getter that runs before
    // deleteLater() takes effect sees no stale Pane*.
    for (auto it = _paneProxies.begin(); it != _paneProxies.end();)
    {
        if (!live.contains(it->first))
        {
            it->second->setPane(nullptr);
            it->second->deleteLater();
            it = _paneProxies.erase(it);
        }
        else
            ++it;
    }

    auto* newRoot = tab != nullptr ? getProxy(tab->rootPane()->id()) : nullptr;
    if (newRoot != _activeTabRootProxy)
    {
        _activeTabRootProxy = newRoot;
        emit activeTabRootPaneChanged();
        // The active tab/tree changed, so the active-pane session likely changed too; keep
        // activeSession consumers (window title) in sync on this path as well as on pane-focus changes.
        emit activeSessionChanged();
    }
}
// }}}

bool TerminalSessionManager::canCloseWindow() const noexcept
{
    auto const displayCount = std::count_if(
        _displayStates.begin(), _displayStates.end(), [](auto const& pair) { return pair.first != nullptr; });

    if (_sessions.size() >= static_cast<size_t>(displayCount))
    {
        managerLog()(
            "Cannot close window: there are {} sessions, and {} displays.", _sessions.size(), displayCount);
        // If there are more sessions than displays, we cannot close the window.
        return false;
    }

    return true;
}

} // namespace contour
