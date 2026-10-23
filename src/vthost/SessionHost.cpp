// SPDX-License-Identifier: Apache-2.0
#include <vthost/SessionHost.hpp>

#include <vtpty/Process.hpp>

#include <core/Assert.hpp>
#include <core/Overloaded.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <mutex>
#include <ranges>
#include <string_view>
#include <tuple>
#include <utility>
#include <variant>

#include <vthost/Logging.hpp>
#include <vthost/SessionSettings.hpp>
#include <vthost/TappingPty.hpp>
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
                             core::Environment const& env,
                             std::unique_ptr<vtpty::Pty> pty,
                             vtbackend::Settings settings,
                             vtbackend::HostIdentity identity,
                             vtbackend::TerminalClocks clocks,
                             core::platform::FileSystem const& fileSystem,
                             std::function<void()> onScreenUpdated,
                             std::function<void()> onBell,
                             SessionNotifyCallback onNotify,
                             std::function<void(std::string)> onCopyToClipboard,
                             std::function<void()> onClosed,
                             HostedSessionExtensions extensions):
    _id(id),
    _extensions(std::move(extensions.extensions)),
    _events(std::move(identity),
            std::move(onScreenUpdated),
            std::move(onBell),
            std::move(onNotify),
            std::move(onCopyToClipboard),
            // A published frame repaints the indicator, and tells the host when what the extensions follow
            // changed: the worker reads that lock-free, and hands the loop only the change.
            [this,
             published = std::move(extensions.onPublished),
             followingChanged = std::move(extensions.onForegroundFollowingChanged)] {
                if (published)
                    published();
                if (_extensions && followingChanged
                    && _foregroundFollowing.changed(_extensions->subscriptions()))
                    followingChanged();
            }),
    _terminal(_events,
              env,
              fileSystem,
              std::move(pty),
              std::move(settings),
              // The daemon runs no {Command} items: commands an extension declares run in its runtime, under
              // the user's permission.
              std::nullopt,
              clocks,
              vtbackend::randomSemanticBlockTokens(),
              std::nullopt,
              _extensions ? std::optional { _extensions->terminalServices() } : std::nullopt),
    _onClosed(std::move(onClosed))
{
    // DECSSDT 2 only REQUESTS the host-writable status line; the frontend decides.
    // The daemon honors it (the GUI does the same), so an app's status line works.
    _events.onShowHostWritableStatusLine = [this] {
        _terminal.answerHostWritableStatusLineRequest(vtbackend::HostWritableStatusLineAnswer::Shown);
    };
    // Once the terminal they reach exists; they are built on another thread, and run once handed back.
    if (_extensions)
        _extensions->start();
}

void HostedSession::pollForegroundProcess()
{
    if (!followsForegroundProcess())
        return;
    auto const* const tapped = dynamic_cast<TappingPty const*>(&_terminal.device());
    auto const* const process =
        dynamic_cast<vtpty::Process const*>(tapped != nullptr ? &tapped->inner() : &_terminal.device());
    if (process == nullptr)
        return;
    auto name = process->foregroundProcessName();
    if (!name || *name == _foregroundProcessName)
        return;
    _foregroundProcessName = std::move(*name);
    auto const guard = std::lock_guard { _terminal };
    _terminal.foregroundProcessChanged(_foregroundProcessName);
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
    // Named before the loop can hand it input: from now on, only the pump writes to the PTY.
    _terminal.setReaderThread(_pumpThread->get_id());
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
    {
        deliverPending();
        _terminal.flushInput();
    }
    if (_onClosed)
        _onClosed();
}

void HostedSession::deliverOnPump(Delivery deliver, std::string_view bytes)
{
    if (!_pumpThread)
    {
        deliver(_terminal, bytes);
        return;
    }
    {
        auto const _ = std::scoped_lock { _pendingMutex };
        if (!_pending.empty() && _pending.back().deliver == deliver)
            _pending.back().bytes.append(bytes);
        else
            _pending.push_back(PendingDelivery { .deliver = deliver, .bytes = std::string { bytes } });
        _pendingBytes += bytes.size();
    }
    // Its read returns, and it delivers what was queued before it reads again.
    _terminal.device().wakeupReader();
}

namespace
{
    /// How the pump delivers a client's input: written to the PTY as the client encoded it.
    void writeToDevice(vtbackend::Terminal& terminal, std::string_view bytes)
    {
        std::ignore = terminal.device().write(bytes);
    }
} // namespace

