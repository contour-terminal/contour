// SPDX-License-Identifier: Apache-2.0
#pragma once

/// @file
/// Test-only: runs a classic BPF program over a `seccomp_data`, as the kernel runs a seccomp filter, so that
/// what a filter decides for a call is checked without installing it -- and without the process it would
/// kill. Knows the instructions SeccompFilter.hpp assembles, and the kernel's other conditional jumps.

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <optional>
#include <span>

#include <linux/filter.h>
#include <linux/seccomp.h>
#include <vtextension/wasm/host/linux/SystemCallRows.hpp>

namespace vtextension::wasm::host::testing
{

/// A system call as a filter sees it.
struct SystemCallUse
{
    AuditArch arch = AuditArch::Amd64;      ///< The architecture it was made on.
    long number = 0;                        ///< Its number there.
    std::array<uint64_t, 6> arguments = {}; ///< Its arguments.
};

/// @return What @p program returns for @p use; nothing if it runs off its end or meets an instruction this
///         evaluator does not know.
[[nodiscard]] inline std::optional<uint32_t> evaluateFilter(std::span<sock_filter const> program,
                                                            SystemCallUse const& use)
{
    auto data = seccomp_data {};
    data.nr = static_cast<int>(use.number);
    data.arch = static_cast<uint32_t>(use.arch);
    std::ranges::copy(use.arguments, std::begin(data.args));
    auto bytes = std::array<std::byte, sizeof(seccomp_data)> {};
    std::memcpy(bytes.data(), &data, sizeof(data));

    auto accumulator = uint32_t { 0 };
    auto counter = std::size_t { 0 };
    while (counter < program.size())
    {
        auto const& instruction = program[counter];
        counter += 1;
        auto const branch = [&](bool taken) {
            counter += taken ? instruction.jt : instruction.jf;
        };
        switch (instruction.code)
        {
            case BPF_LD | BPF_W | BPF_ABS:
                if (instruction.k + sizeof(uint32_t) > bytes.size())
                    return std::nullopt;
                std::memcpy(&accumulator, bytes.data() + instruction.k, sizeof(uint32_t));
                break;
            case BPF_ALU | BPF_AND | BPF_K: accumulator &= instruction.k; break;
            case BPF_JMP | BPF_JA: counter += instruction.k; break;
            case BPF_JMP | BPF_JEQ | BPF_K: branch(accumulator == instruction.k); break;
            case BPF_JMP | BPF_JGE | BPF_K: branch(accumulator >= instruction.k); break;
            case BPF_JMP | BPF_JGT | BPF_K: branch(accumulator > instruction.k); break;
            case BPF_JMP | BPF_JSET | BPF_K: branch((accumulator & instruction.k) != 0); break;
            case BPF_RET | BPF_K: return instruction.k;
            default: return std::nullopt;
        }
    }
    return std::nullopt;
}

/// What a filter's return value decides.
struct FilterVerdict
{
    SystemCallAction action; ///< What becomes of the call.
    int error = 0;           ///< The errno of a `Refuse`.

    bool operator==(FilterVerdict const&) const = default;
};

/// @return What @p value, a seccomp return value, decides; `Kill` for anything but an errno or an allow.
[[nodiscard]] inline FilterVerdict verdictOf(uint32_t value) noexcept
{
    if (value == SECCOMP_RET_ALLOW)
        return FilterVerdict { .action = SystemCallAction::Allow };
    if ((value & SECCOMP_RET_ACTION_FULL) == SECCOMP_RET_ERRNO)
        return FilterVerdict { .action = SystemCallAction::Refuse,
                               .error = static_cast<int>(value & SECCOMP_RET_DATA) };
    return FilterVerdict { .action = SystemCallAction::Kill };
}

} // namespace vtextension::wasm::host::testing
