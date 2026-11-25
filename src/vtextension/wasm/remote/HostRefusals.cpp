// SPDX-License-Identifier: Apache-2.0
#include <vtextension/wasm/remote/HostRefusals.hpp>

#include <core/log/LogStore.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <ranges>
#include <span>
#include <string_view>
#include <utility>

namespace vtextension::wasm::remote
{

namespace
{
    constexpr auto RefusalTable = std::array {
        HostRefusalRow { .refusal = HostRefusal::HelperNotFound,
                         .text = "The extension host cannot be started: it is missing from Contour's "
                                 "installation, or cannot be executed. Extensions do not run." },
        HostRefusalRow { .refusal = HostRefusal::OutOfResources,
                         .text = "The extension host cannot be started: the system is out of memory, "
                                 "processes or handles. Extensions do not run." },
        HostRefusalRow { .refusal = HostRefusal::Unsupported,
                         .text = "The extension host cannot be started: the system lacks what it takes (on "
                                 "Linux, kernel 5.4 or later). Extensions do not run." },
        HostRefusalRow { .refusal = HostRefusal::LaunchRefused,
                         .text = "The extension host cannot be started: the system refused. Extensions do "
                                 "not run." },
        HostRefusalRow { .refusal = HostRefusal::ProtocolVersion,
                         .text = "The extension host speaks another protocol version than Contour, and was "
                                 "refused: it is not the one installed with this Contour. Extensions do not "
                                 "run." },
        HostRefusalRow { .refusal = HostRefusal::LockdownFailed,
                         .text =
                             "The extension host could not confine itself, and was refused. Extensions do "
                             "not run." },
        HostRefusalRow { .refusal = HostRefusal::Unsandboxed,
                         .text = "The extension host runs without a sandbox on this system, which Contour "
                                 "requires, and was refused. Extensions do not run." },
        HostRefusalRow { .refusal = HostRefusal::MemoryMode,
                         .text = "The extension host shares memory where Contour copies it, and was refused: "
                                 "it is not the one installed with this Contour. Extensions do not run." },
    };
    static_assert(std::ranges::all_of(std::views::iota(std::size_t { 0 }, RefusalTable.size()),
                                      [](std::size_t index) {
                                          return std::to_underlying(RefusalTable[index].refusal) == index;
                                      }));
    static_assert(RefusalTable.size() <= 32, "a bit per reason in HostRefusalLog's word");
} // namespace

std::span<HostRefusalRow const> hostRefusals() noexcept
{
    return RefusalTable;
}

HostRefusalLog::HostRefusalLog(Writer writer): _writer { std::move(writer) }
{
}

void HostRefusalLog::refused(HostRefusal refusal)
{
    auto const bit = uint32_t { 1 } << std::to_underlying(refusal);
    if ((_logged.fetch_or(bit) & bit) != 0)
        return;
    _writer(RefusalTable[std::to_underlying(refusal)].text);
}

HostRefusalLog::Writer errorLogWriter()
{
    return [](std::string_view line) {
        errorLog()("{}", line);
    };
}

} // namespace vtextension::wasm::remote
