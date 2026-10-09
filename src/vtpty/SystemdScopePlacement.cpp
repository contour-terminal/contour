// SPDX-License-Identifier: Apache-2.0
#include <vtpty/SystemdScopePlacement.hpp>

#include <vtpty/Pty.hpp>

#include <core/log/LogStore.hpp>

#include <csignal>
#include <exception>
#include <format>
#include <utility>

#include <pthread.h>
#include <unistd.h>

namespace vtpty
{

SystemdScopePlacement::SystemdScopePlacement(SystemdScopeConfig config,
                                             ScopeBusFactory connectBus,
                                             core::platform::IClock const& clock):
    _config { std::move(config) },
    _connectBus { std::move(connectBus) },
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
    // What the worker left queued is released as _queue is destroyed.
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
            _wakeup.wait(lock, [this] { return _workerState == WorkerState::Stopping || !_queue.empty(); });
            if (_workerState == WorkerState::Stopping)
                return;
            child.emplace(std::move(_queue.front()));
            _queue.pop_front();
        }
        place(*child);
    } // each iteration's child is released as it goes out of scope
}

void SystemdScopePlacement::place(ParkedChild const& child)
{
    // Its tab closed while it waited here: asking about its pid could move whatever reuses it now,
    // and a refusal would be reported as a fault over a closed tab.
    if (child.hasExited())
        return;
    if (!_breaker.shouldAttempt(_clock.now()))
        return;

    auto const outcome = [&]() -> std::expected<void, ScopeError> {
        if (!_bus)
        {
            auto connected = _connectBus();
            if (!connected)
                return std::unexpected(connected.error());
            _bus = std::move(*connected);
            _protocol = ScopeProtocol::Current; // a new connection may reach a newer systemd
        }
        auto result = request(child, _protocol);
        // systemd before 253 knows neither PIDFDs nor OOMPolicy on scopes, and refuses the whole
        // request over either. Speak what it knows from now on.
        if (!result && result.error() == ScopeError::Unsupported && _protocol == ScopeProtocol::Current)
        {
            _protocol = ScopeProtocol::Legacy;
            result = request(child, _protocol);
        }
        return result;
    }();

    if (!outcome && outcome.error() == ScopeError::Disconnected)
        _bus.reset(); // connect afresh on the next attempt
    _breaker.record(outcome, _clock.now());
    if (!outcome)
        report(child, outcome.error());
}

std::expected<void, ScopeError> SystemdScopePlacement::request(ParkedChild const& child,
                                                               ScopeProtocol protocol)
{
    auto const ownPid = ::getpid();
    return _bus->startScope(
        ScopeRequest {
            .unitName =
                std::format("{}-{}-{}-{}.scope", _config.unitPrefix, ownPid, child.pid(), ++_sequence),
            .slice = _config.slice,
            .description = std::format("Contour session (contour pid {})", ownPid),
            .protocol = protocol,
            .pid = child.pid(),
            .pidfd = child.pidfd(),
            .memoryLimit = _config.memoryLimit,
        },
        _config.deadline);
}

void SystemdScopePlacement::report(ParkedChild const& child, ScopeError error)
{
    auto const& traits = traitsOf(error);
    if (traits.severity == ScopeErrorSeverity::Expected || _hasReportedFault)
    {
        ptyPlacementLog()(
            "Session process {} not placed in a scope of its own: {}.", child.pid(), traits.description);
        return;
    }
    _hasReportedFault = true;
    errorLog()("Session process {} could not be moved into a systemd scope of its own: {}. Until that "
               "works, an out-of-memory kill inside a session can stop Contour with it.",
               child.pid(),
               traits.description);
}

} // namespace vtpty
