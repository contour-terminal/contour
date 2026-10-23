// SPDX-License-Identifier: Apache-2.0
#include <vtextension/Engine.hpp>

#include <core/Assert.hpp>

#include <algorithm>
#include <system_error>
#include <utility>

#include <wasm_export.h>

#include <vtextension/HostBindings.hpp>
#include <vtextension/wasm/wamr/WamrHandles.hpp>

namespace vtextension
{

std::expected<std::shared_ptr<Engine>, ExtensionError> Engine::create(ExtensionLimits limits)
{
    if (auto acquired = wasm::wamr::acquireRuntime(); !acquired)
        return std::unexpected(acquired.error());

    auto const fail = [] {
        wasm::wamr::releaseRuntime();
        return std::unexpected(ExtensionError::RuntimeUnavailable);
    };

    auto hostRegistration = detail::registerHostFunctions();
    if (!hostRegistration)
        return fail();
    auto const nullDevice = wasm::wamr::detail::openNullDevice();
    if (!nullDevice)
        return fail();

    try
    {
        return std::make_shared<Engine>(PrivateTag {}, limits, *nullDevice, std::move(hostRegistration));
    }
    catch (std::system_error const&)
    {
        // The watchdog thread could not be started: without it no deadline would hold. The engine's
        // constructor did not complete, so what it was handed is released here.
        wasm::wamr::detail::closeNullDevice(*nullDevice);
        return fail();
    }
    catch (...)
    {
        wasm::wamr::detail::closeNullDevice(*nullDevice);
        wasm::wamr::releaseRuntime();
        throw;
    }
}

Engine::Engine(PrivateTag /*unused*/,
               ExtensionLimits limits,
               int64_t nullDevice,
               std::unique_ptr<detail::HostRegistration> hostRegistration):
    _limits { limits },
    _nullDevice { nullDevice },
    _hostRegistration { std::move(hostRegistration) },
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
    _hostRegistration.reset();
    wasm::wamr::detail::closeNullDevice(_nullDevice);
    wasm::wamr::releaseRuntime();
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
    Require(token.instance != nullptr);
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

        // Terminating under the lock is what makes disarm() final: once it has removed a deadline,
        // no terminate for it can still be on its way.
        auto const now = std::chrono::steady_clock::now();
        while (!_deadlines.empty() && _deadlines.front().expiry <= now)
        {
            std::ranges::pop_heap(_deadlines, expiresLater);
            wasm_runtime_terminate(_deadlines.back().token.instance);
            _deadlines.pop_back();
        }
    }
}

} // namespace vtextension
