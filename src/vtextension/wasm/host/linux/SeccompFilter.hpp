// SPDX-License-Identifier: Apache-2.0
#pragma once

/// @file
/// The extension host's system call filter on Linux: SystemCallRows.hpp's rows, assembled by hand into the
/// classic BPF program seccomp runs, and installed on every thread at once.
///
/// The program checks the architecture first (another one, or an x32 call on x86_64, kills the process),
/// then tests each row's number in the rows' order; a row whose number matches decides when its argument
/// checks pass. A call no row names, or one whose row's checks fail, takes the unlisted action: for the
/// host's own filter, killing the process.

#include <cstdint>
#include <expected>
#include <span>
#include <vector>

#include <linux/filter.h>
#include <vtextension/wasm/host/Sandbox.hpp>
#include <vtextension/wasm/host/linux/SystemCallRows.hpp>

namespace vtextension::wasm::host
{

/// The seccomp return value of @p action.
/// @param action What becomes of the call.
/// @param error The errno a `Refuse` fails it with.
/// @return SECCOMP_RET_KILL_PROCESS, SECCOMP_RET_ERRNO with @p error, or SECCOMP_RET_ALLOW.
[[nodiscard]] uint32_t seccompReturnOf(SystemCallAction action, int error) noexcept;

/// Assembles @p rows into a program that kills the process for any call they do not name.
/// @param rows What becomes of each call named, each number at most once.
/// @param descriptors The descriptors `EqualsControl` compares with.
/// @param ownProcess The process id `EqualsOwnProcess` compares with.
/// @param arch The architecture @p rows are numbered for, and the only one the program lets through.
/// @return The program; `SystemCalls` with `E2BIG` if it exceeds what the kernel runs or a jump reaches,
///         with `EINVAL` if a check names no argument or a `Refuse` no errno.
[[nodiscard]] std::expected<std::vector<sock_filter>, ConfinementFailure> assembleFilter(
    std::span<SystemCallRow const> rows, SandboxDescriptors descriptors, int ownProcess, AuditArch arch);

/// assembleFilter(), with the action of a call @p rows do not name given: what an outer sandbox's filter,
/// which lets through all it does not refuse, is made of. A call whose row's argument checks fail takes
/// @p unlisted as well, as the row does not apply to it: a row refusing one value of an argument lets every
/// other value through an allow-by-default filter, rather than killing it.
/// @param rows What becomes of each call named, each number at most once.
/// @param descriptors The descriptors `EqualsControl` compares with.
/// @param ownProcess The process id `EqualsOwnProcess` compares with.
/// @param arch The architecture @p rows are numbered for, and the only one the program lets through.
/// @param unlisted What becomes of a call no row names.
/// @return The program, or why it cannot be assembled.
[[nodiscard]] std::expected<std::vector<sock_filter>, ConfinementFailure> assembleFilter(
    std::span<SystemCallRow const> rows,
    SandboxDescriptors descriptors,
    int ownProcess,
    AuditArch arch,
    SystemCallAction unlisted);

/// Installs @p program on every thread of the process (SECCOMP_FILTER_FLAG_TSYNC). The calling thread must
/// have no_new_privs set; a thread started after it inherits the filter.
/// @param program An assembled program.
/// @return Nothing once every thread runs it; `ThreadSync` (code `EPERM`) if a thread could not take it,
///         which the kernel says when a thread runs a filter of its own; `SystemCalls` with the errno
///         otherwise.
[[nodiscard]] std::expected<void, ConfinementFailure> installFilter(std::span<sock_filter const> program);

} // namespace vtextension::wasm::host
