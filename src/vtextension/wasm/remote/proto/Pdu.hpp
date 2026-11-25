// SPDX-License-Identifier: Apache-2.0
#pragma once

/// @file
/// The vocabulary Contour and its extension host process speak: one struct per PDU, and the enums they carry.
///
/// Every request and every reply is a PDU of its own kind. A PDU travels one way on one channel: the control
/// channel carries what creates and releases runtimes, modules and instances, and a lane carries one thread's
/// calls, host calls and memory accesses. The codec (Codec.hpp) refuses a PDU arriving the other way or on
/// the other channel, as it refuses one larger than its kind may be (PduTable.hpp).

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <variant>
#include <vector>

#include <vtextension/ExtensionError.hpp>
#include <vtextension/ExtensionLimits.hpp>
#include <vtextension/wasm/WasmRuntime.hpp>

namespace vtextension::wasm::remote::proto
{

/// The protocol version both ends must speak; the helper states it in its Hello.
inline constexpr uint32_t ProtocolVersion = 1;

/// The most values a Call carries as arguments, or a CallResult as results.
inline constexpr std::size_t MaxCallValues = 64;

/// The most arguments a CallHost carries: the most parameters a host function takes, on every engine.
inline constexpr std::size_t MaxHostParameters = wasm::MaxHostParameters;

// The most elements each list a PDU carries may hold, checked against its count before any element is read:
// what a list costs once decoded is far more than its encoding, which is all the byte caps bound. The module
// limits are those Wasmtime's parser applies, so that no module it loads is described past them.

/// The most imports a Loaded describes.
inline constexpr std::size_t MaxImports = 100'000;

/// The most exports a Loaded describes, and the most callable exports an Instantiated names.
inline constexpr std::size_t MaxExports = 100'000;

/// The most parameters, and separately the most results, a described function signature has.
inline constexpr std::size_t MaxSignatureValues = 1'000;

/// The most host functions a CreateRuntime provides.
inline constexpr std::size_t MaxHostFunctions = 1'024;

/// How the helper shares an instance's linear memory with the parent.
enum class MemoryMode : uint8_t
{
    Copy,   ///< Memory is read and written through MemoryRead, MemoryData and MemoryWrite PDUs.
    Shared, ///< Memory lives in an object both processes map.
};

/// How far the helper confined itself before it said Hello.
enum class LockdownState : uint8_t
{
    Failed,      ///< It tried and failed; the parent refuses it.
    Unsandboxed, ///< It runs without a sandbox on this platform.
    LockedDown,  ///< It runs inside its sandbox.
};

/// Which way a PDU travels.
enum class Direction : uint8_t
{
    ToHelper, ///< From Contour to the extension host process.
    ToParent, ///< From the extension host process to Contour.
};

/// Which channel a PDU travels on.
enum class ChannelKind : uint8_t
{
    Control, ///< The one channel that creates and releases runtimes, modules and instances.
    Lane,    ///< One of the channels a calling thread runs its calls on.
};

/// What a Release PDU releases.
enum class ReleaseTarget : uint8_t
{
    Instance,
    Module,
    Runtime,
};

/// The kind of a PDU: its tag on the wire, and its row in the PduTable. Zero names none.
enum class PduKind : uint8_t
{
    Hello = 1,
    CreateRuntime,
    RuntimeCreated,
    Load,
    Loaded,
    Instantiate,
    Instantiated,
    Release,
    Call,
    CallHost,
    HostResult,
    CallResult,
    MemoryRead,
    MemoryData,
    MemoryWrite,
};

/// Whether a MemoryRead found its range inside the instance's memory.
enum class MemoryStatus : uint8_t
{
    OutOfBounds, ///< The range runs past the memory's end; nothing was read.
    Ok,          ///< The bytes follow.
};

/// The size limits a stream applies to the PDUs it receives, beside the fixed ones in the PduTable.
struct PduLimits
{
    uint64_t maxModuleBytes = uint64_t { 16 } * 1024 * 1024; ///< The largest module a Load may carry.
    uint64_t maxMemoryBytes = uint64_t { 32 } * 1024 * 1024; ///< The largest instance memory.
};

static_assert(PduLimits {}.maxModuleBytes == ExtensionLimits {}.maxModuleBytes,
              "a stream accepts the largest module the host loads");
static_assert(PduLimits {}.maxMemoryBytes == ExtensionLimits {}.memoryBytes,
              "a stream accepts the largest memory an instance may have");

/// One host function a runtime provides, as CreateRuntime names it.
struct WireHostFunction
{
    std::string module;          ///< The import module name it is provided under.
    std::string name;            ///< The import field name it is provided under.
    uint32_t parameterCount = 0; ///< How many i32 parameters it takes.

    bool operator==(WireHostFunction const&) const = default;
};

/// The helper's first PDU on the control channel: what it speaks and how it runs.
struct Hello
{
    uint32_t version = 0;                           ///< The protocol version it speaks.
    MemoryMode memoryMode = MemoryMode::Copy;       ///< How it shares instance memory.
    LockdownState lockdown = LockdownState::Failed; ///< How far it confined itself.

