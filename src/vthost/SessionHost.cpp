// SPDX-License-Identifier: Apache-2.0
#include <vthost/SessionHost.hpp>

#include <vtpty/Process.hpp>

#include <core/Assert.hpp>
#include <core/Overloaded.hpp>
#include <core/Utf8Path.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <mutex>
#include <ranges>
#include <string_view>
#include <tuple>
#include <utility>
#include <variant>

#include <vthost/HistorySeed.hpp>
#include <vthost/HostedShell.hpp>
#include <vthost/Logging.hpp>
#include <vthost/RerunPolicy.hpp>
#include <vthost/SessionSettings.hpp>
#include <vthost/TappingPty.hpp>
#include <vthost/control/LoopControlBridge.hpp>
#include <vtworkspace/Pane.hpp>
#include <vtworkspace/PaneLayout.hpp>
#include <vtworkspace/Tab.hpp>

namespace vthost
{

using vtcontrol::ObjectKind;
using vtcontrol::ObjectRef;
using vtcontrol::Uuid;
using vtworkspace::PaneId;
using vtworkspace::SessionId;
using vtworkspace::SplitState;
using vtworkspace::Tab;
using vtworkspace::TabId;
using vtworkspace::WindowId;

// ---------------------------------------------------------------------------
// HostGitLookups

HostGitLookups::HostGitLookups(std::optional<vtbackend::GitHeadServices> given,
                               core::platform::FileSystem const& fileSystem):
    _fileSystem { !given && &fileSystem == &core::platform::NativeFileSystem::instance()
                      ? std::make_unique<vtbackend::NativeGitFileSystem>(fileSystem)
                      : nullptr },
    // Started here, on the thread building the host: the daemon blocks its shutdown signals before it does,
    // and the probe thread inherits that mask, as every pump thread does.
    _executor { _fileSystem ? std::make_unique<core::async::ThreadPoolExecutor>(1) : nullptr },
    _services { _fileSystem ? std::optional { vtbackend::GitHeadServices { .executor = *_executor,
                                                                           .fileSystem = *_fileSystem } }
                            : given }
{
}

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
                             SessionSignalCallback onSignal,
                             HostedSessionExtensions extensions,
                             std::string profileName,
                             SettingsOrigin settingsOrigin,
                             std::optional<vtbackend::GitHeadServices> git):
    _id(id),
    _profileName(std::move(profileName)),
    _settingsOrigin(settingsOrigin),
    _extensions(std::move(extensions.extensions)),
    _events(std::move(identity),
            std::move(onScreenUpdated),
            std::move(onBell),
            std::move(onNotify),
            std::move(onCopyToClipboard),
            std::move(onSignal),
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
              _extensions ? std::optional { _extensions->terminalServices() } : std::nullopt,
              git),
    _onClosed(std::move(onClosed))
{
    // DECSSDT 2 only REQUESTS the host-writable status line; the frontend decides.
    // The daemon honors it (the GUI does the same), so an app's status line works.
    _events.onShowHostWritableStatusLine = [this] {
        _terminal.answerHostWritableStatusLineRequest(vtbackend::HostWritableStatusLineAnswer::Shown);
    };
    // Refused, not dropped: a client reads until the terminating chunk. No lock here -- the hook runs on the
    // parser thread with the terminal lock held -- and the pump's flushInput() after the batch writes it.
    _events.onCaptureBuffer = [this] {
        _terminal.primaryScreen().replyCaptureBufferEnd();
    };
    // Once the terminal they reach exists; they are built on another thread, and run once handed back.
    if (_extensions)
        _extensions->start();
}

vtpty::Process const* HostedSession::process() const noexcept
{
    auto const* const tapped = dynamic_cast<TappingPty const*>(&_terminal.device());
    return dynamic_cast<vtpty::Process const*>(tapped != nullptr ? &tapped->inner() : &_terminal.device());
}

void HostedSession::pollForegroundProcess()
{
    if (!followsForegroundProcess())
        return;
    auto const* const child = process();
    if (child == nullptr)
        return;
    auto name = child->foregroundProcessName();
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
    _pumpEnd.store(PumpEnd::Terminated, std::memory_order_release);
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
    _endCause = observeEnd();
    // Strictly before the close is announced, and only for a process that ended by itself: a close the
    // host asked for (terminate()) ends the session, and its program-status records with it.
    if (!terminateRequested())
        _terminal.markProcessExited();
    if (_onClosed)
        _onClosed();
}

