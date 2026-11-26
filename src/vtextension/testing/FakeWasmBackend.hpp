// SPDX-License-Identifier: Apache-2.0
//
// Test-only: a WebAssembly backend that runs no WebAssembly. Its modules describe a world guest and its
// instances run what a test hooks onto their exports, so that the rules above the runtime interface
// (deadlines, faults, the host's lifting) can be driven step by step, with no interpreter in the way.
#pragma once

#include <core/Assert.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <ranges>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include <vtextension/ExtensionError.hpp>
#include <vtextension/wasm/WasmRuntime.hpp>

namespace vtextension::testing
{

/// What a fake export does when called: it receives the call's parameters and leaves its results in
/// the same cells.
using FakeExportHook = std::function<std::expected<void, wasm::CallFailure>(std::span<uint32_t>)>;

/// How the instances of a fake module behave, as a test sets it up before instantiating.
struct FakeGuest
{
    /// What each export does, by name. An export without a hook returns at once, results untouched;
    /// a name with a hook is an export of the instance even if the module does not describe it.
    std::map<std::string, FakeExportHook, std::less<>> exports;

    /// Runs on every memory access the host makes, before it is bounds-checked.
    std::function<void()> onMemoryAccess;
};

/// One fake instance: a 64 KiB linear memory, and exports that run what the test hooked onto them.
///
/// The interrupt is honoured as the contract says: once interrupt() was called, every later call()
/// fails as Interrupted without running its hook.
class FakeWasmInstance final: public wasm::IWasmInstance
{
  public:
    /// The size of the linear memory: one page.
    static constexpr auto MemoryBytes = std::size_t { 64 } * 1024;

    /// Creates an instance with no exports but those @p guest hooks.
    /// @param guest What the exports and memory accesses do.
    /// @param exportNames The function exports the module describes, besides those @p guest hooks.
    /// @param owner The module it was created from, kept alive; null for an instance of no module.
    /// @param context What a hook's host calls reach (callHost()); null for an instance of no module.
    explicit FakeWasmInstance(FakeGuest guest = {},
                              std::vector<std::string> exportNames = {},
                              std::shared_ptr<wasm::IWasmModule const> owner = nullptr,
                              wasm::IHostContext* context = nullptr):
        _owner { std::move(owner) },
        _context { context },
        _guest { std::move(guest) },
        _names { std::move(exportNames) }
    {
        for (auto const& name: _guest.exports | std::views::keys)
            if (std::ranges::find(_names, name) == _names.end())
                _names.push_back(name);
    }

    [[nodiscard]] std::optional<wasm::ExportFunction> function(std::string_view name) const override
    {
        auto const entry = std::ranges::find(_names, name);
        if (entry == _names.end())
            return std::nullopt;
        return wasm::ExportFunction { .index = static_cast<uint32_t>(entry - _names.begin()) };
    }

    [[nodiscard]] std::expected<void, wasm::CallFailure> call(wasm::ExportFunction function,
                                                              std::span<uint32_t> argv) override
    {
        if (_interruption.load(std::memory_order_acquire) == Interruption::Requested)
            return std::unexpected(wasm::CallFailure::Interrupted);
        auto const hook = _guest.exports.find(_names.at(function.index));
        if (hook == _guest.exports.end() || !hook->second)
            return {};
        return hook->second(argv);
    }

    [[nodiscard]] std::expected<std::span<std::byte>, wasm::MemoryFailure> memory(uint32_t address,
                                                                                  uint32_t size) override
    {
        if (_guest.onMemoryAccess)
            _guest.onMemoryAccess();
        if (size == 0)
            return std::span<std::byte> {};
        if (uint64_t { address } + size > linearMemory.size())
            return std::unexpected(wasm::MemoryFailure::OutOfBounds);
        return std::span { linearMemory }.subspan(address, size);
    }

    void interrupt() noexcept override
    {
        _interruption.store(Interruption::Requested, std::memory_order_release);
        interrupts.fetch_add(1, std::memory_order_relaxed);
    }

