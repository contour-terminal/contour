// SPDX-License-Identifier: Apache-2.0
#include <vtpty/SystemdScopePlacement.hpp>

#include <vtpty/Pty.hpp>

#include <core/log/LogStore.hpp>

#include <format>
#include <utility>

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

void SystemdScopePlacement::placeThenRelease(ParkedChild child)
{
    std::call_once(_workerStarted,
                   [this] { _worker = std::jthread { [this](std::stop_token const& stop) { run(stop); } }; });
    {
        auto const _ = std::lock_guard { _mutex };
        _queue.push_back(std::move(child));
    }
    _wakeup.notify_one();
}

void SystemdScopePlacement::run(std::stop_token const& stop)
{
    while (true)
    {
        auto child = std::optional<ParkedChild> {};
        {
            auto lock = std::unique_lock { _mutex };
            _wakeup.wait(lock, stop, [this] { return !_queue.empty(); });
            if (stop.stop_requested())
                return;
            child.emplace(std::move(_queue.front()));
            _queue.pop_front();
        }
        place(*child);
    } // each iteration's child is released as it goes out of scope
}

void SystemdScopePlacement::place(ParkedChild const& child)
{
    if (!_breaker.shouldAttempt(_clock.now()))
        return;

    auto const outcome = [&]() -> std::expected<void, ScopeError> {
        if (!_bus)
        {
            auto connected = _connectBus();
            if (!connected)
                return std::unexpected(connected.error());
            _bus = std::move(*connected);
            _reference = ProcessReference::PidFd; // a new connection may reach a newer systemd
        }
        auto const preferred = child.pidfd() >= 0 ? _reference : ProcessReference::Pid;
        auto result = request(child, preferred);
        // An older systemd knows no PIDFDs. The child is parked, so its pid cannot have been reused.
        if (!result && result.error() == ScopeError::UnknownProperty && preferred == ProcessReference::PidFd)
        {
            _reference = ProcessReference::Pid;
            result = request(child, ProcessReference::Pid);
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
                                                               ProcessReference reference)
{
    auto const ownPid = ::getpid();
    return _bus->startScope(
        ScopeRequest {
            .unitName =
                std::format("{}-{}-{}-{}.scope", _config.unitPrefix, ownPid, child.pid(), ++_sequence),
            .slice = _config.slice,
            .description = std::format("Contour session (contour pid {})", ownPid),
            .reference = reference,
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
