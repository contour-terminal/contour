// SPDX-License-Identifier: Apache-2.0
#include <vtextension/wasm/host/MemoryProvision.hpp>

#include <optional>
#include <utility>

namespace vtextension::wasm::host
{

namespace
{
    /// The provision the calling thread's innermost scope offers; nothing outside one, or once taken.
    thread_local auto offered = std::optional<MemoryProvision> {};
} // namespace

ProvisionScope::ProvisionScope(MemoryProvision provision):
    _outer { std::exchange(offered, std::move(provision)) }
{
}

ProvisionScope::~ProvisionScope()
{
    offered = std::move(_outer);
}

std::optional<MemoryProvision> takeProvision() noexcept
{
    return std::exchange(offered, std::nullopt);
}

} // namespace vtextension::wasm::host
