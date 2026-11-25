// SPDX-License-Identifier: Apache-2.0
#include <vtextension/wasm/host/linux/SystemCallRows.hpp>

#include <sys/mman.h>

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstddef>
#include <ranges>
#include <utility>

#include <fcntl.h>

#include <vtextension/wasm/BuildInstrumentation.hpp>

namespace vtextension::wasm::host
{

namespace
{
    // The numbers, from the kernel's arch/x86/entry/syscalls/syscall_64.tbl and include/uapi/asm-generic/
    // unistd.h (which aarch64 uses): one row per SystemCall, in its order.
    constexpr auto NumberTable = std::array {
        SystemCallNumbers { SystemCall::Accept, "accept", 43, 202 },
        SystemCallNumbers { SystemCall::Accept4, "accept4", 288, 242 },
        SystemCallNumbers { SystemCall::Bind, "bind", 49, 200 },
        SystemCallNumbers { SystemCall::Bpf, "bpf", 321, 280 },
        SystemCallNumbers { SystemCall::Brk, "brk", 12, 214 },
        SystemCallNumbers { SystemCall::ClockGettime, "clock_gettime", 228, 113 },
        SystemCallNumbers { SystemCall::Clone, "clone", 56, 220 },
        SystemCallNumbers { SystemCall::Clone3, "clone3", 435, 435 },
        SystemCallNumbers { SystemCall::Close, "close", 3, 57 },
        SystemCallNumbers { SystemCall::Connect, "connect", 42, 203 },
        SystemCallNumbers { SystemCall::Execve, "execve", 59, 221 },
        SystemCallNumbers { SystemCall::Execveat, "execveat", 322, 281 },
        SystemCallNumbers { SystemCall::Exit, "exit", 60, 93 },
        SystemCallNumbers { SystemCall::ExitGroup, "exit_group", 231, 94 },
        SystemCallNumbers { SystemCall::Fcntl, "fcntl", 72, 25 },
        SystemCallNumbers { SystemCall::Fork, "fork", 57, NoSystemCall },
        SystemCallNumbers { SystemCall::Fstat, "fstat", 5, 80 },
        SystemCallNumbers { SystemCall::Ftruncate, "ftruncate", 77, 46 },
        SystemCallNumbers { SystemCall::Futex, "futex", 202, 98 },
        SystemCallNumbers { SystemCall::Getpid, "getpid", 39, 172 },
        SystemCallNumbers { SystemCall::Getppid, "getppid", 110, 173 },
        SystemCallNumbers { SystemCall::Getrandom, "getrandom", 318, 278 },
        SystemCallNumbers { SystemCall::Gettid, "gettid", 186, 178 },
        SystemCallNumbers { SystemCall::Gettimeofday, "gettimeofday", 96, 169 },
        SystemCallNumbers { SystemCall::Ioctl, "ioctl", 16, 29 },
        SystemCallNumbers { SystemCall::Kill, "kill", 62, 129 },
        SystemCallNumbers { SystemCall::LandlockAddRule, "landlock_add_rule", 445, 445 },
        SystemCallNumbers { SystemCall::LandlockCreateRuleset, "landlock_create_ruleset", 444, 444 },
        SystemCallNumbers { SystemCall::LandlockRestrictSelf, "landlock_restrict_self", 446, 446 },
        SystemCallNumbers { SystemCall::Listen, "listen", 50, 201 },
        SystemCallNumbers { SystemCall::Lseek, "lseek", 8, 62 },
        SystemCallNumbers { SystemCall::Madvise, "madvise", 28, 233 },
        SystemCallNumbers { SystemCall::Membarrier, "membarrier", 324, 283 },
        SystemCallNumbers { SystemCall::Mmap, "mmap", 9, 222 },
        SystemCallNumbers { SystemCall::Mount, "mount", 165, 40 },
        SystemCallNumbers { SystemCall::Mprotect, "mprotect", 10, 226 },
        SystemCallNumbers { SystemCall::Mremap, "mremap", 25, 216 },
        SystemCallNumbers { SystemCall::Munmap, "munmap", 11, 215 },
        SystemCallNumbers { SystemCall::Nanosleep, "nanosleep", 35, 101 },
        SystemCallNumbers { SystemCall::Newfstatat, "newfstatat", 262, 79 },
        SystemCallNumbers { SystemCall::Open, "open", 2, NoSystemCall },
        SystemCallNumbers { SystemCall::Openat, "openat", 257, 56 },
        SystemCallNumbers { SystemCall::Openat2, "openat2", 437, 437 },
        SystemCallNumbers { SystemCall::PerfEventOpen, "perf_event_open", 298, 241 },
        SystemCallNumbers { SystemCall::Personality, "personality", 135, 92 },
        SystemCallNumbers { SystemCall::Poll, "poll", 7, NoSystemCall },
        SystemCallNumbers { SystemCall::Ppoll, "ppoll", 271, 73 },
        SystemCallNumbers { SystemCall::ProcessVmReadv, "process_vm_readv", 310, 270 },
        SystemCallNumbers { SystemCall::ProcessVmWritev, "process_vm_writev", 311, 271 },
        SystemCallNumbers { SystemCall::Ptrace, "ptrace", 101, 117 },
        SystemCallNumbers { SystemCall::Read, "read", 0, 63 },
        SystemCallNumbers { SystemCall::Recvfrom, "recvfrom", 45, 207 },
        SystemCallNumbers { SystemCall::Recvmsg, "recvmsg", 47, 212 },
        SystemCallNumbers { SystemCall::RestartSyscall, "restart_syscall", 219, 128 },
        SystemCallNumbers { SystemCall::RtSigaction, "rt_sigaction", 13, 134 },
        SystemCallNumbers { SystemCall::RtSigprocmask, "rt_sigprocmask", 14, 135 },
        SystemCallNumbers { SystemCall::RtSigreturn, "rt_sigreturn", 15, 139 },
        SystemCallNumbers { SystemCall::SchedYield, "sched_yield", 24, 124 },
        SystemCallNumbers { SystemCall::Seccomp, "seccomp", 317, 277 },
        SystemCallNumbers { SystemCall::Sendmmsg, "sendmmsg", 307, 269 },
        SystemCallNumbers { SystemCall::Sendmsg, "sendmsg", 46, 211 },
        SystemCallNumbers { SystemCall::Sendto, "sendto", 44, 206 },
        SystemCallNumbers { SystemCall::Setns, "setns", 308, 268 },
        SystemCallNumbers { SystemCall::Setsockopt, "setsockopt", 54, 208 },
        SystemCallNumbers { SystemCall::Shutdown, "shutdown", 48, 210 },
        SystemCallNumbers { SystemCall::Sigaltstack, "sigaltstack", 131, 132 },
        SystemCallNumbers { SystemCall::Socket, "socket", 41, 198 },
        SystemCallNumbers { SystemCall::Socketpair, "socketpair", 53, 199 },
        SystemCallNumbers { SystemCall::Statx, "statx", 332, 291 },
        SystemCallNumbers { SystemCall::Tgkill, "tgkill", 234, 131 },
        SystemCallNumbers { SystemCall::Unshare, "unshare", 272, 97 },
        SystemCallNumbers { SystemCall::Vfork, "vfork", 58, NoSystemCall },
        SystemCallNumbers { SystemCall::Write, "write", 1, 64 },
    };
    static_assert(std::ranges::all_of(std::views::iota(std::size_t { 0 }, NumberTable.size()),
                                      [](std::size_t index) {
                                          return std::to_underlying(NumberTable.at(index).call) == index;
                                      }),
                  "NumberTable holds one row per SystemCall, at its index.");
    static_assert(NumberTable.back().call == SystemCall::Write, "NumberTable names every SystemCall.");

