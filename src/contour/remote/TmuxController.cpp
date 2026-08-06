// SPDX-License-Identifier: Apache-2.0
#include <contour/remote/TmuxController.hpp>
#include <contour/session/TerminalSession.hpp>
#include <contour/session/TerminalSessionManager.hpp>

#include <algorithm>
#include <format>
#include <optional>
#include <ranges>
#include <utility>
#include <vector>

#include <vthost/tmux/ControlModeSpawn.hpp>
#include <vtworkspace/LayoutConvert.hpp>

namespace contour::remote
{

namespace
{
    using vthost::tmux::BinaryLayout;

    auto const tmuxLog = logstore::Category("gui.tmux", "GUI tmux -CC mirroring controller.");

    /// @return True when @p node is a split (has both children) rather than a leaf.
    [[nodiscard]] bool isSplit(BinaryLayout const& node) noexcept
    {
        return node.first != nullptr && node.second != nullptr;
    }

    /// Finds the split (orientation + first-child ratio) that joins @p pane into @p node's tree — the
    /// proportions the pane's incremental split should reproduce.
    [[nodiscard]] std::optional<std::pair<vtworkspace::SplitState, double>> parentSplitOf(
        BinaryLayout const& node, uint64_t pane)
    {
        if (!isSplit(node))
            return std::nullopt;
        for (auto const* child: { node.first.get(), node.second.get() })
            if (child->paneId == pane)
                return std::pair { node.orientation, node.ratio };
        for (auto const* child: { node.first.get(), node.second.get() })
            if (auto const found = parentSplitOf(*child, pane))
                return found;
        return std::nullopt;
    }

    /// Collects every leaf pane id of @p node into @p out (DFS order).
    void collectLeafPanes(BinaryLayout const& node, std::vector<uint64_t>& out)
    {
        if (!isSplit(node))
        {
            if (node.paneId)
                out.push_back(*node.paneId);
            return;
        }
        collectLeafPanes(*node.first, out);
        collectLeafPanes(*node.second, out);
    }

    /// Deep-copies a tmux layout tree (its children are unique_ptrs, so it is move-only otherwise).
    /// Used to snapshot a window's tree on the reactor thread for the GUI thread to realize.
    [[nodiscard]] BinaryLayout cloneBinaryLayout(BinaryLayout const& node)
    {
        auto out =
            BinaryLayout { .paneId = node.paneId, .orientation = node.orientation, .ratio = node.ratio };
        if (node.first)
            out.first = std::make_unique<BinaryLayout>(cloneBinaryLayout(*node.first));
        if (node.second)
            out.second = std::make_unique<BinaryLayout>(cloneBinaryLayout(*node.second));
        return out;
    }

