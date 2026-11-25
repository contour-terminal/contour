// SPDX-License-Identifier: Apache-2.0
//
// The contract every backend meets, through real extension host processes: what a call, a host call, an
// interrupt and a memory access do across a process boundary.
#include <catch2/catch_template_test_macros.hpp>
#include <catch2/catch_test_macros.hpp>

#include <memory>

#include <vtextension/wasm/BackendConfig.hpp>
#include <vtextension/wasm/DefaultBackend.hpp>
#include <vtextension/wasm/WasmRuntime.hpp>
#include <vtextension/wasm/testing/BackendContract.hpp>

namespace
{

struct ProcessTraits
{
    [[nodiscard]] static std::unique_ptr<vtextension::wasm::IWasmBackend> make()
    {
        return vtextension::wasm::makeDefaultBackend(vtextension::wasm::BackendConfig {});
    }
};

} // namespace

CONTOUR_WASM_BACKEND_CONTRACT(ProcessTraits)