    /// Where a row applies.
    enum class Architectures : uint8_t
    {
        Every,   ///< Wherever the call exists.
        AArch64, ///< On aarch64 only.
    };

    /// A row before it is numbered for an architecture.
    struct RowSpec
    {
        SystemCall call;
        std::span<ArgumentCheck const> checks;
        SystemCallAction action;
        int error;
        std::string_view reason;
        Architectures architectures = Architectures::Every;
    };

    /// @return An unchecked row.
    constexpr RowSpec allow(SystemCall call,
                            std::string_view reason,
                            Architectures where = Architectures::Every)
    {
        return RowSpec { .call = call,
                         .checks = {},
                         .action = SystemCallAction::Allow,
                         .error = 0,
                         .reason = reason,
                         .architectures = where };
    }

    /// @return A row allowing @p call when its arguments pass @p checks.
    constexpr RowSpec allowWhen(SystemCall call,
                                std::span<ArgumentCheck const> checks,
                                std::string_view reason)
    {
        return RowSpec {
            .call = call, .checks = checks, .action = SystemCallAction::Allow, .error = 0, .reason = reason
        };
    }

    /// @return A row failing @p call with @p error.
    constexpr RowSpec refuse(SystemCall call, int error, std::string_view reason)
    {
        return RowSpec {
            .call = call, .checks = {}, .action = SystemCallAction::Refuse, .error = error, .reason = reason
        };
    }

