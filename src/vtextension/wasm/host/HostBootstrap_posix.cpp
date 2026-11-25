// SPDX-License-Identifier: Apache-2.0
//
// What the extension host does to itself on Linux, FreeBSD and macOS. macOS has no parent-death binding, no
// cap that bounds mmap (RLIMIT_AS and RLIMIT_DATA do not) and no way to deny its own user's debugger short of
// ptrace(PT_DENY_ATTACH), so there it only checks its parent and refuses core dumps.
#include <sys/resource.h>

#include <algorithm>
#include <array>
#include <cerrno>
#include <csignal>
#include <cstdint>
#include <expected>
#include <optional>
#include <span>
#include <string>
#include <system_error>

#include <unistd.h>

#include <vtextension/wasm/host/HostBootstrap.hpp>

#ifdef __linux__
    #include <sys/prctl.h>
#elifdef __FreeBSD__
    #include <sys/procctl.h>
#endif

namespace vtextension::wasm::host
{

using remote::proto::LockdownStep;

namespace
{
    /// What a resource is capped to.
    enum class CappedValue : uint8_t
    {
        Zero,      ///< None at all.
        DataBytes, ///< The data bytes asked for; not capped when they are 0.
    };

    /// One resource cap.
    struct ResourceCap
    {
        int resource;      ///< The limit (RLIMIT_…).
        CappedValue value; ///< What it is capped to.
    };

    /// The caps this system applies.
#ifdef __linux__
    // RLIMIT_DATA counts private writable mappings, not the PROT_NONE reservations an engine makes for its
    // memories, so it bounds what the host commits.
    constexpr auto Caps =
        std::array { ResourceCap { .resource = RLIMIT_NPROC, .value = CappedValue::Zero },
                     ResourceCap { .resource = RLIMIT_DATA, .value = CappedValue::DataBytes } };
#elifdef __FreeBSD__
    constexpr auto Caps = std::array { ResourceCap { .resource = RLIMIT_NPROC, .value = CappedValue::Zero } };
#else
    constexpr auto Caps = std::array<ResourceCap, 0> {};
#endif

    /// @return Nothing if @p result is 0; errno otherwise.
    [[nodiscard]] std::expected<void, int> errnoOf(int result) noexcept
    {
        if (result != 0)
            return std::unexpected(errno);
        return {};
    }

    /// Refuses core dumps, soft and hard.
    [[nodiscard]] std::expected<void, int> refuseCoreDumps() noexcept
    {
        auto const noCore = rlimit { .rlim_cur = 0, .rlim_max = 0 };
        return errnoOf(::setrlimit(RLIMIT_CORE, &noCore));
    }

#ifdef __linux__
    /// Makes this process not dumpable: no other process of its user attaches to it or reads its memory.
    [[nodiscard]] std::expected<void, int> refuseDumping() noexcept
    {
        return errnoOf(::prctl(PR_SET_DUMPABLE, 0));
    }
#elifdef __FreeBSD__
    /// Makes this process not traceable: no other process of its user attaches to it or reads its memory.
    [[nodiscard]] std::expected<void, int> refuseTracing() noexcept
    {
        auto traceControl = PROC_TRACE_CTL_DISABLE;
        return errnoOf(::procctl(P_PID, 0, PROC_TRACE_CTL, &traceControl));
    }
#endif

    /// The steps this system's hardening takes.
    constexpr auto HardeningSteps = std::array {
        HardeningRow { .step = LockdownStep::CoreDumps, .apply = &refuseCoreDumps },
#ifdef __linux__
        HardeningRow { .step = LockdownStep::Dumpable, .apply = &refuseDumping },
#elifdef __FreeBSD__
        HardeningRow { .step = LockdownStep::Tracing, .apply = &refuseTracing },
#endif
    };

    /// Asks the system to kill this process when its parent ends.
    /// @return Nothing once it took the request, or where there is no such request to make; `Refused` if it
    ///         did not take it.
    [[nodiscard]] std::expected<void, BootstrapError> requestParentDeathSignal() noexcept
    {
#ifdef __linux__
        auto const requested = ::prctl(PR_SET_PDEATHSIG, SIGKILL) == 0;
#elifdef __FreeBSD__
        auto signal = SIGKILL;
        auto const requested = ::procctl(P_PID, 0, PROC_PDEATHSIG_CTL, &signal) == 0;
#else
        auto const requested = true;
#endif
        if (!requested)
            return std::unexpected(BootstrapError::Refused);
        return {};
    }
} // namespace

std::expected<void, BootstrapError> bindToParent(int64_t parentProcessId) noexcept
{
    // Checked after the binding: a parent that ended before it reparented this process already, and the
    // signal will never come.
    return requestParentDeathSignal().and_then([parentProcessId]() -> std::expected<void, BootstrapError> {
        if (static_cast<int64_t>(::getppid()) != parentProcessId)
            return std::unexpected(BootstrapError::ParentGone);
        return {};
    });
}

std::span<HardeningRow const> hardeningSteps() noexcept
{
    return HardeningSteps;
}

std::string systemErrorText(int code)
{
    return std::system_category().message(code);
}

std::array<HeldDescriptor, 3> standardStreams() noexcept
{
    return { HeldDescriptor { .role = DescriptorRole::IdleStream, .handle = STDIN_FILENO },
             HeldDescriptor { .role = DescriptorRole::IdleStream, .handle = STDOUT_FILENO },
             HeldDescriptor { .role = DescriptorRole::StandardError, .handle = STDERR_FILENO } };
}

std::expected<void, ConfinementFailure> capResources(uint64_t dataBytes, CapPolicy policy) noexcept
{
    if (policy == CapPolicy::Skip)
        return {};
    auto failed = std::optional<int> {};
    for (auto const& cap: Caps)
    {
        if (cap.value == CappedValue::DataBytes && dataBytes == 0)
            continue;
        auto inherited = rlimit {};
        auto const capped = errnoOf(::getrlimit(cap.resource, &inherited)).and_then([&] {
            // A hard limit cannot be raised: one stricter than the cap, inherited from Contour, caps already.
            auto const wanted =
                cap.value == CappedValue::Zero ? rlim_t { 0 } : static_cast<rlim_t>(dataBytes);
            auto const value = std::min(wanted, inherited.rlim_max);
            auto const limit = rlimit { .rlim_cur = value, .rlim_max = value };
            return errnoOf(::setrlimit(cap.resource, &limit));
        });
        if (!capped && !failed)
            failed = capped.error();
    }
    if (failed)
        return std::unexpected(ConfinementFailure { .step = LockdownStep::ResourceCaps, .code = *failed });
    return {};
}

} // namespace vtextension::wasm::host
