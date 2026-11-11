// SPDX-License-Identifier: Apache-2.0
#include <contour/ContourGuiApp.hpp>
#include <contour/config/Actions.hpp>
#include <contour/config/LayoutBuilder.hpp>
#include <contour/display/TerminalDisplay.hpp>
#include <contour/platform/ColorConversion.hpp>
#include <contour/platform/Notifier.hpp>
#include <contour/platform/QtPath.hpp>
#include <contour/session/DaemonCycle.hpp>
#include <contour/session/EndReason.hpp>
#include <contour/session/Logging.hpp>
#include <contour/session/PaneProxy.hpp>
#include <contour/session/PreviewTranscript.hpp>
#include <contour/session/TerminalSession.hpp>
#include <contour/session/TerminalSessionManager.hpp>
#include <contour/window/HostChipState.hpp>
#include <contour/window/PaneHeaderPolicy.hpp>
#include <contour/window/ProgramStatusBadge.hpp>
#include <contour/window/QtString.hpp>
#include <contour/window/TabBadge.hpp>
#include <contour/window/TabLabel.hpp>
#include <contour/window/WindowController.hpp>

#include <vtbackend/core/Primitives.hpp>
#include <vtbackend/core/WorkingDirectory.hpp>

#include <vtpty/MockPty.hpp>
#include <vtpty/Process.hpp>
#ifdef VTPTY_LIBSSH2
    #include <vtpty/SshSession.hpp>
#endif

#include <core/Overloaded.hpp>
#include <core/Utf8Path.hpp>
#include <core/platform/PathUtils.hpp>

#include <QtGui/QGuiApplication>
#include <QtGui/QScreen>
#include <QtQml/QQmlEngine>
#include <QtQuick/QQuickWindow>

#include <algorithm>
#include <chrono>
#include <exception>
#include <filesystem>
#include <format>
#include <initializer_list>
#include <memory>
#include <optional>
#include <ranges>
#include <string>
#include <tuple>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

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
                                               platform::TaskbarProgressSink& taskbarProgressSink,
                                               platform::ISecureInput& secureInput,
                                               WindowSwitching& windowSwitching,
                                               SwitchDirectory& switchDirectory):
    _app { app },
    _localFactory { factory },
    _windowSwitching { windowSwitching },
    _switchDirectory { switchDirectory },
    _layoutStore { layouts },
    _commandHistoryStore { commands },
    _taskbarProgressSink { taskbarProgressSink },
    _secureInputGate { secureInput },
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

TerminalSessionManager::~TerminalSessionManager() = default;

bool TerminalSessionManager::consumeAttachWindow(contour::window::WindowController* controller)
{
    // A panel is no daemon workspace: it never adopts one, and its own first tab is made here.
    return _attachWindowBinder && controller != nullptr && !isPanelWindow(controller->displayedWorkspace())
               ? _attachWindowBinder(controller)
               : false;
}

void TerminalSessionManager::noteTransportLost(WindowBinding const& binding)
{
    for (auto* session: sessionsBoundTo(binding))
        session->noteTransportLost();
}

std::vector<TerminalSession*> TerminalSessionManager::sessionsBoundTo(WindowBinding const& binding) const
{
    auto bound = std::vector<TerminalSession*> {};
    for (auto const window: windowsBoundTo(binding))
        std::ranges::copy(sessionsOfWindow(window), std::back_inserter(bound));
    return bound;
}

std::vector<TerminalSession*> TerminalSessionManager::sessionsOfWindow(vtworkspace::WorkspaceId window) const
{
    auto sessions = std::vector<TerminalSession*> {};
    if (auto const* const workspace = _model->workspace(window))
        for (auto const index: std::views::iota(0, workspace->tabCount()))
            std::ranges::copy(sessionsInTab(workspace->tabAt(index)), std::back_inserter(sessions));
    return sessions;
}

std::vector<TerminalSession*> TerminalSessionManager::disconnectedOf(WindowBinding const& binding) const
{
    auto lost = std::vector<TerminalSession*> {};
    std::ranges::copy_if(
        sessionsBoundTo(binding), std::back_inserter(lost), [](TerminalSession const* session) {
            return session->endReason() == EndReason::Disconnected;
        });
    return lost;
}

std::vector<window::WindowController*> TerminalSessionManager::controllersBoundTo(
    WindowBinding const& binding) const
{
    auto controllers = std::vector<window::WindowController*> {};
    for (auto const window: windowsBoundTo(binding))
        if (auto* const controller = controllerDisplaying(window))
            controllers.push_back(controller);
    return controllers;
}

vtworkspace::Tab* TerminalSessionManager::tabHostingSession(TerminalSession const* session) const noexcept
{
    if (session == nullptr)
        return nullptr;
    return findTabHostingSession(session->modelSessionId());
}