SessionEnd HostedSession::observeEnd() const
{
    // The host closing the PTY says nothing of how the program ended; and without a process there is nothing
    // to ask.
    auto const* const child = process();
    if (child == nullptr || terminateRequested())
        return SessionEnd::Exited;
    // The PTY reports its hangup before the child is necessarily reaped: give the status a moment to land.
    constexpr auto Patience = std::chrono::milliseconds { 200 };
    constexpr auto Interval = std::chrono::milliseconds { 10 };
    auto const deadline = std::chrono::steady_clock::now() + Patience;
    auto status = child->checkStatus();
    while (!status && !terminateRequested() && std::chrono::steady_clock::now() < deadline)
    {
        std::this_thread::sleep_for(Interval);
        status = child->checkStatus();
    }
    return sessionEndOf(status);
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
    _profilePtys(std::move(config.profilePtys)),
    // Normalized once, here, so _pageSize and every session that does not override it are derived
    // from settings a host can actually serve.
    _settings(hostedSessionSettings(std::move(settings))),
    _environment(env),
    _identity(std::move(config.identity)),
    _instance(config.instance),
    _clocks(config.clocks),
    _pageSize(_settings.pageSize),
    _pumping(config.pumping),
    _inputWatermarks(config.inputWatermarks),
    _sizePolicy(config.sizePolicy),
    _fileSystem(config.fileSystem.get()),
    _git(config.git, config.fileSystem.get()),
    _profiles(std::move(config.profiles)),
    _extensions(std::move(config.extensions)),
    _objects(std::move(config.uuids)),
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
    // The first window is named before anything is spawned into it: its UUID is part of every pane's
    // environment.
    _objects.bind(_window, _objects.mint());
    // Realized here, in the BODY: _window is the last member to construct, so this is the
    // earliest point a tab can legally be created on it. Runs before runDaemon() binds any
    // listener, so the layout is in place before a client could possibly attach. A restored workspace
    // replaces the startup layout, unless it brought nothing back.
    if (config.restore && realizeRestoredWorkspace(*config.restore) > 0)
        _restoreOutcome = RestoreOutcome::Realized;
    else
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

SessionHost::ResolvedSpawn SessionHost::resolveSpawn(SessionSpawnRequest const& request, SessionId id) const
{
    // With a catalog every spawn resolves its shell in it — a named profile, or the default for none — so a
    // reloaded catalog reaches unnamed spawns too, and each shell carries its own profile's environment and
    // sandbox policy rather than the default's. A layout pane's own command and directory still overlay
    // that shell. A named profile's settings replace the requested ones; an unnamed spawn keeps the settings
    // a client stated, else takes the default profile's.
    auto const named = !request.profileName.empty();
    auto spawn = ResolvedSpawn { .settings = request.settings,
                                 .commandOverride = request.commandOverride,
                                 .settingsOrigin = !named && request.settings ? SettingsOrigin::Client
                                                                              : SettingsOrigin::Profile };
    if (!_profiles)
        return spawn;
    auto resolved = _profiles->resolve(request.profileName);
    if (named && resolved.match == ProfileMatch::Default)
        sessionLog()("session {}: profile '{}' is unknown here; using '{}'",
                     id.value,
                     request.profileName,
                     resolved.name);
    vtpty::Process::applyCommandOverride(resolved.shell, request.commandOverride);
    spawn.commandOverride = std::move(resolved.shell);
    spawn.sandbox = resolved.sandbox;
    if (named || !spawn.settings)
        spawn.settings = std::move(resolved.settings);
    // An unnamed spawn records no name: it follows whichever profile is the default, after a reload too.
    if (named)
        spawn.profileName = std::move(resolved.name);
    return spawn;
}

std::unique_ptr<vtpty::Pty> SessionHost::makePty(ResolvedSpawn const& spawn) const
{
    // The profile's own sandbox policy travels with its shell: one inherited from the daemon's default
    // profile would run a contained profile's shell on the host.
    if (spawn.sandbox && _profilePtys && spawn.commandOverride)
        return _profilePtys(_pageSize, *spawn.commandOverride, *spawn.sandbox);
    return _ptyFactory(_pageSize, spawn.commandOverride);
}

Uuid SessionHost::paneUuidFor(SessionSpawnRequest const& request)
{
    return request.presetIds ? request.presetIds->pane : _objects.mint();
}

Uuid SessionHost::tabUuidFor(SessionSpawnRequest const& request)
{
    if (request.presetIds && request.presetIds->tab)
        return *request.presetIds->tab;
    return _objects.mint();
}

Uuid SessionHost::windowUuid(WindowId window) const
{
    return _objects.uuidOf(window).value_or(Uuid {});
}

void SessionHost::reapOrphan(SessionId session)
{
    eraseSession(session.value);
    _objects.forget(session);
}

std::optional<SessionId> SessionHost::seedSession(SessionSpawnRequest const& incoming, SpawnIds const& ids)
{
    auto const id = SessionId { _nextSessionId++ };

    // Every shell is told where it lives, so the override is always engaged; an empty program or working
    // directory in it still means "the configured one". The ids win over anything the request's own
    // environment says.
    auto request = incoming;
    if (!request.commandOverride)
        request.commandOverride.emplace();
    for (auto const& [name, value]: paneEnvironment(ids.window, ids.tab, ids.pane))
        request.commandOverride->env.insert_or_assign(name, value);

    auto spawn = resolveSpawn(request, id);
    auto& requestedSettings = spawn.settings;
    auto& profileName = spawn.profileName;
    auto pty = makePty(spawn);
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
        postStreamEvent(id, &SessionStreamEvents::sessionOutput, std::string { data });
    });

    // A request's settings are re-normalized rather than taken as given: they can have travelled in
    // from a client, and this is the one funnel every session passes through.
    auto settings = requestedSettings ? hostedSessionSettings(*requestedSettings) : _settings;

    // The terminal must open at the same size as its PTY: the client area, not the factory
    // settings' default — and not a size a requesting client named either, since the host projects
    // the pane tree onto the client area itself.
    settings.pageSize = _pageSize;
    if (!profileName.empty())
        sessionLog()("session {}: spawning under profile '{}'", id.value, profileName);
    else if (request.settings)
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
            _loop.post([this, id] { onSessionScreenUpdated(id); });
        },
        /*onBell=*/
        [this, id] { postStreamEvent(id, &SessionStreamEvents::sessionBell); },
        /*onNotify=*/
        [this, id](std::string title, std::string body, std::optional<vtbackend::DisplayOccasion> occasion) {
            postStreamEvent(
                id, &SessionStreamEvents::sessionNotify, std::move(title), std::move(body), occasion);
        },
        /*onCopyToClipboard=*/
        [this, id](std::string data) {
            postStreamEvent(id, &SessionStreamEvents::sessionCopyToClipboard, std::move(data));
        },
        /*onClosed=*/
        [this, id] {
            _loop.post([this, id] {
                // The pump has ended by the time this runs, so what it saw is final.
                if (auto const it = _sessions.find(id.value); it != _sessions.end())
                    endSession(id, it->second->endCause());
            });
        },
        /*onSignal=*/
        [this, id](SessionSignal signal) {
            postStreamEvent(id, &SessionStreamEvents::sessionSignal, std::move(signal));
        },
        makeSessionExtensions(id),
        std::move(profileName),
        spawn.settingsOrigin,
        _git.services());

    // Before the pump starts: the seed assumes nothing else writes to the terminal.
    if (request.restoredHistory)
        seedRestoredHistory(session->terminal(), *request.restoredHistory, request.restoredMarker);
    if (_pumping == SessionPumping::Threaded)
        session->start();
    if (session->hasExtensions())
    {
        // The row a client attaching now mirrors: the daemon renders no frame that would paint it. Nothing is
        // polled yet: what the extensions follow is known once their build lands (extensionsReplaced).
        refreshStatusLine(*session);
    }

    _sessions.emplace(id.value, std::move(session));
    _objects.bind(id, ids.pane);
    if (incoming.presetIds)
        _objects.setLabel(ObjectRef { ObjectKind::Pane, ids.pane }, incoming.presetIds->paneLabel);
    _pendingSessionId = id;
    if (!request.initialInput.empty())
        holdInitialInput(id, request.initialInput);
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
        // Every pane of the tab names this tab and window in its environment; the tab's UUID is minted with
        // its first pane and bound when the model announces the tab.
        auto tabUuid = std::optional<Uuid> {};
        // A pane's profile (else its tab's) is resolved in the host's profile catalog; a host without
        // one has nothing to look it up in, and warns once per such pane so a user who wrote one is not
        // left guessing why it did not take effect. command/arguments/directory ARE honored: an override is
        // engaged when the pane names a program, or a directory. A directory-only pane still runs the
        // daemon's configured shell, just at a different cwd.
        //
        // The directory half deliberately DIVERGES from the local path, which routes a directory
        // through its separate `cwd` channel because engaging an empty-program override there would
        // bypass an SSH profile's SshSession. The daemon has neither that channel nor an SSH path,
        // so the override is the only way a pane's directory can reach the session at all.
        auto const seeder = [&](vtworkspace::LayoutPane const& leaf) -> bool {
            // A pane's profile wins over its tab's, as on the local path.
            auto profile = leaf.profile.or_else([&] { return tabSpec.profile; }).value_or(std::string {});
            if (!profile.empty() && !_profiles)
                sessionLog()("startup layout: ignoring profile '{}' (the host resolves none); spawning the "
                             "daemon's configured shell.",
                             profile);

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
            auto const pane = _objects.mint();
            if (!tabUuid)
            {
                tabUuid = _objects.mint();
                _stagedTab = StagedTab { .uuid = *tabUuid, .label = {} };
            }
            return seedSession(SessionSpawnRequest { .commandOverride = std::move(commandOverride),
                                                     .profileName = std::move(profile) },
                               SpawnIds { .window = windowUuid(window), .tab = *tabUuid, .pane = pane })
                .has_value();
        };
        std::ignore = vtworkspace::realizeLayoutTab(_model, window, tabSpec, seeder);
        _pendingSessionId.reset(); // consumed by the allocator; clear any leftover
        _stagedTab.reset();        // bound by tabAdded, unless the tab was never created
    }
}

