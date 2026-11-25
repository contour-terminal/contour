// SPDX-License-Identifier: Apache-2.0
//
// The engine of the extension host executable: WAMR's, where the build selected it.
#include <vtextension/wasm/host/HelperEngine.hpp>

#include <format>
#include <utility>

#include <vtextension/wasm/host/HostProcess.hpp>
#include <vtextension/wasm/wamr/WamrEngine.hpp>

namespace vtextension::wasm::host
{

std::unique_ptr<IHostedEngine> makeHelperEngine()
{
    // WAMR places linear memory where it chooses, so its instances copy memory wherever they run, and its
    // Hello says so. Every engine shares WAMR's one process-wide state.
    auto engine = wamr::makeWamrEngine();
    if (engine)
        return std::move(*engine);
    // Why, before the host ends for the engine it lacks.
    tellOnStandardError(std::format("no engine: {}", wamr::wamrEngineErrorText(engine.error())));
    return nullptr;
}

std::unique_ptr<IHostedEngine> makeCopyingHelperEngine()
{
    return makeHelperEngine();
}

} // namespace vtextension::wasm::host
