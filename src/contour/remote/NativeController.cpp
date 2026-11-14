// SPDX-License-Identifier: Apache-2.0
#include <contour/Logging.hpp>
#include <contour/config/Actions.hpp>
#include <contour/platform/QtInvoke.hpp>
#include <contour/remote/NativeController.hpp>
#include <contour/session/TerminalSessionManager.hpp>

#include <core/Utf8Path.hpp>
#include <core/Utils.hpp>
#include <core/net/Sockets.hpp>

#include <algorithm>
#include <format>
#include <limits>
#include <ranges>
#include <utility>

#include <vthost/HostIdentity.hpp>
#include <vthost/InputEchoWire.hpp>
#include <vthost/SessionSettings.hpp>
#include <vthost/control/ProgramStatusConvert.hpp>

namespace contour::remote
{

using vthost::client::NativeClient;
using vthost::client::RemoteScreen;

namespace
{
    /// @return Whether @p state ends a run: nothing but a new run's Connecting follows it.
    [[nodiscard]] constexpr bool isEnding(vthost::client::ConnectionState state) noexcept
    {
        using vthost::client::ConnectionState;
        return state == ConnectionState::Failed || state == ConnectionState::Refused
               || state == ConnectionState::Stopped;
    }

    /// @param cells The grid a pane reports.
    /// @param pixels The pixels it occupies (cell x grid), when known.
    /// @return The pixel size of one cell, or nothing when either is empty or absent.
    [[nodiscard]] std::optional<vtpty::ImageSize> cellOf(vtpty::PageSize cells,
                                                         std::optional<vtpty::ImageSize> pixels) noexcept
    {
        if (!pixels.has_value() || cells.area() == 0)
            return std::nullopt;
        auto const cell = *pixels / cells;
        return cell.area() > 0 ? std::optional { cell } : std::nullopt;
    }

    /// @param cell A cell's pixel size, when known.
    /// @return Its wire form.
    [[nodiscard]] std::optional<vthost::proto::WireCellPixelSize> toWireCell(
        std::optional<vtpty::ImageSize> cell) noexcept
    {
        return cell.transform([](auto const& c) {
            return vthost::proto::WireCellPixelSize { .width = unbox<uint32_t>(c.width),
                                                      .height = unbox<uint32_t>(c.height) };
        });
    }

    /// The optional features a controller offers in its ClientHello (@see makeClient() for why each).
    /// @param switchRequests Whether Capability::WorkspaceSwitch is among them.
    /// @return The set.
    [[nodiscard]] vthost::proto::CapabilitySet offeredCapabilities(
        NativeController::SwitchRequests switchRequests)
    {
        auto offered = vthost::proto::CapabilitySet::of({
            vthost::proto::Capability::ExtensionPermissions,
            vthost::proto::Capability::ControlPermissions,
            vthost::proto::Capability::InitialClientArea,
            vthost::proto::Capability::AttachIntent,
            vthost::proto::Capability::Heartbeat,
            vthost::proto::Capability::ProfileByName,
            vthost::proto::Capability::TabEdits,
            vthost::proto::Capability::ProgramStatus,
            vthost::proto::Capability::ProgramStatusTiming,
            vthost::proto::Capability::InputKinds,
            vthost::proto::Capability::InputEcho,
            vthost::proto::Capability::TabMerge,
            vthost::proto::Capability::MoveNode,
            vthost::proto::Capability::LayoutBatch,
            vthost::proto::Capability::TabEditAcks,
            vthost::proto::Capability::ColorPreference,
            vthost::proto::Capability::PanesReprojected,
            vthost::proto::Capability::LiveUpgrade,
            vthost::proto::Capability::CellPixelSize,
            vthost::proto::Capability::ClipboardTargets,
            vthost::proto::Capability::SoftFonts,
            vthost::proto::Capability::DaemonFollow,
            vthost::proto::Capability::WorkspaceMeta,
            vthost::proto::Capability::ClientGoodbye,
        });
        if (switchRequests == NativeController::SwitchRequests::Answered)
            offered.idents.push_back(std::to_underlying(vthost::proto::Capability::WorkspaceSwitch));
        return offered;
    }
} // namespace

namespace
{
    /// @param negotiated What a connection agreed on, if it did.
    /// @return Whether the daemon follows the window's selection (Capability::DaemonFollow), which every
    ///         window needs of it.
    [[nodiscard]] bool followsSelection(std::optional<vthost::proto::Negotiated> const& negotiated)
    {
        return negotiated && negotiated->shared.contains(vthost::proto::Capability::DaemonFollow);
    }

    /// @param where The daemon's address, as the transport describes it.
    /// @return What the user is told of a daemon that cannot follow a window.
    [[nodiscard]] std::string predatesMessage(std::string const& where)
    {
        return std::format("the daemon at {} predates this Contour; upgrade it (`contour daemon upgrade`, or "
                           "Upgrade daemon in the palette)",
                           where);
    }
} // namespace

namespace
{
    /// @param preference The preference a client shows.
    /// @return Its wire form.
    [[nodiscard]] constexpr vthost::proto::WireColorPreference wireOf(
        vtbackend::ColorPreference preference) noexcept
    {
        return preference == vtbackend::ColorPreference::Dark ? vthost::proto::WireColorPreference::Dark
                                                              : vthost::proto::WireColorPreference::Light;
    }