namespace
{
    /// A tab of a snapshot as the layout realizer takes it, plus each of its leaves' captured data.
    struct RestoredTab
    {
        vtworkspace::LayoutTab layout;
        std::unordered_map<vtworkspace::LayoutPane const*, PaneSnapshot const*> panes {};
    };

    [[nodiscard]] vtworkspace::LayoutPane layoutPaneOf(PaneNode const& node)
    {
        auto pane = vtworkspace::LayoutPane {};
        if (node.children.empty())
        {
            pane.profile = node.pane.profile;
            return pane;
        }
        pane.orientation = node.split;
        for (auto const& child: node.children)
            pane.children.push_back(layoutPaneOf(child));
        // The realizer reads a child's share of the whole split: the first gets the ratio, the second the
        // rest.
        pane.children.front().ratio = node.ratio;
        pane.children.back().ratio = 1.0 - node.ratio;
        return pane;
    }

    /// Pairs each leaf of @p pane with the leaf of @p node it was converted from.
    void indexLeaves(vtworkspace::LayoutPane const& pane,
                     PaneNode const& node,
                     std::unordered_map<vtworkspace::LayoutPane const*, PaneSnapshot const*>& panes)
    {
        if (pane.isLeaf())
        {
            panes.emplace(&pane, &node.pane);
            return;
        }
        for (auto const index: std::views::iota(std::size_t { 0 }, pane.children.size()))
            indexLeaves(pane.children[index], node.children[index], panes);
    }

