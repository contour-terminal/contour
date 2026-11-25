// SPDX-License-Identifier: Apache-2.0
//
// The default backend when CONTOUR_WASM_RUNTIME is wamr: this file is compiled into vtextension_wamr only.
#include <vtextension/wasm/DefaultBackend.hpp>

#include <utility>

#include <vtextension/wasm/wamr/WamrBackend.hpp>

namespace vtextension::wasm
{

std::unique_ptr<IWasmBackend> makeDefaultBackend(BackendConfig config)
{
    // WAMR runs extensions in this process: there is no host process for the config to describe, and it is
    // dropped here.
    auto const dropped = std::move(config);
    return wamr::makeWamrBackend();
}

} // namespace vtextension::wasm
