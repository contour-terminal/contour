// SPDX-License-Identifier: Apache-2.0
//
// The extension host's sandbox on Linux. Before any thread: no_new_privs, which every thread started later
// inherits. Once every thread was started and prepared: the system call filter, installed on every thread at
// once.
#include <sys/prctl.h>

#include <cerrno>
#include <expected>
#include <memory>
#include <vector>

#include <unistd.h>

#include <vtextension/wasm/host/Sandbox.hpp>
#include <vtextension/wasm/host/linux/SeccompFilter.hpp>
#include <vtextension/wasm/host/linux/SystemCallRows.hpp>

namespace vtextension::wasm::host
{

namespace
{
    class LinuxSandbox final: public ISandbox
    {
      public:
        explicit LinuxSandbox(SandboxDescriptors descriptors): _descriptors { descriptors } {}

        [[nodiscard]] std::expected<void, ConfinementFailure> restrictBeforeThreads() override
        {
            if (::prctl(PR_SET_NO_NEW_PRIVS, 1, 0, 0, 0) != 0)
                return std::unexpected(ConfinementFailure {
                    .step = remote::proto::LockdownStep::NoNewPrivileges, .code = errno });
            return {};
        }

        [[nodiscard]] std::expected<void, ConfinementFailure> lockDown() override
        {
            auto const rows = buildRows(ThisAuditArch);
            return assembleFilter(rows, _descriptors, ::getpid(), ThisAuditArch)
                .and_then([](std::vector<sock_filter> const& program) { return installFilter(program); });
        }

        [[nodiscard]] remote::proto::LockdownState lockdownState() const noexcept override
        {
            return remote::proto::LockdownState::LockedDown;
        }

      private:
        SandboxDescriptors _descriptors;
    };
} // namespace

std::unique_ptr<ISandbox> makeSystemSandbox(SandboxDescriptors descriptors)
{
    return std::make_unique<LinuxSandbox>(descriptors);
}

} // namespace vtextension::wasm::host
