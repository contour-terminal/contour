// SPDX-License-Identifier: Apache-2.0
//
// The test probe of the Wasmtime backend, for its own test binary: compiled into vtextension_wasmtime_test
// only.
#include <vtextension/wasm/testing/PreparedGeneration.hpp>

#include <vtextension/wasm/wasmtime/WasmtimeRuntime.hpp>

namespace vtextension::wasm::testing
{

uint64_t preparedGeneration() noexcept
{
    return wasmtime::KindState::preparedGeneration();
}

} // namespace vtextension::wasm::testing
