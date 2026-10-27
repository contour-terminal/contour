// SPDX-License-Identifier: Apache-2.0
#include <contour/platform/QtInvoke.hpp>
#include <contour/remote/NativeController.hpp>
#include <contour/session/TerminalSessionManager.hpp>

#include <core/Utils.hpp>
#include <core/net/Sockets.hpp>

#include <algorithm>
#include <format>
#include <ranges>
#include <utility>

#include <vthost/HostIdentity.hpp>
#include <vthost/InputEchoWire.hpp>

namespace contour::remote
{

using vthost::client::NativeClient;
using vthost::client::RemoteScreen;

namespace
{
    auto const attachLog = core::log::Category("gui.attach", "GUI native-attach controller.");
} // namespace

NativeController::NativeController(std::shared_ptr<vthost::ITransportFactory> transport,
                                   vthost::proto::AttachIntent intent,
                                   std::string profileName,
                                   std::optional<vtbackend::Settings> attachSettings,
                                   SessionSettingsSource sessionSettings,
                                   core::platform::IClock const& clock,
                                   vthost::latency::StartupTimeline& timeline):
    RemoteController(clock),
    _transport(std::move(transport)),
    _intent(std::move(intent)),
    _profileName(std::move(profileName)),
    _attachSettings(std::move(attachSettings)),
    _sessionSettings(std::move(sessionSettings)),
    _timeline(timeline)
{
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

NativeController::~NativeController()
{
    stop();
}

// startConnecting(), awaitConnected() and stop() are provided by RemoteController; this controller supplies
// runClient() and the detach / binding-teardown / message hooks (see NativeController.h).

std::unique_ptr<NativeClient> NativeController::makeClient(core::net::EventLoop& loop,
                                                           std::unique_ptr<vthost::Connection> connection,
                                                           vthost::proto::AttachIntent intent)
{
    if (_acceptedConnections == 0)
        _timeline.mark(vthost::latency::StartupMark::Connected);
    return std::make_unique<NativeClient>(
        loop,
        std::move(connection->socket),
        NativeClient::HandshakeOptions {
            .token = connection->token,
            // The daemon applies it to the sessions THIS connection goes on to create, and to nothing
            // that already exists; postSessionCreation() restates it when the configuration changed.
            .sessionSettings =
                _attachSettings.transform([](auto const& s) { return vthost::toWireSessionSettings(s); }),
            // ExtensionPermissions is offered because this client passes a PermissionHandler below: its
            // panes ask the user, ControlPermissions because their prompt lists the control permissions a
            // question asks for (vthost::client::consentRequestOf). InitialClientArea because the hello
            // states the area below. AttachIntent so that the daemon names the windows a reconnect resumes,
            // Heartbeat so that a connection gone half-open is noticed and replaced. ProfileByName because
            // the hello and every spawning verb name a profile for the daemon to resolve in its own
            // configuration. TabEdits because the user's changes to a tab's presentation are reported
            // (reportTabPresentation). ProgramStatus because the mirrors take the OSC 7501 records the daemon
            // pushes, for the tab strip to show. InputEcho because a pane at a password prompt is locked, and
            // turns secure keyboard entry on, only when the daemon says its terminal hides typing.
            .capabilities =
                vthost::proto::CapabilitySet::of({ vthost::proto::Capability::ExtensionPermissions,
                                                   vthost::proto::Capability::ControlPermissions,
                                                   vthost::proto::Capability::InitialClientArea,
                                                   vthost::proto::Capability::AttachIntent,
                                                   vthost::proto::Capability::Heartbeat,
                                                   vthost::proto::Capability::ProfileByName,
                                                   vthost::proto::Capability::TabEdits,
                                                   vthost::proto::Capability::ProgramStatus,
                                                   vthost::proto::Capability::InputEcho }),
            // The profile's terminal size, so the first session spawns at it rather than at the daemon's
            // default; the ResizeRequest sent once the window is realized corrects any difference.
            .clientArea = _attachSettings.transform([](auto const& s) {
                return vthost::proto::WireClientArea { .columns = unbox<uint32_t>(s.pageSize.columns),
                                                       .lines = unbox<uint32_t>(s.pageSize.lines) };
            }),
            .attachIntent = std::move(intent),
            .profileName = _profileName,
        },
        // What the panes' own stores keep (`command_blocks.max_records`), resolved from the same
        // configuration: a mirror holds no more records than the terminal it feeds.
        _attachSettings.transform([](auto const& s) { return s.commandBlockLimits; })
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
            [this](vthost::proto::PermissionRequest const& request) { onPermissionRequest(request); } });
}

core::async::Task<void> NativeController::runClient(core::net::EventLoop* loop)
{
    _timeline.mark(vthost::latency::StartupMark::ConnectBegin);
    auto supervisor = vthost::client::ClientSupervisor {
        *loop,
        _transport,
        [this, loop](std::unique_ptr<vthost::Connection> connection, vthost::proto::AttachIntent intent) {
            return makeClient(*loop, std::move(connection), std::move(intent));
        },
        _intent,
        vthost::client::BackoffSchedule {},
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
        settled = _connection == vthost::client::ConnectionState::Failed
                  || _connection == vthost::client::ConnectionState::Refused
                  || _connection == vthost::client::ConnectionState::Stopped;
    }
    if (!settled)
        onConnectionState(vthost::client::ConnectionState::Stopped, {});
}

void NativeController::onConnectionState(vthost::client::ConnectionState state, std::string const& reason)
{
    using vthost::client::ConnectionState;
    auto const where = _transport->describe();
    auto closed = false;
    auto missingWindows = false;
    {
        auto const lock = std::lock_guard { _mutex };
        _connection = state;
        switch (state)
        {
            case ConnectionState::Connecting: break;
            case ConnectionState::Connected:
                _client = _supervisor != nullptr ? _supervisor->current() : nullptr;
                // Each connection's own: a reconnect can reach an upgraded or downgraded daemon.
                _negotiated.reset();
                captureHandshake();
                if (_acceptedConnections != 0)
                    adoptReconnect();
                else if (_client != nullptr)
                {
                    _daemonInstance = _client->daemonInstance();
                    // Asked for windows the daemon does not have, a first attach would wait out its budget
                    // for a session that never comes: it ends now, saying which.
                    missingWindows = _intent.kind == vthost::proto::AttachIntentKind::Windows
                                     && _client->followedWindows().empty();
                }
                ++_acceptedConnections;
                _inputConnection = _acceptedConnections;
                attachLog()("Attached to {}.", where);
                break;
            case ConnectionState::Reconnecting:
                _client = nullptr;
                _inputConnection = 0;
                forgetInputEchoes();
                // Before the first session arrived the attach still waits; awaitConnected() sees it through.
                if (_state == State::Ready)
                    _state = State::Reconnecting;
                attachLog()("Lost the connection to {} ({}); reconnecting.", where, reason);
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
    if (missingWindows)
        failConnect(std::format("the daemon has no window {}", core::joinHumanReadable(_intent.windows)));
    emit connectionStateChanged();
    if (closed)
        emit connectionClosed();
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

void NativeController::adoptReconnect()
{
    auto* client = _client;
    if (client == nullptr)
        return;
    auto const mapped = std::ranges::to<std::vector>(_layouts | std::views::keys);
    // Window and session ids count from 1 in every run of a daemon: once it restarted, the ids this client
    // holds name nothing of the new run, however many of them it reuses.
    auto const restarted =
        _daemonInstance != 0 && client->daemonInstance() != 0 && _daemonInstance != client->daemonInstance();
    auto const sameInstance = _daemonInstance != 0 && _daemonInstance == client->daemonInstance();
    _daemonInstance = client->daemonInstance();
    if (restarted)
    {
        _lostWindows = mapped;
        // Every binding is detached, its pty left to the window closing it: a new session with the same id
        // must not reach the old pane, nor the old pane's input the new session.
        _bindings.clear();
        // The new run's sessions count from 1 again: an old Hidden is no pane's of this run.
        _inputEchoes.clear();
        _closedSessions.clear();
        _pending.clear();
        // Closes of the old run's sessions: the ids name the new run's, if anything.
        _pendingCloses.clear();
        _unsentCloses.clear();
    }
    else
    {
        // A daemon that predates attach intents names no windows, and every window is followed.
        if (client->negotiated()
            && client->negotiated()->shared.contains(vthost::proto::Capability::AttachIntent))
            _lostWindows = std::ranges::to<std::vector>(mapped | std::views::filter([&](uint64_t window) {
                                                            return !std::ranges::contains(
                                                                client->followedWindows(), window);
                                                        }));
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
    _reportedPresentations.clear();
    {
        auto const geometryLock = std::lock_guard { _geometryMutex };
        _lastReportedArea.reset();
        _lastReportedPaneSizes.clear();
        if (restarted)
        {
            _paneSizes.clear();
            _paneSerials.clear();
            _geometryAnchor.reset();
        }
    }
    // Up again. One lost before its first session arrived is still being waited for, as a first attach is.
    if (_state == State::Reconnecting)
        _state = State::Ready;
    if (auto const dropped = _droppedInputBytes.exchange(0); dropped != 0)
        attachLog()("Dropped {} bytes of input typed while the connection to {} was down.",
                    dropped,
                    _transport->describe());
    if (!_lostWindows.empty())
        attachLog()(
            "{} window(s) of {} are gone after the reconnect.", _lostWindows.size(), _transport->describe());
}

vthost::client::ConnectionState NativeController::connectionState() const
{
    auto const lock = std::lock_guard { _mutex };
    return _connection;
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
    emit remoteSessionDiscovered();
}

void NativeController::onLayout(vthost::proto::LayoutState const& layout)
{
    _timeline.mark(vthost::latency::StartupMark::LayoutReceived);
    auto lost = std::vector<uint64_t> {};
    {
        auto const lock = std::lock_guard { _mutex };
        // Announced with the first window that came instead, so the GUI has a window to show before it
        // closes the lost ones -- closing the last one first would end the application.
        lost = std::exchange(_lostWindows, {});
        captureHandshake();
        _layouts[layout.window] = layout;
        // The daemon's word on these tabs is newer than what was reported for them.
        for (auto const& tab: layout.tabs)
            _reportedPresentations.erase(tab.tabId);
        // A layout push follows a daemon-side re-projection, which discards the per-pane grids this
        // client reported (@see vthost::SessionHost::applyPaneSize). Forget what was sent, so the
        // next report re-asserts all of it instead of being filtered out as "unchanged" — otherwise
        // a pane the daemon resized underneath us (a tab whose divider the user dragged, re-projected
        // by a split in a DIFFERENT tab) would keep the daemon's ratio-derived estimate.
        //
        // Only the bookkeeping is dropped, never a report issued: at this point the panes have not
        // been laid out against the new tree, so composing an area would mix a stale pane extent
        // with a fresh one and propose one that neither describes.
        auto const geometryLock = std::lock_guard { _geometryMutex };
        _lastReportedArea.reset();
        _lastReportedPaneSizes.clear();
    }
    replayUnsentCloses(layout);
    if (!lost.empty())
        emit windowsLost(std::move(lost));
    // The GUI (on its own thread, via a queued connection) reconciles each daemon
    // window's tab/split tree against wireLayout(window) — the authoritative
    // structure — mapping one OS window to each (B4). (The per-session snapshot
    // deltas that follow set State::Ready via onUpdate.)
    emit layoutChanged();
}

std::vector<uint64_t> NativeController::windowIds() const
{
    auto const lock = std::lock_guard { _mutex };
    // std::map keeps the keys ascending, so the primary (lowest-id) window comes first.
    return std::ranges::to<std::vector>(_layouts | std::views::keys);
}

std::optional<vthost::proto::LayoutState> NativeController::layout(uint64_t daemonWindow) const
{
    auto const lock = std::lock_guard { _mutex };
    auto const it = _layouts.find(daemonWindow);
    return it != _layouts.end() ? std::optional { it->second } : std::nullopt;
}

std::optional<vthost::proto::LayoutState> NativeController::layout() const
{
    auto const lock = std::lock_guard { _mutex };
    if (_layouts.empty())
        return std::nullopt;
    return _layouts.begin()->second; // the primary (lowest-id) window
}

vthost::client::WireLayout NativeController::wireLayout() const
{
    auto const lock = std::lock_guard { _mutex };
    return _layouts.empty() ? vthost::client::WireLayout {}
                            : vthost::client::wireToLayout(_layouts.begin()->second);
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
    // The acting pane names the WINDOW: the daemon hosts several (B4), and a request naming none
    // lands in its first one — so a "+" clicked in a second window opened a tab in the first. An
    // unbound (or null) pty resolves to 0, which is exactly that fallback.
    auto const beside = actingPty != nullptr ? sessionForPty(actingPty).value_or(0) : 0;
    // The client's send verbs must run on the reactor thread that owns it.
    postSessionCreation([beside, profile = spawn.profileName](NativeClient& client) mutable {
        client.createTab(beside, std::move(profile));
    });
}

void NativeController::requestCreateWindow(contour::session::RemoteSpawn const& spawn)
{
    postSessionCreation([profile = spawn.profileName](NativeClient& client) mutable {
        client.createWindow(std::move(profile));
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
            for (auto const [index, tab]: core::views::enumerate(layout.tabs))
                if (vthost::client::paneTreeHosts(tab.root, *active))
                {
                    auto const [reported, fresh] = _reportedPresentations.try_emplace(
                        tab.tabId,
                        vthost::client::presentationOf(tab,
                                                       std::cmp_equal(index, layout.activeTab)
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

void NativeController::answerPermission(std::string hostId, std::string digest, vthost::GrantState answer)
{
    _reactor.post([this, hostId = std::move(hostId), digest = std::move(digest), answer]() mutable {
        if (_client != nullptr)
            _client->answerPermission(std::move(hostId), std::move(digest), answer);
    });
}

void NativeController::reportPaneGeometry(uint64_t session, uint64_t serial, vtpty::PageSize cells)
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
        _geometryAnchor = session;
        if (_geometryFlushScheduled)
            return; // a flush is already queued; it will pick this up too
        _geometryFlushScheduled = true;
    }
    // Queued onto the GUI thread: it coalesces every pane resized in this layout pass into one
    // report, and it is what makes the sink safe to call from the parser thread (DECCOLM resizes
    // there). A metacall posted to a QObject is dropped if the object dies first.
    platform::postToObject(this, [this] { flushGeometry(); });
}

void NativeController::flushGeometry()
{
    auto area = std::optional<vtpty::PageSize> {};
    auto panes = std::vector<std::pair<uint64_t, vtpty::PageSize>> {};
    {
        // Both, in the documented order: composedClientArea() reads the layouts (_mutex) and the
        // pane grids (_geometryMutex).
        auto const lock = std::lock_guard { _mutex };
        auto const geometryLock = std::lock_guard { _geometryMutex };
        _geometryFlushScheduled = false;

        if (auto const composed = composedClientArea(); composed && _lastReportedArea != composed)
        {
            _lastReportedArea = composed;
            area = composed;
        }

        // A new client area re-projects every pane on the daemon, so each must be re-asserted then
        // — not just the ones that moved here.
        auto const resendAll = area.has_value();
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

    _reactor.post([this, area, panes = std::move(panes)] {
        if (_client == nullptr)
            return;
        // The client area first: it re-projects the tree, and the per-pane grids refine THAT.
        if (area.has_value())
            _client->requestResize(unbox<uint32_t>(area->columns), unbox<uint32_t>(area->lines));
        for (auto const& [session, size]: panes)
            _client->resizePane(session, unbox<uint32_t>(size.columns), unbox<uint32_t>(size.lines));
    });
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

    // Every mirrored tab, in window-id then tab order — `_layouts` is ordered, so the fallback the
    // composer picks is deterministic rather than a hash-order accident.
    auto const roots =
        _layouts | std::views::values
        | std::views::transform([](auto const& layout) -> auto const& { return layout.tabs; })
        | std::views::join
        | std::views::transform([](auto const& tab) -> vthost::proto::WirePane const& { return tab.root; });
    return vthost::client::composeAnchoredClientArea(roots, _geometryAnchor, resolve);
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
    auto const lock = std::lock_guard { _mutex };
    auto const binding = _bindings.find(session);
    // A detached binding's pty: the session id, if bound at all, is a new session's now.
    if (binding == _bindings.end() || binding->second.serial != serial)
        return;
    _bindings.erase(binding);
    _inputEchoes.erase(session); // a session id is never reused within a run
    // The pane is gone: its grid must not keep contributing to a composed client area, and the
    // sent-size bookkeeping for it is meaningless (a session id is never reused).
    {
        auto const geometryLock = std::lock_guard { _geometryMutex };
        _paneSizes.erase(session);
        _paneSerials.erase(session);
        _lastReportedPaneSizes.erase(session);
        // The anchor is the third thing this pane owned: it selects WHICH tab the client area is
        // composed from, and a session id is never reused, so once the pane is gone the anchor can
        // only ever miss. Left set, a flush already queued when this ran would compose the fallback
        // tab while still believing it had anchored — reporting a client area for a window the user
        // never touched, which the daemon answers by re-projecting every pane of every window.
        if (_geometryAnchor == session)
            _geometryAnchor.reset();
    }
    // Tombstone while the connection lives, so a remote session that outlives its local pane cannot
    // resurrect it through a later delta. Whether that session should also be ENDED on the daemon is
    // NOT knowable here — a pty is destroyed by a pane close, a window close, an app quit and a lost
    // connection alike — so the close is authored by the caller that knows, through
    // SessionFactory::requestRemoteClose. Inferring it from this destructor is what made closing a
    // window destroy the user's shells.
    if (!_stopped)
        _closedSessions.insert(session);
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
    return _realizingLayout || !_pending.empty();
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
        [this, session, serial](std::string_view bytes) {
            // Typed while no connection is up: dropped here, never queued for whatever answers next.
            auto const connection = _inputConnection.load();
            if (connection == 0)
            {
                _droppedInputBytes += bytes.size();
                return;
            }
            _reactor.post([this, session, serial, connection, copy = std::string { bytes }] {
                // Lost meanwhile, or this pane's binding was detached and its session id is another's now.
                if (_client == nullptr || _inputConnection.load() != connection
                    || !isBinding(session, serial))
                {
                    _droppedInputBytes += copy.size();
                    return;
                }
                _client->sendInput(session, copy);
            });
        },
        // A pane's grid is NOT the client area: reporting it as one made the daemon project the
        // whole tab into a single pane's width, halving every pane on every split. The report is
        // composed from all panes instead — see reportPaneGeometry.
        [this, session, serial](vtpty::PageSize cells, std::optional<vtpty::ImageSize> /*pixels*/) {
            reportPaneGeometry(session, serial, cells);
        },
        [this, session, serial] { unbind(session, serial); });
    _bindings[session] = Binding { .serial = serial, .pty = pty.get(), .terminal = nullptr, .mirror = {} };
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

} // namespace contour::remote