    /// Calls a host function as the guest would, from inside a hook: synchronously, on the calling thread.
    /// @param functionIndex The host function's index in the runtime's table.
    /// @param arguments Its flat arguments.
    /// @return What the host context answered.
    [[nodiscard]] std::expected<void, ExtensionError> callHost(std::size_t functionIndex,
                                                               std::span<uint32_t const> arguments)
    {
        Require(_context != nullptr);
        return _context->callHost(functionIndex, arguments);
    }

    /// The linear memory, which a hook writes its results into directly.
    std::vector<std::byte> linearMemory = std::vector<std::byte>(MemoryBytes);

    /// How often interrupt() was called; written from any thread.
    std::atomic<int> interrupts = 0;

  private:
    /// Whether interrupt() was called.
    enum class Interruption : uint8_t
    {
        None,
        Requested,
    };

    std::shared_ptr<wasm::IWasmModule const> _owner;
    wasm::IHostContext* _context;
    FakeGuest _guest;
    std::vector<std::string> _names; ///< The function exports; an ExportFunction indexes it.
    std::atomic<Interruption> _interruption = Interruption::None;
};

/// The function exports a fake module describes: those the world requires, with their core signatures.
struct FakeExportShape
{
    std::string_view name;  ///< The export's name.
    std::size_t parameters; ///< How many i32 parameters it takes.
    std::size_t results;    ///< How many i32 results it returns.
};

/// The world's required exports.
inline constexpr auto FakeRequiredExports = std::array {
    FakeExportShape { .name = "load", .parameters = 2, .results = 1 },
    FakeExportShape { .name = "update", .parameters = 5, .results = 1 },
    FakeExportShape { .name = "render", .parameters = 2, .results = 1 },
    FakeExportShape { .name = "cabi_realloc", .parameters = 4, .results = 1 },
};

/// The backend whose runtimes the fakes come from; declared here so that a module can tell it of the
/// instances it creates.
class FakeWasmBackend;

/// A fake module: whatever bytes it was loaded from, it describes a world guest. It imports every host
/// function its runtime was created with, all satisfied, and exports a one-page `memory` and the
/// world's required functions.
class FakeWasmModule final: public wasm::IWasmModule, public std::enable_shared_from_this<FakeWasmModule>
{
  public:
    /// @param runtime The runtime it was loaded on; kept alive.
    /// @param backend Told of every instance created, and supplies how they behave.
    /// @param hostFunctions What the module imports.
    FakeWasmModule(std::shared_ptr<wasm::IWasmRuntime> runtime,
                   FakeWasmBackend& backend,
                   std::span<wasm::HostFunction const> hostFunctions);

    [[nodiscard]] std::span<wasm::ImportDescription const> imports() const noexcept override
    {
        return _imports;
    }

    [[nodiscard]] std::span<wasm::ExportDescription const> exports() const noexcept override
    {
        return _exports;
    }

    [[nodiscard]] std::expected<std::unique_ptr<wasm::IWasmInstance>, ExtensionError> instantiate(
        wasm::IHostContext& context) override;

  private:
    std::shared_ptr<wasm::IWasmRuntime> _runtime;
    FakeWasmBackend& _backend;
    std::vector<wasm::ImportDescription> _imports;
    std::vector<wasm::ExportDescription> _exports;
};

/// A fake runtime, holding the host functions it was created with.
class FakeWasmRuntime final: public wasm::IWasmRuntime, public std::enable_shared_from_this<FakeWasmRuntime>
{
  public:
    /// @param backend What created it.
    /// @param hostFunctions What its modules import.
    FakeWasmRuntime(FakeWasmBackend& backend, std::span<wasm::HostFunction const> hostFunctions);
    FakeWasmRuntime(FakeWasmRuntime const&) = delete;
    FakeWasmRuntime(FakeWasmRuntime&&) = delete;
    FakeWasmRuntime& operator=(FakeWasmRuntime const&) = delete;
    FakeWasmRuntime& operator=(FakeWasmRuntime&&) = delete;
    /// Tells the backend that one runtime fewer is alive.
    ~FakeWasmRuntime() override;

    /// Tells the backend of @p identity and ignores @p bytes: every module is a world guest.
    /// @return The module; the backend's FakeWasmBackend::loadFailure if it names one.
    [[nodiscard]] std::expected<std::shared_ptr<wasm::IWasmModule>, ExtensionError> load(
        wasm::ModuleIdentity identity, std::vector<std::byte> /*bytes*/) override;