    /// Adapts a tmux `BinaryLayout` for the shared layout converter (@ref vtworkspace::convertLayoutPane) —
    /// the tmux analogue of vthost::client's WirePaneAdapter, so both paths share ONE conversion.
    struct BinaryLayoutAdapter
    {
        [[nodiscard]] bool isSplit(BinaryLayout const& node) const noexcept
        {
            return node.first != nullptr && node.second != nullptr;
        }
        [[nodiscard]] vtworkspace::SplitState orientation(BinaryLayout const& node) const noexcept
        {
            return node.orientation;
        }
        [[nodiscard]] double firstRatio(BinaryLayout const& node) const noexcept { return node.ratio; }
        [[nodiscard]] BinaryLayout const& first(BinaryLayout const& node) const noexcept
        {
            return *node.first;
        }
        [[nodiscard]] BinaryLayout const& second(BinaryLayout const& node) const noexcept
        {
            return *node.second;
        }
        [[nodiscard]] uint64_t leafId(BinaryLayout const& node) const noexcept
        {
            return node.paneId.value_or(0);
        }
    };
} // namespace

TmuxWindowLayout tmuxLayoutToWindowLayout(vthost::tmux::BinaryLayout const& tree)
{
    auto result = TmuxWindowLayout {};
    auto const adapter = BinaryLayoutAdapter {};
    result.layout.tabs.push_back(
        vtworkspace::LayoutTab { .root = vtworkspace::convertLayoutPane(tree, adapter) });
    // Build the leaf → pane map only now that the tree is in place: the pane addresses are stable and
    // a move of the result preserves them (the vectors' buffers move intact).
    vtworkspace::mapLayoutLeaves(result.layout.tabs.front().root, tree, adapter, result.leafPane);
    return result;
}

std::string tmuxResumePaneCommand(uint64_t pane)
{
    return std::format("refresh-client -A %{}:continue", pane);
}

std::string tmuxSplitWindowCommand(uint64_t pane, bool vertical)
{
    // tmux: -h splits left|right (our Vertical); default stacks top|bottom (Horizontal).
    return std::format("split-window {}-t %{}", vertical ? "-h " : "", pane);
}

std::string tmuxKillPaneCommand(uint64_t pane)
{
    return std::format("kill-pane -t %{}", pane);
}

std::string tmuxNewWindowCommand()
{
    return "new-window";
}

/// The model-owned pane sink: buffers bytes until a local pty is bound, then
/// feeds it directly. Destroyed by the model on the reactor thread; the
/// destructor unregisters so the controller never touches a dead feed.
class TmuxController::PaneFeed final: public vthost::tmux::PaneSink
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