    [[nodiscard]] std::unique_ptr<RestoredTab> restoredTabOf(TabSnapshot const& tab)
    {
        // Held by pointer: the leaf index names addresses inside the layout, which must not move.
        auto restored = std::make_unique<RestoredTab>();
        restored->layout.title = tab.title;
        restored->layout.color = tab.color;
        restored->layout.root = layoutPaneOf(tab.root);
        indexLeaves(restored->layout.root, tab.root, restored->panes);
        return restored;
    }

    /// @return The @p index-th leaf of @p pane, depth first, first child first; null past the last.
    [[nodiscard]] vtworkspace::Pane const* leafAt(vtworkspace::Pane const& pane, std::uint32_t& index)
    {
        if (pane.isLeaf())
            return index-- == 0 ? &pane : nullptr;
        if (auto const* const found = leafAt(*pane.first(), index))
            return found;
        return leafAt(*pane.second(), index);
    }

    /// @return The label @p identity carries; empty for none.
    [[nodiscard]] std::string labelOf(std::optional<ObjectIdentityRecord> const& identity)
    {
        return identity ? identity->label.value_or(std::string {}) : std::string {};
    }
} // namespace

Uuid SessionHost::restoredUuid(ObjectKind kind, std::optional<ObjectIdentityRecord> const& identity)
{
    if (identity)
        if (auto const uuid = Uuid { .bytes = identity->uuid };
            uuid != Uuid {} && !_objects.find(ObjectRef { .kind = kind, .uuid = uuid }))
            return uuid;
    return _objects.mint();
}

