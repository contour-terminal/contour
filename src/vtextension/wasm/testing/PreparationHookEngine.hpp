// SPDX-License-Identifier: Apache-2.0
//
// Test-only: a hosted engine that lets a test observe, or fail, the preparation of each thread a host runs
// guests on, while everything else is forwarded to the engine it wraps.
#pragma once

#include <cstdint>
#include <expected>
#include <functional>
#include <memory>
#include <span>
#include <utility>
#include <vector>

#include <vtextension/ExtensionError.hpp>
#include <vtextension/wasm/WasmRuntime.hpp>
#include <vtextension/wasm/host/IHostedEngine.hpp>

namespace vtextension::wasm::testing
{

/// What runs as a thread is prepared, on that thread: nothing to go on, or why the thread is not prepared.
using PreparationHook = std::function<std::expected<void, ExtensionError>()>;

/// A hosted engine whose prepareThread() runs a hook first, then the wrapped engine's own unless the hook
/// failed; everything else is the engine it wraps.
class PreparationHookEngine final: public host::IHostedEngine
{
  public:
    /// @param inner The engine forwarded to.
    /// @param onPrepare What runs as each thread is prepared, on that thread.
    PreparationHookEngine(std::unique_ptr<host::IHostedEngine> inner, PreparationHook onPrepare):
        _inner { std::move(inner) }, _onPrepare { std::move(onPrepare) }
    {
    }

    [[nodiscard]] std::expected<std::shared_ptr<IWasmRuntime>, ExtensionError> createRuntime(
        RuntimeLimits limits, std::span<HostFunction const> hostFunctions) override
    {
        return _inner->createRuntime(limits, hostFunctions);
    }

    [[nodiscard]] std::expected<std::unique_ptr<IThreadGuard>, ExtensionError> enterThread() override
    {
        return _inner->enterThread();
    }

    [[nodiscard]] bool isCurrentThreadPrepared() const override { return _inner->isCurrentThreadPrepared(); }

    [[nodiscard]] uint64_t runtimeGeneration() const noexcept override { return _inner->runtimeGeneration(); }

    [[nodiscard]] BackendCapabilities capabilities() const noexcept override
    {
        return _inner->capabilities();
    }

    [[nodiscard]] remote::proto::MemoryMode memoryMode() const noexcept override
    {
        return _inner->memoryMode();
    }

    [[nodiscard]] uint64_t memoryGuardBytes() const noexcept override { return _inner->memoryGuardBytes(); }

    [[nodiscard]] host::GuestCode guestCode() const noexcept override { return _inner->guestCode(); }

    [[nodiscard]] std::vector<remote::transport::NativeHandle> heldDevices() const override
    {
        return _inner->heldDevices();
    }

    [[nodiscard]] std::expected<void, ExtensionError> prepareThread() override
    {
        return _onPrepare().and_then([this] { return _inner->prepareThread(); });
    }

  private:
    std::unique_ptr<host::IHostedEngine> _inner;
    PreparationHook _onPrepare;
};

} // namespace vtextension::wasm::testing