vtworkspace::Pane* TerminalSessionManager::leafOfSession(vtworkspace::Tab const& tab,
                                                         TerminalSession const& session)
{
    return tab.rootPane() != nullptr ? tab.rootPane()->findLeaf(session.modelSessionId()) : nullptr;
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

TerminalSession* TerminalSessionManager::actingSessionOfWindow(vtworkspace::WorkspaceId window) const
{
    auto* win = _model->workspace(window);
    if (win == nullptr)
        return nullptr;
    // The active tab's active pane first — that is the pane the user is looking at, so it is the
    // most faithful "this window". Any live leaf of the window would resolve to the same workspace on the
    // daemon, so the scan below is a fallback for a window whose active pane happens to have no live
    // session (a tab mid-teardown), not a different answer.
    if (auto* tab = win->activeTab(); tab != nullptr)
        if (auto* leaf = tab->activePane(); leaf != nullptr)
            if (auto* session = sessionForId(leaf->session()); session != nullptr)
                return session;
    auto const sessions = sessionsOfWindow(window);
    return sessions.empty() ? nullptr : sessions.front();
}

vtpty::Pty const* TerminalSessionManager::actingPtyOfWindow(vtworkspace::WorkspaceId window) const
{
    auto* const session = actingSessionOfWindow(window);
    return session != nullptr ? &session->terminal().device() : nullptr;
}

int TerminalSessionManager::rowOfTab(vtworkspace::TabId tab) const noexcept
{
    // Window-agnostic: resolve the tab's OWNING window through the model, so the row is correct for
    // any tab of any OS window.
    auto* win = _model->workspace(_model->workspaceOfTab(tab));
    return win != nullptr ? win->indexOf(tab) : -1;
}

window::WindowController* TerminalSessionManager::createWindowController()
{
    return createWindowController(_app.menuPresentation());
}

window::WindowController* TerminalSessionManager::createWindowController(
    config::MenuPresentation menuPresentation)
{
    return createWindowController(menuPresentation, window::qtNoticeAnnouncer());
}

window::WindowController* TerminalSessionManager::createWindowController(
    config::MenuPresentation menuPresentation, window::NoticeAnnouncerFactory const& noticeAnnouncer)
{
    // Every OS window gets its own vtworkspace::Workspace; the controller adapts exactly that window (all its
    // reads and writes are keyed by the WorkspaceId).
    auto* window = _model->createWorkspace();
    auto const role = std::exchange(_stagedRole, window::WindowRole::Normal);
    // A window spawned for a dropped pane or tab opens where the drop was.
    auto initialPosition = std::exchange(_pendingSpawnPosition, std::nullopt);
    // Restore only the first window, and never a placed one (the quick terminal, R-g). Additional windows
    // keep the existing spawn-screen policy.
    if (!initialPosition && window::windowRoleRow(role).memory == window::PositionMemory::Remembered
        && _app.config().rememberWindowPosition.value() && _controllersByWindow.empty()
        && !_app.config().configFile.empty() && QGuiApplication::platformName() != QStringLiteral("wayland"))
    {
        if (auto saved = _app.windowPositionStore().load(_app.config().configFile.parent_path()
                                                         / window::WindowPositionFileName))
            initialPosition = *saved;
        else
            errorLog()("Failed to load window position: {}", saved.error());
    }
    auto const windowId = window::WindowId { _nextWindowId++ };
    auto* controller = new window::WindowController(*this,
                                                    windowId,
                                                    window->id(),
                                                    menuPresentation,
                                                    initialPosition,
                                                    core::platform::defaultSteadyClock(),
                                                    noticeAnnouncer,
                                                    role);
    QQmlEngine::setObjectOwnership(controller, QQmlEngine::CppOwnership);
    _controllersByWindow[windowId] = controller;
    // A freshly spawned window is the one the user is looking at. Seeding ownership here (rather than
    // waiting for the first Qt focus-in) is what lets the very first tab be focused at creation, and
    // makes a tab switch notify the shell even before the window has ever been clicked.
    _focusedWindow = window->id();
    return controller;
}

window::WindowController* TerminalSessionManager::controllerDisplaying(
    vtworkspace::WorkspaceId workspace) const noexcept
{
    auto const controllers = _controllersByWindow | std::views::values;
    auto const found = std::ranges::find_if(controllers, [workspace](window::WindowController const* c) {
        return c->displayedWorkspace() == workspace;
    });
    return found != controllers.end() ? *found : nullptr;
}

window::WindowController* TerminalSessionManager::controllerOwning(
    vtworkspace::WorkspaceId workspace) const noexcept
{
    auto const controllers = _controllersByWindow | std::views::values;
    auto const found = std::ranges::find_if(controllers, [workspace](window::WindowController const* c) {
        return std::ranges::find(c->ownedWorkspaces(), workspace) != c->ownedWorkspaces().end();
    });
    return found != controllers.end() ? *found : nullptr;
}

window::WindowController* TerminalSessionManager::controllerOf(window::WindowId window) const noexcept
{
    auto const it = _controllersByWindow.find(window);
    return it != _controllersByWindow.end() ? it->second : nullptr;
}

vtworkspace::WorkspaceId TerminalSessionManager::createWorkspaceIn(window::WindowId window)
{
    auto* const controller = controllerOf(window);
    auto* const workspace = _model->createWorkspace();
    if (controller != nullptr)
        controller->adoptWorkspace(workspace->id());
    return workspace->id();
}

void TerminalSessionManager::selectWorkspace(window::WindowId window, std::size_t index) const
{
    if (auto* const controller = controllerOf(window);
        controller != nullptr && index < controller->ownedWorkspaces().size())
        controller->displayWorkspace(controller->ownedWorkspaces()[index]);
}

void TerminalSessionManager::windowDisplayChanged(window::WindowController& controller,
                                                  vtworkspace::WorkspaceId previous)
{
    auto const now = controller.displayedWorkspace();
    if (_focusedWindow == previous)
        _focusedWindow = now;
    if (_lastFocusedWindow == previous)
        _lastFocusedWindow = now;
    // What came into view is seen, and what left it is no longer on screen.
    markVisibleSessionsSeen(now);
    notifyDisplaysExposureChanged(previous);
    notifyDisplaysExposureChanged(now);
    syncFocusForWindow(&controller);
}

window::TabAttention* TerminalSessionManager::ledgerOf(vtworkspace::SessionId session) const noexcept
{
    auto const* const tab = findTabHostingSession(session);
    auto* const owner = tab != nullptr ? controllerOwning(_model->workspaceOfTab(tab->id())) : nullptr;
    return owner != nullptr ? &owner->tabAttention() : nullptr;
}

window::PaneKey TerminalSessionManager::paneKeyOf(vtworkspace::SessionId session) const
{
    auto const* const known = sessionForId(session);
    auto const host = known != nullptr ? hostSessionIdOf(*known) : std::nullopt;
    return window::PaneKey { .value = host.value_or(session.value) };
}

std::vector<TerminalSessionManager::AttentionCarry> TerminalSessionManager::beginCarryingAttention(
    std::span<vtworkspace::SessionId const> sessions) const
{
    auto carried = std::vector<AttentionCarry> {};
    for (auto const session: sessions)
        if (auto* const from = ledgerOf(session))
            carried.push_back({ .session = session, .key = paneKeyOf(session), .from = from });
    return carried;
}

void TerminalSessionManager::carryAttention(std::span<AttentionCarry const> carried)
{
    for (auto const& pane: carried)
    {
        auto* const to = ledgerOf(pane.session);
        if (to == nullptr || to == pane.from)
            continue;
        auto const marks = pane.from->of(pane.key);
        pane.from->forget(pane.key);
        to->adopt(pane.key, marks);
    }
}

window::SessionAttention TerminalSessionManager::attentionOf(vtworkspace::SessionId session) const
{
    auto const* const ledger = ledgerOf(session);
    return ledger != nullptr ? ledger->of(paneKeyOf(session)) : window::SessionAttention {};
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
    vtworkspace::WorkspaceId window,
    std::optional<std::string> const& profileName,
    std::optional<vtpty::Process::ExecInfo> const& command,
    SessionRole role)
{
    // TODO: Remove dependency on app-knowledge and pass shell / terminal-size instead.
    // The GuiApp *or* (Global)Config could be made a global to be accessible from within QML.

    auto& factory = factoryFor(window);
    if (!factory.canCreateSession())
    {
        managerLog()("Refusing to create a session: the session factory cannot back one right now.");
        return nullptr;
    }

    if (!_focusedWindow)
    {
        managerLog()("No focused window found. something went wrong.");
    }

    auto* targetWindow = _model->workspace(window);
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
    auto* session = createBackingSession(factory,
                                         sessionId,
                                         paneWorkingDirectory(command, std::move(ptyPath)),
                                         pageSize,
                                         command,
                                         profileName,
                                         role);

    // Mirror this new session into the vtworkspace model as a new single-pane tab.
    if (auto* tab = _model->createTab(window))
        _tabBySession[sessionId.value] = tab->id();

    return session;
}

TerminalSession* TerminalSessionManager::createBackingSession(
    SessionFactory& factory,
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
    if (!factory.canCreateSession())
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
    // A notification is raised on this machine whatever host runs the session: a binding's factory that
    // names no notifier of its own leaves it to the local factory's.
    auto notifier = factory.createNotifier();
    if (!notifier && &factory != &_localFactory)
        notifier = _localFactory.createNotifier();
    auto* session =
        new TerminalSession(this,
                            factory.createPty(std::move(cwd), pageSize, commandOverride, profileName),
                            _app,
                            profileName.value_or(std::string {}),
                            pageSize,
                            std::move(launchedCommand),
                            std::move(notifier),
                            role,
                            factory.hosting());
    // The manager owns what it makes: removeSession() frees a session that has ended, and those still
    // running go with the manager, along with whatever they had queued for themselves.
    session->setParent(this);
    managerLog()("Create backing session with ID {}({}); {} sessions before it.",
                 session->id(),
                 (void*) session,
                 _sessionsById.size());
    // Close the cycle the constructor could not: the factory handed out a pty, and a
    // daemon-backed one needs the terminal built around it to populate its grid.
    factory.bindTerminal(&session->terminal().device(),
                         session->terminal(),
                         InterruptionSink { .context = session, .deliver = [session](auto commandLine) {
                                               session->noteInterruption(std::move(commandLine));
                                           } });

    // A daemon's session starts under its own preference, so the mirror is first told which one is shown.
    session->stateHostedColorPreference();

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

std::unique_ptr<TerminalSession> TerminalSessionManager::createPreviewSession(config::Config const& config,
                                                                              std::string const& profileName)
{
    // Not through createBackingSession(): the session is the caller's, so it is not registered, not
    // parented to the manager and not watched for closing, and it needs no pty from the factory. Its device
    // is a MockPty that never has a program on it, so the transcript is written to the screen directly. A
    // display that shows it does call start() (TerminalDisplay::setSession), which is harmless here: the
    // mock opens nothing, its read loop gets no bytes and ends, and the session's end path runs with the
    // Preview role, which shows no footer.
    auto session = std::make_unique<TerminalSession>(this,
                                                     std::make_unique<vtpty::MockPty>(PreviewPageSize),
                                                     _app,
                                                     std::string {},
                                                     PreviewPageSize,
                                                     std::nullopt,
                                                     std::make_unique<platform::NullNotifier>(),
                                                     SessionRole::Preview,
                                                     SessionHosting::Local);
    // Its owner destroys it; QML must not collect what it is bound to.
    QQmlEngine::setObjectOwnership(session.get(), QQmlEngine::CppOwnership);
    session->applyReloadedConfig(config, profileName);
    session->terminal().writeToScreen(PreviewTranscript);
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
        _focusedWindow = c->displayedWorkspace();
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
    refreshSecureInput();
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

void TerminalSessionManager::setFocusedWindow(vtworkspace::WorkspaceId window)
{
    // Taking ownership satisfies syncFocusForWindow's gate by construction, so the re-pointing half is
    // that one implementation rather than a second copy of it.
    _focusedWindow = window;
    if (auto const* const controller = controllerDisplaying(window))
        _focusOrder[controller->windowId()] = ++_focusCounter;
    // The daemon names the window that raises by the focus each one reports.
    if (auto* const binding = mutableBindingOf(window))
        binding->reportFocus(vthost::proto::WindowFocus::Focused);
    syncFocusForWindow(controllerDisplaying(window));
    // The app came to the front with focus where it was: no focus-in moved the session to refresh.
    refreshSecureInput();

    // Activated with focus where it already was, as after a window manager that kept it through a
    // minimise: no focus-in tells the pane it is being looked at.
    focusedSessionSeen();
}

void TerminalSessionManager::windowStateChanged(vtworkspace::WorkspaceId window)
{
    if (_focusedWindow == window)
        focusedSessionSeen();
    // A restore puts the window's sessions back on screen, which is the tab badges' "seen" too.
    markVisibleSessionsSeen(window);
    // A minimise or a restore changes what a display's frames are for: the running times redraw only while it
    // is on screen.
    notifyDisplaysExposureChanged(window);
}

void TerminalSessionManager::windowExposed(vtworkspace::WorkspaceId window) const
{
    notifyDisplaysExposureChanged(window);
}

void TerminalSessionManager::focusedSessionSeen()
{
    if (_focusedSession != nullptr
        && sessionVisibility(*_focusedSession) == platform::SessionVisibility::Focused)
        _focusedSession->paneSeen();
}

void TerminalSessionManager::clearFocusedWindow(vtworkspace::WorkspaceId window)
{
    if (_focusedWindow != window) // guarded like clearFocusIfCurrent
        return;
    auto const lost = std::exchange(_focusedWindow, std::nullopt);
    if (auto* const binding = mutableBindingOf(window))
        binding->reportFocus(vthost::proto::WindowFocus::Unfocused);
    // A panel (the quick terminal) is never where an application command lands, so its leaving focus does
    // not displace the window that held it before.
    if (isNormalWindow(window))
        _lastFocusedWindow = lost;
    // Another application in front, or the window closing: setFocusedSession() re-decides the OS's secure
    // input, now with no window of ours in front, so it turns off.
    setFocusedSession(nullptr);
}

bool TerminalSessionManager::isPanelWindow(vtworkspace::WorkspaceId window) const noexcept
{
    auto const* const controller = controllerDisplaying(window);
    return controller != nullptr
           && window::windowRoleRow(controller->role()).level == window::WindowLevel::Panel;
}

SessionFactory& TerminalSessionManager::factoryFor(vtworkspace::WorkspaceId window) const noexcept
{
    auto* const binding = mutableBindingOf(window);
    return binding != nullptr ? binding->sessions() : _localFactory;
}

WindowBinding* TerminalSessionManager::mutableBindingOf(vtworkspace::WorkspaceId window) const noexcept
{
    auto const bound = _bindingOfWindow.find(window.value);
    return bound != _bindingOfWindow.end() ? bound->second : nullptr;
}

SessionFactory& TerminalSessionManager::factoryOf(TerminalSession const& session) const noexcept
{
    auto const* const window = hostOf(&session).window;
    return window != nullptr ? factoryFor(window->id()) : _localFactory;
}

bool TerminalSessionManager::raises(TerminalSession const& session) const
{
    auto const* const window = hostOf(&session).window;
    auto const* const binding = window != nullptr ? bindingOf(window->id()) : nullptr;
    return binding == nullptr || binding->raiseRole() == vthost::proto::RaiseRole::Raise;
}

void TerminalSessionManager::forgetWorkspace(vtworkspace::WorkspaceId workspace)
{
    std::erase(_emptiedWindows, workspace);
    // The window stays: focus follows it to what it displays now (windowDisplayChanged), and a binding it
    // still holds through its displayed workspace goes on serving it.
    if (_focusedWindow == workspace)
        _focusedWindow.reset();
    if (_lastFocusedWindow == workspace)
        _lastFocusedWindow.reset();
    if (auto const bound = _bindingOfWindow.extract(workspace.value))
        if (forgetMember(*bound.mapped(), workspace) == BindingMembers::None)
            bound.mapped()->lastWindowReleased();
}

void TerminalSessionManager::refreshTaskbarAttention()
{
    scheduleTaskbarProgress();
}

void TerminalSessionManager::bindWindow(vtworkspace::WorkspaceId window, WindowBinding& binding)
{
    auto& bound = _bindingOfWindow[window.value];
    if (bound == &binding)
        return;
    auto* const previous = std::exchange(bound, &binding);
    _windowsOfBinding[&binding].push_back(window);
    auto const left = previous != nullptr ? forgetMember(*previous, window) : BindingMembers::Remaining;
    notifyHostBindingChanged(window);
    // A window on a daemon is one whose menus list daemons: start discovering them now, so that the first
    // menu opened already has them. Discovery only: a listing opens a connection per saved host, which a bind
    // must not (a surface that lists them refreshes fully as it opens).
    if (binding.daemonTarget())
        _switchDirectory.refreshDiscovery();
    // Last: the binding the window left may stop, and release what it still holds here.
    if (left == BindingMembers::None)
        previous->lastWindowReleased();
}

WindowBinding const* TerminalSessionManager::bindingOf(vtworkspace::WorkspaceId window) const noexcept
{
    return mutableBindingOf(window);
}

std::span<vtworkspace::WorkspaceId const> TerminalSessionManager::windowsBoundTo(
    WindowBinding const& binding) const noexcept
{
    auto const bound = _windowsOfBinding.find(&binding);
    return bound != _windowsOfBinding.end() ? std::span<vtworkspace::WorkspaceId const> { bound->second }
                                            : std::span<vtworkspace::WorkspaceId const> {};
}

TerminalSessionManager::BindingMembers TerminalSessionManager::forgetMember(WindowBinding const& binding,
                                                                            vtworkspace::WorkspaceId window)
{
    auto const bound = _windowsOfBinding.find(&binding);
    if (bound == _windowsOfBinding.end())
        return BindingMembers::None;
    std::erase(bound->second, window);
    if (!bound->second.empty())
        return BindingMembers::Remaining;
    _windowsOfBinding.erase(bound);
    return BindingMembers::None;
}

void TerminalSessionManager::releaseBinding(WindowBinding const& binding)
{
    clearAttachWindowBinder(binding);
    _linkStates.erase(&binding);
    auto const released = _windowsOfBinding.extract(&binding);
    if (released.empty())
        return;
    for (auto const window: released.mapped())
        _bindingOfWindow.erase(window.value);
    for (auto const window: released.mapped())
        notifyHostBindingChanged(window);
}

void TerminalSessionManager::notifyHostBindingChanged(vtworkspace::WorkspaceId window) const
{
    if (auto* const controller = controllerDisplaying(window))
        controller->notifyHostBindingChanged();
}

std::string_view TerminalSessionManager::hostLabelOf(vtworkspace::WorkspaceId window) const noexcept
{
    auto const* const binding = bindingOf(window);
    return binding != nullptr ? binding->hostLabel() : std::string_view {};
}

std::string_view TerminalSessionManager::chromeLabelOf(vtworkspace::WorkspaceId window) const noexcept
{
    auto const* const binding = bindingOf(window);
    return binding != nullptr ? binding->chromeLabel() : std::string_view {};
}

std::optional<SwitchTarget> TerminalSessionManager::daemonTargetOf(vtworkspace::WorkspaceId window) const
{
    auto const* const binding = bindingOf(window);
    if (binding == nullptr)
        return std::nullopt;
    return binding->daemonTarget();
}

window::DaemonLocality TerminalSessionManager::daemonLocalityOf(vtworkspace::WorkspaceId window) const
{
    auto const* const binding = bindingOf(window);
    return binding != nullptr ? binding->daemonLocality() : window::DaemonLocality::Local;
}

window::DaemonDriftModel* TerminalSessionManager::daemonDriftOf(
    vtworkspace::WorkspaceId window) const noexcept
{
    auto* const binding = mutableBindingOf(window);
    return binding != nullptr ? binding->daemonDrift() : nullptr;
}

std::string_view TerminalSessionManager::hostLabelOf(TerminalSession const& session) const noexcept
{
    auto const* const window = hostOf(&session).window;
    return window != nullptr ? hostLabelOf(window->id()) : std::string_view {};
}

RemoteRouting TerminalSessionManager::requestRemoteWindow(TerminalSession const& acting, QScreen* screen)
{
    // Only a window a daemon serves opens its new window there; a local one, the quick terminal among them,
    // opens a local window.
    auto const* const window = hostOf(&acting).window;
    auto* const binding = window != nullptr ? mutableBindingOf(window->id()) : nullptr;
    if (binding == nullptr || !binding->daemonTarget())
        return RemoteRouting::Local;
    return openDaemonWindowLike(*binding, window->id(), std::nullopt, screen, "New Window");
}

RemoteRouting TerminalSessionManager::openDaemonWindowLike(WindowBinding& binding,
                                                           vtworkspace::WorkspaceId asking,
                                                           std::optional<vtworkspace::TabId> activeTab,
                                                           QScreen* screen,
                                                           std::string_view what) const
{
    auto const opened = binding.openWindowLike(asking, activeTab, screen);
    if (opened)
        return RemoteRouting::Routed;
    managerLog()("{}: {}", what, opened.error());
    if (auto* const controller = controllerDisplaying(asking))
        controller->notices().post(window::windowLinkFailedNotice(opened.error()));
    return RemoteRouting::Refused;
}

namespace
{
    /// @return The run of the daemon that hosts what @p factory makes (SessionFactory::daemonInstance); none
    ///         for a factory no daemon is behind, or one whose daemon has not said yet.
    [[nodiscard]] std::optional<uint64_t> daemonOf(SessionFactory const& factory) noexcept
    {
        return factory.hostKind() == SessionHostKind::Daemon ? factory.daemonInstance() : std::nullopt;
    }

    /// @return Who hosts what @p factory makes, as a drag payload names it.
    [[nodiscard]] window::HostIdentity identityOf(SessionFactory const& factory)
    {
        // Ids a daemon hands out name the same sessions in every process attached to it: those drags are the
        // daemon's. Everything else names this process's model only.
        if (auto const instance = daemonOf(factory))
            return window::HostIdentity { .kind = window::HostKind::Daemon,
                                          .id = QString::number(*instance) };
        return window::HostIdentity { .kind = window::HostKind::Process, .id = window::processToken() };
    }
} // namespace

std::vector<WindowBinding*> TerminalSessionManager::bindingsServing(window::HostIdentity const& host) const
{
    // This process's own factory first: a binding that names no daemon would otherwise claim its drags.
    if (identityOf(_localFactory) == host)
        return {};
    // One daemon may serve several windows, each through a link of its own.
    auto serving = std::vector<WindowBinding*> {};
    for (auto* const binding: _bindingOfWindow | std::views::values)
        if (!std::ranges::contains(serving, binding) && identityOf(binding->sessions()) == host)
            serving.push_back(binding);
    return serving;
}

WindowBinding* TerminalSessionManager::bindingServing(window::HostIdentity const& host) const
{
    auto const serving = bindingsServing(host);
    return serving.empty() ? nullptr : serving.front();
}

bool TerminalSessionManager::isOwnHost(window::HostIdentity const& host) const
{
    return identityOf(_localFactory) == host || bindingServing(host) != nullptr;
}

SessionFactory& TerminalSessionManager::factoryServing(window::HostIdentity const& host) const
{
    auto* const binding = bindingServing(host);
    return binding != nullptr ? binding->sessions() : _localFactory;
}

window::MoveRefusal TerminalSessionManager::moveRefusalIn(ResolvedDrag const& drag,
                                                          vtworkspace::WorkspaceId window,
                                                          window::MovePlace place) const
{
    // Only the host of the window the drag came from could make the move, and it does not serve that window:
    // two windows of this process, served by two hosts (two daemons, or one and this process itself). Two
    // links of one daemon are one host: it moves between the workspaces they show.
    if (place != window::MovePlace::NewWindow && drag.tab && !servedByOneHost(drag.workspace, window))
        return window::MoveRefusal::OtherHost;
    return drag.refusalAt(place);
}

bool TerminalSessionManager::servedByOneHost(vtworkspace::WorkspaceId first,
                                             vtworkspace::WorkspaceId second) const noexcept
{
    auto const* const one = bindingOf(first);
    auto const* const other = bindingOf(second);
    if (one == other)
        return true;
    if (one == nullptr || other == nullptr)
        return false;
    auto const daemon = daemonOf(factoryFor(first));
    return daemon.has_value() && daemon == daemonOf(factoryFor(second));
}

SessionFactory& TerminalSessionManager::factoryOfDrag(ResolvedDrag const& drag) const
{
    return drag.tab ? factoryFor(drag.workspace) : factoryServing(drag.drag.host);
}

bool TerminalSessionManager::isNormalWindow(vtworkspace::WorkspaceId window) const noexcept
{
    auto const* const controller = controllerDisplaying(window);
    return controller != nullptr
           && window::windowRoleRow(controller->role()).level == window::WindowLevel::Normal;
}

window::WindowController* TerminalSessionManager::applicationController() const noexcept
{
    // Panels, the hidden quick terminal among them, are skipped throughout: an application command run
    // in a window the user cannot see would do its work out of sight.
    for (auto const& window: { _focusedWindow, _lastFocusedWindow })
        if (window && isNormalWindow(*window))
            return controllerDisplaying(*window);
    // The oldest window left: ids are minted ascending, and the map has no order of its own.
    auto* oldest = static_cast<window::WindowController*>(nullptr);
    auto oldestId = uint64_t {};
    for (auto const& [id, controller]: _controllersByWindow)
        if (window::windowRoleRow(controller->role()).level == window::WindowLevel::Normal
            && (oldest == nullptr || id.value < oldestId))
        {
            oldest = controller;
            oldestId = id.value;
        }
    return oldest;
}

void TerminalSessionManager::syncFocusForWindow(window::WindowController* controller)
{
    if (controller == nullptr)
        return;
    // Only the focus-owning window moves focus, so a background window can't steal it. This is also the
    // seam that focuses a session swapped onto an already-focused display, where no Qt focus event
    // fires at all -- which is the whole reason a tab switch notifies anyone. @see _focusedWindow.
    if (_focusedWindow == controller->displayedWorkspace())
        setFocusedSession(controller->activeSession());
}

platform::SessionVisibility TerminalSessionManager::sessionVisibility(TerminalSession const& session) const
{
    auto const [tab, win, controller] = hostOf(&session);

    // A zoomed tab draws its active pane alone, so every other pane of it is off screen.
    auto const paneShown =
        tab != nullptr && (!tab->isZoomed() || tab->activePane()->session() == session.modelSessionId());

    return platform::sessionVisibilityFor(platform::SessionPlacement {
        .focus =
            &session == _focusedSession ? platform::SessionFocus::Here : platform::SessionFocus::Elsewhere,
        .tab = win != nullptr && win->activeTab() == tab
                       && (controller == nullptr || controller->displayedWorkspace() == win->id())
                   ? platform::TabSelection::Active
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

TerminalSession* TerminalSessionManager::createSession(vtworkspace::WorkspaceId window,
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
    vtworkspace::WorkspaceId window,
    config::Layout const& layout,
    std::optional<vtbackend::PageSize> pageSize,
    std::function<void(config::LayoutPane const&)> const& beforeLeafSeed)
{
    if (layout.tabs.empty())
    {
        managerLog()("Layout has no tabs; nothing to apply.");
        return false;
    }
    auto& factory = factoryFor(window);
    if (!factory.canCreateSession())
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
            return createBackingSession(factory, sessionId, std::move(cwd), pageSize, command, profileName)
                   != nullptr;
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
    return applyLayoutToWindow(controller->displayedWorkspace(), *layout);
}

void TerminalSessionManager::saveLayout(std::string const& name, TerminalSession* acting)
{
    auto* win = windowHostingSession(acting);
    if (win == nullptr)
        return;
    // A name configured into a SaveLayout binding is the intent: it replaces the layout that has it.
    if (auto const saved = saveWindowLayout(win->id(), name, window::LayoutReplace::Replace); !saved)
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

std::vector<std::string> TerminalSessionManager::savedLayoutNames() const
{
    auto names = std::vector<std::string> {};
    for (auto const& name: _app.config().layouts.value() | std::views::keys)
        names.push_back(name);
    if (auto const stored = _layoutStore.load(layoutsFilePath()); stored)
        for (auto const& name: *stored | std::views::keys)
            if (std::ranges::find(names, name) == names.end())
                names.push_back(name);
    return names;
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
    auto const* const controller = controllerHostingSession(acting);
    _app.openSettings(controller != nullptr ? controller->screen() : nullptr);
}

void TerminalSessionManager::openConnectPicker(TerminalSession* acting)
{
    auto const* const controller = controllerHostingSession(acting);
    _app.openConnectPicker(ContourGuiApp::PickerPresentation::Sheet,
                           controller != nullptr ? controller->screen() : nullptr);
}

namespace
{
    /// @return Why the switching action @p what does nothing for a window that has no daemon to switch from.
    void logNoDaemon(std::string_view what)
    {
        managerLog()("{}: this window's sessions run in-process, so there is no daemon to switch from.",
                     what);
    }
} // namespace

void TerminalSessionManager::newWorkspace(TerminalSession const& acting)
{
    auto* const controller = hostOf(&acting).controller;
    if (controller == nullptr)
        return;
    // On a daemon, the daemon makes the workspace and the window selects it.
    if (daemonTargetOf(controller->displayedWorkspace()))
    {
        requestSwitch(
            acting,
            { .kind = vthost::proto::SwitchDaemonKind::Same, .daemon = {}, .workspace = NewWorkspace {} },
            SwitchMode::Switch);
        return;
    }
    // A window whose binding has no daemon (a tmux window) holds what its binding gives it.
    if (bindingOf(controller->displayedWorkspace()) != nullptr)
    {
        managerLog()("NewWorkspace: this window's workspaces come from what it is attached to.");
        postNotice(window::workspaceRefusedNotice(), &acting);
        return;
    }
    auto const workspace = createWorkspaceIn(controller->windowId());
    controller->displayWorkspace(workspace);
    createNewTab(workspace, std::nullopt);
}

void TerminalSessionManager::requestSwitch(TerminalSession const& acting,
                                           SwitchTarget target,
                                           SwitchMode mode)
{
    auto const* const window = hostOf(&acting).window;
    // An in-process window shows the Nth workspace it owns.
    if (auto* const controller = hostOf(&acting).controller;
        controller != nullptr && window != nullptr && mode == SwitchMode::Switch
        && bindingOf(window->id()) == nullptr && target.kind == vthost::proto::SwitchDaemonKind::Same)
        if (auto const* const at = std::get_if<WorkspaceAt>(&target.workspace);
            at != nullptr && at->position > 0)
        {
            selectWorkspace(controller->windowId(), at->position - 1);
            return;
        }
    if (window == nullptr || !daemonTargetOf(window->id()))
    {
        logNoDaemon("switch");
        return;
    }
    _windowSwitching.request(window->id(),
                             std::move(target),
                             mode,
                             NoLandingDeadline,
                             [](std::expected<vthost::proto::SwitchOutcome, std::string> const& outcome) {
                                 if (!outcome)
                                     managerLog()("Switch failed: {}", outcome.error());
                             });
}

void TerminalSessionManager::cycleDaemon(TerminalSession const& acting, actions::CycleDirection direction)
{
    auto const* const window = hostOf(&acting).window;
    auto const current = window != nullptr ? daemonTargetOf(window->id()) : std::nullopt;
    if (!current)
    {
        logNoDaemon("CycleDaemon");
        return;
    }
    auto const local = _switchDirectory.localDaemons();
    auto const hosts = _switchDirectory.hostsShownHere();
    // What is cached answers this press; the refresh is for the next one.
    _switchDirectory.refresh();
    if (auto next = nextDaemon(local, hosts, *current, direction))
        requestSwitch(acting, *std::move(next), SwitchMode::Switch);
    else
        managerLog()("CycleDaemon: there is no other daemon to show.");
}

void TerminalSessionManager::cycleWorkspace(TerminalSession const& acting, actions::CycleDirection direction)
{
    auto const* const window = hostOf(&acting).window;
    if (window == nullptr || !daemonTargetOf(window->id()))
    {
        logNoDaemon("CycleWorkspace");
        return;
    }
    auto const workspaces = _switchDirectory.workspacesOf(window->id());
    // What is cached answers this press; the refresh is for the next one.
    _switchDirectory.refresh();
    auto const here = std::ranges::find_if(
        workspaces, [](KnownWorkspace const& workspace) { return workspace.role == WorkspaceRole::Current; });
    if (workspaces.size() < 2 || here == workspaces.end())
    {
        managerLog()("CycleWorkspace: there is no other workspace to show.");
        return;
    }
    auto const at = static_cast<size_t>(std::distance(workspaces.begin(), here));
    auto const step = direction == actions::CycleDirection::Next ? 1 : workspaces.size() - 1;
    auto const& next = workspaces[(at + step) % workspaces.size()];
    requestSwitch(acting,
                  { .kind = vthost::proto::SwitchDaemonKind::Same,
                    .daemon = {},
                    .workspace = WorkspaceAt { .position = next.position } },
                  SwitchMode::Switch);
}

void TerminalSessionManager::showSwitcher(TerminalSession const& acting)
{
    openSwitcherOver(acting, window::SwitcherPrompt::Browse, "ShowSwitcher");
}

void TerminalSessionManager::newDaemon(TerminalSession const& acting)
{
    openSwitcherOver(acting, window::SwitcherPrompt::NewDaemon, "NewDaemon");
}

void TerminalSessionManager::upgradeDaemon(TerminalSession const& acting)
{
    auto const* const window = hostOf(&acting).window;
    auto* const binding = window != nullptr ? mutableBindingOf(window->id()) : nullptr;
    auto const started =
        binding != nullptr
            ? binding->upgradeDaemonOnCommand()
            : std::expected<void, std::string> { std::unexpected(std::string { NoDaemonToUpgrade }) };
    if (started)
        return;
    managerLog()("UpgradeDaemon: {}.", started.error());
    postNotice(window::upgradeFailedNotice(started.error()), &acting);
}

void TerminalSessionManager::openSwitcherOver(TerminalSession const& acting,
                                              window::SwitcherPrompt prompt,
                                              std::string_view what)
{
    auto const* const window = hostOf(&acting).window;
    auto const daemon = window != nullptr ? daemonTargetOf(window->id()) : std::nullopt;
    if (!daemon)
    {
        logNoDaemon(what);
        return;
    }
    // The palette's rows and the cycling actions read the directory; the popup asks the daemons itself.
    _switchDirectory.refresh();
    auto const* const controller = controllerHostingSession(&acting);
    if (auto const shown = _app.openSwitcher(window::SwitcherOpening { .window = window->id(),
                                                                       .windowDaemon = *daemon,
                                                                       .mode = SwitchMode::Switch,
                                                                       .prompt = prompt,
                                                                       .pick = std::nullopt },
                                             controller != nullptr ? controller->screen() : nullptr);
        !shown)
        managerLog()("{}: {}.", what, shown.error());
}

void TerminalSessionManager::postNotice(window::Notice notice, TerminalSession const* acting)
{
    if (auto* const controller = controllerHostingSession(acting); controller != nullptr)
        controller->notices().post(std::move(notice));
    else
        managerLog()("No window to tell: {}", notice.text);
}

namespace
{
    /// What @p session's shell is started with that decides where its startup files live: the profile's
    /// own `env` entries, and an empty value for a variable the profile removes from the inherited
    /// environment. The command override of a one-off pane is not visible here.
    [[nodiscard]] platform::RcEnvironment rcEnvironmentOf(TerminalSession const& session)
    {
        return platform::rcEnvironmentOf(session.profile().shell.value().env,
                                         session.profile().shell.value().removedEnvironment);
    }
} // namespace

void TerminalSessionManager::offerShellIntegration(TerminalSession& session, config::ShellKind shell)
{
    // An ended pane's footer owns its bottom rows, so such a pane takes no card (and so no slot).
    if (session.endReason() != EndReason::None)
        return;

    auto const inspection = _app.shellRcEditor().inspect(shell, rcEnvironmentOf(session));
    if (!inspection)
    {
        managerLog()("Not offering shell integration: {}.", platform::describe(inspection.error()));
        return;
    }

    auto dismissal = window::OfferDismissal::Open;
    if (auto const state = _app.guiState()->load())
        if (std::ranges::contains(state->dismissedShellIntegrations, shell))
            dismissal = window::OfferDismissal::Dismissed;

    auto const binary = _app.locatedContourBinary();
    auto const offer =
        window::offerFor({ .shell = shell,
                           .locality = hostLabelOf(session).empty() ? window::RcLocality::ThisMachine
                                                                    : window::RcLocality::Remote,
                           .host = _app.shellHostPlatform(),
                           .binary = binary.reach,
                           .dismissal = dismissal,
                           .rc = inspection->state,
                           .slot = _shellCardSlot,
                           .scripts = inspection->scripts });
    if (offer != window::ShellIntegrationOffer::Card)
        return;

    auto line = window::shellRcLine(shell, binary.path);
    if (!line)
    {
        managerLog()("Not offering shell integration: no safe line for {}.", binary.path.string());
        return;
    }
    _shellCardSlot = window::CardSlot::Taken;
    session.showShellIntegrationCard(shell, inspection->file, *std::move(line), inspection->scripts);
}

std::expected<platform::RcInstall, platform::ShellRcError> TerminalSessionManager::installShellIntegration(
    TerminalSession& session, config::ShellKind shell, std::string_view line)
{
    auto const installed = _app.shellRcEditor().install(shell, line, rcEnvironmentOf(session));

    // The file as the user would say it: asked of the editor again, so the toast names the very file written.
    auto const* const row = config::shellRow(shell);
    auto const shellName = row != nullptr ? row->displayName : std::string_view {};
    auto rc = std::string { shellName };
    if (auto const inspection = _app.shellRcEditor().inspect(shell, rcEnvironmentOf(session)))
        rc = window::displayRcPath(inspection->file, core::pathFromTrustedUtf8(_homeDirectory));

    if (installed)
        postNotice(
            window::Notice { .kind = window::NoticeKind::ShellIntegrationAdded,
                             .text = window::shellIntegrationInstalledText(*installed, rc, shellName) },
            &session);
    else
        postNotice(window::Notice { .kind = window::NoticeKind::ShellIntegrationFailed,
                                    .text = window::shellIntegrationFailedText(rc, installed.error()) },
                   &session);
    return installed;
}

void TerminalSessionManager::dismissShellIntegration(config::ShellKind shell)
{
    auto const saved = _app.guiState()->update([shell](config::GuiState& state) {
        if (!std::ranges::contains(state.dismissedShellIntegrations, shell))
            state.dismissedShellIntegrations.push_back(shell);
    });
    if (!saved)
        errorLog()("Could not remember declining shell integration: {}", config::describe(saved.error()));
}

void TerminalSessionManager::postNoticeToEveryWindow(window::Notice const& notice)
{
    for (auto* const controller: _controllersByWindow | std::views::values)
        if (controller != nullptr)
            controller->notices().post(notice);
}

void TerminalSessionManager::postNoticeToWindowsOf(WindowBinding const& binding, window::Notice const& notice)
{
    for (auto* const controller: controllersBoundTo(binding))
        controller->notices().post(notice);
}

void TerminalSessionManager::noteWorkspaceRestored(WindowBinding const& binding,
                                                   vthost::RestoredCounts counts)
{
    _linkStates[&binding].restoreSummary = counts;
}

void TerminalSessionManager::deliverRestoreSummaryTo(vtworkspace::WorkspaceId window)
{
    auto const state = _linkStates.find(bindingOf(window));
    if (state == _linkStates.end() || !state->second.restoreSummary)
        return;
    auto* const controller = controllerDisplaying(window);
    if (controller == nullptr)
        return;
    auto& summary = state->second.restoreSummary;
    controller->notices().post(
        window::Notice { .kind = window::NoticeKind::RestoreSummary,
                         .text = window::restoreSummaryText(summary->workspaces, summary->tabs) });
    summary.reset();
}

void TerminalSessionManager::expireRestoreSummary(WindowBinding const& binding) noexcept
{
    if (auto const state = _linkStates.find(&binding); state != _linkStates.end())
        state->second.restoreSummary.reset();
}

void TerminalSessionManager::clearNoticeInEveryWindow(window::NoticeKind kind)
{
    for (auto* const controller: _controllersByWindow | std::views::values)
        if (controller != nullptr)
            controller->notices().clear(kind);
}

void TerminalSessionManager::openConfigFile()
{
    auto const url = QUrl::fromLocalFile(QString::fromStdString(core::pathToUtf8(_app.config().configFile)));
    if (auto const opened = _app.externalLauncher().openUrl(url); !opened)
        errorLog()("Could not open the configuration file {}: {}.",
                   core::pathToUtf8(_app.config().configFile),
                   platform::describe(opened.error()));
}

std::expected<void, std::string> TerminalSessionManager::reloadAllSessions()
{
    // Read once, into a Config of its own, so a file that fails to load leaves the app's configuration
    // and every session as they were rather than half-replaced.
    auto loaded = config::tryLoadConfigFromFile(_app.config().configFile);
    if (!loaded)
    {
        // Said in every window, until a reload succeeds: the file is the application's, not a session's.
        postNoticeToEveryWindow(
            window::Notice { .kind = window::NoticeKind::ConfigReloadFailed,
                             .text = window::configReloadFailedNoticeText(loaded.error()) });
        return std::unexpected { std::move(loaded.error()) };
    }
    clearNoticeInEveryWindow(window::NoticeKind::ConfigReloadFailed);

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
    secureInputModeChanged();

    for (auto& [windowId, controller]: _controllersByWindow)
        if (controller != nullptr)
        {
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

void TerminalSessionManager::openBlockMenu(TerminalSession* acting,
                                           vtbackend::CommandBlockId block,
                                           std::optional<QPointF> anchor)
{
    emit blockMenuRequested(acting, block);

    auto* controller = controllerHostingSession(acting);
    if (controller == nullptr)
        return;

    // The menu's rows run against this pane, so it is the active one while the menu stands.
    if (auto const* tab = paneActionTargetTab(acting); tab != nullptr)
        activateSessionPane(*tab, *acting);

    controller->openBlockMenu(*acting, block, anchor);
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
    vtworkspace::WorkspaceId windowId, std::string const& name, window::LayoutReplace replace)
{
    if (name.empty())
        return std::unexpected(config::LayoutSaveError::EmptyName);

    auto* window = _model->workspace(windowId);
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
    if (replace == window::LayoutReplace::Keep
        && (storedLayouts->contains(name) || _app.config().layouts.value().contains(name)))
        return std::unexpected(config::LayoutSaveError::NameTaken);
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

window::FocusMove TerminalSessionManager::moveChromeFocus(TerminalSession const& session,
                                                          window::ChromeFocusAction action)
{
    if (auto* controller = controllerHostingSession(&session); controller != nullptr)
        return controller->moveChromeFocus(action);
    return window::FocusMove::Stayed;
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

void TerminalSessionManager::removeWindowController(window::WindowId windowId)
{
    // Drop the controller for a closed OS window. Real per-window identity: "last window" is
    // _controllersByWindow.size() == 1 (the manager's authoritative window registry), NOT a scan of
    // display keys. The controller has already removed its vtworkspace::Workspace, pruned its proxy tree and
    // terminated its own sessions (WindowController::closeWindow); here we just unregister + delete it,
    // and — if this was the last window — clear any residual shared session registries so no stale entry
    // outlives the process's last window.
    auto const it = _controllersByWindow.find(windowId);
    if (it == _controllersByWindow.end())
        return;

    bool const wasLast = _controllersByWindow.size() == 1;
    auto* controller = it->second;
    // A window bound to a daemon shows the one workspace it realized; that is what the binding and the focus
    // are keyed by.
    auto const workspace =
        controller != nullptr ? controller->displayedWorkspace() : vtworkspace::WorkspaceId {};
    _controllersByWindow.erase(it);
    _focusOrder.erase(windowId);
    _app.refreshWindowAttention();
    _taskbarProgressSink.removeWindow(windowId.value);
    if (controller != nullptr)
        controller->deleteLater();
    // The binding is told last: its sessions are gone, and it may stop serving once it serves no window.
    if (auto const bound = _bindingOfWindow.extract(workspace.value))
    {
        auto* const binding = bound.mapped();
        auto const left = forgetMember(*binding, workspace);
        binding->windowClosed(workspace);
        if (left == BindingMembers::None)
            binding->lastWindowReleased();
    }

    // Ownership must not survive the window that held it: a stale WorkspaceId would let a later model
    // event in an unrelated window re-grant focus through syncFocusForWindow. The window's sessions
    // were terminated by WindowController::closeWindow, and removeSession already dropped the focus
    // back-pointer for each, so the focus-out this issues finds nothing to notify.
    clearFocusedWindow(workspace);

    if (wasLast)
    {
        managerLog()("Last window closed: clearing residual session registries.");
        _sessionsById.clear();
        _tabBySession.clear();
    }
    emit windowRemoved();
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
    _model->moveTab(_model->workspaceOfTab(tab->id()), tab->id(), targetRow);
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
    // Resolved while the session still sits in its tab: its marks are in that window's ledger alone.
    auto* const ledger = ledgerOf(sessionId);
    auto const key = paneKeyOf(sessionId);

    // Drop the focus back-pointer if this was the focused session (without sending a focus-out to a
    // session that is already tearing down), so _focusedSession can never dangle.
    if (_focusedSession == &thatSession)
        _focusedSession = nullptr;
    // The prompt's pane is gone: the OS's secure input must not outlive it.
    refreshSecureInput();

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
            _model->closePane(_model->workspaceOfTab(tab->id()), tab->id(), leaf->id());
    _sessionsById.erase(sessionId.value);
    _tabBySession.erase(sessionId.value);
    if (ledger != nullptr)
        ledger->forget(key);
    for (auto* controller: _controllersByWindow | std::views::values)
        controller->forgetSession(sessionId);
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

void TerminalSessionManager::updateSystemScrollBars(std::optional<config::ScrollBarMode> system)
{
    for (auto* session: _sessionsById | std::views::values)
        session->applySystemScrollBars(system);
}

void TerminalSessionManager::refreshChromeDependents()
{
    for (auto* controller: _controllersByWindow | std::views::values)
    {
        controller->refreshHeaderFollowers();
        controller->refreshTitleBand();
    }
}

vtworkspace::Tab* TerminalSessionManager::tabAtRow(vtworkspace::WorkspaceId window, int index) const noexcept
{
    auto* win = _model->workspace(window);
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
// the WorkspaceId; pane events resolve TabId -> WorkspaceId via the model) and does no list-model / proxy
// work of its own. The manager's own QAbstractListModel emissions and proxy tree — which Stage 2 kept as a
// harmless unobserved superset — are gone; the status-line fan-out (updateStatusLine) still lives here until
// Stage 4 moves it onto the controllers.
void TerminalSessionManager::tabAboutToBeAdded(vtworkspace::WorkspaceId window, int index)
{
    if (auto* c = controllerDisplaying(window))
        c->onTabAboutToBeAdded(index);
}

void TerminalSessionManager::tabAdded(vtworkspace::WorkspaceId window, vtworkspace::TabId id, int index)
{
    layoutChanged();
    if (auto* c = controllerDisplaying(window))
        c->onTabAdded(index);
    if (auto* tab = _model->findTab(id))
        if (auto* session = sessionForId(tab->rootPane()->firstLeaf()->session()))
            if (auto const& color = session->profile().tabColor.value(); color)
                _model->setTabColor(id, vtworkspace::TabColorSource::Profile, *color);
}

void TerminalSessionManager::tabAboutToBeRemoved(vtworkspace::WorkspaceId window, int index)
{
    if (auto* c = controllerDisplaying(window))
        c->onTabAboutToBeRemoved(index);
}

void TerminalSessionManager::tabClosed(vtworkspace::WorkspaceId window, vtworkspace::TabId tab, int)
{
    layoutChanged();
    _paneActivations.erase(tab.value);
    if (auto* c = controllerDisplaying(window))
        c->onTabClosed();
    // The workspace may have lost its last tab to a pane that exited: an in-process window with other
    // workspaces drops it, once the model is done with this event.
    if (auto* const owner = controllerOwning(window); owner != nullptr && owner->ownedWorkspaces().size() > 1)
        QMetaObject::invokeMethod(
            owner, [owner, window] { owner->dropWorkspaceIfEmpty(window); }, Qt::QueuedConnection);
}

void TerminalSessionManager::tabAboutToBeMoved(vtworkspace::WorkspaceId window, int fromIndex, int toIndex)
{
    if (auto* c = controllerDisplaying(window))
        c->onTabAboutToBeMoved(fromIndex, toIndex);
}

void TerminalSessionManager::tabMoved(vtworkspace::WorkspaceId window, vtworkspace::TabId, int, int)
{
    if (auto* c = controllerDisplaying(window))
        c->onTabMoved();
}

void TerminalSessionManager::tabAboutToBeMovedToWorkspace(vtworkspace::WorkspaceId from,
                                                          int fromIndex,
                                                          vtworkspace::WorkspaceId to,
                                                          int toIndex)
{
    // Qt has no cross-model "move rows"; a transplant is a remove on the source and an insert on the
    // destination. Bracket each on its own controller.
    if (auto* src = controllerDisplaying(from))
        src->onTabAboutToBeRemoved(fromIndex);
    if (auto* dst = controllerDisplaying(to))
        dst->onTabAboutToBeAdded(toIndex);
}

void TerminalSessionManager::tabMovedToWorkspace(
    vtworkspace::WorkspaceId from, vtworkspace::TabId, int, vtworkspace::WorkspaceId to, int toIndex)
{
    layoutChanged();
    // Close-half on the source completes its beginRemoveRows; add-half on the destination completes its
    // beginInsertRows (bracketed in tabAboutToBeMovedToWorkspace with the SAME toIndex). onTabAdded ignores
    // the index — endInsertRows needs none — but we pass the event's toIndex, not a re-derived one.
    if (auto* src = controllerDisplaying(from))
        src->onTabClosed();
    if (auto* dst = controllerDisplaying(to))
        dst->onTabAdded(toIndex);
    scheduleTaskbarProgress();
}

void TerminalSessionManager::activeTabChanged(vtworkspace::WorkspaceId window, vtworkspace::TabId tab, int)
{
    if (auto* c = controllerDisplaying(window))
    {
        c->onActiveTabChanged();
        // Re-point focus at the new active leaf, after the rebuild so activeSession() is current; this
        // also covers cross-window moves, which route through activeTabChanged on the destination.
        syncFocusForWindow(c);
    }
    // The tab now in front is seen, which clears its badge's marks.
    markVisibleSessionsSeen(window);
    reportTabPresentation(tab);
}

void TerminalSessionManager::paneSplit(vtworkspace::TabId tab, vtworkspace::PaneId, vtworkspace::PaneId)
{
    layoutChanged();
    if (auto* c = controllerDisplaying(_model->workspaceOfTab(tab)))
    {
        c->notifyTabRowChanged(
            tab,
            { window::WindowController::Roles::TitleRole, window::WindowController::Roles::PaneCountRole });
        c->rebuildActiveTabPaneProxies();
    }
}

void TerminalSessionManager::paneClosed(vtworkspace::TabId tab, vtworkspace::PaneId, vtworkspace::PaneId)
{
    layoutChanged();
    if (auto* c = controllerDisplaying(_model->workspaceOfTab(tab)))
    {
        c->notifyTabRowChanged(
            tab,
            { window::WindowController::Roles::TitleRole, window::WindowController::Roles::PaneCountRole });
        c->rebuildActiveTabPaneProxies();
    }
}

void TerminalSessionManager::activePaneChanged(vtworkspace::TabId tab, vtworkspace::PaneId)
{
    // A FocusPane* move is the keyboard's; anything else that moves the active pane -- a split, a close, a
    // swap, a revealed session -- is not. Written before the proxies below are told.
    _paneActivations[tab.value] =
        _keyboardPaneMove == tab ? window::PaneActivation::Keyboard : window::PaneActivation::Pointer;
    if (auto* c = controllerDisplaying(_model->workspaceOfTab(tab)))
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
    if (auto* c = controllerDisplaying(_model->workspaceOfTab(tab)))
        c->notifyRatioChanged(splitNode);
    reportSplitRatio(tab, splitNode, ratio);
}

void TerminalSessionManager::reportSplitRatio(vtworkspace::TabId tab,
                                              vtworkspace::PaneId splitNode,
                                              double ratio)
{
    // A local window's splits are its own: no host is told of them.
    auto* const binding = mutableBindingOf(_model->workspaceOfTab(tab));
    if (binding == nullptr)
        return;
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
        binding->sessions().reportSplitRatio(first, second, ratio);
}

void TerminalSessionManager::reportTabPresentation(vtworkspace::TabId tabId)
{
    // Every model event below reaches here, local tabs' included: nothing is computed for those.
    auto const windowId = _model->workspaceOfTab(tabId);
    auto* const binding = mutableBindingOf(windowId);
    if (binding == nullptr)
        return;
    auto& factory = binding->sessions();
    if (factory.hosting() != SessionHosting::Remote)
        return;
    auto const* tab = _model->findTab(tabId);
    auto const* window = tab != nullptr ? _model->workspace(windowId) : nullptr;
    if (window == nullptr || tab->activePane() == nullptr)
        return;
    auto const ptyOf = [this](vtworkspace::Pane const* leaf) -> vtpty::Pty const* {
        auto* session = leaf != nullptr ? sessionForId(leaf->session()) : nullptr;
        return session != nullptr ? &session->terminal().device() : nullptr;
    };
    auto const* active = ptyOf(tab->activePane());
    if (active == nullptr)
        return; // the active pane has no session yet: a pane being built, which reports once it has one
    factory.reportTabPresentation(LocalTabPresentation {
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
    if (auto* c = controllerDisplaying(_model->workspaceOfTab(tab)))
        c->rebuildActiveTabPaneProxies();
}

void TerminalSessionManager::paneSwapped(vtworkspace::TabId tab, vtworkspace::PaneId, vtworkspace::PaneId)
{
    layoutChanged();
    // A swap moves sessions between two leaves (ids unchanged). The rebuild re-binds each proxy's
    // `session`, so the two affected panes render their new terminals.
    if (auto* c = controllerDisplaying(_model->workspaceOfTab(tab)))
        c->rebuildActiveTabPaneProxies();
    // Defensive: a swap in a zoomed tab ends the zoom (vtworkspace::Tab), so paneZoomChanged() reports it
    // and marks the uncovered panes seen already. Marking here as well keeps the session now in front seen
    // whichever seam reports the swap, and whatever the model does about zoom in the future.
    markVisibleSessionsSeen(_model->workspaceOfTab(tab));
}

void TerminalSessionManager::paneSessionReplaced(vtworkspace::TabId tab, vtworkspace::PaneId leaf)
{
    layoutChanged();
    // A restart: one leaf carries a new session under the same id. Re-reading that leaf's `session` is
    // all the view needs -- QML rebinds the pane's display to it -- so this is no proxy rebuild.
    if (auto* c = controllerDisplaying(_model->workspaceOfTab(tab)))
        c->notifyPaneSessionReplaced(tab, leaf);
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
    if (auto* c = controllerDisplaying(_model->workspaceOfTab(tab)))
    {
        c->notifyTabRowChanged(tab, { window::WindowController::Roles::ZoomedRole });
        c->refreshActiveTabLayoutRoot();
    }
    // A zoom that ends uncovers the siblings it hid: they are seen now.
    markVisibleSessionsSeen(_model->workspaceOfTab(tab));
    reportTabPresentation(tab);
}

void TerminalSessionManager::paneTreeRestructured(vtworkspace::TabId tab)
{
    layoutChanged();
    // A move re-parents nodes and re-homes ids unpredictably, so re-read the whole tab's tree.
    if (auto* c = controllerDisplaying(_model->workspaceOfTab(tab)))
    {
        c->notifyTabRowChanged(
            tab,
            { window::WindowController::Roles::TitleRole, window::WindowController::Roles::PaneCountRole });
        c->rebuildActiveTabPaneProxies();
    }
    // A move may bring a covered pane in front, as a swap does.
    markVisibleSessionsSeen(_model->workspaceOfTab(tab));
}

void TerminalSessionManager::tabTitleChanged(vtworkspace::TabId tab)
{
    // The indicator status line's {Tabs} entry is built from the tab titles, so republish it here
    // (Stage 4 moves this fan-out onto the controllers); otherwise a renamed tab keeps its old name in
    // the status line until an unrelated event next refreshes it.
    updateStatusLine();
    if (auto* c = controllerDisplaying(_model->workspaceOfTab(tab)))
        c->notifyTabRowChanged(
            tab,
            { window::WindowController::Roles::TitleRole, window::WindowController::Roles::RawTitleRole });
    reportTabPresentation(tab);
}

void TerminalSessionManager::tabColorChanged(vtworkspace::TabId tab)
{
    updateStatusLine();
    if (auto* c = controllerDisplaying(_model->workspaceOfTab(tab)))
        c->notifyTabRowChanged(tab, { window::WindowController::Roles::ColorRole });
    reportTabPresentation(tab);
}

void TerminalSessionManager::refreshAllTabTitles()
{
    // Every window's tab strip may show {TabPosition}-derived labels; let each
    // controller re-emit for its own rows. (The manager itself no longer owns a list model.)
    for (auto* controller: _controllersByWindow | std::views::values)
        controller->refreshAllTabTitles();
}

void TerminalSessionManager::refreshTabForSession(vtworkspace::SessionId session)
{
    // Route to the OWNING window's controller — that list-model is what the visible tab strip
    // renders from. (Emitting on the manager reached nothing: no QML binds the manager's rows.)
    if (auto* tab = findTabHostingSession(session))
        if (auto* c = controllerDisplaying(_model->workspaceOfTab(tab->id())))
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
    if (auto* c = controllerDisplaying(_model->workspaceOfTab(tab->id())))
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

void TerminalSessionManager::refreshTabBadgeForSession(vtworkspace::SessionId session)
{
    // The records changed: the pane's own status may have, as well as the tab's.
    if (auto* s = sessionForId(session))
    {
        refreshPaneProgramStatus(*s);
        // A report landing in a session the user is looking at is seen as it arrives: its tab never shows
        // Done or Error for it.
        if (sessionVisibility(*s) != platform::SessionVisibility::Hidden)
            if (auto* const ledger = ledgerOf(session))
                std::ignore = ledger->noteSeen(paneKeyOf(session), statusGenerationOf(*s));
    }
    refreshTabBadgeOf(session);
}

uint64_t TerminalSessionManager::statusGenerationOf(TerminalSession const& session)
{
    // The lock the parser thread holds while it changes the records.
    auto const& terminal = session.terminal();
    auto const _ = std::lock_guard { terminal };
    auto const generations = terminal.programStatus().records()
                             | std::views::transform(&vtbackend::ProgramStatusRecord::generation);
    return std::ranges::fold_left(
        generations, uint64_t { 0 }, [](uint64_t a, uint64_t b) { return std::max(a, b); });
}

void TerminalSessionManager::refreshTabBadgeOf(vtworkspace::SessionId session)
{
    // Only the badge's roles, for the reason refreshTabProgressForSession gives.
    if (auto* tab = findTabHostingSession(session))
        refreshTabBadge(*tab);
}

void TerminalSessionManager::refreshPaneProgramStatus(TerminalSession& session)
{
    auto const badge = [&] {
        // The lock the parser thread holds while it changes the records.
        auto const& terminal = session.terminal();
        auto const _ = std::lock_guard { terminal };
        return window::paneStatusBadgeOf(window::resolvedRecords(terminal.programStatus()));
    }();
    session.setPaneProgramStatus(badge.kind, QString::fromStdString(badge.text));
}

void TerminalSessionManager::refreshTabBadge(vtworkspace::Tab const& tab)
{
    scheduleTaskbarProgress(); // the waiting-tab count follows the badges
    if (auto* c = controllerDisplaying(_model->workspaceOfTab(tab.id())))
        c->notifyTabRowChanged(tab.id(),
                               { window::WindowController::Roles::BadgeRole,
                                 window::WindowController::Roles::BadgeTextRole,
                                 window::WindowController::Roles::BadgeProgressRole });
}

void TerminalSessionManager::noteInputEchoChanged(TerminalSession const& session)
{
    // A session that was already removed (an end announced while it awaits deletion) has no tab to repaint,
    // and the gate was refreshed when it went.
    if (sessionForId(session.modelSessionId()) != &session)
        return;
    notifySecureTab(session);
    refreshSecureInput();
}

void TerminalSessionManager::notifySecureTab(TerminalSession const& session)
{
    if (auto* tab = findTabHostingSession(session.modelSessionId()))
        if (auto* c = controllerDisplaying(_model->workspaceOfTab(tab->id())))
            c->notifyTabRowChanged(tab->id(), { window::WindowController::Roles::SecureRole });
}

void TerminalSessionManager::refreshSecureInput()
{
    auto const app =
        _focusedWindow.has_value() ? window::AppActivity::Foreground : window::AppActivity::Background;
    auto const focusedEcho =
        _focusedSession != nullptr ? _focusedSession->inputEcho() : vtpty::InputEcho::Unknown;
    _secureInputGate.apply(window::desiredSecureInput(_app.config().secureInput.value(), app, focusedEcho));

    // The one pane the OS secures now. The words of every lock claim it only for that pane, so the pane that
    // had it and the pane that has it are re-announced, with their tabs.
    auto const secured =
        _secureInputGate.state() == platform::SecureInputState::On && _focusedSession != nullptr
            ? std::optional { _focusedSession->modelSessionId() }
            : std::nullopt;
    if (secured == _securedSession)
        return;
    for (auto const id: { _securedSession, secured })
        if (id.has_value())
            if (auto* session = sessionForId(*id); session != nullptr)
            {
                emit session->secureIndicatorChanged();
                notifySecureTab(*session);
            }
    _securedSession = secured;
}

void TerminalSessionManager::secureInputModeChanged()
{
    for (auto* session: _sessionsById | std::views::values)
        if (session != nullptr)
            emit session->secureIndicatorChanged();
    for (auto* controller: _controllersByWindow | std::views::values)
        if (controller != nullptr)
            controller->notifyAllTabRowsChanged({ window::WindowController::Roles::SecureRole });
    refreshSecureInput();
}

void TerminalSessionManager::noteSessionOutput(vtworkspace::SessionId session, window::OutputOrigin origin)
{
    auto const* s = sessionForId(session);
    if (s == nullptr)
        return;
    // Asked of the ledger first: a session that is on screen, or already marked, repaints nothing.
    auto* const ledger = ledgerOf(session);
    if (ledger == nullptr)
        managerLog()("No window ledger for session {}: its output is not marked.", session.value);
    if (ledger != nullptr
        && ledger->noteOutput(paneKeyOf(session), sessionVisibility(*s), origin)
               == window::AttentionChange::Changed)
        refreshTabBadgeOf(session);
}

void TerminalSessionManager::noteBell(vtworkspace::SessionId session, window::OutputOrigin origin)
{
    auto const* s = sessionForId(session);
    if (s == nullptr)
        return;
    auto* const ledger = ledgerOf(session);
    if (ledger == nullptr)
        managerLog()("No window ledger for session {}: its bell is not marked.", session.value);
    if (ledger != nullptr
        && ledger->noteBell(paneKeyOf(session), sessionVisibility(*s), origin)
               == window::AttentionChange::Changed)
        refreshTabBadgeOf(session);
}

void TerminalSessionManager::noteCommandFinished(TerminalSession const& session,
                                                 vtbackend::CommandEnding ending)
{
    // A finish queued before the session was removed may arrive after, while it awaits deletion: it must not
    // mark a session the ledger has forgotten.
    if (sessionForId(session.modelSessionId()) != &session)
        return;
    auto* const ledger = ledgerOf(session.modelSessionId());
    if (ledger == nullptr)
        managerLog()("No window ledger for session {}: its finished command is not marked.",
                     session.modelSessionId().value);
    if (ledger != nullptr
        && ledger->noteFinished(paneKeyOf(session.modelSessionId()), sessionVisibility(session), ending)
               == window::AttentionChange::Changed)
        refreshTabBadgeOf(session.modelSessionId());
}

void TerminalSessionManager::notifyDisplaysExposureChanged(vtworkspace::WorkspaceId window) const
{
    for (auto* const session: sessionsOfWindow(window))
        session->displayExposureChanged();
}

void TerminalSessionManager::markVisibleSessionsSeen(vtworkspace::WorkspaceId window)
{
    auto const* win = _model->workspace(window);
    auto* const ledger =
        controllerOwning(window) != nullptr ? &controllerOwning(window)->tabAttention() : nullptr;
    if (win == nullptr || ledger == nullptr)
        return;
    for (auto const row: std::views::iota(0, win->tabCount()))
    {
        auto const* tab = win->tabAt(row);
        if (tab == nullptr || tab->rootPane() == nullptr)
            continue;
        auto changed = window::AttentionChange::Unchanged;
        tab->rootPane()->walkTree([&](vtworkspace::Pane& pane) {
            auto const* s = pane.isLeaf() ? sessionForId(pane.session()) : nullptr;
            if (s != nullptr && sessionVisibility(*s) != platform::SessionVisibility::Hidden
                && ledger->noteSeen(paneKeyOf(pane.session()), statusGenerationOf(*s))
                       == window::AttentionChange::Changed)
                changed = window::AttentionChange::Changed;
        });
        if (changed == window::AttentionChange::Changed)
            refreshTabBadge(*tab);
    }
}

void TerminalSessionManager::setHostConnection(WindowBinding const& binding,
                                               vthost::client::ConnectionState state)
{
    auto& connection = _linkStates[&binding].connection;
    auto const chipChanged = window::hostChipStateOf(state) != window::hostChipStateOf(connection);
    connection = state;
    // A move asked of a connection that is gone is answered on it or not at all.
    if (state != vthost::client::ConnectionState::Connected && _pendingRemoteMove
        && _pendingRemoteMove->binding == &binding)
        _pendingRemoteMove.reset();
    // What the host serves, and which run of it, may differ once it is back.
    layoutChanged();
    for (auto* session: sessionsBoundTo(binding))
        session->setHostConnection(state);
    if (chipChanged)
        for (auto* const controller: controllersBoundTo(binding))
            controller->notifyHostConnectionChanged();
}

vthost::client::ConnectionState TerminalSessionManager::hostConnection(
    vtworkspace::WorkspaceId window) const noexcept
{
    auto const state = _linkStates.find(bindingOf(window));
    return state != _linkStates.end() ? state->second.connection : vthost::client::ConnectionState::Connected;
}

platform::TaskbarProgress TerminalSessionManager::taskbarProgress(
    std::chrono::steady_clock::time_point now) const
{
    return _taskbarProgress.resolve(now, _app.config().progressTimeout.value());
}

platform::TaskbarProgress TerminalSessionManager::taskbarProgress(
    window::WindowId window, std::chrono::steady_clock::time_point now) const
{
    auto const* const controller = controllerOf(window);
    return _taskbarProgress.resolveFor(
        [this, controller](uint64_t session) {
            auto const found = _tabBySession.find(session);
            if (found == _tabBySession.end() || controller == nullptr)
                return false;
            auto const owned = controller->ownedWorkspaces();
            return std::ranges::find(owned, _model->workspaceOfTab(found->second)) != owned.end();
        },
        now,
        _app.config().progressTimeout.value());
}

void TerminalSessionManager::bindTaskbarWindow(window::WindowId window, QWindow& nativeWindow)
{
    _taskbarProgressSink.bindWindow(window.value, nativeWindow);
    // A (re)bound window starts with no overlay, so what it was last told no longer holds: forget it, and
    // the next refresh tells it its count again.
    std::erase_if(_publishedAttention.perWindow,
                  [&](auto const& told) { return told.first == window.value; });
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
        for (auto const& window: _controllersByWindow | std::views::keys)
            _taskbarProgressSink.setProgress(window.value, taskbarProgress(window, now));
    publishTaskbarAttention();
    if (auto const expiry = _taskbarProgress.nextExpiry(now, _app.config().progressTimeout.value()))
        _taskbarProgressTimer.start(std::chrono::ceil<std::chrono::milliseconds>(*expiry - now));
}

void TerminalSessionManager::publishTaskbarAttention()
{
    auto tabs = std::vector<std::pair<uint64_t, window::TabBadgeKind>> {};
    for (auto const& [window, controller]: _controllersByWindow)
    {
        // Listed even with no tab, so that a window that just lost its last one is told it waits for nothing.
        // A window counts every workspace it owns, the hidden ones included.
        tabs.emplace_back(window.value, window::TabBadgeKind::None);
        for (auto const kind: controller->tabBadgeKinds())
            tabs.emplace_back(window.value, kind);
    }
    auto tally = window::tallyWaitingTabs(tabs);
    // A daemon window counts the tabs of the workspaces it does not display too, from its directory.
    for (auto& [window, waiting]: tally.perWindow)
        if (auto const* const controller = controllerOf(window::WindowId { window }))
            if (auto const* const binding = bindingOf(controller->displayedWorkspace()))
            {
                auto const elsewhere = binding->tabsWaitingElsewhere();
                waiting += elsewhere;
                tally.total += elsewhere;
            }

    if (_taskbarProgressSink.scope() == platform::TaskbarProgressScope::Application)
    {
        // Windows on one daemon list the same workspaces: that daemon counts once, as its most recently
        // focused window counts it; an in-process window counts its own.
        auto windows = std::vector<window::WindowWaiting> {};
        for (auto const& [window, waiting]: tally.perWindow)
        {
            auto const id = window::WindowId { window };
            auto const* const controller = controllerOf(id);
            auto const* const binding =
                controller != nullptr ? bindingOf(controller->displayedWorkspace()) : nullptr;
            auto const focused = _focusOrder.find(id);
            windows.push_back({ .window = window,
                                .waiting = waiting,
                                .daemon = binding != nullptr ? binding->daemonKey() : std::nullopt,
                                .focusOrder = focused != _focusOrder.end() ? focused->second : 0 });
        }
        tally.total = window::applicationWaiting(windows);
        if (tally.total != _publishedAttention.total)
            _taskbarProgressSink.setApplicationAttention({ .waiting = tally.total });
    }
    else
        for (auto const& [window, waiting]: tally.perWindow)
        {
            auto const known = std::ranges::find(
                _publishedAttention.perWindow, window, &std::pair<uint64_t, unsigned>::first);
            if (known == _publishedAttention.perWindow.end() ? waiting != 0 : known->second != waiting)
                _taskbarProgressSink.setAttention(window, { .waiting = waiting });
        }
    _publishedAttention = std::move(tally);
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
void TerminalSessionManager::activateModelTabByRow(vtworkspace::WorkspaceId window, int row)
{
    auto* tab = tabAtRow(window, row);
    if (tab == nullptr)
        return;

    // Update the window's active tab: this fires activeTabChanged -> the owning controller rebuilds
    // its PaneProxy tree and rebinds activeTabRootPane, and the pane tree's `session:` binding
    // attaches the tab's active-leaf session to its display.
    _model->activateTab(window, tab->id());
}

void TerminalSessionManager::activateTab(vtworkspace::WorkspaceId window, int index)
{
    activateModelTabByRow(window, index);
}

void TerminalSessionManager::moveTab(vtworkspace::WorkspaceId window, int fromIndex, int toIndex)
{
    // fromIndex/toIndex are tab-strip rows (tab-space) of @p window. The tab order is owned by the
    // model, so reorder purely there and bounds-check against that window's tab count.
    auto* win = _model->workspace(window);
    if (win == nullptr)
        return;
    auto const tabCount = win->tabCount();
    if (fromIndex < 0 || fromIndex >= tabCount || toIndex < 0 || toIndex >= tabCount)
        return;
    moveTabByTab(tabAtRow(window, fromIndex), toIndex);
}

void TerminalSessionManager::closeWindowIfEmpty(vtworkspace::WorkspaceId window)
{
    // An empty window (no tabs) is not a valid state: after a cross-window move that took a window's
    // last tab, close it. Runs AFTER the transplant, so the moved tab's sessions are already gone from
    // this window and closeWindow()'s removeWindow won't tear them down.
    auto* win = _model->workspace(window);
    if (win == nullptr || !win->empty())
        return;
    // A reconcile still running may fill it again: closed, if still empty, once it is done.
    if (_emptiedWindowHolds > 0)
    {
        if (!std::ranges::contains(_emptiedWindows, window))
            _emptiedWindows.push_back(window);
        return;
    }
    // An in-process window with other workspaces drops the empty one and shows the next.
    if (auto* const owner = controllerOwning(window); owner != nullptr && owner->ownedWorkspaces().size() > 1)
    {
        owner->dropWorkspaceIfEmpty(window);
        return;
    }
    if (auto* controller = controllerDisplaying(window))
        controller->closeWindow();
}

TerminalSessionManager::EmptiedWindowHold::EmptiedWindowHold(TerminalSessionManager& manager) noexcept:
    _manager { manager }
{
    ++_manager._emptiedWindowHolds;
}

TerminalSessionManager::EmptiedWindowHold::~EmptiedWindowHold()
{
    if (--_manager._emptiedWindowHolds > 0)
        return;
    for (auto const window: std::exchange(_manager._emptiedWindows, {}))
        _manager.closeWindowIfEmpty(window);
}

void TerminalSessionManager::EmptiedWindowHold::keep(vtworkspace::WorkspaceId window) const
{
    std::erase(_manager._emptiedWindows, window);
}

void TerminalSessionManager::moveTabToWorkspace(vtworkspace::WorkspaceId from,
                                                int fromIndex,
                                                vtworkspace::WorkspaceId to,
                                                int toIndex)
{
    // Resolve the tab in tab-space of the source window, then hand the model TabIds — the model owns
    // the transplant (and fires the cross-window events the two controllers react to).
    auto* tab = tabAtRow(from, fromIndex);
    // Two hosts share no layout: a tab shown here would stay on the host it came from.
    if (tab == nullptr || bindingOf(from) != bindingOf(to))
        return;
    auto const leaves = tab->leafSessions();
    auto const carried = beginCarryingAttention(leaves);
    _model->moveTabToWorkspace(from, tab->id(), to, toIndex);
    carryAttention(carried);
    updateStatusLine();
    // Dragging a window's LAST tab elsewhere empties it — close the now-empty source window.
    closeWindowIfEmpty(from);
}

void TerminalSessionManager::postTabMergeRefused(TerminalSession const* acting)
{
    auto notice = window::Notice { .kind = window::NoticeKind::DropRefused,
                                   .text = std::string { window::TabMergeRefusedText } };
    if (acting != nullptr)
        postNotice(std::move(notice), acting);
    else
        postNoticeToEveryWindow(notice);
}

void TerminalSessionManager::mergeTabIntoSplit(vtworkspace::WorkspaceId from,
                                               int fromIndex,
                                               vtworkspace::WorkspaceId to,
                                               vtworkspace::TabId target,
                                               vtworkspace::PaneId leaf,
                                               vtworkspace::SplitSide side)
{
    auto* source = tabAtRow(from, fromIndex);
    if (source == nullptr)
        return;
    auto const sourceId = source->id();

    // A daemon owns the layout: it is asked to graft the tab, and its re-push reconciles the merge in. Only a
    // merge issued BY the reconciler itself (while realizing that re-push) is the local half of one the
    // daemon already made, and the factory answers Local for it. Anything else the daemon does not take is
    // refused, never made here: it would leave this window showing a layout the daemon does not have.
    // Two hosts share no layout: neither could make the merge, and none is made here.
    if (bindingOf(from) != bindingOf(to))
    {
        postTabMergeRefused(sessionForId(source->activePane()->session()));
        return;
    }
    auto& factory = factoryFor(from);
    if (factory.hosting() == SessionHosting::Remote)
    {
        auto* const targetTab = _model->findTab(target);
        auto const* const targetLeaf = targetTab != nullptr ? targetTab->rootPane()->findPane(leaf) : nullptr;
        auto* const targetSession = targetLeaf != nullptr ? sessionForId(targetLeaf->session()) : nullptr;
        // The whole tab is named, so the daemon can tell a request made of a stale view from this one.
        auto tabSessions = std::vector<TerminalSession*> {};
        auto tabPtys = std::vector<vtpty::Pty const*> {};
        for (auto const id: source->leafSessions())
        {
            auto* const session = sessionForId(id);
            tabSessions.push_back(session);
            tabPtys.push_back(session != nullptr ? &session->terminal().device() : nullptr);
        }
        // A session that is not found is asked about as no pane at all, so the factory decides (a
        // reconciler's merge is let through, any other is refused).
        switch (factory.requestRemoteTabMerge(
            tabPtys, targetSession != nullptr ? &targetSession->terminal().device() : nullptr, side))
        {
            case RemoteRouting::Routed: return;
            case RemoteRouting::Refused:
                postTabMergeRefused(targetSession != nullptr ? targetSession : tabSessions.front());
                return;
            case RemoteRouting::Local: break;
        }
    }

    // The merge's own events look sessions up by their tab (focus follows the grafted active pane), so
    // the index names the target before they fire; a refused merge puts it back.
    auto const sessions = source->leafSessions();
    for (auto const session: sessions)
        _tabBySession[session.value] = target;

    if (_model->mergeTabIntoSplit(from, sourceId, to, target, leaf, side) == nullptr)
    {
        for (auto const session: sessions)
            _tabBySession[session.value] = sourceId;
        return;
    }
    updateStatusLine();
    // Merging a window's LAST tab elsewhere empties it: close the now-empty source window.
    closeWindowIfEmpty(from);
}

vtworkspace::WorkspaceId TerminalSessionManager::windowOfSource(vtworkspace::MoveSource const& source) const
{
    return std::visit(
        [this](auto const& named) -> vtworkspace::WorkspaceId {
            using Named = std::decay_t<decltype(named)>;
            if constexpr (std::is_same_v<Named, vtworkspace::TabSource>)
                return named.workspace;
            else
            {
                auto const* const window = std::get<0>(_model->findSessionLeaf(named.session));
                return window != nullptr ? window->id() : vtworkspace::WorkspaceId {};
            }
        },
        source);
}

std::expected<vtworkspace::MoveResult, vtworkspace::MoveError> TerminalSessionManager::applyNodeMove(
    vtworkspace::MoveSource const& source, vtworkspace::MoveTarget const& target)
{
    auto const sourceWindow = windowOfSource(source);
    // Where a pane lands beside or onto another, its tab is known before the move: the move's own events look
    // sessions up by their tab (focus follows the moved pane), so the index names it before they fire.
    auto const paneTarget = std::visit(
        [](auto const& named) -> std::optional<vtworkspace::SessionId> {
            using Named = std::decay_t<decltype(named)>;
            if constexpr (std::is_same_v<Named, vtworkspace::EdgeTarget>
                          || std::is_same_v<Named, vtworkspace::SwapTarget>)
                return named.pane;
            else
                return std::nullopt;
        },
        target);
    auto const* const paneSource = std::get_if<vtworkspace::PaneSource>(&source);
    // What the entries said before, so a refused move puts them back.
    auto previous = std::vector<std::pair<uint64_t, std::optional<vtworkspace::TabId>>> {};
    auto const index = [&](vtworkspace::SessionId session, vtworkspace::TabId tab) {
        auto const found = _tabBySession.find(session.value);
        previous.emplace_back(session.value,
                              found != _tabBySession.end() ? std::optional { found->second } : std::nullopt);
        _tabBySession[session.value] = tab;
    };
    if (paneTarget && paneSource != nullptr)
        if (auto const* const targetTab = findTabHostingSession(*paneTarget))
        {
            // A swap sends the target pane the other way.
            if (auto const* const sourceTab = findTabHostingSession(paneSource->session);
                sourceTab != nullptr && std::holds_alternative<vtworkspace::SwapTarget>(target))
                index(*paneTarget, sourceTab->id());
            index(paneSource->session, targetTab->id());
        }

    auto const carriedSessions = std::visit(
        [this](auto const& named) -> std::vector<vtworkspace::SessionId> {
            using Named = std::decay_t<decltype(named)>;
            if constexpr (std::is_same_v<Named, vtworkspace::TabSource>)
            {
                auto const* const tab = _model->findTab(named.tab);
                return tab != nullptr ? tab->leafSessions() : std::vector<vtworkspace::SessionId> {};
            }
            else
                return { named.session };
        },
        source);
    auto const carried = beginCarryingAttention(carriedSessions);
    auto const moved = _model->moveNode(source, target);
    if (!moved)
    {
        for (auto const& [session, tab]: previous | std::views::reverse)
        {
            if (tab)
                _tabBySession[session] = *tab;
            else
                _tabBySession.erase(session);
        }
        return moved;
    }
    // Every leaf of the windows the move touched names its tab again: a pane may have left one tab for
    // another, or for a tab of its own the model just minted.
    for (auto const windowId: { sourceWindow, moved->workspace })
        if (auto* const window = _model->workspace(windowId))
            for (auto const index: std::views::iota(0, window->tabCount()))
                if (auto const* const tab = window->tabAt(index))
                    for (auto const session: tab->leafSessions())
                        _tabBySession[session.value] = tab->id();
    carryAttention(carried);
    updateStatusLine();
    // Moving a window's LAST pane elsewhere empties it: close the now-empty source window.
    if (sourceWindow != moved->workspace)
        closeWindowIfEmpty(sourceWindow);
    return moved;
}

window::HostIdentity TerminalSessionManager::hostIdentity(vtworkspace::WorkspaceId window) const
{
    return identityOf(factoryFor(window));
}

std::optional<uint64_t> TerminalSessionManager::hostSessionIdOf(TerminalSession const& session) const
{
    auto const& factory = factoryOf(session);
    if (identityOf(factory).kind == window::HostKind::Daemon)
        return factory.hostSessionIdOf(session.terminal().device());
    return session.modelSessionId().value;
}

TerminalSession* TerminalSessionManager::sessionForHostId(window::HostIdentity const& host,
                                                          uint64_t hostSession) const
{
    auto const hostedThere = [&](TerminalSession const& session) {
        return identityOf(factoryOf(session)) == host;
    };
    if (host.kind == window::HostKind::Process)
    {
        auto* const session = sessionForId(vtworkspace::SessionId { hostSession });
        return session != nullptr && hostedThere(*session) ? session : nullptr;
    }
    // A daemon's ids name its sessions only, in every window a link to it serves: the bindings serving it are
    // resolved once, and each session is judged by the binding of its window. A window bound to none is the
    // factory this manager was constructed with.
    auto const serving = bindingsServing(host);
    auto const servedHere = identityOf(_localFactory) == host;
    if (serving.empty() && !servedHere)
        return nullptr;
    auto const sessions = _sessionsById | std::views::values;
    auto const found = std::ranges::find_if(sessions, [&](TerminalSession const* session) {
        if (session == nullptr)
            return false;
        // Only one a pane shows. A move between two windows of one daemon briefly has two sessions mirror the
        // daemon's: the copy a reconcile closed out of its pane stays registered, in its old tab, until its
        // end is delivered. Taken for the shown one, it would hold the move -- by whichever the registry
        // happens to iterate first.
        auto const where = hostOf(session);
        if (where.window == nullptr || leafOfSession(*where.tab, *session) == nullptr)
            return false;
        auto const* const bound = bindingOf(where.window->id());
        auto const served = bound != nullptr ? std::ranges::contains(serving, bound) : servedHere;
        return served && hostSessionIdOf(*session) == hostSession;
    });
    return found != sessions.end() ? *found : nullptr;
}

std::optional<window::NodeDrag> TerminalSessionManager::paneDragOf(TerminalSession const& session) const
{
    auto const [win, tab, leaf] = _model->findSessionLeaf(session.modelSessionId());
    auto const host = hostSessionIdOf(session);
    if (leaf == nullptr || !host)
        return std::nullopt;
    return window::NodeDrag { .payload = window::DropPayload::Pane,
                              .host = hostIdentity(win->id()),
                              .window = win->id(),
                              .tabIndex = win->indexOf(tab->id()),
                              .sessions = { *host } };
}

std::optional<window::NodeDrag> TerminalSessionManager::tabDragOf(vtworkspace::WorkspaceId window,
                                                                  int tabIndex) const
{
    auto* const tab = tabAtRow(window, tabIndex);
    if (tab == nullptr)
        return std::nullopt;
    auto sessions = std::vector<uint64_t> {};
    auto complete = true;
    // The active pane first: it names the tab, and is what a tab of one pane drags as.
    auto const add = [&](vtworkspace::SessionId id) {
        auto const* const session = sessionForId(id);
        auto const host = session != nullptr ? hostSessionIdOf(*session) : std::nullopt;
        if (!host)
            complete = false;
        else if (!std::ranges::contains(sessions, *host))
            sessions.push_back(*host);
    };
    add(tab->activePane()->session());
    for (auto const id: tab->leafSessions())
        add(id);
    if (!complete)
        return std::nullopt;
    return window::NodeDrag { .payload = tab->hasMultiplePanes() ? window::DropPayload::Tab
                                                                 : window::DropPayload::Pane,
                              .host = hostIdentity(window),
                              .window = window,
                              .tabIndex = tabIndex,
                              .sessions = std::move(sessions) };
}

ResolvedDrag TerminalSessionManager::resolveNodeDrag(window::NodeDrag drag) const
{
    auto resolved = ResolvedDrag { .drag = std::move(drag),
                                   .tab = std::nullopt,
                                   .workspace = {},
                                   .subject = window::MoveSubject::Pane,
                                   .places = {},
                                   .layout = _layoutGeneration };
    auto const& named = resolved.drag;
    auto const from = isOwnHost(named.host) ? window::DragHost::Own : window::DragHost::Other;

    // The tab showing every session the drag names: a pane names the tab it is in; a tab names all of its
    // panes, and only them.
    auto const* const tab = [&]() -> vtworkspace::Tab const* {
        if (from == window::DragHost::Other || named.sessions.empty())
            return nullptr;
        auto local = std::vector<vtworkspace::SessionId> {};
        for (auto const host: named.sessions)
        {
            auto const* const session = sessionForHostId(named.host, host);
            if (session == nullptr)
                return nullptr;
            local.push_back(session->modelSessionId());
        }
        auto* const found = std::get<1>(_model->findSessionLeaf(local.front()));
        if (found == nullptr
            || !std::ranges::all_of(
                local, [&](vtworkspace::SessionId id) { return found->rootPane()->findLeaf(id) != nullptr; })
            || (named.payload == window::DropPayload::Tab
                && std::cmp_not_equal(found->paneCount(), local.size())))
            return nullptr;
        return found;
    }();
    if (tab != nullptr)
    {
        resolved.tab = tab->id();
        resolved.workspace = _model->workspaceOfTab(tab->id());
    }
    if (named.payload == window::DropPayload::Tab || (tab != nullptr && !tab->hasMultiplePanes()))
        resolved.subject = window::MoveSubject::WholeTab;

    auto const host = factoryOfDrag(resolved).hostMoves();
    for (auto const index: std::views::iota(std::size_t { 0 }, window::MovePlaceCount))
    {
        auto route = window::moveRouteOf(host, static_cast<window::MovePlace>(index), resolved.subject);
        // The tab verbs an older daemon served name the tab by this process's model.
        if (route == window::MoveRoute::TabMove && tab == nullptr)
            route = window::MoveRoute::Refused;
        resolved.places[index] =
            PlaceVerdict { .route = route, .refusal = window::moveRefusalOf(host, from, route) };
    }
    return resolved;
}

ResolvedDrag const* TerminalSessionManager::resolveNodeDrag(QString const& nodePayload) const
{
    // Resolved once, and again only once the layout it was resolved against changed.
    if (_nodeDrag && _nodeDrag->payload() == nodePayload)
    {
        if (_nodeDrag->drag().layout != _layoutGeneration)
            _nodeDrag->reresolve(resolveNodeDrag(_nodeDrag->drag().drag));
        return &_nodeDrag->drag();
    }
    if (!_hoveredDrag || _hoveredDrag->payload != nodePayload)
    {
        auto parsed = window::parseNodeDrag(nodePayload);
        _hoveredDrag = HoveredDrag { .payload = nodePayload,
                                     .drag = parsed ? std::optional { resolveNodeDrag(std::move(*parsed)) }
                                                    : std::nullopt };
    }
    else if (_hoveredDrag->drag && _hoveredDrag->drag->layout != _layoutGeneration)
        _hoveredDrag->drag = resolveNodeDrag(_hoveredDrag->drag->drag);
    return _hoveredDrag->drag ? &*_hoveredDrag->drag : nullptr;
}

std::optional<TerminalSessionManager::DescribedTarget> TerminalSessionManager::describeTarget(
    ResolvedDrag const& drag, vtworkspace::MoveTarget const& target) const
{
    // The daemon names every pane by its own session id; the target is one of this process's panes.
    auto const hostIdOf = [this](vtworkspace::SessionId local) -> std::optional<uint64_t> {
        auto const* const session = sessionForId(local);
        return session != nullptr ? hostSessionIdOf(*session) : std::nullopt;
    };
    auto const windowOfPane = [this](vtworkspace::SessionId pane) -> std::optional<vtworkspace::WorkspaceId> {
        auto const* const window = std::get<0>(_model->findSessionLeaf(pane));
        return window != nullptr ? std::optional { window->id() } : std::nullopt;
    };
    return std::visit(
        core::Overloaded {
            [&](vtworkspace::EdgeTarget const& edge) -> std::optional<DescribedTarget> {
                auto const window = windowOfPane(edge.pane);
                if (!window)
                    return std::nullopt;
                auto const place = drag.edgePlaceIn(*window);
                return DescribedTarget { .place = place,
                                         .landing = place == window::MovePlace::Edge
                                                        ? window::MoveLanding::Beside
                                                        : window::MoveLanding::BesideInOtherWindow,
                                         .workspace = window,
                                         .kind = vtworkspace::MoveTargetKind::Edge,
                                         .hostSession = hostIdOf(edge.pane),
                                         .side = edge.side,
                                         .index = 0 };
            },
            [&](vtworkspace::SwapTarget const& swap) -> std::optional<DescribedTarget> {
                auto const window = windowOfPane(swap.pane);
                if (!window)
                    return std::nullopt;
                return DescribedTarget { .place = window::MovePlace::Swap,
                                         .landing = window::MoveLanding::Swapped,
                                         .workspace = window,
                                         .kind = vtworkspace::MoveTargetKind::Swap,
                                         .hostSession = hostIdOf(swap.pane),
                                         .side = vtworkspace::SplitSide::Right,
                                         .index = 0 };
            },
            [&](vtworkspace::TabWellTarget const& well) -> std::optional<DescribedTarget> {
                auto const* const window = _model->workspace(well.workspace);
                if (window == nullptr)
                    return std::nullopt;
                // The daemon is told the workspace by any pane of it: its active one.
                auto const* const tab = window->activeTab();
                return DescribedTarget { .place = window::MovePlace::Gap,
                                         .landing = window::MoveLanding::NewTab,
                                         .workspace = well.workspace,
                                         .kind = vtworkspace::MoveTargetKind::TabWell,
                                         .hostSession = tab != nullptr
                                                            ? hostIdOf(tab->activePane()->session())
                                                            : std::nullopt,
                                         .side = vtworkspace::SplitSide::Right,
                                         .index = well.index };
            },
            [](vtworkspace::NewWorkspaceTarget const&) -> std::optional<DescribedTarget> {
                return DescribedTarget { .place = window::MovePlace::NewWindow,
                                         .landing = window::MoveLanding::NewWindow,
                                         .workspace = std::nullopt,
                                         .kind = vtworkspace::MoveTargetKind::NewWorkspace,
                                         .hostSession = 0,
                                         .side = vtworkspace::SplitSide::Right,
                                         .index = 0 };
            },
        },
        target);
}

std::optional<vtworkspace::MoveSource> TerminalSessionManager::localSourceOf(ResolvedDrag const& drag) const
{
    if (!drag.tab)
        return std::nullopt;
    if (drag.drag.payload == window::DropPayload::Tab)
        return vtworkspace::TabSource { .workspace = drag.workspace, .tab = *drag.tab };
    auto const* const session = sessionForHostId(drag.drag.host, drag.drag.sessions.front());
    if (session == nullptr)
        return std::nullopt;
    return vtworkspace::PaneSource { .session = session->modelSessionId() };
}

std::expected<void, MoveFailure> TerminalSessionManager::requestHostMove(ResolvedDrag const& drag,
                                                                         DescribedTarget target,
                                                                         QScreen* screen,
                                                                         QPoint at)
{
    if (!target.hostSession)
        return std::unexpected { MoveFailure { vtworkspace::MoveError::UnknownTarget } };
    auto const number = _nextRemoteMove++;
    auto const routing = factoryOfDrag(drag).requestRemoteMove(RemoteMove {
        .source = drag.drag.payload == window::DropPayload::Tab ? vtworkspace::MoveSourceKind::Tab
                                                                : vtworkspace::MoveSourceKind::Pane,
        .sourceSessions = drag.drag.sessions,
        .target = target.kind,
        .targetSession = *target.hostSession,
        .side = target.side,
        .index = target.index,
        .request = number });
    switch (routing)
    {
        case RemoteRouting::Routed: break;
        // Only the host may make it, and it would not: the routing is the reason.
        case RemoteRouting::Refused:
        case RemoteRouting::Local: return std::unexpected { MoveFailure { routing } };
    }
    // Waited for until the push that makes it ends: only then is there a window to place, announce in and
    // focus. A later move replaces it; the earlier one's push still lands, unannounced.
    _pendingRemoteMove = PendingRemoteMove {
        .request = number,
        .host = drag.drag.host,
        .binding = drag.tab ? bindingOf(drag.workspace) : bindingServing(drag.drag.host),
        .sessions = drag.drag.sessions,
        .payload = drag.announcedAs(target.landing),
        .landing = target.landing,
        .spawnAt =
            target.landing == window::MoveLanding::NewWindow ? spawnPositionFor(screen, at) : std::nullopt,
        .deadline = _app.terminalClocks().steady.now() + RemoteMoveTimeout,
        .phase = PendingRemoteMove::Phase::Asked,
    };
    return {};
}

std::optional<window::WindowPosition> TerminalSessionManager::spawnPositionFor(QScreen* screen, QPoint at)
{
    if (screen == nullptr)
        return std::nullopt;
    auto const monitor = QGuiApplication::screens().indexOf(screen);
    if (monitor < 0)
        return std::nullopt;
    return window::WindowPosition { .position = at - screen->availableGeometry().topLeft(),
                                    .monitor = static_cast<int>(monitor) };
}

void TerminalSessionManager::spawnWindowFor(PendingTransplant const& pending, QScreen* screen, QPoint at)
{
    _pendingTransplant = pending;
    _pendingSpawnPosition = spawnPositionFor(screen, at);
    _app.newWindow(screen);
}

void TerminalSessionManager::endWindowSpawn() noexcept
{
    // The new window consumed both as it was made; one that was not made leaves them for nobody.
    _pendingTransplant.reset();
    _pendingSpawnPosition.reset();
}

TerminalSessionManager::PendingRemoteMove const* TerminalSessionManager::livePendingRemoteMove()
    const noexcept
{
    return _pendingRemoteMove && _app.terminalClocks().steady.now() < _pendingRemoteMove->deadline
               ? &*_pendingRemoteMove
               : nullptr;
}

void TerminalSessionManager::remoteMoveRefused(uint64_t request)
{
    if (_pendingRemoteMove && _pendingRemoteMove->request == request)
        _pendingRemoteMove.reset();
}

void TerminalSessionManager::noteRemoteMovesLanded(std::span<uint64_t const> landed)
{
    if (auto const* const pending = livePendingRemoteMove();
        pending != nullptr && std::ranges::contains(landed, pending->request))
        _pendingRemoteMove->phase = PendingRemoteMove::Phase::Landing;
}

QScreen* TerminalSessionManager::stageRemoteMoveWindow(std::span<uint64_t const> daemonSessions)
{
    auto const* const pending = livePendingRemoteMove();
    if (pending == nullptr || pending->phase != PendingRemoteMove::Phase::Landing
        || pending->landing != window::MoveLanding::NewWindow || !pending->spawnAt
        || !std::ranges::all_of(pending->sessions, [&](uint64_t session) {
               return std::ranges::contains(daemonSessions, session);
           }))
        return nullptr;
    _pendingSpawnPosition = pending->spawnAt;
    return QGuiApplication::screens().value(pending->spawnAt->monitor, nullptr);
}

void TerminalSessionManager::completeRemoteMoves(ReconcileReach reach)
{
    auto const* const pending = livePendingRemoteMove();
    if (pending == nullptr || pending->phase != PendingRemoteMove::Phase::Landing
        || reach == ReconcileReach::Partial)
        return;
    // Placed once every session it moved is in a pane of some window; one not yet mirrored waits as well.
    auto const placed = [this, &host = pending->host](uint64_t hostId) {
        auto const* const session = sessionForHostId(host, hostId);
        if (session == nullptr)
            return false;
        auto const [window, tab, leaf] = _model->findSessionLeaf(session->modelSessionId());
        return window != nullptr && tab != nullptr && leaf != nullptr;
    };
    if (!std::ranges::all_of(pending->sessions, placed))
        return;
    auto const landed = *std::exchange(_pendingRemoteMove, std::nullopt);
    auto* const moved = sessionForHostId(landed.host, landed.sessions.front());
    auto const [window, tab, leaf] = _model->findSessionLeaf(moved->modelSessionId());
    // Where the user looks now: the window it landed in, the tab holding it, and the moved pane in that.
    _model->activateTab(window->id(), tab->id());
    activatePane(tab->id(), leaf->id());
    if (auto* const controller = controllerDisplaying(window->id()))
        controller->raiseAndActivate();
    setFocusedWindow(window->id());
    announceMove(vtworkspace::MoveResult { .workspace = window->id(),
                                           .tab = tab->id(),
                                           .focused = moved->modelSessionId() },
                 landed.payload,
                 landed.landing);
}

void TerminalSessionManager::announceMove(vtworkspace::MoveResult const& moved,
                                          window::DropPayload payload,
                                          window::MoveLanding landing) const
{
    auto* const controller = controllerDisplaying(moved.workspace);
    if (controller == nullptr)
        return;
    controller->announceMove(QString::fromStdString(
        window::moveAnnouncement(payload, landing, std::to_string(moved.workspace.value))));
}

std::expected<void, MoveFailure> TerminalSessionManager::moveTabLegacy(ResolvedDrag const& drag,
                                                                       vtworkspace::MoveTarget const& target,
                                                                       QScreen* screen,
                                                                       QPoint at)
{
    auto const* const window = drag.tab ? _model->workspace(drag.workspace) : nullptr;
    if (window == nullptr)
        return std::unexpected { MoveFailure { vtworkspace::MoveError::UnknownSource } };
    if (std::holds_alternative<vtworkspace::NewWorkspaceTarget>(target))
    {
        spawnWindowFor(PendingTransplant { .source = vtworkspace::TabSource { .workspace = drag.workspace,
                                                                              .tab = *drag.tab },
                                           .payload = drag.drag.payload },
                       screen,
                       at);
        return {};
    }
    auto const* const well = std::get_if<vtworkspace::TabWellTarget>(&target);
    if (well == nullptr)
        return std::unexpected { MoveFailure { vtworkspace::MoveError::UnknownTarget } };
    auto const row = window->indexOf(*drag.tab);
    if (well->workspace == drag.workspace)
        moveTab(drag.workspace, row, well->index);
    else
        moveTabToWorkspace(drag.workspace, row, well->workspace, well->index);
    return {};
}

QString TerminalSessionManager::beginNodeDrag(QObject const& owner,
                                              window::NodeDrag drag,
                                              TerminalSession* carried)
{
    // At most one OS drag is in flight: one a lost delegate left behind ends here, in its own window.
    std::ignore = takeNodeDrag();
    auto payload = window::writeNodeDrag(drag);
    _nodeDrag = std::make_unique<NodeDragSession>(
        owner, payload, resolveNodeDrag(std::move(drag)), carried, _app.dragEndProbe());
    emit nodeDragChanged();
    return payload;
}

std::unique_ptr<NodeDragSession> TerminalSessionManager::takeNodeDrag()
{
    if (!_nodeDrag)
        return nullptr;
    auto ended = std::exchange(_nodeDrag, nullptr);
    if (auto moves = ended->takeHeld(); !moves.empty())
        QMetaObject::invokeMethod(
            this,
            [moves = std::move(moves)] {
                for (auto const& move: moves)
                    move();
            },
            Qt::QueuedConnection);
    emit nodeDragChanged();
    return ended;
}

std::unique_ptr<NodeDragSession> TerminalSessionManager::endNodeDrag(QObject const* owner)
{
    if (!_nodeDrag || !_nodeDrag->isOwnedBy(owner))
        return nullptr;
    return takeNodeDrag();
}

void TerminalSessionManager::afterNodeDrag(std::function<void()> move)
{
    if (_nodeDrag)
        _nodeDrag->hold(std::move(move));
    else
        move();
}

TerminalSession* TerminalSessionManager::carriedPane(QObject const* owner) const noexcept
{
    return _nodeDrag && _nodeDrag->isOwnedBy(owner) ? _nodeDrag->carried() : nullptr;
}

std::expected<void, MoveFailure> TerminalSessionManager::moveNode(ResolvedDrag const& drag,
                                                                  vtworkspace::MoveTarget const& target,
                                                                  QScreen* screen,
                                                                  QPoint at)
{
    auto const described = describeTarget(drag, target);
    if (!described)
        return std::unexpected { MoveFailure { vtworkspace::MoveError::UnknownTarget } };
    // Decided again here, not taken from the window that asked: into a window, the window refuses it; out of
    // every window, it names nothing of this host's. Either way the refusal is the reason.
    auto const& verdict = drag.at(described->place);
    if (auto const refusal = described->workspace
                                 ? moveRefusalIn(drag, *described->workspace, described->place)
                                 : verdict.refusal;
        refusal != window::MoveRefusal::None)
        return std::unexpected { MoveFailure { refusal } };

    auto const sourceWindow = drag.tab ? drag.workspace : vtworkspace::WorkspaceId {};
    // The whole of a window, torn off, would only recreate itself: here, and on any daemon.
    if (auto const* const window = drag.tab ? _model->workspace(drag.workspace) : nullptr;
        described->place == window::MovePlace::NewWindow && window != nullptr && drag.movesWholeTab()
        && window->tabCount() == 1)
        return std::unexpected { MoveFailure { vtworkspace::MoveError::NoOp } };

    switch (verdict.route)
    {
        case window::MoveRoute::Refused:
            return std::unexpected { MoveFailure { vtworkspace::MoveError::UnknownTarget } };
        case window::MoveRoute::TabMove: return moveTabLegacy(drag, target, screen, at);
        case window::MoveRoute::Node: break;
    }

    // The daemon owns the layout: it makes the move, and its push brings it here.
    if (factoryOfDrag(drag).hostKind() == SessionHostKind::Daemon)
        return requestHostMove(drag, *described, screen, at);

    auto const source = localSourceOf(drag);
    if (!source)
        return std::unexpected { MoveFailure { vtworkspace::MoveError::UnknownSource } };

    if (std::holds_alternative<vtworkspace::NewWorkspaceTarget>(target))
    {
        spawnWindowFor(PendingTransplant { .source = *source, .payload = drag.drag.payload }, screen, at);
        return {};
    }

    auto const moved = applyNodeMove(*source, target);
    if (!moved)
        return std::unexpected { MoveFailure { moved.error() } };

    // The window it landed in is where the user is looking now.
    if (moved->workspace != sourceWindow)
    {
        if (auto* const controller = controllerDisplaying(moved->workspace))
            controller->raiseAndActivate();
        setFocusedWindow(moved->workspace);
    }
    announceMove(*moved, drag.announcedAs(described->landing), described->landing);
    return {};
}

void TerminalSessionManager::tearOffTabToNewWindow(vtworkspace::WorkspaceId from,
                                                   int fromIndex,
                                                   QScreen* targetScreen)
{
    auto* tab = tabAtRow(from, fromIndex);
    if (tab == nullptr)
        return;
    // A single-tab window torn off would just recreate itself — nothing to do.
    auto* srcWindow = _model->workspace(from);
    if (srcWindow != nullptr && srcWindow->tabCount() <= 1)
        return;

    // On a daemon nothing moves: a window of a connection of its own opens on the same workspace, the torn
    // tab active there.
    if (auto* const binding = mutableBindingOf(from); binding != nullptr && binding->daemonTarget())
    {
        std::ignore = openDaemonWindowLike(*binding, from, tab->id(), targetScreen, "Tab tear-off");
        return;
    }

    // Stage the transplant, then spawn a window. The new window's Main.qml consumes the staged tab in
    // Component.onCompleted (via consumePendingTransplant) INSTEAD of creating a fresh first tab, so the
    // torn-off tab becomes its sole tab — its sessions survive the move (nothing is torn down).
    _pendingTransplant = PendingTransplant {
        .source = vtworkspace::TabSource { .workspace = from, .tab = tab->id() },
        .payload = tab->hasMultiplePanes() ? window::DropPayload::Tab : window::DropPayload::Pane,
    };
    _app.newWindow(targetScreen);
}

bool TerminalSessionManager::consumePendingTransplant(window::WindowController* newController)
{
    if (!_pendingTransplant.has_value() || newController == nullptr)
        return false;
    auto const pending = *std::exchange(_pendingTransplant, std::nullopt);

    // The destination window currently has no tabs (its first-tab creation was deferred pending this). The
    // staged node moves in as its first tab, the same sessions alive; a source window it empties closes. One
    // closed since it was staged moves nothing, and the window opens its own first tab instead.
    // The window is served by whatever served the one the node came from: its sessions are that host's.
    if (auto* const binding = mutableBindingOf(windowOfSource(pending.source)))
        bindWindow(newController->displayedWorkspace(), *binding);
    auto const moved = applyNodeMove(
        pending.source,
        vtworkspace::TabWellTarget { .workspace = newController->displayedWorkspace(), .index = 0 });
    if (!moved)
        return false;
    announceMove(*moved, pending.payload, window::MoveLanding::NewWindow);
    return true;
}

void TerminalSessionManager::closeTabAtIndex(vtworkspace::WorkspaceId window, int index)
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
    for (auto const id: tab->leafSessions())
        if (auto* session = sessionForId(id))
            sessions.push_back(session);
    return sessions;
}

void TerminalSessionManager::authorRemoteEnd(TerminalSession* session)
{
    // A panel's sessions, as every local window's, run here and the host never heard of them.
    if (session != nullptr)
        factoryOf(*session).requestRemoteClose(&session->terminal().device());
}

void TerminalSessionManager::authorRemoteEnds(std::span<TerminalSession* const> sessions)
{
    for (auto* session: sessions)
        authorRemoteEnd(session);
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
    // Every end is authored before the first terminate(): each session must still hold the pty the remote
    // factory identifies it by, and still sit in its tab, whose window names the factory -- terminating the
    // tab's last pane takes the tab, and possibly the window and the link serving it, away.
    if (end == SessionEnd::Destroy)
        authorRemoteEnds(sessions);
    for (auto* session: sessions)
        session->terminate();
}

std::vector<TerminalSession*> TerminalSessionManager::gatherSessionsOfTabsWhere(
    vtworkspace::Workspace& window, std::function<bool(int row, vtworkspace::Tab*)> const& predicate) const
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

void TerminalSessionManager::closeOtherTabs(vtworkspace::WorkspaceId window, int index)
{
    auto* win = _model->workspace(window);
    auto* keep = tabAtRow(window, index);
    if (keep == nullptr || win == nullptr)
        return;

    auto const doomed = gatherSessionsOfTabsWhere(
        *win, [keep](int, vtworkspace::Tab* tab) { return tab->id() != keep->id(); });
    // Authored while the doomed tabs still name their window, and so the host serving it: the model's close
    // takes them away. What is left to do after it is only the local view's.
    authorRemoteEnds(doomed);
    _model->closeOtherTabs(window, keep->id());
    terminateSessions(doomed, SessionEnd::Detach);
}

void TerminalSessionManager::closeTabsToRight(vtworkspace::WorkspaceId window, int index)
{
    auto* win = _model->workspace(window);
    auto* anchor = tabAtRow(window, index);
    if (anchor == nullptr || win == nullptr)
        return;

    // Tabs strictly to the right of @p index. The gather scan is already row-ordered and hands us the row,
    // so compare it directly instead of re-deriving it with a linear rowOfTab() scan per candidate.
    auto const doomed =
        gatherSessionsOfTabsWhere(*win, [index](int row, vtworkspace::Tab*) { return row > index; });
    // As closeOtherTabs(): ended upstream while the tabs still name their window's host.
    authorRemoteEnds(doomed);
    _model->closeTabsToRight(window, anchor->id());
    terminateSessions(doomed, SessionEnd::Detach);
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
        && factoryOf(*acting).requestRemoteSplit(&acting->terminal().device(), vertical, RemoteSpawn {}))
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
    auto* tab = paneActionTargetTab(acting);
    if (tab == nullptr)
        return nullptr;

    auto& factory = factoryFor(_model->workspaceOfTab(tab->id()));
    if (!factory.canCreateSession())
    {
        managerLog()("Refusing to split: the session factory cannot back a new session right now.");
        return nullptr;
    }

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
    auto* session = createBackingSession(factory,
                                         newSessionId,
                                         paneWorkingDirectory(command, std::move(cwd)),
                                         pageSize,
                                         command,
                                         std::nullopt,
                                         role);
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

std::expected<TerminalSession*, RestartError> TerminalSessionManager::restartSession(TerminalSession& ended)
{
    if (ended.endAction() != FooterAction::Restart)
        return std::unexpected(RestartError::NotOffered);
    return replaceEndedSession(ended);
}

void TerminalSessionManager::reconnectHost(TerminalSession const& acting)
{
    auto const* const window = hostOf(&acting).window;
    auto* const bound = window != nullptr ? mutableBindingOf(window->id()) : nullptr;
    if (bound == nullptr)
        return; // a window no link claims has no connection to seek
    auto& binding = *bound;
    // A failure of an earlier try is not this one's.
    for (auto* session: disconnectedOf(binding))
        session->noteReconnectFailure({});
    auto const started = binding.sessions().reconnect();
    _linkStates[&binding].reconnect = ReconnectRun::Running;
    if (!started)
    {
        managerLog()("Not reconnecting: {}.", describe(started.error()));
        // The connection that just failed reports itself through connectionClosed(), with its own reason:
        // that is the one to show, so the run is left for it to end.
        if (started.error() != ReconnectError::ConnectionEnding)
            reconnectFailed(binding, describe(started.error()));
        return;
    }
    // Ahead of the controller's own report of it, which comes queued: the pill shows as the button is
    // pressed.
    setHostConnection(binding, vthost::client::ConnectionState::Reconnecting);
}

void TerminalSessionManager::rebindDisconnected(WindowBinding& binding)
{
    auto const state = _linkStates.find(&binding);
    if (state == _linkStates.end() || state->second.reconnect == ReconnectRun::Idle)
        return;
    auto& factory = binding.sessions();
    for (auto* session: disconnectedOf(binding))
    {
        if (factory.bindForReconnect(&session->terminal().device()) == ReconnectBinding::Bound)
            if (auto const rebound = replaceEndedSession(*session); !rebound)
            {
                managerLog()("Not taking a pane back: {}.", describe(rebound.error()));
                // Left set, the binding would hand the ended pane's remote session to the next one made.
                factory.cancelReconnectBinding();
            }
    }
    // The daemon pushes one layout per workspace and each arrives in its own turn: the run stays up for the
    // workspaces still to come, and ends once no pane is left to take back (or on reconnectFailed()).
    if (disconnectedOf(binding).empty())
        _linkStates[&binding].reconnect = ReconnectRun::Idle;
}

void TerminalSessionManager::reconnectFailed(WindowBinding const& binding, std::string_view reason)
{
    auto const state = _linkStates.find(&binding);
    if (state == _linkStates.end()
        || std::exchange(state->second.reconnect, ReconnectRun::Idle) == ReconnectRun::Idle)
        return;
    for (auto* session: disconnectedOf(binding))
        session->noteReconnectFailure(std::string { reason });
}

void TerminalSessionManager::connectionClosed(WindowBinding const& binding, std::string_view reason)
{
    reconnectFailed(binding, reason);
    noteTransportLost(binding);
}

std::expected<TerminalSession*, RestartError> TerminalSessionManager::replaceEndedSession(
    TerminalSession& ended)
{
    auto const endedId = ended.modelSessionId();
    auto* tab = sessionForId(endedId) == &ended ? findTabHostingSession(endedId) : nullptr;
    auto const* leaf = tab != nullptr ? tab->rootPane()->findLeaf(endedId) : nullptr;
    if (leaf == nullptr)
        return std::unexpected(RestartError::NotHosted);
    auto const tabId = tab->id();
    auto const leafId = leaf->id();

    // Where its shell said it was (OSC 3008/7, filtered for spawning here), else where it was started.
    auto cwd =
        ended.resolveWorkingDirectory(vtbackend::CwdPurpose::Spawn)
            .transform([](vtbackend::ResolvedWorkingDirectory const& resolved) { return resolved.path; })
            .value_or(ended.workingDirectory());

    // The id is minted here and handed to the model explicitly, so the allocator never consumes the
    // _pendingSessionId createBackingSession stages: clear it, or the next model-made session would be
    // handed this live id a second time.
    auto const sessionId = vtworkspace::SessionId { _nextSessionId++ };
    auto* session = createBackingSession(factoryFor(_model->workspaceOfTab(tabId)),
                                         sessionId,
                                         std::move(cwd),
                                         ended.restartPageSize(),
                                         ended.launchedCommand(),
                                         ended.profileOverride(),
                                         ended.role());
    _pendingSessionId.reset();
    if (session == nullptr)
        return std::unexpected(RestartError::NoSession);

    // Indexed before the model raises paneSessionReplaced, so whatever the view resolves from it finds
    // the new session's tab -- and so the new session's own end later prunes this leaf.
    _tabBySession[sessionId.value] = tabId;
    (void) _model->replaceLeafSession(tabId, leafId, sessionId);

    // The focus follows the pane, not the dead session; dropped first, so no focus-out reaches the
    // ended one's closed device.
    if (_focusedSession == &ended)
    {
        _focusedSession = nullptr;
        setFocusedSession(session);
    }

    // No leaf carries the ended session any more, so removeSession() drops only its bookkeeping and
    // frees it later, leaving the leaf to the new one.
    removeSession(ended);
    // It forgot the ended session's marks but found no tab to repaint, as no leaf carries that session now:
    // the badge is repainted through the new one, which its tab now hosts.
    refreshTabBadgeForSession(sessionId);
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
    auto const& factory = acting != nullptr ? factoryOf(*acting) : _localFactory;
    if (factory.hosting() == SessionHosting::Remote)
        return CommandPaneError::RemoteSessions;

    if (!factory.canCreateSession())
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
    // On Destroy, a daemon-hosted session must be closed upstream too, or it lingers there with no view.
    // Authored before the model pane closes: a tab's only pane takes the tab with it, and with the tab goes
    // the window that names the host to tell (and its pty, which identifies the session there, goes later).
    if (end == SessionEnd::Destroy)
        authorRemoteEnd(sessionForId(sessionId));
    // Close the model pane FIRST, then terminate the backing session. terminate() fires
    // sessionClosed -> removeSession, but by then the leaf is already gone from the model, so
    // removeSession's findLeaf() returns nullptr and it does not close the pane a second time — it
    // only drops the local bookkeeping.
    _model->closePane(_model->workspaceOfTab(tab->id()), tab->id(), active->id());
    if (auto* session = sessionForId(sessionId))
        session->terminate();
}

void TerminalSessionManager::focusPane(vtworkspace::FocusDirection direction, TerminalSession* acting)
{
    if (auto* tab = paneActionTargetTab(acting))
    {
        // Only a move that lands reads as the keyboard's: activePaneChanged() records how the pane it fires
        // for was reached, and a move with no neighbour fires nothing.
        // Cleared on the way out however it is left, or a throw from the model's observers would leave every
        // later activation of this tab keyboard-made.
        _keyboardPaneMove = tab->id();
        auto const clear =
            std::unique_ptr<std::optional<vtworkspace::TabId>, decltype([](auto* move) { move->reset(); })> {
                &_keyboardPaneMove
            };
        _model->focusDirection(tab->id(), direction);
    }
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
    auto* tab = paneActionTargetTab(acting);
    if (tab == nullptr)
        return;
    // A daemon-hosted window leaves the flip to the daemon: a local one would be reverted by its next layout
    // push. The daemon's push then brings the new axis in.
    auto const* const leaf = tab->activePane();
    auto const* const split = leaf != nullptr ? leaf->parent() : nullptr;
    auto const* const anchor = leaf != nullptr ? sessionForId(leaf->session()) : nullptr;
    if (split != nullptr && anchor != nullptr)
    {
        auto const axis = split->splitState() == vtworkspace::SplitState::Horizontal
                              ? vtworkspace::SplitState::Vertical
                              : vtworkspace::SplitState::Horizontal;
        if (factoryFor(_model->workspaceOfTab(tab->id()))
                .requestRemoteSplitOrientation(&anchor->terminal().device(), axis)
            != RemoteRouting::Local)
            return;
    }
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

void TerminalSessionManager::notePaneActivation(vtworkspace::TabId tab, window::PaneActivation activation)
{
    if (lastPaneActivation(tab) == activation)
        return;
    _paneActivations[tab.value] = activation;
    // The active pane may stay where it is (a click on the pane the keyboard reached), so no active-pane
    // change would tell the proxies their focus moved.
    if (auto* c = controllerDisplaying(_model->workspaceOfTab(tab)))
        c->notifyActivePaneChanged();
}

window::PaneActivation TerminalSessionManager::lastPaneActivation(vtworkspace::TabId tab) const noexcept
{
    auto const found = _paneActivations.find(tab.value);
    return found != _paneActivations.end() ? found->second : window::PaneActivation::Pointer;
}

void TerminalSessionManager::runPaneHeaderAction(vtworkspace::TabId tab,
                                                 vtworkspace::PaneId leaf,
                                                 window::PaneHeaderAction action)
{
    if (static_cast<size_t>(action) >= window::PaneHeaderActions.size())
        return;
    auto* const model = _model->findTab(tab);
    auto* const pane = model != nullptr ? model->rootPane()->findPane(leaf) : nullptr;
    auto* const session = pane != nullptr && pane->isLeaf() ? sessionForId(pane->session()) : nullptr;
    if (session == nullptr)
        return;

    // The button acts on its own pane, which a split or a zoom finds as the active one.
    notePaneActivation(tab, window::PaneActivation::Pointer);
    activatePane(tab, leaf);
    auto const& row = window::PaneHeaderActions[static_cast<size_t>(action)];
    if (auto const parsed = actions::fromString(std::string { row.commandName }))
        session->executeAction(*parsed);
}

// }}}

} // namespace contour::session
