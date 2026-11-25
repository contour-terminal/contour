// SPDX-License-Identifier: Apache-2.0
//
// What the extension host does to itself on Windows: it asks for no fault report. The kill-on-close job
// Contour starts it in ends it with its parent, caps its memory and its processes, and ends it on an
// unhandled exception rather than show a dialog.
#include <array>
#include <cstdint>
#include <expected>
#include <format>
#include <span>
#include <string>
#include <system_error>

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
    constexpr auto HardeningSteps = std::array { HardeningRow {
        .step = remote::proto::LockdownStep::FaultReports, .apply = &refuseFaultReports } };
} // namespace

std::expected<void, BootstrapError> bindToParent([[maybe_unused]] int64_t parentProcessId) noexcept
{
    return {};
}

std::span<HardeningRow const> hardeningSteps() noexcept
{
    return HardeningSteps;
}

std::string systemErrorText(int code)
{
    // The system's message names an HRESULT it knows; the value itself names every one.
    return std::format(
        "{}, HRESULT 0x{:08X}", std::system_category().message(code), static_cast<uint32_t>(code));
}

std::expected<void, ConfinementFailure> capResources([[maybe_unused]] uint64_t dataBytes,
                                                     [[maybe_unused]] CapPolicy policy) noexcept
{
    return {};
}

} // namespace vtextension::wasm::host
