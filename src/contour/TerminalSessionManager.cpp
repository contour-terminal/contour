// SPDX-License-Identifier: Apache-2.0
#include <contour/ColorConversion.h>
#include <contour/ContourGuiApp.h>
#include <contour/LayoutBuilder.h>
#include <contour/PaneProxy.h>
#include <contour/SettingsController.h>
#include <contour/TabLabel.h>
#include <contour/TerminalSession.h>
#include <contour/TerminalSessionManager.h>
#include <contour/WindowController.h>

#include <vtbackend/primitives.h>

#include <vtpty/Process.h>
#if defined(VTPTY_LIBSSH2)
    #include <vtpty/SshSession.h>
#endif

#include <QtQml/QQmlEngine>
#include <QtQuick/QQuickWindow>

#include <filesystem>
#include <ranges>
#include <string>

using namespace std::string_literals;

using std::make_unique;
using std::nullopt;

namespace contour
{

TerminalSessionManager::TerminalSessionManager(ContourGuiApp& app,
                                               SessionFactory& factory,
                                               LayoutStore& layouts,
                                               CommandHistoryStore& commands):
    _app { app },
    _sessionFactory { factory },
    _layoutStore { layouts },
    _commandHistoryStore { commands },
    // Not seeded here, and capacity deliberately left at 0: the config file has not been loaded yet at
    // this point (ContourGuiApp constructs the manager before it reads the file), so the configured
    // `command_palette_recent_count` is not yet known. Seeding now would read the stored list and
    // truncate it to the DEFAULT capacity, permanently dropping entries a user with a larger
    // recent_count had every right to keep. The history is therefore loaded lazily, on the first
    // palette open, once the real capacity is known — see openCommandPalette().
    _commandHistory { 0 },
    _earlyExitThreshold {}
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
}

TerminalSession* TerminalSessionManager::sessionForId(vtmux::SessionId id) const noexcept
{
    auto const it = _sessionsById.find(id.value);
    return it != _sessionsById.end() ? it->second : nullptr;
}

int TerminalSessionManager::rowOfTab(vtmux::TabId tab) const noexcept
{
    // Window-agnostic: resolve the tab's OWNING window through the model, so the row is correct for
    // any tab of any OS window.
    auto* win = _model->window(_model->windowOfTab(tab));
    return win != nullptr ? win->indexOf(tab) : -1;
}

WindowController* TerminalSessionManager::createWindowController()
{
    // Every OS window gets its own vtmux::Window; the controller adapts exactly that window (all its
    // reads and writes are keyed by the WindowId).
    auto* window = _model->createWindow();
    auto* controller = new WindowController(*this, window->id());
    QQmlEngine::setObjectOwnership(controller, QQmlEngine::CppOwnership);
    _controllersByWindow[window->id().value] = controller;
    return controller;
}

WindowController* TerminalSessionManager::controllerFor(vtmux::WindowId window) const noexcept
{
    auto const it = _controllersByWindow.find(window.value);
    return it != _controllersByWindow.end() ? it->second : nullptr;
}

WindowController* TerminalSessionManager::controllerForDisplay(
    display::TerminalDisplay* display) const noexcept
{
    if (_controllersByWindow.empty())
        return nullptr;
    // Prefer the controller that has adopted this display's OS window.
    if (display != nullptr)
        if (auto* osWindow = display->window(); osWindow != nullptr)
            for (auto const& [id, controller]: _controllersByWindow)
                if (controller->ownsOSWindow(osWindow))
                    return controller;
    // Before any controller has recorded its QQuickWindow (the very first focus-in), there is one rendered
    // window, so the sole controller is the owner. With multiple windows this only mis-targets the very
    // first focus of a second window, which immediately corrects once it records its window.
    return _controllersByWindow.begin()->second;
}

TerminalSession* TerminalSessionManager::createSessionInBackground(vtmux::WindowId window,
                                                                   std::optional<std::string> profileName)
{
    // TODO: Remove dependency on app-knowledge and pass shell / terminal-size instead.
    // The GuiApp *or* (Global)Config could be made a global to be accessible from within QML.

    if (!_activeDisplay)
    {
        managerLog()("No active display found. something went wrong.");
    }

    auto* targetWindow = _model->window(window);
    if (targetWindow == nullptr)
    {
        managerLog()("Refusing to create a session: unknown window {}.", window.value);
        return nullptr;
    }

    // Inherit the working directory AND the grid size of the TARGET WINDOW's active pane (the tab the
    // user is looking at), consistent with splitActivePane. Uses the session's own accessors so the
    // inheritance works on every platform. The size inheritance is what keeps a new tab at the live
    // window size: a window the user already resized is the authority (see initialPageSize) — only a
    // brand-new window (no active pane to inherit from) falls back to the profile's terminalSize.
    // (TerminalSession::workingDirectory() already extracts the filesystem path from an OSC 7 file://
    // URL on Windows — see the crash fix in that accessor — so no URL handling is needed here.)
    std::optional<std::string> ptyPath;
    std::optional<vtbackend::PageSize> runningPageSize;
    if (auto* tab = targetWindow->activeTab())
        if (auto* current = sessionForId(tab->activePane()->session()))
        {
            ptyPath = current->workingDirectory();
            runningPageSize = current->terminal().totalPageSize();
        }
    auto const pageSize = geometry::initialPageSize(runningPageSize, _app.profile().terminalSize.value());

    // Pre-mint the session id so the model's allocator (see ctor) hands it back, keeping the model
    // tab/pane and the Qt session on one id.
    auto const sessionId = vtmux::SessionId { _nextSessionId++ };
    auto* session = createBackingSession(sessionId, ptyPath, pageSize, std::nullopt, profileName);

    // Mirror this new session into the vtmux model as a new single-pane tab.
    if (auto* tab = _model->createTab(window))
        _tabBySession[sessionId.value] = tab->id();

    return session;
}

