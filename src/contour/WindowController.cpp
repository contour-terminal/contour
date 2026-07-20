// SPDX-License-Identifier: Apache-2.0
#include <contour/ColorConversion.h>
#include <contour/ContextMenu.h>
#include <contour/ContextMenuModel.h>
#include <contour/ContourGuiApp.h>
#include <contour/GuiConfigStore.h>
#include <contour/PaneProxy.h>
#include <contour/SettingsController.h>
#include <contour/Shortcut.h>
#include <contour/TabColorScheme.h>
#include <contour/TabLabel.h>
#include <contour/TerminalSession.h>
#include <contour/TerminalSessionManager.h>
#include <contour/TitleBarContextMenu.h>
#include <contour/WindowController.h>
#include <contour/helper.h>

#include <QtCore/QDir>
#include <QtGui/QCursor>
#include <QtGui/QGuiApplication>
#include <QtGui/QScreen>
#include <QtQml/QQmlEngine>

#include <cstdlib>
#include <ranges>
#include <utility>

#include <vtmux/Pane.h>
#include <vtmux/PaneLayout.h>
#include <vtmux/SessionModel.h>
#include <vtmux/Tab.h>

namespace contour
{

WindowController::WindowController(TerminalSessionManager& manager, vtmux::WindowId windowId):
    _manager { manager },
    _windowId { windowId },
    // The palette's sources. They hold references into the app's Config, which outlives every window
    // and survives a config reload in place (the loaded values are assigned into it, the object is not
    // replaced), so these references stay valid for this controller's whole life.
    _tabCommands { *this },
    _profileCommands { manager.app().config() },
    _layoutCommands { manager.app().config() },
    _boundCommands { manager.app().config().inputMappings.value() },
    _commandPalette { std::make_unique<CommandPaletteModel>(manager.commandHistory(), this) }
{
    _commandPalette->setSources(
        { &_tabCommands, &_profileCommands, &_layoutCommands, &_boundCommands, &_actionCommands });

    // The editable settings bridge. Its collaborators are injected so the whole workflow is testable:
    // a config accessor into the app's live (reload-in-place) Config, a file-backed side-file store
    // rooted at the config directory, and an apply step that reloads every session after a save.
    _settingsController = std::make_unique<SettingsController>(
        [this]() -> config::Config const& { return _manager.app().config(); },
        std::make_shared<FileGuiConfigStore>(_manager.app().config().configFile.parent_path()),
        [this]() {
            _manager.reloadAllSessions();
            // Re-apply the GUI chrome theme live: a settings-page change to `theme` is already in the
            // reloaded Config, so the chrome recolors without a restart.
            _manager.app().applyGuiTheme(_manager.app().config().theme.value());
        },
        this);
}

WindowController::~WindowController() = default;

void WindowController::openCommandPalette()
{
    // Re-read the bindings on every open, not once at construction: a ReloadConfig can have rebound a
    // key since, and a shortcut column that advertises the OLD chord is worse than none at all.
    _commandPalette->setShortcuts(shortcutIndex(_manager.app().config().inputMappings.value()));

    // Re-queries the sources (so the tab rows track the tabs that exist now) and clears any filter the
    // last open left behind.
    _commandPalette->refresh();

    emit commandPaletteRequested();
}

void WindowController::openSettings()
{
    // Rebuild the models against the current config on every open, so a config reload since the last
    // visit is reflected — the same freshness discipline openCommandPalette() uses.
    _settingsController->refresh();
    setSettingsActive(true);
}

void WindowController::closeSettings()
{
    setSettingsActive(false);
}

void WindowController::toggleSettings()
{
    setSettingsActive(!_settingsActive);
}

void WindowController::setSettingsActive(bool active)
{
    if (_settingsActive == active)
        return;
    _settingsActive = active;
    emit settingsActiveChanged();
    // Toggling the settings view flips whether a terminal tab reads as active (see IsActiveRole), so
    // repaint the strip: the active tab de-highlights on open and re-highlights on close.
    refreshActiveTabHighlight();
}

void WindowController::openContextMenu()
{
    auto* session = activeSession();
    if (session == nullptr)
        return;

    // The manager has already made the right-clicked pane active, so this IS the pane the user clicked.
    auto state = session->contextMenuState();

    // The one fact the session cannot know: whether its tab holds more than one pane. That belongs to the
    // window, so it is filled in here rather than reached for from the session.
    auto const* tab = activeModelTab();
    state.hasSplits = tab != nullptr && tab->hasMultiplePanes();

    auto const entries = buildContextMenu(state);

    _paneContextMenu.publish(entries);

    // The pane the rows were built for. Held, so that picking one acts on it and not on whichever pane is
    // active by the time the click lands.
    _contextMenuSession = session;

    // Model first, then the request to show: both are synchronous, so the QML has rebuilt the menu's rows
    // by the time it is told to pop it.
    emit contextMenuModelChanged();
    emit contextMenuRequested();
}

void WindowController::triggerContextMenuAction(int actionId)
{
    // The session the menu was OPENED over, not whichever one is active now. Null when that pane has since
    // died — its shell exited while the menu stood open — in which case the row has nothing left to act on.
    auto* session = _contextMenuSession.data();
    if (session == nullptr)
        return;

    // Copied out of the model before running, for the same reason runCommand() does: an action can
    // rebuild this window (ClosePane, ChangeProfile) and free the vector it was standing in.
    auto const action = _paneContextMenu.actionAt(actionId);
    if (!action)
        return;

    // Deliberately NOT recorded in the command history: that list is "commands the user reached for by
    // name", and it exists to float them to the top of the palette. A right-click on "Copy" is not that.
    session->executeAction(*action);
}

void WindowController::openTitleBarContextMenu()
{
    auto state = TitleBarContextMenuState {
        .tabCount = count(),
        // The WINDOW's live modes, not the configuration's: these rows set a runtime override, so a
        // window changed since startup must show what it is actually doing.
        .tabBarVisibility = _tabBarVisibility,
        .tabBarPosition = _tabBarPosition,
        .activeProfile = {},
        .profileNames = {},
    };

    if (auto* session = activeSession())
        state.activeProfile = session->profileName();

    for (auto const& [name, _]: _manager.app().config().profiles.value())
        state.profileNames.push_back(name);
    std::ranges::sort(state.profileNames);

    _titleBarContextMenu.publish(buildTitleBarContextMenu(state));

    // Model first, then the request to show: both are synchronous, so QML has rebuilt the rows by the
    // time it is told to pop them.
    emit titleBarContextMenuModelChanged();
    emit titleBarContextMenuRequested();
}

void WindowController::triggerTitleBarContextMenuAction(int actionId)
{
    auto const action = _titleBarContextMenu.actionAt(actionId);
    if (!action)
        return;

    // Runs against the window's ACTIVE session, unlike the pane menu: every row here is window-scoped
    // (a new tab, this window's tab bar), so there is no particular pane it was opened over.
    if (auto* session = activeSession())
        session->executeAction(*action);
}

void WindowController::runCommand(QString const& id)
{
    auto* session = activeSession();
    if (session == nullptr)
        return;

    // Resolve against what the model currently offers. A command can vanish between the refresh that
    // drew the list and the click that picks it (a tab closes from under the palette), so a stale id
    // must be a no-op rather than a lookup into a dangling row.
    auto const* target = _commandPalette->commandById(id.toStdString());
    if (target == nullptr)
        return;

    // Copy the action out before running it. Several actions (CloseTab, ClosePane, ChangeProfile) reach
    // back through the manager and can rebuild this window — which refreshes the palette and frees the
    // very row `target` points into. Executing through the copy keeps this safe whatever the action does.
    auto const action = target->action;

    // Record BEFORE running, for the same reason: the action may close this pane, this tab, or the whole
    // application (Quit), and a command that quits must still be remembered as the last one used.
    _manager.recordCommand(target->id);
    session->executeAction(action);
}

std::vector<std::string> WindowController::tabTitles() const
{
    auto titles = std::vector<std::string> {};
    auto const tabCount = count();
    titles.reserve(static_cast<std::size_t>(tabCount));

    // Read the titles back through the model's own TitleRole rather than re-deriving them: the strip's
    // label precedence (runtime rename > pane title > ...) lives in resolvedTabLabel(), and a second
    // implementation here would eventually disagree with what the user sees on the tab.
    for (auto const row: std::views::iota(0, tabCount))
        titles.push_back(data(index(row, 0), static_cast<int>(Roles::TitleRole)).toString().toStdString());

    return titles;
}

vtmux::Window* WindowController::window() const noexcept
{
    // Resolve the backing vtmux::Window strictly by this controller's own WindowId, so each controller
    // adapts exactly its window (correct for N windows). For the first controller this is the same object
    // the manager's ctor minted as _modelWindow.
    return _manager.model().window(_windowId);
}

vtmux::Tab* WindowController::activeModelTab() const noexcept
{
    auto* win = window();
    return win != nullptr ? win->activeTab() : nullptr;
}

vtmux::Tab* WindowController::tabAtRow(int index) const noexcept
{
    auto* win = window();
    return win != nullptr ? win->tabAt(index) : nullptr;
}

int WindowController::rowOfTab(vtmux::TabId tab) const noexcept
{
    auto* win = window();
    return win != nullptr ? win->indexOf(tab) : -1;
}

// {{{ QAbstractListModel
QHash<int, QByteArray> WindowController::roleNames() const
{
    return {
        { static_cast<int>(Roles::DisplayRole), "display" },
        { static_cast<int>(Roles::TitleRole), "title" },
        { static_cast<int>(Roles::ColorRole), "accentColor" },
        { static_cast<int>(Roles::IsActiveRole), "isActive" },
        { static_cast<int>(Roles::PaneCountRole), "paneCount" },
        { static_cast<int>(Roles::SessionIdRole), "sessionId" },
        { static_cast<int>(Roles::RawTitleRole), "rawTitle" },
        { static_cast<int>(Roles::ZoomedRole), "zoomed" },
    };
}

QVariant WindowController::data(QModelIndex const& index, int role) const
{
    auto const row = index.row();
    if (row < 0 || row >= rowCount())
        return {};

    auto* tab = tabAtRow(row);
    auto* session = tab != nullptr ? _manager.sessionForId(tab->activePane()->session()) : nullptr;

    switch (static_cast<Roles>(role))
    {
        case Roles::DisplayRole:
        case Roles::SessionIdRole: return session != nullptr ? QVariant(session->id()) : QVariant {};
        case Roles::TitleRole: return resolvedTabLabel(tab, session, row);
        case Roles::RawTitleRole:
            return tab != nullptr ? QString::fromStdString(tab->runtimeTitle().value_or("")) : QString {};
        case Roles::ColorRole: {
            // color() resolves across the tab's color sources, so ask once.
            auto const color = tab != nullptr ? tab->color() : std::nullopt;
            return color.has_value() ? toQColor(*color) : QColor(Qt::transparent);
        }
        // No terminal tab reads as active while the settings "tab" is showing — the settings tab is the
        // active view then, so the strip must not highlight two tabs at once.
        case Roles::IsActiveRole: return !_settingsActive && row == activeTabIndex();
        case Roles::PaneCountRole: return tab != nullptr ? tab->paneCount() : 1;
        case Roles::ZoomedRole: return tab != nullptr && tab->isZoomed();
        default: return {};
    }
}

int WindowController::rowCount(QModelIndex const& parent) const
{
    Q_UNUSED(parent);
    auto* win = window();
    return win != nullptr ? win->tabCount() : 0;
}
// }}}

int WindowController::count() const noexcept
{
    auto* win = window();
    return win != nullptr ? win->tabCount() : 0;
}

int WindowController::activeTabIndex() const noexcept
{
    auto* win = window();
    return win != nullptr ? win->activeTabIndex() : -1;
}

QString WindowController::resolvedTabLabel(vtmux::Tab* tab, TerminalSession* session, int row) const
{
    if (tab == nullptr)
        return session != nullptr ? QString::fromStdString(session->name().value_or("")) : QString {};

    auto const windowTitle = session != nullptr ? session->resolvedWindowTitle() : std::string {};

    // Same precedence as vtmux::Tab::title(), templated rather than resolved (the strip expands
    // {index}/{title} placeholders, so it needs the raw template). The "is this tab named after one of
    // its panes?" decision is Tab's — read it, do not restate it, or the strip and the status line end
    // up disagreeing about what a zoomed tab is called.
    auto templ = std::string_view {};
    if (auto const& renamed = tab->runtimeTitle(); renamed.has_value())
        templ = *renamed;
    else if (tab->usesMultiplePanesLabel())
        templ = vtmux::Tab::MultiplePanesLabel;
    else if (session != nullptr)
        templ = session->profile().tabLabel.value();
    else
        return QString::fromStdString(windowTitle);

    return QString::fromStdString(
        expandTabLabel(templ, TabLabelContext { .position = row + 1, .windowTitle = windowTitle }));
}

// {{{ Tab-strip invokables — structural/session-lifetime ops delegate to the manager, tagged with
// THIS window's id, so the same operation issued from any OS window targets that window's tabs.
// Pure per-tab attributes (title, color) resolve the tab locally and write the model directly.
void WindowController::createNewTab(QString const& profileName)
{
    // A new terminal tab is a terminal view: leave the settings page so the content area shows it.
    setSettingsActive(false);
    auto profile = profileName.isEmpty() ? std::optional<std::string> { std::nullopt }
                                         : std::optional<std::string> { profileName.toStdString() };
    _manager.createNewTab(_windowId, profile);
}

void WindowController::activateTab(int index)
{
    // Selecting a tab returns to the terminal content — the settings page and the tabs are mutually
    // exclusive views of the same region, so activating any tab dismisses the settings page.
    setSettingsActive(false);
    _manager.activateTab(_windowId, index);
}

QString WindowController::tabWorkingDirectory(int index) const
{
    auto* tab = tabAtRow(index);
    if (tab == nullptr)
        return {};

    auto* session = _manager.sessionForId(tab->activePane()->session());
    if (session == nullptr)
        return {};

    // Abbreviated as a shell prompt writes it. Purely presentational, which is why it happens here and
    // not at the source: a caller that wants a path to act on wants the real one.
    return QString::fromStdString(
        abbreviateHomePath(session->displayWorkingDirectory(), QDir::homePath().toStdString()));
}

void WindowController::dispatchTabStripWheel(
    int pixelDeltaX, int pixelDeltaY, int angleDeltaX, int angleDeltaY, int phase, bool inverted)
{
    auto* session = activeSession();
    if (session == nullptr)
        return;

    auto const pixelDelta = crispy::point { .x = pixelDeltaX, .y = pixelDeltaY };
    auto const angleDelta = crispy::point { .x = angleDeltaX, .y = angleDeltaY };
    // Translated, not cast: QML can only hand the phase across as an int, and the two enumerations
    // agreeing numerically today is a coincidence rather than a contract.
    auto const scrollPhase = mapScrollPhase(static_cast<Qt::ScrollPhase>(phase));

    if (!_tabStripWheelGesture.acceptsHorizontal(pixelDelta, angleDelta, scrollPhase, inverted))
        return;

    // A pixel-precise swipe never reaches a notch. The accumulator below counts ANGLE units, of which a
    // trackpad produces none, so measuring a swipe against it would silently do nothing at all — one
    // swipe is simply one step, which consumeNavigationStep() below already guarantees.
    if (pixelDelta.x == 0)
    {
        // One NOTCH is one tab. Deliberately NOT the 40-unit step consumeScroll() uses: that one
        // quantizes continuous scrolling and a notch is three of them, so borrowing it would walk three
        // tabs per detent.
        auto constexpr AngleUnitsPerNotch = 120;
        _tabStripWheelAccumulator += angleDeltaX;
        auto const notches = _tabStripWheelAccumulator / AngleUnitsPerNotch;
        if (notches == 0)
            return;
        _tabStripWheelAccumulator -= notches * AngleUnitsPerNotch;
    }

    // One switch per event even when several notches arrive coalesced: a gesture is a unit of intent, and
    // it moves one tab, as a browser does with the same swipe.
    if (!_tabStripWheelGesture.consumeNavigationStep())
        return;

    // Same rule as the terminal view's navigation gate, from the same function: a swipe follows the
    // fingers, a wheel tilt is taken literally. Nothing here feeds mouse reporting, so it is resolved at
    // the call rather than after a decline.
    auto const towardsRight = (pixelDelta.x != 0 ? pixelDelta.x : angleDeltaX) > 0;
    session->applyFallbackMouseBinding(
        horizontalNavigationButton(towardsRight, _tabStripWheelGesture.usesNaturalDirection()));
}

void WindowController::moveTab(int fromIndex, int toIndex)
{
    _manager.moveTab(_windowId, fromIndex, toIndex);
}

void WindowController::moveTabIntoThisWindow(quint64 sourceWindowId, int fromIndex, int toIndex)
{
    _manager.moveTabToWindow(vtmux::WindowId { sourceWindowId }, fromIndex, _windowId, toIndex);
}

void WindowController::tearOffTab(int index)
{
    // Open the new window on this window's current screen (the best pre-show DPR predictor), matching
    // how spawning a new terminal picks the spawning window's screen.
    _manager.tearOffTabToNewWindow(_windowId, index, _osWindow != nullptr ? _osWindow->screen() : nullptr);
}

void WindowController::setTabTitle(int index, QString const& title)
{
    if (auto* tab = tabAtRow(index); tab != nullptr)
        _manager.model().setTabTitle(tab->id(), title.toStdString());
}

void WindowController::resetTabTitle(int index)
{
    if (auto* tab = tabAtRow(index); tab != nullptr)
        _manager.model().resetTabTitle(tab->id());
}

void WindowController::beginActiveTabTitleEdit()
{
    auto const index = activeTabIndex();
    if (index < 0)
        return;
    emit tabTitleEditRequested(index);
}

void WindowController::beginActiveTabColorPick()
{
    auto const index = activeTabIndex();
    if (index < 0)
        return;
    emit tabColorPickRequested(index);
}

void WindowController::beginSaveLayoutPrompt()
{
    // Window-scoped, not tab-scoped: the layout is the whole window's tab set, so unlike the tab-title /
    // tab-color prompts there is no row to name — the dialog just opens over this window.
    emit saveLayoutRequested();
}

void WindowController::saveLayoutAs(QString const& name)
{
    // A blank prompt must never write a nameless layout (see the header). Trim first, so trailing
    // whitespace neither passes the guard nor lands in the saved key.
    auto const trimmed = name.trimmed();
    if (trimmed.isEmpty())
        return;
    // Route through the same manager entry point the SaveLayout keybinding uses, targeting THIS window's
    // active session — saveLayout() resolves the hosting window from it and serializes that window.
    if (auto* session = activeSession(); session != nullptr)
        _manager.saveLayout(trimmed.toStdString(), session);
}

// Both of these route through the row-based setters the color flyout already uses, so the keyboard and
// the mouse write the identical TabColorSource::User slot. With no active tab the row is -1, which
// tabAtRow() bounds-checks into a null tab — the no-op the header promises, without a second guard.
void WindowController::setActiveTabColor(vtbackend::RGBColor color)
{
    setTabColor(activeTabIndex(), toQColor(color));
}

void WindowController::resetActiveTabColor()
{
    resetTabColor(activeTabIndex());
}

void WindowController::setTabColor(int index, QColor const& color)
{
    if (auto* tab = tabAtRow(index); tab != nullptr)
        _manager.model().setTabColor(tab->id(), vtmux::TabColorSource::User, toRGBColor(color));
}

void WindowController::resetTabColor(int index)
{
    // Clears only the user's own choice. If the application assigned a color via DECAC, the tab falls
    // back to it; otherwise to the host default. "Default" therefore means "whatever I did not choose".
    if (auto* tab = tabAtRow(index); tab != nullptr)
        _manager.model().resetTabColor(tab->id(), vtmux::TabColorSource::User);
}

void WindowController::closeTabAtIndex(int index)
{
    _manager.closeTabAtIndex(_windowId, index);
}

void WindowController::closeOtherTabs(int index)
{
    _manager.closeOtherTabs(_windowId, index);
}

void WindowController::closeTabsToRight(int index)
{
    _manager.closeTabsToRight(_windowId, index);
}

QVariantList WindowController::tabColorPalette() const
{
    return _manager.tabColorPalette();
}

namespace
{
    /// Maps the two focus axes (tab-active, window-active) plus hover to a single tab visual state.
    [[nodiscard]] TabVisualState tabVisualStateFor(bool active, bool hovered, bool windowActive) noexcept
    {
        if (active)
            return TabVisualState::Active;
        if (hovered)
            return TabVisualState::Hover;
        return windowActive ? TabVisualState::Inactive : TabVisualState::InactiveWindowUnfocused;
    }
} // namespace

QColor WindowController::tabBackgroundColor(QColor const& tabColor,
                                            QColor const& rowBackground,
                                            bool const active,
                                            bool const hovered,
                                            bool const windowActive) const
{
    auto const state = tabVisualStateFor(active, hovered, windowActive);
    return toQColor(contour::tabBackgroundColor(toRGBColor(tabColor), toRGBColor(rowBackground), state));
}

QColor WindowController::tabTextColor(QColor const& tabBackground) const
{
    return toQColor(contrastingTextColor(toRGBColor(tabBackground)));
}

void WindowController::closeWindow()
{
    // Re-entry guard: the _osWindow->close() at the end fires QML's `onClosing`, which calls back into
    // closeWindow(). Run the teardown exactly once regardless of which door started the close (the user
    // clicking the window's close button -> onClosing -> here, OR a programmatic close of a window whose
    // last tab was dragged away -> here -> close() -> onClosing -> here again).
    if (_closing)
        return;
    _closing = true;

    // Tear down THIS OS window on real per-window identity (no _displayStates / QQuickWindow grouping).
    // Ordering matters: gather this window's sessions BEFORE removing the model window, remove the model
    // window (which fires tabClosed -> onTabClosed row removal), explicitly prune our own PaneProxy tree
    // (removeWindow does NOT fire activeTabChanged for the final index-0 tab, so the rebuild that would
    // prune the tree never runs — clear it here or _activeTabRootProxy keeps a cached Pane* into the
    // just-freed model tree), then terminate the gathered sessions (their sessionClosed -> removeSession
    // degrades to local registry cleanup because their tabs are already gone), and finally close the OS
    // window and ask the manager to drop this controller.
    auto* win = window();
    std::vector<TerminalSession*> doomed;
    if (win != nullptr)
        for (auto const row: std::views::iota(0, win->tabCount()))
            if (auto* tab = win->tabAt(row); tab != nullptr)
                for (auto* session: _manager.sessionsOfTab(tab))
                    doomed.push_back(session);

    _manager.model().removeWindow(_windowId);

    // Prune our PaneProxy tree explicitly (see above): clear each cached Pane* first so any getter that
    // runs before deleteLater() takes effect sees no stale pointer, then drop the root.
    for (auto const& [id, proxy]: _paneProxies)
    {
        proxy->setPane(nullptr, vtmux::TabId {});
        proxy->deleteLater();
    }
    _paneProxies.clear();
    if (_activeTabRootProxy != nullptr)
    {
        _activeTabRootProxy = nullptr;
        emit activeTabRootPaneChanged();
    }

    _manager.terminate(doomed);

    // Close the OS window itself. When the user clicked the close button, the QQuickWindow is already
    // closing and this is a harmless no-op; when we got here programmatically (an emptied window after a
    // tab was dragged out), this is what actually removes the lingering window — otherwise its title bar
    // / tab strip stays on screen as a ghost. The re-entrant onClosing -> closeWindow() is absorbed by
    // the _closing guard above.
    if (_osWindow != nullptr)
        _osWindow->close();

    // Drop this controller from the manager registry (and, if it was the last window, the manager clears
    // its shared session registries). Must be last — it deleteLater()s this object.
    _manager.removeWindowController(_windowId);
}

bool WindowController::canCloseWindow() const noexcept
{
    // Per-window: the window may close only when THIS window has no remaining pane sessions. onTerminated
    // runs AFTER removeSession has erased the closing pane's session and collapsed its split, so an empty
    // session set for this window means that was its last pane; while any pane remains the split just
    // collapsed and the window stays open. Counting only THIS window's sessions (not all sessions) is what
    // lets the last pane of window A close A while window B still has sessions.
    auto* win = window();
    if (win == nullptr)
        return true;
    for (auto const row: std::views::iota(0, win->tabCount()))
        if (auto* tab = win->tabAt(row); tab != nullptr)
            if (!_manager.sessionsOfTab(tab).empty())
                return false;
    return true;
}
// }}}

// {{{ Window-service reads
TerminalSession* WindowController::activeSession() const noexcept
{
    if (auto* tab = activeModelTab())
        if (auto* active = tab->activePane())
            if (auto* session = _manager.sessionForId(active->session()))
                return session;
    return nullptr;
}

bool WindowController::titleBarVisible() const noexcept
{
    return _titleBarVisible;
}

void WindowController::seedTitleBarVisible(bool visible)
{
    // First-write-wins: the seed arrives from TerminalDisplay::setSession/handleWindowChanged on EVERY
    // session rebind (tab switch, split collapse), but only the first one carries the window's initial
    // profile value — later ones must not reset a runtime ToggleTitleBar.
    if (_titleBarSeeded)
        return;
    _titleBarSeeded = true;
    setTitleBarVisible(visible);
}

void WindowController::setTitleBarVisible(bool visible)
{
    // Apply the native window-frame decoration unconditionally (not only on change) so a seed
    // re-asserts the frame on a freshly adopted window even when the value already matched: shown =>
    // keep the native frame, hidden => frameless so the custom client-side TitleBar is the only
    // decoration. This is the C++ counterpart of main.qml's `flags` binding (both keyed on the same
    // titleBarVisible value); main.qml drops our custom min/max/close controls whenever the native
    // frame shows, so the two decorations never stack.
    if (_osWindow != nullptr)
        _osWindow->setFlag(Qt::FramelessWindowHint, !visible);

    if (_titleBarVisible == visible)
        return;
    _titleBarVisible = visible;
    emit titleBarVisibleChanged();
}

void WindowController::toggleTitleBar()
{
    // Under client-side decoration the title bar is our custom QML TitleBar, not the OS frame; toggling
    // flips the custom bar's visibility and the window stays frameless. On macOS (native frame kept)
    // setTitleBarVisible() drives the frame itself. Window-scoped: a toggle from any pane flips the
    // whole window and survives pane-focus changes and tab switches.
    setTitleBarVisible(!_titleBarVisible);
}

int WindowController::tabBarPosition() const noexcept
{
    return static_cast<int>(_tabBarPosition);
}

int WindowController::tabBarVisibility() const noexcept
{
    return static_cast<int>(_tabBarVisibility);
}

bool WindowController::tabBarShouldShow() const noexcept
{
    switch (_tabBarVisibility)
    {
        case config::TabBarVisibility::Always: return true;
        case config::TabBarVisibility::Never: return false;
        case config::TabBarVisibility::Multiple: return count() > 1;
    }
    return true;
}

void WindowController::setTabBarVisibility(config::TabBarVisibility visibility)
{
    _tabBarVisibilitySeeded = true;
    if (_tabBarVisibility == visibility)
        return;
    _tabBarVisibility = visibility;
    emit tabBarVisibilityChanged();
    // The mode is one of the two inputs to the resolved gate, so QML must re-evaluate it.
    emit tabBarShouldShowChanged();
}

void WindowController::setTabBarPosition(config::TabBarPosition position)
{
    _tabBarPositionSeeded = true;
    if (_tabBarPosition == position)
        return;
    _tabBarPosition = position;
    emit tabBarPositionChanged();
}

void WindowController::applyTabBarFromConfig(config::TabBarPosition position,
                                             config::TabBarVisibility visibility)
{
    // Deliberately bypasses the seed latches rather than reusing them: a reload is the one moment the
    // configured value must win over whatever this window is currently showing. Mark both as seeded so
    // a later session rebind still does not clobber what was just applied.
    _tabBarPositionSeeded = true;
    if (_tabBarPosition != position)
    {
        _tabBarPosition = position;
        emit tabBarPositionChanged();
    }

    _tabBarVisibilitySeeded = true;
    if (_tabBarVisibility != visibility)
    {
        _tabBarVisibility = visibility;
        emit tabBarVisibilityChanged();
        emit tabBarShouldShowChanged();
    }
}

void WindowController::seedTabBarPosition(config::TabBarPosition position)
{
    // First-write-wins, mirroring seedTitleBarVisible: the seed arrives on every session rebind, but
    // only the first (the window's initial configured value) takes effect.
    if (_tabBarPositionSeeded)
        return;
    _tabBarPositionSeeded = true;
    if (_tabBarPosition == position)
        return;
    _tabBarPosition = position;
    emit tabBarPositionChanged();
}

void WindowController::seedTabBarVisibility(config::TabBarVisibility visibility)
{
    // First-write-wins (see seedTabBarPosition).
    if (_tabBarVisibilitySeeded)
        return;
    _tabBarVisibilitySeeded = true;
    if (_tabBarVisibility == visibility)
        return;
    _tabBarVisibility = visibility;
    emit tabBarVisibilityChanged();
    // The visibility mode is one of the two inputs to the resolved gate; notify QML so a Never/Multiple
    // seed re-evaluates the tab strip's `visible` binding immediately (Always is the default, so a
    // window seeded to Always sees no change).
    emit tabBarShouldShowChanged();
}

bool WindowController::isMultimediaReady() const noexcept
{
    return _manager.isMultimediaReady();
}
// }}}

void WindowController::focusDisplay(display::TerminalDisplay* display)
{
    // No per-display state bridging: the window services (titleBarVisible & friends) are controller-owned
    // window state now, so they neither change nor need re-notifying when the focused pane changes.
    _activeDisplay = display;

    // Adopt this display's OS window the first time one focuses in, so the manager can later route a
    // focus/close for that window to this controller (ownsOSWindow). (bindWindow() normally already did
    // this at spawn; this covers displays re-homed across windows.)
    if (display != nullptr && display->window() != nullptr)
        _osWindow = display->window();
}

// {{{ Window-geometry authority
void WindowController::bindWindow(QQuickWindow* osWindow)
{
    if (osWindow == nullptr)
        return;

    _osWindow = osWindow;

    // Assign the pre-show target screen — the DPR predictor for the headless cell metrics. Order:
    // the spawning window's screen (staged by ContourGuiApp::newWindow), else the screen under the
    // cursor (where WMs place new windows; not globally meaningful on Wayland), else Qt's default
    // (the primary screen).
    auto* target = _manager.app().takePendingSpawnScreen();
    if (target == nullptr && QGuiApplication::platformName() != QStringLiteral("wayland"))
        target = QGuiApplication::screenAt(QCursor::pos());
    if (target != nullptr)
        osWindow->setScreen(target);

    // Scale-settlement hooks: QEvent::DevicePixelRatioChange is the only reliable per-window DPR
    // signal (it is how a late Wayland fractional scale or a KDE integer->fractional correction
    // arrives); screenChanged covers mixed-DPR monitor moves.
    osWindow->installEventFilter(this);
    connect(osWindow,
            &QWindow::screenChanged,
            this,
            &WindowController::onWindowScaleMaybeChanged,
            Qt::UniqueConnection);

    // OS window activation (alt-tab, clicking another application) moves terminal focus, so a shell
    // running under DECSET 1004 is told the window went away. Item-level Qt focus events usually cover
    // this, but not for a window whose panes hold no focused display -- and the manager, not Qt, is the
    // focus authority. A member slot, not a lambda: Qt::UniqueConnection only applies to member
    // functions, and bindWindow may run more than once for the same window.
    connect(osWindow,
            &QWindow::activeChanged,
            this,
            &WindowController::onOSWindowActiveChanged,
            Qt::UniqueConnection);
}

void WindowController::onOSWindowActiveChanged()
{
    // Read the window that RAISED the signal, not _osWindow: focusDisplay() re-points _osWindow for a
    // display re-homed across windows, so the member and the event's subject can diverge.
    auto const* osWindow = qobject_cast<QWindow*>(sender());
    if (osWindow == nullptr)
        return;

    if (osWindow->isActive())
        _manager.setFocusedWindow(_windowId);
    else
        _manager.clearFocusedWindow(_windowId);
}

void WindowController::showInitial()
{
    if (_osWindow == nullptr)
        return;

    auto* session = activeSession();
    auto* display = session != nullptr ? session->display() : nullptr;

    if (display == nullptr || !display->hasSession())
    {
        // Never leave the user windowless: map at whatever size the window has.
        displayLog()("showInitial: no display metrics available; showing at default size.");
        _osWindow->show();
        return;
    }

    // Size the still-unmapped window from the REAL cell metrics (headless FreeType math, done during
    // session attach) at the target screen's resolved scale, so the first map is the final geometry.
    auto const scale = display->contentScale();
    auto const marginsDevice = geometry::scaled(toGeometryMargins(session->profile().margins.value()), scale);
    auto const totalPage = session->terminal().totalPageSize();
    auto const size =
        geometry::windowSizeForPage(totalPage, display->cellSize(), marginsDevice, scale, chrome());

    displayLog()("Initial window size {}x{} (page {}, cell {}, scale {}, chrome {}).",
                 size.width,
                 size.height,
                 totalPage,
                 display->cellSize(),
                 scale,
                 _chromeHeight);
    _osWindow->resize(size.width, size.height);
    _lastAppliedScale = scale;

    // First map lands directly in the profile's window state (no post-map normal->maximized jump).
    if (session->profile().fullscreen.value())
        setWindowFullScreen(*display);
    else if (session->profile().maximized.value())
        setWindowMaximized(*display);
    else
        setWindowNormal(*display);
}

bool WindowController::eventFilter(QObject* watched, QEvent* event)
{
    if (watched == _osWindow && event->type() == QEvent::DevicePixelRatioChange)
        onWindowScaleMaybeChanged();
    return QAbstractListModel::eventFilter(watched, event);
}

void WindowController::onWindowScaleMaybeChanged()
{
    auto* session = activeSession();
    auto* display = session != nullptr ? session->display() : nullptr;
    if (display == nullptr || !display->hasSession())
        return;

    auto const newScale = display->contentScale();
    if (newScale == _lastAppliedScale)
        return; // idempotence latch: absorbs signal storms and self-induced echoes
    _lastAppliedScale = newScale;

    auto const pageBefore = session->terminal().totalPageSize();
    displayLog()("Content scale settled to {} (page {}).", newScale, pageBefore);

    // New scale => new font DPI => new cell size; this reflows the grid in place (unconditionally
    // correct: the grid always fits the item) and guarantees cellSize() is fresh even when the scale
    // settles before the first frame (no render target yet) — the corrective resize below must not
    // compute from stale metrics.
    display->applyContentScaleChange();

    // Grid-preserving settlement: ONE corrective window resize restoring the page at the new cell
    // size — cells scale uniformly across all panes of the window, so fixed split ratios preserve
    // every pane's grid. Skipped while our own content-driven resize is in flight (a window
    // straddling mixed-DPR monitors could otherwise answer its own resize forever) and in
    // fullscreen/maximized (never resize those; the reflow stands).
    if (_inContentDrivenResize)
        return;
    if (_osWindow != nullptr && _osWindow->visibility() == QQuickWindow::Visibility::Windowed)
        resizeWindowForPage(*display, pageBefore);
}

QQuickWindow* WindowController::osWindowFor(display::TerminalDisplay& requester) noexcept
{
    auto* requesterWindow = requester.window();
    if (_osWindow == nullptr && requesterWindow != nullptr)
        _osWindow = requesterWindow;
    return requesterWindow != nullptr ? requesterWindow : _osWindow;
}

void WindowController::setChromeHeight(int height)
{
    if (_chromeHeight == height)
        return;
    _chromeHeight = height;
    emit chromeHeightChanged();
    if (_activeDisplay != nullptr)
        updateSizeHintsFor(*_activeDisplay);
}

void WindowController::updateSizeHintsFor(display::TerminalDisplay& requester, HintApplyMode mode)
{
    auto* osWindow = osWindowFor(requester);
    if (osWindow == nullptr || !requester.hasSession())
        return;

    auto const scale = requester.contentScale();
    auto const cellSize = requester.cellSize();
    auto const margins = toGeometryMargins(requester.session().profile().margins.value());
    auto const hints = geometry::sizeHintsFor(cellSize, margins, scale, chrome());

    // The character-cell resize grid (base + increment) is cleared while maximized/fullscreen. An
    // incidental refresh (RespectWindowState) must not re-arm it while the window is non-normal — that
    // would put a sub-cell gap around a maximized window on WMs honoring PResizeInc, and can drop the
    // maximized state entirely. The restore-into-normal paths pass Full: they run BEFORE showNormal()
    // settles visibility(), so they cannot rely on a live read and instead declare their intent. The
    // minimum hint never disturbs the maximized state, so it is always applied.
    auto const applyResizeGrid =
        mode == HintApplyMode::Full || osWindow->visibility() == QQuickWindow::Visibility::Windowed;

    // Apply only the hints this platform can safely honor. macOS omits base + increment: Qt's Cocoa
    // plugin writes the base size straight into `-[NSWindow setFrame:]`, so a small base HARD-RESIZES the
    // freshly-mapped window to title-bar-only — the invisible-window regression. See sizeHintPolicyFor().
    auto const policy = geometry::sizeHintPolicyFor(geometry::currentSizeHintPlatform());
    if (policy.applyMinimum)
        osWindow->setMinimumSize(QSize(hints.minimum.width, hints.minimum.height));
    if (policy.applyBase && applyResizeGrid)
        osWindow->setBaseSize(QSize(hints.base.width, hints.base.height));
    if (policy.applyIncrement && applyResizeGrid)
        osWindow->setSizeIncrement(QSize(hints.increment.width, hints.increment.height));

    displayLog()("Size hints: min={}x{}, base={}x{}, increment={}x{} (cellSize={}, scale={}, chrome={})",
                 hints.minimum.width,
                 hints.minimum.height,
                 hints.base.width,
                 hints.base.height,
                 hints.increment.width,
                 hints.increment.height,
                 cellSize,
                 scale,
                 _chromeHeight);
}

namespace
{
    /// Shows @p osWindow in a non-normal state: WM size increments only make sense for interactive
    /// resizes of a normal window, so clear them first (setWindowNormal()'s hint refresh restores them).
    void showWithoutSizeIncrements(QQuickWindow& osWindow, void (QWindow::*show)())
    {
        osWindow.setSizeIncrement(QSize(0, 0));
        (osWindow.*show)();
    }
} // namespace

void WindowController::setWindowFullScreen(display::TerminalDisplay& requester)
{
    auto* osWindow = osWindowFor(requester);
    if (osWindow == nullptr)
        return;
    showWithoutSizeIncrements(*osWindow, &QWindow::showFullScreen);
}

void WindowController::setWindowMaximized(display::TerminalDisplay& requester)
{
    auto* osWindow = osWindowFor(requester);
    if (osWindow == nullptr)
        return;
    showWithoutSizeIncrements(*osWindow, &QWindow::showMaximized);
    _maximizedState = true;
}

void WindowController::setWindowNormal(display::TerminalDisplay& requester)
{
    auto* osWindow = osWindowFor(requester);
    if (osWindow == nullptr)
        return;
    // Full: we are restoring into normal, so re-establish the interactive-resize grid unconditionally.
    // visibility() is still Maximized/FullScreen here (showNormal() below settles it asynchronously), so
    // the intent — not a live state read — must drive the increment write.
    updateSizeHintsFor(requester, HintApplyMode::Full);
    osWindow->showNormal();
    _maximizedState = false;
}

void WindowController::toggleMaximized()
{
    if (_osWindow == nullptr)
        return;
    // Route through the show-mode protocol: maximizing must clear the WM size increments (a WM that
    // honors PResizeInc in non-normal states would otherwise leave a sub-cell gap around the maximized
    // window), and restoring must re-apply the interactive-resize hints (else resizing stops snapping
    // to the character grid until an unrelated font/DPI event refreshes them). Direct
    // QWindow::showMaximized()/showNormal() calls skip both, hence this invokable for QML. The
    // display-less fallbacks cover the pre-first-focus window, where no hints exist to maintain yet.
    if (_osWindow->visibility() == QQuickWindow::Visibility::Maximized)
    {
        if (_activeDisplay != nullptr)
            setWindowNormal(*_activeDisplay);
        else
            _osWindow->showNormal();
    }
    else
    {
        if (_activeDisplay != nullptr)
            setWindowMaximized(*_activeDisplay);
        else
        {
            showWithoutSizeIncrements(*_osWindow, &QWindow::showMaximized);
            _maximizedState = true;
        }
    }
}

void WindowController::minimizeWindow()
{
    if (_osWindow != nullptr)
        _osWindow->showMinimized();
}

bool WindowController::resizeWindowForPage(display::TerminalDisplay& requester,
                                           vtbackend::PageSize totalPageSize)
{
    if (!requester.hasSession())
        return false;

    auto const scale = requester.contentScale();
    auto const marginsDevice =
        geometry::scaled(toGeometryMargins(requester.session().profile().margins.value()), scale);
    // The pane's requirement as a logical extent (ceil side of the rounding law; chrome added later).
    auto const leafLogical = geometry::windowSizeForPage(
        totalPageSize, requester.cellSize(), marginsDevice, scale, geometry::Chrome {});
    return applyContentDrivenResize(requester, leafLogical);
}

bool WindowController::resizeWindowForContentPixels(display::TerminalDisplay& requester,
                                                    vtbackend::ImageSize contentDevicePx)
{
    if (!requester.hasSession())
        return false;
    return applyContentDrivenResize(
        requester, geometry::logicalSizeForDevicePixels(contentDevicePx, requester.contentScale()));
}

bool WindowController::applyContentDrivenResize(display::TerminalDisplay& requester,
                                                geometry::LogicalSize leafContentLogical)
{
    auto* osWindow = osWindowFor(requester);
    if (osWindow == nullptr)
        return false;

    if (osWindow->visibility() == QQuickWindow::Visibility::FullScreen
        || osWindow->visibility() == QQuickWindow::Visibility::Maximized)
    {
        // The window cannot resize while pinned to the screen. The application's column change (DECCOLM)
        // is NOT lost by refusing here: DECCOLM resizes the grid authoritatively on its own (see
        // Terminal::setMode(Columns132)), so the terminal is already the requested width and renders
        // correctly inside the maximized window. Leaving maximized to fit the window is a UX refinement
        // tracked separately — it needs Wayland-correct unmaximize/resize ordering (a naive
        // resize()-after-showNormal() is a fatal xdg-shell protocol error).
        displayLog()("Refusing content-driven window resize while fullscreen/maximized.");
        return false;
    }

    // Solve the pane tree: the content area must be large enough that THIS leaf receives its
    // requirement at fixed split ratios (identity for an unsplit tab).
    auto* tab = activeModelTab();
    auto* leaf = tab != nullptr && tab->rootPane() != nullptr && requester.hasSession()
                     ? tab->rootPane()->findLeaf(requester.session().modelSessionId())
                     : nullptr;
    if (leaf == nullptr)
    {
        displayLog()("Refusing content-driven window resize: requesting pane is not in the active tab.");
        return false;
    }

    // Only the layout root's subtree is on screen (it is the zoomed leaf while zoomed, the whole tree
    // otherwise). A pane outside it has no geometry, so it must not size the window: honoring it would
    // resize the window to fit a grid nobody can see. Stated over the layout root rather than over zoom
    // so it stays true for any future reason the host renders a subtree.
    auto* const layoutRoot = tab->layoutRoot();
    if (!layoutRoot->contains(leaf))
    {
        displayLog()("Refusing content-driven window resize: requesting pane is not on screen.");
        return false;
    }

    // PaneNode.qml's explicit `handle:` binds its thickness to the same constant (via the session
    // manager's splitHandleThickness property), so solver and rendered handle cannot diverge. The
    // layout root bounds the ratio walk: while zoomed it IS the leaf, so the leaf owns the whole
    // content area and no split ratio above it applies.
    auto const content = vtmux::contentSizeForLeaf(
        *leaf,
        vtmux::LayoutSize { .width = leafContentLogical.width, .height = leafContentLogical.height },
        vtmux::DefaultSplitHandleThickness,
        *layoutRoot);

    displayLog()("Content-driven window resize: leaf {}x{} -> content {}x{} + {} chrome.",
                 leafContentLogical.width,
                 leafContentLogical.height,
                 content.width,
                 content.height,
                 _chromeHeight);

    // Straddle guard for the DPR-settlement handler: a resize can move the window's center across a
    // mixed-DPR monitor boundary, whose DevicePixelRatioChange must NOT answer with another resize.
    // The flag covers the synchronous effects; the queued reset re-arms after the event settles.
    _inContentDrivenResize = true;
    osWindow->resize(content.width, content.height + _chromeHeight);
    QMetaObject::invokeMethod(this, [this]() { _inContentDrivenResize = false; }, Qt::QueuedConnection);
    return true;
}

void WindowController::toggleFullScreen(display::TerminalDisplay& requester)
{
    auto* osWindow = osWindowFor(requester);
    if (osWindow == nullptr)
        return;

    if (osWindow->visibility() != QQuickWindow::Visibility::FullScreen)
    {
        // Remember whether to restore into maximized on the way out.
        _maximizedState = osWindow->visibility() == QQuickWindow::Visibility::Maximized;
        showWithoutSizeIncrements(*osWindow, &QWindow::showFullScreen);
    }
    else if (_maximizedState)
    {
        showWithoutSizeIncrements(*osWindow, &QWindow::showMaximized);
    }
    else
    {
        // Restoring into normal (see setWindowNormal): re-establish the resize grid unconditionally
        // before showNormal() settles visibility().
        updateSizeHintsFor(requester, HintApplyMode::Full);
        osWindow->showNormal();
    }
}
// }}}

void WindowController::onDisplayDetached(display::TerminalDisplay* display) noexcept
{
    if (_activeDisplay == display)
        _activeDisplay = nullptr;
}

// {{{ PaneProxy tree
PaneProxy* WindowController::getProxy(vtmux::PaneId id)
{
    auto it = _paneProxies.find(id.value);
    if (it != _paneProxies.end())
        return it->second;
    auto* proxy = new PaneProxy(_manager, id);
    // Parent the proxy to this controller: the rebuild/close paths deleteLater() pruned proxies
    // explicitly, but a controller torn down through removeWindowController() alone (without a
    // preceding closeWindow(), e.g. in tests) would otherwise strand its live proxies.
    proxy->setParent(this);
    QQmlEngine::setObjectOwnership(proxy, QQmlEngine::CppOwnership);
    _paneProxies[id.value] = proxy;
    return proxy;
}

void WindowController::rebuildActiveTabPaneProxies()
{
    vtmux::Tab const* tab = activeModelTab();

    // Walk from rootPane(), NOT layoutRoot(): the liveness set below decides which proxies survive, and
    // every pane of the tab stays live even while a zoom hides all but one. Walking the layout root
    // instead would prune the hidden panes' proxies and tear their terminals down on every zoom.
    std::unordered_map<uint64_t, vtmux::Pane*> live;
    if (tab != nullptr)
        tab->rootPane()->walkTree([&](vtmux::Pane& p) { live[p.id().value] = &p; });

    for (auto const& [idValue, p]: live)
    {
        auto* proxy = getProxy(vtmux::PaneId { idValue });
        // live is only populated when tab != nullptr, so tab->id() is safe here. The tab id keys the
        // proxy's write-backs (setRatio/activate) to exactly this tab.
        proxy->setPane(p, tab->id());
        if (p->isLeaf())
            proxy->setChildren(nullptr, nullptr);
        else
            proxy->setChildren(getProxy(p->first()->id()), getProxy(p->second()->id()));
        proxy->notifyChanged();
        proxy->notifyActiveChanged();
    }

    for (auto it = _paneProxies.begin(); it != _paneProxies.end();)
    {
        if (!live.contains(it->first))
        {
            it->second->setPane(nullptr, vtmux::TabId {});
            it->second->deleteLater();
            it = _paneProxies.erase(it);
        }
        else
            ++it;
    }

    refreshActiveTabLayoutRoot();
}

void WindowController::refreshActiveTabLayoutRoot()
{
    // Render the tab's LAYOUT root, which is the tree root normally and the zoomed leaf while zoomed.
    // This one substitution is the whole of pane zoom on the view side: PaneNode.qml already gives a
    // leaf the entire area it is handed, so re-rooting the tree at that leaf full-screens it and drops
    // its siblings out of the scene — no visibility flag, no geometry override, no zoom-aware QML.
    //
    // Split out from the rebuild above because zoom moves the ROOT without touching the TREE: the
    // proxies are all still valid, so a zoom toggle costs one pointer swap rather than a full walk.
    auto* tab = activeModelTab();
    auto* newRoot = tab != nullptr ? getProxy(tab->layoutRoot()->id()) : nullptr;
    if (newRoot != _activeTabRootProxy)
    {
        _activeTabRootProxy = newRoot;
        emit activeTabRootPaneChanged();
        emit activeSessionChanged();
    }
}

void WindowController::notifyActivePaneChanged()
{
    for (auto const& [id, proxy]: _paneProxies)
        proxy->notifyActiveChanged();
}

void WindowController::notifyRatioChanged(vtmux::PaneId splitNode)
{
    if (auto const it = _paneProxies.find(splitNode.value); it != _paneProxies.end())
        it->second->notifyRatioChanged();
}
// }}}

// {{{ ModelEvents hooks (Qt row/signal emissions on THIS controller's list-model)
void WindowController::onTabAboutToBeAdded(int index)
{
    beginInsertRows(QModelIndex(), index, index);
}

void WindowController::onTabAdded(int)
{
    endInsertRows();
    emit countChanged();
    // The resolved tab-strip gate depends on the live tab count (Multiple mode): re-notify QML so the
    // strip re-shows when the count crosses 1.
    emit tabBarShouldShowChanged();
    refreshAllTabTitles();
}

void WindowController::onTabAboutToBeRemoved(int index)
{
    beginRemoveRows(QModelIndex(), index, index);
}

void WindowController::onTabClosed()
{
    endRemoveRows();
    emit countChanged();
    // See onTabAdded: re-notify so the strip re-hides in Multiple mode when the count falls back to 1.
    emit tabBarShouldShowChanged();
    emit activeTabIndexChanged();
    refreshAllTabTitles();
}

void WindowController::onTabAboutToBeMoved(int fromIndex, int toIndex)
{
    auto const destination = toIndex > fromIndex ? toIndex + 1 : toIndex;
    _tabMoveInProgress = beginMoveRows(QModelIndex(), fromIndex, fromIndex, QModelIndex(), destination);
}

void WindowController::onTabMoved()
{
    if (_tabMoveInProgress)
    {
        endMoveRows();
        _tabMoveInProgress = false;
    }
    emit activeTabIndexChanged();
    refreshAllTabTitles();
}

void WindowController::onActiveTabChanged()
{
    emit activeTabIndexChanged();
    refreshActiveTabHighlight();
    rebuildActiveTabPaneProxies();
}

void WindowController::notifyTabRowChanged(vtmux::TabId tab, QList<Roles> const& roles)
{
    auto const row = rowOfTab(tab);
    if (row < 0)
        return;

    auto roleIds = QList<int> {};
    roleIds.reserve(roles.size());
    for (auto const role: roles)
        roleIds.append(static_cast<int>(role));
    emit dataChanged(index(row), index(row), roleIds);
}

void WindowController::refreshActiveTabHighlight()
{
    if (auto const rows = rowCount(); rows > 0)
        emit dataChanged(index(0), index(rows - 1), { static_cast<int>(Roles::IsActiveRole) });
}

void WindowController::refreshAllTabTitles()
{
    if (auto const rows = rowCount(); rows > 0)
        emit dataChanged(index(0), index(rows - 1), { static_cast<int>(Roles::TitleRole) });
}

void WindowController::updateStatusLine()
{
    // Per-window status-line fan-out (no _displayStates). Build THIS window's tab list once (identical for
    // every pane in the window), then publish it into every visible leaf session of every tab, with the
    // active-tab marker computed PER TAB — so each pane's indicator status line highlights the tab it
    // belongs to, and a rename/recolor in a background window still refreshes that window's status line.
    auto* win = window();
    if (win == nullptr)
        return;

    auto const tabCount = win->tabCount();
    auto const& resolver = _manager.model().sessionTitleResolver();

    std::vector<vtbackend::TabsInfo::Tab> tabs;
    tabs.reserve(static_cast<size_t>(tabCount));
    for (auto const row: std::views::iota(0, tabCount))
    {
        auto* tab = win->tabAt(row);
        if (tab == nullptr)
            continue;
        auto const color = tab->color().value_or(vtbackend::RGBColor { 0, 0, 0 });
        tabs.push_back({ .name = tab->title(resolver), .color = color });
    }

    for (auto const row: std::views::iota(0, tabCount))
    {
        auto* tab = win->tabAt(row);
        if (tab == nullptr)
            continue;
        // The marker is 1-based and identical for every pane of this tab (they share the tab's row).
        auto const marker = static_cast<size_t>(row) + 1;
        for (auto* session: _manager.sessionsOfTab(tab))
            session->terminal().setGuiTabInfoForStatusLine(
                vtbackend::TabsInfo { .tabs = tabs, .activeTabPosition = marker });
    }
}
// }}}

} // namespace contour
