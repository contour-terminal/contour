// SPDX-License-Identifier: Apache-2.0
#include <vthost/ShellPtyFactory.hpp>

#include <optional>
#include <utility>

#include <vthost/HostedShell.hpp>

namespace vthost
{

ShellDevices ownShellDevices(std::shared_ptr<vtpty::ProcessPlacement> placement)
{
    return [placement = std::move(placement)](vtbackend::PageSize pageSize,
                                              vtpty::Process::ExecInfo exec,
                                              SandboxEscape sandbox) -> std::unique_ptr<vtpty::Pty> {
        return std::make_unique<vtpty::Process>(std::move(exec),
                                                vtpty::createPty(pageSize, std::nullopt),
                                                sandbox == SandboxEscape::Escapes,
                                                placement);
    };
}

PtyFactory makeShellPtyFactory(vtpty::Process::ExecInfo shell,
                               SandboxEscape sandbox,
                               std::filesystem::path const& socketPath,
                               ShellDevices devices)
{
    return
        [shell = std::move(shell),
         hosted = hostedShellEnvironment(socketPath),
         sandbox,
         devices = std::move(devices)](
            vtbackend::PageSize pageSize,
            std::optional<vtpty::Process::ExecInfo> const& commandOverride) -> std::unique_ptr<vtpty::Pty> {
            // A fresh shell per spawn: `shell` is captured by the closure and reused for every session this
            // factory spawns, so writing one session's override back into it would leak that pane's
            // command/directory into every later session. Mirrors AppSessionFactory::createPty.
            return devices(
                pageSize, hostedShellFor(shell, commandOverride, ShellOverride::Overlays, hosted), sandbox);
        };
}

ProfilePtyFactory makeProfilePtyFactory(vtpty::Process::ExecInfo shell,
                                        std::filesystem::path const& socketPath,
                                        ShellDevices devices)
{
    return [base = std::move(shell),
            hosted = hostedShellEnvironment(socketPath),
            devices = std::move(devices)](vtbackend::PageSize pageSize,
                                          vtpty::Process::ExecInfo const& profileShell,
                                          SandboxEscape sandbox) -> std::unique_ptr<vtpty::Pty> {
        return devices(
            pageSize, hostedShellFor(base, profileShell, ShellOverride::Replaces, hosted), sandbox);
    };
}

} // namespace vthost
