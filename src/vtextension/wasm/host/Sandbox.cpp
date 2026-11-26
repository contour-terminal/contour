// SPDX-License-Identifier: Apache-2.0
//
// What every system's sandbox shares: how a step that failed is phrased.
#include <vtextension/wasm/host/Sandbox.hpp>

#include <string>

#include <vtextension/wasm/remote/proto/LockdownSteps.hpp>

namespace vtextension::wasm::host
{

std::string confinementFailureText(ConfinementFailure const& failure)
{
    // An HRESULT is negative as an int: its bit pattern is what a Hello carries, and what is phrased.
    return remote::proto::lockdownFailureText(
        remote::proto::LockdownFailure { .step = failure.step, .code = static_cast<uint32_t>(failure.code) });
}

} // namespace vtextension::wasm::host
