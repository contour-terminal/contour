// SPDX-License-Identifier: Apache-2.0
#include <vthost/SessionHost.hpp>

#include <chrono>
#include <mutex>
#include <ranges>
#include <utility>
#include <vector>

#include <vthost/SessionSettings.hpp>
#include <vthost/TappingPty.hpp>
#include <vthost/logging.hpp>
#include <vtworkspace/Pane.hpp>
#include <vtworkspace/PaneLayout.hpp>
#include <vtworkspace/Tab.hpp>

namespace vthost
{

using vtworkspace::PaneId;
using vtworkspace::SessionId;
using vtworkspace::SplitState;
using vtworkspace::Tab;
using vtworkspace::TabId;
using vtworkspace::WindowId;

// ---------------------------------------------------------------------------
// HostedSession

HostedSession::HostedSession(SessionId id,
                             crispy::environment const& env,
                             std::unique_ptr<vtpty::Pty> pty,
                             vtbackend::Settings settings,
                             std::function<void()> onScreenUpdated,
                             std::function<void()> onBell,
                             std::function<void(std::string, std::string)> onNotify,
                             std::function<void(std::string)> onCopyToClipboard,
                             std::function<void()> onClosed):
    _id(id),
    _events(std::move(onScreenUpdated), std::move(onBell), std::move(onNotify), std::move(onCopyToClipboard)),
    _terminal(_events, env, std::move(pty), std::move(settings), std::chrono::steady_clock::now()),
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
    // start() reports a failure as a VALUE rather than throwing (it used to throw, which unwound
    // out of session setup -- issue #1711). A daemon has no screen to paint the notice on, so it
    // says so in the log, and still runs the pump: its first read on the dead device ends the loop
    // through the ordinary closed-session path, which prunes the session rather than leaving it
    // hosted and mute.
    if (auto const started = _terminal.device().start(); !started)
        errorLog()("session {}: device failed to start: {}", _id.value, started.error());
    else if (!started->diagnostic.empty())
        sessionLog()("session {}: {}", _id.value, started->diagnostic);
    _pumpThread = std::make_unique<std::thread>([this] { pumpLoop(); });
}

void HostedSession::terminate()
{
    if (!_terminal.device().isClosed())
        _terminal.device().close();
    // Unconditionally, even when someone else closed the device first: close() wakes a blocked
    // reader exactly once, and the pump consumes that wakeup before it re-tests the loop condition
    // below. A second wakeup costs one eventfd write and removes the whole class of "who closed it
    // first" lost-wakeup reasoning from the teardown path.
    _terminal.device().wakeupReader();
}

void HostedSession::pumpLoop()
{
    // Mirrors TerminalSession::mainLoop: block in the PTY read, parse, repeat,
    // until the device closes (shell exit or terminate()). Each batch flushes
    // the terminal's queued replies back into the PTY — the daemon-side stand-in
    // for the GUI's screenUpdated->flushInput hop; without it a shell blocks in
    // its startup terminal probes (DA1, OSC 11, kitty keyboard) forever.
    //
    // The device test is what LETS THIS LOOP END, and it is not redundant with
    // processInputOnce()'s own return: a read on a closed device reports EAGAIN, and
    // processInputOnce answers EAGAIN with `true` (retry) rather than `false` (done). The pump
    // reads with NO timeout, so the retry blocks in the selector forever with no further wakeup
    // coming — and HostedSession's destructor joins this thread, so the whole daemon hung on
    // SIGTERM as soon as it hosted a single session. The GUI's mainLoop ends for the same reason,
    // via its own `_terminating` flag; deriving the condition from the device instead keeps
    // one source of truth, since the device is what terminate() actually mutates.
    while (!_terminal.device().isClosed() && _terminal.processInputOnce())
        _terminal.flushInput();
    if (_onClosed)
        _onClosed();
}

// ---------------------------------------------------------------------------
// SessionHost

