// SPDX-License-Identifier: Apache-2.0
//
// What the extension host does to itself on Windows: it asks for no fault report. The kill-on-close job
// Contour starts it in ends it with its parent, caps its memory and its processes, and ends it on an
// unhandled exception rather than show a dialog.
#include <array>
#include <cstdint>
#include <expected>
#include <span>

#include <vtextension/wasm/host/HostBootstrap.hpp>

#define WIN32_LEAN_AND_MEAN
#include <Windows.h>
#include <werapi.h>

namespace vtextension::wasm::host
{

namespace
{
    /// Asks for no fault report, and so for no dialog that would hold the process on a crash.
    [[nodiscard]] std::expected<void, int> refuseFaultReports() noexcept
    {
        auto const result = ::WerSetFlags(WER_FAULT_REPORTING_NO_UI);
        if (FAILED(result))
            return std::unexpected(static_cast<int>(result));
        return {};
    }

    /// The steps this system's hardening takes.
    constexpr auto HardeningSteps = std::array { HardeningRow { .step = HardeningStep::FaultReports,
                                                                .text = "fault reports cannot be turned off",
                                                                .apply = &refuseFaultReports } };
} // namespace

std::expected<void, BootstrapError> bindToParent([[maybe_unused]] int64_t parentProcessId) noexcept
{
    return {};
}

std::span<HardeningRow const> hardeningSteps() noexcept
{
    return HardeningSteps;
}

remote::proto::LockdownState capResources([[maybe_unused]] uint64_t dataBytes,
                                          [[maybe_unused]] CapPolicy policy) noexcept
{
    return remote::proto::LockdownState::Unsandboxed;
}

} // namespace vtextension::wasm::host
