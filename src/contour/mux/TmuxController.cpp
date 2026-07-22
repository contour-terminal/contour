// SPDX-License-Identifier: Apache-2.0
#include <contour/TerminalSession.h>
#include <contour/TerminalSessionManager.h>
#include <contour/mux/TmuxController.h>

#include <format>
#include <utility>

#include <muxserver/tmux/ControlModeSpawn.h>

namespace contour
{

namespace
{
    auto const tmuxLog = logstore::category("gui.tmux", "GUI tmux -CC mirroring controller.");

    /// Finds the orientation of the split joining @p pane into @p node's tree.
    [[nodiscard]] std::optional<vtmux::SplitState> parentOrientationOf(
        muxserver::tmux::BinaryLayout const& node, uint64_t pane)
    {
        if (node.first == nullptr || node.second == nullptr)
            return std::nullopt;
        for (auto const* child: { node.first.get(), node.second.get() })
            if (child->paneId == pane)
                return node.orientation;
        for (auto const* child: { node.first.get(), node.second.get() })
            if (auto const found = parentOrientationOf(*child, pane))
                return found;
        return std::nullopt;
    }
} // namespace

/// The model-owned pane sink: buffers bytes until a local pty is bound, then
/// feeds it directly. Destroyed by the model on the reactor thread; the
/// destructor unregisters so the controller never touches a dead feed.
class TmuxController::PaneFeed final: public muxserver::tmux::PaneSink
{
  public:
    PaneFeed(TmuxController& controller, uint64_t pane): _controller(controller), _pane(pane) {}

    ~PaneFeed() override { _controller.dropFeed(_pane); }

    PaneFeed(PaneFeed const&) = delete;
    PaneFeed& operator=(PaneFeed const&) = delete;
    PaneFeed(PaneFeed&&) = delete;
    PaneFeed& operator=(PaneFeed&&) = delete;

    void feed(std::string_view bytes) override
    {
        auto const lock = std::lock_guard { _mutex };
        if (_pty != nullptr)
            _pty->feed(bytes);
        else
            _buffered.append(bytes);
    }

    void resize(int /*columns*/, int /*lines*/) override
    {
        // The layout extent lands via the GUI's own pane geometry; the local
        // grid follows the display, and resize-pane proposes it upstream.
    }

    /// Binds @p pty and flushes everything buffered so far (replay bytes).
    void attach(vtpty::ChannelPty* pty)
    {
        auto const lock = std::lock_guard { _mutex };
        _pty = pty;
        if (_pty != nullptr && !_buffered.empty())
        {
            _pty->feed(_buffered);
            _buffered.clear();
        }
    }