SessionHost::SessionHost(net::EventLoop& loop,
                         PtyFactory ptyFactory,
                         vtbackend::Settings settings,
                         crispy::environment const& env,
                         bool startPumps,
                         ClientSizePolicy sizePolicy):
    _loop(loop),
    _ptyFactory(std::move(ptyFactory)),
    // Normalized once, here, so _pageSize and every session that does not override it are derived
    // from settings a host can actually serve.
    _settings(hostedSessionSettings(std::move(settings))),
    _environment(env),
    _pageSize(_settings.pageSize),
    _startPumps(startPumps),
    _sizePolicy(sizePolicy),
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

std::optional<SessionId> SessionHost::seedSession(SessionSpawnRequest const& request)
{
    auto const id = SessionId { _nextSessionId++ };
    auto pty = _ptyFactory(_pageSize);
    if (!pty)
    {
        // Until this line existed, a shell that could not be spawned produced NOTHING: the
        // caller saw a null tab and the user saw a client that had simply not opened a pane.
        errorLog()("session {}: PTY factory failed; no session created", id.value);
        return std::nullopt;
    }

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

    // A request's settings are re-normalized rather than taken as given: they can have travelled in
    // from a client, and this is the one funnel every session passes through.
    auto settings = request.settings ? hostedSessionSettings(*request.settings) : _settings;

    // The terminal must open at the same size as its PTY: the client area, not the factory
    // settings' default — and not a size a requesting client named either, since the host projects
    // the pane tree onto the client area itself.
    settings.pageSize = _pageSize;
    if (request.settings)
        sessionLog()("session {}: spawning with the requesting client's emulation settings ({})",
                     id.value,
                     settings.terminalId);

    auto session = std::make_unique<HostedSession>(
        id,
        _environment,
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
    sessionLog()("session {} spawned at {}x{} ({} live)",
                 id.value,
                 unbox(_pageSize.columns),
                 unbox(_pageSize.lines),
                 _sessions.size());
    return id;
}

Tab* SessionHost::createTab(SessionSpawnRequest const& request, std::optional<SessionId> beside)
{
    // The window the request names, resolved through the session hosting it — exactly as
    // splitActivePane's caller resolves one. Falling back to `_window` unconditionally is what put
    // every client's "new tab" into the daemon's FIRST window, however many it hosts.
    auto const window = [&] {
        if (!beside)
            return _window;
        auto const [owner, tab, leaf] = _model.findSessionLeaf(*beside);
        return owner != nullptr ? owner->id() : _window;
    }();

    auto const seeded = seedSession(request);
    if (!seeded)
        return nullptr;

    auto* tab = _model.createTab(window);
    _pendingSessionId.reset(); // consumed by the allocator; clear any leftover
    if (tab == nullptr)
    {
        // The model refused: destroy the orphaned backing session.
        errorLog()("model refused createTab; reaping orphaned session {}", seeded->value);
        _sessions.erase(seeded->value);
        return nullptr;
    }
    return tab;
}

vtworkspace::Window* SessionHost::createWindow(SessionSpawnRequest const& request)
{
    auto const seeded = seedSession(request);
    if (!seeded)
        return nullptr;

    auto* window = _model.createWindow();
    auto* tab = _model.createTab(window->id());
    _pendingSessionId.reset(); // consumed by the allocator; clear any leftover
    if (tab == nullptr)
    {
        // The model refused the tab: drop the orphaned session and the empty window.
        errorLog()("model refused the first tab of window {}; reaping orphaned session {}",
                   window->id().value,
                   seeded->value);
        _sessions.erase(seeded->value);
        _model.removeWindow(window->id());
        return nullptr;
    }
    return window;
}

void SessionHost::splitActivePane(TabId tab,
                                  SplitState orientation,
                                  double ratio,
                                  SessionSpawnRequest const& request)
{
    auto const seeded = seedSession(request);
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
    {
        errorLog()(
            "model refused the split of tab {}; reaping orphaned session {}", tab.value, seeded->value);
        _sessions.erase(seeded->value);
    }
}

void SessionHost::resolveAuthoritativeArea()
{
    auto const resolved = resolveClientArea(_sizePolicy, _clientAreas | std::views::values);
    // No client has reported one: keep what we had. The sessions still exist and must stay some
    // size, and the last attached client's area is a better guess than the settings' default.
    if (!resolved || *resolved == _pageSize)
        return;
    sessionLog()("client area now {}x{} ({} of {} client(s)); reprojecting every pane",
                 unbox(resolved->columns),
                 unbox(resolved->lines),
                 nameOf(_sizePolicy),
                 _clientAreas.size());
    _pageSize = *resolved;
}

SizeChange SessionHost::applyClientSize(SessionStreamEvents* client, vtpty::PageSize size)
{
    _clientAreas.insert_or_assign(client, ClientArea { .size = size, .sequence = _nextAreaSequence++ });
    resolveAuthoritativeArea();

    // Whether mirrors must resync is NOT answered by the area alone, in either direction.
    // An unchanged area still re-projects — that is how a per-pane refinement (applyPaneSize)
    // gets discarded, and clients re-assert an unchanged area on every attach. A changed area
    // can still leave a given pane on the size it already had. So let the panes report, and
    // resync only when a grid genuinely moved: a resync is a full-grid snapshot, and answering
    // a no-op with one per pane is how a healthy client used to lose its connection.
    return reprojectLayouts();
}

SizeChange SessionHost::applyPaneSize(vtworkspace::SessionId session, vtpty::PageSize size)
{
    auto* backing = terminal(session);
    if (backing == nullptr)
    {
        errorLog()("applyPaneSize: unknown session {}", session.value);
        return SizeChange::Unchanged;
    }
    return resizeLocked(session, *backing, size);
}

SizeChange SessionHost::resizeLocked(SessionId session, vtbackend::Terminal& backing, vtpty::PageSize size)
{
    {
        auto const guard = std::lock_guard { backing };
        // Ask the terminal what this request becomes rather than comparing the raw request:
        // resizeScreen clamps it (a total page must leave room for the status line), so a size
        // that differs from the current one can still be a no-op once clamped.
        if (backing.clampedTotalPageSize(size) == backing.totalPageSize())
            return SizeChange::Unchanged;
        backing.resizeScreen(size);
    }

    // Outside the guard above: an observer resyncs the session, which takes the very lock this
    // function was holding. Announced unconditionally rather than by the requesting connection —
    // every attached client renders this grid, not only the one whose window moved.
    fanOutStream(&SessionStreamEvents::sessionResized, session);
    return SizeChange::Applied;
}

SizeChange SessionHost::reprojectLayouts()
{
    // `|=` rather than a short-circuiting `||`: every pane must be resized regardless of
    // what the earlier ones reported.
    auto moved = false;

    // Every window's panes track the (daemon-wide) client area — a client resize
    // must reach the PTYs of secondary windows too, or their apps keep rendering
    // at stale dimensions while the layout advertises the new geometry.
    _model.forEachTab([&](vtworkspace::Window&, vtworkspace::Tab& tab) {
        // The underlying layout's leaves first; a zoomed leaf then overrides to
        // the full area (tmux's zoom model: the saved layout keeps the rest).
        for (auto const& rect: vtworkspace::layoutInCells(*tab.rootPane(), _pageSize))
            if (auto const* leaf = tab.rootPane()->findPane(rect.pane))
                if (auto* backing = terminal(leaf->session()))
                    moved |= resizeLocked(leaf->session(),
                                          *backing,
                                          vtpty::PageSize { .lines = vtpty::LineCount(rect.height),
                                                            .columns = vtpty::ColumnCount(rect.width) })
                             == SizeChange::Applied;
        if (auto const* zoomed = tab.layoutRoot(); zoomed != tab.rootPane())
            if (auto* backing = terminal(zoomed->session()))
                moved |= resizeLocked(zoomed->session(), *backing, _pageSize) == SizeChange::Applied;
    });
    return moved ? SizeChange::Applied : SizeChange::Unchanged;
}

vtbackend::Terminal* SessionHost::terminal(SessionId session) noexcept
{
    auto const it = _sessions.find(session.value);
    return it != _sessions.end() ? &it->second->terminal() : nullptr;
}

void SessionHost::subscribe(vtworkspace::ModelEvents* observer)
{
    _subscribers.push_back(observer);
}

void SessionHost::unsubscribe(vtworkspace::ModelEvents* observer)
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

    // A departed client's size stops counting, and the remaining ones re-decide. Detaching the
    // large client of a `smallest` pair, or the most recent of a `latest` pair, has to give the
    // survivors their size back — otherwise a client that is gone keeps every application on the
    // daemon at its dimensions for the daemon's whole life. The reproject is what makes it visible
    // (and, through resizeLocked, announces itself to everyone still attached).
    if (_clientAreas.erase(observer) != 0)
    {
        resolveAuthoritativeArea();
        std::ignore = reprojectLayouts();
    }
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
    // Reached from the pump thread's posted completion, so this runs on the loop thread like
    // every other line in this file.
    sessionLog()("session {} exited ({} live)", session.value, _sessions.size());

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
    fanOut(&vtworkspace::ModelEvents::tabAdded, window, tab, index);
}