std::size_t SessionHost::realizeRestoredWorkspace(WorkspaceRestore const& restore)
{
    auto realized = std::size_t { 0 };
    for (auto const& window: restore.snapshot.windows)
    {
        // The first window that brings anything back is the host's own, which never stays empty while a
        // later one fills.
        auto const id = realized == 0 ? _window : _model.createWindow()->id();
        // The host's own window takes the identity of each one that tries to fill it, the last such one
        // keeping it.
        auto const uuid = restoredUuid(ObjectKind::Window, window.identity);
        _objects.bind(id, uuid);
        _objects.setLabel(ObjectRef { .kind = ObjectKind::Window, .uuid = uuid }, labelOf(window.identity));
        auto const panes = realizeRestoredWindow(id, window, restore);
        // A window none of whose panes came back is not left behind empty.
        if (panes == 0 && id != _window)
        {
            _objects.forget(id);
            _model.removeWindow(id);
        }
        realized += panes;
    }
    sessionLog()("restore: {} pane(s) of a snapshot of {} window(s) came back",
                 realized,
                 restore.snapshot.windows.size());
    return realized;
}

std::size_t SessionHost::realizeRestoredWindow(WindowId window,
                                               WindowSnapshot const& snapshot,
                                               WorkspaceRestore const& restore)
{
    auto realized = std::size_t { 0 };
    auto tabs = std::vector<Tab*> {};
    for (auto const& tabSnapshot: snapshot.tabs)
    {
        auto const restored = restoredTabOf(tabSnapshot);
        // As for a startup layout: the tab's UUID is staged with its first pane and bound when the model
        // announces the tab. A restored object keeps the UUID and label it was captured with.
        auto tabUuid = std::optional<Uuid> {};
        auto const seeder = [&](vtworkspace::LayoutPane const& leaf) -> bool {
            auto const& pane = *restored->panes.at(&leaf);
            auto request = SessionSpawnRequest { .profileName = pane.profile.value_or(std::string {}) };
            if (pane.workingDirectory)
            {
                // A directory deleted or unmounted since is no reason to lose the pane.
                if (_fileSystem.isDirectory(*pane.workingDirectory))
                {
                    request.commandOverride = vtpty::Process::ExecInfo {};
                    request.commandOverride->workingDirectory = *pane.workingDirectory;
                }
                else
                    sessionLog()(
                        "restore: {} no longer exists; the pane starts in the shell's default directory",
                        core::pathToUtf8(*pane.workingDirectory));
            }
            if (pane.history)
            {
                request.restoredHistory = std::make_shared<proto::Delta const>(*pane.history);
                request.restoredMarker = restore.marker;
            }
            request.initialInput = rerunInputFor(pane, restore.rerunPrograms).value_or(std::string {});
            auto const paneUuid = restoredUuid(ObjectKind::Pane, pane.identity);
            request.presetIds = PresetObjectIds { .pane = paneUuid, .paneLabel = labelOf(pane.identity) };
            if (!tabUuid)
            {
                tabUuid = restoredUuid(ObjectKind::Tab, tabSnapshot.identity);
                _stagedTab = StagedTab { .uuid = *tabUuid, .label = labelOf(tabSnapshot.identity) };
            }
            auto const seeded =
                seedSession(request,
                            SpawnIds { .window = windowUuid(window), .tab = *tabUuid, .pane = paneUuid })
                    .has_value();
            realized += seeded ? 1 : 0;
            return seeded;
        };
        auto* const tab = vtworkspace::realizeLayoutTab(_model, window, restored->layout, seeder);
        _pendingSessionId.reset(); // consumed by the allocator; clear any leftover
        _stagedTab.reset();        // bound by tabAdded, unless the tab was never created
        if (tab == nullptr)
            continue;
        tabs.push_back(tab);
        auto leaf = tabSnapshot.activeLeaf;
        if (auto const* const active = leafAt(*tab->rootPane(), leaf))
            _model.setActivePane(tab->id(), active->id());
        if (tabSnapshot.zoom == TabZoom::Zoomed && tab->paneCount() > 1)
            _model.toggleActivePaneZoom(tab->id());
    }
    if (snapshot.activeTab < tabs.size())
        _model.activateTab(window, tabs[snapshot.activeTab]->id());
    return realized;
}