void HostedSession::writeInput(std::string_view bytes)
{
    deliverOnPump(writeToDevice, bytes);
}

void HostedSession::deliverPending()
{
    auto pending = std::vector<PendingDelivery> {};
    {
        auto const _ = std::scoped_lock { _pendingMutex };
        pending = std::exchange(_pending, {});
    }
    for (auto const& delivery: pending)
    {
        delivery.deliver(_terminal, delivery.bytes);
        // What a parse asked to send goes out ahead of what was handed over after it.
        _terminal.flushInput();
        auto const _ = std::scoped_lock { _pendingMutex };
        _pendingBytes -= delivery.bytes.size();
    }
}

std::size_t HostedSession::inputBacklog() const
{
    auto const _ = std::scoped_lock { _pendingMutex };
    return _pendingBytes;
}

// ---------------------------------------------------------------------------
// SessionHost

SessionHost::SessionHost(core::net::EventLoop& loop,
                         PtyFactory ptyFactory,
                         vtbackend::Settings settings,
                         core::Environment const& env,
                         SessionHostConfig config):
    _loop(loop),
    _gate(std::make_shared<HostGate>(HostGate { .host = this })),
    _ptyFactory(std::move(ptyFactory)),
    // Normalized once, here, so _pageSize and every session that does not override it are derived
    // from settings a host can actually serve.
    _settings(hostedSessionSettings(std::move(settings))),
    _environment(env),
    _identity(std::move(config.identity)),
    _clocks(config.clocks),
    _pageSize(_settings.pageSize),
    _pumping(config.pumping),
    _inputWatermarks(config.inputWatermarks),
    _sizePolicy(config.sizePolicy),
    _fileSystem(config.fileSystem.get()),
    _extensions(std::move(config.extensions)),
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
    // Realized here, in the BODY: _window is the last member to construct, so this is the
    // earliest point a tab can legally be created on it. Runs before runDaemon() binds any
    // listener, so the layout is in place before a client could possibly attach.
    realizeStartupLayout(_window, config.startupLayout);
}

SessionHost::~SessionHost()
{
    // Close every PTY first so all pump threads unblock and join promptly.
    for (auto& [id, session]: _sessions)
        session->terminate();
    if (_extensions)
    {
        // The drain below turns the loop: from inside a turn -- or beside one on another thread -- it would
        // re-enter it, or race it.
        Require(!_loop.running());
        // Every runner is destroyed with its session, and posts its last job to its lane; a build in flight
        // still holds the engine. Only then are the threads stopped, and what they posted dropped.
        _sessions.clear();
        _extensions->stop();
        std::ignore = _loop.runOnce(std::chrono::milliseconds { 0 });
    }
    _gate->host = nullptr;
}

