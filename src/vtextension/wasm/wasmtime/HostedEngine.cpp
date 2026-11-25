// SPDX-License-Identifier: Apache-2.0
//
// The hosted-engine probe of the Wasmtime engine, for the host's test binary: compiled into
// vtextension_remote_test only.
#include <vtextension/wasm/testing/HostedEngine.hpp>

#include <vtextension/wasm/wasmtime/WasmtimeEngine.hpp>

namespace vtextension::wasm::testing
{

std::unique_ptr<host::IHostedEngine> makeHostedEngine()
{
    static auto const kind = wasmtime::makeKindState();
    return wasmtime::makeWasmtimeEngine(
        kind, wasmtime::WasmtimeConfig { .trapMode = wasmtime::TrapMode::Software });
}

} // namespace vtextension::wasm::testing