    /// The low half of an argument: what an `int` argument is, whatever the register's upper half holds.
    constexpr auto IntBits = uint64_t { 0xFFFF'FFFF };

    constexpr auto OnControl = std::array {
        ArgumentCheck { .index = 0, .test = ArgumentTest::EqualsControl, .mask = IntBits, .value = 0 },
    };
    constexpr auto OnOwnProcess = std::array {
        ArgumentCheck { .index = 0, .test = ArgumentTest::EqualsOwnProcess, .mask = IntBits, .value = 0 },
    };
    constexpr auto WritableOrExecutable = uint64_t { PROT_WRITE | PROT_EXEC };
    constexpr auto SharedOrFixed = uint64_t { MAP_SHARED | MAP_FIXED };
    /// Never executable; shared only at a fixed address, over a reservation of the host's own.
    constexpr auto MappingRelease = std::array {
        ArgumentCheck { .index = 2, .test = ArgumentTest::AllClear, .mask = PROT_EXEC, .value = 0 },
        ArgumentCheck {
            .index = 3, .test = ArgumentTest::NotEquals, .mask = SharedOrFixed, .value = MAP_SHARED },
    };
    /// Never executable.
    constexpr auto MappingCoverage = std::array {
        ArgumentCheck { .index = 2, .test = ArgumentTest::AllClear, .mask = PROT_EXEC, .value = 0 },
    };
    /// Never writable and executable at once.
    constexpr auto ProtectionRelease = std::array {
        ArgumentCheck { .index = 2,
                        .test = ArgumentTest::NotEquals,
                        .mask = WritableOrExecutable,
                        .value = WritableOrExecutable },
    };
    /// The descriptor's own metadata: the path argument is empty.
    constexpr auto StatOfDescriptorAt3 = std::array {
        ArgumentCheck { .index = 3, .test = ArgumentTest::AllSet, .mask = AT_EMPTY_PATH, .value = 0 },
    };
    constexpr auto StatOfDescriptorAt2 = std::array {
        ArgumentCheck { .index = 2, .test = ArgumentTest::AllSet, .mask = AT_EMPTY_PATH, .value = 0 },
    };

    // Most frequent first, as the helper suites measured them: the filter tests the rows in this order.
    constexpr auto ReleaseTable = std::array {
        allow(SystemCall::Futex, "every wait and wake between the host's threads"),
        allow(SystemCall::Sendto, "send() on a channel: every reply, host call and growth request"),
        allow(SystemCall::Recvfrom, "recv() on a lane, and on the wake signal"),
        allowWhen(SystemCall::Recvmsg,
                  OnControl,
                  "the control channel takes in the memory objects Contour grants; nothing else does"),
        allow(SystemCall::Poll, "the interrupt listener waits on the wake signal and its stop"),
        allow(SystemCall::Ppoll, "poll() where the architecture has only ppoll"),
        allow(SystemCall::Read, "reads of descriptors the host holds already"),
        allow(SystemCall::Write, "the wake signal, and lines on stderr"),
        allowWhen(
            SystemCall::Mmap,
            MappingRelease,
            "the allocator, the engine's code and stacks, and guest memory mapped over its reservation"),
        allow(SystemCall::Munmap, "the allocator, and released instances"),
        allow(SystemCall::Madvise, "the allocator and the engine return pages"),
        allowWhen(SystemCall::Mprotect,
                  ProtectionRelease,
                  "compiled code turns executable once written, and guest memory opens as it grows"),
        allow(SystemCall::Mremap, "the allocator grows large blocks"),
        allow(SystemCall::Brk, "the allocator's main arena"),
        allow(SystemCall::Close, "released descriptors: granted objects, and the channels at the end"),
        allow(SystemCall::Shutdown, "the host ends its channels as it stops"),
        allow(SystemCall::RtSigreturn, "a guest trap returns from the engine's signal handler"),
        allow(SystemCall::Sigaltstack, "a thread's alternate signal stack is queried and removed as it ends"),
        allow(SystemCall::RtSigprocmask, "a thread blocks signals as it ends"),
        allow(SystemCall::SchedYield, "spin locks back off"),
        allow(SystemCall::ClockGettime, "the clock, where the vDSO cannot read it"),
        allow(SystemCall::Getrandom, "hash seeds of tables made after lockdown"),
        allow(SystemCall::Fstat, "a granted object's size is checked before it is mapped"),
        allowWhen(
            SystemCall::Newfstatat, StatOfDescriptorAt3, "fstat() where the C library makes it fstatat()"),
        allowWhen(SystemCall::Statx, StatOfDescriptorAt2, "fstat() where the C library makes it statx()"),
        refuse(SystemCall::Ioctl, ENOTTY, "the engine asks whether stderr is a terminal: it is not"),
        allow(SystemCall::Getpid, "abort() signals its own process"),
        allow(SystemCall::Gettid, "abort() signals its own thread"),
        allowWhen(SystemCall::Tgkill, OnOwnProcess, "abort() signals its own thread"),
        allowWhen(SystemCall::Kill, OnOwnProcess, "signals to its own process only"),
        allow(SystemCall::RestartSyscall, "a call interrupted by a stop resumes"),
        allow(SystemCall::Exit, "a thread ends"),
        allow(SystemCall::ExitGroup, "the host ends"),
        refuse(SystemCall::Socket, EPERM, "no network and no new channel"),
        refuse(SystemCall::Socketpair, EPERM, "no new channel"),
        refuse(SystemCall::Openat,
               EACCES,
               "no file: the C library's probes (overcommit_memory as an arena shrinks) do without"),
        refuse(SystemCall::Open, EACCES, "no file"),
        refuse(SystemCall::Openat2, EACCES, "no file"),
        allow(SystemCall::Membarrier,
              "the engine flushes other cores' instruction caches once it wrote code",
              Architectures::AArch64),
    };

    constexpr auto SanitizerTable = std::array {
        allow(SystemCall::Openat,
              "a report reads /proc/self/maps; the file system restriction allows /proc only"),
        allow(SystemCall::Gettimeofday, "the thread sanitizer's clock"),
        allow(SystemCall::Nanosleep, "the thread sanitizer's background thread sleeps"),
    };

    constexpr auto CoverageTable = std::array {
        allowWhen(SystemCall::Mmap,
                  MappingCoverage,
                  "counter files are mapped shared to be merged; the release row's other uses still pass"),
        allow(SystemCall::Openat, "counter files; the file system restriction allows the build tree only"),
        allow(SystemCall::Fcntl, "counter files are locked while they are merged"),
        allow(SystemCall::Lseek, "counter files are read and written in place"),
        allow(SystemCall::Ftruncate, "a counter file new to the build tree is sized before it is mapped"),
    };

    /// @return @p specs numbered for @p arch, without those that do not exist or apply there.
    [[nodiscard]] std::vector<SystemCallRow> numbered(std::span<RowSpec const> specs, AuditArch arch)
    {
        auto rows = std::vector<SystemCallRow> {};
        for (auto const& spec: specs)
        {
            if (spec.architectures == Architectures::AArch64 && arch != AuditArch::AArch64)
                continue;
            if (auto const number = systemCallNumber(spec.call, arch))
                rows.push_back(SystemCallRow { .number = *number,
                                               .checks = spec.checks,
                                               .action = spec.action,
                                               .error = spec.error,
                                               .reason = spec.reason });
        }
        return rows;
    }

    /// @return The rows @p instrumentation adds, numbered for @p arch.
    [[nodiscard]] std::vector<SystemCallRow> instrumentationRows(BuildInstrumentation instrumentation,
                                                                 AuditArch arch)
    {
        switch (instrumentation)
        {
            case BuildInstrumentation::None: return {};
            case BuildInstrumentation::Coverage: return coverageRows(arch);
            case BuildInstrumentation::Sanitizers: return sanitizerRows(arch);
        }
        return {};
    }
} // namespace

std::span<SystemCallNumbers const> systemCallNumbers() noexcept
{
    return NumberTable;
}

std::optional<long> systemCallNumber(SystemCall call, AuditArch arch) noexcept
{
    auto const& row = NumberTable.at(std::to_underlying(call));
    auto const number = arch == AuditArch::Amd64 ? row.amd64 : row.aarch64;
    if (number == NoSystemCall)
        return std::nullopt;
    return number;
}

std::vector<SystemCallRow> releaseRows(AuditArch arch)
{
    return numbered(ReleaseTable, arch);
}

std::vector<SystemCallRow> sanitizerRows(AuditArch arch)
{
    return numbered(SanitizerTable, arch);
}

std::vector<SystemCallRow> coverageRows(AuditArch arch)
{
    return numbered(CoverageTable, arch);
}

std::vector<SystemCallRow> rowsFor(BuildInstrumentation instrumentation, AuditArch arch)
{
    auto rows = releaseRows(arch);
    for (auto const& added: instrumentationRows(instrumentation, arch))
    {
        auto const same = std::ranges::find(rows, added.number, &SystemCallRow::number);
        if (same != rows.end())
            *same = added;
        else
            rows.push_back(added);
    }
    return rows;
}

std::vector<SystemCallRow> buildRows(AuditArch arch)
{
    return rowsFor(ThisBuildInstrumentation, arch);
}

} // namespace vtextension::wasm::host
