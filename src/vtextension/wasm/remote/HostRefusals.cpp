// SPDX-License-Identifier: Apache-2.0
#include <vtextension/wasm/remote/HostRefusals.hpp>

#include <core/log/LogStore.hpp>

#include <algorithm>
#include <array>
#include <cstdint>
#include <format>
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
    static_assert(std::to_underlying(HostRefusal::MemoryMode) < 32,
                  "a bit per reason in HostRefusalLog's word");
} // namespace

std::string refusalLine(HostRefusal refusal)
{
    for (auto const& row: process::launchErrors())
        if (row.refusal == refusal)
            return std::format(LaunchLine, row.text);
    auto const handshake = std::ranges::find(HandshakeRefusals, refusal, &HandshakeRefusalRow::refusal);
    if (handshake == HandshakeRefusals.end())
        return {};
    return std::string { handshake->text };
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