void SessionHost::holdInitialInput(SessionId session, std::string input)
{
    auto pending = std::make_unique<PendingInitialInput>(this, session, std::move(input));
    pending->fallback.emplace(
        _loop, _loop.clock().now() + InitialInputFallback, &initialInputFallbackExpired, pending.get());
    _initialInputs.insert_or_assign(session.value, std::move(pending));
}

void SessionHost::onSessionScreenUpdated(SessionId session)
{
    auto const hosted = _sessions.find(session.value);
    if (hosted == _sessions.end())
        return;
    // What the output changed of the indicator's other items, as a frame would repaint it: for a
    // client to be sent, so not while none is attached, and once per loop turn however many
    // batches arrived meanwhile.
    if (hosted->second->hasExtensions() && hasScreenMirrors())
        hosted->second->requestStatusLineRepaint();
    typeInitialInputAtPrompt(session);
    for (auto* observer: _streamSubscribers)
        observer->sessionScreenUpdated(session);
}

void SessionHost::typeInitialInputAtPrompt(SessionId session)
{
    auto const held = _initialInputs.find(session.value);
    if (held == _initialInputs.end())
        return;
    auto* const backing = terminal(session);
    if (backing == nullptr)
        return;
    auto const prompting = [&] {
        auto const guard = std::lock_guard { *backing };
        return backing->commandBlocks().current() != nullptr;
    }();
    if (prompting)
        typeInitialInput(session);
}

void SessionHost::typeInitialInput(SessionId session)
{
    auto const held = _initialInputs.find(session.value);
    if (held == _initialInputs.end())
        return;
    // Out of the map first: erasing it destroys the fallback, which may be what is running this.
    auto const pending = std::move(held->second);
    _initialInputs.erase(held);
    if (auto const hosted = _sessions.find(session.value); hosted != _sessions.end())
    {
        sessionLog()("session {}: re-running its restored program", session.value);
        hosted->second->writeInput(pending->input);
    }
}

void SessionHost::initialInputFallbackExpired(void* pending)
{
    auto const& held = *static_cast<PendingInitialInput const*>(pending);
    held.host->typeInitialInput(held.session);
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

    auto const pane = paneUuidFor(request);
    auto const tabUuid = tabUuidFor(request);
    auto const seeded =
        seedSession(request, SpawnIds { .window = windowUuid(window), .tab = tabUuid, .pane = pane });
    if (!seeded)
        return nullptr;

    _stagedTab = StagedTab { .uuid = tabUuid,
                             .label = request.presetIds ? request.presetIds->tabLabel : std::string {} };
    auto* tab = _model.createTab(window);
    _stagedTab.reset();
    _pendingSessionId.reset(); // consumed by the allocator; clear any leftover
    if (tab == nullptr)
    {
        // The model refused: destroy the orphaned backing session.
        errorLog()("model refused createTab; reaping orphaned session {}", seeded->value);
        reapOrphan(*seeded);
        return nullptr;
    }
    return tab;
}

vtworkspace::Window* SessionHost::createWindow(SessionSpawnRequest const& request,
                                               SessionStreamEvents const* creator)
{
    auto const pane = paneUuidFor(request);
    auto const tabUuid = tabUuidFor(request);
    auto const newWindow =
        request.presetIds && request.presetIds->window ? *request.presetIds->window : _objects.mint();
    auto const seeded = seedSession(request, SpawnIds { .window = newWindow, .tab = tabUuid, .pane = pane });
    if (!seeded)
        return nullptr;

    auto* window = _model.createWindow();
    _objects.bind(window->id(), newWindow);
    if (request.presetIds)
        _objects.setLabel(ObjectRef { ObjectKind::Window, newWindow }, request.presetIds->windowLabel);
    // Claimed before the first tab is announced: every client re-decides what it follows on that event,
    // and the primary viewer would otherwise take an unclaimed window that is this creator's own.
    if (creator != nullptr)
        _windowCreators.insert_or_assign(window->id().value, creator);
    _stagedTab = StagedTab { .uuid = tabUuid,
                             .label = request.presetIds ? request.presetIds->tabLabel : std::string {} };
    auto* tab = _model.createTab(window->id());
    _stagedTab.reset();
    _pendingSessionId.reset(); // consumed by the allocator; clear any leftover
    if (tab == nullptr)
    {
        // The model refused the tab: drop the orphaned session and the empty window.
        errorLog()("model refused the first tab of window {}; reaping orphaned session {}",
                   window->id().value,
                   seeded->value);
        reapOrphan(*seeded);
        _windowCreators.erase(window->id().value);
        _objects.forget(window->id());
        _model.removeWindow(window->id());
        return nullptr;
    }
    return window;
}

