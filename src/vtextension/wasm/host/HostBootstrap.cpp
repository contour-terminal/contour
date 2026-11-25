// SPDX-License-Identifier: Apache-2.0
//
// What every system's hardening shares: the steps are its own (HostBootstrap_posix.cpp,
// HostBootstrap_win32.cpp), taking them is not.
#include <vtextension/wasm/host/HostBootstrap.hpp>

#include <expected>
#include <optional>
#include <span>

namespace vtextension::wasm::host
{

std::expected<void, ConfinementFailure> hardenProcess(std::span<HardeningRow const> steps) noexcept
{
    auto failed = std::optional<ConfinementFailure> {};
    for (auto const& row: steps)
        if (auto const applied = row.apply(); !applied && !failed)
            failed = ConfinementFailure { .step = row.step, .code = applied.error() };
    if (failed)
        return std::unexpected(*failed);
    return {};
}

std::expected<void, ConfinementFailure> hardenProcess() noexcept
{
    return hardenProcess(hardeningSteps());
}

} // namespace vtextension::wasm::host
