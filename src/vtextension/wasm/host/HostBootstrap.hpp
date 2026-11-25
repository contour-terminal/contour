// SPDX-License-Identifier: Apache-2.0
#pragma once

/// @file
/// What the extension host process does to itself before it serves: it binds its life to its parent's, first
/// thing; hardens itself while its main thread is its only one; and caps its own resources once its threads
/// run.
///
/// Each is the helper's own doing rather than the launcher's: on Linux the parent-death signal is bound to
/// the thread that started the process, which only the child can name after the fact; what is set per thread
/// (no_new_privs, Landlock) reaches every thread only when set before the first is created, which the
/// hardening step is the place for; and RLIMIT_NPROC counts threads, so a cap applied before exec would keep
/// the helper from starting its own.

#include <cstdint>
#include <expected>
#include <span>
#include <string_view>

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

/// A step of the host's hardening.
enum class HardeningStep : uint8_t
{
    CoreDumps,    ///< No core dump (POSIX: RLIMIT_CORE 0, soft and hard).
    Dumpable,     ///< Not dumpable (Linux: PR_SET_DUMPABLE 0).
    Tracing,      ///< Not traceable (FreeBSD: PROC_TRACE_CTL_DISABLE).
    FaultReports, ///< No fault report with its dialog (Windows: WerSetFlags).
};

/// One step of the hardening, as this system takes it.
struct HardeningRow
{
    HardeningStep step;    ///< The step.
    std::string_view text; ///< What failed if it fails, user-readable: a sentence's end.
    /// Takes the step.
    /// @return Nothing once taken; the system's error code otherwise (errno on POSIX, an HRESULT on Windows).
    std::expected<void, int> (*apply)() noexcept;
};

/// @return The steps this system's hardening takes, in order: no core dump, which would stall a crash while
/// it
///         is written and put an extension's memory on disk; on Linux not dumpable, on FreeBSD not traceable,
///         so that another process of the same user can neither attach to it nor read its memory; on Windows
///         no fault report with its dialog, where the job Contour started it in ends it on an unhandled
///         exception.
[[nodiscard]] std::span<HardeningRow const> hardeningSteps() noexcept;

/// Takes every step of @p steps, even past one that failed, so that a host is as hard as the system lets it
/// be.
/// @param steps The steps, in order.
/// @return Nothing if every step was taken; the first that failed otherwise.
[[nodiscard]] std::expected<void, HardeningStep> hardenProcess(std::span<HardeningRow const> steps) noexcept;

/// Hardens this process by this system's steps (hardeningSteps()), before it starts any thread of its own.
/// @return Nothing if every step was taken; the first that failed otherwise.
[[nodiscard]] std::expected<void, HardeningStep> hardenProcess() noexcept;

/// @param step A step of the hardening.
/// @return What failed when it fails, as this system's row says; empty if this system takes no such step.
[[nodiscard]] std::string_view hardeningStepText(HardeningStep step) noexcept;

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