  private:
    FakeWasmBackend& _backend;
    std::vector<wasm::HostFunction> _hostFunctions;
};

/// A fake backend with no per-thread state. It keeps the contract's bring-up rules all the same: the
/// generation counts up each time a runtime is created with none alive, a guard is refused while no
/// runtime is alive, and every thread is prepared while one is. Refused memory growth would be reported
/// (it never happens).
class FakeWasmBackend final: public wasm::IWasmBackend
{
  public:
    [[nodiscard]] std::expected<std::shared_ptr<wasm::IWasmRuntime>, ExtensionError> createRuntime(
        wasm::RuntimeLimits /*limits*/, std::span<wasm::HostFunction const> hostFunctions) override
    {
        return std::make_shared<FakeWasmRuntime>(*this, hostFunctions);
    }

    [[nodiscard]] std::expected<std::unique_ptr<wasm::IThreadGuard>, ExtensionError> enterThread() override
    {
        auto const lock = std::scoped_lock { _mutex };
        if (_runtimesAlive == 0)
            return std::unexpected(ExtensionError::RuntimeUnavailable);
        return std::make_unique<Guard>(_generation.load(std::memory_order_relaxed));
    }

    [[nodiscard]] bool isCurrentThreadPrepared() const override
    {
        auto const lock = std::scoped_lock { _mutex };
        return _runtimesAlive != 0;
    }

    [[nodiscard]] uint64_t runtimeGeneration() const noexcept override
    {
        return _generation.load(std::memory_order_acquire);
    }

    [[nodiscard]] wasm::BackendCapabilities capabilities() const noexcept override
    {
        return wasm::BackendCapabilities { .memoryGrowth = wasm::MemoryGrowthReporting::Reported };
    }

    /// @return How many modules were loaded, failed loads included.
    [[nodiscard]] std::size_t loads() const noexcept
    {
        auto const lock = std::scoped_lock { _mutex };
        return _loads;
    }

    /// @return The thread each load ran on, failed ones included, in order.
    [[nodiscard]] std::vector<std::thread::id> loadThreads() const
    {
        auto const lock = std::scoped_lock { _mutex };
        return _loadThreads;
    }

    /// @return The thread each instantiation ran on, in order.
    [[nodiscard]] std::vector<std::thread::id> instantiateThreads() const
    {
        auto const lock = std::scoped_lock { _mutex };
        return _instantiateThreads;
    }

    /// @return The identity the last module was loaded with; empty before the first.
    [[nodiscard]] wasm::ModuleIdentity lastIdentity() const
    {
        auto const lock = std::scoped_lock { _mutex };
        return _lastIdentity;
    }

    /// How every instance created from now on behaves.
    FakeGuest guest;

    /// What every load from now on fails with; nothing for loads that succeed. Read after loadGate returns.
    std::optional<ExtensionError> loadFailure;

    /// Run by every load once it is counted, before it answers, on the loading thread: a test holds a load in
    /// flight with it. None for loads that answer at once.
    std::function<void()> loadGate;

    /// The instance created last; null before the first. Valid only while that instance lives.
    FakeWasmInstance* lastInstance = nullptr;

  private:
    friend class FakeWasmRuntime;
    friend class FakeWasmModule;

    /// A guard that prepares nothing, as nothing needs preparing; it remembers its generation.
    class Guard final: public wasm::IThreadGuard
    {
      public:
        /// @param generation The generation it was entered for.
        explicit Guard(uint64_t generation) noexcept: _generation { generation } {}

        [[nodiscard]] uint64_t generation() const noexcept override { return _generation; }

      private:
        uint64_t _generation;
    };

    /// Counts a runtime created; the first one alive is a bring-up of the engine.
    void runtimeCreated()
    {
        auto const lock = std::scoped_lock { _mutex };
        if (_runtimesAlive++ == 0)
            _generation.fetch_add(1, std::memory_order_release);
    }