  private:
    TmuxController& _controller;
    uint64_t _pane;
    std::mutex _mutex;
    vtpty::ChannelPty* _pty = nullptr;
    std::string _buffered;
};

TmuxController::TmuxController(std::string tmuxSocket): _tmuxSocket(std::move(tmuxSocket))
{
    _model.setPaneSinkFactory([this](uint64_t pane, int /*columns*/, int /*lines*/) {
        auto feed = std::make_unique<PaneFeed>(*this, pane);
        {
            auto const lock = std::lock_guard { _mutex };
            _feeds[pane] = feed.get();
        }
        return feed;
    });
    _model.subscribe(this);
}

TmuxController::~TmuxController()
{
    stop();
}

std::expected<void, std::string> TmuxController::connectAndWait(std::chrono::milliseconds timeout)
{
    _reactor.start([this](net::EventLoop* loop) { return runClient(loop); });

    auto const outcome = awaitMuxConnect(_mutex, _connected, _state, _failure, timeout);
    if (outcome.timedOut)
    {
        stop();
        return std::unexpected("timed out waiting for the tmux session's first pane");
    }
    if (!outcome.ready)
        return std::unexpected(outcome.failure.empty() ? std::string("tmux client ended during attach")
                                                       : outcome.failure);
    return {};
}

void TmuxController::stop()
{
    if (stopMuxReactor(_mutex, _stopped, _reactor, [this] {
            if (_gateway != nullptr)
                _gateway->detach();
        }))
        closeAllPanes();
}

coro::Task<void> TmuxController::runClient(net::EventLoop* loop)
{
    auto spawned = muxserver::tmux::spawnControlMode(*loop, _tmuxSocket);
    if (!spawned)
    {
        {
            auto const lock = std::lock_guard { _mutex };
            _state = State::Failed;
            _failure = spawned.error();
        }
        _connected.notify_all();
        emit connectionClosed();
        co_return;
    }
    _tmuxPid = spawned->pid;

    auto gateway = muxserver::tmux::TmuxGateway { *loop, std::move(spawned->transport), _model };
    _model.bind(gateway);
    {
        auto const lock = std::lock_guard { _mutex };
        _gateway = &gateway;
    }

    try
    {
        co_await gateway.run();
    }
    catch (coro::OperationCancelled const&)
    {
        tmuxLog()("tmux serve loop cancelled by stop().");
    }

    {
        auto const lock = std::lock_guard { _mutex };
        _gateway = nullptr;
        if (_state == State::Connecting)
        {
            _state = State::Failed;
            _failure = "tmux exited before a pane was discovered (is a tmux server running?)";
        }
        else if (_state == State::Ready)
            _state = State::Closed;
    }
    _connected.notify_all();
    muxserver::tmux::reapControlMode(std::exchange(_tmuxPid, -1));
    emit connectionClosed();
}

void TmuxController::paneAdded(uint64_t window, uint64_t pane, int columns, int lines)
{
    auto record = PendingPane { .window = window, .pane = pane, .columns = columns, .lines = lines };
    if (auto const it = _model.windows().find(window); it != _model.windows().end() && it->second.tree)
        if (auto const orientation = parentOrientationOf(*it->second.tree, pane))
            record.vertical = *orientation == vtmux::SplitState::Vertical;

    {
        auto const lock = std::lock_guard { _mutex };
        _pending.push_back(record);
        if (_state == State::Connecting)
            _state = State::Ready;
    }
    _connected.notify_all();
    emit remotePaneDiscovered();
}

void TmuxController::paneRemoved(uint64_t /*window*/, uint64_t pane)
{
    // Close under the lock, like closeAllPanes(): ~BoundPanePty (GUI thread) unbinds
    // through the same _mutex, so holding it here blocks that destructor until close()
    // returns — the pty cannot be freed mid-close. Releasing the lock first would
    // reintroduce the use-after-free race between the reactor and GUI threads.
    auto const lock = std::lock_guard { _mutex };
    std::erase_if(_pending, [pane](PendingPane const& pending) { return pending.pane == pane; });
    if (auto const it = _ptys.find(pane); it != _ptys.end())
        it->second->close(); // the session sees EOF; the manager prunes its pane/tab
}

void TmuxController::exited(std::string const& reason)
{
    tmuxLog()("tmux control mode exited: {}", reason);
}

void TmuxController::unbindPane(uint64_t pane)
{
    auto const lock = std::lock_guard { _mutex };
    _ptys.erase(pane);
    if (auto const it = _feeds.find(pane); it != _feeds.end())
        it->second->attach(nullptr);
}

void TmuxController::dropFeed(uint64_t pane)
{
    auto const lock = std::lock_guard { _mutex };
    _feeds.erase(pane);
}

void TmuxController::closeAllPanes()
{
    auto const lock = std::lock_guard { _mutex };
    for (auto& [pane, pty]: _ptys)
        pty->close();
}

bool TmuxController::canCreateSession() const noexcept
{
    auto const lock = std::lock_guard { _mutex };
    return !_pending.empty();
}

std::unique_ptr<vtpty::Pty> TmuxController::createPty(std::optional<std::string> /*cwd*/,
                                                      std::optional<vtbackend::PageSize> pageSize,
                                                      std::optional<vtpty::Process::ExecInfo> /*command*/,
                                                      std::optional<std::string> /*profileName*/)
{
    auto lock = std::unique_lock { _mutex };
    if (_pending.empty())
    {
        tmuxLog()("No pending tmux pane; handing out an unbound pty.");
        return makeUnboundFallbackPty(pageSize);
    }

    auto const record = _pending.front();
    _pending.pop_front();

    auto pty = std::make_unique<SelfUnbindingChannelPty>(
        vtpty::PageSize { vtpty::LineCount(record.lines), vtpty::ColumnCount(record.columns) },
        [this, pane = record.pane] { unbindPane(pane); });
    pty->setWriteSink([this, pane = record.pane](std::string_view bytes) {
        _reactor.post([this, pane, copy = std::string { bytes }] {
            if (_gateway != nullptr)
                _gateway->sendRawInput(pane, copy);
        });
    });
    pty->setResizeSink(
        [this, pane = record.pane](vtpty::PageSize cells, std::optional<vtpty::ImageSize> /*pixels*/) {
            _reactor.post([this, pane, cells] {
                if (_gateway == nullptr)
                    return;
                _gateway->sendCommand(std::format(
                    "resize-pane -t %{} -x {} -y {}", pane, unbox(cells.columns), unbox(cells.lines)));
                // A single-pane window follows the client area instead.
                for (auto const& [window, view]: _model.windows())
                    if (view.panes.size() == 1 && view.panes.front() == pane)
                        _gateway->sendCommand(
                            std::format("refresh-client -C {}x{}", unbox(cells.columns), unbox(cells.lines)));
            });
        });
    _ptys[record.pane] = pty.get();
    auto* feed = [&]() -> PaneFeed* {
        auto const it = _feeds.find(record.pane);
        return it != _feeds.end() ? it->second : nullptr;
    }();
    lock.unlock();

    if (feed != nullptr)
        feed->attach(pty.get());
    tmuxLog()("Bound tmux pane %{} to a new local pty ({}x{}).", record.pane, record.columns, record.lines);
    return pty;
}

void TmuxController::adoptPendingPanes(TerminalSessionManager& manager, vtmux::WindowId window)
{
    while (true)
    {
        auto record = PendingPane {};
        {
            auto const lock = std::lock_guard { _mutex };
            if (_pending.empty())
                return;
            record = _pending.front(); // consumed by createPty inside the calls below
        }

        auto* acting = [&]() -> TerminalSession* {
            auto const lock = std::lock_guard { _mutex };
            auto const it = _actingByWindow.find(record.window);
            return it != _actingByWindow.end() ? it->second : nullptr;
        }();

        auto* created = static_cast<TerminalSession*>(nullptr);
        if (acting == nullptr)
            created = manager.createSessionInBackground(window);
        else
        {
            manager.splitActivePane(record.vertical, acting);
            created = acting; // the anchor stays valid for further splits
        }

        auto const lock = std::lock_guard { _mutex };
        // The manager consumes the pending pane through createPty; if it did
        // not (creation refused or failed), stop instead of spinning.
        if (!_pending.empty() && _pending.front().pane == record.pane)
            return;
        if (created != nullptr)
            _actingByWindow[record.window] = created;
    }
}

} // namespace contour
