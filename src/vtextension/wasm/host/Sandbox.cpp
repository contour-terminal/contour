// SPDX-License-Identifier: Apache-2.0
//
// What every system's sandbox shares: how a step that failed is phrased.
#include <vtextension/wasm/host/Sandbox.hpp>

#include <format>
#include <string>

#include <vtextension/wasm/host/HostBootstrap.hpp>
#include <vtextension/wasm/remote/proto/LockdownSteps.hpp>

namespace vtextension::wasm::host
{

std::string confinementFailureText(ConfinementFailure const& failure)
{
    return std::format(
        "{}: {}", remote::proto::lockdownStepText(failure.step), systemErrorText(failure.code));
}

} // namespace vtextension::wasm::host