std::optional<SessionId> SessionHost::seedSession(SessionSpawnRequest const& request)
{
    auto const id = SessionId { _nextSessionId++ };
    auto pty = _ptyFactory(_pageSize, request.commandOverride);
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
        _identity,
        _clocks,
        _fileSystem,
        /*onScreenUpdated=*/
        [this, id] {
            // Pump thread -> loop thread; the host may already be gone at
            // drain time only if the loop is too, so `this` stays valid.
            _loop.post([this, id] {
                auto const hosted = _sessions.find(id.value);
                if (hosted == _sessions.end())
                    return;
                // What the output changed of the indicator's other items, as a frame would repaint it: for a
                // client to be sent, so not while none is attached, and once per loop turn however many
                // batches arrived meanwhile.
                if (hosted->second->hasExtensions() && hasScreenMirrors())
                    hosted->second->requestStatusLineRepaint();
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
        [this, id](std::string title, std::string body, std::optional<vtbackend::DisplayOccasion> occasion) {
            _loop.post([this, id, title = std::move(title), body = std::move(body), occasion] {
                if (!_sessions.contains(id.value))
                    return;
                for (auto* observer: _streamSubscribers)
                    observer->sessionNotify(id, title, body, occasion);
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
        [this, id] { _loop.post([this, id] { handleSessionExit(id); }); },
        makeSessionExtensions(id));

    if (_pumping == SessionPumping::Threaded)
        session->start();
    if (session->hasExtensions())
    {
        // The row a client attaching now mirrors: the daemon renders no frame that would paint it. Nothing is
        // polled yet: what the extensions follow is known once their build lands (extensionsReplaced).
        refreshStatusLine(*session);
    }

    _sessions.emplace(id.value, std::move(session));
    _pendingSessionId = id;
    sessionLog()("session {} spawned at {}x{} ({} live)",
                 id.value,
                 unbox(_pageSize.columns),
                 unbox(_pageSize.lines),
                 _sessions.size());
    return id;
}

void SessionHost::realizeStartupLayout(vtworkspace::WindowId window, vtworkspace::Layout const& layout)
{
    for (auto const& tabSpec: layout.tabs)
    {
        // A pane's profile has no resolution path on the daemon (no Config object to look an
        // arbitrary named profile up in) -- see the design doc's "Out of scope". Warn once per
        // such pane so a user who wrote one is not left guessing why it did not take effect.
        // command/arguments/directory ARE honored: an override is engaged when the pane names a
        // program, or a directory. A directory-only pane still runs the daemon's configured shell,
        // just at a different cwd.
        //
        // The directory half deliberately DIVERGES from the local path, which routes a directory
        // through its separate `cwd` channel because engaging an empty-program override there would
        // bypass an SSH profile's SshSession. The daemon has neither that channel nor an SSH path,
        // so the override is the only way a pane's directory can reach the session at all.
        auto const seeder = [&](vtworkspace::LayoutPane const& leaf) -> bool {
            if (leaf.profile)
                sessionLog()("startup layout: ignoring pane override (profile); spawning the "
                             "daemon's configured shell.");

            // Arguments name what @c command is run with, so without one there is nothing for them
            // to belong to. Say so rather than dropping them where nobody can see it: the local path
            // forwards them to a factory that declines an empty program just the same, so the pane
            // is malformed either way -- silence is the only part worth fixing here. An engaged but
            // EMPTY command is just as program-less as an absent one -- applyCommandOverride declines
            // to overlay program/arguments for either, so both must warn the same way.
            if ((!leaf.command || leaf.command->empty()) && !leaf.arguments.empty())
                sessionLog()("startup layout: ignoring pane arguments ({}) given without a command.",
                             leaf.arguments.size());

            auto commandOverride = std::optional<vtpty::Process::ExecInfo> {};
            if (leaf.command || leaf.directory)
            {
                commandOverride = vtpty::Process::ExecInfo {};
                if (leaf.command)
                {
                    commandOverride->program = *leaf.command;
                    commandOverride->arguments = leaf.arguments;
                }
                if (leaf.directory)
                    commandOverride->workingDirectory = *leaf.directory;
            }
            return seedSession(SessionSpawnRequest { .commandOverride = std::move(commandOverride) })
                .has_value();
        };
        std::ignore = vtworkspace::realizeLayoutTab(_model, window, tabSpec, seeder);
        _pendingSessionId.reset(); // consumed by the allocator; clear any leftover
    }
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

void SessionHost::writeInput(SessionId session, std::string_view bytes)
{
    if (auto const it = _sessions.find(session.value); it != _sessions.end())
        it->second->writeInput(bytes);
}

std::size_t SessionHost::inputBacklog(SessionId session) const
{
    auto const it = _sessions.find(session.value);
    return it != _sessions.end() ? it->second->inputBacklog() : 0;
}

namespace
{
    /// Performs an engine operation on the loop thread, with everything its request names, under the terminal
    /// lock the host takes around the call: the operation mutates the grid the pump thread writes, and
    /// neither sleeps nor writes to the PTY.
    using LoopPerform = void (*)(vtbackend::Terminal& terminal, proto::PerformOperation const& request);

    /// What an engine operation does, and so on which thread: the loop thread's (LoopPerform), or the session
    /// pump's, between two reads, as it parses the application's output (HostedSession::Delivery), handed the
    /// request's text alone: a parse, which can sleep (DECSCLM) and reply.
    /// @see HostedSession::deliverOnPump()
    using OperationPerform = std::variant<LoopPerform, HostedSession::Delivery>;

    /// One engine operation a client may ask the host to perform.
    struct OperationRow
    {
        proto::EngineOperation operation;
        OperationPerform perform;
    };

    /// Has @p terminal place or take off the user mark on the row @p request names.
    /// @param change What to do to the mark.
    void placeUserMark(vtbackend::Terminal& terminal,
                       proto::PerformOperation const& request,
                       vtbackend::UserMarkChange change)
    {
        // A row the grid no longer holds, or a generation gone since the client looked, has nothing to mark:
        // no failure of the request, and nothing the mirror follows.
        terminal.placeUserMark(vtbackend::UserMarkRequest {
            .stableId = request.row.stableId, .generation = request.row.generation, .change = change });
    }

    /// Every operation the host performs, one row each. Each runs as the GUI runs the same action on a
    /// local session.
    constexpr auto OperationTable = std::array {
        OperationRow { .operation = proto::EngineOperation::ClearToPrompt,
                       // Nothing to clear (no prompt, or one that left the history) is no failure of the
                       // request: the grid simply stays as it is, and the mirror with it.
                       .perform =
                           LoopPerform { [](vtbackend::Terminal& terminal, proto::PerformOperation const&) {
                               std::ignore = terminal.clearToPrompt();
                           } } },
        OperationRow { .operation = proto::EngineOperation::ClearHistoryAndReset,
                       .perform =
                           LoopPerform { [](vtbackend::Terminal& terminal, proto::PerformOperation const&) {
                               terminal.hardReset();
                           } } },
        OperationRow { .operation = proto::EngineOperation::SoftReset,
                       .perform =
                           LoopPerform { [](vtbackend::Terminal& terminal, proto::PerformOperation const&) {
                               terminal.softReset();
                           } } },
        OperationRow {
            .operation = proto::EngineOperation::WriteScreen,
            .perform = HostedSession::Delivery { [](vtbackend::Terminal& terminal, std::string_view text) {
                terminal.writeToScreen(text);
            } } },
        // A mirror's vi marks are the session's, so every client shows them and the host's reflow carries
        // them.
        OperationRow { .operation = proto::EngineOperation::SetUserMark,
                       .perform = LoopPerform { [](vtbackend::Terminal& terminal,
                                                   proto::PerformOperation const& request) {
                           placeUserMark(terminal, request, vtbackend::UserMarkChange::Added);
                       } } },
        OperationRow { .operation = proto::EngineOperation::ClearUserMark,
                       .perform = LoopPerform { [](vtbackend::Terminal& terminal,
                                                   proto::PerformOperation const& request) {
                           placeUserMark(terminal, request, vtbackend::UserMarkChange::Removed);
                       } } },
    };
} // namespace

std::expected<void, OperationError> SessionHost::performOperation(proto::PerformOperation const& request)
{
    auto const session = SessionId { request.session };
    auto const hosted = _sessions.find(session.value);
    if (hosted == _sessions.end())
        return std::unexpected(OperationError::UnknownSession);
    auto const row = std::ranges::find(OperationTable, request.operation, &OperationRow::operation);
    if (row == OperationTable.end())
        return std::unexpected(OperationError::UnknownOperation);

    auto& hostedSession = *hosted->second;
    std::visit(
        core::Overloaded {
            [&](LoopPerform perform) {
                {
                    auto& terminal = hostedSession.terminal();
                    auto const guard = std::lock_guard { terminal };
                    perform(terminal, request);
                }
                // Outside the lock, as a resize is announced: an observer builds the delta under it.
                // Announced by the host rather than left to the terminal, since only a parse raises a
                // screen update of its own, and every attached client renders this grid, not only the
                // one that asked.
                fanOutStream(&SessionStreamEvents::sessionScreenUpdated, session);
            },
            // A parse, which raises a screen update of its own as the application's output does.
            [&](HostedSession::Delivery deliver) { hostedSession.deliverOnPump(deliver, request.text); },
        },
        row->perform);
    return {};
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
    // Nothing repainted the rows while no client was attached: one attaching is handed them current.
    if (!observer->mirrorsScreens())
        return;
    for (auto const& [id, session]: _sessions)
        if (session->hasExtensions())
            refreshStatusLine(*session);
}

bool SessionHost::hasScreenMirrors() const noexcept
{
    return std::ranges::any_of(
        _streamSubscribers, [](SessionStreamEvents const* observer) { return observer->mirrorsScreens(); });
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

    auto const hadExtensions = it->second->hasExtensions();
    _sessions.erase(it);
    // Reached from the pump thread's posted completion, so this runs on the loop thread like
    // every other line in this file.
    sessionLog()("session {} exited ({} live)", session.value, _sessions.size());
    auto rehomed = std::vector<proto::PermissionRequest> {};
    if (hadExtensions)
    {
        rehomed = rehomePermissionRequests(session);
        scheduleRetiredRelease();
    }

    // The session is gone: tell stream observers so they drop any per-session
    // state (delta cursors, sent-hyperlink sets) instead of accumulating it.
    for (auto* observer: _streamSubscribers)
        observer->sessionClosed(session);

    // Told after the close, which drops what observers kept of the questions in the pane that is gone.
    for (auto const& request: rehomed)
        for (auto* observer: _streamSubscribers)
            observer->sessionPermissionRequested(SessionId { request.session }, request);
}

std::vector<proto::PermissionRequest> SessionHost::rehomePermissionRequests(SessionId gone)
{
    // A runner asks only once, and a question the same extension asked in another pane was not kept: dropped
    // here, it would never be asked again. So it moves to another pane whose extensions run; it goes only
    // when none is left, as no client could show it.
    auto const heir =
        std::ranges::find_if(_sessions, [](auto const& entry) { return entry.second->hasExtensions(); });
    if (heir == _sessions.end())
    {
        std::erase_if(_pendingPermissions,
                      [&](proto::PermissionRequest const& request) { return request.session == gone.value; });
        return {};
    }
    auto rehomed = std::vector<proto::PermissionRequest> {};
    for (auto& request: _pendingPermissions)
    {
        if (request.session != gone.value)
            continue;
        request.session = heir->first;
        rehomed.push_back(request);
    }
    if (!rehomed.empty())
        sessionLog()("session {} exited with {} open extension question(s): asked in session {} now",
                     gone.value,
                     rehomed.size(),
                     heir->first);
    return rehomed;
}

// ---------------------------------------------------------------------------
// Status-line extensions

HostedSessionExtensions SessionHost::makeSessionExtensions(SessionId session)
{
    if (!_extensions)
        return {};
    // Every hook marshals onto the loop, and does nothing there once the session or the host is gone: session
    // ids are never reused, so one no longer hosted is this one, destroyed; the gate is revoked as the host
    // goes. A job dropped that way is destroyed on the loop, which is where a dropped build must release
    // what it holds -- and ~SessionHost drains the loop before the extensions it would release to go.
    auto const hosted = [gate = _gate, session] {
        return gate->host != nullptr && gate->host->_sessions.contains(session.value);
    };
    auto post = OwnerPost { [&loop = _loop, hosted](std::function<void()> job) {
        loop.post([hosted, job = std::move(job)] {
            if (hosted())
                job();
        });
    } };
    auto events = SessionExtensionEvents {
        .onPermissionNeeded =
            [&loop = _loop, gate = _gate, hosted, session](proto::PermissionRequest request) {
                loop.post([gate, hosted, session, request = std::move(request)]() mutable {
                    if (hosted())
                        gate->host->offerPermission(session, std::move(request));
                });
            },
        // Runs from a job `post` hands to the loop, so only while the session is hosted.
        .onReplaced = [this, session] { extensionsReplaced(session); },
    };
    // A burst of frames costs the loop one repaint, not one per frame: the worker posts only while none is
    // pending, and the loop takes the flag down before it repaints, so a frame published meanwhile posts
    // again.
    auto pending = std::make_shared<std::atomic<bool>>(false);
    auto onPublished = [&loop = _loop, gate = _gate, hosted, session, pending] {
        if (pending->exchange(true, std::memory_order_acq_rel))
            return;
        loop.post([gate, hosted, session, pending] {
            pending->store(false, std::memory_order_release);
            if (hosted())
                gate->host->extensionFramePublished(session);
        });
    };
    // What the extensions follow is known once they have loaded: the poll is re-checked as a frame shows it
    // changed, not only on a replace, which comes before they load.
    auto onForegroundFollowingChanged = [&loop = _loop, gate = _gate] {
        loop.post([gate] {
            if (gate->host != nullptr)
                gate->host->armForegroundPoll();
        });
    };
    return HostedSessionExtensions { .extensions =
                                         _extensions->createSession(std::move(events), std::move(post)),
                                     .onPublished = std::move(onPublished),
                                     .onForegroundFollowingChanged =
                                         std::move(onForegroundFollowingChanged) };
}

void SessionHost::refreshStatusLine(HostedSession& session)
{
    auto& terminal = session.terminal();
    auto const guard = std::lock_guard { terminal };
    terminal.refreshIndicatorStatusLine();
}

void SessionHost::extensionFramePublished(SessionId session)
{
    auto const hosted = _sessions.find(session.value);
    // Repainted for a client to be sent; one attaching later is handed the row current (subscribeStream).
    if (hosted == _sessions.end() || !hasScreenMirrors())
        return;
    refreshStatusLine(*hosted->second);
    fanOutStream(&SessionStreamEvents::sessionScreenUpdated, session);
}

void SessionHost::extensionsReplaced(SessionId session)
{
    auto const hosted = _sessions.find(session.value);
    if (hosted == _sessions.end())
        return;
    hosted->second->forgetForegroundFollowing();
    {
        auto& terminal = hosted->second->terminal();
        auto const guard = std::lock_guard { terminal };
        terminal.statusLineExtensionsReplaced();
        terminal.refreshIndicatorStatusLine();
    }
    // The extensions replaced retire their engines from their lanes.
    scheduleRetiredRelease();
    // What the new ones follow decides whether the hosted programs are asked who runs in the foreground.
    armForegroundPoll();
    fanOutStream(&SessionStreamEvents::sessionScreenUpdated, session);
}

void SessionHost::offerPermission(SessionId session, proto::PermissionRequest request)
{
    // Asked once per installed extension and manifest, however many of its calls, panes or clients run
    // into it; and not at all once answered, which another session's prompt may have done meanwhile.
    auto const sameQuestion = [&](proto::PermissionRequest const& open) {
        return open.hostId == request.hostId && open.digest == request.digest;
    };
    if (_extensions->lookup(request.hostId, request.digest) != GrantState::Pending
        || std::ranges::any_of(_pendingPermissions, sameQuestion))
        return;
    request.session = session.value;
    sessionLog()("session {}: an extension asks to run its commands ({} declared)",
                 session.value,
                 request.commands.size());
    _pendingPermissions.push_back(request);
    // Announced from the copy this keeps in hand rather than the one kept open: an observer may close the
    // question as it is told of it, and the list holding it with it.
    for (auto* observer: _streamSubscribers)
        observer->sessionPermissionRequested(session, request);
}

void SessionHost::answerPermission(std::string_view hostId, std::string_view digest, GrantState answer)
{
    if (!_extensions)
        return;
    // Only an open question is a client's to close: a peer may name any extension and digest.
    if (std::erase_if(_pendingPermissions,
                      [&](proto::PermissionRequest const& request) {
                          return request.hostId == hostId && request.digest == digest;
                      })
        == 0)
    {
        sessionLog()("ignoring an answer to no open permission request");
        return;
    }
    // A dismissal closes it as well, and records nothing: it is asked again when an extension next asks.
    if (answer != GrantState::Pending)
        _extensions->answer(hostId, digest, answer);
}

bool SessionHost::followsForegroundProcess() const noexcept
{
    return std::ranges::any_of(_sessions,
                               [](auto const& entry) { return entry.second->followsForegroundProcess(); });
}

void SessionHost::armForegroundPoll()
{
    if ((_foregroundPoll && !_foregroundPoll->settled()) || !followsForegroundProcess())
        return;
    _foregroundPoll.reset();
    _foregroundPoll.emplace(
        _loop, _loop.clock().now() + std::chrono::seconds { 1 }, &pollForegroundProcesses, this);
}

void SessionHost::pollForegroundProcesses(void* host)
{
    auto& self = *static_cast<SessionHost*>(host);
    for (auto const& [id, session]: self._sessions)
        session->pollForegroundProcess();
    // Re-armed from inside its own callback, which DeadlineTimer allows: it is settled before it is called.
    // Not while no extension follows the foreground process any more: a build that subscribes re-arms it.
    self._foregroundPoll.reset();
    self.armForegroundPoll();
}

void SessionHost::scheduleRetiredRelease()
{
    if (!_extensions || (_retiredRelease && !_retiredRelease->settled()))
        return;
    _retiredRelease.reset();
    _retiredRelease.emplace(_loop, _loop.clock().now() + std::chrono::seconds { 1 }, &releaseRetired, this);
}

void SessionHost::releaseRetired(void* host)
{
    static_cast<SessionHost*>(host)->_extensions->releaseRetired();
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