WindowClaim SessionHost::windowClaim(WindowId window, SessionStreamEvents const* client) const noexcept
{
    auto const creator = _windowCreators.find(window.value);
    if (creator == _windowCreators.end())
        return WindowClaim::Unclaimed;
    return creator->second != nullptr && creator->second == client ? WindowClaim::Yours : WindowClaim::Others;
}

void SessionHost::noteClientAttached(SessionStreamEvents const* client)
{
    if (!std::ranges::contains(_attachedClients, client))
        _attachedClients.push_back(client);
}

void SessionHost::noteClientDetached(SessionStreamEvents const* client)
{
    if (std::erase(_attachedClients, client) == 0)
        return;
    if (_primaryViewer == client)
        _primaryViewer = nullptr;
    // Still claimed, by nobody present: a window a GUI left behind waits for the next launch that takes
    // every window, rather than appearing in another GUI. The address is forgotten, so a connection
    // constructed where this one was is not taken for its creator. Windows gone from the model go too.
    std::erase_if(_windowCreators,
                  [this](auto const& entry) { return _model.window(WindowId { entry.first }) == nullptr; });
    for (auto& [window, creator]: _windowCreators)
        if (creator == client)
            creator = nullptr;
}

void SessionHost::claimPrimaryViewer(SessionStreamEvents const* client)
{
    if (_primaryViewer == nullptr)
        _primaryViewer = client;
}

