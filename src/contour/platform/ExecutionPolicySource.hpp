// SPDX-License-Identifier: Apache-2.0
#pragma once

/// @file
/// Where PowerShell's execution policy is read from, and the one change Contour makes to it: allowing
/// the user's own scripts. Behind an interface so that the decision built on it is testable with any
/// policy, on any platform. Qt-free.

#include <contour/config/ExecutionPolicy.hpp>
#include <contour/config/ShellKind.hpp>
#include <contour/platform/Registry.hpp>

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

/// PowerShell's execution policy as Windows PowerShell and PowerShell read it from the registry.
///
/// Windows PowerShell: group policy (machine, then user), then the user's and then the machine's
/// `ShellIds` setting; failing all, `RemoteSigned` on Windows Server and `Restricted` elsewhere.
/// PowerShell 7 keeps its non-policy settings in JSON files, not the registry, so only its group policy
/// is read here; failing that, its default `RemoteSigned` applies.
class RegistryExecutionPolicySource final: public IExecutionPolicySource
{
  public:
    /// @param registry The registry to read and write; it must outlive this source.
    explicit RegistryExecutionPolicySource(IRegistry& registry) noexcept: _registry { registry } {}

    /// @copydoc IExecutionPolicySource::effectivePolicy
    [[nodiscard]] config::EffectivePolicy effectivePolicy(config::ShellKind shell) const override;

    /// Writes `RemoteSigned` to Windows PowerShell's user-scope `ExecutionPolicy`.
    /// @param shell The shell to allow scripts in; only config::ShellKind::WindowsPowerShell has a
    ///        user-scope policy in the registry.
    /// @return Nothing on success, else why the value could not be written, or that @p shell has none.
    [[nodiscard]] std::expected<void, std::string> allowUserScripts(config::ShellKind shell) override;

  private:
    IRegistry& _registry;
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
/// @return On Windows, a RegistryExecutionPolicySource over @p registry; elsewhere a NoExecutionPolicySource.
[[nodiscard]] std::unique_ptr<IExecutionPolicySource> makePlatformExecutionPolicySource(IRegistry& registry);

} // namespace contour::platform
