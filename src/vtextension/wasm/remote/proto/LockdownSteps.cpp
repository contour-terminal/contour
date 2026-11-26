// SPDX-License-Identifier: Apache-2.0
#include <vtextension/wasm/remote/proto/LockdownSteps.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <format>
#include <ranges>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>

#include <vtextension/ExtensionError.hpp>

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
        LockdownStepRow { .step = LockdownStep::Job, .text = "the process does not run in a job of its own" },
        LockdownStepRow { .step = LockdownStep::Priming,
                          .text = "a thread to run extensions on cannot be prepared",
                          .code = FailureCodeKind::Extension },
        LockdownStepRow { .step = LockdownStep::WriteXorExecute,
                          .text =
                              "the process cannot be kept from mapping memory both writable and executable" },
    };

    /// What an extension runtime's error a step states says, in users' words naming no engine. A step's
    /// ExtensionError not here is left unphrased.
    struct ExtensionCodeRow
    {
        ExtensionError error;  ///< The error.
        std::string_view text; ///< What it says.
    };

    /// The errors preparing a thread may state.
    constexpr auto ExtensionCodeTexts = std::array {
        ExtensionCodeRow { .error = ExtensionError::RuntimeUnavailable,
                           .text = "the extension runtime is unavailable" },
        ExtensionCodeRow { .error = ExtensionError::OutOfMemory, .text = "out of memory" },
    };

    static_assert(std::ranges::all_of(std::views::iota(std::size_t { 0 }, LockdownStepTable.size()),
                                      [](std::size_t index) {
                                          return std::to_underlying(LockdownStepTable[index].step) == index;
                                      }),
                  "LockdownStepTable lists every LockdownStep at its own index");
    static_assert(LockdownStepTable.back().step == LockdownStep::WriteXorExecute,
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

std::string systemErrorText(int code)
{
#ifdef _WIN32
    // The system's message names an HRESULT it knows; the value itself names every one.
    return std::format(
        "{}, HRESULT 0x{:08X}", std::system_category().message(code), static_cast<uint32_t>(code));
#else
    return std::system_category().message(code);
#endif
}

std::string failureCodeText(LockdownFailure failure)
{
    auto const index = static_cast<std::size_t>(std::to_underlying(failure.step));
    auto const kind =
        index < LockdownStepTable.size() ? LockdownStepTable[index].code : FailureCodeKind::System;
    switch (kind)
    {
        case FailureCodeKind::System:
            // An HRESULT travels as its bit pattern: the int it was.
            return failure.code == 0 ? std::string {} : systemErrorText(static_cast<int>(failure.code));
        case FailureCodeKind::Extension:
            for (auto const& row: ExtensionCodeTexts)
                if (std::to_underlying(row.error) == failure.code)
                    return std::string { row.text };
            return {};
    }
    return {};
}

std::string lockdownFailureText(LockdownFailure failure)
{
    auto const code = failureCodeText(failure);
    if (code.empty())
        return std::string { lockdownStepText(failure.step) };
    return std::format("{} ({})", lockdownStepText(failure.step), code);
}

} // namespace vtextension::wasm::remote::proto
