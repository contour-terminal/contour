// SPDX-License-Identifier: Apache-2.0
#pragma once

/// @file
/// What the extension host process does to itself before it serves: it binds its life to its parent's, first
/// thing, and caps its own resources once its threads run.
///
/// Both are the helper's own doing rather than the launcher's: on Linux the parent-death signal is bound to
/// the thread that started the process, which only the child can name after the fact, and RLIMIT_NPROC
/// counts threads, so a cap applied before exec would keep the helper from starting its own.

#include <cstdint>
#include <expected>

#include <vtextension/wasm/remote/proto/Pdu.hpp>

namespace vtextension::wasm::host
{

/// Why the host cannot bind itself to its parent.
enum class BootstrapError : uint8_t
{
    ParentGone, ///< Its parent is not the process it was told of: that one ended, and it was reparented.
    Refused,    ///< The system refused the binding.
};

/// Ends this process when its parent ends: Linux PR_SET_PDEATHSIG, FreeBSD PROC_PDEATHSIG_CTL, each with
/// SIGKILL, then a check that the parent is still @p parentProcessId, in case it ended before the binding.
/// macOS has no such binding and only checks; Windows does nothing, as the kill-on-close job Contour started
/// it in binds it already.
/// @param parentProcessId The process that started this one.
/// @return Nothing once bound; why not otherwise.
[[nodiscard]] std::expected<void, BootstrapError> bindToParent(int64_t parentProcessId) noexcept;

/// Whether the host caps its own resources.
enum class CapPolicy : uint8_t
{
    Skip,  ///< It caps nothing: a sanitizer's shadow memory and its stop-the-world threads exceed any cap.
    Apply, ///< It caps them.
};

/// How this build caps the host: not at all in a sanitizer build (CONTOUR_SANITIZED_BUILD).
#ifdef CONTOUR_SANITIZED_BUILD
inline constexpr CapPolicy BuildCapPolicy = CapPolicy::Skip;
#else
inline constexpr CapPolicy BuildCapPolicy = CapPolicy::Apply;
#endif

/// Caps this process's resources, as far as @p policy says: on Linux no further process or thread
/// (RLIMIT_NPROC 0) and @p dataBytes of data memory (RLIMIT_DATA); on FreeBSD no further process or thread;
/// on macOS and Windows nothing (Windows caps memory in the job Contour started it in). Soft and hard limits
/// alike, so that they cannot be raised again; a hard limit it inherited that is stricter still is kept, as
/// it caps already.
/// @param dataBytes The cap on its data memory; 0 for none.
/// @param policy Whether to cap at all.
/// @return `Unsandboxed`; `Failed` if the system refused a cap.
[[nodiscard]] remote::proto::LockdownState capResources(uint64_t dataBytes, CapPolicy policy) noexcept;

} // namespace vtextension::wasm::host
