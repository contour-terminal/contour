// SPDX-License-Identifier: Apache-2.0
#pragma once

/// @file
/// What only Linux's launcher has: whether the kernel waits on pidfds, asked once and handed in, so that a
/// launcher on a kernel that cannot is made and tested on any kernel.

#include <cstdint>
#include <memory>

#include <vtextension/wasm/remote/process/ProcessLauncher.hpp>

namespace vtextension::wasm::remote::process
{

/// Whether the kernel waits on pidfds.
enum class PidfdWaits : uint8_t
{
    Unsupported, ///< Before Linux 5.4: no waitid(P_PIDFD), or (before 5.3) no pidfd at all.
    Supported,   ///< waitid(P_PIDFD) works.
};

/// @return Whether the kernel waits on pidfds, asked of this process's own: waitid(P_PIDFD) on it fails
///         ECHILD where the id type is known, as a process is no child of its own, and EINVAL where not.
[[nodiscard]] PidfdWaits probePidfdWaits() noexcept;

/// @param pidfdWaits Whether the kernel waits on pidfds; under `Unsupported` every launch is refused as
///                   `LaunchError::Unsupported` before anything starts, as a child that could not be waited
///                   for must not run.
/// @return The launcher of this system's processes. makeSystemProcessLauncher() makes it with
///         probePidfdWaits().
[[nodiscard]] std::shared_ptr<IProcessLauncher> makeLinuxProcessLauncher(PidfdWaits pidfdWaits);

} // namespace vtextension::wasm::remote::process
