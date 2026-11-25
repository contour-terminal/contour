// SPDX-License-Identifier: Apache-2.0
//
// The extension host's sandbox on FreeBSD. Before any thread: PROC_NO_NEW_PRIVS, where the system has it
// (FreeBSD 14 and later). Once every thread was started and prepared: each descriptor the host keeps is
// limited to what its role uses it for (DescriptorRights.hpp), and then the process enters capability mode,
// which every thread takes on at its next system call: no file system, no network address, no other process.
//
// Capsicum cannot keep a socket that may send from sending descriptors over it (SCM_RIGHTS); what the host
// attaches, Contour's end discards.
#include <sys/capsicum.h>
#include <sys/procctl.h>

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <memory>
#include <optional>
#include <ranges>
#include <utility>

#include <unistd.h>

#include <vtextension/wasm/host/DescriptorRights.hpp>
#include <vtextension/wasm/host/Sandbox.hpp>

namespace vtextension::wasm::host
{

namespace
{
    /// A right in Capsicum's words.
    struct CapabilityRight
    {
        DescriptorRight right; ///< The right.
        uint64_t capability;   ///< The CAP_… right it is (rights(4)).
    };

    /// Every DescriptorRight, at its enumerator's index. GetStatusFlags is CAP_FCNTL, narrowed to F_GETFL by
    /// cap_fcntls_limit() (CommandsFor).
    constexpr auto CapabilityRights = std::array {
        CapabilityRight { .right = DescriptorRight::Read, .capability = CAP_READ },
        CapabilityRight { .right = DescriptorRight::Write, .capability = CAP_WRITE },
        CapabilityRight { .right = DescriptorRight::Receive, .capability = CAP_RECV },
        CapabilityRight { .right = DescriptorRight::Send, .capability = CAP_SEND },
        CapabilityRight { .right = DescriptorRight::Event, .capability = CAP_EVENT },
        CapabilityRight { .right = DescriptorRight::Shutdown, .capability = CAP_SHUTDOWN },
        CapabilityRight { .right = DescriptorRight::Fstat, .capability = CAP_FSTAT },
        CapabilityRight { .right = DescriptorRight::MapRead, .capability = CAP_MMAP_R },
        CapabilityRight { .right = DescriptorRight::Seek, .capability = CAP_SEEK },
        CapabilityRight { .right = DescriptorRight::GetStatusFlags, .capability = CAP_FCNTL },
    };

    static_assert(std::ranges::all_of(std::views::iota(std::size_t { 0 }, CapabilityRights.size()),
                                      [](std::size_t index) {
                                          return std::size_t {
                                              std::to_underlying(CapabilityRights[index].right)
                                          } == index;
                                      }),
                  "CapabilityRights lists every DescriptorRight at its own index");
    static_assert(CapabilityRights.back().right == DescriptorRight::GetStatusFlags,
                  "CapabilityRights ends at the last DescriptorRight");

    /// The fcntl(2) commands that need CAP_FCNTL a descriptor with GetStatusFlags may still issue.
    constexpr auto StatusFlagCommands = uint32_t { CAP_FCNTL_GETFL };

    /// @return Nothing if @p result is 0; @p step with errno otherwise.
    [[nodiscard]] std::expected<void, ConfinementFailure> stepOf(int result, remote::proto::LockdownStep step)
    {
        if (result != 0)
            return std::unexpected(ConfinementFailure { .step = step, .code = errno });
        return {};
    }

    /// @return The Capsicum rights of @p role.
    [[nodiscard]] cap_rights_t capabilitiesOf(DescriptorRole role)
    {
        auto capabilities = cap_rights_t {};
        cap_rights_init(&capabilities);
        for (auto const right: rightsOf(role))
            cap_rights_set(&capabilities, CapabilityRights[std::to_underlying(right)].capability);
        return capabilities;
    }

    /// @return The fcntl commands needing CAP_FCNTL that @p role may issue: F_GETFL where it has
    ///         GetStatusFlags, none otherwise (cap_rights_limit() clears them without CAP_FCNTL).
    [[nodiscard]] uint32_t fcntlsOf(DescriptorRole role)
    {
        auto const rights = rightsOf(role);
        return std::ranges::find(rights, DescriptorRight::GetStatusFlags) != rights.end() ? StatusFlagCommands
                                                                                          : uint32_t { 0 };
    }

