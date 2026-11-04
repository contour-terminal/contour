// SPDX-License-Identifier: Apache-2.0
#include <contour/platform/ExecutionPolicySource.hpp>

#include <algorithm>
#include <array>
#include <cstdint>
#include <optional>
#include <ranges>
#include <span>
#include <string_view>
#include <vector>

namespace contour::platform
{

namespace
{

    using config::ExecutionPolicy;
    using config::PolicyLayer;
    using config::PolicyScope;
    using config::ShellKind;

    /// How a layer's registry key spells its policy.
    enum class PolicySpelling : uint8_t
    {
        /// The `ExecutionPolicy` string alone, as `Set-ExecutionPolicy` writes it.
        Named = 0,
        /// Group policy: the `EnableScripts` DWORD, 0 for no scripts at all, 1 for the policy the
        /// `ExecutionPolicy` string names.
        GroupPolicy,
    };

    /// One place a policy can be set, and the scope it is in effect at.
    struct LayerSource
    {
        PolicyScope scope;
        RegistryRoot root;
        std::string_view key;
        PolicySpelling spelling;
    };

    /// Where a setting is written: its hive, key, value name and data.
    struct RegistrySetting
    {
        RegistryRoot root;
        std::string_view key;
        std::string_view value;
        std::string_view data;
    };

    constexpr auto ExecutionPolicyValue = std::string_view { "ExecutionPolicy" };
    constexpr auto EnableScriptsValue = std::string_view { "EnableScripts" };
    constexpr auto ShellIdsKey =
        std::string_view { "SOFTWARE\\Microsoft\\PowerShell\\1\\ShellIds\\Microsoft.PowerShell" };
    constexpr auto WindowsPolicyKey =
        std::string_view { "SOFTWARE\\Policies\\Microsoft\\Windows\\PowerShell" };
    constexpr auto CorePolicyKey = std::string_view { "SOFTWARE\\Policies\\Microsoft\\PowerShellCore" };

    /// The edition of Windows: `InstallationType` is `Server` on Windows Server.
    constexpr auto InstallationType = RegistrySetting {
        .root = RegistryRoot::LocalMachine,
        .key = "SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion",
        .value = "InstallationType",
        .data = "Server",
    };

    /// Windows PowerShell's layers, strongest first.
    constexpr auto WindowsPowerShellLayers = std::array {
        LayerSource { .scope = PolicyScope::GroupPolicy,
                      .root = RegistryRoot::LocalMachine,
                      .key = WindowsPolicyKey,
                      .spelling = PolicySpelling::GroupPolicy },
        LayerSource { .scope = PolicyScope::GroupPolicy,
                      .root = RegistryRoot::CurrentUser,
                      .key = WindowsPolicyKey,
                      .spelling = PolicySpelling::GroupPolicy },
        LayerSource { .scope = PolicyScope::CurrentUser,
                      .root = RegistryRoot::CurrentUser,
                      .key = ShellIdsKey,
                      .spelling = PolicySpelling::Named },
        LayerSource { .scope = PolicyScope::LocalMachine,
                      .root = RegistryRoot::LocalMachine,
                      .key = ShellIdsKey,
                      .spelling = PolicySpelling::Named },
    };

    /// PowerShell's layers in the registry, strongest first: its other scopes live in JSON files.
    constexpr auto PwshLayers = std::array {
        LayerSource { .scope = PolicyScope::GroupPolicy,
                      .root = RegistryRoot::LocalMachine,
                      .key = CorePolicyKey,
                      .spelling = PolicySpelling::GroupPolicy },
        LayerSource { .scope = PolicyScope::GroupPolicy,
                      .root = RegistryRoot::CurrentUser,
                      .key = CorePolicyKey,
                      .spelling = PolicySpelling::GroupPolicy },
    };

    /// What one shell's policy is read from, and what Contour writes to allow the user's scripts.
    struct ShellPolicy
    {
        ShellKind shell;
        std::span<LayerSource const> layers;
        ExecutionPolicy clientFallback; ///< The policy when no layer sets one, on a client Windows.
        ExecutionPolicy serverFallback; ///< The policy when no layer sets one, on Windows Server.
        std::optional<RegistrySetting> allowUserScripts;
    };