    /// Counts a module loaded with @p identity.
    void moduleLoaded(wasm::ModuleIdentity identity)
    {
        auto const lock = std::scoped_lock { _mutex };
        ++_loads;
        _loadThreads.push_back(std::this_thread::get_id());
        _lastIdentity = std::move(identity);
    }

    /// Counts an instance created, on the calling thread.
    void instanceCreated()
    {
        auto const lock = std::scoped_lock { _mutex };
        _instantiateThreads.push_back(std::this_thread::get_id());
    }

    /// Counts a runtime destroyed, on whichever thread drops the last reference to it.
    void runtimeDestroyed()
    {
        auto const lock = std::scoped_lock { _mutex };
        --_runtimesAlive;
    }

    mutable std::mutex _mutex;          ///< Guards the counters below; the generation is read without it.
    std::size_t _runtimesAlive = 0;     ///< How many runtimes are alive.
    std::size_t _loads = 0;             ///< How many modules were loaded.
    wasm::ModuleIdentity _lastIdentity; ///< The identity the last module was loaded with.
    std::vector<std::thread::id> _loadThreads;        ///< Where each load ran.
    std::vector<std::thread::id> _instantiateThreads; ///< Where each instantiation ran.
    std::atomic<uint64_t> _generation = 0;            ///< How many times the engine was brought up.
};

inline FakeWasmRuntime::FakeWasmRuntime(FakeWasmBackend& backend,
                                        std::span<wasm::HostFunction const> hostFunctions):
    _backend { backend }, _hostFunctions { hostFunctions.begin(), hostFunctions.end() }
{
    _backend.runtimeCreated();
}

inline FakeWasmRuntime::~FakeWasmRuntime()
{
    _backend.runtimeDestroyed();
}

inline std::expected<std::shared_ptr<wasm::IWasmModule>, ExtensionError> FakeWasmRuntime::load(
    wasm::ModuleIdentity identity, std::vector<std::byte> /*bytes*/)
{
    _backend.moduleLoaded(std::move(identity));
    if (_backend.loadGate)
        _backend.loadGate();
    if (_backend.loadFailure)
        return std::unexpected(*_backend.loadFailure);
    return std::make_shared<FakeWasmModule>(shared_from_this(), _backend, _hostFunctions);
}

inline FakeWasmModule::FakeWasmModule(std::shared_ptr<wasm::IWasmRuntime> runtime,
                                      FakeWasmBackend& backend,
                                      std::span<wasm::HostFunction const> hostFunctions):
    _runtime { std::move(runtime) }, _backend { backend }
{
    auto const i32s = [](std::size_t count) {
        return std::vector<wasm::ValueType>(count, wasm::ValueType::I32);
    };
    for (auto const& function: hostFunctions)
        _imports.push_back(wasm::ImportDescription {
            .module = std::string { function.module },
            .name = std::string { function.name },
            .kind = wasm::ExternKind::Function,
            .signature =
                wasm::FunctionSignature { .parameters = i32s(function.parameterCount), .results = {} },
            .linkage = wasm::Linkage::Satisfied,
        });
    _exports.push_back(wasm::ExportDescription {
        .name = "memory", .kind = wasm::ExternKind::Memory, .signature = {}, .initialPages = 1 });
    for (auto const& shape: FakeRequiredExports)
        _exports.push_back(wasm::ExportDescription {
            .name = std::string { shape.name },
            .kind = wasm::ExternKind::Function,
            .signature = wasm::FunctionSignature { .parameters = i32s(shape.parameters),
                                                   .results = i32s(shape.results) },
            .initialPages = 0,
        });
}

inline std::expected<std::unique_ptr<wasm::IWasmInstance>, ExtensionError> FakeWasmModule::instantiate(
    wasm::IHostContext& context)
{
    auto names = std::ranges::to<std::vector<std::string>>(
        _exports | std::views::filter([](wasm::ExportDescription const& entry) {
            return entry.kind == wasm::ExternKind::Function;
        })
        | std::views::transform(&wasm::ExportDescription::name));
    _backend.instanceCreated();
    auto instance =
        std::make_unique<FakeWasmInstance>(_backend.guest, std::move(names), shared_from_this(), &context);
    _backend.lastInstance = instance.get();
    return instance;
}

} // namespace vtextension::testing
