// SPDX-License-Identifier: Apache-2.0
//
// The extension host's sandbox on macOS: a Seatbelt profile, deny by default (macos/SandboxProfile.hpp),
// applied once every thread was started and prepared. It is the process's, so it reaches every thread at
// once. Before any thread there is nothing to take: macOS has no no_new_privs, and the profile refuses fork
// and exec outright. Nor does a parent-death binding remain: macOS has none, and bindToParent() only checks
// that the parent is still there (HostBootstrap.hpp).
//
// The profile looks up no Mach service, and the engine needs none: Wasmtime's Mach exception port is off
// (wasmtime_config_macos_use_mach_ports_set(false), wasmtime/WasmtimeEngine.cpp), so its traps arrive as
// signals and no handler thread of its own runs. Whether the engine makes compiled code executable is the
// hardened runtime's to allow, through the extension host's one entitlement
// (support/macOS/contour-extension-host.entitlements), not the profile's.
#include <cerrno>
#include <cstdint>
#include <expected>
#include <memory>
#include <string>
#include <utility>

#include <vtextension/wasm/host/HostProcess.hpp>
#include <vtextension/wasm/host/Sandbox.hpp>
#include <vtextension/wasm/host/macos/SandboxProfile.hpp>

// libsystem_sandbox's, declared as Chromium declares them (sandbox/mac/seatbelt.cc) rather than through
// <sandbox.h>: that header declares no sandbox_init_with_parameters() at all, and sandbox_free_error()
// deprecated, which -Werror refuses. Mach-O C names carry a leading underscore; the labels name them so, so
// that the C++ names follow the project's.
extern "C"
{
    /// Applies the profile @p profile, reading its parameters from @p parameters, to this process.
    /// @return 0 once applied; -1 otherwise, with what is wrong in @p errorbuf.
    int sandboxInitWithParameters(char const* profile,
                                  uint64_t flags,
                                  char const* const parameters[],
                                  char** errorbuf) __asm__("_sandbox_init_with_parameters");
    /// Frees what sandboxInitWithParameters() wrote to its errorbuf.
    void sandboxFreeError(char* errorbuf) __asm__("_sandbox_free_error");
}

namespace vtextension::wasm::host
{

namespace
{
    /// The flags the profile is applied with: none, as it is profile text rather than a named profile.
    constexpr auto ProfileTextFlags = uint64_t { 0 };

    class MacSandbox final: public ISandbox
    {
      public:
        /// Takes the profile in as the C string the library reads, so that locking down allocates nothing.
        /// @param descriptors What the host keeps using.
        explicit MacSandbox(SandboxDescriptors descriptors):
            _descriptors { std::move(descriptors) }, _profile { SandboxProfile }
        {
        }

        [[nodiscard]] std::expected<void, ConfinementFailure> restrictBeforeThreads() override { return {}; }

        [[nodiscard]] std::expected<void, ConfinementFailure> lockDown() override
        {
            constexpr auto Parameters = profileParameterList();
            char* errorbuf = nullptr;
            errno = 0;
            if (sandboxInitWithParameters(_profile.c_str(), ProfileTextFlags, Parameters.data(), &errorbuf)
                == 0)
                return {};
            // The library says why in words, not always in errno: a profile it cannot compile sets none.
            auto const code = errno != 0 ? errno : EINVAL;
            if (errorbuf)
            {
                tellOnStandardError(std::string { "sandbox profile: " } + errorbuf);
                sandboxFreeError(errorbuf);
            }
            return std::unexpected(
                ConfinementFailure { .step = remote::proto::LockdownStep::Profile, .code = code });
        }

        [[nodiscard]] remote::proto::LockdownState lockdownState() const noexcept override
        {
            return remote::proto::LockdownState::LockedDown;
        }

      private:
        /// What the host keeps using. The profile names no descriptor: it leaves these as they are, and
        /// refuses the host any new one.
        SandboxDescriptors _descriptors;
        std::string _profile; ///< SandboxProfile.
    };
} // namespace

DescriptorLimit limitOf([[maybe_unused]] HeldDescriptor descriptor)
{
    // Its profile confines operations, not what each descriptor may be used for.
    return DescriptorLimit::Unlimited;
}

std::unique_ptr<ISandbox> makeSystemSandbox(SandboxDescriptors descriptors, [[maybe_unused]] GuestCode code)
{
    // How the engine runs code changes nothing here (above).
    return std::make_unique<MacSandbox>(std::move(descriptors));
}

} // namespace vtextension::wasm::host
