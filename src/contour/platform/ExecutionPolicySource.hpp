// SPDX-License-Identifier: Apache-2.0
#pragma once

/// @file
/// Where PowerShell's execution policy is read from, and the one change Contour makes to it: allowing
/// the user's own scripts. Behind an interface so that the decision built on it is testable with any
/// policy, on any platform. The interface is Qt-free; the implementation reads PowerShell 7's JSON
/// configuration files with Qt's JSON parser.

#include <contour/config/ExecutionPolicy.hpp>
#include <contour/config/ShellKind.hpp>
#include <contour/platform/KnownFolders.hpp>
#include <contour/platform/Registry.hpp>

#include <core/platform/FileSystem.hpp>

#include <expected>
#include <memory>
#include <string>

namespace contour::platform
{

/// The execution policy a shell runs scripts under.
class IExecutionPolicySource
{
  public:
    virtual ~IExecutionPolicySource() = default;

    /// The policy in force for @p shell.
    /// @param shell The shell whose profile would run.
    /// @return The policy and the scope that sets it; `{Unrestricted, Default}` for a shell that has no
    ///         execution policy.
    [[nodiscard]] virtual config::EffectivePolicy effectivePolicy(config::ShellKind shell) const = 0;

    /// Lets the user's own scripts run in @p shell: `Set-ExecutionPolicy -Scope CurrentUser RemoteSigned`.
    /// @param shell The shell to allow scripts in.
    /// @return Nothing on success, else why the policy could not be changed, or that @p shell has no
    ///         user-scope policy this can set.
    [[nodiscard]] virtual std::expected<void, std::string> allowUserScripts(config::ShellKind shell) = 0;
};

/// PowerShell's execution policy as Windows PowerShell and PowerShell read it: from the registry, and
/// PowerShell 7's own scopes from its `powershell.config.json` files.
///
/// Windows PowerShell: group policy (machine, then user), then the user's and then the machine's
/// `ShellIds` setting; failing all, `RemoteSigned` on Windows Server and `Restricted` elsewhere.
/// PowerShell 7: its group policy (machine, then user), then `Microsoft.PowerShell:ExecutionPolicy` in the
/// user's `Documents\PowerShell\powershell.config.json`, then in the one beside an installed PowerShell 7
/// (the first `InstallLocation` under `HKLM\SOFTWARE\Microsoft\PowerShellCore\InstalledVersions` that has
/// a readable one); failing all, `RemoteSigned`. A file that is not a JSON object sets no policy.
class RegistryExecutionPolicySource final: public IExecutionPolicySource
{
  public:
    /// @param registry The registry to read and write; it must outlive this source.
    /// @param fileSystem Where PowerShell 7's configuration files are read and written; it must outlive
    ///        this source.
    /// @param knownFolders Where the user's Documents folder is; it must outlive this source.
    RegistryExecutionPolicySource(IRegistry& registry,
                                  core::platform::FileSystem const& fileSystem,
                                  IKnownFolders const& knownFolders) noexcept:
        _registry { registry }, _fileSystem { fileSystem }, _knownFolders { knownFolders }
    {
    }

    /// @copydoc IExecutionPolicySource::effectivePolicy
    [[nodiscard]] config::EffectivePolicy effectivePolicy(config::ShellKind shell) const override;

    /// Sets `RemoteSigned` as the user's own policy: Windows PowerShell's user-scope `ExecutionPolicy` in
    /// the registry, or `Microsoft.PowerShell:ExecutionPolicy` in PowerShell 7's user
    /// `powershell.config.json`. That file keeps every other key; it and its folder are created when
    /// missing, and a file that is not a JSON object is left as it is and reported.
    /// @param shell The shell to allow scripts in.
    /// @return Nothing on success, else why the setting could not be written, or that @p shell has none.
    [[nodiscard]] std::expected<void, std::string> allowUserScripts(config::ShellKind shell) override;

  private:
    IRegistry& _registry;
    core::platform::FileSystem const& _fileSystem;
    IKnownFolders const& _knownFolders;
};

/// The execution policy of a platform where no shell has one: every script may run.
class NoExecutionPolicySource final: public IExecutionPolicySource
{
  public:
    /// @return Always `{Unrestricted, Default}`.
    [[nodiscard]] config::EffectivePolicy effectivePolicy(config::ShellKind /*shell*/) const override
    {
        return { .policy = config::ExecutionPolicy::Unrestricted, .scope = config::PolicyScope::Default };
    }

    /// @return Always an error: there is no policy to change.
    [[nodiscard]] std::expected<void, std::string> allowUserScripts(config::ShellKind /*shell*/) override
    {
        return std::unexpected(std::string { "no execution policy on this platform" });
    }
};

/// The platform's execution-policy source.
/// @param registry The registry it reads on Windows; it must outlive the source.
/// @param fileSystem Where it reads PowerShell 7's configuration files on Windows; it must outlive the
/// source.
/// @param knownFolders Where it finds the user's Documents folder on Windows; it must outlive the source.
/// @return On Windows, a RegistryExecutionPolicySource over them; elsewhere a NoExecutionPolicySource, which
///         reads nothing.
[[nodiscard]] std::unique_ptr<IExecutionPolicySource> makePlatformExecutionPolicySource(
    IRegistry& registry, core::platform::FileSystem const& fileSystem, IKnownFolders const& knownFolders);

} // namespace contour::platform
