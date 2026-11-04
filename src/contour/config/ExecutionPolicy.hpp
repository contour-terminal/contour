// SPDX-License-Identifier: Apache-2.0
#pragma once

/// @file
/// PowerShell's execution policies, and whether one lets a profile script run.
/// Pure and Qt-free: reading the policy from the registry or a process is somebody else's job.

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string_view>
#include <utility>

namespace contour::config
{

/// A PowerShell execution policy.
enum class ExecutionPolicy : uint8_t
{
    Undefined = 0, ///< Not set at that scope.
    Restricted,
    AllSigned,
    RemoteSigned,
    Unrestricted,
    Bypass,
};

/// Where an execution policy is set, from weakest to strongest in effect.
enum class PolicyScope : uint8_t
{
    Default = 0, ///< Nothing sets it; PowerShell's built-in default applies.
    LocalMachine,
    CurrentUser,
    GroupPolicy, ///< Set by the organisation; the user cannot override it.
};

/// The policy in force, and the scope that decides it.
struct EffectivePolicy
{
    ExecutionPolicy policy = ExecutionPolicy::Unrestricted;
    PolicyScope scope = PolicyScope::Default;

    /// @return Whether both members are equal.
    [[nodiscard]] bool operator==(EffectivePolicy const&) const = default;
};

/// What one scope says about the policy.
struct PolicyLayer
{
    PolicyScope scope = PolicyScope::Default;
    ExecutionPolicy policy = ExecutionPolicy::Undefined;
};

/// Whether a profile script runs under a policy, and if not, what would change that.
enum class ProfileScripts : uint8_t
{
    Run = 0,              ///< The profile runs.
    NeedUserPolicy,       ///< It does not, but the user can allow scripts for their own account.
    BlockedByGroupPolicy, ///< It does not, and the organisation's policy forbids changing that.
    SignedOnly,           ///< It runs only when signed, which an appended line is not.
};

/// The policy a name spells.
/// @param name A policy name, in any ASCII case.
/// @return The policy, or nullopt for `Default` and anything that is not a policy.
[[nodiscard]] constexpr std::optional<ExecutionPolicy> executionPolicyNamed(std::string_view name) noexcept
{
    constexpr auto names = std::array {
        std::pair { std::string_view { "Undefined" }, ExecutionPolicy::Undefined },
        std::pair { std::string_view { "Restricted" }, ExecutionPolicy::Restricted },
        std::pair { std::string_view { "AllSigned" }, ExecutionPolicy::AllSigned },
        std::pair { std::string_view { "RemoteSigned" }, ExecutionPolicy::RemoteSigned },
        std::pair { std::string_view { "Unrestricted" }, ExecutionPolicy::Unrestricted },
        std::pair { std::string_view { "Bypass" }, ExecutionPolicy::Bypass },
    };
    constexpr auto lower = [](char c) noexcept {
        return c >= 'A' && c <= 'Z' ? static_cast<char>(c + 32) : c;
    };
    for (auto const& [spelling, policy]: names)
        if (std::ranges::equal(spelling, name, {}, lower, lower))
            return policy;
    return std::nullopt;
}

/// The policy PowerShell enforces.
/// @param byPrecedence The scopes' settings, strongest first.
/// @param fallback What applies when no scope sets one.
/// @return The first layer that sets a policy, else @p fallback at PolicyScope::Default.
[[nodiscard]] constexpr EffectivePolicy effectivePolicyOf(std::span<PolicyLayer const> byPrecedence,
                                                          ExecutionPolicy fallback) noexcept
{
    auto const set = std::ranges::find_if(
        byPrecedence, [](PolicyLayer const& layer) { return layer.policy != ExecutionPolicy::Undefined; });
    if (set == byPrecedence.end())
        return { .policy = fallback, .scope = PolicyScope::Default };
    return { .policy = set->policy, .scope = set->scope };
}

/// Whether a profile script runs under @p policy.
/// @param policy The policy in force.
/// @return What happens to an unsigned profile, and who can change it.
[[nodiscard]] constexpr ProfileScripts profileScriptsUnder(EffectivePolicy policy) noexcept
{
    /// One row per ExecutionPolicy, in enumerator order.
    struct Row
    {
        ProfileScripts underGroupPolicy;
        ProfileScripts underOtherScope;
    };
    constexpr auto rows = std::array {
        Row { ProfileScripts::BlockedByGroupPolicy, ProfileScripts::NeedUserPolicy }, // Undefined
        Row { ProfileScripts::BlockedByGroupPolicy, ProfileScripts::NeedUserPolicy }, // Restricted
        Row { ProfileScripts::BlockedByGroupPolicy, ProfileScripts::SignedOnly },     // AllSigned
        Row { ProfileScripts::Run, ProfileScripts::Run },                             // RemoteSigned
        Row { ProfileScripts::Run, ProfileScripts::Run },                             // Unrestricted
        Row { ProfileScripts::Run, ProfileScripts::Run },                             // Bypass
    };
    auto const& row = rows[static_cast<std::size_t>(policy.policy)];
    return policy.scope == PolicyScope::GroupPolicy ? row.underGroupPolicy : row.underOtherScope;
}

} // namespace contour::config
