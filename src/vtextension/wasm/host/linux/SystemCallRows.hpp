// SPDX-License-Identifier: Apache-2.0
#pragma once

/// @file
/// Which system calls the extension host may make once it is locked down on Linux, as data: a row per call,
/// each saying what becomes of it and why. SeccompFilter.hpp assembles the rows into the filter the kernel
/// runs; a call no row names kills the process.
///
/// The rows were measured, not guessed: every call the unsandboxed helper made after it locked down, across
/// the helper suites in the release, sanitizer and coverage trees, is a row here, or refused where refusing
/// it is what the caller handles (a library probing a file it may do without). The release rows are the
/// product's; the sanitizer and coverage rows widen them only in builds made for those, and replace a release
/// row of the same call.

#include <cstdint>
#include <optional>
#include <span>
#include <string_view>
#include <vector>

#include <vtextension/wasm/BuildInstrumentation.hpp>

namespace vtextension::wasm::host
{

/// The architectures a filter is assembled for, by the value the kernel reports in `seccomp_data.arch`
/// (AUDIT_ARCH_*). The values are ours, so that both are known wherever the filter is tested.
enum class AuditArch : uint32_t
{
    Amd64 = 0xC000'003E,   ///< AUDIT_ARCH_X86_64: x86_64.
    AArch64 = 0xC000'00B7, ///< AUDIT_ARCH_AARCH64.
};

/// The architecture this build runs on.
#ifdef __x86_64__
inline constexpr auto ThisAuditArch = AuditArch::Amd64;
#elifdef __aarch64__
inline constexpr auto ThisAuditArch = AuditArch::AArch64;
#else
    #error "The Linux sandbox knows the system calls of x86_64 and aarch64 only."
#endif

/// Every system call a row or a test names. Numbered per architecture by systemCallNumbers().
enum class SystemCall : uint8_t
{
    Accept,
    Accept4,
    Bind,
    Bpf,
    Brk,
    ClockGettime,
    Clone,
    Clone3,
    Close,
    Connect,
    Execve,
    Execveat,
    Exit,
    ExitGroup,
    Fcntl,
    Fork,
    Fstat,
    Ftruncate,
    Futex,
    Getpid,
    Getppid,
    Getrandom,
    Gettid,
    Gettimeofday,
    Ioctl,
    Kill,
    LandlockAddRule,
    LandlockCreateRuleset,
    LandlockRestrictSelf,
    Listen,
    Lseek,
    Madvise,
    Membarrier,
    Mmap,
    Mount,
    Mprotect,
    Mremap,
    Munmap,
    Nanosleep,
    Newfstatat,
    Open,
    Openat,
    Openat2,
    PerfEventOpen,
    Personality,
    Poll,
    Ppoll,
    ProcessVmReadv,
    ProcessVmWritev,
    Ptrace,
    Read,
    Recvfrom,
    Recvmsg,
    RestartSyscall,
    RtSigaction,
    RtSigprocmask,
    RtSigreturn,
    SchedYield,
    Seccomp,
    Sendmmsg,
    Sendmsg,
    Sendto,
    Setns,
    Setsockopt,
    Shutdown,
    Sigaltstack,
    Socket,
    Socketpair,
    Statx,
    Tgkill,
    Unshare,
    Vfork,
    Write,
    Writev,
};

/// What an architecture numbers a call it does not have.
inline constexpr auto NoSystemCall = long { -1 };

/// A system call's numbers.
struct SystemCallNumbers
{
    SystemCall call;       ///< The call.
    std::string_view name; ///< Its name, as strace prints it.
    long amd64;            ///< Its number on x86_64; NoSystemCall if it has none there.
    long aarch64;          ///< Its number on aarch64; NoSystemCall if it has none there.
};

/// @return Every call's numbers, one row per SystemCall, at its index.
[[nodiscard]] std::span<SystemCallNumbers const> systemCallNumbers() noexcept;

/// @param call A call.
/// @param arch An architecture.
/// @return Its number there; nothing where it has none.
[[nodiscard]] std::optional<long> systemCallNumber(SystemCall call, AuditArch arch) noexcept;

/// What becomes of a call a row names.
enum class SystemCallAction : uint8_t
{
    Kill,   ///< The process is killed by SIGSYS.
    Refuse, ///< The call fails with the row's errno, and the process goes on.
    Allow,  ///< The call is made.
};

/// How an argument is tested: on `(argument & mask)` against `value`, each 32-bit half the mask covers.
enum class ArgumentTest : uint8_t
{
    Equals,           ///< `(argument & mask) == value`.
    NotEquals,        ///< `(argument & mask) != value`.
    AllClear,         ///< `(argument & mask) == 0`; value unused.
    AllSet,           ///< `(argument & mask) == mask`; value unused.
    EqualsControl,    ///< `(argument & mask) ==` the control channel's descriptor; value unused.
    EqualsOwnProcess, ///< `(argument & mask) ==` the process's own id; value unused.
};

/// A test a call's argument must pass for its row to decide; a call failing any of its row's is killed.
struct ArgumentCheck
{
    uint8_t index;     ///< Which argument, 0 to 5.
    ArgumentTest test; ///< How it is tested.
    uint64_t mask;     ///< Which of its bits are tested.
    uint64_t value;    ///< What they are compared with, where the test compares with a value.
};

/// What becomes of one system call, and why.
struct SystemCallRow
{
    long number;                           ///< The call's number on the architecture the rows are for.
    std::span<ArgumentCheck const> checks; ///< What its arguments must pass; all of them.
    SystemCallAction action;               ///< What becomes of it when they pass.
    int error;                             ///< The errno a `Refuse` fails it with; 0 otherwise.
    std::string_view reason;               ///< Why, for whoever reads the table.
};

/// @param arch The architecture the rows are numbered for.
/// @return What every build allows or refuses, most frequent first.
[[nodiscard]] std::vector<SystemCallRow> releaseRows(AuditArch arch);

/// @param arch The architecture the rows are numbered for.
/// @return What a sanitizer's runtime needs beyond them.
[[nodiscard]] std::vector<SystemCallRow> sanitizerRows(AuditArch arch);

/// @param arch The architecture the rows are numbered for.
/// @return What writing coverage counters needs beyond them.
[[nodiscard]] std::vector<SystemCallRow> coverageRows(AuditArch arch);

/// @param instrumentation How the build is instrumented.
/// @param arch The architecture the rows are numbered for.
/// @return The rows such a build locks down with: releaseRows(), each replaced by a row of the same call in
/// the
///         set @p instrumentation names, and that set's other rows after them.
[[nodiscard]] std::vector<SystemCallRow> rowsFor(BuildInstrumentation instrumentation, AuditArch arch);

/// @param arch The architecture the rows are numbered for.
/// @return The rows this build locks down with: rowsFor(ThisBuildInstrumentation).
[[nodiscard]] std::vector<SystemCallRow> buildRows(AuditArch arch);

} // namespace vtextension::wasm::host