    /// Limits @p descriptor to what its role uses it for: its rights, and of the fcntl commands only F_GETFL.
    /// No ioctl is left, as no role has CAP_IOCTL.
    [[nodiscard]] std::expected<void, ConfinementFailure> limit(HeldDescriptor descriptor)
    {
        auto const capabilities = capabilitiesOf(descriptor.role);
        auto const limited = stepOf(::cap_rights_limit(descriptor.handle, &capabilities),
                                    remote::proto::LockdownStep::DescriptorRights);
        auto const fcntls = fcntlsOf(descriptor.role);
        if (!limited || fcntls == 0)
            return limited;
        return stepOf(::cap_fcntls_limit(descriptor.handle, fcntls),
                      remote::proto::LockdownStep::DescriptorRights);
    }

    class FreeBsdSandbox final: public ISandbox
    {
      public:
        explicit FreeBsdSandbox(SandboxDescriptors descriptors): _descriptors { std::move(descriptors) } {}

        [[nodiscard]] std::expected<void, ConfinementFailure> restrictBeforeThreads() override
        {
#ifdef PROC_NO_NEW_PRIVS_CTL
            // Per process, and kept across fork: set once, it reaches every thread.
            auto enable = PROC_NO_NEW_PRIVS_ENABLE;
            return stepOf(::procctl(P_PID, 0, PROC_NO_NEW_PRIVS_CTL, &enable),
                          remote::proto::LockdownStep::NoNewPrivileges);
#else
            // Older than FreeBSD 14: capability mode refuses execve(2) all the same.
            return {};
#endif
        }

        [[nodiscard]] std::expected<void, ConfinementFailure> lockDown() override
        {
            // Every descriptor is limited, and capability mode entered, even past a step that failed; the
            // first that failed is the one stated. Limiting comes first: cap_rights_limit() works in
            // capability mode too, but a descriptor left unlimited would keep every right it was opened with.
            auto first = std::optional<ConfinementFailure> {};
            auto const note = [&first](std::expected<void, ConfinementFailure> const& step) {
                if (!step && !first)
                    first = step.error();
            };
            note(limit(HeldDescriptor { .role = DescriptorRole::Control, .handle = _descriptors.control }));
            for (auto const& descriptor: _descriptors.held)
                note(limit(descriptor));
            note(stepOf(::cap_enter(), remote::proto::LockdownStep::Capabilities));
            if (first)
                return std::unexpected(*first);
            return {};
        }

        [[nodiscard]] remote::proto::LockdownState lockdownState() const noexcept override
        {
            return remote::proto::LockdownState::LockedDown;
        }

      private:
        SandboxDescriptors _descriptors;
    };
} // namespace

DescriptorLimit limitOf(HeldDescriptor descriptor)
{
    auto held = cap_rights_t {};
    if (cap_rights_get(descriptor.handle, &held) != 0)
        return DescriptorLimit::Otherwise;
    auto every = cap_rights_t {};
    CAP_ALL(&every);
    if (::cap_rights_contains(&held, &every))
        return DescriptorLimit::Unlimited;
    auto const wanted = capabilitiesOf(descriptor.role);
    auto fcntls = uint32_t {};
    auto const asItsRole = ::cap_rights_contains(&held, &wanted) && ::cap_rights_contains(&wanted, &held)
                           && ::cap_fcntls_get(descriptor.handle, &fcntls) == 0
                           && fcntls == fcntlsOf(descriptor.role)
                           && ::cap_ioctls_get(descriptor.handle, nullptr, 0) == 0;
    return asItsRole ? DescriptorLimit::AsItsRole : DescriptorLimit::Otherwise;
}

std::unique_ptr<ISandbox> makeSystemSandbox(SandboxDescriptors descriptors, [[maybe_unused]] GuestCode code)
{
    // Capability mode leaves memory protection alone, so how the engine runs code changes nothing here.
    return std::make_unique<FreeBsdSandbox>(std::move(descriptors));
}

} // namespace vtextension::wasm::host
