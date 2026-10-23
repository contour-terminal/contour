// SPDX-License-Identifier: Apache-2.0
//
// The seam between the extension host and the WebAssembly runtime that executes extensions: a
// core-module interface, below the Canonical ABI, small enough that any mature engine can implement it.
// Nothing here names a runtime; a backend (wasm/wamr/ for WAMR) implements it, and the composition root
// picks one and injects it.
//
// Every guarantee the extension host's security rests on is part of this contract, not left to a
// backend's good intentions: an interrupt that stops a call for good, a memory cap and a stack cap,
// truthful import descriptions, synchronous execution, and no process signal handler. How a fault is
// labelled may vary between backends (see MemoryGrowthReporting), never whether it is stopped.
#pragma once

#include <cstddef>
#include <cstdint>
#include <expected>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include <vtextension/ExtensionError.hpp>

namespace vtextension::wasm
{

/// A core WebAssembly value type. Every flat value of the statusline world is an i32; the others are
/// named so that a module using them is described truthfully, and refused by the world check.
enum class ValueType : uint8_t
{
    Other, ///< Any type not named here: v128, a reference type.
    I32,
    I64,
    F32,
    F64,
};

/// What an import or export is.
enum class ExternKind : uint8_t
{
    Function,
    Memory,
    Table,
    Global,
};

/// Whether the runtime provides an import.
///
/// Satisfaction is decided against what the backend provides, which need not be per runtime: a backend
/// whose host-function registry is process-wide (WAMR's is) resolves an import against every runtime
/// of its kind alive at load time, and may link it to another runtime's registration of the same host
/// function. That is harmless only because runtimes alive at once provide each host function alike
/// (@see IWasmBackend::createRuntime); what is promised is that the import, once satisfied, stays
/// callable for as long as the module lives.
enum class Linkage : uint8_t
{
    Unsatisfied, ///< Nothing the runtime provides matches it, by name or by signature.
    Satisfied,   ///< The runtime provides it with this very signature; calling it reaches the provider.
};

/// Whether a backend tells a trap caused by refused memory growth apart from any other trap.
enum class MemoryGrowthReporting : uint8_t
{
    Unreported, ///< Such a trap fails a call as CallFailure::Trap.
    Reported,   ///< Such a trap fails a call as CallFailure::MemoryGrowthRefused.
};

/// Why an export call failed.
enum class CallFailure : uint8_t
{
    Trap,                ///< The guest trapped, for any reason not named below.
    Interrupted,         ///< IWasmInstance::interrupt() stopped the call, or had before it began.
    MemoryGrowthRefused, ///< The guest trapped after its memory refused to grow past the cap; only
                         ///< from a backend declaring MemoryGrowthReporting::Reported.
    HostFailure,         ///< A host function the guest called failed (IHostContext::callHost).
};

/// A function's core signature.
struct FunctionSignature
{
    std::vector<ValueType> parameters; ///< The parameter types, in order.
    std::vector<ValueType> results;    ///< The result types, in order.

    /// @return Whether both signatures name the same types in the same order.
    bool operator==(FunctionSignature const&) const = default;
};

/// One import a module declares.
struct ImportDescription
{
    std::string module;                     ///< The import's module name.
    std::string name;                       ///< The import's field name.
    ExternKind kind = ExternKind::Function; ///< What is imported.
    FunctionSignature signature;            ///< The function's signature; empty unless kind is Function.
    /// Whether the runtime provides it; decided at load time and final.
    Linkage linkage = Linkage::Unsatisfied;
};

/// One export a module declares.
struct ExportDescription
{
    std::string name;                       ///< The export's name.
    ExternKind kind = ExternKind::Function; ///< What is exported.
    FunctionSignature signature;            ///< The function's signature; empty unless kind is Function.
    uint32_t initialPages = 0; ///< The memory's initial size in 64 KiB pages; 0 unless kind is Memory.
};

/// The limits a runtime holds every instance it creates to.
struct RuntimeLimits
{
    std::size_t memoryBytes = 0; ///< The cap on an instance's linear memory; growth past it is refused.
    std::size_t stackBytes = 0;  ///< The cap on an instance's stack; recursion past it traps.
};

/// One host import. Every world host function takes only i32s and returns nothing.
struct HostFunction
{
    std::string_view module;     ///< The import module name it is provided under.
    std::string_view name;       ///< The import field name it is provided under.
    uint32_t parameterCount = 0; ///< How many i32 parameters it takes.
};

/// A function export looked up on an instance, to be called through it.
struct ExportFunction
{
    uint32_t index = 0; ///< A backend-defined handle, valid only for the instance that handed it out.
};

/// What a backend can do beyond what the contract requires of every backend.
struct BackendCapabilities
{
    /// Whether refused memory growth is told apart from other traps.
    MemoryGrowthReporting memoryGrowth = MemoryGrowthReporting::Unreported;
};

/// The host side of one instance: what a host function call made from inside it reaches.
/// Implemented by the extension host; handed to IWasmModule::instantiate().
class IHostContext
{
  public:
    IHostContext() = default;
    IHostContext(IHostContext const&) = delete;
    IHostContext(IHostContext&&) = delete;
    IHostContext& operator=(IHostContext const&) = delete;
    IHostContext& operator=(IHostContext&&) = delete;
    virtual ~IHostContext() = default;

