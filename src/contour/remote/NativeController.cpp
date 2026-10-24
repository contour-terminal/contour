// SPDX-License-Identifier: Apache-2.0
#include <contour/platform/QtInvoke.hpp>
#include <contour/remote/NativeController.hpp>
#include <contour/session/TerminalSessionManager.hpp>

#include <core/Utils.hpp>
#include <core/net/Sockets.hpp>

#include <algorithm>
#include <ranges>
#include <utility>

#include <vthost/HostIdentity.hpp>

namespace contour::remote
{

using vthost::client::NativeClient;
using vthost::client::RemoteScreen;

namespace
{
    auto const attachLog = core::log::Category("gui.attach", "GUI native-attach controller.");
} // namespace

NativeController::NativeController(vthost::AttachEndpoint endpoint,
                                   std::optional<vtbackend::Settings> attachSettings,
                                   SessionSettingsSource sessionSettings):
    _endpoint(std::move(endpoint)),
    _attachSettings(std::move(attachSettings)),
    _sessionSettings(std::move(sessionSettings))
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

// connectAndWait() and stop() are provided by RemoteController; this controller supplies runClient()
// and the detach / binding-teardown / message hooks (see NativeController.h).

core::async::Task<void> NativeController::runClient(core::net::EventLoop* loop)
{
    auto const token = vthost::endpointToken(_endpoint);
    auto socket = co_await vthost::connectAttach(loop, _endpoint);
    if (!socket)
    {
        {
            auto const lock = std::lock_guard { _mutex };
            _state = State::Failed;
            _failure = socket.error();
        }
        _connected.notify_all();
        emit connectionClosed();
        co_return;
    }

    auto client = NativeClient {
        *loop,
        std::move(*socket),
        NativeClient::HandshakeOptions {
            .token = token,
            // The daemon applies it to the sessions THIS connection goes on to create, and to nothing
            // that already exists; postSessionCreation() restates it when the configuration changed.
            .sessionSettings =
                _attachSettings.transform([](auto const& s) { return vthost::toWireSessionSettings(s); }),
            // Offered because this client passes a PermissionHandler below: its panes ask the user.
            .capabilities =
                vthost::proto::CapabilitySet::of({ vthost::proto::Capability::ExtensionPermissions }),
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
            [this](vthost::proto::PermissionRequest const& request) { onPermissionRequest(request); } }
    };
    {
        auto const lock = std::lock_guard { _mutex };
        _client = &client;
    }
    // Forgotten on EVERY exit, not only the epilogue's. An exception other than cancellation
    // unwinds straight past that epilogue (the reactor thread catches it), and `client` is a local
    // — _client would then point at a destroyed object for as long as the controller lives.
    auto const forgetClient = core::Finally { [this] {
        auto const lock = std::lock_guard { _mutex };
        _client = nullptr;
    } };

    try
    {
        co_await client.run();
    }
    catch (core::async::OperationCancelled const&)
    {
        // stop() cancelled the loop mid-serve; fall through to the normal
        // bookkeeping so state and observers still see the closure.
        attachLog()("Attach serve loop cancelled by stop().");
    }

    {
        // _client is cleared by `forgetClient` above, on this path and on every other.
        auto const lock = std::lock_guard { _mutex };
        if (_state == State::Connecting || _state == State::Ready)
        {
            if (auto const& refusal = client.refusal())
            {
                _state = State::Failed;
                _failure = vthost::proto::describe(*refusal, vthost::proto::PeerRole::Daemon);
            }
            else
                _state = State::Closed;
        }
    }
    _connected.notify_all();
    emit connectionClosed();
}

void NativeController::onUpdate(RemoteScreen const& screen, vthost::proto::Delta const& delta)
{
    auto lock = std::unique_lock { _mutex };

    // Ordered by precedence: a live binding always consumes the delta, a
    // tombstoned session is ignored, a pending one refreshes its geometry, and
    // only a never-seen session is adopted as a new tab.
    if (auto const binding = _bindings.find(screen.session); binding != _bindings.end())
    {
        // The mirror is absent only in the window between createPty() and the priming bindTerminal()
        // posts; that replay picks the session up from whatever arrived meanwhile.
        if (binding->second.mirror)
            binding->second.mirror->apply(screen, delta);
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
    {
        auto const lock = std::lock_guard { _mutex };
        _layouts[layout.window] = layout;
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

void NativeController::requestCreateTab(vtpty::Pty const* actingPty)
{
    // The acting pane names the WINDOW: the daemon hosts several (B4), and a request naming none
    // lands in its first one — so a "+" clicked in a second window opened a tab in the first. An
    // unbound (or null) pty resolves to 0, which is exactly that fallback.
    auto const beside = actingPty != nullptr ? sessionForPty(actingPty).value_or(0) : 0;
    // The client's send verbs must run on the reactor thread that owns it.
    postSessionCreation([beside](NativeClient& client) { client.createTab(beside); });
}

void NativeController::requestCreateWindow()
{
    postSessionCreation([](NativeClient& client) { client.createWindow(); });
}

void NativeController::requestSplitPane(vtpty::Pty const* actingPty, bool vertical)
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
    postSessionCreation([session = *session, orientation, ratio = EvenSplitRatio](NativeClient& client) {
        client.splitPane(session, orientation, ratio);
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

void NativeController::requestClosePane(uint64_t session)
{
    _reactor.post([this, session] {
        if (_client != nullptr)
            _client->closePane(session);
    });
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

void NativeController::reportPaneGeometry(uint64_t session, vtpty::PageSize cells)
{
    {
        // _geometryMutex alone: this runs from the pty's resize sink, which the mirror reaches while
        // _mutex is held. Taking _mutex here would deadlock the reactor against itself.
        auto const lock = std::lock_guard { _geometryMutex };
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
}

void NativeController::bindTerminal(vtpty::Pty const* pty, vtbackend::Terminal& terminal)
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

void NativeController::unbind(uint64_t session)
{
    auto const lock = std::lock_guard { _mutex };
    _bindings.erase(session);
    // The pane is gone: its grid must not keep contributing to a composed client area, and the
    // sent-size bookkeeping for it is meaningless (a session id is never reused).
    {
        auto const geometryLock = std::lock_guard { _geometryMutex };
        _paneSizes.erase(session);
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

    auto pty = std::make_unique<SelfUnbindingChannelPty>(
        vtpty::PageSize { vtpty::LineCount(lines), vtpty::ColumnCount(columns) },
        [this, session](std::string_view bytes) {
            _reactor.post([this, session, copy = std::string { bytes }] {
                if (_client != nullptr)
                    _client->sendInput(session, copy);
            });
        },
        // A pane's grid is NOT the client area: reporting it as one made the daemon project the
        // whole tab into a single pane's width, halving every pane on every split. The report is
        // composed from all panes instead — see reportPaneGeometry.
        [this, session](vtpty::PageSize cells, std::optional<vtpty::ImageSize> /*pixels*/) {
            reportPaneGeometry(session, cells);
        },
        [this, session] { unbind(session); });
    _bindings[session].pty = pty.get();
    // The birth size is this pane's first known grid, so the composition is complete from the
    // moment the pane exists rather than only once something resizes it.
    {
        auto const geometryLock = std::lock_guard { _geometryMutex };
        _paneSizes.insert_or_assign(
            session,
            vtpty::PageSize { .lines = vtpty::LineCount(lines), .columns = vtpty::ColumnCount(columns) });
    }
    lock.unlock();

    // No priming here: it needs the terminal, which does not exist until bindTerminal() announces
    // it (the pty is a constructor argument to the session that owns that terminal).
    attachLog()("Bound remote session {} to a new local pty ({}x{}).", session, columns, lines);
    return pty;
}

} // namespace contour::remote
