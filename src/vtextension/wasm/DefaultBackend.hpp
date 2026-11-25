// SPDX-License-Identifier: Apache-2.0
//
// The WebAssembly runtime a build was configured with (CONTOUR_WASM_RUNTIME), named without naming it.
#pragma once

#include <memory>

#include <vtextension/wasm/BackendConfig.hpp>
#include <vtextension/wasm/WasmRuntime.hpp>

namespace vtextension::wasm
{

/// Creates a backend of the WebAssembly runtime this build selected.
///
/// Declared here and defined by the selected backend's target (vtextension_wamr for wamr), which a program
/// links through vtextension_runtime. A composition root calls this rather than naming a backend, so that
/// it compiles against whichever runtime the build was configured with.
/// @param config How the backend is set up: where an extension host process comes from, and how long it may
///        take. A backend running extensions in this process ignores it.
/// @return The backend.
[[nodiscard]] std::unique_ptr<IWasmBackend> makeDefaultBackend(BackendConfig config);

} // namespace vtextension::wasm
