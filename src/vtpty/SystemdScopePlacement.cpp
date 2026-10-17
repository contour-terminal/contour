// SPDX-License-Identifier: Apache-2.0
#include <vtpty/SystemdScopePlacement.hpp>

#include <vtpty/Pty.hpp>

#include <core/log/LogStore.hpp>

#include <algorithm>
#include <csignal>
#include <cstddef>
#include <cstdio>
#include <exception>
#include <format>
#include <iterator>
#include <utility>

#include <pthread.h>
#include <unistd.h>

namespace vtpty
{

namespace
{
    /// @return Whether a child released unplaced for @p reason is to be placed once systemd answers
    ///         again: when the bus fell silent where it should have answered, but not when there is no
    ///         systemd user instance at all, which is a valid configuration rather than an outage.
    [[nodiscard]] constexpr bool isWorthPlacingLate(ScopeError reason) noexcept
    {
        auto const& traits = traitsOf(reason);
        return traits.health == BusHealth::Unresponsive && traits.severity == ScopeErrorSeverity::Fault;
    }

    static_assert(!isWorthPlacingLate(ScopeError::Unavailable) && isWorthPlacingLate(ScopeError::TimedOut)
                  && isWorthPlacingLate(ScopeError::Disconnected)
                  && !isWorthPlacingLate(ScopeError::Refused));
} // namespace

PlacementLogLine describe(PlacementReport const& report)
{
    // A process released unplaced for a fault is told where a user sees it -- each one, as each is a
    // session an out-of-memory kill can take Contour down with -- and so is its late placement.
    auto const severity = report.reason ? traitsOf(*report.reason).severity : ScopeErrorSeverity::Expected;
    auto const why = report.reason ? traitsOf(*report.reason).description : std::string_view {};
    auto const process = traitsOf(report.role).noun;
    // A switch rather than a table: switch exhaustiveness names this spot when a status is added.
    switch (report.status)
    {
        case PlacementStatus::Placed:
            return { .severity = ScopeErrorSeverity::Expected,
                     .text = std::format("{} {} runs in a systemd scope of its own.", process, report.pid) };
        case PlacementStatus::PlacedUnheard:
            return { .severity = severity,
                     .text = std::format("{} {} runs in a systemd scope of its own, but {}: its "
                                         "answers are going astray. Until one arrives in time, new sessions "
                                         "start unplaced and move into scopes of their own later.",
                                         process,
                                         report.pid,
                                         why) };
        case PlacementStatus::Deferred:
            return { .severity = severity,
                     .text = std::format("{} {} runs unplaced for now: {}. It moves into a systemd "
                                         "scope of its own once systemd answers again; until then, an "
                                         "out-of-memory kill inside it can stop Contour with it.",
                                         process,
                                         report.pid,
                                         why) };
        case PlacementStatus::PlacedLate:
            return { .severity = severity,
                     .text = std::format("{} {} moved into a systemd scope of its own after all. "
                                         "What it started before then stays in Contour's own unit.",
                                         process,
                                         report.pid) };
        case PlacementStatus::Unplaced:
            return { .severity = severity,
                     .text = std::format("{} {} could not be moved into a systemd scope of its "
                                         "own: {}. An out-of-memory kill inside it can stop Contour with it.",
                                         process,
                                         report.pid,
                                         why) };
        case PlacementStatus::Ended:
            return { .severity = ScopeErrorSeverity::Expected,
                     .text =
                         std::format("{} {} ended before it could be moved into a systemd scope of its own.",
                                     process,
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
        // Anything thrown here -- an allocation, the reporter, the bus -- would reach std::terminate and end
        // Contour, which this worker exists to keep running. The child it concerned is let go instead.
        try
        {
            if (child)
                place(*child);
            else
                placeLate();
        }
        catch (std::exception const& failure)
        {
            letGoAfterFailure(child, failure.what());
        }
        catch (...)
        {
            letGoAfterFailure(child, "an unknown exception");
        }
    } // each iteration's child is released as it goes out of scope
}

void SystemdScopePlacement::letGoAfterFailure(std::optional<ParkedChild>& child, char const* what) noexcept
{
    // A queued child is released, as its tab waits for it. A deferred one the late pass was asking about
    // is not asked about again: a failure that repeats would keep the worker spinning. A failure before
    // the late pass asked about any -- telling of the children that ended -- drops none still running.
    auto pid = 0;
    auto role = ProcessRole::Session;
    if (child)
    {
        pid = child->pid();
        role = child->role();
        child->release();
    }
    else if (_askingLate && !_deferred.empty() && _deferred.front().child.pid() == *_askingLate)
    {
        pid = *_askingLate;
        role = _deferred.front().child.role();
        _deferred.pop_front();
    }
    _askingLate.reset();
    try
    {
        errorLog()("Placing {} {} failed with {}. It may run unplaced, where an out-of-memory kill inside it "
                   "can stop Contour with it.",
                   traitsOf(role).noun,
                   pid,
                   what);
    }
    catch (...)
    {
        // Not even a line could be formatted: one that needs no memory.
        std::fputs("contour: placing a session process failed, and why could not be told.\n", stderr);
    }
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

    auto const outcome =
        attempt(ScopeTarget { .pid = child.pid(), .pidfd = child.pidfd(), .role = child.role() });
    if (outcome == Answer::InTime)
        report(child.pid(), child.role(), PlacementStatus::Placed, std::nullopt);
    else if (outcome) // placed, yet heard of too late: told, as the breaker now holds requests off for it
        report(child.pid(), child.role(), PlacementStatus::PlacedUnheard, ScopeError::TimedOut);
    else if (traitsOf(outcome.error()).health == BusHealth::Unresponsive)
        defer(child, outcome.error()); // asked again once the breaker lets a request through
    else
        report(child.pid(), child.role(), PlacementStatus::Unplaced, outcome.error()); // systemd said no
}

void SystemdScopePlacement::defer(ParkedChild& child, ScopeError reason)
{
    auto unplaced = child.releaseUnplaced();
    // Not kept where systemd is expected to stay away -- no systemd user instance is a valid
    // configuration, not an outage -- nor without a pidfd, which alone tells later whether the pid
    // still names this child.
    if (!isWorthPlacingLate(reason) || unplaced.pidfd() < 0)
    {
        report(unplaced.pid(), unplaced.role(), PlacementStatus::Unplaced, reason);
        return;
    }
    report(unplaced.pid(), unplaced.role(), PlacementStatus::Deferred, reason);
    _deferred.push_back(DeferredChild { .child = std::move(unplaced), .reason = reason });
}

void SystemdScopePlacement::placeLate()
{
    // Those that exited are let go first: their pids may name other processes by now. Each is taken out
    // before it is told of, so that a failure telling of one costs no child still running its place.
    auto const ended = std::ranges::stable_partition(
        _deferred, [](DeferredChild const& deferred) { return !deferred.child.hasExited(); });
    auto const firstEnded = static_cast<std::size_t>(std::distance(_deferred.begin(), ended.begin()));
    while (_deferred.size() > firstEnded)
    {
        auto const gone = std::move(_deferred[firstEnded]);
        _deferred.erase(_deferred.begin() + static_cast<std::ptrdiff_t>(firstEnded));
        report(gone.child.pid(), gone.child.role(), PlacementStatus::Ended, std::nullopt);
    }
    if (_deferred.empty())
        return;

    // The oldest goes first, and is the trial when the cooldown has just passed. systemd moves the
    // running process -- by pidfd where it takes one, which cannot name another process; by pid
    // otherwise, checked alive just above -- but not what it has started meanwhile.
    auto const& oldest = _deferred.front();
    _askingLate = oldest.child.pid();
    auto const outcome = attempt(ScopeTarget {
        .pid = oldest.child.pid(), .pidfd = oldest.child.pidfd(), .role = oldest.child.role() });
    if (!outcome && traitsOf(outcome.error()).health == BusHealth::Unresponsive)
    {
        _askingLate.reset();
        return; // still silent: it waits for the next trial, and was told about already
    }
    if (outcome)
        report(oldest.child.pid(), oldest.child.role(), PlacementStatus::PlacedLate, oldest.reason);
    else
        report(oldest.child.pid(), oldest.child.role(), PlacementStatus::Unplaced, outcome.error());
    _deferred.pop_front();
    _askingLate.reset();
}

std::expected<SystemdScopePlacement::Answer, ScopeError> SystemdScopePlacement::attempt(ScopeTarget target)
{
    auto const outcome = [&]() -> std::expected<Answer, ScopeError> {
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
    // An unheard answer placed its child, but the request still waited out its whole deadline. For the
    // breaker it is the silence it was: answers that go astray for good then cost one deadline per
    // cooldown, as a silent bus does, rather than one per tab.
    auto const heard = outcome.and_then([](Answer answer) -> std::expected<void, ScopeError> {
        if (answer == Answer::Unheard)
            return std::unexpected(ScopeError::TimedOut);
        return {};
    });
    _breaker.record(heard, _clock.now());
    return outcome;
}

std::expected<SystemdScopePlacement::Answer, ScopeError> SystemdScopePlacement::request(
    ScopeTarget target, ScopeProtocol protocol)
{
    auto const ownPid = ::getpid();
    auto const scope = ScopeRequest {
        .unitName = std::format("{}-{}-{}-{}.scope", _config.unitPrefix, ownPid, target.pid, ++_sequence),
        .slice = _config.slice,
        .description = std::format("{} (contour pid {})", traitsOf(target.role).description, ownPid),
        .protocol = protocol,
        .pid = target.pid,
        .pidfd = target.pidfd,
        .memoryLimit = _config.memoryLimit,
    };
    auto const outcome = _bus->startScope(scope, _config.deadline);
    // A late answer is still an answer. The job may have finished in time and its answer arrived late,
    // or not at all; the kernel knows. A child that runs in its scope was placed.
    if (!outcome && outcome.error() == ScopeError::TimedOut && runsIn(target.pid, scope.unitName))
    {
        ptyPlacementLog()("Session process {} runs in {}, though systemd did not answer in time.",
                          target.pid,
                          scope.unitName);
        return Answer::Unheard;
    }
    return outcome.transform([] { return Answer::InTime; });
}

bool SystemdScopePlacement::runsIn(int pid, std::string_view unit) const
{
    auto const cgroup = _readCgroup(pid);
    return cgroup && isInUnit(*cgroup, unit);
}

void SystemdScopePlacement::report(int pid,
                                   ProcessRole role,
                                   PlacementStatus status,
                                   std::optional<ScopeError> reason) const
{
    _report(PlacementReport { .pid = pid, .status = status, .reason = reason, .role = role });
}

} // namespace vtpty
