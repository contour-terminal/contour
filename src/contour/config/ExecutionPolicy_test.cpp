// SPDX-License-Identifier: Apache-2.0
#include <contour/config/ExecutionPolicy.hpp>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <optional>

using namespace contour::config;

TEST_CASE("ExecutionPolicy.named", "[shells]")
{
    CHECK(executionPolicyNamed("remotesigned") == ExecutionPolicy::RemoteSigned);
    CHECK(executionPolicyNamed("RESTRICTED") == ExecutionPolicy::Restricted);
    CHECK(executionPolicyNamed("AllSigned") == ExecutionPolicy::AllSigned);
    CHECK(executionPolicyNamed("Unrestricted") == ExecutionPolicy::Unrestricted);
    CHECK(executionPolicyNamed("Bypass") == ExecutionPolicy::Bypass);
    CHECK(executionPolicyNamed("Undefined") == ExecutionPolicy::Undefined);
    CHECK_FALSE(executionPolicyNamed("Default").has_value());
    CHECK_FALSE(executionPolicyNamed("bogus").has_value());
    CHECK_FALSE(executionPolicyNamed("").has_value());
}

TEST_CASE("ExecutionPolicy.effective", "[shells]")
{
    auto const layers = std::array {
        PolicyLayer { .scope = PolicyScope::GroupPolicy, .policy = ExecutionPolicy::Undefined },
        PolicyLayer { .scope = PolicyScope::CurrentUser, .policy = ExecutionPolicy::RemoteSigned },
        PolicyLayer { .scope = PolicyScope::LocalMachine, .policy = ExecutionPolicy::Restricted },
    };
    CHECK(effectivePolicyOf(layers, ExecutionPolicy::Restricted)
          == EffectivePolicy { .policy = ExecutionPolicy::RemoteSigned, .scope = PolicyScope::CurrentUser });

    auto const undefined = std::array {
        PolicyLayer { .scope = PolicyScope::GroupPolicy },
        PolicyLayer { .scope = PolicyScope::CurrentUser },
    };
    CHECK(effectivePolicyOf(undefined, ExecutionPolicy::Restricted)
          == EffectivePolicy { .policy = ExecutionPolicy::Restricted, .scope = PolicyScope::Default });
    CHECK(effectivePolicyOf({}, ExecutionPolicy::Unrestricted)
          == EffectivePolicy { .policy = ExecutionPolicy::Unrestricted, .scope = PolicyScope::Default });
}

TEST_CASE("ExecutionPolicy.profileScripts", "[shells]")
{
    struct Cell
    {
        ExecutionPolicy policy;
        ProfileScripts underGroupPolicy;
        ProfileScripts underOtherScope;
    };
    constexpr auto cells = std::array {
        Cell { ExecutionPolicy::Undefined,
               ProfileScripts::BlockedByGroupPolicy,
               ProfileScripts::NeedUserPolicy },
        Cell { ExecutionPolicy::Restricted,
               ProfileScripts::BlockedByGroupPolicy,
               ProfileScripts::NeedUserPolicy },
        Cell { ExecutionPolicy::AllSigned, ProfileScripts::BlockedByGroupPolicy, ProfileScripts::SignedOnly },
        Cell { ExecutionPolicy::RemoteSigned, ProfileScripts::Run, ProfileScripts::Run },
        Cell { ExecutionPolicy::Unrestricted, ProfileScripts::Run, ProfileScripts::Run },
        Cell { ExecutionPolicy::Bypass, ProfileScripts::Run, ProfileScripts::Run },
    };
    for (auto const& cell: cells)
    {
        CHECK(profileScriptsUnder({ cell.policy, PolicyScope::GroupPolicy }) == cell.underGroupPolicy);
        for (auto const scope: { PolicyScope::Default, PolicyScope::LocalMachine, PolicyScope::CurrentUser })
            CHECK(profileScriptsUnder({ cell.policy, scope }) == cell.underOtherScope);
    }
    CHECK(profileScriptsUnder({ ExecutionPolicy::Restricted, PolicyScope::Default })
          == ProfileScripts::NeedUserPolicy);
    CHECK(profileScriptsUnder({ ExecutionPolicy::AllSigned, PolicyScope::LocalMachine })
          == ProfileScripts::SignedOnly);
}