    constexpr auto ShellPolicies = std::array {
        ShellPolicy { .shell = ShellKind::WindowsPowerShell,
                      .layers = WindowsPowerShellLayers,
                      .clientFallback = ExecutionPolicy::Restricted,
                      .serverFallback = ExecutionPolicy::RemoteSigned,
                      .allowUserScripts = RegistrySetting { .root = RegistryRoot::CurrentUser,
                                                            .key = ShellIdsKey,
                                                            .value = ExecutionPolicyValue,
                                                            .data = "RemoteSigned" } },
        ShellPolicy { .shell = ShellKind::Pwsh,
                      .layers = PwshLayers,
                      .clientFallback = ExecutionPolicy::RemoteSigned,
                      .serverFallback = ExecutionPolicy::RemoteSigned,
                      .allowUserScripts = std::nullopt },
    };

    /// A shell with no execution policy: nothing to read, every script runs.
    constexpr auto NoPolicy = ShellPolicy { .shell = ShellKind::Unknown,
                                            .layers = {},
                                            .clientFallback = ExecutionPolicy::Unrestricted,
                                            .serverFallback = ExecutionPolicy::Unrestricted,
                                            .allowUserScripts = std::nullopt };

    [[nodiscard]] ShellPolicy const& shellPolicyOf(ShellKind shell) noexcept
    {
        auto const row = std::ranges::find(ShellPolicies, shell, &ShellPolicy::shell);
        return row == ShellPolicies.end() ? NoPolicy : *row;
    }

    /// @return The policy the `ExecutionPolicy` string under @p key names; Undefined when absent or no
    /// policy.
    [[nodiscard]] ExecutionPolicy namedPolicy(IRegistry const& registry,
                                              RegistryRoot root,
                                              std::string_view key)
    {
        return registry.readString(root, key, ExecutionPolicyValue)
            .and_then([](std::string const& name) { return config::executionPolicyNamed(name); })
            .value_or(ExecutionPolicy::Undefined);
    }

    /// @return What @p source says about the policy.
    [[nodiscard]] PolicyLayer readLayer(IRegistry const& registry, LayerSource const& source)
    {
        auto const policy = [&] {
            switch (source.spelling)
            {
                case PolicySpelling::Named: return namedPolicy(registry, source.root, source.key);
                case PolicySpelling::GroupPolicy:
                    switch (registry.readDword(source.root, source.key, EnableScriptsValue).value_or(~0U))
                    {
                        case 0: return ExecutionPolicy::Restricted;
                        case 1: return namedPolicy(registry, source.root, source.key);
                        default: return ExecutionPolicy::Undefined;
                    }
            }
            return ExecutionPolicy::Undefined;
        }();
        return PolicyLayer { .scope = source.scope, .policy = policy };
    }

    /// @return Whether this Windows is Windows Server.
    [[nodiscard]] bool isServer(IRegistry const& registry)
    {
        return registry.readString(InstallationType.root, InstallationType.key, InstallationType.value)
               == InstallationType.data;
    }

} // namespace

config::EffectivePolicy RegistryExecutionPolicySource::effectivePolicy(config::ShellKind shell) const
{
    auto const& row = shellPolicyOf(shell);
    auto const layers =
        row.layers
        | std::views::transform([this](LayerSource const& source) { return readLayer(_registry, source); })
        | std::ranges::to<std::vector>();
    // Only Windows PowerShell's default depends on the edition; ask the registry only when it matters.
    auto const fallback = row.clientFallback == row.serverFallback || !isServer(_registry)
                              ? row.clientFallback
                              : row.serverFallback;
    return config::effectivePolicyOf(layers, fallback);
}

std::expected<void, std::string> RegistryExecutionPolicySource::allowUserScripts(config::ShellKind shell)
{
    auto const& setting = shellPolicyOf(shell).allowUserScripts;
    if (!setting)
        return std::unexpected(std::string { "this shell has no user execution policy to set" });
    return _registry.writeString(setting->root, setting->key, setting->value, setting->data);
}

std::unique_ptr<IExecutionPolicySource> makePlatformExecutionPolicySource(
    [[maybe_unused]] IRegistry& registry)
{
#ifdef _WIN32
    return std::make_unique<RegistryExecutionPolicySource>(registry);
#else
    return std::make_unique<NoExecutionPolicySource>();
#endif
}

} // namespace contour::platform
