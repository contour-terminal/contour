// SPDX-License-Identifier: Apache-2.0
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <ranges>
#include <string>
#include <string_view>
#include <vector>

#include <orchestrator/core/Permissions.hpp>

using namespace orchestrator;
using namespace std::string_view_literals;

namespace
{
[[nodiscard]] PermissionProfile const& named(std::vector<PermissionProfile> const& profiles,
                                             std::string_view name)
{
    auto const profile = std::ranges::find(profiles, name, &PermissionProfile::name);
    REQUIRE(profile != profiles.end());
    return *profile;
}
} // namespace

TEST_CASE("Permissions: what a rule may look like", "[orchestrator][permissions]")
{
    struct Row
    {
        std::string_view rule;
        bool accepted;
    };
    auto const rows = std::vector<Row> {
        { .rule = "Read", .accepted = true },
        { .rule = "Bash(git push:*)", .accepted = true },
        { .rule = "mcp__server__tool", .accepted = true },
        { .rule = "Skill(contour-workflows:*)", .accepted = true },
        { .rule = "WebFetch(domain:github.com)", .accepted = true },
        { .rule = "", .accepted = false },
        { .rule = "Bash()", .accepted = false },
        { .rule = "Bash(a(b))", .accepted = false },
        { .rule = "1Bash", .accepted = false },
        { .rule = "Bash(x", .accepted = false },
        { .rule = "Bash x", .accepted = false },
        { .rule = "Bash(a\nb)", .accepted = false },
    };
    for (auto const& row: rows)
    {
        INFO(row.rule);
        CHECK(isPermissionRule(row.rule) == row.accepted);
    }
    for (auto const& profile: builtinProfiles())
    {
        INFO(profile.name);
        CHECK(std::ranges::all_of(profile.allow, isPermissionRule));
        CHECK(std::ranges::all_of(profile.deny, isPermissionRule));
    }
}

TEST_CASE("Permissions: modes are spelt as Claude Code spells them", "[orchestrator][permissions]")
{
    CHECK(permissionModeOf("acceptEdits") == PermissionMode::AcceptEdits);
    CHECK(permissionModeOf("default") == PermissionMode::Default);
    CHECK(permissionModeOf("plan") == PermissionMode::Plan);
    CHECK_FALSE(permissionModeOf(RefusedPermissionMode).has_value());
    CHECK_FALSE(permissionModeOf("AcceptEdits").has_value());
    for (auto const& row: PermissionModeNames)
        CHECK(nameOf(row.mode) == row.word);
}

TEST_CASE("Permissions: the built-in profiles resolve", "[orchestrator][permissions]")
{
    auto const profiles = resolveProfiles({});
    REQUIRE(profiles.has_value());
    REQUIRE(profiles->size() == 3);

    auto const& standard = named(*profiles, DefaultProfileName);
    CHECK(standard.mode == PermissionMode::AcceptEdits);
    CHECK(standard.allow.size() == 19);
    CHECK(standard.deny.size() == 4);
    CHECK_FALSE(standard.extends.has_value());

    auto const& trusted = named(*profiles, "trusted");
    CHECK(trusted.mode == PermissionMode::AcceptEdits);
    CHECK(trusted.allow.size() == standard.allow.size() + 3);
    CHECK(std::ranges::equal(trusted.allow | std::views::take(standard.allow.size()), standard.allow));
    CHECK(trusted.allow.back() == "Bash(gh pr edit:*)");
    CHECK(trusted.deny == standard.deny);
    CHECK_FALSE(trusted.extends.has_value());

    auto const& ask = named(*profiles, "ask");
    CHECK(ask.mode == PermissionMode::Default);
    CHECK(ask.allow.empty());
    CHECK(ask.deny.empty());
}

TEST_CASE("Permissions: a configured profile replaces a built-in one, and others build on it",
          "[orchestrator][permissions]")
{
    auto const configured = std::vector<PermissionProfile> {
        PermissionProfile { .name = "standard", .mode = PermissionMode::Plan, .allow = { "Read" } },
        PermissionProfile {
            .name = "careful", .extends = "trusted", .deny = { "Bash(git commit:*)", "Read" } },
    };
    auto const profiles = resolveProfiles(configured);
    REQUIRE(profiles.has_value());
    REQUIRE(profiles->size() == 4);
    CHECK(named(*profiles, "trusted").allow
          == std::vector<std::string> {
              "Read", "Bash(git push:*)", "Bash(gh pr create:*)", "Bash(gh pr edit:*)" });
    CHECK(named(*profiles, "trusted").mode == PermissionMode::Plan);

    auto const& careful = named(*profiles, "careful");
    CHECK(careful.mode == PermissionMode::Plan);
    CHECK(careful.deny == std::vector<std::string> { "Bash(git commit:*)", "Read" });
    CHECK(careful.allow.size() == 4);
}

TEST_CASE("Permissions: an unknown or circular extends is refused", "[orchestrator][permissions]")
{
    auto const cycle = std::vector<PermissionProfile> {
        PermissionProfile { .name = "a", .extends = "b" },
        PermissionProfile { .name = "b", .extends = "a" },
    };
    CHECK(resolveProfiles(cycle).error()
          == ProfileProblem { .profile = "a", .error = ProfileError::ProfileCycle });

    auto const self = std::vector<PermissionProfile> { PermissionProfile { .name = "s", .extends = "s" } };
    CHECK(resolveProfiles(self).error()
          == ProfileProblem { .profile = "s", .error = ProfileError::ProfileCycle });

    auto const unknown =
        std::vector<PermissionProfile> { PermissionProfile { .name = "x", .extends = "nope" } };
    CHECK(resolveProfiles(unknown).error()
          == ProfileProblem { .profile = "x", .error = ProfileError::UnknownProfile });

    // A built-in profile replaced so that it builds on one built on it.
    auto const builtinCycle =
        std::vector<PermissionProfile> { PermissionProfile { .name = "standard", .extends = "trusted" } };
    CHECK(resolveProfiles(builtinCycle).error()
          == ProfileProblem { .profile = "standard", .error = ProfileError::ProfileCycle });
}

TEST_CASE("Permissions: the settings Claude Code takes", "[orchestrator][permissions]")
{
    auto const profiles = resolveProfiles({});
    REQUIRE(profiles.has_value());
    CHECK(settingsJsonOf(named(*profiles, "ask"))
          == R"({"permissions":{"defaultMode":"default","allow":[],"deny":[]}})");

    auto const& standard = named(*profiles, DefaultProfileName);
    auto const json = settingsJsonOf(standard);
    CHECK(json.starts_with(
        R"js({"permissions":{"defaultMode":"acceptEdits","allow":["Bash(git status:*)",)js"));
    CHECK(summaryOfSettings(json)
          == SettingsSummary { .mode = PermissionMode::AcceptEdits, .allowed = 19, .denied = 4 });

    CHECK_FALSE(summaryOfSettings("").has_value());
    CHECK_FALSE(summaryOfSettings("{}").has_value());
    CHECK_FALSE(
        summaryOfSettings(R"({"permissions":{"defaultMode":"bypassPermissions","allow":[],"deny":[]}})")
            .has_value());
}