void SessionHost::splitActivePane(TabId tab,
                                  SplitState orientation,
                                  double ratio,
                                  SessionSpawnRequest const& request)
{
    auto const pane = paneUuidFor(request);
    auto const seeded = seedSession(request,
                                    SpawnIds { .window = windowUuid(_model.windowOfTab(tab)),
                                               .tab = _objects.uuidOf(tab).value_or(Uuid {}),
                                               .pane = pane });
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
        reapOrphan(*seeded);
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

HostedSession const* SessionHost::hostedSession(SessionId session) const noexcept
{
    auto const it = _sessions.find(session.value);
    return it != _sessions.end() ? it->second.get() : nullptr;
}

void SessionHost::replaceProfiles(std::shared_ptr<ProfileCatalog const> profiles)
{
    if (!profiles)
        return;
    _profiles = std::move(profiles);
    auto relimited = std::vector<SessionId> {};
    for (auto const& [id, session]: _sessions)
    {
        // A client that named no profile stated its own settings, which no profile of this host's replaces.
        if (session->settingsOrigin() == SettingsOrigin::Client)
            continue;
        // Normalized as a spawn's settings are, so a reload cannot hand a running session a limit no new
        // session could have.
        auto const limits =
            hostedSessionSettings(_profiles->resolve(session->profileName()).settings).historyLimits;
        auto& terminal = session->terminal();
        auto const guard = std::lock_guard { terminal };
        if (terminal.settings().historyLimits == limits)
            continue;
        terminal.setHistoryLimits(limits);
        relimited.push_back(session->id());
    }
    sessionLog()("profiles reloaded; {} running session(s) took new scrollback limits", relimited.size());
    // Outside every lock, as a resize is announced: a lowered limit evicts history, which the mirrors
    // learn from the next delta.
    for (auto const session: relimited)
        fanOutStream(&SessionStreamEvents::sessionScreenUpdated, session);
}

void SessionHost::writeInput(SessionId session, std::string_view bytes)
{
    if (auto const it = _sessions.find(session.value); it != _sessions.end())
    {
        _lastInput[session.value] = _loop.clock().now();
        it->second->writeInput(bytes);
    }
}

std::optional<core::platform::SteadyTimePoint> SessionHost::lastInputAt(SessionId session) const
{
    if (auto const it = _lastInput.find(session.value); it != _lastInput.end())
        return it->second;
    return std::nullopt;
}

void SessionHost::eraseSession(uint64_t session)
{
    _initialInputs.erase(session);
    _lastInput.erase(session);
    _sessions.erase(session);
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

void SessionHost::attachExtensionControl(vtcontrol::ControlApi* api)
{
    _extensionControl = *api;
}

void SessionHost::detachExtensionControl(vtcontrol::ControlApi* api)
{
    if (&_extensionControl.get() == api)
        _extensionControl = vtcontrol::nullControlApi();
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
    endSession(session, SessionEnd::Exited);
}

void SessionHost::closeSession(SessionId session)
{
    endSession(session, SessionEnd::Requested);
}

PaneRuntime SessionHost::paneRuntime(SessionId session) const
{
    auto const it = _sessions.find(session.value);
    if (it == _sessions.end())
        return {};
    auto& hosted = *it->second;
    auto runtime = PaneRuntime {};
    {
        auto const guard = std::lock_guard { hosted.terminal() };
        if (auto const resolved = hosted.terminal().resolveWorkingDirectory(vtbackend::CwdPurpose::Spawn))
            if (auto path = core::pathFromUtf8(resolved->path))
                runtime.workingDirectory = *std::move(path);
    }
    if (auto const* const child = hosted.process())
    {
        // "." is the lookup's failure value.
        if (auto const directory = child->workingDirectory();
            !runtime.workingDirectory && !directory.empty() && directory != ".")
            if (auto path = core::pathFromUtf8(directory))
                runtime.workingDirectory = *std::move(path);
        runtime.foregroundProcess = child->foregroundProcessName();
    }
    if (!hosted.profileName().empty())
        runtime.profile = hosted.profileName();
    return runtime;
}

void SessionHost::endSession(SessionId session, SessionEnd cause)
{
    auto const it = _sessions.find(session.value);
    if (it == _sessions.end())
        return;

    // Told first, while the pane still has a tab and a window to name.
    for (auto* observer: _streamSubscribers)
        observer->sessionClosing(session);

    // Prune the pane from the model FIRST (prune-then-terminate): closing the
    // pane may fire paneClosed or tabClosed to subscribers while the session
    // still exists; only then destroy the terminal. The session may live in ANY
    // window (a client-created secondary window included).
    if (auto const [window, tab, leaf] = _model.findSessionLeaf(session); leaf != nullptr)
        _model.closePane(window->id(), tab->id(), leaf->id());

    auto const hadExtensions = it->second->hasExtensions();
    eraseSession(session.value);
    // Reached from the pump thread's posted completion, so this runs on the loop thread like
    // every other line in this file.
    sessionLog()("session {} {} ({} live)", session.value, describe(cause), _sessions.size());
    auto rehomed = std::vector<proto::PermissionRequest> {};
    if (hadExtensions)
    {
        rehomed = rehomePermissionRequests(session);
        scheduleRetiredRelease();
    }

    // The session is gone: tell stream observers so they drop any per-session
    // state (delta cursors, sent-hyperlink sets) instead of accumulating it.
    for (auto* observer: _streamSubscribers)
        observer->sessionEnded(session, cause);
    for (auto* observer: _streamSubscribers)
        observer->sessionClosed(session);
    // Forgotten only now: the observers above could still name the pane that closed.
    _objects.forget(session);

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
        // Called on the extension's worker; served on the loop, as the session's pane, by whatever control
        // API is attached when the call gets there.
        .callControl =
            [bridge =
                 control::LoopControlBridge {
                     _loop,
                     [gate = _gate, hosted, session]() -> std::optional<control::ExtensionCaller> {
                         if (!hosted())
                             return std::nullopt;
                         auto& host = *gate->host;
                         auto const uuid = host._objects.uuidOf(session);
                         if (!uuid)
                             return std::nullopt;
                         return control::ExtensionCaller {
                             .api = host._extensionControl.get(),
                             .pane = vtcontrol::toString(
                                 vtcontrol::ObjectRef { .kind = vtcontrol::ObjectKind::Pane, .uuid = *uuid }),
                         };
                     } }](std::string_view method,
                          std::string_view params,
                          vtcontrol::ControlGrants granted,
                          std::chrono::milliseconds timeout) {
                return bridge.call(method, params, granted, timeout);
            },
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
    // Named before the fan-out, so an observer told of the tab can already look its UUID up.
    if (_stagedTab)
    {
        _objects.bind(tab, _stagedTab->uuid);
        _objects.setLabel(ObjectRef { ObjectKind::Tab, _stagedTab->uuid }, _stagedTab->label);
        _stagedTab.reset();
    }
    fanOut(&vtworkspace::ModelEvents::tabAdded, window, tab, index);
}

void SessionHost::tabClosed(WindowId window, TabId tab, int index)
{
    fanOut(&vtworkspace::ModelEvents::tabClosed, window, tab, index);
    // Forgotten after the fan-out, so an observer told of the close can still name the tab.
    _objects.forget(tab);
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
