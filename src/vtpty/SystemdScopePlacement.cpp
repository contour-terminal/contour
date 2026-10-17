// SPDX-License-Identifier: Apache-2.0
#include <vtpty/SystemdScopePlacement.hpp>

#include <vtpty/Pty.hpp>

#include <core/log/LogStore.hpp>

#include <algorithm>
#include <csignal>
#include <exception>
#include <format>
#include <utility>

#include <pthread.h>
#include <unistd.h>

namespace vtpty
{

PlacementLogLine describe(PlacementReport const& report)
{
    // A process released unplaced for a fault is told where a user sees it -- each one, as each is a
    // session an out-of-memory kill can take Contour down with -- and so is its late placement.
    auto const severity = report.reason ? traitsOf(*report.reason).severity : ScopeErrorSeverity::Expected;
    auto const why = report.reason ? traitsOf(*report.reason).description : std::string_view {};
    // A switch rather than a table: switch exhaustiveness names this spot when a status is added.
    switch (report.status)
    {
        case PlacementStatus::Placed:
            return { .severity = ScopeErrorSeverity::Expected,
                     .text =
                         std::format("Session process {} runs in a systemd scope of its own.", report.pid) };
        case PlacementStatus::Deferred:
            return { .severity = severity,
                     .text =
                         std::format("Session process {} runs unplaced for now: {}. It moves into a systemd "
                                     "scope of its own once systemd answers again; until then, an "
                                     "out-of-memory kill inside it can stop Contour with it.",
                                     report.pid,
                                     why) };
        case PlacementStatus::PlacedLate:
            return { .severity = severity,
                     .text =
                         std::format("Session process {} moved into a systemd scope of its own after all. "
                                     "What it started before then stays in Contour's own unit.",
                                     report.pid) };
        case PlacementStatus::Unplaced:
            return { .severity = severity,
                     .text = std::format("Session process {} could not be moved into a systemd scope of its "
                                         "own: {}. An out-of-memory kill inside it can stop Contour with it.",
                                         report.pid,
                                         why) };
        case PlacementStatus::Ended:
            return { .severity = ScopeErrorSeverity::Expected,
                     .text = std::format(
                         "Session process {} ended before it could be moved into a systemd scope of its own.",
                         report.pid) };
    }
    std::unreachable();
}

void logPlacement(PlacementReport const& report)
{
    auto const line = describe(report);
    if (line.severity == ScopeErrorSeverity::Fault)
        errorLog()("{}", line.text);
    else
        ptyPlacementLog()("{}", line.text);
}

SystemdScopePlacement::SystemdScopePlacement(SystemdScopeConfig config,
                                             ScopeBusFactory connectBus,
                                             ProcessCgroupReader readCgroup,
                                             PlacementReporter report,
                                             core::platform::IClock const& clock):
    _config { std::move(config) },
    _connectBus { std::move(connectBus) },
    _readCgroup { std::move(readCgroup) },
    _report { std::move(report) },
    _clock { clock },
    _breaker { _config.breakerCooldown }
{
}

SystemdScopePlacement::~SystemdScopePlacement()
{
    {
        auto const _ = std::lock_guard { _mutex };
        _workerState = WorkerState::Stopping;
    }
    _wakeup.notify_one();
    if (_worker.joinable())
        _worker.join();
    // What the worker left queued is released as _queue is destroyed. Deferred children run already.
}

void SystemdScopePlacement::placeThenRelease(ParkedChild child) noexcept
{
    try
    {
        std::call_once(_workerStarted, [this] { startWorker(); });
        auto const _ = std::lock_guard { _mutex };
        _queue.push_back(std::move(child));
    }
    catch (std::exception const& e)
    {
        // No worker, or no room for one more child. It runs unplaced -- `child` releases it on the
        // way out -- rather than fail the spawn; the next spawn tries to start the worker again.
        errorLog()("Session process {} could not be queued for a systemd scope of its own: {}.",
                   child.pid(),
                   e.what());
        return;
    }
    _wakeup.notify_one();
}

void SystemdScopePlacement::startWorker()
{
    // Started with every signal blocked, which it inherits. Signals meant for the process -- the
    // daemon waits for SIGTERM and SIGINT with sigwait() on a thread of its own -- must not land
    // here, where their default action would end the process.
    auto all = sigset_t {};
    auto previous = sigset_t {};
    sigfillset(&all);
    pthread_sigmask(SIG_SETMASK, &all, &previous);
    try
    {
        _worker = std::thread { [this] { run(); } };
    }
    catch (...)
    {
        pthread_sigmask(SIG_SETMASK, &previous, nullptr);
        throw;
    }
    pthread_sigmask(SIG_SETMASK, &previous, nullptr);
}

void SystemdScopePlacement::run()
{
#ifdef __linux__
    pthread_setname_np(pthread_self(), WorkerThreadName); // what `ps -T` and debuggers show
#endif
    while (true)
    {
        auto child = std::optional<ParkedChild> {};
        {
            auto lock = std::unique_lock { _mutex };
            awaitWork(lock);
            if (_workerState == WorkerState::Stopping)
                return;
            // A queued child before a deferred one: it is parked, and its tab waits for it.
            if (!_queue.empty())
            {
                child.emplace(std::move(_queue.front()));
                _queue.pop_front();
            }
        }
        if (child)
            place(*child);
        else
            placeLate();
    } // each iteration's child is released as it goes out of scope
}

void SystemdScopePlacement::awaitWork(std::unique_lock<std::mutex>& lock)
{
    // The deferred children and the breaker are the worker's alone; the lock guards the queue and the
    // state, which the GUI thread and the destructor change.
    while (_workerState == WorkerState::Running && _queue.empty() && !isLatePlacementDue())
    {
        if (_deferred.empty())
            _wakeup.wait(lock);
        else
            // The breaker measures the cooldown on the injected clock; it is waited out in real time.
            // A clock a test moves by hand is therefore looked at again once per cooldown at most.
            _wakeup.wait_for(lock, _breaker.cooldownLeft(_clock.now()));
    }
}

bool SystemdScopePlacement::isLatePlacementDue() const noexcept
{
    return !_deferred.empty() && _breaker.shouldAttempt(_clock.now());
}

void SystemdScopePlacement::place(ParkedChild& child)
{
    // Its tab closed while it waited here: asking about its pid could move whatever reuses it now,
    // and a refusal would be reported as a fault over a closed tab.
    if (child.hasExited())
        return;
    if (auto const heldOffBy = _breaker.heldOffBy(_clock.now()))
    {
        defer(child, *heldOffBy);
        return;
    }

    auto const outcome = attempt(ScopeTarget { .pid = child.pid(), .pidfd = child.pidfd() });
    if (outcome)
        report(child.pid(), PlacementStatus::Placed, std::nullopt);
    else if (traitsOf(outcome.error()).health == BusHealth::Unresponsive)
        defer(child, outcome.error()); // asked again once the breaker lets a request through
    else
        report(child.pid(), PlacementStatus::Unplaced, outcome.error()); // systemd said no
}

void SystemdScopePlacement::defer(ParkedChild& child, ScopeError reason)
{
    auto unplaced = child.releaseUnplaced();
    // Without a pidfd there would be no telling, later, whether the pid still names this child.
    if (unplaced.pidfd() < 0)
    {
        report(unplaced.pid(), PlacementStatus::Unplaced, reason);
        return;
    }
    report(unplaced.pid(), PlacementStatus::Deferred, reason);
    _deferred.push_back(DeferredChild { .child = std::move(unplaced), .reason = reason });
}

void SystemdScopePlacement::placeLate()
{
    // Those that exited are let go first: their pids may name other processes by now.
    auto const ended = std::ranges::stable_partition(
        _deferred, [](DeferredChild const& deferred) { return !deferred.child.hasExited(); });
    for (auto const& gone: ended)
        report(gone.child.pid(), PlacementStatus::Ended, std::nullopt);
    _deferred.erase(ended.begin(), ended.end());
    if (_deferred.empty())
        return;

    // The oldest goes first, and is the trial when the cooldown has just passed. systemd moves the
    // running process -- by pidfd where it takes one, which cannot name another process; by pid
    // otherwise, checked alive just above -- but not what it has started meanwhile.
    auto const& oldest = _deferred.front();
    auto const outcome = attempt(ScopeTarget { .pid = oldest.child.pid(), .pidfd = oldest.child.pidfd() });
    if (!outcome && traitsOf(outcome.error()).health == BusHealth::Unresponsive)
        return; // still silent: it waits for the next trial, and was told about already
    if (outcome)
        report(oldest.child.pid(), PlacementStatus::PlacedLate, oldest.reason);
    else
        report(oldest.child.pid(), PlacementStatus::Unplaced, outcome.error());
    _deferred.pop_front();
}

std::expected<void, ScopeError> SystemdScopePlacement::attempt(ScopeTarget target)
{
    auto const outcome = [&]() -> std::expected<void, ScopeError> {
        if (!_bus)
        {
            auto connected = _connectBus();
            if (!connected)
                return std::unexpected(connected.error());
            _bus = std::move(*connected);
            _protocol = ScopeProtocol::Current; // a new connection may reach a newer systemd
        }
        // systemd before 253 knows neither PIDFDs nor OOMPolicy on scopes, and refuses the whole
        // request over either. Speak what it knows from now on.
        if (auto result = request(target, _protocol);
            result || result.error() != ScopeError::Unsupported || _protocol != ScopeProtocol::Current)
            return result;
        _protocol = ScopeProtocol::Legacy;
        return request(target, _protocol);
    }();

    if (!outcome && outcome.error() == ScopeError::Disconnected)
        _bus.reset(); // connect afresh on the next attempt
    _breaker.record(outcome, _clock.now());
    return outcome;
}

std::expected<void, ScopeError> SystemdScopePlacement::request(ScopeTarget target, ScopeProtocol protocol)
{
    auto const ownPid = ::getpid();
    auto const scope = ScopeRequest {
        .unitName = std::format("{}-{}-{}-{}.scope", _config.unitPrefix, ownPid, target.pid, ++_sequence),
        .slice = _config.slice,
        .description = std::format("Contour session (contour pid {})", ownPid),
        .protocol = protocol,
        .pid = target.pid,
        .pidfd = target.pidfd,
        .memoryLimit = _config.memoryLimit,
    };
    auto const outcome = _bus->startScope(scope, _config.deadline);
    // A late answer is still an answer. The job may have finished in time and its answer arrived late,
    // or not at all; the kernel knows. A child that runs in its scope was placed, and the bus that
    // placed it is not to be held off.
    if (!outcome && outcome.error() == ScopeError::TimedOut && runsIn(target.pid, scope.unitName))
    {
        ptyPlacementLog()("Session process {} runs in {}, though systemd did not answer in time.",
                          target.pid,
                          scope.unitName);
        return {};
    }
    return outcome;
}

bool SystemdScopePlacement::runsIn(int pid, std::string_view unit) const
{
    auto const cgroup = _readCgroup(pid);
    return cgroup && isInUnit(*cgroup, unit);
}

void SystemdScopePlacement::report(int pid, PlacementStatus status, std::optional<ScopeError> reason) const
{
    _report(PlacementReport { .pid = pid, .status = status, .reason = reason });
}

} // namespace vtpty
