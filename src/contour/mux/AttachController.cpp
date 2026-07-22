// SPDX-License-Identifier: Apache-2.0
#include <contour/TerminalSessionManager.h>
#include <contour/mux/AttachController.h>

#include <algorithm>
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

std::expected<void, std::string> AttachController::connectAndWait(std::chrono::milliseconds timeout)
{
    _reactor.start([this](net::EventLoop* loop) { return runClient(loop); });

    auto const outcome = awaitMuxConnect(_mutex, _connected, _state, _failure, timeout);
    if (outcome.timedOut)
    {
        stop();
        return std::unexpected("timed out waiting for the daemon's snapshot");
    }
    if (!outcome.ready)
        return std::unexpected(outcome.failure.empty() ? std::string("connection closed during attach")
                                                       : outcome.failure);
    return {};
}

void AttachController::stop()
{
    if (stopMuxReactor(_mutex, _stopped, _reactor, [this] {
            if (_client != nullptr)
                _client->detach();
        }))
        closeAllBindings();
}

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
        _layout = layout;
    }
    // The GUI (on its own thread, via a queued connection) reconciles its tab and
    // split tree against wireLayout() — the daemon's authoritative structure. (The
    // per-session snapshot deltas that follow set State::Ready via onUpdate.)
    emit layoutChanged();
}

std::optional<muxserver::proto::LayoutState> AttachController::layout() const
{
    auto const lock = std::lock_guard { _mutex };
    return _layout;
}

muxserver::client::WireLayout AttachController::wireLayout() const
{
    auto const lock = std::lock_guard { _mutex };
    return _layout ? muxserver::client::wireToLayout(*_layout) : muxserver::client::WireLayout {};
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

void AttachController::requestSplitPane(vtpty::Pty const* actingPty, bool vertical)
{
    auto const session = sessionForPty(actingPty);
    if (!session)
        return;
    auto const orientation = static_cast<uint8_t>(vertical ? 2 : 1); // vtmux::SplitState
    _reactor.post([this, session = *session, orientation] {
        if (_client != nullptr)
            _client->splitPane(session, orientation, 5000);
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
    auto const lock = std::lock_guard { _mutex };
    _bindings.erase(session);

    // A pty destroyed while the connection is still live means the user closed
    // that one tab: tombstone the id so its still-running remote session cannot
    // resurrect the tab through a later delta. During teardown (`_stopped`) the
    // whole connection is ending, so tombstoning would be pointless — and it is
    // exactly the detach path that must NOT tombstone every session.
    if (!_stopped)
        _closedSessions.insert(session);
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
    auto columns = 0;
    auto lines = 0;
    if (_nextBindSession.has_value())
    {
        // Layout executor: bind this pane to the remote session the beforeLeafSeed
        // hook named. Its screen may not have arrived yet (the layout leads the
        // snapshot), so the pane is born at the window's size and the mirror's first
        // replay (on binding) repaints it; a resize then reconciles.
        session = *_nextBindSession;
        _nextBindSession.reset();
        auto const size = pageSize.value_or(vtbackend::PageSize {});
        columns = size.columns.value != 0 ? unbox<int>(size.columns) : 80;
        lines = size.lines.value != 0 ? unbox<int>(size.lines) : 25;
    }
    else if (!_pending.empty())
    {
        auto const front = _pending.front();
        _pending.pop_front();
        session = front.session;
        columns = static_cast<int>(front.columns);
        lines = static_cast<int>(front.lines);
    }
    else
    {
        // The creation guards should have prevented this; a session must
        // still be born, so give it a dead-end pty it can close cleanly.
        attachLog()("No pending remote session; handing out an unbound pty.");
        return makeUnboundFallbackPty(pageSize);
    }

    // Born at the REMOTE size so the mirror's first replay paints a matching
    // grid; the display's own resize then proposes the local size upstream.
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