    /// Runs one host function on behalf of the guest.
    ///
    /// Called synchronously, on the calling thread, from inside a guest call: never from a thread of
    /// the backend's own, never after the export call that made it has returned. Must not throw.
    /// @param functionIndex The host function's index in the table the runtime was created with.
    /// @param arguments Its flat i32 arguments.
    /// @return Nothing on success; an error makes the backend stop the guest call, which then fails
    ///         as CallFailure::HostFailure.
    [[nodiscard]] virtual std::expected<void, ExtensionError> callHost(
        std::size_t functionIndex, std::span<uint32_t const> arguments) noexcept = 0;
};

/// One instantiated module.
///
/// Calls into one instance never overlap, but may come from any prepared thread (@see IWasmBackend);
/// different instances of one runtime may run on different threads at once. An export call runs on the
/// calling thread, and every host function it calls runs synchronously on that same thread.
class IWasmInstance
{
  public:
    IWasmInstance() = default;
    IWasmInstance(IWasmInstance const&) = delete;
    IWasmInstance(IWasmInstance&&) = delete;
    IWasmInstance& operator=(IWasmInstance const&) = delete;
    IWasmInstance& operator=(IWasmInstance&&) = delete;
    /// Releases the instance; the module (and with it the runtime) may go with it.
    virtual ~IWasmInstance() = default;

    /// Looks a function export up, once, for later calls.
    /// @param name The export's name.
    /// @return Its handle; nothing if the instance exports no function of that name.
    [[nodiscard]] virtual std::optional<ExportFunction> function(std::string_view name) const = 0;

    /// Calls a function export on the calling thread, which must be prepared.
    /// @param function A handle function() returned on this instance.
    /// @param argv The parameters on entry, the results on return; as large as the larger of the two.
    /// @return Nothing on success; otherwise why the call failed.
    [[nodiscard]] virtual std::expected<void, CallFailure> call(ExportFunction function,
                                                                std::span<uint32_t> argv) = 0;

    /// A bounds-checked view of the instance's linear memory. Valid until the next call into the
    /// instance, which may grow (and so move) the memory.
    /// @param address The guest address the view starts at.
    /// @param size The view's size in bytes; 0 yields an empty span.
    /// @return The view; nothing if any byte of it lies outside the memory.
    [[nodiscard]] virtual std::optional<std::span<std::byte>> memory(uint32_t address, uint32_t size) = 0;

    /// Stops the call running in the instance, if any, for good: callable from any thread, and final.
    /// Every later call() fails as CallFailure::Interrupted without entering the guest, including when
    /// no call was running at the moment of the interrupt. A per-call check of a flag could not close
    /// the window between the check and the entry; a sticky interrupt does.
    virtual void interrupt() noexcept = 0;
};

/// A loaded, uninstantiated module. Describing it runs none of its code.
class IWasmModule
{
  public:
    IWasmModule() = default;
    IWasmModule(IWasmModule const&) = delete;
    IWasmModule(IWasmModule&&) = delete;
    IWasmModule& operator=(IWasmModule const&) = delete;
    IWasmModule& operator=(IWasmModule&&) = delete;
    /// Releases the module; the runtime may go with it.
    virtual ~IWasmModule() = default;

    /// @return The module's imports, in declaration order, each with whether the runtime provides it.
    ///         A backend reports this truthfully at load time: an import described as satisfied never
    ///         traps as unlinked when called. Applying an allow-list is the caller's business.
    [[nodiscard]] virtual std::span<ImportDescription const> imports() const noexcept = 0;

    /// @return The module's exports, in declaration order.
    [[nodiscard]] virtual std::span<ExportDescription const> exports() const noexcept = 0;

    /// Instantiates the module under the runtime's limits, with WASI preview 1 as the backend provides
    /// it: no arguments, no environment, no preopened directory, and standard streams whose writes
    /// reach nobody. May run on another prepared thread than the one that loaded the module.
    /// @param context What the instance's host function calls reach; must outlive the instance.
    /// @return The instance, which keeps the module alive; `OutOfMemory` if a memory's initial size
    ///         exceeds the runtime's memory cap, `InvalidComponent` if the runtime cannot instantiate the
    ///         module, `RuntimeUnavailable` on a resource failure.
    [[nodiscard]] virtual std::expected<std::unique_ptr<IWasmInstance>, ExtensionError> instantiate(
        IHostContext& context) = 0;
};

/// The handle on one running engine, created with its host functions and its limits.
///
/// The limits are per runtime: every instance it creates is held to them. Two instances needing
/// different limits need two runtimes.
class IWasmRuntime
{
  public:
    IWasmRuntime() = default;
    IWasmRuntime(IWasmRuntime const&) = delete;
    IWasmRuntime(IWasmRuntime&&) = delete;
    IWasmRuntime& operator=(IWasmRuntime const&) = delete;
    IWasmRuntime& operator=(IWasmRuntime&&) = delete;
    /// Releases the runtime; with the last one alive, the engine is torn down.
    virtual ~IWasmRuntime() = default;

