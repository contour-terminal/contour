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
                                                                      ExtensionLimits limits)
{
    auto runtime = backend.createRuntime(
        wasm::RuntimeLimits { .memoryBytes = limits.memoryBytes, .stackBytes = limits.stackBytes },
        detail::hostFunctions());
    if (!runtime)
        return std::unexpected(ExtensionError::RuntimeUnavailable);

    try
    {
        return std::make_shared<Engine>(PrivateTag {}, backend, limits, std::move(*runtime));
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
               ExtensionLimits limits,
               std::shared_ptr<wasm::IWasmRuntime> runtime):
    _backend { backend },
    _limits { limits },
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

void Engine::armDeadline(WatchToken token, std::chrono::milliseconds timeout)
{
    Require(token.instance != nullptr && token.state != nullptr);
    auto const expiry = std::chrono::steady_clock::now() + timeout;
    {
        auto const lock = std::scoped_lock { _mutex };
        removeDeadline(token);
        _deadlines.push_back(Deadline { .expiry = expiry, .token = token });
        std::ranges::push_heap(_deadlines, expiresLater);
    }
    _wakeup.notify_one();
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
            auto const token = _deadlines.back().token;
            _deadlines.pop_back();
            token.state->store(DeadlineState::Expired, std::memory_order_release);
            token.instance->interrupt();
        }
    }
}

} // namespace vtextension
