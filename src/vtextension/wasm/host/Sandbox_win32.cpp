// SPDX-License-Identifier: Apache-2.0
//
// The extension host's sandbox on Windows. A process enters an AppContainer only as it is created, so Contour
// starts the host in one (ProcessIsolation::AppContainer, remote/process/AppContainer_win32.hpp) with no
// capabilities, no child process and the kill-on-close job; what is left to the host is to verify it and to
// add what a process may only set on itself. Its lockdown asks its token whether it runs in an AppContainer:
// if not -- a test composition under wine, which stubs AppContainers, or a parent that did not isolate it --
// it runs `Unsandboxed`, and says so, which a parent requiring a lockdown refuses. If so, it turns on the
// mitigations of MitigationRows and checks that it runs in a job; every step runs, the first that failed
// fails the lockdown, and only all of them passing makes it `LockedDown`. The decision is
// ContainerSandbox.hpp's, tested wherever the host is built; this file asks the system.
//
// Before any thread there is nothing to take: the container and the job are the parent's, and the mitigations
// reach the whole process whenever they are set. The dynamic code policy stays off: the engine compiles a
// module's code and makes it executable.
#include <array>
#include <cstdint>
#include <expected>
#include <functional>
#include <memory>
#include <utility>
#include <vector>

#include <vtextension/wasm/host/ContainerSandbox.hpp>
#include <vtextension/wasm/host/Sandbox.hpp>

#define WIN32_LEAN_AND_MEAN
#include <Windows.h>

namespace vtextension::wasm::host
{

namespace
{
    /// One mitigation the host turns on itself: its policy, and the flags it is set to. Each policy's
    /// structure is a union of one DWORD of flags with the bit fields that name them.
    struct MitigationRow
    {
        PROCESS_MITIGATION_POLICY policy; ///< Which policy.
        DWORD (*flags)() noexcept;        ///< Its flags, set through the bit fields' names.
    };

    static_assert(sizeof(PROCESS_MITIGATION_EXTENSION_POINT_DISABLE_POLICY) == sizeof(DWORD));
    static_assert(sizeof(PROCESS_MITIGATION_IMAGE_LOAD_POLICY) == sizeof(DWORD));
    static_assert(sizeof(PROCESS_MITIGATION_STRICT_HANDLE_CHECK_POLICY) == sizeof(DWORD));

    /// The mitigations: no legacy extension point loads a DLL into it (AppInit DLLs, Winsock LSPs, shims), no
    /// image loads from a remote share or with a low mandatory label, and a use of an invalid handle raises
    /// an exception rather than fail quietly -- for good.
    constexpr auto MitigationRows = std::array {
        MitigationRow { .policy = ProcessExtensionPointDisablePolicy,
                        .flags =
                            []() noexcept {
                                auto policy = PROCESS_MITIGATION_EXTENSION_POINT_DISABLE_POLICY {};
                                policy.DisableExtensionPoints = 1;
                                return policy.Flags;
                            } },
        MitigationRow { .policy = ProcessImageLoadPolicy,
                        .flags =
                            []() noexcept {
                                auto policy = PROCESS_MITIGATION_IMAGE_LOAD_POLICY {};
                                policy.NoRemoteImages = 1;
                                policy.NoLowMandatoryLabelImages = 1;
                                return policy.Flags;
                            } },
        MitigationRow { .policy = ProcessStrictHandleCheckPolicy,
                        .flags =
                            []() noexcept {
                                auto policy = PROCESS_MITIGATION_STRICT_HANDLE_CHECK_POLICY {};
                                policy.RaiseExceptionOnInvalidHandleReference = 1;
                                policy.HandleExceptionsPermanentlyEnabled = 1;
                                return policy.Flags;
                            } },
    };

    static_assert(NotInJobCode == static_cast<int>(HRESULT_FROM_WIN32(ERROR_PROCESS_NOT_IN_JOB)));

    /// @return The HRESULT of this thread's last error.
    [[nodiscard]] int lastErrorCode() noexcept
    {
        return static_cast<int>(HRESULT_FROM_WIN32(::GetLastError()));
    }

    /// @return What this process's token says of its container; the HRESULT of why it could not be asked.
    [[nodiscard]] std::expected<TokenContainer, int> tokenContainer() noexcept
    {
        auto isAppContainer = DWORD { 0 };
        auto length = DWORD { 0 };
        if (!::GetTokenInformation(::GetCurrentProcessToken(),
                                   TokenIsAppContainer,
                                   &isAppContainer,
                                   sizeof(isAppContainer),
                                   &length))
            return std::unexpected(lastErrorCode());
        return isAppContainer != 0 ? TokenContainer::AppContainer : TokenContainer::None;
    }

    /// @return Whether this process runs in a job; the HRESULT of why it could not be asked.
    [[nodiscard]] std::expected<JobMembership, int> jobMembership() noexcept
    {
        auto inJob = BOOL { FALSE };
        if (!::IsProcessInJob(::GetCurrentProcess(), nullptr, &inJob))
            return std::unexpected(lastErrorCode());
        return inJob ? JobMembership::Inside : JobMembership::Outside;
    }

    /// @return What the sandbox asks this system: MitigationRows each, and the token and the job.
    [[nodiscard]] ContainerApi systemContainerApi()
    {
        auto mitigations = std::vector<std::function<std::expected<void, int>()>> {};
        for (auto const& row: MitigationRows)
            mitigations.emplace_back([row]() -> std::expected<void, int> {
                auto flags = row.flags();
                if (!::SetProcessMitigationPolicy(row.policy, &flags, sizeof(flags)))
                    return std::unexpected(lastErrorCode());
                return {};
            });
        return ContainerApi { .tokenContainer = &tokenContainer,
                              .mitigations = std::move(mitigations),
                              .jobMembership = &jobMembership };
    }
} // namespace

DescriptorLimit limitOf([[maybe_unused]] HeldDescriptor descriptor)
{
    // The container confines what the host may open, not what each handle it holds may be used for.
    return DescriptorLimit::Unlimited;
}

std::unique_ptr<ISandbox> makeSystemSandbox([[maybe_unused]] SandboxDescriptors descriptors,
                                            [[maybe_unused]] GuestCode code)
{
    // The container names no handle: it leaves the inherited ones as they are, and refuses the host any new
    // one it is not granted. How the engine runs code changes nothing (above).
    return makeContainerSandbox(systemContainerApi());
}

} // namespace vtextension::wasm::host
