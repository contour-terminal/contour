// SPDX-License-Identifier: Apache-2.0
//
// The engine of the extension host executable: Wasmtime's.
#include <vtextension/wasm/host/HelperEngine.hpp>

#include <vtextension/wasm/BuildInstrumentation.hpp>
#include <vtextension/wasm/remote/transport/GuestMemoryMapping.hpp>
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

    /// @return A new engine of the process's one kind state, its instances' memory as @p memory says.
    [[nodiscard]] std::unique_ptr<IHostedEngine> makeEngine(wasmtime::MemorySharing memory)
    {
        static auto const kind = wasmtime::makeKindState();
        return wasmtime::makeWasmtimeEngine(
            kind, wasmtime::WasmtimeConfig { .trapMode = HelperTrapMode, .memory = memory });
    }
} // namespace

std::unique_ptr<IHostedEngine> makeHelperEngine()
{
    // Its Hello says which: Contour drives either.
    return makeEngine(remote::transport::canMapGuestMemory() ? wasmtime::MemorySharing::Provided
                                                             : wasmtime::MemorySharing::Private);
}

std::unique_ptr<IHostedEngine> makeCopyingHelperEngine()
{
    return makeEngine(wasmtime::MemorySharing::Private);
}

} // namespace vtextension::wasm::host