    void resize(int columns, int lines) override
    {
        // tmux is the authority on a mirrored pane's extent, so a pane not yet realized must be
        // BORN at the size tmux states NOW rather than the one its discovery reported — the
        // record it is created from is updated here.
        //
        // A pane that already holds a local pty is deliberately not resized from here, and the
        // reason is worth stating so this does not read as an omission: its grid is a function of
        // the GUI widget it is drawn in, which the display recomputes on every geometry event
        // (@see TerminalSession::resizeTerminalToDisplaySize), so a size forced in from the
        // reactor thread is undone by the next frame; and answering tmux's extent by re-proposing
        // our own would ping-pong for ever against a server that clamps it. Making tmux's geometry
        // authoritative for a realized pane means reconciling the GUI's pane tree with the tmux
        // layout — the layoutTreeChanged path, not this one.
        _controller.notePaneExtent(_pane, columns, lines);
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

TmuxController::TmuxController(std::string tmuxSocket):
    _tmuxSocket(std::move(tmuxSocket)), _model([this](uint64_t pane, int /*columns*/, int /*lines*/) {
        auto feed = std::make_unique<PaneFeed>(*this, pane);
        {
            auto const lock = std::lock_guard { _mutex };
            _feeds[pane] = feed.get();
        }
        return feed;
    })
{
    _model.subscribe(this);
}

TmuxController::~TmuxController()
{
    stop();
}

// connectAndWait() and stop() are provided by RemoteController; this controller supplies runClient()
// and the detach / binding-teardown / message hooks (see TmuxController.h).

coro::Task<void> TmuxController::runClient(net::EventLoop* loop)
{
    auto spawned = vthost::tmux::spawnControlMode(*loop, _tmuxSocket);
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

    auto gateway = vthost::tmux::TmuxGateway { *loop, std::move(spawned->transport), _model };
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
    vthost::tmux::reapControlMode(std::exchange(_tmuxPid, -1));
    emit connectionClosed();
}

TmuxController::PaneCapture TmuxController::capturePendingPane(uint64_t window,
                                                               uint64_t pane,
                                                               int columns,
                                                               int lines) const
{
    // Runs on the reactor thread, where the model is safe to read: take the pane's split
    // proportions and a snapshot of the window's whole tree, so the GUI thread can realize it
    // without racing the model.
    // Every field named, `tree` included: a designated initializer that leaves one out is a
    // -Wmissing-designated-field-initializers error under the pedantic build.
    auto capture = PaneCapture {
        .record = PendingPane { .window = window, .pane = pane, .columns = columns, .lines = lines },
        .tree = std::nullopt,
    };
    auto const it = _model.windows().find(window);
    if (it == _model.windows().end() || !it->second.tree)
        return capture;

    if (auto const split = parentSplitOf(*it->second.tree, pane))
    {
        capture.record.vertical = split->first == vtworkspace::SplitState::Vertical;
        capture.record.ratio = split->second;
    }
    capture.tree = cloneBinaryLayout(*it->second.tree);
    return capture;
}

void TmuxController::paneAdded(uint64_t window, uint64_t pane, int columns, int lines)
{
    auto [record, tree] = capturePendingPane(window, pane, columns, lines);

    {
        auto const lock = std::lock_guard { _mutex };
        _pending.push_back(record);
        if (tree)
            _pendingTrees.insert_or_assign(window, std::move(*tree)); // latest layout wins
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

void TmuxController::paneMoved(uint64_t /*fromWindow*/, uint64_t toWindow, uint64_t pane)
{
    // The destination window's tree is already in place when this fires, so the pane's NEW split
    // proportions are captured exactly as a freshly discovered pane's are.
    auto [record, tree] = capturePendingPane(toWindow, pane);

    auto realizeNow = false;
    {
        auto const lock = std::lock_guard { _mutex };
        // A pane still waiting to be realized carries its discovered size over to its new home.
        if (auto const it = std::ranges::find(_pending, pane, &PendingPane::pane); it != _pending.end())
        {
            record.columns = it->columns;
            record.lines = it->lines;
            _pending.erase(it);
        }
        if (tree)
            _pendingTrees.insert_or_assign(toWindow, std::move(*tree)); // latest layout wins

        auto const bound = _ptys.find(pane);
        if (bound == _ptys.end())
        {
            // Never realized locally: there is no binding to tear down, so the destination can
            // realize it straight away.
            _pending.push_back(record);
            realizeNow = true;
        }
        else
        {
            // The destination record waits for that unbind (@see _movedPanes): realizing against a
            // pty that is closing but still registered would drop the pane instead of moving it.
            _movedPanes.insert_or_assign(pane, record);
            bound->second->close(); // the session sees EOF; the manager prunes its pane/tab
        }
        if (_state == State::Connecting)
            _state = State::Ready;
    }
    _connected.notify_all();
    if (realizeNow)
        emit remotePaneDiscovered();
}

void TmuxController::windowRenamed(uint64_t window, std::string const& name)
{
    {
        auto const lock = std::lock_guard { _mutex };
        _pendingRenames[window] = name; // latest name wins; applied on the GUI thread
    }
    emit tabTitleChanged();
}

void TmuxController::applyPendingRenames(contour::session::TerminalSessionManager& manager)
{
    // Resolve each pending rename to its tab's session under the lock (the acting-session
    // map is shared with the reactor thread), then apply outside it — setTabTitleForSession
    // touches the GUI model and could re-enter the controller.
    auto renames = std::vector<std::pair<vtworkspace::SessionId, std::string>> {};
    {
        auto const lock = std::lock_guard { _mutex };
        for (auto it = _pendingRenames.begin(); it != _pendingRenames.end();)
        {
            auto const acting = _actingByWindow.find(it->first);
            if (acting != _actingByWindow.end())
            {
                renames.emplace_back(acting->second, std::move(it->second));
                it = _pendingRenames.erase(it);
            }
            else
                ++it; // window not realized yet; adoptPendingPanes drains it once it is
        }
    }
    for (auto& [session, name]: renames)
        manager.setTabTitleForSession(session, std::move(name));
}

void TmuxController::panePaused(uint64_t pane, bool paused)
{
    // %continue needs nothing (the pane already resumed). On %pause, resume at once: the mirror
    // consumes output as fast as it arrives, so a pause only stalls it. This fires on the reactor
    // thread that owns the gateway, so the command can be sent directly.
    if (!paused || _gateway == nullptr)
        return;
    _gateway->sendCommand(tmuxResumePaneCommand(pane));
}

void TmuxController::exited(std::string const& reason)
{
    tmuxLog()("tmux control mode exited: {}", reason);
}

void TmuxController::notePaneExtent(uint64_t pane, int columns, int lines)
{
    auto const lock = std::lock_guard { _mutex };
    auto const apply = [columns, lines](PendingPane& record) {
        record.columns = columns;
        record.lines = lines;
    };
    if (auto const it = std::ranges::find(_pending, pane, &PendingPane::pane); it != _pending.end())
        apply(*it);
    // A pane awaiting its destination realization keeps its own copy of the record (@see
    // paneMoved), so it needs the same update or the move re-creates it at a stale size.
    if (auto const moved = _movedPanes.find(pane); moved != _movedPanes.end())
        apply(moved->second);
}

std::optional<uint64_t> TmuxController::paneForPtyLocked(vtpty::Pty const* pty) const
{
    for (auto const& [id, bound]: _ptys)
        if (bound == pty)
            return id;
    return std::nullopt;
}

void TmuxController::requestRemoteClose(vtpty::Pty const* pty)
{
    auto const pane = [&] {
        auto const lock = std::lock_guard { _mutex };
        return paneForPtyLocked(pty);
    }();
    if (!pane)
        return; // not one of ours (a local session, or already unbound)
    _reactor.post([this, pane = *pane] {
        if (_gateway != nullptr)
            _gateway->sendCommand(tmuxKillPaneCommand(pane));
    });
}

void TmuxController::unbindPane(uint64_t pane)
{
    auto rediscovered = false;
    {
        auto const lock = std::lock_guard { _mutex };
        _ptys.erase(pane);
        if (auto const it = _feeds.find(pane); it != _feeds.end())
            it->second->attach(nullptr);
        // NOTHING is inferred about the user's intent here. A pty is destroyed when the user closes
        // the pane, when the window closes, when the app quits and when the connection is lost
        // alike — this destructor cannot tell them apart, and guessing "the user closed it" made
        // closing a Contour window kill-pane every mirrored pane, destroying the shells and jobs
        // running in the user's tmux session. The close is authored by the caller that knows, in
        // requestRemoteClose (@see SessionFactory::requestRemoteClose and
        // TerminalSessionManager::SessionEnd) — the same correction NativeController::unbind
        // already carries.
        //
        // A pane tmux RE-PARENTED parked its destination record instead of queueing it (@see
        // paneMoved). Now that the source binding is gone the destination can realize it: the
        // erase above is what makes realizeOnePane treat it as a pane to build rather than one
        // already realized.
        if (auto const moved = _movedPanes.find(pane); moved != _movedPanes.end())
        {
            if (!_stopped)
            {
                _pending.push_back(moved->second);
                rediscovered = true;
            }
            _movedPanes.erase(moved);
        }
    }
    if (rediscovered)
        emit remotePaneDiscovered();
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
    // During a whole-tree realization panes are bound by setNextBindPane (a leaf may have no pending
    // record yet), so allow creation even with nothing pending.
    return _nextBindPane.has_value() || !_pending.empty();
}

void TmuxController::setNextBindPane(uint64_t pane)
{
    auto const lock = std::lock_guard { _mutex };
    _nextBindPane = pane;
}

bool TmuxController::requestRemoteSplit(vtpty::Pty const* actingPty, bool vertical)
{
    auto const pane = [&]() -> std::optional<uint64_t> {
        auto const lock = std::lock_guard { _mutex };
        if (_realizing)
            return std::nullopt; // the reconciler's own split: build the mirror pane locally
        return paneForPtyLocked(actingPty);
    }();
    if (!pane)
        return false; // unknown pty or realizing → the manager splits locally
    _reactor.post([this, pane = *pane, vertical] {
        if (_gateway != nullptr)
            _gateway->sendCommand(tmuxSplitWindowCommand(pane, vertical));
    });
    return true;
}

bool TmuxController::requestRemoteTab(vtpty::Pty const* /*actingPty*/)
{
    // The acting pane is ignored: a tmux WINDOW is a tab, and `new-window` creates it in the
    // attached session — of which a mirror has exactly one, so there is no second window to pick.
    {
        auto const lock = std::lock_guard { _mutex };
        if (_realizing)
            return false; // a tab realized by the reconciler is not re-authored on the server
    }
    // Author a new tmux window; its %window-add + layout push realizes as a new tab through the
    // mirror. A no-op if the connection is gone (the gateway is torn down).
    _reactor.post([this] {
        if (_gateway != nullptr)
            _gateway->sendCommand(tmuxNewWindowCommand());
    });
    return true;
}

std::unique_ptr<vtpty::Pty> TmuxController::createPty(std::optional<std::string> /*cwd*/,
                                                      std::optional<vtbackend::PageSize> pageSize,
                                                      std::optional<vtpty::Process::ExecInfo> /*command*/,
                                                      std::optional<std::string> /*profileName*/)
{
    auto lock = std::unique_lock { _mutex };
    // A whole-tree realization names the exact tmux pane to bind (setNextBindPane); otherwise pop the
    // FIFO of discovered panes. A named pane may not have a pending record yet — then it is born at the
    // window size and the mirror's first replay repaints it.
    auto record = PendingPane {};
    if (_nextBindPane.has_value())
    {
        record.pane = *_nextBindPane;
        _nextBindPane.reset();
        auto const it = std::ranges::find(_pending, record.pane, &PendingPane::pane);
        if (it != _pending.end())
        {
            record = *it;
            _pending.erase(it);
        }
        else if (auto const size = pageSize.value_or(vtbackend::PageSize {}); size.columns.value != 0)
        {
            record.columns = unbox<int>(size.columns);
            record.lines = unbox<int>(size.lines);
        }
    }
    else if (!_pending.empty())
    {
        record = _pending.front();
        _pending.pop_front();
    }
    else
    {
        tmuxLog()("No pending tmux pane; handing out an unbound pty.");
        return makeUnboundFallbackPty(pageSize);
    }

    auto pty = std::make_unique<SelfUnbindingChannelPty>(
        vtpty::PageSize { vtpty::LineCount(record.lines), vtpty::ColumnCount(record.columns) },
        [this, pane = record.pane](std::string_view bytes) {
            _reactor.post([this, pane, copy = std::string { bytes }] {
                if (_gateway != nullptr)
                    _gateway->sendRawInput(pane, copy);
            });
        },
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
        },
        [this, pane = record.pane] { unbindPane(pane); });
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

void TmuxController::adoptPendingPanes(contour::session::TerminalSessionManager& manager,
                                       vtworkspace::WindowId guiWindow)
{
    // Splits performed here (whole-tree realize, or an incremental split of an existing tmux pane)
    // must build the mirror pane locally, not author a new split back to tmux. (Mirrors
    // NativeController::isRealizingLayout.)
    {
        auto const lock = std::lock_guard { _mutex };
        _realizing = true;
    }
    while (true)
    {
        // The next window with pending panes, plus a snapshot of what we need to decide how to realize
        // it — all read under the lock, so the GUI thread never touches the reactor-owned model.
        auto window = uint64_t {};
        auto tree = std::optional<BinaryLayout> {};
        auto wholeTree = false;
        {
            auto const lock = std::lock_guard { _mutex };
            if (_pending.empty())
                break;
            window = _pending.front().window;

            if (auto const it = _pendingTrees.find(window); it != _pendingTrees.end())
            {
                auto leaves = std::vector<uint64_t> {};
                collectLeafPanes(it->second, leaves);
                auto const realized =
                    std::ranges::any_of(leaves, [&](uint64_t p) { return _ptys.contains(p); });
                auto const allPending = std::ranges::all_of(leaves, [&](uint64_t p) {
                    return std::ranges::find(_pending, p, &PendingPane::pane) != _pending.end();
                });
                // A window first seen with its whole multi-pane layout (every leaf still pending) is
                // realized as one faithful tree; anything else falls through to incremental.
                if (!realized && allPending && leaves.size() > 1)
                {
                    tree = cloneBinaryLayout(it->second);
                    wholeTree = true;
                }
            }
        }

        if (wholeTree)
        {
            realizeWindowLayout(manager, guiWindow, window, *tree);
            auto const lock = std::lock_guard { _mutex };
            _pendingTrees.erase(window); // realized; a later change re-snapshots on the next paneAdded
        }
        else if (!realizeOnePane(manager, guiWindow))
            break; // creation stalled — stop instead of spinning
    }
    {
        auto const lock = std::lock_guard { _mutex };
        _realizing = false;
    }
    // A window renamed before its first pane was realized now has a tab — title it.
    applyPendingRenames(manager);
}

void TmuxController::realizeWindowLayout(contour::session::TerminalSessionManager& manager,
                                         vtworkspace::WindowId guiWindow,
                                         uint64_t tmuxWindow,
                                         vthost::tmux::BinaryLayout const& tree)
{
    auto const converted = tmuxLayoutToWindowLayout(tree);
    manager.applyLayoutToWindow(
        guiWindow, converted.layout, std::nullopt, [&](contour::config::LayoutPane const& leaf) {
            if (auto const it = converted.leafPane.find(&leaf); it != converted.leafPane.end())
                setNextBindPane(it->second);
        });

    // Seed the split anchor so a later incremental split of this window has a session to split beside.
    // The ID is stored (not the pointer): the anchor is resolved back to a live session at use time,
    // so a pane dying in between can never leave a dangling pointer behind.
    if (auto* win = manager.model().window(guiWindow); win != nullptr && win->tabCount() > 0)
        if (auto* tab = win->tabAt(win->tabCount() - 1); tab != nullptr)
        {
            auto const lock = std::lock_guard { _mutex };
            _actingByWindow.insert_or_assign(tmuxWindow, tab->activePane()->session());
        }
}

bool TmuxController::realizeOnePane(contour::session::TerminalSessionManager& manager,
                                    vtworkspace::WindowId guiWindow)
{
    auto record = PendingPane {};
    {
        auto const lock = std::lock_guard { _mutex };
        if (_pending.empty())
            return false;
        record = _pending.front();
        // A pane already bound (e.g. by a prior whole-tree realize) is just dropped from the queue.
        if (_ptys.contains(record.pane))
        {
            _pending.pop_front();
            return true;
        }
    }

    // Resolve the anchor id back to a LIVE session: a stale anchor (its session died since it was
    // seeded — tab closed, or tmux removed the pane) resolves to nullptr (SessionId{} never resolves,
    // ids start at 1) and falls back to a fresh session instead of dereferencing a dead pane or
    // no-op-splitting forever.
    auto const anchor = [&] {
        auto const lock = std::lock_guard { _mutex };
        auto const it = _actingByWindow.find(record.window);
        return it != _actingByWindow.end() ? it->second : vtworkspace::SessionId {};
    }();
    auto* acting = manager.sessionForId(anchor);

    auto* created = static_cast<contour::session::TerminalSession*>(nullptr);
    if (acting == nullptr)
        created = manager.createSessionInBackground(guiWindow);
    else
    {
        manager.splitActivePane(record.vertical, acting, record.ratio);
        created = acting; // the anchor stays valid for further splits
    }

    auto const lock = std::lock_guard { _mutex };
    // The manager consumes the pending pane through createPty; if it did not (creation refused or
    // failed), report no progress so the caller stops instead of spinning.
    if (!_pending.empty() && _pending.front().pane == record.pane)
        return false;
    if (created != nullptr)
        _actingByWindow[record.window] = created->modelSessionId();
    return true;
}

} // namespace contour::remote
