// SPDX-License-Identifier: Apache-2.0
#include <contour/TerminalSessionManager.h>
#include <contour/mux/AttachController.h>

#include <algorithm>
#include <ranges>
#include <utility>

#include <net/Sockets.h>

namespace contour
{

using muxserver::client::AttachClient;
using muxserver::client::RemoteScreen;

namespace
{
    auto const attachLog = logstore::category("gui.attach", "GUI native-attach controller.");
} // namespace

AttachController::AttachController(muxserver::AttachEndpoint endpoint): _endpoint(std::move(endpoint))
{
}

AttachController::~AttachController()
{
    stop();
}

// connectAndWait() and stop() are provided by MuxControllerBase; this controller supplies runClient()
// and the detach / binding-teardown / message hooks (see AttachController.h).

coro::Task<void> AttachController::runClient(net::EventLoop* loop)
{
    auto const token = muxserver::endpointToken(_endpoint);
    auto socket = co_await muxserver::connectAttach(loop, _endpoint);
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

    auto client = AttachClient { *loop, std::move(*socket), token };
    client.setUpdateHandler([this](RemoteScreen const& screen, muxserver::proto::Delta const& delta) {
        onUpdate(screen, delta);
    });
    client.setLayoutHandler([this](muxserver::proto::LayoutState const& layout) { onLayout(layout); });
    {
        auto const lock = std::lock_guard { _mutex };
        _client = &client;
    }

    try
    {
        co_await client.run();
    }
    catch (coro::OperationCancelled const&)
    {
        // stop() cancelled the loop mid-serve; fall through to the normal
        // bookkeeping so state and observers still see the closure.
        attachLog()("Attach serve loop cancelled by stop().");
    }

    {
        auto const lock = std::lock_guard { _mutex };
        _client = nullptr;
        if (_state == State::Connecting || _state == State::Ready)
        {
            if (client.versionMismatch())
            {
                _state = State::Failed;
                _failure = "daemon speaks an incompatible protocol version";
            }
            else
                _state = State::Closed;
        }
    }
    _connected.notify_all();
    emit connectionClosed();
}

void AttachController::onUpdate(RemoteScreen const& screen, muxserver::proto::Delta const& delta)
{
    auto lock = std::unique_lock { _mutex };

    // Ordered by precedence: a live binding always consumes the delta, a
    // tombstoned session is ignored, a pending one refreshes its geometry, and
    // only a never-seen session is adopted as a new tab.
    if (auto const binding = _bindings.find(screen.session); binding != _bindings.end())
    {
        binding->second.pty->feed(binding->second.mirror.apply(screen, delta));
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

void AttachController::onLayout(muxserver::proto::LayoutState const& layout)
{
    {
        auto const lock = std::lock_guard { _mutex };
        _layouts[layout.window] = layout;
    }
    // The GUI (on its own thread, via a queued connection) reconciles each daemon
    // window's tab/split tree against wireLayout(window) — the authoritative
    // structure — mapping one OS window to each (B4). (The per-session snapshot
    // deltas that follow set State::Ready via onUpdate.)
    emit layoutChanged();
}

std::vector<uint64_t> AttachController::windowIds() const
{
    auto const lock = std::lock_guard { _mutex };
    // std::map keeps the keys ascending, so the primary (lowest-id) window comes first.
    return std::ranges::to<std::vector>(_layouts | std::views::keys);
}

std::optional<muxserver::proto::LayoutState> AttachController::layout(uint64_t daemonWindow) const
{
    auto const lock = std::lock_guard { _mutex };
    auto const it = _layouts.find(daemonWindow);
    return it != _layouts.end() ? std::optional { it->second } : std::nullopt;
}

std::optional<muxserver::proto::LayoutState> AttachController::layout() const
{
    auto const lock = std::lock_guard { _mutex };
    if (_layouts.empty())
        return std::nullopt;
    return _layouts.begin()->second; // the primary (lowest-id) window
}

muxserver::client::WireLayout AttachController::wireLayout() const
{
    auto const lock = std::lock_guard { _mutex };
    return _layouts.empty() ? muxserver::client::WireLayout {}
                            : muxserver::client::wireToLayout(_layouts.begin()->second);
}

void AttachController::setNextBindSession(uint64_t session)
{
    auto const lock = std::lock_guard { _mutex };
    _nextBindSession = session;
}

void AttachController::setRealizingLayout(bool realizing)
{
    auto const lock = std::lock_guard { _mutex };
    _realizingLayout = realizing;
}

bool AttachController::isRealizingLayout() const
{
    auto const lock = std::lock_guard { _mutex };
    return _realizingLayout;
}

bool AttachController::isBound(uint64_t session) const
{
    auto const lock = std::lock_guard { _mutex };
    return _bindings.contains(session);
}

bool AttachController::isClaimed(uint64_t session) const
{
    auto const lock = std::lock_guard { _mutex };
    return _bindings.contains(session) || _closedSessions.contains(session);
}

std::optional<uint64_t> AttachController::sessionForPty(vtpty::Pty const* pty) const
{
    auto const lock = std::lock_guard { _mutex };
    for (auto const& [session, binding]: _bindings)
        if (binding.pty == pty)
            return session;
    return std::nullopt;
}

void AttachController::requestCreateTab()
{
    // The client's send verbs must run on the reactor thread that owns it.
    _reactor.post([this] {
        if (_client != nullptr)
            _client->createTab();
    });
}

void AttachController::requestCreateWindow()
{
    _reactor.post([this] {
        if (_client != nullptr)
            _client->createWindow();
    });
}

void AttachController::requestSplitPane(vtpty::Pty const* actingPty, bool vertical)
{
    auto const session = sessionForPty(actingPty);
    if (!session)
        return;
    auto const orientation = static_cast<uint8_t>(
        std::to_underlying(vertical ? vtworkspace::SplitState::Vertical : vtworkspace::SplitState::Horizontal));
    // 5000 = 0.5 × 10000, the wire encoding for an even split (see proto/Pdu.h).
    constexpr uint16_t EvenSplitRatio = 5000;
    _reactor.post([this, session = *session, orientation] {
        if (_client != nullptr)
            _client->splitPane(session, orientation, EvenSplitRatio);
    });
}

void AttachController::requestClosePane(uint64_t session)
{
    _reactor.post([this, session] {
        if (_client != nullptr)
            _client->closePane(session);
    });
}

void AttachController::primeBinding(uint64_t session)
{
    if (_client == nullptr)
        return;
    auto const screen = _client->screens().find(session);
    if (screen == _client->screens().end())
        return;

    auto const lock = std::lock_guard { _mutex };
    auto const binding = _bindings.find(session);
    if (binding == _bindings.end())
        return;
    binding->second.pty->feed(binding->second.mirror.fullReplay(screen->second));
}

void AttachController::unbind(uint64_t session)
{
    auto userClosed = false;
    {
        auto const lock = std::lock_guard { _mutex };
        _bindings.erase(session);
        // A pty destroyed while the connection is still live means the user closed
        // that one pane. Tombstone the id so its still-running remote session cannot
        // resurrect the pane through a later delta before the daemon removes it.
        // During teardown (`_stopped`) the whole connection is ending, so neither the
        // tombstone nor authoring a close applies — the detach path must do neither.
        if (!_stopped)
        {
            _closedSessions.insert(session);
            userClosed = true;
        }
    }
    // Author the close on the daemon so the remote session is actually destroyed —
    // otherwise it lingers headless. (Post is thread-safe; done outside the lock.)
    if (userClosed)
        requestClosePane(session);
}

void AttachController::closeAllBindings()
{
    auto const lock = std::lock_guard { _mutex };
    for (auto& [session, binding]: _bindings)
        binding.pty->close();
}

std::size_t AttachController::pendingCount() const
{
    auto const lock = std::lock_guard { _mutex };
    return _pending.size();
}

bool AttachController::canCreateSession() const noexcept
{
    auto const lock = std::lock_guard { _mutex };
    // During a layout realization the panes are bound by setNextBindSession (not
    // the FIFO queue), so allow creation even with nothing pending.
    return _realizingLayout || !_pending.empty();
}

std::unique_ptr<vtpty::Pty> AttachController::createPty(std::optional<std::string> /*cwd*/,
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
        [this, session] { unbind(session); });
    pty->setWriteSink([this, session](std::string_view bytes) {
        _reactor.post([this, session, copy = std::string { bytes }] {
            if (_client != nullptr)
                _client->sendInput(session, copy);
        });
    });
    pty->setResizeSink([this](vtpty::PageSize cells, std::optional<vtpty::ImageSize> /*pixels*/) {
        _reactor.post([this, cells] {
            if (_client != nullptr)
                _client->requestResize(unbox<uint32_t>(cells.columns), unbox<uint32_t>(cells.lines));
        });
    });
    _bindings[session].pty = pty.get();
    lock.unlock();

    _reactor.post([this, session] { primeBinding(session); });
    attachLog()("Bound remote session {} to a new local pty ({}x{}).", session, columns, lines);
    return pty;
}

} // namespace contour
