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
#include <span>
#include <string>
#include <vector>

#include <vtextension/ExtensionError.hpp>
#include <vtextension/wasm/WasmRuntime.hpp>
#include <vtextension/wasm/wasmtime/WasmtimeBackend.hpp>
#include <vtextension/wasm/wasmtime/WasmtimeHandles.hpp>

namespace vtextension::wasm::wasmtime
{

class KindState
{
  public:
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

    /// Test-only: what the calling thread was prepared for, of whichever kind.
    /// @return The generation the calling thread's guard was entered for; 0 if none is held.
    [[nodiscard]] static uint64_t preparedGeneration() noexcept;

  private:
    std::mutex _mutex;                    ///< Guards _liveRuntimes and each bring-up.
    std::size_t _liveRuntimes = 0;        ///< How many runtimes are alive.
    std::atomic<uint64_t> _generation {}; ///< The current bring-up generation.
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
    /// @return The runtime; `RuntimeUnavailable` if @p limits allow no stack at all, or the engine or the
    ///         linker could not be set up.
    [[nodiscard]] static std::expected<std::shared_ptr<WasmtimeRuntime>, ExtensionError> create(
        std::shared_ptr<KindState> kind,
        WasmtimeConfig config,
        RuntimeLimits limits,
        std::span<HostFunction const> hostFunctions);

    /// Use create(). Public for std::make_shared only.
    WasmtimeRuntime(std::shared_ptr<KindState> kind,
                    RuntimeLimits limits,
                    detail::EngineHandle engine,
                    std::vector<HostBinding> hostBindings,
                    detail::LinkerHandle linker) noexcept;

    WasmtimeRuntime(WasmtimeRuntime const&) = delete;
    WasmtimeRuntime(WasmtimeRuntime&&) = delete;
    WasmtimeRuntime& operator=(WasmtimeRuntime const&) = delete;
    WasmtimeRuntime& operator=(WasmtimeRuntime&&) = delete;
    ~WasmtimeRuntime() override;

    [[nodiscard]] std::expected<std::shared_ptr<IWasmModule>, ExtensionError> load(
        std::vector<std::byte> bytes) override;

    /// @return The host functions the runtime provides.
    [[nodiscard]] std::span<HostBinding const> hostBindings() const noexcept { return _hostBindings; }

    /// @return The limits every instance is held to.
    [[nodiscard]] RuntimeLimits limits() const noexcept { return _limits; }

  private:
    std::shared_ptr<KindState> _kind;
    RuntimeLimits _limits;
    detail::EngineHandle _engine;
    // The linker's callbacks point into _hostBindings, so the linker goes first.
    std::vector<HostBinding> _hostBindings;
    detail::LinkerHandle _linker;
};

/// A compiled module, described once at load time.
class WasmtimeModule final: public IWasmModule
{
  public:
    /// @param runtime The runtime it was loaded on, kept alive by it.
    /// @param module The compiled module.
    /// @param imports Its imports, with what the runtime provides.
    /// @param exports Its exports.
    WasmtimeModule(std::shared_ptr<WasmtimeRuntime> runtime,
                   detail::ModuleHandle module,
                   std::vector<ImportDescription> imports,
                   std::vector<ExportDescription> exports) noexcept;

    [[nodiscard]] std::span<ImportDescription const> imports() const noexcept override { return _imports; }
    [[nodiscard]] std::span<ExportDescription const> exports() const noexcept override { return _exports; }
    [[nodiscard]] std::expected<std::unique_ptr<IWasmInstance>, ExtensionError> instantiate(
        IHostContext& context) override;

  private:
    std::shared_ptr<WasmtimeRuntime> _runtime; // Declared first, so released after the module.
    detail::ModuleHandle _module;
    std::vector<ImportDescription> _imports;
    std::vector<ExportDescription> _exports;
};

/// Decides whether a runtime provides an import, by module, name and signature: one of its host
/// functions (all-i32 parameters, no result), or an allowed WASI preview 1 function with its preview 1
/// signature (wasi/Preview1Signatures.hpp).
/// @param import The import, its linkage not yet decided.
/// @param hostBindings The runtime's host functions.
/// @return Whether it is provided.
[[nodiscard]] Linkage linkageOf(ImportDescription const& import, std::span<HostBinding const> hostBindings);

/// Compiles and describes a module on @p runtime.
/// @return The module; `InvalidComponent` if Wasmtime refuses the bytes, or the module imports or
///         exports something the interface cannot describe.
[[nodiscard]] std::expected<std::shared_ptr<IWasmModule>, ExtensionError> loadModule(
    std::shared_ptr<WasmtimeRuntime> runtime, wasm_engine_t* engine, std::span<std::byte const> bytes);

} // namespace vtextension::wasm::wasmtime