    /// @param kind What the terminal says the bytes it wrote carry.
    /// @return Its wire form.
    [[nodiscard]] constexpr vthost::proto::InputKind wireInputKind(vtpty::InputKind kind) noexcept
    {
        switch (kind)
        {
            case vtpty::InputKind::Keyboard: return vthost::proto::InputKind::Keyboard;
            case vtpty::InputKind::Other: break;
        }
        return vthost::proto::InputKind::Other;
    }
} // namespace

NativeController::NativeController(Config config):
    RemoteController(config.clock, DefaultDetachBound),
    // Told when the transport returns its connection, which is when an AttachBudget's handshake bound starts.
    _transport(std::make_shared<vthost::ConnectObservingFactory>(std::move(config.transport),
                                                                 [this] { noteTransportReturned(); })),
    _intent(config.intent),
    _resumeFallback(config.resumeFallback),
    _switchRequests(config.switchRequests),
    _profileName(std::move(config.profileName)),
    _workingDirectory(std::move(config.workingDirectory)),
    _attachSettings(std::move(config.attachSettings)),
    _sessionSettings(std::move(config.sessionSettings)),
    _colorPreference(std::move(config.colorPreference)),
    _timeline(config.timeline),
    _backoff(config.backoff)
{
    // On the monitor's platform thread: only marshaled from there, as its contract asks.
    if (config.powerMonitor != nullptr)
        _powerSubscription = config.powerMonitor->subscribe([this](core::platform::SystemPowerEvent event) {
            // The monitor's dispatch is noexcept, so a throw here would terminate; a hint lost to a failed
            // post costs only the hint: on macOS the late liveness tick still reveals the sleep, and where
            // the steady clock stops during suspend there is no silence to mistake for a loss.
            try
            {
                _reactor.post([this, event] { notePowerEvent(event); });
            }
            catch (...)
            {
                errorLog()("power: dropped a system power event; the hint is lost");
            }
        });
}

void NativeController::notePowerEvent(core::platform::SystemPowerEvent event)
{
    if (_client == nullptr)
    {
        // Reconnecting, or still connecting: there is no connection to probe, but a wake may have brought
        // the network back, so a backoff wait under way is cut short (a first connect is not retried).
        if (event == core::platform::SystemPowerEvent::DidWake && _supervisor != nullptr)
        {
            attachLog()("attach: the system woke; reconnecting now");
            _supervisor->retryNow();
        }
        return;
    }
    switch (event)
    {
        case core::platform::SystemPowerEvent::WillSleep:
            attachLog()("attach: the system is going to sleep");
            _client->noteSystemSleep();
            break;
        case core::platform::SystemPowerEvent::DidWake:
            attachLog()("attach: the system woke");
            _client->noteSystemWake();
            break;
    }
}

void NativeController::postSessionCreation(std::function<void(NativeClient&)> create)
{
    // Resolved here, on the GUI thread, for every creation rather than once at attach: a session the
    // user creates after a configuration reload must get the reloaded profile. NativeClient sends the
    // restatement only when it differs from what the daemon already has.
    auto settings = std::optional<vthost::proto::WireSessionSettings> {};
    if (_sessionSettings)
    {
        if (auto resolved = _sessionSettings(); resolved)
            settings = vthost::toWireSessionSettings(*resolved);
        else
            errorLog()("attach: keeping the session settings last stated: {}", resolved.error());
    }
    _reactor.post([this, settings = std::move(settings), create = std::move(create)] {
        if (_client == nullptr)
            return;
        if (settings)
            _client->restateSessionSettings(settings);
        create(*_client);
    });
}

void NativeController::restateSessionSettings()
{
    if (!_sessionSettings)
        return;
    auto resolved = _sessionSettings();
    if (!resolved)
    {
        errorLog()("attach: keeping the session settings last stated: {}", resolved.error());
        return;
    }
    auto settings = vthost::toWireSessionSettings(*resolved);
    {
        // Read by makeClient() on the reactor, for the hello of every later connection.
        auto const lock = std::lock_guard { _mutex };
        _attachSettings = *std::move(resolved);
    }
    // NativeClient sends the restatement only when it differs from what the daemon already has.
    _reactor.post([this, settings = std::move(settings)] {
        if (_client != nullptr)
            _client->restateSessionSettings(settings);
    });
}

NativeController::~NativeController()
{
    stop();
}

void NativeController::resumeAsLaunch()
{
    auto const lock = std::lock_guard { _mutex };
    _resumeFallback = ResumeFallback::Launch;
}

NativeController::ResumeFallback NativeController::resumeFallback() const
{
    auto const lock = std::lock_guard { _mutex };
    return _resumeFallback;
}

// startConnecting(), awaitConnected() and stop() are provided by RemoteController; this controller supplies
// runClient() and the detach / binding-teardown / message hooks (see NativeController.h).

std::unique_ptr<NativeClient> NativeController::makeClient(core::net::EventLoop& loop,
                                                           std::unique_ptr<vthost::Connection> connection,
                                                           vthost::proto::AttachIntent intent)
{
    if (_acceptedConnections == 0)
        _timeline.mark(vthost::latency::StartupMark::Connected);
    // A reconnect resumes the workspace the supervisor's intent names: the one the lost connection selected.
    // The controller's own record only stands in where that names none.
    if (intent.kind == vthost::proto::AttachIntentKind::Resume)
    {
        auto const lock = std::lock_guard { _mutex };
        if (intent.workspace == 0)
            intent.workspace = _selected;
        // Ids of the run they were shown in: a daemon that restarted since knows they name none of its own.
        intent.daemonInstance = _daemonInstance;
        // Exactly these, or none: no launch rule takes over what other windows of this process resume.
        if (_resumeFallback == ResumeFallback::Nothing)
            intent.kind = vthost::proto::AttachIntentKind::Select;
    }
    // The area the hello states: what the window composes to now, else what was last stated, else the
    // profile's. Reactor thread, no lock held: both are taken here, _mutex first.
    // A first connection composes nothing: no layout came in yet.
    auto helloCell = std::optional<vtpty::ImageSize> {};
    // What the hello states of this client's profile, as restateSessionSettings() last replaced it.
    auto attachSettings = std::optional<vtbackend::Settings> {};
    auto const helloArea = [&]() -> std::optional<vtpty::PageSize> {
        auto const lock = std::lock_guard { _mutex };
        auto const geometryLock = std::lock_guard { _geometryMutex };
        attachSettings = _attachSettings;
        auto const area = composedClientArea()
                              .or_else([this] { return _lastStatedArea; })
                              .or_else([&attachSettings]() -> std::optional<vtpty::PageSize> {
                                  return attachSettings.transform([](auto const& s) { return s.pageSize; });
                              });
        _helloArea = area;
        helloCell = _cellPixelSize;
        _lastReportedCell = _cellPixelSize; // the hello states it
        if (area)
            _lastStatedArea = area;
        return area;
    }();
    return std::make_unique<NativeClient>(
        loop,
        std::move(connection->socket),
        NativeClient::HandshakeOptions {
            .token = connection->token,
            // The daemon applies it to the sessions THIS connection goes on to create, and to nothing
            // that already exists; postSessionCreation() restates it when the configuration changed.
            .sessionSettings =
                attachSettings.transform([](auto const& s) { return vthost::toWireSessionSettings(s); }),
            // ExtensionPermissions is offered because this client passes a PermissionHandler below: its
            // panes ask the user, ControlPermissions because their prompt lists the control permissions a
            // question asks for (vthost::client::consentRequestOf). InitialClientArea because the hello
            // states the area below. AttachIntent so that the daemon names the workspaces a reconnect
            // resumes, Heartbeat so that a connection gone half-open is noticed and replaced. ProfileByName
            // because the hello and every spawning verb name a profile for the daemon to resolve in its own
            // configuration. TabEdits because the user's changes to a tab's presentation are reported
            // (reportTabPresentation). ProgramStatus because the mirrors take the OSC 7501 records the daemon
            // pushes, for the tab strip to show. InputEcho because a pane at a password prompt is locked, and
            // turns secure keyboard entry on, only when the daemon says its terminal hides typing. TabMerge
            // so that a tab dropped on a pane's edge is merged into the split by the daemon
            // (requestRemoteTabMerge) instead of torn off. TabEditAcks so that a pane or tab the user
            // activates stays active over a layout the daemon pushed before it read the change, rather than
            // flipping back until the echo arrives. MoveNode so that a pane or tab dragged in a daemon-hosted
            // window is moved by the daemon (requestRemoteMove), whose layout pushes are then realized here.
            // LayoutBatch so that the daemon says where each push ends, and the GUI reconciles each push
            // whole: a pane a move took out of one workspace is then always seen in the workspace it went to.
            // LiveUpgrade so that a daemon replacing itself in place says so first, and the supervisor waits
            // for that same daemon to come back rather than starting another one into the gap.
            // DaemonFollow so that the window selects one workspace and the daemon routes bells, clipboard
            // writes and switch requests to the window that shows it.
            // WorkspaceSwitch, unless declined, so that `workspace.switch` reaches this client's windows
            // (onSwitchRequest()).
            .capabilities = offeredCapabilities(_switchRequests),
            // The profile's terminal size, so the first session spawns at it rather than at the daemon's
            // default; the ResizeRequest sent once the window is realized corrects any difference.
            .clientArea = helloArea.transform([](auto const& size) {
                return vthost::proto::WireClientArea { .columns = unbox<uint32_t>(size.columns),
                                                       .lines = unbox<uint32_t>(size.lines) };
            }),
            .attachIntent = intent,
            .profileName = _profileName,
            .colorPreference = shownColors(),
            .cellPixelSize = toWireCell(helloCell),
            // The first connection's alone: it is the one whose attach creates the session the directory was
            // asked for. A reconnect resumes the windows it lost and creates nothing of its own.
            .workingDirectory =
                _acceptedConnections == 0 ? core::pathToUtf8(_workingDirectory) : std::string {},
            // What signs in to a daemon that authenticates by key: the transport's, which only it can make.
            .keySource = connection->keySource,
        },
        // What the panes' own stores keep (`command_blocks.max_records`), resolved from the same
        // configuration: a mirror holds no more records than the terminal it feeds.
        attachSettings.transform([](auto const& s) { return s.commandBlockLimits; })
            .value_or(vtbackend::CommandBlockStoreLimits {}),
        NativeClient::UpdateHandler { [this](RemoteScreen const& screen, vthost::proto::Delta const& delta) {
            onUpdate(screen, delta);
        } },
        NativeClient::ImageHandler {
            [this](RemoteScreen const& screen, uint32_t imageId) { onImage(screen, imageId); } },
        NativeClient::SessionEventHandler {
            [this](RemoteScreen const& screen, vthost::proto::SessionEventPdu const& event) {
                onSessionEvent(screen, event);
            } },
        NativeClient::LayoutHandler {
            [this](vthost::proto::LayoutState const& layout) { onLayout(layout); } },
        NativeClient::PermissionHandler {
            [this](vthost::proto::PermissionRequest const& request) { onPermissionRequest(request); } },
        vthost::client::LivenessPolicy {},
        NativeClient::TabMergeRefusedHandler {
            [this](vthost::proto::TabMergeRefused const&) { emit tabMergeRefused(); } },
        NativeClient::MoveNodeRefusedHandler {
            [this](vthost::proto::MoveNodeRefused const& refused, uint64_t serial) {
                onMoveNodeRefused(refused, serial);
            } },
        NativeClient::LayoutBatchEndHandler {
            [this](vthost::proto::LayoutBatchEnd const& end) { onLayoutBatchEnd(end); } },
        NativeClient::PanesReprojectedHandler { [this] { onPanesReprojected(); } },
        NativeClient::SwitchRequestHandler {
            [this](vthost::proto::SwitchRequest const& request) { onSwitchRequest(request); } },
        NativeClient::PaneSummaryHandler {
            [this](vthost::proto::PaneSummary const& summary) { onPaneSummary(summary); } },
        NativeClient::RaiseRoleHandler { [this](vthost::proto::RaiseRole role) { onRaiseRole(role); } },
        NativeClient::WorkspaceMetaHandler {
            [this](vthost::proto::WorkspaceMeta const& meta) { onWorkspaceMeta(meta); } });
}

core::async::Task<void> NativeController::runClient(core::net::EventLoop* loop)
{
    _timeline.mark(vthost::latency::StartupMark::ConnectBegin);
    auto supervisor = vthost::client::ClientSupervisor {
        *loop,
        _transport,
        [this, loop](std::unique_ptr<vthost::Connection> connection, vthost::proto::AttachIntent intent) {
            return makeClient(*loop, std::move(connection), intent);
        },
        _intent,
        _backoff,
        [this](vthost::client::ConnectionState state, std::string const& reason) {
            onConnectionState(state, reason);
        }
    };
    {
        auto const lock = std::lock_guard { _mutex };
        _supervisor = &supervisor;
    }
    // Forgotten on EVERY exit, not only the epilogue's. An exception other than cancellation
    // unwinds straight past that epilogue (the reactor thread catches it), and `supervisor` is a local
    // — the pointers would then name a destroyed object for as long as the controller lives.
    auto const forgetSupervisor = core::Finally { [this] {
        auto const lock = std::lock_guard { _mutex };
        _supervisor = nullptr;
        _client = nullptr;
    } };

    try
    {
        co_await supervisor.run();
    }
    catch (core::async::OperationCancelled const&)
    {
        // stop() cancelled the loop mid-serve; fall through to the normal
        // bookkeeping so state and observers still see the closure.
        attachLog()("Attach serve loop cancelled by stop().");
    }

    // A cancelled loop ends the supervisor without its final state: settle it here.
    auto settled = false;
    {
        auto const lock = std::lock_guard { _mutex };
        settled = isEnding(_connection);
    }
    if (!settled)
        onConnectionState(vthost::client::ConnectionState::Stopped, {});
}

void NativeController::onConnectionState(vthost::client::ConnectionState state, std::string const& reason)
{
    using vthost::client::ConnectionState;
    auto const where = _transport->describe();
    auto closed = false;
    auto missingWorkspaces = false;
    auto predatesFollow = false;
    auto lostAtOnce = std::vector<uint64_t> {};
    auto restored = std::optional<vthost::RestoredCounts> {};
    {
        auto const lock = std::lock_guard { _mutex };
        // The first ending of a run is its ending, and only reconnect() starts the next run. A reactor
        // that died reports Failed at once -- possibly before its supervisor has even started -- and that
        // supervisor's Connecting and its cancelled Stopped must neither revive the run nor close the
        // connection a second time.
        if (isEnding(_connection))
            return;
        _connection = state;
        // A prompt of this attach waits for the user to be looking: that is what the connection says, until
        // it is shown.
        if (_userAttention == UserAttention::Needed
            && (state == ConnectionState::Reconnecting || state == ConnectionState::Connecting))
        {
            _stateUnderAttention = state;
            _connection = ConnectionState::NeedsAttention;
        }
        switch (state)
        {
            case ConnectionState::Connecting: break;
            case ConnectionState::NeedsAttention: break; // set above, by the controller itself
            case ConnectionState::Connected:
                _client = _supervisor != nullptr ? _supervisor->current() : nullptr;
                // Each connection's own: a reconnect can reach an upgraded or downgraded daemon.
                _negotiated.reset();
                captureHandshake();
                if (_client != nullptr)
                    restored = _client->restoredCounts();
                if (_acceptedConnections != 0)
                {
                    adoptReconnect(lock);
                    if (_client != nullptr && _resumeFallback == ResumeFallback::Nothing)
                        _resumedAwaitingLayout = _client->selectedWorkspace() != 0
                                                     ? std::set { _client->selectedWorkspace() }
                                                     : std::set<uint64_t> {};
                    // A reconnect that selects nothing gets no layout to announce its losses with: they are
                    // told now, as no window comes instead.
                    if (_client != nullptr && _client->selectedWorkspace() == 0 && _client->negotiated()
                        && _client->negotiated()->shared.contains(vthost::proto::Capability::AttachIntent))
                    {
                        lostAtOnce = std::exchange(_lostWorkspaces, {});
                        // Every window the GUI shows is lost: closed, it shows nothing of any run.
                        _shownWorkspaces = { .ids = {}, .daemonInstance = _daemonInstance };
                    }
                }
                else if (_client != nullptr)
                {
                    _daemonInstance = _client->daemonInstance();
                    // Asked for workspaces the daemon does not have, a first attach would wait out its budget
                    // for a session that never comes: it ends now, saying which.
                    missingWorkspaces = _intent.kind == vthost::proto::AttachIntentKind::Select
                                        && _client->selectedWorkspace() == 0;
                }
                if (_client != nullptr)
                    _selected = _client->selectedWorkspace();
                // A daemon that cannot follow a window's selection cannot serve a window of this version.
                predatesFollow = _client != nullptr && !followsSelection(_client->negotiated());
                // The new connection knows nothing of the focus the lost one was told.
                if (_client != nullptr && _focus == vthost::proto::WindowFocus::Focused)
                    std::ignore = _client->reportFocus(_focus);
                ++_acceptedConnections;
                _inputConnection = _acceptedConnections;
                attachLog()("Attached to {}.", where);
                break;
            case ConnectionState::Reconnecting:
            case ConnectionState::Upgrading:
                // An upgrade is a reconnect to the same daemon: adoptReconnect() keeps every binding then.
                _client = nullptr;
                _inputConnection = 0;
                forgetInputEchoes();
                // Before the first session arrived the attach still waits; awaitConnected() sees it through.
                if (_state == State::Ready)
                    _state = State::Reconnecting;
                attachLog()("Lost the connection to {} ({}); {}.",
                            where,
                            reason,
                            state == ConnectionState::Upgrading ? "waiting for it to come back"
                                                                : "reconnecting");
                break;
            case ConnectionState::Failed:
            case ConnectionState::Refused:
            case ConnectionState::Stopped:
                _client = nullptr;
                _inputConnection = 0;
                forgetInputEchoes();
                if (state == ConnectionState::Stopped)
                {
                    if (_state == State::Connecting || _state == State::Ready
                        || _state == State::Reconnecting)
                        _state = State::Closed;
                }
                else
                {
                    _state = State::Failed;
                    // Named once: an ssh transport's message already says which host it failed on.
                    _failure = reason.contains(where) ? reason : std::format("{} ({})", reason, where);
                    errorLog()("attach: {}", _failure);
                }
                closed = true;
                break;
        }
    }
    _connected.notify_all();
    emit attachProgressed();
    if (missingWorkspaces)
        failAttach(std::format("the daemon has no workspace {}", _intent.workspace));
    else if (predatesFollow)
        failAttach(predatesMessage(where));
    // After the lock is released, and ahead of the layout that follows the hello, so the GUI holds the
    // summary by the time it realizes the workspaces it is for.
    if (restored)
        emit workspaceRestored(restored->workspaces, restored->tabs);
    if (!lostAtOnce.empty())
        emit workspacesLost(std::move(lostAtOnce));
    emit connectionStateChanged();
    if (closed)
        emit connectionClosed();
}

void NativeController::reactorDied(std::string const& reason)
{
    using vthost::client::ConnectionState;
    auto ended = false;
    {
        auto const lock = std::lock_guard { _mutex };
        ended = isEnding(_connection);
    }
    // A connection that already ended told its owner so.
    if (ended)
        failConnect(reason);
    else
        onConnectionState(ConnectionState::Failed, reason);
}

void NativeController::forgetInputEchoes()
{
    // With the connection down, typed input is dropped and nothing reads the program's reply: a stale Hidden
    // would keep the lock, and the OS's secure input, on for a prompt nobody is answering. The reconnect's
    // snapshot states each pane's echo again.
    _inputEchoes.clear();
    for (auto const& binding: _bindings | std::views::values)
        if (binding.pty != nullptr)
            binding.pty->setInputEcho(vtpty::InputEcho::Unknown);
}

void NativeController::adoptReconnect(std::lock_guard<std::mutex> const& /*held*/)
{
    auto* client = _client;
    if (client == nullptr)
        return;
    // Workspace and session ids count from 1 in every run of a daemon: once it restarted, the ids this client
    // holds name nothing of the new run, however many of them it reuses.
    auto const restartedSince = [&](uint64_t instance) {
        return instance != 0 && client->daemonInstance() != 0 && instance != client->daemonInstance();
    };
    // What the GUI shows: what this connection settled, releases and all; or, had it settled nothing, still
    // what the one before it did.
    if (_connectionLayouts == ConnectionLayouts::Settled)
        _shownWorkspaces = { .ids = std::ranges::to<std::vector>(_settledLayouts | std::views::keys),
                             .daemonInstance = _daemonInstance };
    auto const restarted = restartedSince(_daemonInstance);
    auto const sameInstance = _daemonInstance != 0 && _daemonInstance == client->daemonInstance();
    _daemonInstance = client->daemonInstance();
    // Lost: what the GUI's windows show and this connection does not select. Computed against the windows,
    // not against the last connection's layouts: one lost before its push settled showed the GUI nothing, and
    // its frames may name another run's workspaces than the windows do (its own, after a restart).
    auto const& shown = _shownWorkspaces;
    if (restartedSince(shown.daemonInstance))
        _lostWorkspaces = shown.ids;
    // The resume fell back to another workspace: the one the window selected is gone. A daemon that predates
    // attach intents names no workspace, and nothing is lost.
    else if (_selected != 0 && client->selectedWorkspace() != _selected && client->negotiated()
             && client->negotiated()->shared.contains(vthost::proto::Capability::AttachIntent))
        _lostWorkspaces = { _selected };
    else
        _lostWorkspaces.clear();
    _connectionLayouts = ConnectionLayouts::Awaited;
    if (restarted)
    {
        // The new run's workspace ids name other workspaces than the one selected.
        _selected = 0;
        // Every binding is detached, its pty left to the window closing it: a new session with the same id
        // must not reach the old pane, nor the old pane's input the new session.
        _bindings.clear();
        // The new run's sessions count from 1 again: an old Hidden is no pane's of this run.
        _inputEchoes.clear();
        // The new run's sessions count from 1 again: an old status is no session's of this run.
        _paneStatus.clear();
        _closedSessions.clear();
        // The new run's tab ids count from 1 again: an old pairing would name another tab of this run.
        _wireTabOf.clear();
        _pending.clear();
        // Closes of the old run's sessions: the ids name the new run's, if anything.
        _pendingCloses.clear();
        _unsentCloses.clear();
    }
    else
    {
        // The same sessions, re-snapshotted: each mirror takes the new connection's first snapshot as a
        // first.
        for (auto& binding: _bindings | std::views::values)
            if (binding.mirror)
                binding.mirror->restart();
        // The panes the user closed meanwhile are still tombstoned, so the layout this connection pushes
        // does not bring them back; the daemon is told now, and its next push drops them.
        for (auto const session: std::exchange(_pendingCloses, {}))
            std::ignore = client->closePane(session);
        // A legacy daemon (instance 0) may be a restarted one whose ids were reused, so the closes it never
        // got are dropped as a restarted daemon's are; only a daemon that names its run is trusted with them.
        if (!sameInstance)
            _unsentCloses.clear();
    }
    // Replaced by what the new connection pushes; what was reported against them was told another daemon,
    // or a connection that may not have delivered it.
    _layouts.clear();
    _settledLayouts.clear();
    // The new connection states every workspace's metadata again, in full.
    _workspaceMeta.clear();
    // A move asked of the lost connection is answered on it or not at all.
    _moveRequests.clear();
    _landedMoves.clear();
    _selectRequests.clear();
    _selectAnswers.clear();
    _reportedPresentations.clear();
    {
        auto const geometryLock = std::lock_guard { _geometryMutex };
        _lastReportedArea.reset();
        _lastReportedPaneSizes.clear();
        // The daemon projected every pane onto the hello's area: they are re-asserted once the GUI
        // reconciled.
        _geometryReassert = GeometryReassert::Pending;
        if (restarted)
        {
            _paneSizes.clear();
            _paneSerials.clear();
            _geometryAnchor.reset();
        }
    }
    // What the panes show, stated again to the daemon for every session mirrored: a change made while down
    // was refused, and the host ignores a preference it already has.
    for (auto const session: _bindings | std::views::keys)
        stateColorPreference(*client, session);
    // Up again. One lost before its first session arrived is still being waited for, as a first attach is.
    if (_state == State::Reconnecting)
        _state = State::Ready;
    if (auto const dropped = _droppedInputBytes.exchange(0); dropped != 0)
        attachLog()("Dropped {} bytes of input typed while the connection to {} was down.",
                    dropped,
                    _transport->describe());
    if (!_lostWorkspaces.empty())
        attachLog()("{} workspace(s) of {} are gone after the reconnect.",
                    _lostWorkspaces.size(),
                    _transport->describe());
}

vthost::client::ConnectionState NativeController::connectionState() const
{
    auto const lock = std::lock_guard { _mutex };
    return _connection;
}

void NativeController::setUserAttention(UserAttention attention)
{
    using vthost::client::ConnectionState;
    {
        auto const lock = std::lock_guard { _mutex };
        _userAttention = attention;
        if (attention == UserAttention::Needed
            && (_connection == ConnectionState::Reconnecting || _connection == ConnectionState::Connecting))
        {
            _stateUnderAttention = _connection;
            _connection = ConnectionState::NeedsAttention;
        }
        else if (attention == UserAttention::NotNeeded && _connection == ConnectionState::NeedsAttention)
            _connection = _stateUnderAttention;
        else
            return;
    }
    emit connectionStateChanged();
}

void NativeController::captureHandshake()
{
    if (_negotiated || _client == nullptr || !_client->negotiated())
        return;
    _negotiated = _client->negotiated();
    _daemonRelease = _client->daemonRelease();
}

std::string NativeController::daemonRelease() const
{
    auto const lock = std::lock_guard { _mutex };
    return _daemonRelease;
}

std::optional<vthost::proto::Negotiated> NativeController::negotiated() const
{
    auto const lock = std::lock_guard { _mutex };
    return _negotiated;
}

void NativeController::onUpdate(RemoteScreen const& screen, vthost::proto::Delta const& delta)
{
    auto lock = std::unique_lock { _mutex };
    captureHandshake();
    // A mirrored pane says its title and directory in its own stream, not in summaries.
    if (auto& status = _paneStatus[screen.session]; status.title != screen.title || status.cwd != screen.cwd)
    {
        status.title = screen.title;
        status.cwd = screen.cwd;
        scheduleStatusFlush();
    }

    // Ordered by precedence: a live binding always consumes the delta, a
    // tombstoned session is ignored, a pending one refreshes its geometry, and
    // only a never-seen session is adopted as a new tab.
    if (auto const binding = _bindings.find(screen.session); binding != _bindings.end())
    {
        // The mirror is absent only in the window between createPty() and the priming bindTerminal()
        // posts; that replay picks the session up from whatever arrived meanwhile.
        if (binding->second.mirror)
        {
            binding->second.mirror->apply(screen, delta);
            tellInterruption(binding->second);
        }
        return;
    }
    if (_closedSessions.contains(screen.session))
    {
        // The user closed this session's tab; the remote session lives on,
        // but its deltas must not resurrect a local tab.
        return;
    }
    if (auto const pending = std::ranges::find(_pending, screen.session, &PendingSession::session);
        pending != _pending.end())
    {
        pending->columns = screen.columns;
        pending->lines = screen.lines;
        return;
    }

    _timeline.mark(vthost::latency::StartupMark::FirstSessionDiscovered);
    _pending.push_back(
        PendingSession { .session = screen.session, .columns = screen.columns, .lines = screen.lines });
    if (_state == State::Connecting)
        _state = State::Ready;
    lock.unlock();

    _connected.notify_all();
    emit attachProgressed();
    emit remoteSessionDiscovered();
}

void NativeController::onLayout(vthost::proto::LayoutState const& layout)
{
    _timeline.mark(vthost::latency::StartupMark::LayoutReceived);
    auto lost = std::vector<uint64_t> {};
    auto lostEmptied = false;
    auto batched = false;
    {
        auto const lock = std::lock_guard { _mutex };
        captureHandshake();
        // A daemon that ends its pushes settles them itself (onLayoutBatchEnd); one that does not settles
        // each frame as it comes, as before there were batches.
        batched = layoutBatchShared();
        if (_connectionLayouts == ConnectionLayouts::Awaited)
            _connectionLayouts = ConnectionLayouts::Received;
        // Resumed exactly, the workspace is followed even when its last session ended meanwhile: it is lost.
        if (_resumedAwaitingLayout.erase(layout.workspace) != 0 && layout.tabs.empty())
        {
            _lostWorkspaces.push_back(layout.workspace);
            _layouts.erase(layout.workspace);
            _settledLayouts.erase(layout.workspace);
            lostEmptied = true;
        }
        if (!lostEmptied)
        {
            auto held = std::make_shared<vthost::proto::LayoutState const>(layout);
            if (!batched)
                _settledLayouts[layout.workspace] = held;
            _layouts[layout.workspace] = std::move(held);
            // The daemon's word on these tabs is newer than what was reported for them.
            for (auto const& tab: layout.tabs)
                _reportedPresentations.erase(tab.tabId);
        }
        // Announced once the workspaces that came instead are settled, so the GUI has a window to show
        // before it closes the lost ones -- closing the last one first would end the application. A batched
        // push settles at its end, so its losses are announced there: announced with its first frame, the
        // GUI could reconcile before the push ended, find no settled layout to open a window for, and close
        // the last window of the link.
        if (!batched)
            lost = settle(lock);
        // A layout push follows a daemon-side re-projection, which discards the per-pane grids this
        // client reported (@see vthost::SessionHost::applyPaneSize). Forget what was sent, so the
        // next report re-asserts all of it instead of being filtered out as "unchanged" — otherwise
        // a pane the daemon resized underneath us (a tab whose divider the user dragged, re-projected
        // by a split in a DIFFERENT tab) would keep the daemon's ratio-derived estimate.
        //
        // Only the bookkeeping is dropped, never a report issued: at this point the panes have not
        // been laid out against the new tree, so composing an area would mix a stale pane extent
        // with a fresh one and propose one that neither describes. Panes another client's departure moves,
        // under a geometry of this client's own that does not, the daemon restores to this client's
        // remembered sizes itself.
        auto const geometryLock = std::lock_guard { _geometryMutex };
        _lastReportedArea.reset();
        _lastReportedPaneSizes.clear();
    }
    replayUnsentCloses(layout);
    if (!lost.empty())
        emit workspacesLost(std::move(lost));
    // The GUI (on its own thread, via a queued connection) reconciles each daemon
    // workspace's tab/split tree against wireLayout(workspace) — the authoritative
    // structure — mapping one OS window to each (B4). (The per-session snapshot
    // deltas that follow set State::Ready via onUpdate.) A push the daemon ends is reconciled at its end
    // only: until then the layouts held mix this push's workspaces with the last one's.
    if (!batched)
    {
        emit layoutChanged();
        emit directoryChanged();
    }
}

void NativeController::onMoveNodeRefused(vthost::proto::MoveNodeRefused const& refused, uint64_t serial)
{
    auto request = uint64_t { 0 };
    if (auto const found = _moveRequests.find(serial); found != _moveRequests.end())
    {
        request = found->second;
        _moveRequests.erase(found);
    }
    emit moveRefused(refused.reason, refused.modelReason, request);
}

std::vector<uint64_t> NativeController::takeLandedMoves()
{
    auto const lock = std::lock_guard { _mutex };
    return std::exchange(_landedMoves, {});
}

std::vector<SelectAnswer> NativeController::takeSelectAnswers()
{
    auto const lock = std::lock_guard { _mutex };
    return std::exchange(_selectAnswers, {});
}

std::vector<vthost::proto::PaneSummary> NativeController::takeRaised()
{
    auto const lock = std::lock_guard { _mutex };
    return std::exchange(_raised, {});
}

void NativeController::onPanesReprojected()
{
    // A daemon built while it asked on every departure of another client wants this client's pane sizes
    // again; today's daemon restores them itself and never asks. Nothing of this client's moved, so nothing
    // would report the panes again. Pending until a flush that composes an area spends it, should this one
    // find the panes not laid out yet.
    {
        auto const geometryLock = std::lock_guard { _geometryMutex };
        _geometryReassert = GeometryReassert::Pending;
    }
    platform::postToObject(this, [this] { flushGeometry(GeometryFlush::PanesOnly); });
}

void NativeController::onLayoutBatchEnd(vthost::proto::LayoutBatchEnd const& end)
{
    // The moves whose effects this push completes, if this client asked for any: named with the layouts it
    // settles, so a reconcile that takes them reads them.
    auto const landed = takeCompletedRequests(_moveRequests, end.causeSerial);
    auto const answered = takeCompletedRequests(_selectRequests, end.causeSerial);
    auto lost = std::vector<uint64_t> {};
    {
        auto const lock = std::lock_guard { _mutex };
        _landedMoves.insert(_landedMoves.end(), landed.begin(), landed.end());
        // The client holds the selection the daemon stated with this batch.
        if (_client != nullptr)
            _selected = _client->selectedWorkspace();
        for (auto const& [request, asked]: answered)
            _selectAnswers.push_back(
                SelectAnswer { .request = request, .asked = asked, .selected = _selected });
        _settledLayouts = _layouts; // pointers: the layouts themselves are shared, never changed
        lost = settle(lock);
        pruneStatus();
        clearSelectedActivity();
    }
    if (!lost.empty())
        emit workspacesLost(std::move(lost));
    emit layoutChanged();
    emit directoryChanged();
}

bool NativeController::layoutBatchShared() const
{
    return _negotiated && _negotiated->shared.contains(vthost::proto::Capability::LayoutBatch);
}

std::vector<uint64_t> NativeController::settle(std::lock_guard<std::mutex> const& /*held*/)
{
    // A push that ended with no frame in it -- one a move request alone caused, or an empty one -- settles
    // nothing of this connection: the windows still show what the last one that did settled, and the losses
    // wait for a window that comes instead. Announced now, the GUI would close them with none to show.
    if (_connectionLayouts == ConnectionLayouts::Awaited)
        return {};
    _connectionLayouts = ConnectionLayouts::Settled;
    // Settled now: the workspaces that came instead have their layouts for the GUI to open windows for.
    return std::exchange(_lostWorkspaces, {});
}

LayoutSnapshot NativeController::settledLayouts() const
{
    auto const lock = std::lock_guard { _mutex };
    return _settledLayouts;
}

std::shared_ptr<vthost::proto::LayoutState const> NativeController::settledLayout(
    uint64_t daemonWorkspace) const
{
    auto const lock = std::lock_guard { _mutex };
    auto const it = _settledLayouts.find(daemonWorkspace);
    return it != _settledLayouts.end() ? it->second : nullptr;
}

std::shared_ptr<vthost::proto::LayoutState const> NativeController::settledLayout() const
{
    auto const lock = std::lock_guard { _mutex };
    return _settledLayouts.empty() ? nullptr : _settledLayouts.begin()->second;
}

std::vector<uint64_t> NativeController::workspaceIds() const
{
    auto const lock = std::lock_guard { _mutex };
    // std::map keeps the keys ascending, so the primary (lowest-id) workspace comes first.
    return std::ranges::to<std::vector>(_layouts | std::views::keys);
}

std::optional<vthost::proto::LayoutState> NativeController::layout(uint64_t daemonWorkspace) const
{
    auto const lock = std::lock_guard { _mutex };
    auto const it = _layouts.find(daemonWorkspace);
    return it != _layouts.end() ? std::optional { *it->second } : std::nullopt;
}

std::optional<vthost::proto::LayoutState> NativeController::layout() const
{
    auto const lock = std::lock_guard { _mutex };
    if (_layouts.empty())
        return std::nullopt;
    return *_layouts.begin()->second; // the primary (lowest-id) workspace
}

vthost::client::WireLayout NativeController::wireLayout() const
{
    auto const lock = std::lock_guard { _mutex };
    return _layouts.empty() ? vthost::client::WireLayout {}
                            : vthost::client::wireToLayout(*_layouts.begin()->second);
}

std::expected<void, contour::session::ReconnectError> NativeController::reconnect()
{
    using contour::session::ReconnectError;
    if (!_connectStartedAt)
        return std::unexpected(ReconnectError::NotAttached);
    // Only once stop() has run: a run that failed is Failed before the GUI handles its connectionClosed, and
    // a new run started in that gap would be stopped by the old run's handler.
    if (!isStopped())
        return std::unexpected(isConnecting() ? ReconnectError::AlreadyConnected
                                              : ReconnectError::ConnectionEnding);
    {
        // The workspace this client selects, as a reconnect inside one run resumes it.
        auto const lock = std::lock_guard { _mutex };
        // The new run is Connecting from here on: a reactor that dies before it enters Connecting itself
        // must not find the previous run's Failed and swallow its own death (@see reactorDied).
        _connection = vthost::client::ConnectionState::Connecting;
        _intent = vthost::proto::AttachIntent {
            .kind = vthost::proto::AttachIntentKind::Resume,
            .workspace = _selected,
        };
    }
    restartConnecting();
    return {};
}

std::expected<uint64_t, std::string> NativeController::select(uint64_t workspace,
                                                              std::optional<uint64_t> directoryFrom)
{
    auto request = uint64_t { 0 };
    {
        auto const lock = std::lock_guard { _mutex };
        if (_state != State::Ready || _client == nullptr)
            return std::unexpected(std::string { "the connection to the daemon is not up" });
        if (!followsSelection(_negotiated))
            return std::unexpected(predatesMessage(_transport->describe()));
        request = _nextSelectRequest++;
    }
    // The selection is the one the daemon states with the batch that answers: a workspace it does not have
    // leaves it as it was.
    _reactor.post([this, workspace, request, directoryFrom] {
        if (_client == nullptr)
            return;
        // Answered by the batch naming it as its cause (onLayoutBatchEnd).
        if (auto const serial = _client->selectWorkspace(workspace, directoryFrom))
            _selectRequests.insert_or_assign(*serial,
                                             SelectRequest { .request = request, .asked = workspace });
    });
    return request;
}

void NativeController::reportFocus(vthost::proto::WindowFocus focus)
{
    {
        auto const lock = std::lock_guard { _mutex };
        _focus = focus;
    }
    _reactor.post([this, focus] {
        if (_client != nullptr)
            std::ignore = _client->reportFocus(focus);
    });
}

vthost::proto::RaiseRole NativeController::raiseRole() const noexcept
{
    return _guiRaiseRole;
}

uint64_t NativeController::selectedWorkspace() const noexcept
{
    auto const lock = std::lock_guard { _mutex };
    return _selected;
}

namespace
{
    /// A directory's view of the status store.
    class StoreLookup final: public window::PaneStatusLookup
    {
      public:
        explicit StoreLookup(std::map<uint64_t, window::PaneStatus> const& store): _store(store) {}

