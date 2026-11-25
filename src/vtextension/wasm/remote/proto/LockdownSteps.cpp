// SPDX-License-Identifier: Apache-2.0
#include <vtextension/wasm/remote/proto/LockdownSteps.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <ranges>
#include <span>
#include <string_view>
#include <utility>

namespace vtextension::wasm::remote::proto
{

namespace
{
    /// Every step, at its enumerator's index.
    constexpr auto LockdownStepTable = std::array {
        LockdownStepRow { .step = LockdownStep::None, .text = "no step of its confinement failed" },
        LockdownStepRow { .step = LockdownStep::CoreDumps, .text = "core dumps cannot be turned off" },
        LockdownStepRow { .step = LockdownStep::Dumpable,
                          .text = "the process cannot be kept from being dumped or attached to" },
        LockdownStepRow { .step = LockdownStep::Tracing,
                          .text = "the process cannot be kept from being traced" },
        LockdownStepRow { .step = LockdownStep::FaultReports, .text = "fault reports cannot be turned off" },
        LockdownStepRow { .step = LockdownStep::NoNewPrivileges,
                          .text = "the process cannot be kept from gaining privileges" },
        LockdownStepRow { .step = LockdownStep::FileSystem,
                          .text = "its access to the file system cannot be taken away" },
        LockdownStepRow { .step = LockdownStep::SystemCalls,
                          .text = "the system calls it may make cannot be restricted" },
        LockdownStepRow { .step = LockdownStep::ThreadSync,
                          .text = "the restriction of its system calls cannot reach every thread" },
        LockdownStepRow { .step = LockdownStep::ResourceCaps, .text = "its resources cannot be capped" },
        LockdownStepRow { .step = LockdownStep::Capabilities,
                          .text = "the process cannot enter capability mode" },
        LockdownStepRow { .step = LockdownStep::DescriptorRights,
                          .text = "the rights of its channels cannot be limited" },
        LockdownStepRow { .step = LockdownStep::Profile, .text = "its sandbox profile cannot be applied" },
        LockdownStepRow { .step = LockdownStep::Container,
                          .text = "the process cannot be started in a container of its own" },
        LockdownStepRow { .step = LockdownStep::Mitigations,
                          .text = "its process mitigations cannot be turned on" },
    };

    static_assert(std::ranges::all_of(std::views::iota(std::size_t { 0 }, LockdownStepTable.size()),
                                      [](std::size_t index) {
                                          return std::to_underlying(LockdownStepTable[index].step) == index;
                                      }),
                  "LockdownStepTable lists every LockdownStep at its own index");
    static_assert(LockdownStepTable.back().step == LockdownStep::Mitigations,
                  "LockdownStepTable ends at the last LockdownStep");
} // namespace

std::span<LockdownStepRow const> lockdownSteps() noexcept
{
    return LockdownStepTable;
}

std::string_view lockdownStepText(LockdownStep step) noexcept
{
    auto const index = static_cast<std::size_t>(std::to_underlying(step));
    if (index >= LockdownStepTable.size())
        return LockdownStepTable.front().text;
    return LockdownStepTable[index].text;
}

} // namespace vtextension::wasm::remote::proto
