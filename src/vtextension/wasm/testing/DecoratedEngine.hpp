// SPDX-License-Identifier: Apache-2.0
//
// Test-only: a hosted engine whose instances a test decorates, to observe or disturb what a host does with
// them while everything else is forwarded to the engine it wraps.
#pragma once

#include <cstddef>
#include <cstdint>
#include <expected>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string_view>
#include <utility>
#include <vector>

#include <vtextension/ExtensionError.hpp>
#include <vtextension/wasm/WasmRuntime.hpp>
#include <vtextension/wasm/host/IHostedEngine.hpp>

namespace vtextension::wasm::testing
{

/// An instance that forwards everything to the one it wraps; a decorator overrides what it observes.
class ForwardingInstance: public IWasmInstance
{
  public:
    /// @param inner The instance forwarded to.
    explicit ForwardingInstance(std::unique_ptr<IWasmInstance> inner) noexcept: _inner { std::move(inner) } {}

    [[nodiscard]] std::optional<ExportFunction> function(std::string_view name) const override
    {
        return _inner->function(name);
    }

    [[nodiscard]] std::expected<void, CallFailure> call(ExportFunction function,
                                                        std::span<uint32_t> argv) override
    {
        return _inner->call(function, argv);
    }

    [[nodiscard]] std::expected<std::span<std::byte>, MemoryFailure> memory(uint32_t address,
                                                                            uint32_t size) override
    {
        return _inner->memory(address, size);
    }

    void interrupt() noexcept override { _inner->interrupt(); }

  private:
    std::unique_ptr<IWasmInstance> _inner;
};

/// What wraps each instance a decorated engine creates.
using InstanceDecorator = std::function<std::unique_ptr<IWasmInstance>(std::unique_ptr<IWasmInstance>)>;

namespace detail
{
    /// A module of a decorated engine, whose instances are decorated.
    class DecoratedModule final: public IWasmModule
    {
      public:
        DecoratedModule(std::shared_ptr<IWasmModule> inner, InstanceDecorator decorate):
            _inner { std::move(inner) }, _decorate { std::move(decorate) }
        {
        }

        [[nodiscard]] std::span<ImportDescription const> imports() const noexcept override
        {
            return _inner->imports();
        }

        [[nodiscard]] std::span<ExportDescription const> exports() const noexcept override
        {
            return _inner->exports();
        }

        [[nodiscard]] std::expected<std::unique_ptr<IWasmInstance>, ExtensionError> instantiate(
            IHostContext& context) override
        {
            return _inner->instantiate(context).transform(_decorate);
        }

      private:
        std::shared_ptr<IWasmModule> _inner;
        InstanceDecorator _decorate;
    };

    /// A runtime of a decorated engine, whose modules are decorated.
    class DecoratedRuntime final: public IWasmRuntime
    {
      public:
        DecoratedRuntime(std::shared_ptr<IWasmRuntime> inner, InstanceDecorator decorate):
            _inner { std::move(inner) }, _decorate { std::move(decorate) }
        {
        }

        [[nodiscard]] std::expected<std::shared_ptr<IWasmModule>, ExtensionError> load(
            ModuleIdentity identity, std::vector<std::byte> bytes) override
        {
            return _inner->load(std::move(identity), std::move(bytes))
                .transform([this](std::shared_ptr<IWasmModule> module) -> std::shared_ptr<IWasmModule> {
                    return std::make_shared<DecoratedModule>(std::move(module), _decorate);
                });
        }

      private:
        std::shared_ptr<IWasmRuntime> _inner;
        InstanceDecorator _decorate;
    };
} // namespace detail

/// A hosted engine whose instances are wrapped by a decorator; everything else is the engine it wraps.
class DecoratedEngine final: public host::IHostedEngine
{
  public:
    /// @param inner The engine forwarded to.
    /// @param decorate What wraps each instance it creates.
    DecoratedEngine(std::unique_ptr<host::IHostedEngine> inner, InstanceDecorator decorate):
        _inner { std::move(inner) }, _decorate { std::move(decorate) }
    {
    }

    [[nodiscard]] std::expected<std::shared_ptr<IWasmRuntime>, ExtensionError> createRuntime(
        RuntimeLimits limits, std::span<HostFunction const> hostFunctions) override
    {
        return _inner->createRuntime(limits, hostFunctions)
            .transform([this](std::shared_ptr<IWasmRuntime> runtime) -> std::shared_ptr<IWasmRuntime> {
                return std::make_shared<detail::DecoratedRuntime>(std::move(runtime), _decorate);
            });
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

  private:
    std::unique_ptr<host::IHostedEngine> _inner;
    InstanceDecorator _decorate;
};

} // namespace vtextension::wasm::testing
