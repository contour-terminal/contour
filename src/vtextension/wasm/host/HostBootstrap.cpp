// SPDX-License-Identifier: Apache-2.0
//
// What every system's hardening shares: the steps are its own (HostBootstrap_posix.cpp,
// HostBootstrap_win32.cpp), taking them is not.
#include <vtextension/wasm/host/HostBootstrap.hpp>

#include <cstdint>
#include <expected>
#include <optional>
#include <span>

#include <vtextension/wasm/host/HostServer.hpp>
#include <vtextension/wasm/host/InterruptListener.hpp>
#include <vtextension/wasm/remote/transport/HelperArguments.hpp>

namespace vtextension::wasm::host
{

namespace
{
    /// The host's program text and libraries, as measured for the WAMR host on Linux.
    constexpr auto ProgramImageBytes = uint64_t { 16 } * 1024 * 1024;

    /// What a host maps beside its guests' memory and its heap: its program and every thread's stack.
    constexpr auto FixedMappingBytes = ProgramImageBytes
                                       + (uint64_t { remote::MaxLaneChannels } * ExecutorStackBytes)
                                       + uint64_t { InterruptListenerStackBytes };
} // namespace

// The heap gets at least three times what the program and the stacks take: more lanes, or larger executor
// stacks, must grow the headroom too rather than silently leaving the heap less of it.
static_assert(FixedMappingBytes * 4 <= AddressSpaceHeadroomBytes,
              "AddressSpaceHeadroomBytes leaves the heap less than three times the program and its stacks");

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
