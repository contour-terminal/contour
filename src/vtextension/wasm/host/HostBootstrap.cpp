// SPDX-License-Identifier: Apache-2.0
//
// What every system's hardening shares: the steps are its own (HostBootstrap_posix.cpp,
// HostBootstrap_win32.cpp), taking them is not.
#include <vtextension/wasm/host/HostBootstrap.hpp>

#include <expected>
#include <format>
#include <optional>
#include <span>
#include <string>

namespace vtextension::wasm::host
{

std::expected<void, HardeningFailure> hardenProcess(std::span<HardeningRow const> steps) noexcept
{
    auto failed = std::optional<HardeningFailure> {};
    for (auto const& row: steps)
        if (auto const applied = row.apply(); !applied && !failed)
            failed = HardeningFailure { .step = row.step, .text = row.text, .code = applied.error() };
    if (failed)
        return std::unexpected(*failed);
    return {};
}

std::expected<void, HardeningFailure> hardenProcess() noexcept
{
    return hardenProcess(hardeningSteps());
}

std::string hardeningFailureText(HardeningFailure const& failure)
{
    return std::format("{} ({})", failure.text, systemErrorText(failure.code));
}

} // namespace vtextension::wasm::host
