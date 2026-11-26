// SPDX-License-Identifier: Apache-2.0
//
// The Wasmtime backend's internals, shared by its translation units: the state every backend of the
// kind shares, a runtime (one engine and its linker), and a loaded module.
#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include <vtextension/ExtensionError.hpp>
#include <vtextension/wasm/WasmBinary.hpp>
#include <vtextension/wasm/WasmRuntime.hpp>
#include <vtextension/wasm/wasi/Preview1Shim.hpp>
#include <vtextension/wasm/wasmtime/WasmtimeEngine.hpp>
#include <vtextension/wasm/wasmtime/WasmtimeHandles.hpp>

struct wasmtime_context;

namespace vtextension::wasm::wasmtime
{

class KindState
{
  public:
    /// Takes the next serial of the process: a thread's stamp names its kind by it, never by its address,
    /// which a later kind may take.
    KindState() noexcept;

    /// Counts one more runtime alive, bringing the engine up anew if it is the first.
    void acquireRuntime() noexcept;

    /// Counts one runtime fewer alive.
    void releaseRuntime() noexcept;

    /// Prepares the calling thread for the current generation, unless it already is.
    /// @return The guard; `RuntimeUnavailable` while no runtime is alive.
    [[nodiscard]] std::expected<std::unique_ptr<IThreadGuard>, ExtensionError> enterThread();

    /// @return Whether the calling thread is prepared for the current generation of this kind.
    [[nodiscard]] bool isCurrentThreadPrepared() const noexcept;

    /// @return The current bring-up generation; 0 before the first.
    [[nodiscard]] uint64_t generation() const noexcept { return _generation.load(); }

    /// Counts one more call of an epoch deadline callback, of any instance of the kind.
    void countEpochCallback() noexcept { _epochCallbacks.fetch_add(1, std::memory_order_relaxed); }

    /// Test-only: how often a running guest woke at its epoch deadline, which only an interrupt moves.
    /// @return The count of epoch deadline callbacks of every instance of the kind so far.
    [[nodiscard]] uint64_t epochCallbacks() const noexcept
    {
        return _epochCallbacks.load(std::memory_order_relaxed);
    }

    /// Test-only: what the calling thread was prepared for, of whichever kind.
    /// @return The generation the calling thread's guard was entered for; 0 if none is held.
    [[nodiscard]] static uint64_t preparedGeneration() noexcept;

    /// @return The kind's serial, unique in the process.
    [[nodiscard]] uint64_t serial() const noexcept { return _serial; }

  private:
    uint64_t _serial;                         ///< Names the kind in a thread's stamp.
    std::mutex _mutex;                        ///< Guards _liveRuntimes and each bring-up.
    std::size_t _liveRuntimes = 0;            ///< How many runtimes are alive.
    std::atomic<uint64_t> _generation {};     ///< The current bring-up generation.
    std::atomic<uint64_t> _epochCallbacks {}; ///< How often a running guest woke at its epoch deadline.
};

/// Whether an instance was interrupted (IWasmInstance::interrupt()).
enum class InterruptState : uint8_t
{
    Running,     ///< Not interrupted: its calls run.
    Interrupted, ///< Interrupted, for good: no call of it runs again, and a running one stops.
};

/// The data of an instance's store: what a host function the guest calls reaches, how it failed, and
/// whether the instance was interrupted.
///
/// Lives at a stable address for as long as the store, which hands it to every host function call and
/// to the epoch deadline callback.
struct StoreData
{
    IHostContext& context;                        ///< What the instance's host function calls reach.
    wasi::ShimEnvironment const& wasiEnvironment; ///< What the instance's WASI function calls read.
    KindState& kind; ///< Where the epoch deadline callback is counted, for the tests to see.
    /// Written from any thread by interrupt(); read by call() and by the epoch deadline callback.
    std::atomic<InterruptState> interrupt {};
    /// The error the latest host function call failed with, if it failed; it is what tells a call stopped
    /// by a failing host function apart from one the guest trapped in.
    std::optional<ExtensionError> hostFailure;
    /// The context Wasmtime handed the host function call running, if one is: inside it, that is the only
    /// context the store may be reached through (wasmtime/store.h), so a call made back into the
    /// instance from there, or a read of its memory, goes through it.
    wasmtime_context* callerContext = nullptr;
};

/// One host function as a runtime provides it, at a stable address its linker callback is handed.
struct HostBinding
{
    std::string module;          ///< The import module name it is provided under.
    std::string name;            ///< The import field name it is provided under.
    std::size_t index = 0;       ///< Its index in the table the runtime was created with.
    uint32_t parameterCount = 0; ///< How many i32 parameters it takes.
};

/// A runtime: one engine, configured from EngineFeatures and the runtime's limits, and one linker
/// providing its host functions.
class WasmtimeRuntime final: public IWasmRuntime, public std::enable_shared_from_this<WasmtimeRuntime>
{
  public:
    /// Brings up an engine and its linker.
    /// @param kind The state of the backends of the kind; counts this runtime alive while it lives.
    /// @param config How the engine is set up.
    /// @param limits What every instance is held to.
    /// @param hostFunctions What modules may import; copied.
    /// @return The runtime; `RuntimeUnavailable` if @p limits allow no stack at all, a host function takes
    ///         more than MaxHostParameters, or the engine or the linker could not be set up.
    [[nodiscard]] static std::expected<std::shared_ptr<WasmtimeRuntime>, ExtensionError> create(
        std::shared_ptr<KindState> kind,
        WasmtimeConfig config,
        RuntimeLimits limits,
        std::span<HostFunction const> hostFunctions);

