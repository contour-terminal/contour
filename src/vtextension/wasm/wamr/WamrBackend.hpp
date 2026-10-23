// SPDX-License-Identifier: Apache-2.0
//
// The WAMR backend of the WebAssembly runtime interface: the fast interpreter, with WASI preview 1.
#pragma once

#include <memory>

#include <vtextension/wasm/WasmRuntime.hpp>
#include <vtextension/wasm/wamr/WamrThreadEnv.hpp>

namespace vtextension::wasm::wamr
{

/// Creates a WAMR backend.
///
/// WAMR's runtime, its reference-counted initialization and its per-thread environment are process-wide,
/// so the backend is a facade over that one state (@see ThreadEnv, acquireRuntime): any number of WAMR
/// backends may exist in a process, and all of them behave as one. Refused memory growth is reported
/// (MemoryGrowthReporting::Reported).
/// @return The backend.
[[nodiscard]] std::unique_ptr<IWasmBackend> makeWamrBackend();

} // namespace vtextension::wasm::wamr
