// SPDX-License-Identifier: Apache-2.0
//
// The Wasmtime backend of the WebAssembly runtime interface: extensions compiled to native code by
// Cranelift, one engine per runtime, configured from the one feature table (../EngineFeatures.hpp).
#pragma once

#include <cstdint>
#include <memory>

#include <vtextension/wasm/WasmRuntime.hpp>
#include <vtextension/wasm/wasi/Preview1Shim.hpp>
#include <vtextension/wasm/wasi/SystemEnvironment.hpp>

namespace vtextension::wasm::wasmtime
{

/// How compiled guest code detects an out-of-bounds access or a stack overflow.
enum class TrapMode : uint8_t
{
    Software, ///< Explicit bounds and stack checks in the compiled code; no signal handler is installed.
    Signals,  ///< Guard pages and a process signal handler, as Wasmtime does by default.
};

/// How every Wasmtime backend of a process is set up.
struct WasmtimeConfig
{
    /// How traps are detected. Software keeps the contract's promise of no process signal handler.
    TrapMode trapMode = TrapMode::Software;
    /// Where the WASI functions every instance imports read clocks and random bytes from.
    wasi::ShimEnvironment wasiEnvironment = wasi::systemEnvironment();
};

/// What all Wasmtime backends of a process share: the bring-up generation and the count of runtimes
/// alive. Created once by the composition root and handed to every backend it makes, so that they
/// behave as one (@see IWasmBackend).
class KindState;

/// Creates the state Wasmtime backends of one process share.
/// @return The state, with no runtime alive and generation 0.
[[nodiscard]] std::shared_ptr<KindState> makeKindState();

/// Creates a Wasmtime backend.
/// @param kind The state every Wasmtime backend of the process shares.
/// @param config How the backend sets up each engine it creates.
/// @return The backend. Refused memory growth is not reported (MemoryGrowthReporting::Unreported): the
///         guest's memory.grow returns -1, and whatever the guest does about it fails the call.
[[nodiscard]] std::unique_ptr<IWasmBackend> makeWasmtimeBackend(std::shared_ptr<KindState> kind,
                                                                WasmtimeConfig config);

} // namespace vtextension::wasm::wasmtime
