// SPDX-License-Identifier: Apache-2.0
#pragma once

/// @file
/// The extension host's sandbox, as the host process drives it: two steps at two points of its start, the
/// first before it starts any thread, the second once every thread it runs was started and prepared.
///
/// What is set per thread and inherited by the threads started after it (Linux no_new_privs and Landlock,
/// FreeBSD PROC_NO_NEW_PRIVS) is the first step's. What would forbid what preparing a thread still does, or
/// must reach threads that run already (a system call filter synchronized across threads, capability mode, a
/// sandbox profile), is the second's. Each system's sandbox is made by makeSystemSandbox(); a system without
/// one runs the host unsandboxed (NullSandbox), and says so in its Hello.

#include <cstdint>
#include <expected>
#include <memory>
#include <string>

#include <vtextension/wasm/remote/proto/Pdu.hpp>
#include <vtextension/wasm/remote/transport/Channel.hpp>

#ifndef CONTOUR_PLATFORM_SANDBOX
    #error "CONTOUR_PLATFORM_SANDBOX is set by linking vtextension_host."
#endif

namespace vtextension::wasm::host
{

/// Which system's sandbox makeSystemSandbox() makes in this build.
enum class PlatformSandbox : uint8_t
{
    None,    ///< None: the host runs unsandboxed.
    Linux,   ///< no_new_privs, Landlock and a seccomp filter.
    FreeBSD, ///< Capsicum.
    MacOS,   ///< A Seatbelt profile.
    Windows, ///< An AppContainer, verified by the host.
};

/// This build's sandbox, as the build chose its source (CONTOUR_PLATFORM_SANDBOX, a public definition of
/// vtextension_host): a host of this build says `LockedDown` unless it is `None`.
inline constexpr auto ThisPlatformSandbox = PlatformSandbox { CONTOUR_PLATFORM_SANDBOX };

/// A step of the host's confinement that failed, and how.
struct ConfinementFailure
{
    remote::proto::LockdownStep step; ///< The step; never `None`.
    /// What the system said of it: errno on POSIX, an HRESULT on Windows.
    int code;

    bool operator==(ConfinementFailure const&) const = default;
};

/// @param failure A step that failed.
/// @return What failed and why, user-readable: the step's text (lockdownSteps()), then the system's word on
///         the error.
[[nodiscard]] std::string confinementFailureText(ConfinementFailure const& failure);

/// What a sandbox confines the host to: the handles it keeps using once it is locked down.
struct SandboxDescriptors
{
    remote::transport::NativeHandle control; ///< The control channel's helper end, adopted by the host.
};

/// The host's sandbox. Each step is taken once, in order; a step that failed leaves the other to be taken
/// all the same, so that a host is as confined as the system lets it be.
class ISandbox
{
  public:
    ISandbox() = default;
    ISandbox(ISandbox const&) = delete;
    ISandbox(ISandbox&&) = delete;
    ISandbox& operator=(ISandbox const&) = delete;
    ISandbox& operator=(ISandbox&&) = delete;
    virtual ~ISandbox() = default;

    /// Restricts what every thread started from now on inherits: on the main thread, while it is the only
    /// one.
    /// @return Nothing once restricted; the first step that failed otherwise.
    [[nodiscard]] virtual std::expected<void, ConfinementFailure> restrictBeforeThreads() = 0;

    /// Locks the host down, once every thread it runs was started and prepared (IHostedEngine::
    /// prepareThread()) and its resources were capped: nothing it does from then on may need more.
    /// @return Nothing once locked down; the first step that failed otherwise.
    [[nodiscard]] virtual std::expected<void, ConfinementFailure> lockDown() = 0;

    /// @return How far the host is confined once both steps were taken without a failure: what its Hello
    ///         states then. `Unsandboxed` where the system has no sandbox, `LockedDown` where it has one.
    [[nodiscard]] virtual remote::proto::LockdownState lockdownState() const noexcept = 0;
};

/// The sandbox of a system that has none: both steps do nothing, and the host runs `Unsandboxed`.
class NullSandbox final: public ISandbox
{
  public:
    [[nodiscard]] std::expected<void, ConfinementFailure> restrictBeforeThreads() override { return {}; }
    [[nodiscard]] std::expected<void, ConfinementFailure> lockDown() override { return {}; }
    [[nodiscard]] remote::proto::LockdownState lockdownState() const noexcept override
    {
        return remote::proto::LockdownState::Unsandboxed;
    }
};

/// Makes this system's sandbox for a host that keeps using @p descriptors. Defined per system: where there is
/// no sandbox yet (Sandbox_none.cpp), a NullSandbox.
/// @param descriptors The handles the host keeps using.
/// @return The sandbox; never null.
[[nodiscard]] std::unique_ptr<ISandbox> makeSystemSandbox(SandboxDescriptors descriptors);

} // namespace vtextension::wasm::host
