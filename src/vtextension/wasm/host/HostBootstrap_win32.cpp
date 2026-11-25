// SPDX-License-Identifier: Apache-2.0
//
// What the extension host does to itself on Windows: nothing. The kill-on-close job Contour starts it in ends
// it with its parent, and caps its memory and its processes.
#include <cstdint>
#include <expected>

#include <vtextension/wasm/host/HostBootstrap.hpp>

namespace vtextension::wasm::host
{

std::expected<void, BootstrapError> bindToParent([[maybe_unused]] int64_t parentProcessId) noexcept
{
    return {};
}

remote::proto::LockdownState capResources([[maybe_unused]] uint64_t dataBytes,
                                          [[maybe_unused]] CapPolicy policy) noexcept
{
    return remote::proto::LockdownState::Unsandboxed;
}

} // namespace vtextension::wasm::host
