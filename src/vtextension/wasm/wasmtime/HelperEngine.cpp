// SPDX-License-Identifier: Apache-2.0
//
// The engine of the extension host executable: Wasmtime's.
#include <vtextension/wasm/host/HelperEngine.hpp>

#include <vtextension/wasm/BuildInstrumentation.hpp>
#include <vtextension/wasm/wasmtime/WasmtimeEngine.hpp>

namespace vtextension::wasm::host
{

namespace
{
    /// How the helper's engine traps: in software where a sanitizer or coverage owns the signal handlers,
    /// through guard pages and signals otherwise.
    constexpr auto HelperTrapMode = ThisBuildInstrumentation == BuildInstrumentation::None
                                        ? wasmtime::TrapMode::Signals
                                        : wasmtime::TrapMode::Software;
} // namespace

std::unique_ptr<IHostedEngine> makeHelperEngine()
{
    static auto const kind = wasmtime::makeKindState();
    return wasmtime::makeWasmtimeEngine(kind, wasmtime::WasmtimeConfig { .trapMode = HelperTrapMode });
}

} // namespace vtextension::wasm::host
