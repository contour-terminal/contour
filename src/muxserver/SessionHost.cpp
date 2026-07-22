// SPDX-License-Identifier: Apache-2.0
#include <muxserver/SessionHost.h>

#include <chrono>
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
                             std::function<void()> onClosed):
    _id(id),
    _events(std::move(onScreenUpdated)),
    _terminal(_events, std::move(pty), std::move(settings), std::chrono::steady_clock::now()),
    _onClosed(std::move(onClosed))
{
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
    auto* window = _model.window(_window);
    for (auto const tabIndex: std::views::iota(0, window->tabCount()))
    {
        auto* tab = window->tabAt(tabIndex);
        // The underlying layout's leaves first; a zoomed leaf then overrides to
        // the full area (tmux's zoom model: the saved layout keeps the rest).
        for (auto const& rect: vtmux::layoutInCells(*tab->rootPane(), _pageSize))
            if (auto const* leaf = tab->rootPane()->findPane(rect.pane))
                if (auto* backing = terminal(leaf->session()))
                    backing->resizeScreen(vtpty::PageSize { .lines = vtpty::LineCount(rect.height),
                                                            .columns = vtpty::ColumnCount(rect.width) });
        if (auto const* zoomed = tab->layoutRoot(); zoomed != tab->rootPane())
            if (auto* backing = terminal(zoomed->session()))
                backing->resizeScreen(_pageSize);
    }
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
    // still exists; only then destroy the terminal.
    for (auto const tabIndex: std::views::iota(0, _model.window(_window)->tabCount()))
    {
        auto* tab = _model.window(_window)->tabAt(tabIndex);
        if (tab == nullptr)
            continue;
        PaneId leaf {};
        auto found = false;
        tab->rootPane()->walkTree([&](vtmux::Pane& pane) {
            if (pane.isLeaf() && pane.session() == session)
            {
                leaf = pane.id();
                found = true;
            }
        });
        if (found)
        {
            _model.closePane(_window, tab->id(), leaf);
            break;
        }
    }

    _sessions.erase(it);

    // The session is gone: tell stream observers so they drop any per-session
    // state (delta cursors, sent-hyperlink sets) instead of accumulating it.
    for (auto* observer: _streamSubscribers)
        observer->sessionClosed(session);
}

// ---------------------------------------------------------------------------
// ModelEvents fan-out

void SessionHost::tabAdded(WindowId window, TabId tab, int index)
{
    for (auto* observer: _subscribers)
        observer->tabAdded(window, tab, index);
}

void SessionHost::tabClosed(WindowId window, TabId tab, int index)
{
    for (auto* observer: _subscribers)
        observer->tabClosed(window, tab, index);
}

void SessionHost::tabMoved(WindowId window, TabId tab, int fromIndex, int toIndex)
{
    for (auto* observer: _subscribers)
        observer->tabMoved(window, tab, fromIndex, toIndex);
}

void SessionHost::activeTabChanged(WindowId window, TabId tab, int index)
{
    for (auto* observer: _subscribers)
        observer->activeTabChanged(window, tab, index);
}

void SessionHost::paneSplit(TabId tab, PaneId splitNode, PaneId newLeaf)
{
    // Layout shape changed: bring PTY sizes in line BEFORE observers project
    // the new layout, so what they advertise is what the shells experience.
    reprojectLayouts();
    for (auto* observer: _subscribers)
        observer->paneSplit(tab, splitNode, newLeaf);
}

void SessionHost::paneClosed(TabId tab, PaneId closed, PaneId survivor)
{
    reprojectLayouts();
    for (auto* observer: _subscribers)
        observer->paneClosed(tab, closed, survivor);
}

void SessionHost::activePaneChanged(TabId tab, PaneId leaf)
{
    for (auto* observer: _subscribers)
        observer->activePaneChanged(tab, leaf);
}

void SessionHost::paneRatioChanged(TabId tab, PaneId splitNode, double ratio)
{
    reprojectLayouts();
    for (auto* observer: _subscribers)
        observer->paneRatioChanged(tab, splitNode, ratio);
}

void SessionHost::tabTitleChanged(TabId tab)
{
    for (auto* observer: _subscribers)
        observer->tabTitleChanged(tab);
}

void SessionHost::tabColorChanged(TabId tab)
{
    for (auto* observer: _subscribers)
        observer->tabColorChanged(tab);
}

void SessionHost::paneOrientationChanged(TabId tab, PaneId splitNode, SplitState state)
{
    reprojectLayouts();
    for (auto* observer: _subscribers)
        observer->paneOrientationChanged(tab, splitNode, state);
}

void SessionHost::paneSwapped(TabId tab, PaneId a, PaneId b)
{
    reprojectLayouts();
    for (auto* observer: _subscribers)
        observer->paneSwapped(tab, a, b);
}

void SessionHost::paneZoomChanged(TabId tab, std::optional<PaneId> zoomedLeaf)
{
    reprojectLayouts();
    for (auto* observer: _subscribers)
        observer->paneZoomChanged(tab, zoomedLeaf);
}

void SessionHost::paneTreeRestructured(TabId tab)
{
    reprojectLayouts();
    for (auto* observer: _subscribers)
        observer->paneTreeRestructured(tab);
}

} // namespace muxserver
