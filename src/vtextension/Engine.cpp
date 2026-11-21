// SPDX-License-Identifier: Apache-2.0
#include <vtextension/Engine.hpp>

#include <core/Assert.hpp>

#include <algorithm>
#include <system_error>
#include <utility>

#include <vtextension/HostBindings.hpp>

namespace vtextension
{

std::expected<std::shared_ptr<Engine>, ExtensionError> Engine::create(wasm::IWasmBackend& backend,
                                                                      World world,
                                                                      ExtensionLimits limits,
                                                                      IThreadCpuClock const& cpuClock)
{
    auto runtime = backend.createRuntime(
        wasm::RuntimeLimits { .memoryBytes = limits.memoryBytes, .stackBytes = limits.stackBytes },
        detail::hostFunctions(world));
    if (!runtime)
        return std::unexpected(ExtensionError::RuntimeUnavailable);

    try
    {
        return std::make_shared<Engine>(PrivateTag {}, backend, world, limits, cpuClock, std::move(*runtime));
    }
    catch (std::system_error const&)
    {
        // The watchdog thread could not be started: without it no deadline would hold. The runtime goes
        // with the engine that never was.
        return std::unexpected(ExtensionError::RuntimeUnavailable);
    }
}

Engine::Engine(PrivateTag /*unused*/,
               wasm::IWasmBackend& backend,
               World world,
               ExtensionLimits limits,
               IThreadCpuClock const& cpuClock,
               std::shared_ptr<wasm::IWasmRuntime> runtime):
    _backend { backend },
    _world { world },
    _limits { limits },
    _cpuClock { cpuClock },
    _runtime { std::move(runtime) },
    _watchdog { [this] { watchdogLoop(); } }
{
}

Engine::~Engine()
{
    {
        auto const lock = std::scoped_lock { _mutex };
        _state = WatchdogState::Stopping;
    }
    _wakeup.notify_one();
    _watchdog.join();
}

bool Engine::expiresLater(Deadline const& a, Deadline const& b) noexcept
{
    return a.expiry > b.expiry;
}

void Engine::removeDeadline(WatchToken token)
{
    if (std::erase_if(_deadlines, [token](Deadline const& deadline) { return deadline.token == token; }) != 0)
        std::ranges::make_heap(_deadlines, expiresLater);
}

std::chrono::nanoseconds Engine::unspentAt(Deadline const& deadline, TimePoint now) const noexcept
{
    auto const consumed = deadline.thread ? _cpuClock.consumed(*deadline.thread) : std::nullopt;
    auto const spent = consumed ? *consumed - deadline.cpuAtArm
                                : std::chrono::duration_cast<std::chrono::nanoseconds>(now - deadline.armed);
    auto const left = std::min(deadline.budget - spent,
                               std::chrono::duration_cast<std::chrono::nanoseconds>(deadline.latest - now));
    return std::max(left, std::chrono::nanoseconds::zero());
}

void Engine::armDeadline(WatchToken token, std::chrono::milliseconds timeout)
{
    Require(token.instance != nullptr && token.state != nullptr);
    // Read before the lock: the clock is the calling thread's, which is the guarded one.
    auto const thread = _cpuClock.currentThread();
    auto const cpuAtArm = thread ? _cpuClock.consumed(*thread) : std::nullopt;
    auto const now = std::chrono::steady_clock::now();
    auto deadline = Deadline {
        // A thread spends CPU time no faster than real time: its budget cannot be spent any earlier.
        .expiry = now + timeout,
        .armed = now,
        .latest = now + (timeout * _limits.deadlineRealTimeFactor),
        .budget = timeout,
        .thread = cpuAtArm ? thread : std::nullopt,
        .cpuAtArm = cpuAtArm.value_or(std::chrono::nanoseconds::zero()),
        .token = token,
    };
    {
        auto const lock = std::scoped_lock { _mutex };
        removeDeadline(token);
        _deadlines.push_back(deadline);
        std::ranges::push_heap(_deadlines, expiresLater);
    }
    _wakeup.notify_one();
}

std::chrono::nanoseconds Engine::unspent(WatchToken token)
{
    auto const lock = std::scoped_lock { _mutex };
    auto const found = std::ranges::find(_deadlines, token, &Deadline::token);
    return found != _deadlines.end() ? unspentAt(*found, std::chrono::steady_clock::now())
                                     : std::chrono::nanoseconds::zero();
}

void Engine::disarm(WatchToken token)
{
    auto const lock = std::scoped_lock { _mutex };
    removeDeadline(token);
    // No notification: a watchdog sleeping towards the dropped deadline wakes, finds nothing due, and
    // sleeps on.
}

void Engine::watchdogLoop()
{
    auto lock = std::unique_lock { _mutex };
    while (_state == WatchdogState::Running)
    {
        if (_deadlines.empty())
        {
            _wakeup.wait(lock);
            continue;
        }

        // A copy: while the watchdog waits, arming may reallocate the heap from under a reference.
        auto const earliest = _deadlines.front().expiry;
        _wakeup.wait_until(lock, earliest);

        // Interrupting under the lock is what makes disarm() final: once it has removed a deadline,
        // no interrupt for it can still be on its way. The deadline is marked expired first, so that
        // whoever sees the call fail finds out why.
        auto const now = std::chrono::steady_clock::now();
        while (!_deadlines.empty() && _deadlines.front().expiry <= now)
        {
            std::ranges::pop_heap(_deadlines, expiresLater);
            // A deadline whose thread waited for a CPU has budget left: the watchdog looks again once
            // that could be spent, if it ran from now on without waiting.
            if (auto const left = unspentAt(_deadlines.back(), now); left > std::chrono::nanoseconds::zero())
            {
                _deadlines.back().expiry = now + left;
                std::ranges::push_heap(_deadlines, expiresLater);
                continue;
            }
            auto const token = _deadlines.back().token;
            _deadlines.pop_back();
            token.state->store(DeadlineState::Expired, std::memory_order_release);
            token.instance->interrupt();
        }
    }
}

} // namespace vtextension