        [[nodiscard]] std::optional<window::PaneStatus> statusOf(window::PaneKey pane) const override
        {
            auto const found = _store.find(pane.value);
            return found != _store.end() ? std::optional { found->second } : std::nullopt;
        }

      private:
        std::map<uint64_t, window::PaneStatus> const& _store;
    };

    /// A directory's view of what the daemon stated of each workspace.
    class MetaLookup final: public window::WorkspaceMetaLookup
    {
      public:
        explicit MetaLookup(std::map<uint64_t, window::WorkspaceMetadata> const& store): _store(store) {}

        [[nodiscard]] window::WorkspaceMetadata metaOf(uint64_t workspace) const override
        {
            auto const found = _store.find(workspace);
            return found != _store.end() ? found->second : window::WorkspaceMetadata {};
        }

      private:
        std::map<uint64_t, window::WorkspaceMetadata> const& _store;
    };

    /// @return @p git as a directory holds it; none for a workspace in no repository.
    [[nodiscard]] std::optional<window::DirectoryGit> directoryGitOf(vthost::proto::WireGitInfo const& git)
    {
        using vthost::proto::GitHeadKindWire;
        auto const kind = [&]() -> std::optional<vtbackend::GitHeadKind> {
            switch (git.kind)
            {
                case GitHeadKindWire::None: return std::nullopt;
                case GitHeadKindWire::Branch: return vtbackend::GitHeadKind::Branch;
                case GitHeadKindWire::Detached: return vtbackend::GitHeadKind::Detached;
                case GitHeadKindWire::OtherRef: break;
            }
            return vtbackend::GitHeadKind::OtherRef;
        }();
        return kind.transform([&](vtbackend::GitHeadKind headKind) {
            return window::DirectoryGit {
                .branch = git.branch, .kind = headKind, .ahead = git.ahead, .behind = git.behind
            };
        });
    }
} // namespace

window::WorkspaceDirectory NativeController::directory() const
{
    auto const lock = std::lock_guard { _mutex };
    // std::map keeps the keys ascending: the daemon's workspaces in the order it numbered them. The place the
    // daemon states for each workspace (it moves them) goes first; the stable sort keeps id order for a
    // daemon that states none, and for a workspace whose position has not arrived yet.
    // A layout that holds no tab is not listed: an emptied layout is how the daemon says a workspace is gone,
    // and the one tabless workspace it keeps (its only one, once its last session ended) has nothing to show.
    auto layouts = std::ranges::to<std::vector>(
        _settledLayouts | std::views::values
        | std::views::filter([](auto const& layout) { return !layout->tabs.empty(); })
        | std::views::transform([](auto const& layout) { return *layout; }));
    auto const positionOf = [this](vthost::proto::LayoutState const& layout) {
        auto const found = _workspaceMeta.find(layout.workspace);
        return found != _workspaceMeta.end() && found->second.position ? *found->second.position
                                                                       : std::numeric_limits<uint32_t>::max();
    };
    std::ranges::stable_sort(layouts,
                             [&](auto const& a, auto const& b) { return positionOf(a) < positionOf(b); });
    return window::directoryFromLayouts(layouts,
                                        StoreLookup { _paneStatus },
                                        MetaLookup { _workspaceMeta },
                                        _selected != 0 ? std::optional { _selected } : std::nullopt);
}

void NativeController::onPaneSummary(vthost::proto::PaneSummary const& summary)
{
    auto const lock = std::lock_guard { _mutex };
    auto& status = _paneStatus[summary.session];
    // Field by field: an absent one is unchanged, and the activity and the bell stay marked until the pane is
    // shown.
    if (summary.title)
        status.title = *summary.title;
    if (summary.cwd)
        status.cwd = *summary.cwd;
    // Each occurrence takes the next serial, so a window that acknowledged the old one still sees the new.
    if (summary.activity == vthost::proto::OutputActivity::Output)
    {
        status.activity = vthost::proto::OutputActivity::Output;
        status.activitySerial = ++_flagSerial;
    }
    if (summary.bell == vthost::proto::PaneBell::Rang)
    {
        status.bell = vthost::proto::PaneBell::Rang;
        status.bellSerial = ++_flagSerial;
    }
    if (summary.finished)
    {
        status.finished = *summary.finished;
        status.finishedSerial = ++_flagSerial;
    }
    // What it raised is judged by the role as it stands now, in the order the daemon decided both: a focus
    // change on its way to the GUI thread cannot leave the raise to no window.
    if (vthost::proto::carriesRaise(summary) && _raiseRole == vthost::proto::RaiseRole::Raise)
        _raised.push_back(vthost::proto::PaneSummary { .session = summary.session,
                                                       .finished = summary.finished,
                                                       .bell = summary.bell,
                                                       .notifications = summary.notifications });
    scheduleStatusFlush();
}

void NativeController::onWorkspaceMeta(vthost::proto::WorkspaceMeta const& meta)
{
    auto const lock = std::lock_guard { _mutex };
    auto& stated = _workspaceMeta[meta.workspace];
    // Field by field: an absent one is unchanged, an empty label clears it, and a git state of kind None says
    // the workspace is in no repository anymore.
    stated.controlId = meta.controlId;
    if (meta.label)
        stated.label = *meta.label;
    if (meta.git)
        stated.git = directoryGitOf(*meta.git);
    if (meta.position)
        stated.position = meta.position;
    scheduleStatusFlush();
}

void NativeController::onRaiseRole(vthost::proto::RaiseRole role)
{
    {
        auto const lock = std::lock_guard { _mutex };
        _raiseRole = role;
    }
    // The GUI thread's copy changes in line with the events: a bell or notification this connection was sent
    // before the change was posted to the GUI thread before this is, so it is judged by the role it arrived
    // under there too (raiseRole()), as a summary is (takeRaised()).
    platform::postToObject(this, [this, role] { _guiRaiseRole = role; });
}

void NativeController::failAttach(std::string reason)
{
    failConnect(std::move(reason));
    auto const lock = std::lock_guard { _mutex };
    if (_supervisor != nullptr)
        _supervisor->stop();
}

void NativeController::clearSelectedActivity()
{
    auto const selected = _layouts.find(_selected);
    if (selected == _layouts.end())
        return;
    for (auto const& tab: selected->second->tabs)
        window::forEachLeaf(tab.root, [this](vthost::proto::WirePane const& leaf) {
            if (auto const found = _paneStatus.find(leaf.session); found != _paneStatus.end())
            {
                found->second.activity = vthost::proto::OutputActivity::None;
                found->second.bell = vthost::proto::PaneBell::Silent;
            }
        });
}

void NativeController::scheduleStatusFlush()
{
    if (std::exchange(_statusFlush, StatusFlush::Queued) == StatusFlush::Queued)
        return;
    platform::postToObject(this, [this] { flushStatus(); });
}

void NativeController::flushStatus()
{
    {
        auto const lock = std::lock_guard { _mutex };
        _statusFlush = StatusFlush::Idle;
    }
    emit directoryChanged();
}

void NativeController::pruneStatus()
{
    auto named = std::set<uint64_t> {};
    for (auto const& layout: _layouts | std::views::values)
        for (auto const& tab: layout->tabs)
            window::forEachLeaf(
                tab.root, [&named](vthost::proto::WirePane const& leaf) { named.insert(leaf.session); });
    std::erase_if(_paneStatus, [&named](auto const& entry) { return !named.contains(entry.first); });
    // A workspace's metadata goes with its layout, which comes first: one whose layout is gone, or emptied as
    // the daemon removes it, is gone.
    std::erase_if(_workspaceMeta, [this](auto const& entry) {
        auto const layout = _layouts.find(entry.first);
        return layout == _layouts.end() || layout->second->tabs.empty();
    });
}

namespace
{
    /// @param pane A node of a layout.
    /// @param session A daemon session id.
    /// @return Whether a leaf at or under @p pane carries @p session.
    [[nodiscard]] bool paneNames(vthost::proto::WirePane const& pane, uint64_t session)
    {
        if (!pane.isSplit())
            return pane.session == session;
        return std::ranges::any_of(pane.children,
                                   [session](auto const& child) { return paneNames(child, session); });
    }
} // namespace

contour::session::ReconnectBinding NativeController::bindForReconnect(vtpty::Pty const* endedPty)
{
    using contour::session::ReconnectBinding;
    auto const session = sessionForPty(endedPty);
    if (!session)
        return ReconnectBinding::Gone;
    auto const lock = std::lock_guard { _mutex };
    auto const named = std::ranges::any_of(_layouts | std::views::values, [&](auto const& layout) {
        return std::ranges::any_of(layout->tabs,
                                   [&](auto const& tab) { return paneNames(tab.root, *session); });
    });
    if (!named)
        return ReconnectBinding::Gone;
    _nextBindSession = session;
    return ReconnectBinding::Bound;
}

void NativeController::cancelReconnectBinding()
{
    auto const lock = std::lock_guard { _mutex };
    _nextBindSession.reset();
}

void NativeController::setNextBindSession(uint64_t session)
{
    auto const lock = std::lock_guard { _mutex };
    _nextBindSession = session;
}

void NativeController::setRealizingLayout(bool realizing)
{
    auto const lock = std::lock_guard { _mutex };
    _realizingLayout = realizing;
}

bool NativeController::isRealizingLayout() const
{
    auto const lock = std::lock_guard { _mutex };
    return _realizingLayout;
}

bool NativeController::isBound(uint64_t session) const
{
    auto const lock = std::lock_guard { _mutex };
    return _bindings.contains(session);
}

void NativeController::pairTab(vtworkspace::TabId tab, uint64_t wireTab)
{
    auto const lock = std::lock_guard { _mutex };
    std::erase_if(_wireTabOf, [wireTab](auto const& entry) { return entry.second == wireTab; });
    _wireTabOf.insert_or_assign(tab.value, wireTab);
}

std::optional<vtworkspace::TabId> NativeController::tabPairedWith(uint64_t wireTab) const
{
    auto const lock = std::lock_guard { _mutex };
    auto const it = std::ranges::find(_wireTabOf, wireTab, [](auto const& entry) { return entry.second; });
    return it != _wireTabOf.end() ? std::optional { vtworkspace::TabId { it->first } } : std::nullopt;
}

void NativeController::forgetClosedTabs(std::unordered_set<uint64_t> const& liveTabs)
{
    auto const lock = std::lock_guard { _mutex };
    std::erase_if(_wireTabOf, [&liveTabs](auto const& entry) { return !liveTabs.contains(entry.first); });
}

std::optional<uint64_t> NativeController::pairedWireTab(vtworkspace::TabId tab) const
{
    auto const lock = std::lock_guard { _mutex };
    auto const it = _wireTabOf.find(tab.value);
    return it != _wireTabOf.end() ? std::optional { it->second } : std::nullopt;
}

bool NativeController::isClaimed(uint64_t session) const
{
    auto const lock = std::lock_guard { _mutex };
    return _bindings.contains(session) || _closedSessions.contains(session);
}

std::optional<uint64_t> NativeController::sessionForPty(vtpty::Pty const* pty) const
{
    auto const lock = std::lock_guard { _mutex };
    for (auto const& [session, binding]: _bindings)
        if (binding.pty == pty)
            return session;
    return std::nullopt;
}

void NativeController::requestCreateTab(vtpty::Pty const* actingPty,
                                        contour::session::RemoteSpawn const& spawn)
{
    // The acting pane names the WORKSPACE: the daemon hosts several (B4), and a request naming none
    // lands in its first one — so a "+" clicked in a second OS window opened a tab in the first. An
    // unbound (or null) pty resolves to 0, which is exactly that fallback.
    auto const beside = actingPty != nullptr ? sessionForPty(actingPty).value_or(0) : 0;
    // The client's send verbs must run on the reactor thread that owns it.
    postSessionCreation([beside, profile = spawn.profileName](NativeClient& client) mutable {
        client.createTab(beside, std::move(profile));
    });
}

void NativeController::requestSplitPane(vtpty::Pty const* actingPty,
                                        bool vertical,
                                        contour::session::RemoteSpawn const& spawn)
{
    auto const session = sessionForPty(actingPty);
    if (!session)
        return;
    auto const orientation = static_cast<uint8_t>(std::to_underlying(
        vertical ? vtworkspace::SplitState::Vertical : vtworkspace::SplitState::Horizontal));
    // An even split, because that is what every caller asking for one wants: the GUI's split actions
    // take TerminalSessionManager::splitActivePane's 0.5 default, and the reconciler-driven splits —
    // the only ones carrying a ratio — return above without reaching here. A divider the user then
    // MOVES is a separate verb (ResizeSplit); thread a ratio through here only once a caller varies it.
    constexpr uint16_t EvenSplitRatio = 5000;
    postSessionCreation(
        [session = *session, orientation, ratio = EvenSplitRatio, profile = spawn.profileName](
            NativeClient& client) mutable {
            client.splitPane(session, orientation, ratio, std::move(profile));
        });
}

contour::session::TabMergeSupport NativeController::tabMergeSupport() const noexcept
{
    auto const agreed = negotiated();
    return agreed && agreed->shared.contains(vthost::proto::Capability::TabMerge)
               ? contour::session::TabMergeSupport::Supported
               : contour::session::TabMergeSupport::Unsupported;
}

contour::session::RemoteRouting NativeController::requestRemoteTabMerge(
    std::span<vtpty::Pty const* const> tabPtys, vtpty::Pty const* targetPty, vtworkspace::SplitSide side)
{
    // A merge the reconciler itself applies came FROM the daemon; asking for it again would loop.
    if (isRealizingLayout())
        return contour::session::RemoteRouting::Local;
    auto tabSessions = std::vector<uint64_t> {};
    auto const allBound = std::ranges::all_of(tabPtys, [&](vtpty::Pty const* pty) {
        auto const session = sessionForPty(pty);
        if (session)
            tabSessions.push_back(*session);
        return session.has_value();
    });
    auto const targetSession = sessionForPty(targetPty);
    // The daemon owns this layout: a merge it cannot take is no merge, not one made here that it never hears
    // of.
    if (tabPtys.empty() || !allBound || !targetSession
        || tabMergeSupport() == contour::session::TabMergeSupport::Unsupported)
        return contour::session::RemoteRouting::Refused;
    // The client's send verbs must run on the reactor thread that owns it.
    _reactor.post([this, tabs = std::move(tabSessions), target = *targetSession, side] {
        if (_client != nullptr)
            std::ignore = _client->mergeTabIntoSplit(tabs, target, side);
    });
    return contour::session::RemoteRouting::Routed;
}

contour::session::MoveSupport NativeController::moveSupport() const noexcept
{
    auto const agreed = negotiated();
    return agreed && agreed->shared.contains(vthost::proto::Capability::MoveNode)
                   && agreed->shared.contains(vthost::proto::Capability::LayoutBatch)
               ? contour::session::MoveSupport::Supported
               : contour::session::MoveSupport::Unsupported;
}

contour::session::RemoteRouting NativeController::requestRemoteMove(contour::session::RemoteMove const& move)
{
    // A move the reconciler itself applies came FROM the daemon; asking for it again would loop.
    if (isRealizingLayout())
        return contour::session::RemoteRouting::Local;
    // The daemon owns this layout: a move it cannot take is no move, not one made here that it never hears
    // of.
    auto const namesSource = move.source == vtworkspace::MoveSourceKind::Pane
                                 ? move.sourceSessions.size() == 1
                                 : !move.sourceSessions.empty();
    if (!namesSource || moveSupport() == contour::session::MoveSupport::Unsupported)
        return contour::session::RemoteRouting::Refused;
    auto pdu = vthost::proto::MoveNode {
        .sourceKind = std::to_underlying(move.source),
        .sourceSessions = move.sourceSessions,
        .targetKind = std::to_underlying(move.target),
        .targetSession = move.target == vtworkspace::MoveTargetKind::NewWorkspace ? 0 : move.targetSession,
        .side = std::to_underlying(move.side),
        .index = static_cast<uint64_t>(std::max(move.index, 0)),
    };
    // The client's send verbs must run on the reactor thread that owns it.
    _reactor.post([this, pdu = std::move(pdu), request = move.request] {
        if (auto const serial = _client != nullptr ? _client->moveNode(pdu) : std::nullopt)
        {
            _moveRequests.insert_or_assign(*serial, request);
            return;
        }
        // Routed, so the manager waits for it: told now, it waits no longer, and the user learns why nothing
        // moved.
        emit moveRefused(MoveNotSent, 0, request);
    });
    return contour::session::RemoteRouting::Routed;
}

contour::session::RemoteRouting NativeController::requestRemoteRenameWorkspace(uint64_t workspace,
                                                                               std::string_view label)
{
    auto const agreed = negotiated();
    if (!agreed || !agreed->shared.contains(vthost::proto::Capability::WorkspaceMeta))
        return contour::session::RemoteRouting::Refused;
    // The client's send verbs must run on the reactor thread that owns it.
    _reactor.post([this, workspace, label = std::string { label }]() mutable {
        if (_client != nullptr)
            std::ignore = _client->setWorkspaceLabel(workspace, std::move(label));
    });
    return contour::session::RemoteRouting::Routed;
}

contour::session::RemoteRouting NativeController::requestRemoteTabTitle(uint64_t session,
                                                                        std::string_view title)
{
    if (!negotiated())
        return contour::session::RemoteRouting::Refused;
    auto edit = vthost::proto::EditTab {
        .session = session,
        .kind = title.empty() ? vthost::proto::TabEditKind::ClearTitle : vthost::proto::TabEditKind::SetTitle,
        .title = std::string { title },
        .color = 0,
    };
    _reactor.post([this, edit = std::move(edit)] {
        if (_client != nullptr)
            std::ignore = _client->editTab(edit);
    });
    return contour::session::RemoteRouting::Routed;
}

contour::session::RemoteRouting NativeController::requestRemoteMoveWorkspace(uint64_t workspace,
                                                                             uint32_t position)
{
    auto const agreed = negotiated();
    if (!agreed || !agreed->shared.contains(vthost::proto::Capability::WorkspaceMeta))
        return contour::session::RemoteRouting::Refused;
    _reactor.post([this, workspace, position] {
        if (_client != nullptr)
            std::ignore = _client->moveWorkspace(workspace, position);
    });
    return contour::session::RemoteRouting::Routed;
}

contour::session::RemoteRouting NativeController::requestRemoteCloseWorkspace(uint64_t workspace)
{
    auto const agreed = negotiated();
    if (!isConnected() || !agreed || !agreed->shared.contains(vthost::proto::Capability::WorkspaceMeta))
        return contour::session::RemoteRouting::Refused;
    {
        // Tombstoned before the close goes out, as requestRemoteClose() does: the daemon ends the sessions
        // later, and their in-flight deltas must not re-adopt a pane the user just ended.
        auto const lock = std::lock_guard { _mutex };
        if (auto const layout = _layouts.find(workspace); layout != _layouts.end())
        {
            auto const tombstone = [&](auto const& self, vthost::proto::WirePane const& pane) -> void {
                if (pane.isSplit())
                {
                    for (auto const& child: pane.children)
                        self(self, child);
                    return;
                }
                _closedSessions.insert(pane.session);
            };
            for (auto const& tab: layout->second->tabs)
                tombstone(tombstone, tab.root);
        }
    }
    _reactor.post([this, workspace] {
        if (_client != nullptr)
            std::ignore = _client->closeWorkspace(workspace);
    });
    return contour::session::RemoteRouting::Routed;
}

contour::session::RemoteRouting NativeController::requestRemoteSplitOrientation(vtpty::Pty const* anchorPty,
                                                                                vtworkspace::SplitState axis)
{
    // An axis the reconciler itself applies came FROM the daemon; asking for it again would loop.
    if (isRealizingLayout() || moveSupport() == contour::session::MoveSupport::Unsupported)
        return contour::session::RemoteRouting::Local;
    auto const anchor = sessionForPty(anchorPty);
    if (!anchor)
        return contour::session::RemoteRouting::Refused;
    // The client's send verbs must run on the reactor thread that owns it.
    _reactor.post([this, session = *anchor, axis] {
        if (_client != nullptr)
            std::ignore = _client->setSplitOrientation(session, axis);
    });
    return contour::session::RemoteRouting::Routed;
}

std::optional<uint64_t> NativeController::daemonInstance() const noexcept
{
    auto const lock = std::lock_guard { _mutex };
    return _daemonInstance != 0 ? std::optional { _daemonInstance } : std::nullopt;
}

void NativeController::reportSplitRatio(vtpty::Pty const* firstPty, vtpty::Pty const* secondPty, double ratio)
{
    // A ratio the reconciler itself applied came FROM the daemon; sending it back would make every
    // layout push author a write on the peer that pushed it.
    if (isRealizingLayout())
        return;
    auto const first = sessionForPty(firstPty);
    auto const second = sessionForPty(secondPty);
    if (!first || !second)
        return;
    auto const wireRatio = vthost::proto::toWireRatio(ratio);
    _reactor.post([this, first = *first, second = *second, wireRatio] {
        if (_client != nullptr)
            _client->resizeSplit(first, second, wireRatio);
    });
}

void NativeController::reportTabPresentation(contour::session::LocalTabPresentation const& presentation)
{
    // What the reconciler applies came FROM the daemon, as for a ratio (@see reportSplitRatio).
    if (isRealizingLayout())
        return;
    auto const active = sessionForPty(presentation.activePty);
    if (!active)
        return;
    auto const local = vthost::client::TabPresentation {
        .activeSession = *active,
        .zoomedSession =
            presentation.zoomedPty != nullptr ? sessionForPty(presentation.zoomedPty).value_or(0) : 0,
        .title = presentation.title,
        .color = presentation.color,
        .activity = presentation.activity,
    };
    auto edits = [&] {
        auto const lock = std::lock_guard { _mutex };
        for (auto const& layout: _layouts | std::views::values)
            for (auto const [index, tab]: core::views::enumerate(layout->tabs))
                if (vthost::client::paneTreeHosts(tab.root, *active))
                {
                    auto const [reported, fresh] = _reportedPresentations.try_emplace(
                        tab.tabId,
                        vthost::client::presentationOf(tab,
                                                       std::cmp_equal(index, layout->activeTab)
                                                           ? vthost::client::TabActivity::Active
                                                           : vthost::client::TabActivity::Inactive));
                    std::ignore = fresh;
                    auto found = vthost::client::tabEditsToReport(local, reported->second);
                    reported->second = local;
                    return found;
                }
        // A tab the daemon has not pushed yet, or no longer has: nothing to compare with.
        return std::vector<vthost::proto::EditTab> {};
    }();
    if (edits.empty())
        return;
    _reactor.post([this, edits = std::move(edits)] {
        if (_client == nullptr)
            return;
        for (auto const& edit: edits)
            std::ignore = _client->editTab(edit);
    });
}

void NativeController::replayUnsentCloses(vthost::proto::LayoutState const& layout)
{
    if (_client == nullptr || _unsentCloses.empty())
        return;
    auto const resend = [&](auto const& self, vthost::proto::WirePane const& pane) -> void {
        if (pane.isSplit())
        {
            for (auto const& child: pane.children)
                self(self, child);
            return;
        }
        if (std::erase(_unsentCloses, pane.session) != 0)
            std::ignore = _client->closePane(pane.session);
    };
    for (auto const& tab: layout.tabs)
        resend(resend, tab.root);
}

void NativeController::requestClosePane(uint64_t session)
{
    _reactor.post([this, session] { closePaneOnReactor(session); });
}

void NativeController::closePaneOnReactor(uint64_t session)
{
    // A close is an intent, not a keystroke: one issued while the connection is down is sent once the
    // same daemon answers again (adoptReconnect), rather than leave its shell running where no window of
    // this client can ever show it.
    if (_client == nullptr)
    {
        _pendingCloses.push_back(session);
        return;
    }
    // The connection can be dying with no state change delivered yet, and a close it never wrote is
    // sent again if the same daemon still lists the pane (replayUnsentCloses).
    if (_client->closePane(session) == vthost::client::Delivery::Unsupported)
        _unsentCloses.push_back(session);
}

void NativeController::onPermissionRequest(vthost::proto::PermissionRequest const& request)
{
    {
        auto const lock = std::lock_guard { _mutex };
        _permissionRequests.push_back(request);
    }
    emit extensionPermissionRequested();
}

std::vector<NativeController::HostedPermissionRequest> NativeController::takePermissionRequests()
{
    auto const lock = std::lock_guard { _mutex };
    auto taken = std::vector<HostedPermissionRequest> {};
    // A question for a session the user closed is nobody's to answer here; one for a session not bound
    // yet waits for its pane.
    std::erase_if(_permissionRequests, [&](vthost::proto::PermissionRequest const& request) {
        if (_closedSessions.contains(request.session))
            return true;
        auto const binding = _bindings.find(request.session);
        if (binding == _bindings.end() || binding->second.pty == nullptr)
            return false;
        taken.push_back(HostedPermissionRequest { .pty = binding->second.pty, .request = request });
        return true;
    });
    return taken;
}

void NativeController::onSwitchRequest(vthost::proto::SwitchRequest const& request)
{
    {
        auto const lock = std::lock_guard { _mutex };
        _switchRequestQueue.push_back(request);
    }
    emit switchRequested();
}

std::vector<vthost::proto::SwitchRequest> NativeController::takeSwitchRequests()
{
    auto const lock = std::lock_guard { _mutex };
    return std::exchange(_switchRequestQueue, {});
}

void NativeController::replySwitch(vthost::proto::SwitchReply reply)
{
    _reactor.post([this, reply = std::move(reply)]() mutable {
        if (_client != nullptr)
            _client->replySwitch(std::move(reply));
    });
}

void NativeController::answerPermission(std::string hostId, std::string digest, vthost::GrantState answer)
{
    _reactor.post([this, hostId = std::move(hostId), digest = std::move(digest), answer]() mutable {
        if (_client != nullptr)
            _client->answerPermission(std::move(hostId), std::move(digest), answer);
    });
}

void NativeController::reportPaneGeometry(uint64_t session,
                                          uint64_t serial,
                                          vtpty::PageSize cells,
                                          std::optional<vtpty::ImageSize> pixels)
{
    {
        // _geometryMutex alone: this runs from the pty's resize sink, which the mirror reaches while
        // _mutex is held. Taking _mutex here would deadlock the reactor against itself.
        auto const lock = std::lock_guard { _geometryMutex };
        // A detached binding's pane: the session id is a new session's now, if anyone's.
        if (auto const owner = _paneSerials.find(session);
            owner == _paneSerials.end() || owner->second != serial)
            return;
        _paneSizes.insert_or_assign(session, cells);
        // The pixels are cell x page, so the division is exact; a report without them keeps the last cell.
        if (auto const cell = cellOf(cells, pixels); cell.has_value())
            _cellPixelSize = cell;
        _geometryAnchor = session;
        if (_geometryFlushScheduled)
            return; // a flush is already queued; it will pick this up too
        _geometryFlushScheduled = true;
    }
    // Queued onto the GUI thread: it coalesces every pane resized in this layout pass into one
    // report, and it is what makes the sink safe to call from the parser thread (DECCOLM resizes
    // there). A metacall posted to a QObject is dropped if the object dies first.
    platform::postToObject(this, [this] { flushGeometry(GeometryFlush::Report); });
}

void NativeController::flushGeometry(GeometryFlush scope)
{
    auto area = std::optional<vtpty::PageSize> {};
    auto cell = std::optional<vtpty::ImageSize> {};
    auto panes = std::vector<std::pair<uint64_t, vtpty::PageSize>> {};
    {
        // Both, in the documented order: composedClientArea() reads the layouts (_mutex) and the
        // pane grids (_geometryMutex).
        auto const lock = std::lock_guard { _mutex };
        auto const geometryLock = std::lock_guard { _geometryMutex };
        _geometryFlushScheduled = false;

        auto const composed = composedClientArea();
        cell = _cellPixelSize;
        // A reconnect or a daemon's PanesReprojected asks for every pane again: spent once they compose.
        // Spent then even if the layout is only partly laid out -- some panes not yet in _paneSizes, so not
        // re-sent below. That relies on an invariant: every pane reports its grid at least once after a
        // reconcile (the resize sink fires as its display is created), so a pane missing here is covered by
        // its own first report, which is never filtered as unchanged.
        auto const reassert =
            composed.has_value()
            && std::exchange(_geometryReassert, GeometryReassert::Idle) == GeometryReassert::Pending;
        if (scope == GeometryFlush::PanesOnly && !reassert)
            return;
        auto resendAll = reassert;
        // The hello's area, once composed with a connection that heard it, is the daemon's: not stated again.
        if (composed.has_value() && _client != nullptr && _helloArea.has_value())
        {
            if (composed == _helloArea)
            {
                _lastReportedArea = std::exchange(_helloArea, std::nullopt);
                resendAll = true;
            }
            else if (scope == GeometryFlush::Report)
                _helloArea.reset(); // the window moved on: this report states its own area
        }
        // A changed cell is stated like a moved area, as the daemon lays images out on it -- but only a new
        // client area re-projects every pane on the daemon, so only that re-asserts all, not just the moved.
        if (scope == GeometryFlush::Report && composed.has_value())
        {
            auto const areaMoved = _lastReportedArea != composed;
            if (areaMoved || _lastReportedCell != _cellPixelSize)
            {
                _lastReportedCell = _cellPixelSize;
                _lastReportedArea = composed;
                _lastStatedArea = composed;
                area = composed;
                resendAll = resendAll || areaMoved;
            }
        }
        for (auto const& [session, size]: _paneSizes)
        {
            if (!_bindings.contains(session))
                continue;
            auto const sent = _lastReportedPaneSizes.find(session);
            if (!resendAll && sent != _lastReportedPaneSizes.end() && sent->second == size)
                continue;
            _lastReportedPaneSizes.insert_or_assign(session, size);
            panes.emplace_back(session, size);
        }
    }

    if (!area.has_value() && panes.empty())
        return;

    _reactor.post([this, area, cell, panes = std::move(panes)] {
        if (_client == nullptr)
            return;
        // The client area first: it re-projects the tree, and the per-pane grids refine THAT.
        if (area.has_value())
            _client->requestResize(
                unbox<uint32_t>(area->columns), unbox<uint32_t>(area->lines), toWireCell(cell));
        for (auto const& [session, size]: panes)
            _client->resizePane(
                session, unbox<uint32_t>(size.columns), unbox<uint32_t>(size.lines), toWireCell(cell));
    });
}

void NativeController::reassertGeometryAfterReconcile()
{
    {
        // So that a reconcile with nothing pending takes no _mutex; released before flushGeometry() takes it.
        auto const lock = std::lock_guard { _geometryMutex };
        if (_geometryReassert != GeometryReassert::Pending)
            return;
    }
    flushGeometry(GeometryFlush::PanesOnly);
}

std::optional<vtpty::PageSize> NativeController::composedClientArea() const
{
    auto const resolve = [this](uint64_t session) -> std::optional<vtpty::PageSize> {
        if (auto const rendered = _paneSizes.find(session); rendered != _paneSizes.end())
            return rendered->second;
        // Discovered but not realized yet: the remote screen's own extent stands in, so a tab
        // mid-realization composes to the area the daemon already believes in (a no-op report)
        // rather than dropping out entirely.
        if (auto const pending = std::ranges::find(_pending, session, &PendingSession::session);
            pending != _pending.end())
            return vtpty::PageSize { .lines = vtpty::LineCount(static_cast<int>(pending->lines)),
                                     .columns = vtpty::ColumnCount(static_cast<int>(pending->columns)) };
        return std::nullopt;
    };

    // Every mirrored tab, in workspace-id then tab order — `_layouts` is ordered, so the fallback the
    // composer picks is deterministic rather than a hash-order accident.
    auto const roots =
        _layouts | std::views::values
        | std::views::transform([](auto const& layout) -> auto const& { return layout->tabs; })
        | std::views::join
        | std::views::transform([](auto const& tab) -> vthost::proto::WirePane const& { return tab.root; });
    return vthost::client::composeAnchoredClientArea(roots, _geometryAnchor, resolve);
}

vthost::proto::WireColorPreference NativeController::shownColors() const
{
    // Read, never stored back: a reactor that wrote what it read could overwrite a newer statement the GUI
    // thread made meanwhile, which requestHostedOperation() keeps only in _statedColors when unreachable.
    if (auto const shown = _colorPreference ? _colorPreference() : std::nullopt)
        return wireOf(*shown);
    return _statedColors.load();
}

void NativeController::stateColorPreference(vthost::client::NativeClient& client, uint64_t session) const
{
    auto const stated = shownColors();
    if (stated == vthost::proto::WireColorPreference::Unstated)
        return;
    client.performOperation(vthost::proto::PerformOperation {
        .session = session,
        .operation = stated == vthost::proto::WireColorPreference::Dark
                         ? vthost::proto::EngineOperation::UseDarkColors
                         : vthost::proto::EngineOperation::UseLightColors });
}

void NativeController::primeBinding(uint64_t session)
{
    if (_client == nullptr)
        return;

    auto const lock = std::lock_guard { _mutex };
    auto const binding = _bindings.find(session);
    if (binding == _bindings.end() || binding->second.terminal == nullptr || binding->second.mirror)
        return;
    // Judged once, here: the ServerHello that named the daemon's machine preceded every session the
    // connection mirrors, and the verdict holds for the connection's life.
    auto& terminal = *binding->second.terminal;
    binding->second.mirror = std::make_unique<vthost::client::ScreenMirror>(
        terminal, vthost::daemonMachineOf(_client->daemonIdentity(), terminal.localIdentity()));

    // What the app shows, for a session whose pane was bound before the connection could take the request.
    stateColorPreference(*_client, session);

    // The daemon may already have described this session before its pane existed.
    auto const screen = _client->screens().find(session);
    if (screen == _client->screens().end())
        return;
    // Discard: this pane's mirror terminal is brand new, so there is no local
    // scrollback of its own to preserve.
    binding->second.mirror->fullReplay(screen->second, vthost::client::LocalHistory::Discard);
    tellInterruption(binding->second);
}

void NativeController::tellInterruption(Binding& binding)
{
    if (!binding.mirror || binding.interruptions.context == nullptr)
        return;
    auto const& commandLine = binding.mirror->interruptedCommand();
    if (commandLine == binding.interruptionTold)
        return;
    binding.interruptionTold = commandLine;
    auto told = commandLine.empty() ? std::optional<vtbackend::CommandLineText> {}
                                    : std::optional { vtbackend::CommandLineText { commandLine } };
    // The reactor thread: the session's state is the GUI thread's, so the news is posted there.
    platform::postToObject(binding.interruptions.context,
                           [deliver = binding.interruptions.deliver, told = std::move(told)]() mutable {
                               deliver(std::move(told));
                           });
}

void NativeController::bindTerminal(vtpty::Pty const* pty,
                                    vtbackend::Terminal& terminal,
                                    contour::session::InterruptionSink const& interruptions)
{
    auto const session = sessionForPty(pty);
    if (!session)
        return; // not one of ours (a local session, or already unbound)

    auto asked = false;
    {
        auto const lock = std::lock_guard { _mutex };
        auto const binding = _bindings.find(*session);
        if (binding == _bindings.end())
            return;
        binding->second.terminal = &terminal;
        binding->second.interruptions = interruptions;
        asked = std::ranges::any_of(_permissionRequests,
                                    [&](auto const& request) { return request.session == *session; });
    }
    // The mirror is built and primed on the reactor, where _client lives, rather than from here.
    _reactor.post([this, id = *session] { primeBinding(id); });
    // A question asked before the pane was bound is the pane's to ask now.
    if (asked)
        emit extensionPermissionRequested();
}

void NativeController::onImage(RemoteScreen const& screen, uint32_t imageId)
{
    auto const lock = std::lock_guard { _mutex };
    if (auto const binding = _bindings.find(screen.session);
        binding != _bindings.end() && binding->second.mirror)
        binding->second.mirror->applyImage(screen, imageId);
}

void NativeController::onSessionEvent(RemoteScreen const& screen, vthost::proto::SessionEventPdu const& event)
{
    auto const lock = std::lock_guard { _mutex };
    // The program-status set of every session, mirrored or not: what a directory lists of the panes of the
    // workspaces this window does not select.
    if (auto const* status = std::get_if<vthost::proto::ProgramStatus>(&event))
    {
        auto& records = _paneStatus[status->session].status;
        records.clear();
        for (auto const& wire: status->records)
            if (auto record = vthost::control::fromWire(wire))
                records.push_back(std::move(*record));
        scheduleStatusFlush();
    }
    // On the reactor thread. Stored on the pane's device, which the session reads on the GUI thread when the
    // output that follows it lands (TerminalSession::refreshInputEcho); never applied to the grid.
    if (auto const* echo = std::get_if<vthost::proto::SessionInputEcho>(&event))
    {
        auto const value = vthost::inputEchoFromWire(echo->echo);
        _inputEchoes.insert_or_assign(screen.session, value);
        if (auto const binding = _bindings.find(screen.session);
            binding != _bindings.end() && binding->second.pty != nullptr)
            binding->second.pty->setInputEcho(value);
        return;
    }
    if (auto const binding = _bindings.find(screen.session);
        binding != _bindings.end() && binding->second.mirror)
        binding->second.mirror->applyEvent(event);
}

void NativeController::requestRemoteClose(vtpty::Pty const* pty)
{
    auto const session = sessionForPty(pty);
    if (!session)
        return; // not one of ours (a local session, or already unbound)
    {
        // Tombstone before the close goes out: the remote session stays alive until the daemon
        // honors it, and its in-flight deltas must not re-adopt a pane the user just ended.
        auto const lock = std::lock_guard { _mutex };
        _closedSessions.insert(*session);
    }
    requestClosePane(*session);
}

contour::session::HostedOperationRoute NativeController::requestHostedOperation(
    vtpty::Pty const* pty, contour::session::HostedOperationRequest const& request)
{
    using contour::session::HostedOperationRoute;
    // Remembered before anything can refuse it, for the hello and the reconnect to state again.
    if (auto const preference = contour::session::colorPreferenceOf(request.operation))
        _statedColors = wireOf(*preference);
    // First, and for every pty: each pane this factory made mirrors a daemon session, and a mirror whose
    // daemon is out of reach has nowhere to run the operation that would stick.
    if (!isConnected())
        return HostedOperationRoute::Unreachable;
    auto const session = sessionForPty(pty);
    if (!session)
        return HostedOperationRoute::RunHere; // not one of ours
    auto const route = contour::session::routeOf(request.operation, contour::session::RemoteHostKind::Daemon);
    if (route == HostedOperationRoute::RunOnHost)
        _reactor.post([this, session = *session, request] {
            if (_client == nullptr)
                return;
            auto operation = vthost::proto::PerformOperation {
                .session = session, .operation = toWireOperation(request.operation), .text = request.text
            };
            if (request.row)
            {
                // Named on the reactor, where the mirror is applied: only it knows which daemon row a row of
                // its own holds, and the answer stands only until its next update.
                auto const row = hostRowOf(session, *request.row);
                if (!row)
                {
                    attachLog()("Did not {}: the row is not one the daemon can be told of.",
                                contour::session::describe(request.operation));
                    return;
                }
                operation.row = *row;
            }
            _client->performOperation(std::move(operation));
        });
    return route;
}

std::optional<vthost::proto::RowAddress> NativeController::hostRowOf(uint64_t session,
                                                                     contour::session::HostedRow row) const
{
    auto const lock = std::lock_guard { _mutex };
    auto const binding = _bindings.find(session);
    if (binding == _bindings.end() || !binding->second.mirror)
        return std::nullopt;
    return binding->second.mirror->hostRowOf(row.stableId, row.generation);
}

bool NativeController::isBinding(uint64_t session, uint64_t serial) const
{
    auto const lock = std::lock_guard { _mutex };
    auto const binding = _bindings.find(session);
    return binding != _bindings.end() && binding->second.serial == serial;
}

void NativeController::unbind(uint64_t session, uint64_t serial)
{
    forgetBinding(session, serial);
    // Also for a detached binding's pty: it was counted live until now. Outside the lock: a receiver asks
    // bindsPanes(), which takes it.
    emit paneUnbound();
}

bool NativeController::bindsPanes() const
{
    auto const lock = std::lock_guard { _mutex };
    return _livePtys != 0;
}

void NativeController::forgetBinding(uint64_t session, uint64_t serial)
{
    auto const lock = std::lock_guard { _mutex };
    --_livePtys;
    auto const binding = _bindings.find(session);
    // A detached binding's pty, or one released as its pane was detached: the session id, if bound at all, is
    // another pane's.
    if (binding == _bindings.end() || binding->second.serial != serial)
        return;
    dropBinding(binding);
    _inputEchoes.erase(session); // a session id is never reused within a run
    // Tombstone while the connection lives, so a remote session that outlives its local pane cannot
    // resurrect it through a later delta. Whether that session should also be ENDED on the daemon is
    // NOT knowable here — a pty is destroyed by a pane close, a window close, an app quit and a lost
    // connection alike — so the close is authored by the caller that knows, through
    // SessionFactory::requestRemoteClose. Inferring it from this destructor is what made closing a
    // window destroy the user's shells.
    if (_stopPhase == StopPhase::Running)
        _closedSessions.insert(session);
}

void NativeController::releaseRemoteSession(vtpty::Pty const* pty)
{
    auto const lock = std::lock_guard { _mutex };
    auto const binding =
        std::ranges::find_if(_bindings, [pty](auto const& entry) { return entry.second.pty == pty; });
    // The pty stays counted live until it is destroyed, when unbind() finds no binding of its serial to
    // forget.
    if (binding != _bindings.end())
        dropBinding(binding);
}

void NativeController::dropBinding(std::unordered_map<uint64_t, Binding>::iterator binding)
{
    auto const session = binding->first;
    _bindings.erase(binding);
    // The pane is gone: its grid must not keep contributing to a composed client area, and the
    // sent-size bookkeeping for it is meaningless (a pane's binding is never bound again).
    auto const geometryLock = std::lock_guard { _geometryMutex };
    _paneSizes.erase(session);
    _paneSerials.erase(session);
    _lastReportedPaneSizes.erase(session);
    // The anchor is the third thing this pane owned: it selects WHICH tab the client area is
    // composed from, and once the pane is gone the anchor can only ever miss. Left set, a flush already
    // queued when this ran would compose the fallback tab while still believing it had anchored —
    // reporting a client area for a window the user never touched, which the daemon answers by
    // re-projecting every pane of every workspace.
    if (_geometryAnchor == session)
        _geometryAnchor.reset();
}

void NativeController::closeAllBindings()
{
    // This acquires _mutex then calls pty->close() for each binding, which
    // wakes the parser thread. The parser thread processes EOF, tears down
    // the TerminalSession, and the SelfUnbindingChannelPty destructor calls
    // unbind(session) — which also tries to acquire _mutex. This is cross-
    // thread contention, not a deadlock: the parser thread blocks until
    // closeAllBindings() returns, delaying cleanup but never wedging.
    auto const lock = std::lock_guard { _mutex };
    for (auto& [session, binding]: _bindings)
        binding.pty->close();
}

std::size_t NativeController::pendingCount() const
{
    auto const lock = std::lock_guard { _mutex };
    return _pending.size();
}

bool NativeController::canCreateSession() const noexcept
{
    auto const lock = std::lock_guard { _mutex };
    // During a layout realization the panes are bound by setNextBindSession (not
    // the FIFO queue), so allow creation even with nothing pending.
    // A pane taken back after a reconnect names its session by the same hook (bindForReconnect).
    return _realizingLayout || _nextBindSession.has_value() || !_pending.empty();
}

std::unique_ptr<vtpty::Pty> NativeController::createPty(std::optional<std::string> /*cwd*/,
                                                        std::optional<vtbackend::PageSize> pageSize,
                                                        std::optional<vtpty::Process::ExecInfo> /*command*/,
                                                        std::optional<std::string> /*profileName*/)
{
    auto lock = std::unique_lock { _mutex };
    auto session = uint64_t {};
    auto taken = std::optional<PendingSession> {};
    if (_nextBindSession.has_value())
    {
        // Layout executor: bind this pane to the remote session the beforeLeafSeed
        // hook named. Its screen may not have arrived yet (the layout leads the
        // snapshot), so the pane is born at the window's size and the mirror's first
        // replay (on binding) repaints it; a resize then reconciles.
        session = *_nextBindSession;
        _nextBindSession.reset();
        // A realized session must leave the pending queue (as TmuxController's
        // parallel branch does): left behind, a later hook-less creation pops the
        // stale entry and rebinds an already-live session, hijacking its feed.
        if (auto const pending = std::ranges::find(_pending, session, &PendingSession::session);
            pending != _pending.end())
        {
            taken = *pending;
            _pending.erase(pending);
        }
    }
    else if (!_pending.empty())
    {
        taken = _pending.front();
        _pending.pop_front();
        session = taken->session;
    }
    else
    {
        // The creation guards should have prevented this; a session must
        // still be born, so give it a dead-end pty it can close cleanly.
        attachLog()("No pending remote session; handing out an unbound pty.");
        return makeUnboundFallbackPty(pageSize);
    }

    // Born at the REMOTE size (the pending record carries the remote screen's
    // geometry) so the mirror's first replay paints a matching grid; without a
    // record the window's size stands in and a resize then reconciles.
    auto const fallback = pageSize.value_or(vtbackend::PageSize {});

    // Resolve a dimension from the pending record if available; otherwise fall
    // back to the pageSize field, defaulting to the given value when unset.
    auto const sizeField = [](auto const& dim, int defaultValue) noexcept -> int {
        if (dim.value != 0)
            return unbox<int>(dim);
        return defaultValue;
    };
    auto const columns = taken ? static_cast<int>(taken->columns) : sizeField(fallback.columns, 80);
    auto const lines = taken ? static_cast<int>(taken->lines) : sizeField(fallback.lines, 25);

    auto const serial = _nextBindingSerial++;
    auto pty = std::make_unique<SelfUnbindingChannelPty>(
        vtpty::PageSize { vtpty::LineCount(lines), vtpty::ColumnCount(columns) },
        [this, session, serial](std::string_view bytes, vtpty::InputKind kind) {
            // Typed while no connection is up: dropped here, never queued for whatever answers next.
            auto const connection = _inputConnection.load();
            if (connection == 0)
            {
                _droppedInputBytes += bytes.size();
                return;
            }
            _reactor.post([this, session, serial, connection, kind, copy = std::string { bytes }] {
                // Lost meanwhile, or this pane's binding was detached and its session id is another's now.
                if (_client == nullptr || _inputConnection.load() != connection
                    || !isBinding(session, serial))
                {
                    _droppedInputBytes += copy.size();
                    return;
                }
                _client->sendInput(session, copy, wireInputKind(kind));
            });
        },
        // A pane's grid is NOT the client area: reporting it as one made the daemon project the
        // whole tab into a single pane's width, halving every pane on every split. The report is
        // composed from all panes instead — see reportPaneGeometry.
        [this, session, serial](vtpty::PageSize cells, std::optional<vtpty::ImageSize> pixels) {
            reportPaneGeometry(session, serial, cells, pixels);
        },
        [this, session, serial] { unbind(session, serial); });
    _bindings[session] = Binding { .serial = serial, .pty = pty.get(), .terminal = nullptr, .mirror = {} };
    ++_livePtys; // until its destructor's unbind(), however long the binding itself lasts
    // A prompt that was waiting before this pane was bound.
    if (auto const echo = _inputEchoes.find(session); echo != _inputEchoes.end())
        pty->setInputEcho(echo->second);
    // The birth size is this pane's first known grid, so the composition is complete from the
    // moment the pane exists rather than only once something resizes it.
    {
        auto const geometryLock = std::lock_guard { _geometryMutex };
        _paneSizes.insert_or_assign(
            session,
            vtpty::PageSize { .lines = vtpty::LineCount(lines), .columns = vtpty::ColumnCount(columns) });
        _paneSerials.insert_or_assign(session, serial);
    }
    lock.unlock();

    // No priming here: it needs the terminal, which does not exist until bindTerminal() announces
    // it (the pty is a constructor argument to the session that owns that terminal).
    attachLog()("Bound remote session {} to a new local pty ({}x{}).", session, columns, lines);
    return pty;
}

void abandonConnect(std::unique_ptr<NativeController>& live,
                    std::unique_ptr<NativeController>& abandoned,
                    QObject const* owner)
{
    live->stop();
    QObject::disconnect(live.get(), nullptr, owner, nullptr);
    abandoned = std::move(live);
}

} // namespace contour::remote
