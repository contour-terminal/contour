// SPDX-License-Identifier: Apache-2.0
#include <muxserver/SessionHost.h>

#include <chrono>
#include <mutex>
#include <ranges>
#include <utility>
#include <vector>

#include <muxserver/TappingPty.h>
#include <vtmux/Pane.h>
#include <vtmux/PaneLayout.h>
#include <vtmux/Tab.h>

namespace muxserver
{

using vtmux::PaneId;
using vtmux::SessionId;
using vtmux::SplitState;
using vtmux::Tab;
using vtmux::TabId;
using vtmux::WindowId;

// ---------------------------------------------------------------------------
// HostedSession

HostedSession::HostedSession(SessionId id,
                             std::unique_ptr<vtpty::Pty> pty,
                             vtbackend::Settings settings,
                             std::function<void()> onScreenUpdated,
                             std::function<void()> onBell,
                             std::function<void(std::string, std::string)> onNotify,
                             std::function<void(std::string)> onCopyToClipboard,
                             std::function<void()> onClosed):
    _id(id),
    _events(std::move(onScreenUpdated), std::move(onBell), std::move(onNotify), std::move(onCopyToClipboard)),
    _terminal(_events, std::move(pty), std::move(settings), std::chrono::steady_clock::now()),
    _onClosed(std::move(onClosed))
{
    // DECSSDT 2 only REQUESTS the host-writable status line; the frontend decides.
    // The daemon honors it (the GUI does the same), so an app's status line works.
    _events.onShowHostWritableStatusLine = [this] {
        _terminal.setStatusDisplay(vtbackend::StatusDisplayType::HostWritable);
    };
}

HostedSession::~HostedSession()
{
    terminate();
    if (_pumpThread && _pumpThread->joinable())
        _pumpThread->join();
}

void HostedSession::start()
{
    if (_pumpThread)
        return;
    _terminal.device().start();
    _pumpThread = std::make_unique<std::thread>([this] { pumpLoop(); });
}

void HostedSession::terminate()
{
    if (!_terminal.device().isClosed())
        _terminal.device().close();
}

void HostedSession::pumpLoop()
{
    // Mirrors TerminalSession::mainLoop: block in the PTY read, parse, repeat,
    // until the device closes (shell exit or terminate()). Each batch flushes
    // the terminal's queued replies back into the PTY — the daemon-side stand-in
    // for the GUI's screenUpdated->flushInput hop; without it a shell blocks in
    // its startup terminal probes (DA1, OSC 11, kitty keyboard) forever.
    while (_terminal.processInputOnce())
        _terminal.flushInput();
    if (_onClosed)
        _onClosed();
}

// ---------------------------------------------------------------------------
// SessionHost

SessionHost::SessionHost(net::EventLoop& loop,
                         PtyFactory ptyFactory,
                         vtbackend::Settings settings,
                         bool startPumps):
    _loop(loop),
    _ptyFactory(std::move(ptyFactory)),
    _settings(std::move(settings)),
    _pageSize(_settings.pageSize),
    _startPumps(startPumps),
    _model(*this,
           [this]() -> SessionId {
               // The allocator hand-back half of the pre-mint handshake (the GUI's
               // TerminalSessionManager does the same): a seeded creation returns
               // the pre-minted id; anything else mints an unbacked one.
               if (_pendingSessionId)
               {
                   auto const id = *_pendingSessionId;
                   _pendingSessionId.reset();
                   return id;
               }
               return SessionId { _nextSessionId++ };
           }),
    _window(_model.createWindow()->id())
{
}

SessionHost::~SessionHost()
{
    // Close every PTY first so all pump threads unblock and join promptly.
    for (auto& [id, session]: _sessions)
        session->terminate();
}

std::optional<SessionId> SessionHost::seedSession()
{
    auto const id = SessionId { _nextSessionId++ };
    auto pty = _ptyFactory(_pageSize);
    if (!pty)
        return std::nullopt;

    // The control-mode byte tap: fires on the session's PUMP thread with a view
    // into the read buffer, so the bytes are copied before crossing to the loop.
    auto tapped = std::make_unique<TappingPty>(std::move(pty), [this, id](std::string_view data) {
        _loop.post([this, id, copy = std::string { data }] {
            if (!_sessions.contains(id.value))
                return;
            for (auto* observer: _streamSubscribers)
                observer->sessionOutput(id, copy);
        });
    });

    // The terminal must open at the same size as its PTY: the client area, not
    // the factory settings' default.
    auto settings = _settings;
    settings.pageSize = _pageSize;

    auto session = std::make_unique<HostedSession>(
        id,
        std::move(tapped),
        std::move(settings),
        /*onScreenUpdated=*/
        [this, id] {
            // Pump thread -> loop thread; the host may already be gone at
            // drain time only if the loop is too, so `this` stays valid.
            _loop.post([this, id] {
                if (!_sessions.contains(id.value))
                    return;
                for (auto* observer: _streamSubscribers)
                    observer->sessionScreenUpdated(id);
            });
        },
        /*onBell=*/
        [this, id] {
            _loop.post([this, id] {
                if (!_sessions.contains(id.value))
                    return;
                for (auto* observer: _streamSubscribers)
                    observer->sessionBell(id);
            });
        },
        /*onNotify=*/
        [this, id](std::string title, std::string body) {
            _loop.post([this, id, title = std::move(title), body = std::move(body)] {
                if (!_sessions.contains(id.value))
                    return;
                for (auto* observer: _streamSubscribers)
                    observer->sessionNotify(id, title, body);
            });
        },
        /*onCopyToClipboard=*/
        [this, id](std::string data) {
            _loop.post([this, id, data = std::move(data)] {
                if (!_sessions.contains(id.value))
                    return;
                for (auto* observer: _streamSubscribers)
                    observer->sessionCopyToClipboard(id, data);
            });
        },
        /*onClosed=*/
        [this, id] { _loop.post([this, id] { handleSessionExit(id); }); });

    if (_startPumps)
        session->start();

    _sessions.emplace(id.value, std::move(session));
    _pendingSessionId = id;
    return id;
}

Tab* SessionHost::createTab()
{
    auto const seeded = seedSession();
    if (!seeded)
        return nullptr;

    auto* tab = _model.createTab(_window);
    _pendingSessionId.reset(); // consumed by the allocator; clear any leftover
    if (tab == nullptr)
    {
        // The model refused: destroy the orphaned backing session.
        _sessions.erase(seeded->value);
        return nullptr;
    }
    return tab;
}

vtmux::Window* SessionHost::createWindow()
{
    auto const seeded = seedSession();
    if (!seeded)
        return nullptr;

    auto* window = _model.createWindow();
    auto* tab = _model.createTab(window->id());
    _pendingSessionId.reset(); // consumed by the allocator; clear any leftover
    if (tab == nullptr)
    {
        // The model refused the tab: drop the orphaned session and the empty window.
        _sessions.erase(seeded->value);
        _model.removeWindow(window->id());
        return nullptr;
    }
    return window;
}

void SessionHost::splitActivePane(TabId tab, SplitState orientation, double ratio)
{
    auto const seeded = seedSession();
    if (!seeded)
        return;

    auto const paneCountBefore = [&] {
        auto const* tabPtr = _model.findTab(tab);
        return tabPtr != nullptr ? tabPtr->paneCount() : 0;
    }();

    _model.splitActivePane(tab, orientation, ratio);
    _pendingSessionId.reset();

    // The model refused (unknown tab, zoomed, ...): reap the orphaned session.
    auto const* tabPtr = _model.findTab(tab);
    if (tabPtr == nullptr || tabPtr->paneCount() == paneCountBefore)
        _sessions.erase(seeded->value);
}

void SessionHost::applyClientSize(vtpty::PageSize size)
{
    _pageSize = size;
    reprojectLayouts();
}

void SessionHost::reprojectLayouts()
{
    // resizeScreen mutates shared terminal state and does NOT lock internally; the
    // session's pump thread writes the same grid under _stateMutex. Hold the
    // terminal lock across the resize, exactly as the GUI's sole caller does
    // (TerminalSession::attachDisplay).
    auto const resizeLocked = [](vtbackend::Terminal& backing, vtpty::PageSize size) {
        auto const guard = std::lock_guard { backing };
        backing.resizeScreen(size);
    };

    // Every window's panes track the (daemon-wide) client area — a client resize
    // must reach the PTYs of secondary windows too, or their apps keep rendering
    // at stale dimensions while the layout advertises the new geometry.
    _model.forEachTab([&](vtmux::Window&, vtmux::Tab& tab) {
        // The underlying layout's leaves first; a zoomed leaf then overrides to
        // the full area (tmux's zoom model: the saved layout keeps the rest).
        for (auto const& rect: vtmux::layoutInCells(*tab.rootPane(), _pageSize))
            if (auto const* leaf = tab.rootPane()->findPane(rect.pane))
                if (auto* backing = terminal(leaf->session()))
                    resizeLocked(*backing,
                                 vtpty::PageSize { .lines = vtpty::LineCount(rect.height),
                                                   .columns = vtpty::ColumnCount(rect.width) });
        if (auto const* zoomed = tab.layoutRoot(); zoomed != tab.rootPane())
            if (auto* backing = terminal(zoomed->session()))
                resizeLocked(*backing, _pageSize);
    });
}

vtbackend::Terminal* SessionHost::terminal(SessionId session) noexcept
{
    auto const it = _sessions.find(session.value);
    return it != _sessions.end() ? &it->second->terminal() : nullptr;
}

void SessionHost::subscribe(vtmux::ModelEvents* observer)
{
    _subscribers.push_back(observer);
}

void SessionHost::unsubscribe(vtmux::ModelEvents* observer)
{
    std::erase(_subscribers, observer);
}

void SessionHost::subscribeStream(SessionStreamEvents* observer)
{
    _streamSubscribers.push_back(observer);
}

void SessionHost::unsubscribeStream(SessionStreamEvents* observer)
{
    std::erase(_streamSubscribers, observer);
}

void SessionHost::handleSessionExit(SessionId session)
{
    auto const it = _sessions.find(session.value);
    if (it == _sessions.end())
        return;

    // Prune the pane from the model FIRST (prune-then-terminate): closing the
    // pane may fire paneClosed or tabClosed to subscribers while the session
    // still exists; only then destroy the terminal. The session may live in ANY
    // window (a client-created secondary window included).
    if (auto const [window, tab, leaf] = _model.findSessionLeaf(session); leaf != nullptr)
        _model.closePane(window->id(), tab->id(), leaf->id());

    _sessions.erase(it);

    // The session is gone: tell stream observers so they drop any per-session
    // state (delta cursors, sent-hyperlink sets) instead of accumulating it.
    for (auto* observer: _streamSubscribers)
        observer->sessionClosed(session);
}

// ---------------------------------------------------------------------------
// ModelEvents fan-out

// Each override forwards to every subscriber via fanOut; the layout-shape-changing
// events use fanOutAfterReproject so PTY sizes are brought in line BEFORE observers
// project the new layout (what they advertise is what the shells experience).

void SessionHost::tabAdded(WindowId window, TabId tab, int index)
{
    fanOut(&vtmux::ModelEvents::tabAdded, window, tab, index);
}

void SessionHost::tabClosed(WindowId window, TabId tab, int index)
{
    fanOut(&vtmux::ModelEvents::tabClosed, window, tab, index);
}

void SessionHost::tabMoved(WindowId window, TabId tab, int fromIndex, int toIndex)
{
    fanOut(&vtmux::ModelEvents::tabMoved, window, tab, fromIndex, toIndex);
}

void SessionHost::activeTabChanged(WindowId window, TabId tab, int index)
{
    fanOut(&vtmux::ModelEvents::activeTabChanged, window, tab, index);
}

void SessionHost::paneSplit(TabId tab, PaneId splitNode, PaneId newLeaf)
{
    fanOutAfterReproject(&vtmux::ModelEvents::paneSplit, tab, splitNode, newLeaf);
}

void SessionHost::paneClosed(TabId tab, PaneId closed, PaneId survivor)
{
    fanOutAfterReproject(&vtmux::ModelEvents::paneClosed, tab, closed, survivor);
}

void SessionHost::activePaneChanged(TabId tab, PaneId leaf)
{
    fanOut(&vtmux::ModelEvents::activePaneChanged, tab, leaf);
}

void SessionHost::paneRatioChanged(TabId tab, PaneId splitNode, double ratio)
{
    fanOutAfterReproject(&vtmux::ModelEvents::paneRatioChanged, tab, splitNode, ratio);
}

void SessionHost::tabTitleChanged(TabId tab)
{
    fanOut(&vtmux::ModelEvents::tabTitleChanged, tab);
}

void SessionHost::tabColorChanged(TabId tab)
{
    fanOut(&vtmux::ModelEvents::tabColorChanged, tab);
}

void SessionHost::paneOrientationChanged(TabId tab, PaneId splitNode, SplitState state)
{
    fanOutAfterReproject(&vtmux::ModelEvents::paneOrientationChanged, tab, splitNode, state);
}

void SessionHost::paneSwapped(TabId tab, PaneId a, PaneId b)
{
    fanOutAfterReproject(&vtmux::ModelEvents::paneSwapped, tab, a, b);
}

void SessionHost::paneZoomChanged(TabId tab, std::optional<PaneId> zoomedLeaf)
{
    fanOutAfterReproject(&vtmux::ModelEvents::paneZoomChanged, tab, zoomedLeaf);
}

void SessionHost::paneTreeRestructured(TabId tab)
{
    fanOutAfterReproject(&vtmux::ModelEvents::paneTreeRestructured, tab);
}

} // namespace muxserver
