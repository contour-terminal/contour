// SPDX-License-Identifier: Apache-2.0
#include <vthost/keeper/Keeper.hpp>

#include <algorithm>
#include <expected>
#include <ranges>
#include <utility>
#include <variant>

#include <vthost/keeper/Codec.hpp>
#include <vthost/keeper/Logging.hpp>

namespace vthost::keeper
{

namespace
{
    /// The status a process exits with to report @p status: its code, or 128 + the signal, as a shell does.
    [[nodiscard]] constexpr int exitStatusOf(WireExitStatus status) noexcept
    {
        constexpr auto SignalBase = 128;
        return status.kind == ExitKind::Signal ? SignalBase + status.value : status.value;
    }

    /// The status the keeper exits with when it gave up: non-zero, so a service manager's limits apply.
    constexpr auto GaveUpExitStatus = 1;

    /// The status the keeper exits with when not even a first daemon could be launched.
    constexpr auto LaunchFailedExitStatus = 1;

    void earliest(std::optional<core::platform::SteadyTimePoint>& slot,
                  core::platform::SteadyTimePoint candidate)
    {
        slot = slot ? std::min(*slot, candidate) : candidate;
    }
} // namespace

Keeper::Keeper(KeeperPorts ports,
               std::unique_ptr<LabelLock> labelLock,
               RestartPolicyConfig policy,
               std::filesystem::path daemonImage,
               std::vector<uint8_t> launchOptions):
    _ports { ports },
    _labelLock { std::move(labelLock) },
    _policyConfig { policy },
    _policy { policy },
    _daemonImage { std::move(daemonImage) },
    _launchOptions { std::move(launchOptions) }
{
}

KeeperOutcome Keeper::run()
{
    launchDaemon(LaunchContinuity::Fresh);
    // Checked before every wait: an end that needs nothing more (a first launch that failed) must not wait
    // for an event that never comes.
    while (!hasEnded())
    {
        auto const deadline = nextDeadline();
        auto events = _ports.events.wait(deadline);
        forgiveOwnStop(deadline);
        _descriptorsLeft.reset();
        for (auto& event: events)
            onEvent(std::move(event));
        onTimers();
    }
    _outcome = _ending;
    return *_outcome;
}

// {{{ the daemon's life

void Keeper::launchDaemon(LaunchContinuity continuity)
{
    auto launched = _ports.launcher.launch(_daemonImage);
    if (!launched)
    {
        if (continuity == LaunchContinuity::Fresh)
            beginEnding(KeeperOutcome::DaemonFailedToStart, LaunchFailedExitStatus);
        else
            apply(_policy.onExit(DaemonExitKind::Crashed, now()), {});
        return;
    }

    auto const fresh = continuity == LaunchContinuity::Fresh;
    _daemon = Daemon {
        .pid = launched->pid,
        .generation = DaemonGeneration { ++_lastGeneration },
        .channel = std::move(launched->channel),
        .state = fresh ? DaemonEndState::StartingFirst : DaemonEndState::StartingRestart,
        .inventory = fresh ? InventoryDelivery::Delivered : InventoryDelivery::Owed,
        .startedAt = now(),
        .hungRestart = HungRestart::Off,
        .heartbeatTimeout = DefaultHeartbeatTimeout,
        .beats = Beats::NoneYet,
        .lastHeartbeat = now(),
        .killAt = std::nullopt,
        .stableAt = std::nullopt,
        .exited = std::nullopt,
        .drainUntil = std::nullopt,
        .greeting = Greeting::Awaited,
        .channelVersion = 0,
    };
    _ports.events.watchChannel(*_daemon->channel, _daemon->generation);
    // The keeper reaps the daemon itself, after its last signal: until then its zombie holds the pid.
    _ports.events.watchPid(
        WatchedProcess { .pid = _daemon->pid, .role = WatchedRole::Daemon, .processHandle = -1 });
    send(Hello {});
    send(Launch { .continuity = continuity,
                  .options = _launchOptions,
                  .originalDescriptorLimit = _ports.budget.originalSoftLimit() });
}

void Keeper::onEnded(int pid, WireExitStatus status)
{
    if (_daemon && _daemon->pid == pid)
    {
        // Judged only once its channel is drained: a Shutdown it wrote before it exited still counts.
        _daemon->exited = status;
        _daemon->killAt.reset();
        if (_daemon->channel)
            _daemon->drainUntil = now() + ChannelDrainBound;
        else
            judgeDaemonExit();
        return;
    }

    auto const session = _sessions.reap(pid, status);
    if (!session)
        return; // no process the keeper watches
    // The table no longer offers it to be signalled (SessionTable::signallableChild()): its pid may go.
    _ports.events.reap(pid);
    if (session->fate == ReapedFate::Kept && linked())
        send(Exited { .session = std::to_underlying(session->id), .status = status });
}

void Keeper::judgeDaemonExit()
{
    auto const kind = classifyExit(_daemon->state);
    auto const status = _daemon->exited.value_or(WireExitStatus {});
    auto const pid = _daemon->pid;
    dropChannel();
    _daemon.reset();
    // Judged, and nothing more is sent to it: only now may its pid go.
    _ports.events.reap(pid);
    if (kind == DaemonExitKind::Crashed || kind == DaemonExitKind::Hung)
        keeperLog()("the daemon (pid {}) {} with {} {}{}",
                    pid,
                    kind == DaemonExitKind::Hung ? "was killed as hung, and ended" : "crashed",
                    status.kind == ExitKind::Signal ? "signal" : "exit status",
                    status.value,
                    _ending ? "" : "; restarting it as the restart policy allows");
    if (!_ending)
        apply(_policy.onExit(kind, now()), status);
}

void Keeper::onLinkLost()
{
    if (!_daemon)
        return;
    auto const hadChannel = _daemon->channel != nullptr;
    dropChannel();
    if (_daemon->exited)
    {
        judgeDaemonExit();
        return;
    }
    if (!hadChannel)
        return; // lost already, and dealt with then
    switch (_daemon->state)
    {
        case DaemonEndState::ShutdownAnnounced:
        case DaemonEndState::StopForwarded:
        case DaemonEndState::HungKilled:
        case DaemonEndState::FirstStartKilled: return; // it is ending, and a kill is already scheduled
        case DaemonEndState::StartingFirst:
            // A first start that fails closes its channel on its way out, before it has exited: its own
            // status is what the keeper exits with, so it is given the drain bound to end before it is
            // killed.
            earliest(_daemon->killAt, now() + ChannelDrainBound);
            return;
        case DaemonEndState::StartingRestart:
        case DaemonEndState::Serving: break;
    }
    // Gone without Shutdown: a crash, whatever the process does now.
    _ports.signaller.send(_daemon->pid, KeeperSignal::Kill);
}

void Keeper::onStopSignal()
{
    if (_ending)
        return;
    if (!_daemon)
    {
        beginEnding(KeeperOutcome::Clean, 0);
        return;
    }
    if (_daemon->state == DaemonEndState::ShutdownAnnounced
        || _daemon->state == DaemonEndState::StopForwarded)
        return;
    if (_daemon->exited)
    {
        // Already gone, its channel not yet drained: a stop asked for now is not a crash to restart from.
        _daemon->state = DaemonEndState::StopForwarded;
        return;
    }
    _ports.signaller.send(_daemon->pid, KeeperSignal::Terminate);
    _daemon->state = DaemonEndState::StopForwarded;
    earliest(_daemon->killAt, now() + DaemonExitBound);
}

void Keeper::forgiveOwnStop(core::platform::SteadyTimePoint deadline)
{
    if (!_daemon || deadline == core::platform::SteadyTimePoint::max())
        return;
    // Any overrun past scheduling noise: the deadline is the daemon's last beat plus its timeout, so a stop
    // that began mid-silence overruns it by less than the timeout, and the beat the continued daemon sends is
    // read only after this wait's timers would judge it.
    auto const overrun = now() - deadline;
    if (overrun <= MinHeartbeatTimeout)
        return;
    // A wait that returns this long after its deadline was not run: the keeper itself was stopped (Ctrl-Z on
    // a foreground `contour daemon` stops the daemon with it). The daemon's silence is its own stop's, not a
    // hang, and the heartbeats it sends once continued have yet to be read: it is measured afresh.
    keeperLog()("the keeper did not run for {} ms past its deadline; the daemon's silence is measured afresh",
                std::chrono::duration_cast<std::chrono::milliseconds>(overrun).count());
    _daemon->lastHeartbeat = now();
    _daemon->startedAt = now();
}

void Keeper::onTimers()
{
    auto const time = now();
    for (auto const pid: _sessions.takeDueKills(time))
        _ports.signaller.send(pid, KeeperSignal::Kill);

    if (_daemon && _daemon->drainUntil && *_daemon->drainUntil <= time)
        judgeDaemonExit();
    else if (_daemon && !_daemon->exited)
    {
        auto& daemon = *_daemon;
        if (daemon.killAt && *daemon.killAt <= time)
        {
            _ports.signaller.send(daemon.pid, KeeperSignal::Kill);
            daemon.killAt.reset();
        }
        if (auto const deadline = silenceDeadline(); deadline && *deadline <= time)
        {
            // Hung: abort it for the core, continue it in case it is stopped, kill it if that does not do.
            auto const beating = daemon.state == DaemonEndState::Serving || daemon.beats == Beats::Heard;
            keeperLog()("the daemon (pid {}) is hung: {} for {} ms; aborting it",
                        daemon.pid,
                        beating ? "no heartbeat" : "not serving",
                        std::chrono::duration_cast<std::chrono::milliseconds>(
                            time - (beating ? daemon.lastHeartbeat : daemon.startedAt))
                            .count());
            _ports.signaller.send(daemon.pid, KeeperSignal::Abort);
            _ports.signaller.send(daemon.pid, KeeperSignal::Continue);
            // A first start that hangs is a failed start: the keeper exits rather than restarts.
            daemon.state = daemon.state == DaemonEndState::StartingFirst ? DaemonEndState::FirstStartKilled
                                                                         : DaemonEndState::HungKilled;
            earliest(daemon.killAt, time + HungKillDelay);
        }
        if (daemon.stableAt && *daemon.stableAt <= time)
        {
            _policy.onStable(time);
            daemon.stableAt.reset();
        }
    }

    if (_relaunchAt && *_relaunchAt <= time)
    {
        _relaunchAt.reset();
        launchDaemon(LaunchContinuity::Recovered);
    }
    if (_graceAt && *_graceAt <= time)
    {
        _graceAt.reset();
        apply(_policy.onGraceElapsed(time), {});
    }
}

void Keeper::apply(RestartDecision decision, WireExitStatus status)
{
    switch (decision.action)
    {
        case RestartAction::RestartNow:
        case RestartAction::RetryNewestImage: launchDaemon(LaunchContinuity::Recovered); break;
        case RestartAction::RestartAfter: _relaunchAt = now() + decision.delay; break;
        case RestartAction::GiveUp: _graceAt = now() + decision.delay; break;
        case RestartAction::EndAll: beginEnding(KeeperOutcome::GaveUp, GaveUpExitStatus); break;
        case RestartAction::ExitWithStatus:
            beginEnding(KeeperOutcome::DaemonFailedToStart, exitStatusOf(status));
            break;
        case RestartAction::ExitClean: beginEnding(KeeperOutcome::Clean, 0); break;
        case RestartAction::Ignore: break;
    }
}

void Keeper::beginEnding(KeeperOutcome outcome, int exitStatus)
{
    if (_ending)
        return;
    _ending = outcome;
    _exitStatus = exitStatus;
    _labelLock.reset();
    _listeners.clear();
    _structure.reset();
    _relaunchAt.reset();
    _graceAt.reset();
    _sessions.releaseAll(now() + ShellHangupGrace);
}

void Keeper::dropChannel()
{
    if (!_daemon || !_daemon->channel)
        return;
    _ports.events.unwatchChannel(*_daemon->channel);
    _daemon->channel.reset();
}

void Keeper::send(Message const& message, std::vector<int> fds)
{
    if (_daemon && _daemon->channel)
        _daemon->channel->send(Frame { .body = encode(message), .fds = std::move(fds) });
}

bool Keeper::hasEnded() const noexcept
{
    return _ending && !_daemon && _sessions.empty();
}

bool Keeper::takesIntake() const noexcept
{
    if (_ending || !_daemon || _daemon->exited)
        return false;
    switch (_daemon->state)
    {
        case DaemonEndState::StartingFirst:
        case DaemonEndState::StartingRestart:
        case DaemonEndState::Serving:
        case DaemonEndState::StopForwarded: return true; // still serving its clients until it exits
        case DaemonEndState::ShutdownAnnounced:
        case DaemonEndState::HungKilled:
        case DaemonEndState::FirstStartKilled: break;
    }
    return false;
}

bool Keeper::linked() const noexcept
{
    return _daemon && _daemon->channel && _daemon->inventory == InventoryDelivery::Delivered;
}

std::optional<core::platform::SteadyTimePoint> Keeper::silenceDeadline() const
{
    if (!_daemon || _daemon->exited)
        return std::nullopt;
    switch (_daemon->state)
    {
        case DaemonEndState::Serving:
            if (_daemon->hungRestart == HungRestart::On)
                return _daemon->lastHeartbeat + _daemon->heartbeatTimeout;
            return std::nullopt;
        case DaemonEndState::StartingFirst:
        case DaemonEndState::StartingRestart:
            // A start is bounded until it first says it is alive, and judged by its heartbeats from then on:
            // one that restores at length while it beats is not hung. Each silence of a start may last as
            // long as the startup bound allows a whole start, which a user raises for a slow restore.
            if (_daemon->beats == Beats::Heard)
                return _daemon->lastHeartbeat
                       + std::max(_daemon->heartbeatTimeout, _policyConfig.restartStartupBound);
            return _daemon->startedAt + _policyConfig.restartStartupBound;
        case DaemonEndState::ShutdownAnnounced:
        case DaemonEndState::StopForwarded:
        case DaemonEndState::HungKilled:
        case DaemonEndState::FirstStartKilled: break;
    }
    return std::nullopt;
}

core::platform::SteadyTimePoint Keeper::nextDeadline() const
{
    auto next = _sessions.nextKill();
    for (auto const& candidate: { _relaunchAt,
                                  _graceAt,
                                  _daemon ? _daemon->killAt : std::nullopt,
                                  _daemon ? _daemon->stableAt : std::nullopt,
                                  _daemon ? _daemon->drainUntil : std::nullopt,
                                  silenceDeadline() })
        if (candidate)
            earliest(next, *candidate);
    return next.value_or(core::platform::SteadyTimePoint::max());
}

// }}}
// {{{ events and frames

void Keeper::onEvent(Event event)
{
    switch (event.kind)
    {
        case EventKind::Frame:
            if (!event.frame)
                break;
            if (!_daemon || event.generation != _daemon->generation || !_daemon->channel)
            {
                // A dead daemon's late frame, or one after its link was dropped: never acted on.
                closeAll(event.frame->fds);
                break;
            }
            onFrame(std::move(*event.frame));
            break;
        case EventKind::ChannelClosed:
            if (_daemon && event.generation == _daemon->generation)
                onLinkLost();
            break;
        case EventKind::Ended: onEnded(event.pid, event.status); break;
        case EventKind::StopSignal: onStopSignal(); break;
        case EventKind::Timer: break; // onTimers() runs after every wait
    }
}

void Keeper::onFrame(Frame frame)
{
    auto fds = std::vector<UniqueFd> {};
    for (auto const fd: frame.fds)
        fds.emplace_back(fd);

    auto message = decodeFrame(frame);
    if (!message)
    {
        // A newer daemon's message is answered and the link kept: a v1 keeper serves every daemon a live
        // upgrade or a restart runs. Its descriptors close with `fds`.
        auto const tag = peekTag(frame.body);
        if (tag && failureOf(message.error()) == DecodeFailure::Unsupported)
        {
            keeperLog()(
                "the daemon sent message {}, which this keeper cannot read (codec error {}); answered "
                "Unsupported",
                *tag,
                std::to_underlying(message.error()));
            send(Unsupported { .tag = *tag });
            return;
        }
        onLinkLost(); // a daemon whose frame is cut short is broken
        return;
    }
    std::visit([&](auto const& alternative) { handle(alternative, fds); }, *message);
}

void Keeper::handle(Hello const& message, std::vector<UniqueFd>& /*fds*/)
{
    if (!_daemon)
        return;
    // Each image speaks its own version: a live upgrade's new one may speak a newer one than the last.
    _daemon->channelVersion = negotiate(ChannelVersion, message.version);
    if (_daemon->greeting == Greeting::Awaited)
    {
        _daemon->greeting = Greeting::Greeted;
        return;
    }
    // The same process speaks again: a live upgrade exec'd a new image in place, which kept the channel and
    // is owed a Launch of its own. It takes the sessions over from the handover, not from an inventory.
    keeperLog()("the daemon (pid {}) runs a new image; it carries on from its handover", _daemon->pid);
    send(Hello {});
    send(Launch { .continuity = LaunchContinuity::HandedOver,
                  .options = _launchOptions,
                  .originalDescriptorLimit = _ports.budget.originalSoftLimit() });
    // An exit the previous image may have heard and not acted on is the new image's to hear: the handover
    // image carries no status, so the sessions that ended and are still held are told again.
    for (auto const& entry: _sessions.inventory())
        if (entry.message.exited)
            send(Exited { .session = entry.message.session, .status = *entry.message.exited });
}

void Keeper::handle(Listener const& message, std::vector<UniqueFd>& fds)
{
    if (fds.size() != 1 || !takesIntake())
        return; // closed at once: nobody would hand it on

    auto kept = KeptListener { .name = message.name,
                               .tlsIdentity = message.tlsIdentity,
                               .path = message.path,
                               .fd = std::move(fds.front()) };
    // Deposited again by the new image of a live upgrade: the same socket, held once.
    if (auto const held = std::ranges::find(_listeners, message.name, &KeptListener::name);
        held != _listeners.end())
        *held = std::move(kept);
    else
        _listeners.push_back(std::move(kept));
}

void Keeper::handle(Serving const& message, std::vector<UniqueFd>& /*fds*/)
{
    if (!_daemon)
        return;
    auto& daemon = *_daemon;
    if (daemon.state == DaemonEndState::Serving)
    {
        // The new image of a live upgrade serves on, and may run with other settings than the one before.
        adoptKeepingSettings(daemon, message);
        return;
    }
    if (daemon.state != DaemonEndState::StartingFirst && daemon.state != DaemonEndState::StartingRestart)
        return;
    auto const time = now();
    daemon.state = DaemonEndState::Serving;
    adoptKeepingSettings(daemon, message);
    daemon.stableAt = time + _policyConfig.stableAfter;
}

void Keeper::adoptKeepingSettings(Daemon& daemon, Serving const& message)
{
    daemon.hungRestart = message.hungRestart;
    daemon.heartbeatTimeout =
        std::max(std::chrono::milliseconds { message.heartbeatTimeoutMs }, MinHeartbeatTimeout);
    if (std::chrono::milliseconds { message.heartbeatTimeoutMs } < MinHeartbeatTimeout)
        keeperLog()("the daemon's heartbeat timeout of {} ms is below the minimum; {} ms is used",
                    message.heartbeatTimeoutMs,
                    MinHeartbeatTimeout.count());
    daemon.lastHeartbeat = now();
}

void Keeper::handle(Spawn const& message, std::vector<UniqueFd>& fds)
{
    auto const spawned = spawnSession(message, fds);
    if (!spawned)
    {
        send(SpawnFailed { .request = message.request, .error = spawned.error() });
        return;
    }
    auto replyFds = std::vector<int> {};
    if (spawned->processHandle >= 0)
        replyFds.push_back(spawned->processHandle);
    send(Spawned { .request = message.request,
                   .session = std::to_underlying(spawned->id),
                   .pid = spawned->pid },
         std::move(replyFds));
}

std::expected<Keeper::SpawnedSession, SpawnError> Keeper::spawnSession(Spawn const& message,
                                                                       std::vector<UniqueFd>& fds)
{
    if (!takesIntake())
        return std::unexpected { SpawnError::ShuttingDown };
    auto const childCount = message.childFds.size();
    auto const executableCount = message.executable == ExecutableSource::Descriptor ? 1U : 0U;
    if (fds.size() != childCount + message.kept.size() + executableCount)
        return std::unexpected { SpawnError::Unknown };
    auto const executable = message.executable == ExecutableSource::Descriptor
                                ? std::optional { fds.back().get() }
                                : std::nullopt;
    // The child's pidfd is the one more descriptor a spawn keeps open here: what it was sent is open already.
    if (!_descriptorsLeft)
        _descriptorsLeft = _ports.budget.available();
    if (*_descriptorsLeft < SpawnDescriptorReserve + 1)
        return std::unexpected { SpawnError::TooManyDescriptors };

    auto descriptors = SessionTable::Descriptors {};
    for (auto&& [role, fd]: std::views::zip(message.kept, fds | std::views::drop(childCount)))
    {
        auto& slot = descriptors[slotOf(role)];
        if (role == KeptDescriptor::ProcessHandle || slot.isOpen())
            return std::unexpected { SpawnError::Unknown };
        slot = std::move(fd);
    }

    auto const childDescriptors = std::ranges::to<std::vector>(fds | std::views::take(childCount)
                                                               | std::views::transform(&UniqueFd::get));
    auto spawned = _ports.spawner.spawn(message, childDescriptors, executable);
    if (!spawned)
        return std::unexpected { spawned.error() };

    // Reaped by the keeper as its Ended is handled (onEnded()): until then its zombie holds the pid, so that
    // a Signal handled from the same wait() before the Ended never reaches another process. Watched by the
    // pidfd the spawn opened, which the session table keeps open until then (SessionTable::release()).
    auto const processHandle = spawned->processHandle.get();
    _ports.events.watchPid(
        WatchedProcess { .pid = spawned->pid, .role = WatchedRole::Session, .processHandle = processHandle });
    if (spawned->processHandle.isOpen())
        --*_descriptorsLeft;
    descriptors[slotOf(KeptDescriptor::ProcessHandle)] = std::move(spawned->processHandle);
    return SpawnedSession { .id = _sessions.add(spawned->pid, std::move(descriptors)),
                            .pid = spawned->pid,
                            .processHandle = processHandle };
}

void Keeper::handle(Placed const& message, std::vector<UniqueFd>& /*fds*/)
{
    _sessions.place(KeeperSessionId { message.session });
}

void Keeper::handle(Release const& message, std::vector<UniqueFd>& /*fds*/)
{
    _sessions.release(KeeperSessionId { message.session },
                      now() + std::chrono::milliseconds { message.graceMs });
}

void Keeper::handle(Structure const& /*message*/, std::vector<UniqueFd>& fds)
{
    if (fds.size() == 1 && !_ending)
        _structure = std::move(fds.front());
}

void Keeper::handle(Heartbeat const& /*message*/, std::vector<UniqueFd>& /*fds*/)
{
    if (!_daemon)
        return;
    _daemon->lastHeartbeat = now();
    _daemon->beats = Beats::Heard;
}

void Keeper::handle(Adopt const& /*message*/, std::vector<UniqueFd>& /*fds*/)
{
    if (!_daemon)
        return;
    auto const sessions = _sessions.inventory();
    auto structureFds = std::vector<int> {};
    if (_structure.isOpen())
        structureFds.push_back(_structure.get());
    send(InventoryBegin { .daemonImagePath = _daemonImage.string(),
                          .sessionCount = static_cast<uint32_t>(sessions.size()),
                          .listenerCount = static_cast<uint32_t>(_listeners.size()) },
         std::move(structureFds));
    for (auto const& listener: _listeners)
        send(InventoryListener { .name = listener.name,
                                 .tlsIdentity = listener.tlsIdentity,
                                 .path = listener.path },
             { listener.fd.get() });
    for (auto const& session: sessions)
        send(session.message, session.fds);
    send(InventoryEnd {});
    _daemon->inventory = InventoryDelivery::Delivered;
}

void Keeper::handle(Shutdown const& /*message*/, std::vector<UniqueFd>& /*fds*/)
{
    if (!_daemon)
        return;
    // The label is free for a new instance and the listeners close at once; the shells wait for the
    // daemon's exit, so its final snapshot still sees them.
    _labelLock.reset();
    _listeners.clear();
    _daemon->state = DaemonEndState::ShutdownAnnounced;
    earliest(_daemon->killAt, now() + DaemonExitBound);
}

void Keeper::handle(Signal const& message, std::vector<UniqueFd>& /*fds*/)
{
    // Never a child reaped already: its pid may name another process by now.
    auto const pid = _sessions.signallableChild(KeeperSessionId { message.session });
    if (!pid)
        return;
    switch (message.signal)
    {
        case ChildSignal::Terminate: _ports.signaller.send(*pid, KeeperSignal::Terminate); break;
        case ChildSignal::Kill: _ports.signaller.send(*pid, KeeperSignal::Kill); break;
    }
}

// }}}

} // namespace vthost::keeper