void SessionHost::tabClosed(WindowId window, TabId tab, int index)
{
    fanOut(&vtworkspace::ModelEvents::tabClosed, window, tab, index);
}

void SessionHost::tabMoved(WindowId window, TabId tab, int fromIndex, int toIndex)
{
    fanOut(&vtworkspace::ModelEvents::tabMoved, window, tab, fromIndex, toIndex);
}

void SessionHost::activeTabChanged(WindowId window, TabId tab, int index)
{
    fanOut(&vtworkspace::ModelEvents::activeTabChanged, window, tab, index);
}

void SessionHost::paneSplit(TabId tab, PaneId splitNode, PaneId newLeaf)
{
    fanOutAfterReproject(&vtworkspace::ModelEvents::paneSplit, tab, splitNode, newLeaf);
}

void SessionHost::paneClosed(TabId tab, PaneId closed, PaneId survivor)
{
    fanOutAfterReproject(&vtworkspace::ModelEvents::paneClosed, tab, closed, survivor);
}

void SessionHost::activePaneChanged(TabId tab, PaneId leaf)
{
    fanOut(&vtworkspace::ModelEvents::activePaneChanged, tab, leaf);
}

void SessionHost::paneRatioChanged(TabId tab, PaneId splitNode, double ratio)
{
    fanOutAfterReproject(&vtworkspace::ModelEvents::paneRatioChanged, tab, splitNode, ratio);
}

void SessionHost::tabTitleChanged(TabId tab)
{
    fanOut(&vtworkspace::ModelEvents::tabTitleChanged, tab);
}

void SessionHost::tabColorChanged(TabId tab)
{
    fanOut(&vtworkspace::ModelEvents::tabColorChanged, tab);
}

void SessionHost::paneOrientationChanged(TabId tab, PaneId splitNode, SplitState state)
{
    fanOutAfterReproject(&vtworkspace::ModelEvents::paneOrientationChanged, tab, splitNode, state);
}

void SessionHost::paneSwapped(TabId tab, PaneId a, PaneId b)
{
    fanOutAfterReproject(&vtworkspace::ModelEvents::paneSwapped, tab, a, b);
}

void SessionHost::paneZoomChanged(TabId tab, std::optional<PaneId> zoomedLeaf)
{
    fanOutAfterReproject(&vtworkspace::ModelEvents::paneZoomChanged, tab, zoomedLeaf);
}

void SessionHost::paneTreeRestructured(TabId tab)
{
    fanOutAfterReproject(&vtworkspace::ModelEvents::paneTreeRestructured, tab);
}

} // namespace vthost
