// SPDX-License-Identifier: Apache-2.0
//
// What the extension host does to itself on Linux, FreeBSD and macOS. macOS has no parent-death binding and
// no cap that bounds mmap (RLIMIT_AS and RLIMIT_DATA do not), so there it only checks its parent.
#include <sys/resource.h>

#include <algorithm>
#include <array>
#include <csignal>
#include <cstdint>
#include <expected>
#include <span>

#include <unistd.h>

#include <vtextension/wasm/host/HostBootstrap.hpp>

#ifdef __linux__
    #include <sys/prctl.h>
#elifdef __FreeBSD__
    #include <sys/procctl.h>
#endif

namespace vtextension::wasm::host
{

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

remote::proto::LockdownState capResources(uint64_t dataBytes, CapPolicy policy) noexcept
{
    if (policy == CapPolicy::Skip)
        return remote::proto::LockdownState::Unsandboxed;
    auto const applied = std::ranges::all_of(Caps, [dataBytes](ResourceCap const& cap) {
        if (cap.value == CappedValue::DataBytes && dataBytes == 0)
            return true;
        auto const value = cap.value == CappedValue::Zero ? rlim_t { 0 } : static_cast<rlim_t>(dataBytes);
        auto const limit = rlimit { .rlim_cur = value, .rlim_max = value };
        return ::setrlimit(cap.resource, &limit) == 0;
    });
    return applied ? remote::proto::LockdownState::Unsandboxed : remote::proto::LockdownState::Failed;
}

} // namespace vtextension::wasm::host