    /// Loads a module. Runs none of its code.
    /// @param bytes The module's bytes; the module keeps them.
    /// @return The module, which keeps the runtime alive; `InvalidComponent` if the bytes are not a
    ///         valid core module.
    [[nodiscard]] virtual std::expected<std::shared_ptr<IWasmModule>, ExtensionError> load(
        std::vector<std::byte> bytes) = 0;
};

/// A thread's preparation to call into instances, held for as long as the guard lives.
class IThreadGuard
{
  public:
    IThreadGuard() = default;
    IThreadGuard(IThreadGuard const&) = delete;
    IThreadGuard(IThreadGuard&&) = delete;
    IThreadGuard& operator=(IThreadGuard const&) = delete;
    IThreadGuard& operator=(IThreadGuard&&) = delete;
    /// Ends the preparation; must run on the thread that entered the guard.
    virtual ~IThreadGuard() = default;

    /// @return The runtime generation the guard was entered for. @see IWasmBackend::runtimeGeneration
    [[nodiscard]] virtual uint64_t generation() const noexcept = 0;
};

/// One kind of WebAssembly runtime, as the composition root creates and injects it.
///
/// **One per kind.** A backend may be created more than once per process, and all backends of one kind
/// behave as one: a runtime's state may be process-wide, and two backend objects never disagree
/// about whether a thread is prepared or which generation is current.
///
/// **Threading.** Calls into one instance never overlap, but may come from any prepared thread; a module
/// may be loaded on one prepared thread and instantiated on another; different instances of one
/// runtime may run on different threads at once. A runtime with single-entry execution contexts meets
/// this internally.
///
/// **Synchronous execution.** An export call runs on the calling thread, and every host function it
/// calls runs synchronously on that same thread before the call returns.
///
/// **No signal handlers.** A backend installs no process signal handler, neither a trap handler for
/// out-of-bounds accesses nor a wake-up signal: signal state is process-wide, and belongs to the
/// process (and to a sanitizer, if any).
///
/// **Host functions.** A backend's registry of host functions may be process-wide, as WAMR's is: an
/// import is then resolved against the host functions of every runtime of the kind alive at load time,
/// not only the one loading the module, and a runtime cannot keep another from providing a name.
/// Runtimes alive at once must therefore provide each host function alike (same module, name, index
/// and parameter count), which they do when the extension host creates them all from one table. In
/// return, an import satisfied at load stays callable for as long as its module lives, whichever
/// runtime it was resolved against, and dropping one runtime never unlinks another's modules.
///
/// **WASI.** A backend provides at least every WASI preview 1 function the extension host allows,
/// with no arguments, no environment, no preopened directory, and standard streams whose writes reach
/// nobody. Providing more is harmless: the host refuses modules importing what it does not allow.
class IWasmBackend
{
  public:
    IWasmBackend() = default;
    IWasmBackend(IWasmBackend const&) = delete;
    IWasmBackend(IWasmBackend&&) = delete;
    IWasmBackend& operator=(IWasmBackend const&) = delete;
    IWasmBackend& operator=(IWasmBackend&&) = delete;
    /// Must outlive every runtime created from it.
    virtual ~IWasmBackend() = default;

    /// Creates a runtime. The engine is brought up with the first runtime alive and torn down with
    /// the last, so a process creating none starts nothing.
    /// @param limits The limits every instance of the runtime is held to.
    /// @param hostFunctions The host functions modules may import; IHostContext::callHost() receives an
    ///                      index into this table. The runtime copies what it needs. Runtimes alive at
    ///                      once must agree on every host function they share (see the class notes).
    /// @return The runtime; `RuntimeUnavailable` if the engine could not be brought up, or the backend
    ///         cannot provide one of @p hostFunctions.
    [[nodiscard]] virtual std::expected<std::shared_ptr<IWasmRuntime>, ExtensionError> createRuntime(
        RuntimeLimits limits, std::span<HostFunction const> hostFunctions) = 0;

    /// Prepares the calling thread to call into instances, for as long as the guard lives. Guards
    /// nest: an inner one on a prepared thread does nothing. A guard must be destroyed on the thread
    /// that entered it. A guard entered for an earlier generation no longer prepares its thread.
    /// @return The guard; `RuntimeUnavailable` when no runtime is up, or the thread cannot be prepared.
    [[nodiscard]] virtual std::expected<std::unique_ptr<IThreadGuard>, ExtensionError> enterThread() = 0;

    /// @return Whether the calling thread may call into instances of the current generation's runtimes.
    [[nodiscard]] virtual bool isCurrentThreadPrepared() const = 0;

    /// @return Which bring-up of the engine is current: it counts up each time the engine is brought up
    ///         anew (no runtime being alive); 0 before the first. A backend needing no thread
    ///         preparation may keep it constant.
    [[nodiscard]] virtual uint64_t runtimeGeneration() const noexcept = 0;

    /// @return What the backend can do beyond the contract.
    [[nodiscard]] virtual BackendCapabilities capabilities() const noexcept = 0;
};

} // namespace vtextension::wasm
