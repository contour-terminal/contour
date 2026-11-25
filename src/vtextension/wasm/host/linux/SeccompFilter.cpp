// SPDX-License-Identifier: Apache-2.0
#include <vtextension/wasm/host/linux/SeccompFilter.hpp>

#include <sys/syscall.h>

#include <array>
#include <cerrno>
#include <cstddef>
#include <limits>
#include <optional>
#include <ranges>
#include <type_traits>
#include <utility>

#include <unistd.h>

#include <linux/bpf_common.h>
#include <linux/seccomp.h>

namespace vtextension::wasm::host
{

namespace
{
    /// The bit x86_64 sets in the number of an x32 call, which must not reach the rows.
    constexpr auto X32SystemCallBit = uint32_t { 0x4000'0000 };

    /// The most errno a seccomp return carries.
    constexpr auto MaxErrno = 4095;

    /// The most a conditional jump skips.
    constexpr auto MaxJump = std::size_t { std::numeric_limits<uint8_t>::max() };

    /// Where `seccomp_data`'s fields are, for an absolute load.
    constexpr auto NumberOffset = uint32_t { offsetof(seccomp_data, nr) };
    constexpr auto ArchOffset = uint32_t { offsetof(seccomp_data, arch) };
    constexpr auto ArgumentsOffset = uint32_t { offsetof(seccomp_data, args) };

    /// The words a 64-bit argument is loaded as: little-endian on both architectures.
    enum class Half : uint8_t
    {
        Low,
        High,
    };

    /// @return The offset of @p half of argument @p index.
    constexpr uint32_t offsetOf(uint8_t index, Half half) noexcept
    {
        return ArgumentsOffset + (uint32_t { index } * sizeof(uint64_t))
               + (half == Half::High ? sizeof(uint32_t) : 0);
    }

    /// @return The 32 bits of @p value that @p half names.
    constexpr uint32_t halfOf(uint64_t value, Half half) noexcept
    {
        return static_cast<uint32_t>(half == Half::High ? value >> 32U : value);
    }

    constexpr sock_filter statement(uint16_t code, uint32_t operand) noexcept
    {
        return sock_filter { .code = code, .jt = 0, .jf = 0, .k = operand };
    }

    constexpr sock_filter jump(uint16_t code, uint32_t operand, uint8_t whenTrue, uint8_t whenFalse) noexcept
    {
        return sock_filter { .code = code, .jt = whenTrue, .jf = whenFalse, .k = operand };
    }

    constexpr auto LoadWord = uint16_t { BPF_LD | BPF_W | BPF_ABS };
    constexpr auto AndConstant = uint16_t { BPF_ALU | BPF_AND | BPF_K };
    constexpr auto JumpIfEqual = uint16_t { BPF_JMP | BPF_JEQ | BPF_K };
    constexpr auto JumpIfAtLeast = uint16_t { BPF_JMP | BPF_JGE | BPF_K };
    constexpr auto Return = uint16_t { BPF_RET | BPF_K };

    /// When a comparison passes.
    enum class Sense : uint8_t
    {
        Equal,     ///< When the masked argument equals the value.
        Different, ///< When it differs from it.
    };

    /// A check, as the program tests it: its masked argument against a value.
    struct Comparison
    {
        uint8_t index;
        uint64_t mask;
        uint64_t value;
        Sense sense;
    };

    /// The branches of a conditional jump.
    enum class Branch : uint8_t
    {
        WhenFalse,
        WhenTrue,
    };

    /// @return @p check as a comparison, the descriptors and process filled in.
    [[nodiscard]] Comparison comparisonOf(ArgumentCheck const& check,
                                          SandboxDescriptors descriptors,
                                          int ownProcess) noexcept
    {
        auto const fixed = [&](uint64_t value) {
            return Comparison {
                .index = check.index, .mask = check.mask, .value = value, .sense = Sense::Equal
            };
        };
        switch (check.test)
        {
            case ArgumentTest::Equals: return fixed(check.value & check.mask);
            case ArgumentTest::NotEquals:
                return Comparison { .index = check.index,
                                    .mask = check.mask,
                                    .value = check.value & check.mask,
                                    .sense = Sense::Different };
            case ArgumentTest::AllClear: return fixed(0);
            case ArgumentTest::AllSet: return fixed(check.mask);
            case ArgumentTest::EqualsControl:
                return fixed(static_cast<uint64_t>(static_cast<uint32_t>(descriptors.control)) & check.mask);
            case ArgumentTest::EqualsOwnProcess:
                return fixed(static_cast<uint64_t>(static_cast<uint32_t>(ownProcess)) & check.mask);
        }
        return fixed(check.value);
    }

    /// A row's body as it is assembled: its instructions, and the jumps to patch once their targets are
    /// known.
    class RowBody
    {
      public:
        /// Appends the test of @p comparison; failing it jumps to the row's failure return.
        void compare(Comparison const& comparison)
        {
            auto halves = std::vector<Half> {};
            for (auto const half: { Half::Low, Half::High })
                if (halfOf(comparison.mask, half) != 0)
                    halves.push_back(half);
            auto passes = std::vector<std::size_t> {};
            for (auto const position: std::views::iota(std::size_t { 0 }, halves.size()))
            {
                auto const half = halves[position];
                auto const isLast = position + 1 == halves.size();
                _program.push_back(statement(LoadWord, offsetOf(comparison.index, half)));
                if (halfOf(comparison.mask, half) != std::numeric_limits<uint32_t>::max())
                    _program.push_back(statement(AndConstant, halfOf(comparison.mask, half)));
                _program.push_back(jump(JumpIfEqual, halfOf(comparison.value, half), 0, 0));
                auto const at = _program.size() - 1;
                if (comparison.sense == Sense::Equal)
                    _fails.push_back(Patch { .at = at, .branch = Branch::WhenFalse });
                else if (isLast)
                    _fails.push_back(Patch { .at = at, .branch = Branch::WhenTrue });
                else
                    passes.push_back(at); // this half differs: the whole differs
            }
            for (auto const at: passes)
                _program.at(at).jf = static_cast<uint8_t>(_program.size() - (at + 1));
        }

        /// Ends the body with @p decision, and the return its failed checks jump to.
        /// @param decision What the row decides when its checks pass.
        /// @param failed What it decides when one fails: the filter's unlisted action.
        /// @return The body; nothing if a jump would exceed what an instruction reaches.
        [[nodiscard]] std::optional<std::vector<sock_filter>> finish(uint32_t decision, uint32_t failed) &&
        {
            _program.push_back(statement(Return, decision));
            if (_fails.empty())
                return std::move(_program);
            _program.push_back(statement(Return, failed));
            auto const fail = _program.size() - 1;
            for (auto const& patch: _fails)
            {
                auto const distance = fail - (patch.at + 1);
                if (distance > MaxJump)
                    return std::nullopt;
                (patch.branch == Branch::WhenTrue ? _program.at(patch.at).jt : _program.at(patch.at).jf) =
                    static_cast<uint8_t>(distance);
            }
            return std::move(_program);
        }

      private:
        struct Patch
        {
            std::size_t at;
            Branch branch; ///< Which branch of the jump at `at` goes to the failure return.
        };
        std::vector<sock_filter> _program;
        std::vector<Patch> _fails;
    };

    [[nodiscard]] ConfinementFailure assemblyFailure(int error) noexcept
    {
        return ConfinementFailure { .step = remote::proto::LockdownStep::SystemCalls, .code = error };
    }
} // namespace

uint32_t seccompReturnOf(SystemCallAction action, int error) noexcept
{
    switch (action)
    {
        case SystemCallAction::Kill: return SECCOMP_RET_KILL_PROCESS;
        case SystemCallAction::Refuse:
            return SECCOMP_RET_ERRNO | (static_cast<uint32_t>(error) & SECCOMP_RET_DATA);
        case SystemCallAction::Allow: return SECCOMP_RET_ALLOW;
    }
    return SECCOMP_RET_KILL_PROCESS;
}

std::expected<std::vector<sock_filter>, ConfinementFailure> assembleFilter(
    std::span<SystemCallRow const> rows, SandboxDescriptors descriptors, int ownProcess, AuditArch arch)
{
    return assembleFilter(rows, descriptors, ownProcess, arch, SystemCallAction::Kill);
}

std::expected<std::vector<sock_filter>, ConfinementFailure> assembleFilter(
    std::span<SystemCallRow const> rows,
    SandboxDescriptors descriptors,
    int ownProcess,
    AuditArch arch,
    SystemCallAction unlisted)
{
    auto program = std::vector<sock_filter> {
        statement(LoadWord, ArchOffset),
        jump(JumpIfEqual, std::to_underlying(arch), 1, 0),
        statement(Return, SECCOMP_RET_KILL_PROCESS),
        statement(LoadWord, NumberOffset),
    };
    if (arch == AuditArch::Amd64)
    {
        program.push_back(jump(JumpIfAtLeast, X32SystemCallBit, 0, 1));
        program.push_back(statement(Return, SECCOMP_RET_KILL_PROCESS));
    }

    for (auto const& row: rows)
    {
        if (row.action == SystemCallAction::Refuse && (row.error <= 0 || row.error > MaxErrno))
            return std::unexpected(assemblyFailure(EINVAL));
        auto body = RowBody {};
        for (auto const& check: row.checks)
        {
            if (check.index >= std::extent_v<decltype(seccomp_data::args)>)
                return std::unexpected(assemblyFailure(EINVAL));
            body.compare(comparisonOf(check, descriptors, ownProcess));
        }
        auto const assembled =
            std::move(body).finish(seccompReturnOf(row.action, row.error), seccompReturnOf(unlisted, 0));
        if (!assembled || assembled->size() > MaxJump)
            return std::unexpected(assemblyFailure(E2BIG));
        // A row whose number differs is skipped whole; the accumulator still holds the number then, as only a
        // matching row loads its arguments, and every row that does ends in a return.
        program.push_back(
            jump(JumpIfEqual, static_cast<uint32_t>(row.number), 0, static_cast<uint8_t>(assembled->size())));
        program.insert(program.end(), assembled->begin(), assembled->end());
    }
    program.push_back(statement(Return, seccompReturnOf(unlisted, 0)));
    if (program.size() > BPF_MAXINSNS)
        return std::unexpected(assemblyFailure(E2BIG));
    return program;
}

std::expected<void, ConfinementFailure> installFilter(std::span<sock_filter const> program)
{
    // The kernel copies the program, through a pointer it declares mutable.
    auto copy = std::vector<sock_filter>(program.begin(), program.end());
    auto const filter = sock_fprog { .len = static_cast<unsigned short>(copy.size()), .filter = copy.data() };
    auto const result = ::syscall(SYS_seccomp, SECCOMP_SET_MODE_FILTER, SECCOMP_FILTER_FLAG_TSYNC, &filter);
    if (result == 0)
        return {};
    // A positive result names the thread that could not be synchronized: it runs a filter of its own.
    if (result > 0)
        return std::unexpected(
            ConfinementFailure { .step = remote::proto::LockdownStep::ThreadSync, .code = EPERM });
    return std::unexpected(
        ConfinementFailure { .step = remote::proto::LockdownStep::SystemCalls, .code = errno });
}

} // namespace vtextension::wasm::host
