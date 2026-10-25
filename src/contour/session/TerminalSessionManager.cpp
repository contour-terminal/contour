// SPDX-License-Identifier: Apache-2.0
#include <contour/ContourGuiApp.hpp>
#include <contour/config/LayoutBuilder.hpp>
#include <contour/platform/ColorConversion.hpp>
#include <contour/platform/QtPath.hpp>
#include <contour/session/Logging.hpp>
#include <contour/session/PaneProxy.hpp>
#include <contour/session/TerminalSession.hpp>
#include <contour/session/TerminalSessionManager.hpp>
#include <contour/window/SettingsController.hpp>
#include <contour/window/TabLabel.hpp>
#include <contour/window/WindowController.hpp>

#include <vtbackend/core/Primitives.hpp>

#include <vtpty/Process.hpp>
#ifdef VTPTY_LIBSSH2
    #include <vtpty/SshSession.hpp>
#endif

#include <core/Utf8Path.hpp>
#include <core/platform/PathUtils.hpp>

#include <QtQml/QQmlEngine>
#include <QtQuick/QQuickWindow>

#include <chrono>
#include <exception>
#include <filesystem>
#include <format>
#include <ranges>
#include <string>
#include <utility>

using namespace std::string_literals;

namespace contour::session
{

namespace
{
    /// The working directory a new pane asks for: the one its command names, where it names one -- a
    /// file_opener pane works where its file is -- else the one inherited from the pane it opens beside.
    /// @param command What the pane runs instead of a shell, if anything.
    /// @param inherited The working directory of the pane it opens beside, if known.
    /// @return The directory to ask the session factory for, as UTF-8.
    [[nodiscard]] std::optional<std::string> paneWorkingDirectory(
        std::optional<vtpty::Process::ExecInfo> const& command, std::optional<std::string> inherited)
    {
        if (command && !command->workingDirectory.empty())
            return core::pathToUtf8(command->workingDirectory);
        return inherited;
    }
} // namespace

TerminalSessionManager::TerminalSessionManager(ContourGuiApp& app,
                                               SessionFactory& factory,
                                               config::LayoutStore& layouts,
                                               command::CommandHistoryStore& commands,
                                               std::string homeDirectory,
                                               platform::TaskbarProgressSink& taskbarProgressSink):
    _app { app },
    _sessionFactory { factory },
    _layoutStore { layouts },
    _commandHistoryStore { commands },
    _taskbarProgressSink { taskbarProgressSink },
    _homeDirectory { std::move(homeDirectory) },
    // Not seeded here, and capacity deliberately left at 0: the config file has not been loaded yet at
    // this point (ContourGuiApp constructs the manager before it reads the file), so the configured
    // `command_palette_recent_count` is not yet known. Seeding now would read the stored list and
    // truncate it to the DEFAULT capacity, permanently dropping entries a user with a larger
    // recent_count had every right to keep. The history is therefore loaded lazily, on the first
    // palette open, once the real capacity is known — see openCommandPalette().
    _commandHistory { 0 },
    _earlyExitThreshold {},
    _configFileWatcher {},
    _configReloadTimer {}
{
    _taskbarProgressTimer.setSingleShot(true);
    connect(&_taskbarProgressTimer, &QTimer::timeout, this, &TerminalSessionManager::refreshTaskbarProgress);
    // Long enough to take in every notification of one save, short enough to read as immediate.
    constexpr auto ConfigReloadCoalescing = std::chrono::milliseconds { 100 };
    _configReloadTimer.setSingleShot(true);
    _configReloadTimer.setInterval(ConfigReloadCoalescing);
    connect(&_configReloadTimer, &QTimer::timeout, this, [this] {
        if (auto const reloaded = reloadAllSessions(); !reloaded)
            errorLog()("Failed to reload configuration. {}", reloaded.error());
    });

    // The model allocates a fresh vtworkspace session id whenever it needs one (a new tab or a new split
    // pane). For tabs we pre-mint the id in createSessionInBackground() and hand it back here; for
    // splits (Phase 2) there is no pre-minted id, so we mint a fresh one. Either way the manager
    // then maps the id to a backing TerminalSession.
    _model = std::make_unique<vtworkspace::SessionModel>(*this, [this]() -> vtworkspace::SessionId {
        if (_pendingSessionId.has_value())
        {
            auto const id = *_pendingSessionId;
            _pendingSessionId.reset();
            return id;
        }
        return vtworkspace::SessionId { _nextSessionId++ };
    });
    _model->setSessionTitleResolver([this](vtworkspace::SessionId id) -> std::string {
        if (auto* session = sessionForId(id))
            if (auto name = session->name())
                return *name;
        return {};
    });
}

TerminalSession* TerminalSessionManager::sessionForId(vtworkspace::SessionId id) const noexcept
{
    auto const it = _sessionsById.find(id.value);
    return it != _sessionsById.end() ? it->second : nullptr;
}

TerminalSession* TerminalSessionManager::sessionForPty(vtpty::Pty const* pty) const noexcept
{
    auto const sessions = _sessionsById | std::views::values;
    auto const found = std::ranges::find_if(
        sessions, [pty](TerminalSession const* session) { return &session->terminal().device() == pty; });
    return found != sessions.end() ? *found : nullptr;
}

vtpty::Pty const* TerminalSessionManager::actingPtyOfWindow(vtworkspace::WindowId window) const
{
    auto* win = _model->window(window);
    if (win == nullptr)
        return nullptr;
    // The active tab's active pane first — that is the pane the user is looking at, so it is the
    // most faithful "this window" the remote side can be given. Any live leaf of the window would
    // resolve to the same window on the daemon, so the scan below is a fallback for a window whose
    // active pane happens to have no live session (a tab mid-teardown), not a different answer.
    auto const ptyOf = [this](vtworkspace::SessionId id) -> vtpty::Pty const* {
        auto* session = sessionForId(id);
        return session != nullptr ? &session->terminal().device() : nullptr;
    };
    if (auto* tab = win->activeTab(); tab != nullptr)
        if (auto* leaf = tab->activePane(); leaf != nullptr)
            if (auto const* pty = ptyOf(leaf->session()); pty != nullptr)
                return pty;
    for (auto const row: std::views::iota(0, win->tabCount()))
        if (auto* tab = win->tabAt(row); tab != nullptr)
            for (auto* session: sessionsInTab(tab))
                if (session != nullptr)
                    return &session->terminal().device();
    return nullptr;
}

int TerminalSessionManager::rowOfTab(vtworkspace::TabId tab) const noexcept
{
    // Window-agnostic: resolve the tab's OWNING window through the model, so the row is correct for
    // any tab of any OS window.
    auto* win = _model->window(_model->windowOfTab(tab));
    return win != nullptr ? win->indexOf(tab) : -1;
}

window::WindowController* TerminalSessionManager::createWindowController()
{
    // Every OS window gets its own vtworkspace::Window; the controller adapts exactly that window (all its
    // reads and writes are keyed by the WindowId).
    auto* window = _model->createWindow();
    auto initialPosition = std::optional<window::WindowPosition> {};
    // Restore only the first window. Additional windows keep the existing spawn-screen policy.
    if (_app.config().rememberWindowPosition.value() && _controllersByWindow.empty()
        && !_app.config().configFile.empty() && QGuiApplication::platformName() != QStringLiteral("wayland"))
    {
        if (auto saved = _app.windowPositionStore().load(_app.config().configFile.parent_path()
                                                         / window::WindowPositionFileName))
            initialPosition = *saved;
        else
            errorLog()("Failed to load window position: {}", saved.error());
    }
    auto* controller = new window::WindowController(*this, window->id(), initialPosition);
    QQmlEngine::setObjectOwnership(controller, QQmlEngine::CppOwnership);
    _controllersByWindow[window->id().value] = controller;
    // A freshly spawned window is the one the user is looking at. Seeding ownership here (rather than
    // waiting for the first Qt focus-in) is what lets the very first tab be focused at creation, and
    // makes a tab switch notify the shell even before the window has ever been clicked.
    _focusedWindow = window->id();
    return controller;
}

window::WindowController* TerminalSessionManager::controllerFor(vtworkspace::WindowId window) const noexcept
{
    auto const it = _controllersByWindow.find(window.value);
    return it != _controllersByWindow.end() ? it->second : nullptr;
}

window::WindowController* TerminalSessionManager::controllerForDisplay(
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

display::WindowHost* TerminalSessionManager::windowHostForDisplay(
    display::TerminalDisplay* display) const noexcept
{
    // The upcast is the whole point: it happens HERE, in the one place that already knows both types,
    // so display/ never has to.
    return controllerForDisplay(display);
}

TerminalSession* TerminalSessionManager::createSessionInBackground(
    vtworkspace::WindowId window,
    std::optional<std::string> const& profileName,
    std::optional<vtpty::Process::ExecInfo> const& command,
    SessionRole role)
{
    // TODO: Remove dependency on app-knowledge and pass shell / terminal-size instead.
    // The GuiApp *or* (Global)Config could be made a global to be accessible from within QML.

    if (!_sessionFactory.canCreateSession())
    {
        managerLog()("Refusing to create a session: the session factory cannot back one right now.");
        return nullptr;
    }

    if (!_focusedWindow)
    {
        managerLog()("No focused window found. something went wrong.");
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
    auto const sessionId = vtworkspace::SessionId { _nextSessionId++ };
    auto* session = createBackingSession(
        sessionId, paneWorkingDirectory(command, std::move(ptyPath)), pageSize, command, profileName, role);

    // Mirror this new session into the vtworkspace model as a new single-pane tab.
    if (auto* tab = _model->createTab(window))
        _tabBySession[sessionId.value] = tab->id();

    return session;
}

TerminalSession* TerminalSessionManager::createBackingSession(
    vtworkspace::SessionId sessionId,
    std::optional<std::string> cwd,
    std::optional<vtbackend::PageSize> pageSize,
    std::optional<vtpty::Process::ExecInfo> const& commandOverride,
    std::optional<std::string> const& profileName,
    SessionRole role)
{
    // The chokepoint guard: every creation path funnels through here, so a
    // future entry point that forgets its own fail-fast check still cannot
    // mint a backing pty the factory refuses (e.g. a local shell inside an
    // attach-mode mirror window). The entry points keep their earlier checks
    // for whole-operation fail-fast semantics.
    if (!_sessionFactory.canCreateSession())
    {
        managerLog()("Refusing to create a backing session: the factory cannot back one right now.");
        return nullptr;
    }

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
                            std::move(launchedCommand),
                            _sessionFactory.createNotifier(),
                            role,
                            _sessionFactory.hosting());
    // The manager owns what it makes: removeSession() frees a session that has ended, and those still
    // running go with the manager, along with whatever they had queued for themselves.
    session->setParent(this);
    managerLog()("Create backing session with ID {}({}); {} sessions before it.",
                 session->id(),
                 (void*) session,
                 _sessionsById.size());
    // Close the cycle the constructor could not: the factory handed out a pty, and a
    // daemon-backed one needs the terminal built around it to populate its grid.
    _sessionFactory.bindTerminal(&session->terminal().device(), session->terminal());

    _pendingSessionId = sessionId;
    _sessionsById[sessionId.value] = session;
    session->setModelSessionId(sessionId);

    connect(session, &TerminalSession::sessionClosed, [this, session]() { removeSession(*session); });
    watchConfigFile();

    // Nor is it QML's to collect: QQmlEngine took sessions it was handed for garbage at random times, while
    // they were still in use.
    QQmlEngine::setObjectOwnership(session, QQmlEngine::CppOwnership);

    return session;
}

void TerminalSessionManager::detachDisplay(display::TerminalDisplay* display) noexcept
{
    if (display == nullptr)
        return;

    // The display (one pane of a tab) is being destroyed. Session->display ownership lives solely on the
    // pane tree now, so there is no per-display map to scrub; just make sure no controller keeps this
    // freed display as its focused one. Focus ownership is per WINDOW and survives this untouched.
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

void TerminalSessionManager::focusOnDisplay(display::TerminalDisplay* display)
{
    managerLog()("Setting active display to {}", (void*) display);

    // Session->display ownership lives solely on the pane tree (the QML `session:` binding -> setSession);
    // a focused pane already owns its session. focusOnDisplay only makes this the active display and hands
    // the focus to the owning window's controller, which re-points its window-service signal bridge and
    // re-emits the window bindings. Route by the display's OS window so the correct controller updates.
    if (auto* c = controllerForDisplay(display))
    {
        // A genuine Qt focus-in transfers focus ownership to this display's window. The session is not
        // re-pointed here: the caller (TerminalDisplay::focusInEvent) supplies its own pane's session,
        // which mid-rebind can differ from the controller's active one.
        _focusedWindow = c->windowId();
        c->focusDisplay(display);
    }
}

void TerminalSessionManager::setFocusedSession(TerminalSession* next)
{
    if (next == _focusedSession)
        return;
    // With `theme: terminal` the chrome follows the focused session's live background. Read before the
    // focus moves, under that session's terminal lock, on this (the GUI) thread; under any other theme
    // it is not read at all.
    auto const nextBackground = next != nullptr && _app.config().theme.value() == config::GuiTheme::Terminal
                                    ? std::optional { next->liveBackground() }
                                    : std::nullopt;
    if (_focusedSession != nullptr)
        _focusedSession->sendFocusOutEvent();
    _focusedSession = next;
    if (_focusedSession != nullptr)
        _focusedSession->sendFocusInEvent();
    if (nextBackground.has_value())
        _app.followLiveBackground(*nextBackground);
}

void TerminalSessionManager::sessionBackgroundChanged(vtworkspace::SessionId id, vtbackend::RGBColor color)
{
    // An unregistered session carries the empty id, which must not match a focused one that also does.
    if (_focusedSession == nullptr || id == vtworkspace::SessionId {}
        || _focusedSession->modelSessionId() != id)
        return;
    _app.followLiveBackground(color);
}

void TerminalSessionManager::clearFocusIfCurrent(TerminalSession* session)
{
    if (session != nullptr && session == _focusedSession)
        setFocusedSession(nullptr);
}

void TerminalSessionManager::setFocusedWindow(vtworkspace::WindowId window)
{
    // Taking ownership satisfies syncFocusForWindow's gate by construction, so the re-pointing half is
    // that one implementation rather than a second copy of it.
    _focusedWindow = window;
    syncFocusForWindow(controllerFor(window));

    // Activated with focus where it already was, as after a window manager that kept it through a
    // minimise: no focus-in tells the pane it is being looked at.
    focusedSessionSeen();
}

void TerminalSessionManager::windowStateChanged(vtworkspace::WindowId window)
{
    if (_focusedWindow == window)
        focusedSessionSeen();
}

void TerminalSessionManager::focusedSessionSeen()
{
    if (_focusedSession != nullptr
        && sessionVisibility(*_focusedSession) == platform::SessionVisibility::Focused)
        _focusedSession->paneSeen();
}

void TerminalSessionManager::clearFocusedWindow(vtworkspace::WindowId window)
{
    if (_focusedWindow != window) // guarded like clearFocusIfCurrent
        return;
    _focusedWindow.reset();
    setFocusedSession(nullptr);
}

void TerminalSessionManager::syncFocusForWindow(window::WindowController* controller)
{
    if (controller == nullptr)
        return;
    // Only the focus-owning window moves focus, so a background window can't steal it. This is also the
    // seam that focuses a session swapped onto an already-focused display, where no Qt focus event
    // fires at all -- which is the whole reason a tab switch notifies anyone. @see _focusedWindow.
    if (_focusedWindow == controller->windowId())
        setFocusedSession(controller->activeSession());
}

platform::SessionVisibility TerminalSessionManager::sessionVisibility(TerminalSession const& session) const
{
    auto const [tab, win, controller] = hostOf(&session);

    // A zoomed tab draws its active pane alone, so every other pane of it is off screen.
    auto const paneShown =
        tab != nullptr && (!tab->isZoomed() || tab->activePane()->session() == session.modelSessionId());

    // The settings page replaces the window's whole content area, so while it shows, no tab is the one
    // on screen and no terminal is what the user is typing into -- the rule the strip's IsActiveRole
    // applies too. A session no window hosts has no settings page to be covered by.
    auto const contentShown = controller == nullptr || !controller->settingsActive();

    return platform::sessionVisibilityFor(platform::SessionPlacement {
        .focus = contentShown && &session == _focusedSession ? platform::SessionFocus::Here
                                                             : platform::SessionFocus::Elsewhere,
        .tab = contentShown && win != nullptr && win->activeTab() == tab ? platform::TabSelection::Active
                                                                         : platform::TabSelection::Background,
        .pane = paneShown ? platform::PaneExposure::Shown : platform::PaneExposure::Covered,
        .window = controller != nullptr && controller->isMinimized() ? platform::WindowPresence::Minimized
                                                                     : platform::WindowPresence::Shown,
    });
}

std::string TerminalSessionManager::tabTitleForSession(TerminalSession const& session) const
{
    // One row, so one terminal lock: every other tab's parser may be mid-write.
    auto const host = hostOf(&session);
    return host.controller != nullptr ? host.controller->tabTitleOf(host.tab->id()) : std::string {};
}

void TerminalSessionManager::revealSession(TerminalSession const& session)
{
    auto const [tab, win, controller] = hostOf(&session);
    if (win == nullptr)
        return;

    // The pane first: while its tab is still in the background, syncFocusForWindow finds the window's
    // active session unchanged, so focus does not move. Activating the tab first would focus the tab's
    // previously active pane on the way, and a DECSET 1004 application there would read a CSI I and then
    // a CSI O for a pane the user never looked at.
    activateSessionPane(*tab, session);

    // The settings page covers every tab, so it closes before the tab becomes the one on screen -- as
    // the strip's own activateTab does.
    if (controller != nullptr)
        controller->closeSettings();

    _model->activateTab(win->id(), tab->id());

    // Terminal focus follows through syncFocusForWindow: at once when this window owns focus, otherwise
    // once the activation below lands and the window takes focus.
    if (controller != nullptr)
        controller->raiseAndActivate();
}

void TerminalSessionManager::requestWindowNotification(TerminalSession const& session,
                                                       QString const& title,
                                                       QString const& body,
                                                       std::function<void()> onClicked)
{
    if (auto* controller = controllerHostingSession(&session))
        controller->requestNotification(title, body, std::move(onClicked));
}

void TerminalSessionManager::requestWindowBell(TerminalSession const& session,
                                               QString const& source,
                                               double volume)
{
    if (auto* controller = controllerHostingSession(&session))
        controller->requestBell(source, volume);
}

void TerminalSessionManager::requestWindowAlert(TerminalSession const& session)
{
    if (auto* controller = controllerHostingSession(&session))
        controller->requestAlert();
}

TerminalSession* TerminalSessionManager::createSession(vtworkspace::WindowId window,
                                                       std::optional<std::string> const& profileName)
{
    // Just create the backing session + its model tab. The model's activeTabChanged fires the owning
    // controller's proxy rebuild, and the pane tree's `session:` binding attaches the session to its
    // display — no legacy activateSession display-assignment.
    return createSessionInBackground(window, profileName);
}

void TerminalSessionManager::createNewTab(TerminalSession* acting,
                                          std::optional<std::string> const& profileName)
{
    // The CreateNewTab keybinding: the new tab belongs to the window the user typed in.
    if (auto* win = windowHostingSession(acting))
        createNewTab(win->id(), profileName);
}

void TerminalSessionManager::closeAllTabs(TerminalSession* acting)
{
    if (auto* controller = controllerHostingSession(acting))
        controller->closeWindow();
}

bool TerminalSessionManager::applyLayoutToWindow(
    vtworkspace::WindowId window,
    config::Layout const& layout,
    std::optional<vtbackend::PageSize> pageSize,
    std::function<void(config::LayoutPane const&)> const& beforeLeafSeed)
{
    if (layout.tabs.empty())
    {
        managerLog()("Layout has no tabs; nothing to apply.");
        return false;
    }
    if (!_sessionFactory.canCreateSession())
    {
        managerLog()("Refusing to apply a layout: the session factory cannot back new sessions right now.");
        return false;
    }
    for (auto const& tabSpec: layout.tabs)
    {
        // The seeder stages a backing session for each pane right before the model allocates it,
        // exactly like createBackingSession's use in splitActivePane/createSessionInBackground.
        auto seeder = [&](config::LayoutPane const& leaf) -> bool {
            // Attach mode binds the pane about to be born to a specific remote
            // session here, before its backing session (and thus its ChannelPty) is
            // created — see NativeController.
            if (beforeLeafSeed)
                beforeLeafSeed(leaf);
            auto const sessionId = vtworkspace::SessionId { _nextSessionId++ };
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
            // The directory as every new tab's and split's cwd travels: in UTF-8.
            auto cwd = leaf.directory.transform(
                [](std::filesystem::path const& directory) { return core::pathToUtf8(directory); });
            // Report whether the backing session was actually created: a nullptr (the factory
            // refused, e.g. an attach-mode pool that ran dry) stops the realizer before it
            // allocates a pane with no session behind it — the same guard the split path uses.
            return createBackingSession(sessionId, std::move(cwd), pageSize, command, profileName) != nullptr;
        };

        auto* modelTab = vtworkspace::realizeLayoutTab(*_model, window, tabSpec, seeder);
        _pendingSessionId.reset(); // consumed by the allocator; clear any leftover
        if (modelTab != nullptr)
        {
            // Map every leaf session in the new tab to this tab id (mirrors createSessionInBackground).
            modelTab->rootPane()->walkTree([&](vtworkspace::Pane& p) {
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

bool TerminalSessionManager::consumeDefaultLayout(contour::window::WindowController* controller)
{
    // One-shot: Main.qml calls this for EVERY window it loads, but the startup layout belongs to
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

std::expected<void, std::string> TerminalSessionManager::reloadAllSessions()
{
    // Read once, into a Config of its own, so a file that fails to load leaves the app's configuration
    // and every session as they were rather than half-replaced.
    auto loaded = config::tryLoadConfigFromFile(_app.config().configFile);
    if (!loaded)
        return std::unexpected { std::move(loaded.error()) };

    // The master config first, so the app — the next new tab, and the settings page that reads it — sees
    // the new file and side files immediately. Then each live session applies it under its own profile,
    // updating the terminals.
    auto const rowSnappingChanged = _app.config().snapToRows.value() != loaded->snapToRows.value();
    _app.config() = *std::move(loaded);
    for (auto& [id, session]: _sessionsById)
        if (session != nullptr)
            session->applyReloadedConfig(_app.config(), session->profileName());

    // Once per reload, not per session: with `theme: terminal` the chrome's light/dark follows the
    // terminal background, so a hand-edited colour change must re-evaluate it as well as the terminals.
    // Over the focused session's live background, which an OSC 11 may have moved off the configured one.
    _app.applyGuiTheme(_app.config().theme.value(),
                       _app.defaultProfileColors(),
                       _focusedSession != nullptr ? std::optional { _focusedSession->liveBackground() }
                                                  : std::nullopt);
    // And what else derives from the configuration: the chrome tokens (a reload may change the chrome font
    // and the scheme the Terminal row paints with) and the new-defaults banner (it may change the edition).
    _app.onConfigurationReloaded();

    // The settings page reads through the app config just reloaded above, but each OS window owns its
    // own SettingsController with model caches rebuilt only by refresh(). The controller that triggered
    // the save refreshes itself; refresh every OTHER window's too, so a settings page open in a second
    // window reflects the change immediately instead of serving stale caches until it is reopened.
    for (auto& [windowId, controller]: _controllersByWindow)
        if (controller != nullptr)
        {
            if (auto* settings = controller->settingsController(); settings != nullptr)
                settings->refresh();

            // The tab bar is window state, seeded first-write-wins so a session rebind cannot reset a
            // runtime override. That latch also swallowed a reload, so changing tab_bar_visibility did
            // nothing until restart. A reload is the one moment the configured value must win.
            controller->applyTabBarFromConfig(_app.config().tabBarPosition.value(),
                                              _app.config().tabBarVisibility.value());
            if (rowSnappingChanged)
                if (auto* session = controller->activeSession(); session != nullptr)
                    if (auto* display = session->display(); display != nullptr)
                        controller->updateSizeHintsFor(
                            *display, window::WindowController::HintApplyMode::RespectWindowState);
        }
    scheduleTaskbarProgress();
    return {};
}

void TerminalSessionManager::watchConfigFile()
{
    if (_configFileWatcher || !_app.liveConfig())
        return;

    auto const& file = _app.config().configFile;
    managerLog()("Enable live configuration reloading of file {}.", core::platform::normalizePath(file));
    _configFileWatcher = std::make_unique<QFileSystemWatcher>();
    _configFileWatcher->addPath(platform::toQString(file));
    connect(_configFileWatcher.get(),
            &QFileSystemWatcher::fileChanged,
            this,
            &TerminalSessionManager::onConfigFileChanged);
}

void TerminalSessionManager::onConfigFileChanged(QString const& path)
{
    if (!_configFileWatcher->files().contains(path))
        _configFileWatcher->addPath(path);
    _configReloadTimer.start();
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
    if (auto const* tab = paneActionTargetTab(acting); tab != nullptr)
        activateSessionPane(*tab, *acting);

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

std::expected<void, config::LayoutSaveError> TerminalSessionManager::saveWindowLayout(
    vtworkspace::WindowId windowId, std::string const& name)
{
    if (name.empty())
        return std::unexpected(config::LayoutSaveError::EmptyName);

    auto* window = _model->window(windowId);
    if (window == nullptr)
        return std::unexpected(config::LayoutSaveError::UnknownWindow);

    auto const resolve = [this](vtworkspace::SessionId id) {
        vtworkspace::PaneLeafData data;
        if (auto* session = sessionForId(id))
        {
            if (auto const dir = session->workingDirectory(); !dir.empty())
                data.directory = dir;
            // An engaged override with an EMPTY program runs the profile's default shell, so there
            // is no command worth persisting for it. Nor is a pager's: it names a private file that is
            // gone by the time the layout is restored, so the pane comes back as the profile's shell.
            if (auto const& launched = session->launchedCommand();
                launched && !launched->program.empty() && session->role() == SessionRole::Shell)
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
            layout.tabs.push_back(vtworkspace::serializeTab(*tab, resolve));

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
                     core::pathToUtf8(path),
                     storedLayouts.error());
        return std::unexpected(config::LayoutSaveError::StoreUnreadable);
    }
    (*storedLayouts)[name] = layout;

    if (auto const written = _layoutStore.save(path, *storedLayouts); !written)
    {
        managerLog()("Failed to save layout '{}': {}", name, written.error());
        return std::unexpected(config::LayoutSaveError::WriteFailed);
    }

    // Only now that the store has accepted the layout do we update the in-memory config, so a
    // subsequent LaunchLayout in this run sees the save, and runtime state never diverges from
    // what is actually persisted. This merges the new entry into the existing in-memory (inline +
    // file) view rather than replacing it wholesale, so inline layouts stay visible in this run
    // even though they were deliberately excluded from the store just written.
    _app.config().layouts.value()[name] = std::move(layout);

    managerLog()("Saved layout '{}' to {}", name, core::pathToUtf8(path));
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

void TerminalSessionManager::runTabAction(actions::Action const& action, TerminalSession* acting)
{
    auto* controller = controllerHostingSession(acting);
    auto const* tab = tabHostingSession(acting);
    if (controller != nullptr && tab != nullptr)
        controller->runTabAction(tab->id(), action);
}

void TerminalSessionManager::showAbout(TerminalSession* acting)
{
    if (auto* controller = controllerHostingSession(acting); controller != nullptr)
        controller->showAbout();
}

void TerminalSessionManager::minimizeWindow(TerminalSession* acting)
{
    if (auto* controller = controllerHostingSession(acting); controller != nullptr)
        controller->minimizeWindow();
}

void TerminalSessionManager::zoomWindow(TerminalSession* acting)
{
    if (auto* controller = controllerHostingSession(acting); controller != nullptr)
        controller->toggleMaximized();
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

void TerminalSessionManager::removeWindowController(vtworkspace::WindowId windowId)
{
    // Drop the controller for a closed OS window. Real per-window identity: "last window" is
    // _controllersByWindow.size() == 1 (the manager's authoritative window registry), NOT a scan of
    // display keys. The controller has already removed its vtworkspace::Window, pruned its proxy tree and
    // terminated its own sessions (WindowController::closeWindow); here we just unregister + delete it,
    // and — if this was the last window — clear any residual shared session registries so no stale entry
    // outlives the process's last window.
    auto const it = _controllersByWindow.find(windowId.value);
    if (it == _controllersByWindow.end())
        return;

    bool const wasLast = _controllersByWindow.size() == 1;
    auto* controller = it->second;
    _controllersByWindow.erase(it);
    _taskbarProgressSink.removeWindow(windowId.value);
    if (controller != nullptr)
        controller->deleteLater();

    // Ownership must not survive the window that held it: a stale WindowId would let a later model
    // event in an unrelated window re-grant focus through syncFocusForWindow. The window's sessions
    // were terminated by WindowController::closeWindow, and removeSession already dropped the focus
    // back-pointer for each, so the focus-out this issues finds nothing to notify.
    clearFocusedWindow(windowId);

    if (wasLast)
    {
        managerLog()("Last window closed: clearing residual session registries.");
        _sessionsById.clear();
        _tabBySession.clear();
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
    terminateSessions(sessionsInTab(tab), SessionEnd::Destroy);
}

void TerminalSessionManager::moveTabByTab(vtworkspace::Tab* tab, int targetRow)
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

    // And forget whatever progress it was reporting: with no expiry configured (the default) a
    // report is kept until it is withdrawn, so a pane closed mid-operation would otherwise keep
    // contributing to the window's taskbar aggregate for the life of the process.
    _taskbarProgress.onSessionClosed(sessionId.value);

    // Mirror the removal into the vtworkspace model FIRST, while the registry still resolves this session
    // to its tab/leaf, then erase the local bookkeeping. A session is one *pane*; closing it must
    // close only that pane. closePane() absorbs the surviving sibling when the tab still has other
    // panes and only tears down the whole tab (firing tabClosed -> beginRemoveRows) when this was
    // the tab's last pane — so closing one split pane no longer destroys its siblings.
    if (auto* tab = findTabHostingSession(sessionId); tab != nullptr)
        if (auto* leaf = tab->rootPane()->findLeaf(sessionId); leaf != nullptr)
            _model->closePane(_model->windowOfTab(tab->id()), tab->id(), leaf->id());
    _sessionsById.erase(sessionId.value);
    _tabBySession.erase(sessionId.value);
    scheduleTaskbarProgress();

    // Session->display ownership is the pane tree's; there is no per-display map to scrub. The pane's
    // model leaf was closed above (collapsing its split); the surviving panes keep their sessions via
    // their own `session:` bindings. Refresh the status line so the closed pane's tab drops from it.
    updateStatusLine();

    // Nothing else frees a session the manager made. Nothing still needs it: its pane's proxy looks it up
    // by id, which now finds nothing, and a display still naming it is let go by ~TerminalSession. Freed
    // later rather than here, since the call that ended it -- onClosed(), the key press acknowledging an
    // early exit -- is still running on it; what it had queued for itself is dropped with it.
    thatSession.deleteLater();
}

void TerminalSessionManager::updateColorPreference(vtbackend::ColorPreference const& preference)
{
    for (auto* session: _sessionsById | std::views::values)
        session->updateColorPreference(preference);
}

void TerminalSessionManager::updateMotionPreference(config::MotionPreference motion)
{
    for (auto* session: _sessionsById | std::views::values)
        session->applyMotionPreference(motion);
}

vtworkspace::Tab* TerminalSessionManager::tabAtRow(vtworkspace::WindowId window, int index) const noexcept
{
    auto* win = _model->window(window);
    return win != nullptr ? win->tabAt(index) : nullptr;
}

vtworkspace::Tab* TerminalSessionManager::findTabHostingSession(vtworkspace::SessionId session) const noexcept
{
    // O(1) via the SessionId -> TabId index.
    if (auto const it = _tabBySession.find(session.value); it != _tabBySession.end())
        return _model->findTab(it->second);
    return nullptr;
}

// {{{ vtworkspace::ModelEvents
//
// Stage 3 of the per-window refactor: the manager hosts the SessionModel and implements ModelEvents, but
// the QML tab strip / pane tree bind to the per-window WindowController. So each handler is now a pure
// ROUTER: it dispatches the Qt row/signal/proxy work to the OWNING window's controller (tab events carry
// the WindowId; pane events resolve TabId -> WindowId via the model) and does no list-model / proxy work of
// its own. The manager's own QAbstractListModel emissions and proxy tree — which Stage 2 kept as a harmless
// unobserved superset — are gone; the status-line fan-out (updateStatusLine) still lives here until Stage 4
// moves it onto the controllers.
void TerminalSessionManager::tabAboutToBeAdded(vtworkspace::WindowId window, int index)
{
    if (auto* c = controllerFor(window))
        c->onTabAboutToBeAdded(index);
}

void TerminalSessionManager::tabAdded(vtworkspace::WindowId window, vtworkspace::TabId id, int index)
{
    if (auto* c = controllerFor(window))
        c->onTabAdded(index);
    if (auto* tab = _model->findTab(id))
        if (auto* session = sessionForId(tab->rootPane()->firstLeaf()->session()))
            if (auto const& color = session->profile().tabColor.value(); color)
                _model->setTabColor(id, vtworkspace::TabColorSource::Profile, *color);
}

void TerminalSessionManager::tabAboutToBeRemoved(vtworkspace::WindowId window, int index)
{
    if (auto* c = controllerFor(window))
        c->onTabAboutToBeRemoved(index);
}

void TerminalSessionManager::tabClosed(vtworkspace::WindowId window, vtworkspace::TabId, int)
{
    if (auto* c = controllerFor(window))
        c->onTabClosed();
}

void TerminalSessionManager::tabAboutToBeMoved(vtworkspace::WindowId window, int fromIndex, int toIndex)
{
    if (auto* c = controllerFor(window))
        c->onTabAboutToBeMoved(fromIndex, toIndex);
}

void TerminalSessionManager::tabMoved(vtworkspace::WindowId window, vtworkspace::TabId, int, int)
{
    if (auto* c = controllerFor(window))
        c->onTabMoved();
}

void TerminalSessionManager::tabAboutToBeMovedToWindow(vtworkspace::WindowId from,
                                                       int fromIndex,
                                                       vtworkspace::WindowId to,
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
    vtworkspace::WindowId from, vtworkspace::TabId, int, vtworkspace::WindowId to, int toIndex)
{
    // Close-half on the source completes its beginRemoveRows; add-half on the destination completes its
    // beginInsertRows (bracketed in tabAboutToBeMovedToWindow with the SAME toIndex). onTabAdded ignores
    // the index — endInsertRows needs none — but we pass the event's toIndex, not a re-derived one.
    if (auto* src = controllerFor(from))
        src->onTabClosed();
    if (auto* dst = controllerFor(to))
        dst->onTabAdded(toIndex);
    scheduleTaskbarProgress();
}

void TerminalSessionManager::activeTabChanged(vtworkspace::WindowId window, vtworkspace::TabId tab, int)
{
    if (auto* c = controllerFor(window))
    {
        c->onActiveTabChanged();
        // Re-point focus at the new active leaf, after the rebuild so activeSession() is current; this
        // also covers cross-window moves, which route through activeTabChanged on the destination.
        syncFocusForWindow(c);
    }
    reportTabPresentation(tab);
}

void TerminalSessionManager::paneSplit(vtworkspace::TabId tab, vtworkspace::PaneId, vtworkspace::PaneId)
{
    if (auto* c = controllerFor(_model->windowOfTab(tab)))
    {
        c->notifyTabRowChanged(
            tab,
            { window::WindowController::Roles::TitleRole, window::WindowController::Roles::PaneCountRole });
        c->rebuildActiveTabPaneProxies();
    }
}

void TerminalSessionManager::paneClosed(vtworkspace::TabId tab, vtworkspace::PaneId, vtworkspace::PaneId)
{
    if (auto* c = controllerFor(_model->windowOfTab(tab)))
    {
        c->notifyTabRowChanged(
            tab,
            { window::WindowController::Roles::TitleRole, window::WindowController::Roles::PaneCountRole });
        c->rebuildActiveTabPaneProxies();
    }
}

void TerminalSessionManager::activePaneChanged(vtworkspace::TabId tab, vtworkspace::PaneId)
{
    if (auto* c = controllerFor(_model->windowOfTab(tab)))
    {
        c->notifyTabRowChanged(tab, { window::WindowController::Roles::TitleRole });
        c->notifyActivePaneChanged();
        c->emitActiveSessionChanged();
        // Move terminal focus to the newly active pane's leaf (symmetric out/in via setFocusedSession).
        syncFocusForWindow(c);
    }
    reportTabPresentation(tab);
}

void TerminalSessionManager::paneRatioChanged(vtworkspace::TabId tab,
                                              vtworkspace::PaneId splitNode,
                                              double ratio)
{
    if (auto* c = controllerFor(_model->windowOfTab(tab)))
        c->notifyRatioChanged(splitNode);
    reportSplitRatio(tab, splitNode, ratio);
}

void TerminalSessionManager::reportSplitRatio(vtworkspace::TabId tab,
                                              vtworkspace::PaneId splitNode,
                                              double ratio)
{
    auto* split = _model->findPane(tab, splitNode);
    if (split == nullptr || split->isLeaf())
        return;

    // Name the split by ANY leaf from each side — the daemon resolves their lowest common ancestor,
    // which is this node whichever leaves the two sides contribute. firstLeaf() is simply the cheapest
    // one to reach; nothing downstream depends on the choice.
    auto const ptyOfLeafUnder = [this](vtworkspace::Pane* child) -> vtpty::Pty const* {
        auto* session = sessionForId(child->firstLeaf()->session());
        return session != nullptr ? &session->terminal().device() : nullptr;
    };

    auto const* first = ptyOfLeafUnder(split->first());
    auto const* second = ptyOfLeafUnder(split->second());
    if (first != nullptr && second != nullptr)
        _sessionFactory.reportSplitRatio(first, second, ratio);
}

void TerminalSessionManager::reportTabPresentation(vtworkspace::TabId tabId)
{
    // Every model event below reaches here, local tabs' included: nothing is computed for those.
    if (_sessionFactory.hosting() != SessionHosting::Remote)
        return;
    auto const* tab = _model->findTab(tabId);
    auto const* window = tab != nullptr ? _model->window(_model->windowOfTab(tabId)) : nullptr;
    if (window == nullptr || tab->activePane() == nullptr)
        return;
    auto const ptyOf = [this](vtworkspace::Pane const* leaf) -> vtpty::Pty const* {
        auto* session = leaf != nullptr ? sessionForId(leaf->session()) : nullptr;
        return session != nullptr ? &session->terminal().device() : nullptr;
    };
    auto const* active = ptyOf(tab->activePane());
    if (active == nullptr)
        return; // the active pane has no session yet: a pane being built, which reports once it has one
    _sessionFactory.reportTabPresentation(LocalTabPresentation {
        .activePty = active,
        .zoomedPty = tab->isZoomed() ? ptyOf(tab->activePane()) : nullptr,
        .title = tab->runtimeTitle(),
        .color = tab->color(vtworkspace::TabColorSource::User).transform([](vtbackend::RGBColor color) {
            return color.value();
        }),
        .activity = window->activeTab() == tab ? vthost::client::TabActivity::Active
                                               : vthost::client::TabActivity::Inactive,
    });
}

void TerminalSessionManager::paneOrientationChanged(vtworkspace::TabId tab,
                                                    vtworkspace::PaneId,
                                                    vtworkspace::SplitState)
{
    // Rebuild the tab's proxy tree: the coarse refresh re-emits every proxy's `changed` (which carries
    // `orientation`), so PaneNode.qml re-reads the flipped axis and re-lays out its SplitView. Same
    // path as paneSplit/paneClosed — the tree is small, so a targeted update buys nothing.
    if (auto* c = controllerFor(_model->windowOfTab(tab)))
        c->rebuildActiveTabPaneProxies();
}

void TerminalSessionManager::paneSwapped(vtworkspace::TabId tab, vtworkspace::PaneId, vtworkspace::PaneId)
{
    // A swap moves sessions between two leaves (ids unchanged). The rebuild re-binds each proxy's
    // `session`, so the two affected panes render their new terminals.
    if (auto* c = controllerFor(_model->windowOfTab(tab)))
        c->rebuildActiveTabPaneProxies();
}

void TerminalSessionManager::paneZoomChanged(vtworkspace::TabId tab, std::optional<vtworkspace::PaneId>)
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
        c->notifyTabRowChanged(tab, { window::WindowController::Roles::ZoomedRole });
        c->refreshActiveTabLayoutRoot();
    }
    reportTabPresentation(tab);
}

void TerminalSessionManager::paneTreeRestructured(vtworkspace::TabId tab)
{
    // A move re-parents nodes and re-homes ids unpredictably, so re-read the whole tab's tree.
    if (auto* c = controllerFor(_model->windowOfTab(tab)))
    {
        c->notifyTabRowChanged(
            tab,
            { window::WindowController::Roles::TitleRole, window::WindowController::Roles::PaneCountRole });
        c->rebuildActiveTabPaneProxies();
    }
}

void TerminalSessionManager::tabTitleChanged(vtworkspace::TabId tab)
{
    // The indicator status line's {Tabs} entry is built from the tab titles, so republish it here
    // (Stage 4 moves this fan-out onto the controllers); otherwise a renamed tab keeps its old name in
    // the status line until an unrelated event next refreshes it.
    updateStatusLine();
    if (auto* c = controllerFor(_model->windowOfTab(tab)))
        c->notifyTabRowChanged(
            tab,
            { window::WindowController::Roles::TitleRole, window::WindowController::Roles::RawTitleRole });
    reportTabPresentation(tab);
}

void TerminalSessionManager::tabColorChanged(vtworkspace::TabId tab)
{
    updateStatusLine();
    if (auto* c = controllerFor(_model->windowOfTab(tab)))
        c->notifyTabRowChanged(tab, { window::WindowController::Roles::ColorRole });
    reportTabPresentation(tab);
}

void TerminalSessionManager::refreshAllTabTitles()
{
    // Every window's tab strip may show {TabPosition}/{WindowTitle}-derived labels; let each
    // controller re-emit for its own rows. (The manager itself no longer owns a list model.)
    for (auto* controller: _controllersByWindow | std::views::values)
        controller->refreshAllTabTitles();
}

void TerminalSessionManager::refreshTabForSession(vtworkspace::SessionId session)
{
    // Route to the OWNING window's controller — that list-model is what the visible tab strip
    // renders from. (Emitting on the manager reached nothing: no QML binds the manager's rows.)
    if (auto* tab = findTabHostingSession(session))
        if (auto* c = controllerFor(_model->windowOfTab(tab->id())))
            c->notifyTabRowChanged(tab->id(), { window::WindowController::Roles::TitleRole });
}

void TerminalSessionManager::refreshTabProgressForSession(vtworkspace::SessionId session)
{
    // Same routing as refreshTabForSession, but only the two progress roles: the tab label is
    // untouched by OSC 9;4, and an application updating progress once a second must not drag the
    // label's re-expansion along with it.
    auto* tab = findTabHostingSession(session);
    if (tab == nullptr)
        return;
    if (auto* c = controllerFor(_model->windowOfTab(tab->id())))
        c->notifyTabRowChanged(tab->id(),
                               { window::WindowController::Roles::ProgressStateRole,
                                 window::WindowController::Roles::ProgressPercentageRole });

    // The window-level aggregate the OS taskbar would show. Fed here, on the GUI thread, because
    // this is the one place a progress change is already known to have landed; the router decides
    // what several sessions reduce to and when a report goes stale, and a platform backend attaches
    // to its answer. Kept current even with no backend present, so adding one is a sink, not a
    // rewiring. @see platform::TaskbarProgressRouter.
    if (auto* s = sessionForId(session))
        recordProgress(session, s->terminal().resolvedProgress(), _app.terminalClocks().steady.now());
}

void TerminalSessionManager::refreshTabProgramStatusForSession(vtworkspace::SessionId session)
{
    // Only the badge's roles, for the reason refreshTabProgressForSession gives.
    auto* tab = findTabHostingSession(session);
    if (tab == nullptr)
        return;
    if (auto* c = controllerFor(_model->windowOfTab(tab->id())))
        c->notifyTabRowChanged(tab->id(),
                               { window::WindowController::Roles::ProgramStatusRole,
                                 window::WindowController::Roles::ProgramStatusTextRole,
                                 window::WindowController::Roles::ProgramStatusProgressRole });
}

platform::TaskbarProgress TerminalSessionManager::taskbarProgress(
    std::chrono::steady_clock::time_point now) const
{
    return _taskbarProgress.resolve(now, _app.config().progressTimeout.value());
}

platform::TaskbarProgress TerminalSessionManager::taskbarProgress(
    vtworkspace::WindowId window, std::chrono::steady_clock::time_point now) const
{
    return _taskbarProgress.resolveFor(
        [this, window](uint64_t session) {
            auto const found = _tabBySession.find(session);
            return found != _tabBySession.end() && _model->windowOfTab(found->second) == window;
        },
        now,
        _app.config().progressTimeout.value());
}

void TerminalSessionManager::bindTaskbarWindow(vtworkspace::WindowId window, QWindow& nativeWindow)
{
    _taskbarProgressSink.bindWindow(window.value, nativeWindow);
    scheduleTaskbarProgress();
}

void TerminalSessionManager::scheduleTaskbarProgress()
{
    if (_taskbarProgressSink.isSupported())
        _taskbarProgressTimer.start(0);
}

void TerminalSessionManager::refreshTaskbarProgress()
{
    auto const now = _app.terminalClocks().steady.now();
    if (_taskbarProgressSink.scope() == platform::TaskbarProgressScope::Application)
        _taskbarProgressSink.setApplicationProgress(taskbarProgress(now));
    else
        for (auto const& [window, controller]: _controllersByWindow)
            _taskbarProgressSink.setProgress(window, taskbarProgress(vtworkspace::WindowId { window }, now));
    if (auto const expiry = _taskbarProgress.nextExpiry(now, _app.config().progressTimeout.value()))
        _taskbarProgressTimer.start(std::chrono::ceil<std::chrono::milliseconds>(*expiry - now));
}

void TerminalSessionManager::setTabTitleForSession(vtworkspace::SessionId session, std::string title)
{
    if (auto* tab = findTabHostingSession(session))
        _model->setTabTitle(tab->id(), std::move(title));
}

void TerminalSessionManager::setTabColorForSession(vtworkspace::SessionId session, vtbackend::RGBColor color)
{
    // Reuse the authoritative tab-color path: SessionModel::setTabColor fires tabColorChanged, which
    // this manager routes to the owning WindowController's ColorRole, repainting the tab strip. The
    // Application source keeps this write off the user's own color slot, which outranks it.
    if (auto* tab = findTabHostingSession(session))
        _model->setTabColor(tab->id(), vtworkspace::TabColorSource::Application, color);
}

void TerminalSessionManager::resetTabColorForSession(vtworkspace::SessionId session)
{
    if (auto* tab = findTabHostingSession(session))
        _model->resetTabColor(tab->id(), vtworkspace::TabColorSource::Application);
}
// }}}

// {{{ Tab-strip operations (window-routed)
void TerminalSessionManager::activateModelTabByRow(vtworkspace::WindowId window, int row)
{
    auto* tab = tabAtRow(window, row);
    if (tab == nullptr)
        return;

    // Update the window's active tab: this fires activeTabChanged -> the owning controller rebuilds
    // its PaneProxy tree and rebinds activeTabRootPane, and the pane tree's `session:` binding
    // attaches the tab's active-leaf session to its display.
    _model->activateTab(window, tab->id());
}

void TerminalSessionManager::activateTab(vtworkspace::WindowId window, int index)
{
    activateModelTabByRow(window, index);
}

void TerminalSessionManager::moveTab(vtworkspace::WindowId window, int fromIndex, int toIndex)
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

void TerminalSessionManager::closeWindowIfEmpty(vtworkspace::WindowId window)
{
    // An empty window (no tabs) is not a valid state: after a cross-window move that took a window's
    // last tab, close it. Runs AFTER the transplant, so the moved tab's sessions are already gone from
    // this window and closeWindow()'s removeWindow won't tear them down.
    if (auto* win = _model->window(window); win != nullptr && win->empty())
        if (auto* controller = controllerFor(window))
            controller->closeWindow();
}

void TerminalSessionManager::moveTabToWindow(vtworkspace::WindowId from,
                                             int fromIndex,
                                             vtworkspace::WindowId to,
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

void TerminalSessionManager::tearOffTabToNewWindow(vtworkspace::WindowId from,
                                                   int fromIndex,
                                                   QScreen* targetScreen)
{
    auto* tab = tabAtRow(from, fromIndex);
    if (tab == nullptr)
        return;
    // A single-tab window torn off would just recreate itself — nothing to do.
    auto* srcWindow = _model->window(from);
    if (srcWindow != nullptr && srcWindow->tabCount() <= 1)
        return;

    // Stage the transplant, then spawn a window. The new window's Main.qml consumes the staged tab in
    // Component.onCompleted (via consumePendingTransplant) INSTEAD of creating a fresh first tab, so the
    // torn-off tab becomes its sole tab — its sessions survive the move (nothing is torn down).
    _pendingTransplant = std::pair { from, tab->id() };
    _app.newWindow(targetScreen);
}

bool TerminalSessionManager::consumePendingTransplant(window::WindowController* newController)
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

void TerminalSessionManager::closeTabAtIndex(vtworkspace::WindowId window, int index)
{
    // index is a tab row (rows are tabs). Close the whole tab by terminating each of its panes'
    // sessions; the model collapses to the survivor on each close and tears the tab down with the
    // last pane.
    terminateSessions(sessionsInTab(tabAtRow(window, index)), SessionEnd::Destroy);
}

std::vector<TerminalSession*> TerminalSessionManager::sessionsInTab(vtworkspace::Tab* tab) const
{
    std::vector<TerminalSession*> sessions;
    if (tab == nullptr)
        return sessions;
    tab->rootPane()->walkTree([&](vtworkspace::Pane& pane) {
        if (pane.isLeaf())
            if (auto* session = sessionForId(pane.session()))
                sessions.push_back(session);
    });
    return sessions;
}

void TerminalSessionManager::authorRemoteEnd(TerminalSession* session)
{
    if (session != nullptr)
        _sessionFactory.requestRemoteClose(&session->terminal().device());
}

void TerminalSessionManager::terminateSessions(std::span<TerminalSession* const> sessions, SessionEnd end)
{
    // The single whole-tab close primitive shared by every close entry point. The caller has already
    // gathered the doomed sessions into a stable vector, so terminate() -> removeSession is free to
    // mutate the model tab tree (collapsing to the survivor and finally erasing the tab) without
    // invalidating what we iterate. terminate() now closes the PTY even for a display-less background
    // pane, so panes of a non-active tab are torn down too (previously a silent no-op that leaked the
    // session and its shell process).
    //
    // A Destroy also has to be authored UPSTREAM for a session this process does not own: a
    // daemon-hosted one outlives its pty, so closing the local view is not closing the session.
    // Done before the terminate() below, while each session still holds the pty the remote factory
    // identifies it by.
    for (auto* session: sessions)
    {
        if (end == SessionEnd::Destroy)
            authorRemoteEnd(session);
        session->terminate();
    }
}

std::vector<TerminalSession*> TerminalSessionManager::gatherSessionsOfTabsWhere(
    vtworkspace::Window& window, std::function<bool(int row, vtworkspace::Tab*)> const& predicate) const
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

void TerminalSessionManager::closeOtherTabs(vtworkspace::WindowId window, int index)
{
    auto* win = _model->window(window);
    auto* keep = tabAtRow(window, index);
    if (keep == nullptr || win == nullptr)
        return;

    auto const doomed = gatherSessionsOfTabsWhere(
        *win, [keep](int, vtworkspace::Tab* tab) { return tab->id() != keep->id(); });
    _model->closeOtherTabs(window, keep->id());
    terminateSessions(doomed, SessionEnd::Destroy);
}

void TerminalSessionManager::closeTabsToRight(vtworkspace::WindowId window, int index)
{
    auto* win = _model->window(window);
    auto* anchor = tabAtRow(window, index);
    if (anchor == nullptr || win == nullptr)
        return;

    // Tabs strictly to the right of @p index. The gather scan is already row-ordered and hands us the row,
    // so compare it directly instead of re-deriving it with a linear rowOfTab() scan per candidate.
    auto const doomed =
        gatherSessionsOfTabsWhere(*win, [index](int row, vtworkspace::Tab*) { return row > index; });
    _model->closeTabsToRight(window, anchor->id());
    terminateSessions(doomed, SessionEnd::Destroy);
}

QVariantList TerminalSessionManager::tabColorPalette() const
{
    // The palette is immutable for the model's lifetime; build the QVariantList once and cache it.
    if (_tabColorPaletteCache.isEmpty())
        for (auto const& color: _model->colorPalette())
            _tabColorPaletteCache.append(platform::toQColor(color));
    return _tabColorPaletteCache;
}
// }}}

// {{{ Split-pane operations
void TerminalSessionManager::splitActivePane(bool vertical, TerminalSession* acting, double ratio)
{
    // Attach mode: author the split on the daemon (B3-Qt); its layout re-push
    // reconciles the new pane in. A reconciliation-driven split (during realization)
    // returns false and builds locally below, as does a local factory.
    // No profile is named: a local split runs the default profile too.
    if (acting != nullptr
        && _sessionFactory.requestRemoteSplit(&acting->terminal().device(), vertical, RemoteSpawn {}))
        return;

    auto const direction = vertical ? vtworkspace::SplitState::Vertical : vtworkspace::SplitState::Horizontal;
    splitPane(acting, direction, ratio, std::nullopt, SessionRole::Shell);
}

TerminalSession* TerminalSessionManager::splitPane(TerminalSession* acting,
                                                   vtworkspace::SplitState direction,
                                                   double ratio,
                                                   std::optional<vtpty::Process::ExecInfo> const& command,
                                                   SessionRole role)
{
    if (!_sessionFactory.canCreateSession())
    {
        managerLog()("Refusing to split: the session factory cannot back a new session right now.");
        return nullptr;
    }

    auto* tab = paneActionTargetTab(acting);
    if (tab == nullptr)
        return nullptr;

    // Split beside the pane the keybinding actually fired from. _model->splitActivePane splits the tab's
    // model-active pane, so if the acting session's pane is not already the active one (focus vs.
    // model-active can diverge in a multi-pane tab), make it active first — otherwise both the split and
    // the inherited cwd below would target the wrong pane. When acting is null (QML) or not a leaf of this
    // tab, fall back to the tab's current active pane.
    if (acting != nullptr)
        if (auto const* actingLeaf = leafOfSession(*tab, *acting); actingLeaf != nullptr)
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
    auto const newSessionId = vtworkspace::SessionId { _nextSessionId++ };
    auto* session = createBackingSession(
        newSessionId, paneWorkingDirectory(command, std::move(cwd)), pageSize, command, std::nullopt, role);
    if (session == nullptr)
        return nullptr; // no backing session, no split

    auto* newLeaf = _model->splitActivePane(tab->id(), direction, ratio);
    _pendingSessionId.reset(); // consumed by the allocator; clear any leftover
    if (newLeaf == nullptr)
    {
        // The split did not happen; the backing session we created has no model pane, so terminate
        // it to drop the orphaned session/registry entries rather than leak them. In attach mode it
        // is a real daemon session too, so end it upstream — nothing will ever show it.
        if (auto* orphan = sessionForId(newSessionId))
        {
            authorRemoteEnd(orphan);
            orphan->terminate();
        }
        return nullptr;
    }

    // The new leaf joins the same tab as the pane it split from.
    _tabBySession[newSessionId.value] = tab->id();
    return session;
}

std::expected<TerminalSession*, CommandPaneError> TerminalSessionManager::openCommandPane(
    TerminalSession* acting, CommandPanePlacement placement, vtpty::Process::ExecInfo const& command)
{
    if (auto const refusal = commandPaneRefusal(acting, placement))
        return std::unexpected(*refusal);

    auto* const pane = [&]() -> TerminalSession* {
        switch (placement)
        {
            case CommandPanePlacement::Split:
                // Stacked below the pane it came from: output reads top to bottom, and keeps the full width.
                return splitPane(
                    acting, vtworkspace::SplitState::Horizontal, 0.5, command, SessionRole::CommandOutput);
            case CommandPanePlacement::Tab:
                if (auto* window = windowHostingSession(acting))
                    return createSessionInBackground(
                        window->id(), std::nullopt, command, SessionRole::CommandOutput);
                return nullptr;
        }
        return nullptr;
    }();
    if (pane == nullptr)
        return std::unexpected(CommandPaneError::NoPane);
    return pane;
}

std::optional<CommandPaneError> TerminalSessionManager::commandPaneRefusal(
    TerminalSession* acting, CommandPanePlacement placement) const
{
    // An attach-mode factory binds each pane it makes to a session the remote end hosts and drops the
    // command: the pane would show that session, on the host, which never sees a file written here.
    if (_sessionFactory.hosting() == SessionHosting::Remote)
        return CommandPaneError::RemoteSessions;

    if (!_sessionFactory.canCreateSession())
        return CommandPaneError::NoPane;

    switch (placement)
    {
        case CommandPanePlacement::Split:
            if (paneActionTargetTab(acting) == nullptr)
                return CommandPaneError::NoPane;
            break;
        case CommandPanePlacement::Tab:
            if (windowHostingSession(acting) == nullptr)
                return CommandPaneError::NoPane;
            break;
    }
    return std::nullopt;
}

void TerminalSessionManager::closeActivePane(TerminalSession* acting, SessionEnd end)
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
    {
        // On Destroy, a daemon-hosted session must be closed upstream too, or it lingers there with
        // no view. Authored while the pty still exists, since that is what identifies it.
        if (end == SessionEnd::Destroy)
            authorRemoteEnd(session);
        session->terminate();
    }
}

void TerminalSessionManager::focusPane(vtworkspace::FocusDirection direction, TerminalSession* acting)
{
    if (auto* tab = paneActionTargetTab(acting))
        _model->focusDirection(tab->id(), direction);
}

void TerminalSessionManager::swapPane(vtworkspace::FocusDirection direction, TerminalSession* acting)
{
    if (auto* tab = paneActionTargetTab(acting))
        _model->swapActivePane(tab->id(), direction);
}

void TerminalSessionManager::movePane(vtworkspace::FocusDirection direction, TerminalSession* acting)
{
    if (auto* tab = paneActionTargetTab(acting))
        _model->moveActivePane(tab->id(), direction);
}

void TerminalSessionManager::toggleActivePaneOrientation(TerminalSession* acting)
{
    if (auto* tab = paneActionTargetTab(acting))
        _model->toggleActivePaneOrientation(tab->id());
}

void TerminalSessionManager::resizeActivePane(vtworkspace::FocusDirection direction,
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
bool TerminalSessionManager::isActivePane(vtworkspace::TabId tab, vtworkspace::PaneId id) const noexcept
{
    // Keyed by the proxy's OWN tab (not "the active tab"), so a proxy of any window's tab answers
    // correctly — a divider drag or pane click in a second OS window must not consult the first
    // window's active tab.
    if (auto* t = _model->findTab(tab))
        return t->activePane() != nullptr && t->activePane()->id() == id;
    return false;
}

void TerminalSessionManager::setPaneRatio(vtworkspace::TabId tab, vtworkspace::PaneId id, double ratio)
{
    if (_model->findTab(tab) != nullptr)
        _model->setPaneRatio(tab, id, ratio);
}

void TerminalSessionManager::activatePane(vtworkspace::TabId tab, vtworkspace::PaneId id)
{
    if (_model->findTab(tab) != nullptr)
        _model->setActivePane(tab, id);
}

// }}}

} // namespace contour::session
