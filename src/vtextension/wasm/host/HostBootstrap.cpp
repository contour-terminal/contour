// SPDX-License-Identifier: Apache-2.0
//
// What every system's hardening shares: the steps are its own (HostBootstrap_posix.cpp,
// HostBootstrap_win32.cpp), taking them is not.
#include <vtextension/wasm/host/HostBootstrap.hpp>

#include <algorithm>
#include <expected>
#include <optional>
#include <span>
#include <string_view>

namespace vtextension::wasm::host
{

std::expected<void, HardeningStep> hardenProcess(std::span<HardeningRow const> steps) noexcept
{
    auto failed = std::optional<HardeningStep> {};
    for (auto const& row: steps)
        if (!row.apply() && !failed)
            failed = row.step;
    if (failed)
        return std::unexpected(*failed);
    return {};
}

std::expected<void, HardeningStep> hardenProcess() noexcept
{
    return hardenProcess(hardeningSteps());
}

std::string_view hardeningStepText(HardeningStep step) noexcept
{
    auto const steps = hardeningSteps();
    auto const row = std::ranges::find(steps, step, &HardeningRow::step);
    return row != steps.end() ? row->text : std::string_view {};
}

} // namespace vtextension::wasm::host