    /// Use create(). Public for std::make_shared only.
    WasmtimeRuntime(std::shared_ptr<KindState> kind,
                    RuntimeLimits limits,
                    wasi::ShimEnvironment wasiEnvironment,
                    detail::EngineHandle engine,
                    std::vector<HostBinding> hostBindings,
                    detail::LinkerHandle linker) noexcept;

    WasmtimeRuntime(WasmtimeRuntime const&) = delete;
    WasmtimeRuntime(WasmtimeRuntime&&) = delete;
    WasmtimeRuntime& operator=(WasmtimeRuntime const&) = delete;
    WasmtimeRuntime& operator=(WasmtimeRuntime&&) = delete;
    ~WasmtimeRuntime() override;

    /// Loads @p bytes in this process; the identity is ignored, as every module runs here.
    [[nodiscard]] std::expected<std::shared_ptr<IWasmModule>, ExtensionError> load(
        ModuleIdentity identity, std::vector<std::byte> bytes) override;

    /// @return The engine modules are compiled and stores are created on.
    [[nodiscard]] wasm_engine_t* engine() const noexcept { return _engine.get(); }

    /// @return The linker providing the host functions and WASI to every instance.
    [[nodiscard]] wasmtime_linker const* linker() const noexcept { return _linker.get(); }

    /// @return The host functions the runtime provides.
    [[nodiscard]] std::span<HostBinding const> hostBindings() const noexcept { return _hostBindings; }

    /// @return The state of the backends of the kind, kept alive by the runtime.
    [[nodiscard]] KindState& kind() const noexcept { return *_kind; }

    /// @return The limits every instance is held to.
    [[nodiscard]] RuntimeLimits limits() const noexcept { return _limits; }

    /// @return Where the WASI functions of every instance read clocks and random bytes from.
    [[nodiscard]] wasi::ShimEnvironment const& wasiEnvironment() const noexcept { return _wasiEnvironment; }

  private:
    std::shared_ptr<KindState> _kind;
    RuntimeLimits _limits;
    wasi::ShimEnvironment _wasiEnvironment;
    detail::EngineHandle _engine;
    // The linker's callbacks point into _hostBindings, so the linker goes first.
    std::vector<HostBinding> _hostBindings;
    detail::LinkerHandle _linker;
};

/// A compiled module, described once at load time.
class WasmtimeModule final: public IWasmModule, public std::enable_shared_from_this<WasmtimeModule>
{
  public:
    /// @param runtime The runtime it was loaded on, kept alive by it.
    /// @param module The compiled module.
    /// @param imports Its imports, with what the runtime provides.
    /// @param exports Its exports.
    /// @param largestInitialPages The largest initial size of the memories it defines, exported or not.
    /// @param largestInitialTableElements The largest initial size of the tables it defines, exported or not.
    WasmtimeModule(std::shared_ptr<WasmtimeRuntime> runtime,
                   detail::ModuleHandle module,
                   std::vector<ImportDescription> imports,
                   std::vector<ExportDescription> exports,
                   uint32_t largestInitialPages,
                   uint32_t largestInitialTableElements) noexcept;

    [[nodiscard]] std::span<ImportDescription const> imports() const noexcept override { return _imports; }
    [[nodiscard]] std::span<ExportDescription const> exports() const noexcept override { return _exports; }
    /// Defined in WasmtimeInstance.cpp. @see IWasmModule::instantiate
    [[nodiscard]] std::expected<std::unique_ptr<IWasmInstance>, ExtensionError> instantiate(
        IHostContext& context) override;

  private:
    std::shared_ptr<WasmtimeRuntime> _runtime; // Declared first, so released after the module.
    detail::ModuleHandle _module;
    std::vector<ImportDescription> _imports;
    std::vector<ExportDescription> _exports;
    uint32_t _largestInitialPages;         ///< In pages; what instantiate() checks against the memory cap.
    uint32_t _largestInitialTableElements; ///< What instantiate() checks against the table cap.
};

/// Decides whether a runtime provides an import, by module, name and signature: one of its host
/// functions (all-i32 parameters, no result), or an allowed WASI preview 1 function with its preview 1
/// signature (wasi/Preview1Signatures.hpp).
/// @param import The import, its linkage not yet decided.
/// @param hostBindings The runtime's host functions.
/// @return Whether it is provided.
[[nodiscard]] Linkage linkageOf(ImportDescription const& import, std::span<HostBinding const> hostBindings);

/// @return The function type of @p signature.
[[nodiscard]] detail::FuncTypeHandle functionType(FunctionSignature const& signature);

/// Compiles and describes a module on @p runtime.
/// @return The module; `InvalidComponent` if Wasmtime refuses the bytes, or the module imports or
///         exports something the interface cannot describe.
[[nodiscard]] std::expected<std::shared_ptr<IWasmModule>, ExtensionError> loadModule(
    std::shared_ptr<WasmtimeRuntime> runtime, wasm_engine_t* engine, std::span<std::byte const> bytes);

} // namespace vtextension::wasm::wasmtime