TerminalSession* TerminalSessionManager::createBackingSession(
    vtmux::SessionId sessionId,
    std::optional<std::string> cwd,
    std::optional<vtbackend::PageSize> pageSize,
    std::optional<vtpty::Process::ExecInfo> const& commandOverride,
    std::optional<std::string> const& profileName)
{
    // The command this session ACTUALLY runs: an explicit override wins; otherwise, a session on
    // the app-default profile inherits the CLI-verbatim command (`contour terminal PROGRAM ...`),
    // which mutated that profile's shell for the whole process — so SaveLayout can capture it.
    auto launchedCommand = commandOverride;
    if (!launchedCommand && (!profileName || profileName->empty()))
        launchedCommand = _app.cliCommand();

    // Seed the terminal grid AND the child PTY with the inherited page size: the PTY via the factory
    // (its initial winsize), the Terminal via the constructor (its birth page size). Both matter — the
    // PTY winsize for a shell that reads it immediately, the grid so a background tab that never attaches
    // a display still holds the right size.
    auto* session =
        new TerminalSession(this,
                            _sessionFactory.createPty(std::move(cwd), pageSize, commandOverride, profileName),
                            _app,
                            profileName.value_or(std::string {}),
                            pageSize,
                            std::move(launchedCommand));
    managerLog()("Create backing session with ID {}({}); {} sessions before it.",
                 session->id(),
                 (void*) session,
                 _sessionsById.size());

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

void TerminalSessionManager::detachDisplay(display::TerminalDisplay* display) noexcept
{
    if (display == nullptr)
        return;

    // The display (one pane of a tab) is being destroyed. Session->display ownership lives solely on the
    // pane tree now, so there is no per-display map to scrub; just make sure no controller keeps this
    // freed display as its focused one.
    if (_activeDisplay == display)
        _activeDisplay = nullptr;

    for (auto const& [id, controller]: _controllersByWindow)
        controller->onDisplayDetached(display);
}

void TerminalSessionManager::updateStatusLine()
{
    // Single publish path: each WindowController fans the status line out to its own window's tabs'
    // sessions with a per-tab marker. Route to every live controller.
    for (auto const& [id, controller]: _controllersByWindow)
        controller->updateStatusLine();
}

void TerminalSessionManager::FocusOnDisplay(display::TerminalDisplay* display)
{
    managerLog()("Setting active display to {}", (void*) display);

    // Session->display ownership lives solely on the pane tree (the QML `session:` binding -> setSession);
    // a focused pane already owns its session. FocusOnDisplay only makes this the active display and hands
    // the focus to the owning window's controller, which re-points its window-service signal bridge and
    // re-emits the window bindings. Route by the display's OS window so the correct controller updates.
    _activeDisplay = display;
    if (auto* c = controllerForDisplay(display))
        c->focusDisplay(display);
}

void TerminalSessionManager::setFocusedSession(TerminalSession* next)
{
    if (next == _focusedSession)
        return;
    if (_focusedSession != nullptr)
        _focusedSession->sendFocusOutEvent();
    _focusedSession = next;
    if (_focusedSession != nullptr)
        _focusedSession->sendFocusInEvent();
}

void TerminalSessionManager::clearFocusIfCurrent(TerminalSession* session)
{
    if (session != nullptr && session == _focusedSession)
        setFocusedSession(nullptr);
}

void TerminalSessionManager::syncFocusForWindow(WindowController* controller)
{
    if (controller == nullptr)
        return;
    // Only the window owning the focused display moves focus (so a background window can't steal it);
    // this is the seam that focuses a session swapped onto an already-focused display, where no Qt
    // focus event fires.
    if (_activeDisplay != nullptr && controllerForDisplay(_activeDisplay) == controller)
        setFocusedSession(controller->activeSession());
}

TerminalSession* TerminalSessionManager::createSession(vtmux::WindowId window,
                                                       std::optional<std::string> profileName)
{
    // Just create the backing session + its model tab. The model's activeTabChanged fires the owning
    // controller's proxy rebuild, and the pane tree's `session:` binding attaches the session to its
    // display — no legacy activateSession display-assignment.
    return createSessionInBackground(window, std::move(profileName));
}

void TerminalSessionManager::createNewTab(TerminalSession* acting)
{
    // The CreateNewTab keybinding: the new tab belongs to the window the user typed in.
    if (auto* win = windowHostingSession(acting))
        createSession(win->id());
}

bool TerminalSessionManager::applyLayoutToWindow(vtmux::WindowId window,
                                                 config::Layout const& layout,
                                                 std::optional<vtbackend::PageSize> pageSize)
{
    if (layout.tabs.empty())
    {
        managerLog()("Layout has no tabs; nothing to apply.");
        return false;
    }
    for (auto const& tabSpec: layout.tabs)
    {
        // The seeder stages a backing session for each pane right before the model allocates it,
        // exactly like createBackingSession's use in splitActivePane/createSessionInBackground.
        auto seeder = [&](config::LayoutPane const& leaf) {
            auto const sessionId = vtmux::SessionId { _nextSessionId++ };
            // A command override ONLY when the pane actually names a program to run. A pane that
            // just picks a directory still runs the profile's shell — it travels through `cwd`,
            // the same channel every other new tab/split uses. Engaging an override with an empty
            // program here would tell the factory "this session overrides the shell", which (among
            // other things) would skip an SSH profile's SshSession and open a LOCAL shell instead.
            std::optional<vtpty::Process::ExecInfo> command;
            if (leaf.command || !leaf.arguments.empty())
            {
                command = vtpty::Process::ExecInfo {};
                command->program = leaf.command.value_or(std::string {});
                command->arguments = leaf.arguments;
            }
            std::optional<std::string> profileName = leaf.profile ? leaf.profile : tabSpec.profile;
            if (profileName && _app.config().findProfile(*profileName) == nullptr)
            {
                managerLog()("Layout references unknown profile '{}'; using window profile.", *profileName);
                profileName.reset();
            }
            std::optional<std::string> cwd =
                leaf.directory ? std::optional { leaf.directory->string() } : std::nullopt;
            createBackingSession(sessionId, cwd, pageSize, command, profileName);
        };

        auto* modelTab = realizeLayoutTab(*_model, window, tabSpec, seeder);
        _pendingSessionId.reset(); // consumed by the allocator; clear any leftover
        if (modelTab != nullptr)
        {
            // Map every leaf session in the new tab to this tab id (mirrors createSessionInBackground).
            modelTab->rootPane()->walkTree([&](vtmux::Pane& p) {
                if (p.isLeaf())
                    _tabBySession[p.session().value] = modelTab->id();
            });
        }
    }
    return true;
}

config::Layout const* TerminalSessionManager::findLayout(std::string const& name,
                                                         std::string_view context) const
{
    auto const& map = _app.config().layouts.value();
    auto const it = map.find(name);
    if (it == map.end())
    {
        managerLog()("{}: no layout named '{}'.", context, name);
        return nullptr;
    }
    return &it->second;
}

void TerminalSessionManager::launchLayout(std::string const& name, TerminalSession* acting)
{
    auto const* layout = findLayout(name, "LaunchLayout");
    if (layout == nullptr)
        return;
    auto* win = windowHostingSession(acting);
    if (win == nullptr)
        return;
    // The new tabs join a LIVE window: birth their grids and PTYs at its running page size (like
    // CreateNewTab/splits do), not at the profile's configured default — a maximized window would
    // otherwise spawn every layout command into an 80x25 terminal.
    applyLayoutToWindow(win->id(), *layout, acting->terminal().totalPageSize());
}

bool TerminalSessionManager::consumeDefaultLayout(contour::WindowController* controller)
{
    // One-shot: main.qml calls this for EVERY window it loads, but the startup layout belongs to
    // the first window only — a later NewTerminalWindow must not re-run all the layout's commands.
    if (_startupLayoutConsumed)
        return false;
    auto const name = _app.layoutName();
    if (name.empty())
        return false;
    _startupLayoutConsumed = true;

    auto const* layout = findLayout(name, "Startup layout");
    if (layout == nullptr)
        return false; // the window falls back to its usual single default tab
    return applyLayoutToWindow(controller->windowId(), *layout);
}

void TerminalSessionManager::saveLayout(std::string const& name, TerminalSession* acting)
{
    auto* win = windowHostingSession(acting);
    if (win == nullptr)
        return;
    if (auto const saved = saveWindowLayout(win->id(), name); !saved)
        managerLog()("SaveLayout '{}' failed ({}).", name, describe(saved.error()));
}

std::filesystem::path TerminalSessionManager::configSiblingPath(std::string_view fileName) const
{
    // Next to the loaded config file — which is exactly where loadConfigFromFile() merges these files
    // back from, so a custom `--config` path moves the store with it instead of stranding saves in the
    // default config home. An empty configFile (a default-constructed config, e.g. in tests) means no
    // file was loaded: fall back to the config home the loader would have used.
    auto const& configFile = _app.config().configFile;
    return (configFile.empty() ? config::configHome() : configFile.parent_path()) / fileName;
}

std::filesystem::path TerminalSessionManager::layoutsFilePath() const
{
    return configSiblingPath("layouts.yml");
}

std::filesystem::path TerminalSessionManager::commandHistoryFilePath() const
{
    return configSiblingPath("command-history.yml");
}

void TerminalSessionManager::ensureCommandHistoryReady()
{
    // Capacity first, and on EVERY call: editing `command_palette_recent_count` and reloading the
    // config then takes effect without a restart.
    //
    // A negative count is a typo, not a request for a negative list, so clamp rather than wrap into a
    // huge size_t. Zero is meaningful and honored: it turns the "recently used" section off.
    auto const configured = _app.config().commandPaletteRecentCount.value();
    _commandHistory.setCapacity(static_cast<std::size_t>(std::max(0, configured)));

    if (_commandHistoryLoaded)
        return;
    _commandHistoryLoaded = true;

    // The lazy first load, now that the capacity is known — see the constructor for why it cannot
    // happen there. A failure is not fatal: the palette simply starts with an empty "recently used"
    // section, which is what a first run looks like anyway. It IS logged, because a corrupt file would
    // otherwise silently keep losing the user's list on every run.
    if (auto stored = _commandHistoryStore.load(commandHistoryFilePath()))
        _commandHistory.reset(*stored);
    else
        managerLog()("Failed to load command history: {}", stored.error());
}

void TerminalSessionManager::openCommandPalette(TerminalSession* acting)
{
    auto* controller = controllerHostingSession(acting);
    if (controller == nullptr)
        return;

    ensureCommandHistoryReady();
    controller->openCommandPalette();
}

void TerminalSessionManager::openSettings(TerminalSession* acting)
{
    auto* controller = controllerHostingSession(acting);
    if (controller == nullptr)
        return;

    controller->openSettings();
}

void TerminalSessionManager::reloadAllSessions()
{
    // Refresh the master config first, so the app — and the settings page that reads it — see the new
    // side files immediately. Then have each live session re-read and re-apply, updating the terminals.
    config::loadConfigFromFile(_app.config(), _app.config().configFile);
    for (auto& [id, session]: _sessionsById)
        if (session != nullptr)
            session->onConfigReload();

    // The settings page reads through the app config just reloaded above, but each OS window owns its
    // own SettingsController with model caches rebuilt only by refresh(). The controller that triggered
    // the save refreshes itself; refresh every OTHER window's too, so a settings page open in a second
    // window reflects the change immediately instead of serving stale caches until it is reopened.
    for (auto& [windowId, controller]: _controllersByWindow)
        if (controller != nullptr)
            if (auto* settings = controller->settingsController(); settings != nullptr)
                settings->refresh();
}

void TerminalSessionManager::openContextMenu(TerminalSession* acting)
{
    // Announced before the routing, which no-ops when there is no window to show a menu in. That makes the
    // ARRIVAL of the request observable on its own — otherwise "the right-click reached this action" and
    // "the right-click was swallowed on the way here" look exactly alike from outside.
    emit contextMenuRequested(acting);

    auto* controller = controllerHostingSession(acting);
    if (controller == nullptr)
        return;

    // The menu belongs to the pane that was right-clicked, which is not necessarily the active one. Make
    // it active first: the menu is then built from ITS state, and every row it offers runs against IT.
    // This is also what gives the pane keyboard focus, which is what a user expects a right-click to do.
    if (auto* tab = paneActionTargetTab(acting); tab != nullptr && tab->rootPane() != nullptr)
        if (auto* leaf = tab->rootPane()->findLeaf(acting->modelSessionId()); leaf != nullptr)
            activatePane(tab->id(), leaf->id());

    controller->openContextMenu();
}

void TerminalSessionManager::recordCommand(std::string const& id)
{
    // Prepared here too, not just in openCommandPalette(). Depending on the palette having been opened
    // first would be true in the GUI and false for any other caller — and the failure would be SILENT:
    // an unprepared history has capacity 0, so record() would quietly drop every command and then
    // faithfully persist the empty list over whatever the user had.
    ensureCommandHistoryReady();

    _commandHistory.record(id);

    if (auto const saved = _commandHistoryStore.save(commandHistoryFilePath(), _commandHistory.recent());
        !saved)
    {
        // Losing the MRU costs the user some ordering, not their work, so a failed write is logged and
        // shrugged off rather than being surfaced as a modal the user must dismiss mid-command.
        managerLog()("Failed to save command history: {}", saved.error());
    }
}

std::expected<void, LayoutSaveError> TerminalSessionManager::saveWindowLayout(vtmux::WindowId windowId,
                                                                              std::string const& name)
{
    if (name.empty())
        return std::unexpected(LayoutSaveError::EmptyName);

    auto* window = _model->window(windowId);
    if (window == nullptr)
        return std::unexpected(LayoutSaveError::UnknownWindow);

    auto const resolve = [this](vtmux::SessionId id) {
        PaneLeafData data;
        if (auto* session = sessionForId(id))
        {
            if (auto const dir = session->workingDirectory(); !dir.empty())
                data.directory = dir;
            // An engaged override with an EMPTY program runs the profile's default shell, so there
            // is no command worth persisting for it.
            if (auto const& launched = session->launchedCommand(); launched && !launched->program.empty())
            {
                data.command = launched->program;
                data.arguments = launched->arguments;
            }
            // Only capture a REAL per-pane profile override: profileName() always resolves to a
            // concrete profile (the app default when none was given), which must NOT be pinned
            // into the store — the saved layout should keep following the user's default profile.
            data.profile = session->profileOverride();
        }
        return data;
    };

    config::Layout layout;
    for (auto const i: std::views::iota(0, window->tabCount()))
        if (auto* tab = window->tabAt(i))
            layout.tabs.push_back(serializeTab(*tab, resolve));

    // Persist the store's OWN prior contents plus this layout — not the merged inline+file view
    // held in memory: the store wins name collisions on load, so writing the merged view back out
    // would permanently freeze any inline contour.yml layouts into it.
    auto const path = layoutsFilePath();
    auto storedLayouts = _layoutStore.load(path);
    if (!storedLayouts)
    {
        // Refusing beats destroying: treating an unreadable store as empty and rewriting it would
        // permanently delete every layout it still holds.
        managerLog()("Refusing to save layout '{}': {} is unreadable ({}); fix or remove the file.",
                     name,
                     path.string(),
                     storedLayouts.error());
        return std::unexpected(LayoutSaveError::StoreUnreadable);
    }
    (*storedLayouts)[name] = layout;

    if (auto const written = _layoutStore.save(path, *storedLayouts); !written)
    {
        managerLog()("Failed to save layout '{}': {}", name, written.error());
        return std::unexpected(LayoutSaveError::WriteFailed);
    }

    // Only now that the store has accepted the layout do we update the in-memory config, so a
    // subsequent LaunchLayout in this run sees the save, and runtime state never diverges from
    // what is actually persisted. This merges the new entry into the existing in-memory (inline +
    // file) view rather than replacing it wholesale, so inline layouts stay visible in this run
    // even though they were deliberately excluded from the store just written.
    _app.config().layouts.value()[name] = std::move(layout);

    managerLog()("Saved layout '{}' to {}", name, path.string());
    return {};
}

void TerminalSessionManager::switchToPreviousTab(TerminalSession* acting)
{
    // The "previous tab" memory lives on the acting session's window.
    auto* win = windowHostingSession(acting);
    if (win == nullptr)
        return;
    auto* previous = win->previousActiveTab();
    if (previous == nullptr)
        return;
    managerLog()("switch to previous tab: row {}", rowOfTab(previous->id()));
    activateModelTabByRow(win->id(), rowOfTab(previous->id()));
}

void TerminalSessionManager::switchToTabLeft(TerminalSession* acting)
{
    auto* win = windowHostingSession(acting);
    if (win == nullptr)
        return;
    auto const tabCount = win->tabCount();
    if (tabCount <= 0)
        return;
    auto const current = win->activeTabIndex();
    // Move one tab to the left in tab-space (rows are tabs), wrapping at the start.
    auto const target = current > 0 ? current - 1 : tabCount - 1;
    managerLog()("switch to tab left: current row {}, target row {}, tabs {}", current, target, tabCount);
    activateModelTabByRow(win->id(), target);
}

void TerminalSessionManager::switchToTabRight(TerminalSession* acting)
{
    auto* win = windowHostingSession(acting);
    if (win == nullptr)
        return;
    auto const tabCount = win->tabCount();
    if (tabCount <= 0)
        return;
    auto const current = win->activeTabIndex();
    // Move one tab to the right in tab-space (rows are tabs), wrapping at the end.
    auto const target = current >= 0 && current < tabCount - 1 ? current + 1 : 0;
    managerLog()("switch to tab right: current row {}, target row {}, tabs {}", current, target, tabCount);
    activateModelTabByRow(win->id(), target);
}

void TerminalSessionManager::beginTabTitleEdit(TerminalSession* acting)
{
    if (auto* controller = controllerHostingSession(acting); controller != nullptr)
        controller->beginActiveTabTitleEdit();
}

void TerminalSessionManager::beginTabColorPick(TerminalSession* acting)
{
    if (auto* controller = controllerHostingSession(acting); controller != nullptr)
        controller->beginActiveTabColorPick();
}

void TerminalSessionManager::beginSaveLayoutPrompt(TerminalSession* acting)
{
    if (auto* controller = controllerHostingSession(acting); controller != nullptr)
        controller->beginSaveLayoutPrompt();
}

void TerminalSessionManager::setActiveTabColor(vtbackend::RGBColor color, TerminalSession* acting)
{
    if (auto* controller = controllerHostingSession(acting); controller != nullptr)
        controller->setActiveTabColor(color);
}

void TerminalSessionManager::resetActiveTabColor(TerminalSession* acting)
{
    if (auto* controller = controllerHostingSession(acting); controller != nullptr)
        controller->resetActiveTabColor();
}

void TerminalSessionManager::switchToTab(int position, TerminalSession* acting)
{
    auto* win = windowHostingSession(acting);
    if (win == nullptr)
        return;
    // position is 1-based (keyboard "go to tab N"); rows are tabs.
    managerLog()("switchToTab to position {} (out of {} tabs)", position, win->tabCount());
    if (1 <= position && position <= win->tabCount())
        activateModelTabByRow(win->id(), position - 1);
}

void TerminalSessionManager::removeWindowController(vtmux::WindowId windowId)
{
    // Drop the controller for a closed OS window. Real per-window identity: "last window" is
    // _controllersByWindow.size() == 1 (the manager's authoritative window registry), NOT a scan of
    // display keys. The controller has already removed its vtmux::Window, pruned its proxy tree and
    // terminated its own sessions (WindowController::closeWindow); here we just unregister + delete it,
    // and — if this was the last window — clear any residual shared session registries so no stale entry
    // outlives the process's last window.
    auto const it = _controllersByWindow.find(windowId.value);
    if (it == _controllersByWindow.end())
        return;

    bool const wasLast = _controllersByWindow.size() == 1;
    auto* controller = it->second;
    _controllersByWindow.erase(it);
    if (controller != nullptr)
        controller->deleteLater();

    if (wasLast)
    {
        managerLog()("Last window closed: clearing residual session registries.");
        _sessionsById.clear();
        _tabBySession.clear();
        _activeDisplay = nullptr;
    }
}

void TerminalSessionManager::closeTab(TerminalSession* acting)
{
    // CloseTab is documented "Closes current tab." — close every pane of the acting session's tab,
    // not just one leaf, so a split tab is fully closed. The model collapses to the survivor on each
    // close and tears the tab down with the last pane (same whole-tab path as closeTabAtIndex / the
    // GUI context menu).
    if (acting == nullptr)
    {
        managerLog()("Failed to close tab: no acting session.");
        return;
    }
    auto* tab = tabHostingSession(acting);
    if (tab == nullptr)
    {
        removeSession(*acting);
        return;
    }
    managerLog()("Close tab: acting session ID {}, tab row {}", acting->id(), rowOfTab(tab->id()));
    terminateSessions(sessionsInTab(tab));
}

void TerminalSessionManager::moveTabByTab(vtmux::Tab* tab, int targetRow)
{
    // The single move mechanism: reorder the tab through the authoritative model — within the tab's
    // OWNING window — so the tab strip and status line (both model-driven) actually move, then
    // refresh. Every move entry point funnels through here so this post-move refresh lives in one
    // place.
    if (tab == nullptr)
        return;
    _model->moveTab(_model->windowOfTab(tab->id()), tab->id(), targetRow);
    updateStatusLine();
}

void TerminalSessionManager::moveTabToRow(TerminalSession* session, int targetRow)
{
    moveTabByTab(tabHostingSession(session), targetRow);
}

void TerminalSessionManager::moveTabTo(int position, TerminalSession* acting)
{
    // position is 1-based (the keybinding's argument); the model is 0-based tab-space.
    auto* win = windowHostingSession(acting);
    if (win == nullptr || position < 1 || position > win->tabCount())
        return;
    moveTabToRow(acting, position - 1);
}

void TerminalSessionManager::moveTabToLeft(TerminalSession* session)
{
    auto* tab = tabHostingSession(session);
    if (tab == nullptr)
        return;
    if (auto const row = rowOfTab(tab->id()); row > 0)
        moveTabToRow(session, row - 1);
}

void TerminalSessionManager::moveTabToRight(TerminalSession* session)
{
    auto* win = windowHostingSession(session);
    auto* tab = tabHostingSession(session);
    if (tab == nullptr || win == nullptr)
        return;
    if (auto const row = rowOfTab(tab->id()); row >= 0 && row + 1 < win->tabCount())
        moveTabToRow(session, row + 1);
}

void TerminalSessionManager::currentSessionIsTerminated()
{
    managerLog()("got notified that session is terminated, number of existing sessions: {}",
                 _sessionsById.size());
}

void TerminalSessionManager::removeSession(TerminalSession& thatSession)
{
    managerLog()(
        "remove session: session: {}, sessions registered: {}", (void*) &thatSession, _sessionsById.size());

    auto const sessionId = thatSession.modelSessionId();
    if (auto const it = _sessionsById.find(sessionId.value);
        it == _sessionsById.end() || it->second != &thatSession)
    {
        managerLog()("Session not found in session registry.");
        return;
    }

    // Drop the focus back-pointer if this was the focused session (without sending a focus-out to a
    // session that is already tearing down), so _focusedSession can never dangle.
    if (_focusedSession == &thatSession)
        _focusedSession = nullptr;

    // Mirror the removal into the vtmux model FIRST, while the registry still resolves this session
    // to its tab/leaf, then erase the local bookkeeping. A session is one *pane*; closing it must
    // close only that pane. closePane() absorbs the surviving sibling when the tab still has other
    // panes and only tears down the whole tab (firing tabClosed -> beginRemoveRows) when this was
    // the tab's last pane — so closing one split pane no longer destroys its siblings.
    if (auto* tab = findTabHostingSession(sessionId); tab != nullptr)
        if (auto* leaf = tab->rootPane()->findLeaf(sessionId); leaf != nullptr)
            _model->closePane(_model->windowOfTab(tab->id()), tab->id(), leaf->id());
    _sessionsById.erase(sessionId.value);
    _tabBySession.erase(sessionId.value);

    // Session->display ownership is the pane tree's; there is no per-display map to scrub. The pane's
    // model leaf was closed above (collapsing its split); the surviving panes keep their sessions via
    // their own `session:` bindings. Refresh the status line so the closed pane's tab drops from it.
    updateStatusLine();
}

void TerminalSessionManager::updateColorPreference(vtbackend::ColorPreference const& preference)
{
    for (auto* session: _sessionsById | std::views::values)
        session->updateColorPreference(preference);
}

vtmux::Tab* TerminalSessionManager::tabAtRow(vtmux::WindowId window, int index) const noexcept
{
    auto* win = _model->window(window);
    return win != nullptr ? win->tabAt(index) : nullptr;
}

vtmux::Tab* TerminalSessionManager::findTabHostingSession(vtmux::SessionId session) const noexcept
{
    // O(1) via the SessionId -> TabId index.
    if (auto const it = _tabBySession.find(session.value); it != _tabBySession.end())
        return _model->findTab(it->second);
    return nullptr;
}

// {{{ vtmux::ModelEvents
//
// Stage 3 of the per-window refactor: the manager hosts the SessionModel and implements ModelEvents, but
// the QML tab strip / pane tree bind to the per-window WindowController. So each handler is now a pure
// ROUTER: it dispatches the Qt row/signal/proxy work to the OWNING window's controller (tab events carry
// the WindowId; pane events resolve TabId -> WindowId via the model) and does no list-model / proxy work of
// its own. The manager's own QAbstractListModel emissions and proxy tree — which Stage 2 kept as a harmless
// unobserved superset — are gone; the status-line fan-out (updateStatusLine) still lives here until Stage 4
// moves it onto the controllers.
void TerminalSessionManager::tabAboutToBeAdded(vtmux::WindowId window, int index)
{
    if (auto* c = controllerFor(window))
        c->onTabAboutToBeAdded(index);
}

void TerminalSessionManager::tabAdded(vtmux::WindowId window, vtmux::TabId, int index)
{
    if (auto* c = controllerFor(window))
        c->onTabAdded(index);
}

void TerminalSessionManager::tabAboutToBeRemoved(vtmux::WindowId window, int index)
{
    if (auto* c = controllerFor(window))
        c->onTabAboutToBeRemoved(index);
}

void TerminalSessionManager::tabClosed(vtmux::WindowId window, vtmux::TabId, int)
{
    if (auto* c = controllerFor(window))
        c->onTabClosed();
}

void TerminalSessionManager::tabAboutToBeMoved(vtmux::WindowId window, int fromIndex, int toIndex)
{
    if (auto* c = controllerFor(window))
        c->onTabAboutToBeMoved(fromIndex, toIndex);
}

void TerminalSessionManager::tabMoved(vtmux::WindowId window, vtmux::TabId, int, int)
{
    if (auto* c = controllerFor(window))
        c->onTabMoved();
}

void TerminalSessionManager::tabAboutToBeMovedToWindow(vtmux::WindowId from,
                                                       int fromIndex,
                                                       vtmux::WindowId to,
                                                       int toIndex)
{
    // Qt has no cross-model "move rows"; a transplant is a remove on the source and an insert on the
    // destination. Bracket each on its own controller.
    if (auto* src = controllerFor(from))
        src->onTabAboutToBeRemoved(fromIndex);
    if (auto* dst = controllerFor(to))
        dst->onTabAboutToBeAdded(toIndex);
}

void TerminalSessionManager::tabMovedToWindow(
    vtmux::WindowId from, vtmux::TabId, int, vtmux::WindowId to, int toIndex)
{
    // Close-half on the source completes its beginRemoveRows; add-half on the destination completes its
    // beginInsertRows (bracketed in tabAboutToBeMovedToWindow with the SAME toIndex). onTabAdded ignores
    // the index — endInsertRows needs none — but we pass the event's toIndex, not a re-derived one.
    if (auto* src = controllerFor(from))
        src->onTabClosed();
    if (auto* dst = controllerFor(to))
        dst->onTabAdded(toIndex);
}

void TerminalSessionManager::activeTabChanged(vtmux::WindowId window, vtmux::TabId, int)
{
    if (auto* c = controllerFor(window))
    {
        c->onActiveTabChanged();
        // Re-point focus at the new active leaf, after the rebuild so activeSession() is current; this
        // also covers cross-window moves, which route through activeTabChanged on the destination.
        syncFocusForWindow(c);
    }
}

void TerminalSessionManager::paneSplit(vtmux::TabId tab, vtmux::PaneId, vtmux::PaneId)
{
    if (auto* c = controllerFor(_model->windowOfTab(tab)))
    {
        c->notifyTabRowChanged(tab, { WindowController::TitleRole, WindowController::PaneCountRole });
        c->rebuildActiveTabPaneProxies();
    }
}

void TerminalSessionManager::paneClosed(vtmux::TabId tab, vtmux::PaneId, vtmux::PaneId)
{
    if (auto* c = controllerFor(_model->windowOfTab(tab)))
    {
        c->notifyTabRowChanged(tab, { WindowController::TitleRole, WindowController::PaneCountRole });
        c->rebuildActiveTabPaneProxies();
    }
}

void TerminalSessionManager::activePaneChanged(vtmux::TabId tab, vtmux::PaneId)
{
    if (auto* c = controllerFor(_model->windowOfTab(tab)))
    {
        c->notifyTabRowChanged(tab, { WindowController::TitleRole });
        c->notifyActivePaneChanged();
        c->emitActiveSessionChanged();
        // Move terminal focus to the newly active pane's leaf (symmetric out/in via setFocusedSession).
        syncFocusForWindow(c);
    }
}

void TerminalSessionManager::paneRatioChanged(vtmux::TabId tab, vtmux::PaneId splitNode, double)
{
    if (auto* c = controllerFor(_model->windowOfTab(tab)))
        c->notifyRatioChanged(splitNode);
}

void TerminalSessionManager::paneOrientationChanged(vtmux::TabId tab, vtmux::PaneId, vtmux::SplitState)
{
    // Rebuild the tab's proxy tree: the coarse refresh re-emits every proxy's `changed` (which carries
    // `orientation`), so PaneNode.qml re-reads the flipped axis and re-lays out its SplitView. Same
    // path as paneSplit/paneClosed — the tree is small, so a targeted update buys nothing.
    if (auto* c = controllerFor(_model->windowOfTab(tab)))
        c->rebuildActiveTabPaneProxies();
}

void TerminalSessionManager::paneSwapped(vtmux::TabId tab, vtmux::PaneId, vtmux::PaneId)
{
    // A swap moves sessions between two leaves (ids unchanged). The rebuild re-binds each proxy's
    // `session`, so the two affected panes render their new terminals.
    if (auto* c = controllerFor(_model->windowOfTab(tab)))
        c->rebuildActiveTabPaneProxies();
}

void TerminalSessionManager::paneZoomChanged(vtmux::TabId tab, std::optional<vtmux::PaneId>)
{
    // Zoom moves the tab's layout ROOT without reshaping its TREE, so this re-points the rendered root
    // rather than rebuilding the proxy tree: the panes (and their terminals) are all still there, just
    // hidden behind the zoomed one. That also keeps this cheap when a zoom clear rides along with a
    // restructuring event, which has already rebuilt.
    //
    // Only the badge is republished here: the retitle a zoom implies is announced by the model itself
    // (SessionModel::announceZoomChange -> tabTitleChanged), so every host gets it, not just this one.
    if (auto* c = controllerFor(_model->windowOfTab(tab)))
    {
        c->notifyTabRowChanged(tab, { WindowController::ZoomedRole });
        c->refreshActiveTabLayoutRoot();
    }
}

void TerminalSessionManager::paneTreeRestructured(vtmux::TabId tab)
{
    // A move re-parents nodes and re-homes ids unpredictably, so re-read the whole tab's tree.
    if (auto* c = controllerFor(_model->windowOfTab(tab)))
    {
        c->notifyTabRowChanged(tab, { WindowController::TitleRole, WindowController::PaneCountRole });
        c->rebuildActiveTabPaneProxies();
    }
}

void TerminalSessionManager::tabTitleChanged(vtmux::TabId tab)
{
    // The indicator status line's {Tabs} entry is built from the tab titles, so republish it here
    // (Stage 4 moves this fan-out onto the controllers); otherwise a renamed tab keeps its old name in
    // the status line until an unrelated event next refreshes it.
    updateStatusLine();
    if (auto* c = controllerFor(_model->windowOfTab(tab)))
        c->notifyTabRowChanged(tab, { WindowController::TitleRole, WindowController::RawTitleRole });
}

void TerminalSessionManager::tabColorChanged(vtmux::TabId tab)
{
    updateStatusLine();
    if (auto* c = controllerFor(_model->windowOfTab(tab)))
        c->notifyTabRowChanged(tab, { WindowController::ColorRole });
}

void TerminalSessionManager::refreshAllTabTitles()
{
    // Every window's tab strip may show {TabPosition}/{WindowTitle}-derived labels; let each
    // controller re-emit for its own rows. (The manager itself no longer owns a list model.)
    for (auto* controller: _controllersByWindow | std::views::values)
        controller->refreshAllTabTitles();
}

void TerminalSessionManager::refreshTabForSession(vtmux::SessionId session)
{
    // Route to the OWNING window's controller — that list-model is what the visible tab strip
    // renders from. (Emitting on the manager reached nothing: no QML binds the manager's rows.)
    if (auto* tab = findTabHostingSession(session))
        if (auto* c = controllerFor(_model->windowOfTab(tab->id())))
            c->notifyTabRowChanged(tab->id(), { WindowController::TitleRole });
}

void TerminalSessionManager::setTabColorForSession(vtmux::SessionId session, vtbackend::RGBColor color)
{
    // Reuse the authoritative tab-color path: SessionModel::setTabColor fires tabColorChanged, which
    // this manager routes to the owning WindowController's ColorRole, repainting the tab strip. The
    // Application source keeps this write off the user's own color slot, which outranks it.
    if (auto* tab = findTabHostingSession(session))
        _model->setTabColor(tab->id(), vtmux::TabColorSource::Application, color);
}

void TerminalSessionManager::resetTabColorForSession(vtmux::SessionId session)
{
    if (auto* tab = findTabHostingSession(session))
        _model->resetTabColor(tab->id(), vtmux::TabColorSource::Application);
}
// }}}

// {{{ Tab-strip operations (window-routed)
void TerminalSessionManager::activateModelTabByRow(vtmux::WindowId window, int row)
{
    auto* tab = tabAtRow(window, row);
    if (tab == nullptr)
        return;

    // Update the window's active tab: this fires activeTabChanged -> the owning controller rebuilds
    // its PaneProxy tree and rebinds activeTabRootPane, and the pane tree's `session:` binding
    // attaches the tab's active-leaf session to its display.
    _model->activateTab(window, tab->id());
}

void TerminalSessionManager::activateTab(vtmux::WindowId window, int index)
{
    activateModelTabByRow(window, index);
}

void TerminalSessionManager::moveTab(vtmux::WindowId window, int fromIndex, int toIndex)
{
    // fromIndex/toIndex are tab-strip rows (tab-space) of @p window. The tab order is owned by the
    // model, so reorder purely there and bounds-check against that window's tab count.
    auto* win = _model->window(window);
    if (win == nullptr)
        return;
    auto const tabCount = win->tabCount();
    if (fromIndex < 0 || fromIndex >= tabCount || toIndex < 0 || toIndex >= tabCount)
        return;
    moveTabByTab(tabAtRow(window, fromIndex), toIndex);
}

void TerminalSessionManager::closeWindowIfEmpty(vtmux::WindowId window)
{
    // An empty window (no tabs) is not a valid state: after a cross-window move that took a window's
    // last tab, close it. Runs AFTER the transplant, so the moved tab's sessions are already gone from
    // this window and closeWindow()'s removeWindow won't tear them down.
    if (auto* win = _model->window(window); win != nullptr && win->empty())
        if (auto* controller = controllerFor(window))
            controller->closeWindow();
}

void TerminalSessionManager::moveTabToWindow(vtmux::WindowId from,
                                             int fromIndex,
                                             vtmux::WindowId to,
                                             int toIndex)
{
    // Resolve the tab in tab-space of the source window, then hand the model TabIds — the model owns
    // the transplant (and fires the cross-window events the two controllers react to).
    auto* tab = tabAtRow(from, fromIndex);
    if (tab == nullptr)
        return;
    _model->moveTabToWindow(from, tab->id(), to, toIndex);
    updateStatusLine();
    // Dragging a window's LAST tab elsewhere empties it — close the now-empty source window.
    closeWindowIfEmpty(from);
}

void TerminalSessionManager::tearOffTabToNewWindow(vtmux::WindowId from, int fromIndex, QScreen* targetScreen)
{
    auto* tab = tabAtRow(from, fromIndex);
    if (tab == nullptr)
        return;
    // A single-tab window torn off would just recreate itself — nothing to do.
    auto* srcWindow = _model->window(from);
    if (srcWindow != nullptr && srcWindow->tabCount() <= 1)
        return;

    // Stage the transplant, then spawn a window. The new window's main.qml consumes the staged tab in
    // Component.onCompleted (via consumePendingTransplant) INSTEAD of creating a fresh first tab, so the
    // torn-off tab becomes its sole tab — its sessions survive the move (nothing is torn down).
    _pendingTransplant = std::pair { from, tab->id() };
    _app.newWindow(targetScreen);
}

bool TerminalSessionManager::consumePendingTransplant(WindowController* newController)
{
    if (!_pendingTransplant.has_value() || newController == nullptr)
        return false;
    auto const [fromWindow, tabId] = *_pendingTransplant;
    _pendingTransplant.reset();

    // The destination window currently has no tabs (its first-tab creation was deferred pending this).
    // Move the staged tab in at index 0; the model fixes both windows' bookkeeping and fires events so
    // the source strip loses the row and this new window gains it.
    _model->moveTabToWindow(fromWindow, tabId, newController->windowId(), 0);
    updateStatusLine();

    // Tearing the tab out empties the source window (it was single-tab, so it is now empty) — close it.
    closeWindowIfEmpty(fromWindow);

    return true;
}

void TerminalSessionManager::closeTabAtIndex(vtmux::WindowId window, int index)
{
    // index is a tab row (rows are tabs). Close the whole tab by terminating each of its panes'
    // sessions; the model collapses to the survivor on each close and tears the tab down with the
    // last pane.
    terminateSessions(sessionsInTab(tabAtRow(window, index)));
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

std::vector<TerminalSession*> TerminalSessionManager::gatherSessionsOfTabsWhere(
    vtmux::Window& window, std::function<bool(int row, vtmux::Tab*)> const& predicate) const
{
    // Collect the backing sessions of every tab of @p window matching @p predicate. Used by the
    // bulk-close operations, which must gather the doomed sessions BEFORE the model mutates (so the
    // tabs still exist), then close structurally through the unit-tested model method, then terminate
    // the gathered sessions. terminate() -> removeSession then only cleans up the Qt bookkeeping, so
    // we close in tab-space, not pane-space. The row is handed to the predicate so a positional test
    // (closeTabsToRight) compares directly instead of re-deriving it with a linear rowOfTab() scan
    // per tab (which made the anchor comparison O(tabs^2)).
    std::vector<TerminalSession*> doomed;
    for (auto const row: std::views::iota(0, window.tabCount()))
        if (auto* tab = window.tabAt(row); tab != nullptr && predicate(row, tab))
            for (auto* session: sessionsInTab(tab))
                doomed.push_back(session);
    return doomed;
}

void TerminalSessionManager::closeOtherTabs(vtmux::WindowId window, int index)
{
    auto* win = _model->window(window);
    auto* keep = tabAtRow(window, index);
    if (keep == nullptr || win == nullptr)
        return;

    auto const doomed =
        gatherSessionsOfTabsWhere(*win, [keep](int, vtmux::Tab* tab) { return tab->id() != keep->id(); });
    _model->closeOtherTabs(window, keep->id());
    terminateSessions(doomed);
}

void TerminalSessionManager::closeTabsToRight(vtmux::WindowId window, int index)
{
    auto* win = _model->window(window);
    auto* anchor = tabAtRow(window, index);
    if (anchor == nullptr || win == nullptr)
        return;

    // Tabs strictly to the right of @p index. The gather scan is already row-ordered and hands us the row,
    // so compare it directly instead of re-deriving it with a linear rowOfTab() scan per candidate.
    auto const doomed =
        gatherSessionsOfTabsWhere(*win, [index](int row, vtmux::Tab*) { return row > index; });
    _model->closeTabsToRight(window, anchor->id());
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

    // Split beside the pane the keybinding actually fired from. _model->splitActivePane splits the tab's
    // model-active pane, so if the acting session's pane is not already the active one (focus vs.
    // model-active can diverge in a multi-pane tab), make it active first — otherwise both the split and
    // the inherited cwd below would target the wrong pane. When acting is null (QML) or not a leaf of this
    // tab, fall back to the tab's current active pane.
    if (acting != nullptr)
        if (auto* actingLeaf = tab->rootPane()->findLeaf(acting->modelSessionId()); actingLeaf != nullptr)
            _model->setActivePane(tab->id(), actingLeaf->id());

    // Inherit the working directory AND the grid size of the pane we are splitting from. Use the
    // session's own accessors so the split pane inherits both on every platform — the previous inline
    // extraction lacked the Windows currentWorkingDirectory() fallback, so a split pane silently did
    // not inherit the cwd on Windows while a new tab/window did. Splitting always happens inside a live
    // window, so a running pane exists to inherit the size from (initialPageSize's profile fallback is
    // reached only for a brand-new window's very first pane).
    std::optional<std::string> cwd;
    std::optional<vtbackend::PageSize> runningPageSize;
    if (auto* activeSession = sessionForId(tab->activePane()->session()))
    {
        cwd = activeSession->workingDirectory();
        runningPageSize = activeSession->terminal().totalPageSize();
    }
    auto const pageSize = geometry::initialPageSize(runningPageSize, _app.profile().terminalSize.value());

    // Create the backing Qt session BEFORE the model split. _model->splitActivePane synchronously
    // fires paneSplit/activePaneChanged -> rebuildActiveTabPaneProxies, which binds the new pane's
    // QML to sessionForId(newSessionId). If the session were registered only afterwards (the old
    // order), that binding would resolve to nullptr and the pane would render permanently blank with
    // nothing re-notifying the proxy. createBackingSession registers the id and stages it as the
    // _pendingSessionId, so the model allocator (invoked inside splitActivePane) hands back exactly
    // this id — same backing-session-first order as createSessionInBackground/createTab.
    auto const newSessionId = vtmux::SessionId { _nextSessionId++ };
    createBackingSession(newSessionId, std::move(cwd), pageSize);

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
    _model->closePane(_model->windowOfTab(tab->id()), tab->id(), active->id());
    if (auto* session = sessionForId(sessionId))
        session->terminate();
}

void TerminalSessionManager::focusPane(vtmux::FocusDirection direction, TerminalSession* acting)
{
    if (auto* tab = paneActionTargetTab(acting))
        _model->focusDirection(tab->id(), direction);
}

void TerminalSessionManager::swapPane(vtmux::FocusDirection direction, TerminalSession* acting)
{
    if (auto* tab = paneActionTargetTab(acting))
        _model->swapActivePane(tab->id(), direction);
}

void TerminalSessionManager::movePane(vtmux::FocusDirection direction, TerminalSession* acting)
{
    if (auto* tab = paneActionTargetTab(acting))
        _model->moveActivePane(tab->id(), direction);
}

void TerminalSessionManager::toggleActivePaneOrientation(TerminalSession* acting)
{
    if (auto* tab = paneActionTargetTab(acting))
        _model->toggleActivePaneOrientation(tab->id());
}

void TerminalSessionManager::resizeActivePane(vtmux::FocusDirection direction,
                                              double fraction,
                                              TerminalSession* acting)
{
    if (auto* tab = paneActionTargetTab(acting))
        _model->resizeActivePane(tab->id(), direction, fraction);
}

void TerminalSessionManager::toggleActivePaneZoom(TerminalSession* acting)
{
    if (auto* tab = paneActionTargetTab(acting))
        _model->toggleActivePaneZoom(tab->id());
}
// }}}

// {{{ PaneProxy support
bool TerminalSessionManager::isActivePane(vtmux::TabId tab, vtmux::PaneId id) const noexcept
{
    // Keyed by the proxy's OWN tab (not "the active tab"), so a proxy of any window's tab answers
    // correctly — a divider drag or pane click in a second OS window must not consult the first
    // window's active tab.
    if (auto* t = _model->findTab(tab))
        return t->activePane() != nullptr && t->activePane()->id() == id;
    return false;
}

void TerminalSessionManager::setPaneRatio(vtmux::TabId tab, vtmux::PaneId id, double ratio)
{
    if (_model->findTab(tab) != nullptr)
        _model->setPaneRatio(tab, id, ratio);
}

void TerminalSessionManager::activatePane(vtmux::TabId tab, vtmux::PaneId id)
{
    if (_model->findTab(tab) != nullptr)
        _model->setActivePane(tab, id);
}

// }}}

} // namespace contour
