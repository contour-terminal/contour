// SPDX-License-Identifier: Apache-2.0
#pragma once

/// @file
/// The extension host's sandbox on Windows, as a decision over what the system is asked: whether the host's
/// token is an AppContainer's, the mitigations it turns on itself, and whether it runs in a job. Free of
/// Windows itself, so that the decision is tested wherever the host is built; Sandbox_win32.cpp asks the
/// system through it (systemContainerApi()).

#include <cstdint>
#include <expected>
#include <functional>
#include <memory>
#include <vector>

#include <vtextension/wasm/host/Sandbox.hpp>

namespace vtextension::wasm::host
{

/// What the host's token says of its container.
enum class TokenContainer : uint8_t
{
    None,         ///< An ordinary token: it was started in no AppContainer.
    AppContainer, ///< An AppContainer's.
};

/// Whether the host runs in a job.
enum class JobMembership : uint8_t
{
    Outside, ///< In none: nothing kills it with Contour, and nothing limits the processes it starts.
    Inside,  ///< In one: the kill-on-close job its parent started it in.
};

/// The HRESULT a host outside any job states for `LockdownStep::Job`:
/// HRESULT_FROM_WIN32(ERROR_PROCESS_NOT_IN_JOB), which Sandbox_win32.cpp checks against the system's headers.
inline constexpr auto NotInJobCode = static_cast<int>(0x8007'02F7U);

/// What the Windows sandbox asks the system, each answering with an HRESULT where it cannot.
struct ContainerApi
{
    /// Asks the process token whether it is an AppContainer's.
    std::function<std::expected<TokenContainer, int>()> tokenContainer;
    /// Each mitigation the host turns on itself (SetProcessMitigationPolicy()), in order.
    std::vector<std::function<std::expected<void, int>()>> mitigations;
    /// Asks whether the process runs in a job (IsProcessInJob()).
    std::function<std::expected<JobMembership, int>()> jobMembership;
};

/// Makes the Windows sandbox over @p api. Before any thread it has nothing to take. Its lockdown runs every
/// step and states the first that failed: a token that cannot be asked fails `Container`; a token in no
/// container runs `Unsandboxed`, and nothing else is asked; in a container, every mitigation is turned on
/// (a refused one fails `Mitigations`), and the job asked of (none, or one that cannot be asked, fails
/// `Job`). Only all of them passing makes it `LockedDown`.
/// @param api What the system is asked through.
/// @return The sandbox; never null.
[[nodiscard]] std::unique_ptr<ISandbox> makeContainerSandbox(ContainerApi api);

} // namespace vtextension::wasm::host
