// SPDX-License-Identifier: Apache-2.0
#include <vtextension/wasm/remote/HostRefusals.hpp>

#include <core/log/LogStore.hpp>

#include <algorithm>
#include <array>
#include <cstdint>
#include <format>
#include <ranges>
#include <string>
#include <string_view>
#include <utility>

#include <vtextension/wasm/remote/process/ProcessLauncher.hpp>

namespace vtextension::wasm::remote
{

namespace
{
    /// The line for a host that could not be started, around what the launcher says of why.
    constexpr auto LaunchLine =
        std::string_view { "The extension host cannot be started: {}. Extensions do not run." };

    /// A reason a host was refused at the handshake, and the line logged for it.
    struct HandshakeRefusalRow
    {
        HostRefusal refusal;   ///< The reason.
        std::string_view text; ///< The line logged.
    };

    /// The reasons a host that started was refused for. Those it never started for are the launcher's.
    constexpr auto HandshakeRefusals = std::array {
        HandshakeRefusalRow { .refusal = HostRefusal::ProtocolVersion,
                              .text =
                                  "The extension host speaks another protocol version than Contour, and was "
                                  "refused: it is not the one installed with this Contour. Extensions do not "
                                  "run." },
        HandshakeRefusalRow { .refusal = HostRefusal::LockdownFailed,
                              .text =
                                  "The extension host could not confine itself, and was refused. Extensions "
                                  "do not run." },
        HandshakeRefusalRow { .refusal = HostRefusal::Unsandboxed,
                              .text =
                                  "The extension host runs without a sandbox on this system, which Contour "
                                  "requires, and was refused. Extensions do not run." },
        HandshakeRefusalRow {
            .refusal = HostRefusal::MemoryMode,
            .text = "The extension host shares memory where Contour copies it, and was refused: "
                    "it is not the one installed with this Contour. Extensions do not run." },
    };
    static_assert(std::to_underlying(LastHostRefusal) < 32, "a bit per reason in HostRefusalLog's word");

    /// The line for a host that could not be started for a reason the launcher's table does not name.
    constexpr auto UnnamedLaunchLine =
        std::string_view { "The extension host cannot be started. Extensions do not run." };

    /// @return Whether every reason the handshake refuses for has its row.
    [[nodiscard]] consteval bool everyHandshakeRefusalHasARow() noexcept
    {
        for (auto const value: std::views::iota(0, std::to_underlying(LastHostRefusal) + 1))
        {
            auto const refusal = static_cast<HostRefusal>(value);
            if (refusalSource(refusal) == RefusalSource::Handshake
                && std::ranges::find(HandshakeRefusals, refusal, &HandshakeRefusalRow::refusal)
                       == HandshakeRefusals.end())
                return false;
        }
        return true;
    }
    static_assert(everyHandshakeRefusalHasARow());
} // namespace

std::string refusalLine(HostRefusal refusal)
{
    switch (refusalSource(refusal))
    {
        case RefusalSource::Launch:
            for (auto const& row: process::launchErrors())
                if (row.refusal == refusal)
                    return std::format(LaunchLine, row.text);
            return std::string { UnnamedLaunchLine };
        case RefusalSource::Handshake:
            // Every such reason has its row: checked at compile time.
            return std::string {
                std::ranges::find(HandshakeRefusals, refusal, &HandshakeRefusalRow::refusal)->text
            };
        case RefusalSource::None: break;
    }
    return std::string { UnnamedLaunchLine };
}

HostRefusalLog::HostRefusalLog(Writer writer): _writer { std::move(writer) }
{
}

void HostRefusalLog::refused(HostRefusal refusal)
{
    auto const bit = uint32_t { 1 } << std::to_underlying(refusal);
    if ((_logged.fetch_or(bit) & bit) != 0)
        return;
    _writer(refusalLine(refusal));
}

HostRefusalLog::Writer errorLogWriter()
{
    return [](std::string_view line) {
        errorLog()("{}", line);
    };
}

} // namespace vtextension::wasm::remote
