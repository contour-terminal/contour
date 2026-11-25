// SPDX-License-Identifier: Apache-2.0
#pragma once

/// @file
/// How the composition root configures the backend that runs extensions: how their host processes are
/// started -- by the connector, which alone knows the host executable -- and how long they may take. Names no
/// engine.

#include <chrono>
#include <cstdint>
#include <memory>
#include <utility>

namespace vtextension::wasm
{

namespace remote
{
    class IHostConnector;
} // namespace remote

/// How long an extension host process may take over each step, before it counts as hung.
struct HostTimeouts
{
    std::chrono::milliseconds load { 10'000 };       ///< To load a module.
    std::chrono::milliseconds instantiate { 1'000 }; ///< To instantiate one.
    std::chrono::milliseconds handshake { 2'000 };   ///< To say Hello once started.
    /// To stop a call once interrupted, the time the parent spends serving its host calls not counted.
    std::chrono::milliseconds interruptGrace { 50 };
};

/// Which extension hosts the backend drives, by what they report of their confinement in their Hello.
enum class LockdownRequirement : uint8_t
{
    RequireLockedDown, ///< Only a host inside its sandbox; one that runs without is refused.
    AcceptUnsandboxed, ///< A host without a sandbox too: tests, and platforms that have none yet.
};

/// @param requirement A requirement, or any value of its type.
/// @return Whether @p requirement names one.
[[nodiscard]] constexpr bool isLockdownRequirement(LockdownRequirement requirement) noexcept
{
    switch (requirement)
    {
        case LockdownRequirement::RequireLockedDown:
        case LockdownRequirement::AcceptUnsandboxed: return true;
    }
    return false;
}

/// The last requirement: what a table with a row per requirement is sized by. isLockdownRequirement()'s
/// switch, which has no default, keeps it the last: a requirement added after it fails to compile until this
/// names it.
inline constexpr auto LastLockdownRequirement = LockdownRequirement::AcceptUnsandboxed;

static_assert(isLockdownRequirement(LastLockdownRequirement));
static_assert(
    !isLockdownRequirement(static_cast<LockdownRequirement>(std::to_underlying(LastLockdownRequirement) + 1)),
    "LastLockdownRequirement must name the last requirement");

/// How the backend that runs extensions is set up.
struct BackendConfig
{
    /// How long a host may take: a host overstaying its handshake, a load or an instantiation is stopped, and
    /// so is one whose interrupted call runs on past `interruptGrace` of its own time. A host serving in this
    /// process has no such backstop.
    HostTimeouts timeouts {};
    /// How hosts are started. Null leaves it to the composition root: the remote backend itself needs one,
    /// and fails every load `HostUnavailable` without it, while a composition that can run hosts in this
    /// process supplies a connector for them.
    std::shared_ptr<remote::IHostConnector> connector;
    /// Which hosts are driven: a host that failed to confine itself is refused whatever this says.
    LockdownRequirement lockdown = LockdownRequirement::RequireLockedDown;
};

} // namespace vtextension::wasm
