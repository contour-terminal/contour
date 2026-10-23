// SPDX-License-Identifier: Apache-2.0
//
// The default backend when CONTOUR_WASM_RUNTIME is wamr: this file is compiled into vtextension_wamr only.
#include <vtextension/wasm/DefaultBackend.hpp>

#include <vtextension/wasm/wamr/WamrBackend.hpp>

namespace vtextension::wasm
{

std::unique_ptr<IWasmBackend> makeDefaultBackend()
{
    return wamr::makeWamrBackend();
}

} // namespace vtextension::wasm
