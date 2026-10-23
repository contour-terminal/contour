// SPDX-License-Identifier: Apache-2.0
//
// The WebAssembly runtime a build was configured with (CONTOUR_WASM_RUNTIME), named without naming it.
#pragma once

#include <memory>

#include <vtextension/wasm/WasmRuntime.hpp>

namespace vtextension::wasm
{

/// Creates a backend of the WebAssembly runtime this build selected.
///
/// Declared here and defined by the selected backend's target (vtextension_wamr for wamr), which a program
/// links through vtextension_runtime. A composition root calls this rather than naming a backend, so that
/// it compiles against whichever runtime the build was configured with.
/// @return The backend.
[[nodiscard]] std::unique_ptr<IWasmBackend> makeDefaultBackend();

} // namespace vtextension::wasm