    bool operator==(Hello const&) const = default;
};

/// Creates a runtime in the helper.
struct CreateRuntime
{
    uint32_t runtimeId = 0;   ///< The parent-chosen id the runtime is known by.
    uint64_t memoryBytes = 0; ///< RuntimeLimits::memoryBytes.
    uint64_t stackBytes = 0;  ///< RuntimeLimits::stackBytes.
    /// RuntimeLimits::tableElements; the helper refuses more than MaxTableElements.
    uint64_t tableElements = MaxTableElements;
    std::vector<WireHostFunction>
        functions; ///< Its host functions, indexed by CallHost; at most MaxHostFunctions.

    bool operator==(CreateRuntime const&) const = default;
};

/// Answers CreateRuntime.
struct RuntimeCreated
{
    uint32_t runtimeId = 0;              ///< The runtime it answers for.
    std::optional<ExtensionError> error; ///< Why it was not created; nothing if it was.

    bool operator==(RuntimeCreated const&) const = default;
};

/// Loads a module into a runtime.
struct Load
{
    uint32_t runtimeId = 0;       ///< The runtime it is loaded into.
    uint32_t moduleId = 0;        ///< The parent-chosen id the module is known by.
    std::vector<std::byte> bytes; ///< The module's bytes.

    bool operator==(Load const&) const = default;
};

/// Answers Load.
struct Loaded
{
    uint32_t moduleId = 0;                  ///< The module it answers for.
    std::optional<ExtensionError> error;    ///< Why it was not loaded; nothing if it was.
    std::vector<ImportDescription> imports; ///< What the module imports, in order; at most MaxImports.
    std::vector<ExportDescription> exports; ///< What the module exports, in order; at most MaxExports.

    bool operator==(Loaded const&) const = default;
};

/// Instantiates a module.
struct Instantiate
{
    uint32_t moduleId = 0;   ///< The module to instantiate.
    uint32_t instanceId = 0; ///< The parent-chosen id the instance is known by.

    bool operator==(Instantiate const&) const = default;
};

/// Answers Instantiate.
struct Instantiated
{
    uint32_t instanceId = 0;               ///< The instance it answers for.
    std::optional<ExtensionError> error;   ///< Why it was not instantiated; nothing if it was.
    std::vector<uint32_t> callableExports; ///< The export ordinals a Call may name; at most MaxExports.

    bool operator==(Instantiated const&) const = default;
};

/// Releases a runtime, module or instance the parent no longer holds.
struct Release
{
    ReleaseTarget target = ReleaseTarget::Instance; ///< What kind of object is released.
    uint32_t id = 0;                                ///< Its id.

    bool operator==(Release const&) const = default;
};

/// Calls an export of an instance.
struct Call
{
    uint32_t instanceId = 0;    ///< The instance called.
    uint32_t exportOrdinal = 0; ///< Which of its exports, by ordinal.
    std::vector<uint32_t> argv; ///< The i32 arguments; at most MaxCallValues.

    bool operator==(Call const&) const = default;
};

/// The guest called a host function while a Call ran; the parent runs it and answers HostResult.
struct CallHost
{
    uint32_t functionIndex = 0;      ///< Which of the runtime's host functions, by index.
    std::vector<uint32_t> arguments; ///< Its i32 arguments; at most MaxHostParameters.

    bool operator==(CallHost const&) const = default;
};

/// Answers CallHost.
struct HostResult
{
    std::optional<ExtensionError> error; ///< Why the host function failed; nothing if it did not.

    bool operator==(HostResult const&) const = default;
};

/// Answers Call.
struct CallResult
{
    std::optional<CallFailure> failure; ///< Why the call failed; nothing if it returned.
    std::vector<uint32_t> argv;         ///< The i32 results; at most MaxCallValues.

    bool operator==(CallResult const&) const = default;
};

/// Reads a range of an instance's memory, while a Call runs or between calls.
struct MemoryRead
{
    uint32_t instanceId = 0; ///< Whose memory.
    uint32_t address = 0;    ///< Where the range starts.
    uint32_t size = 0;       ///< How many bytes it spans.

    bool operator==(MemoryRead const&) const = default;
};

/// Answers MemoryRead.
struct MemoryData
{
    MemoryStatus status = MemoryStatus::OutOfBounds; ///< Whether the range was inside the memory.
    std::vector<std::byte> bytes;                    ///< The bytes read; empty unless Ok.

    bool operator==(MemoryData const&) const = default;
};

/// Writes a range of an instance's memory. Unanswered: a write out of bounds fails the call it belongs to.
struct MemoryWrite
{
    uint32_t instanceId = 0;      ///< Whose memory.
    uint32_t address = 0;         ///< Where the range starts.
    std::vector<std::byte> bytes; ///< What is written there.

    bool operator==(MemoryWrite const&) const = default;
};

/// Any PDU. Its alternatives are in PduKind order: alternative `i` is the PDU of kind `i + 1`.
using Pdu = std::variant<Hello,
                         CreateRuntime,
                         RuntimeCreated,
                         Load,
                         Loaded,
                         Instantiate,
                         Instantiated,
                         Release,
                         Call,
                         CallHost,
                         HostResult,
                         CallResult,
                         MemoryRead,
                         MemoryData,
                         MemoryWrite>;

static_assert(std::variant_size_v<Pdu> == static_cast<std::size_t>(PduKind::MemoryWrite),
              "one Pdu alternative per PduKind");

} // namespace vtextension::wasm::remote::proto
