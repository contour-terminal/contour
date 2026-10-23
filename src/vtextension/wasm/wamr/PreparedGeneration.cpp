// SPDX-License-Identifier: Apache-2.0
//
// The test probe of the default backend when CONTOUR_WASM_RUNTIME is wamr: compiled into vtextension_test
// only.
#include <vtextension/wasm/testing/PreparedGeneration.hpp>

#include <vtextension/wasm/wamr/WamrThreadEnv.hpp>

namespace vtextension::wasm::testing
{

uint64_t preparedGeneration() noexcept
{
    return wamr::ThreadEnv::preparedGeneration();
}

} // namespace vtextension::wasm::testing
