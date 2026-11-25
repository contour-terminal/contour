// SPDX-License-Identifier: Apache-2.0
//
// The sandbox of a system Contour has none for: the host runs unsandboxed, and its Hello says so.
#include <memory>

#include <vtextension/wasm/host/Sandbox.hpp>

namespace vtextension::wasm::host
{

std::unique_ptr<ISandbox> makeSystemSandbox([[maybe_unused]] SandboxDescriptors descriptors,
                                            [[maybe_unused]] GuestCode code)
{
    return std::make_unique<NullSandbox>();
}

} // namespace vtextension::wasm::host
