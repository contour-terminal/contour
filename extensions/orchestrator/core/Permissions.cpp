// SPDX-License-Identifier: Apache-2.0
#include <orchestrator/core/Permissions.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <expected>
#include <format>
#include <optional>
#include <ranges>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include <glaze/glaze.hpp>
#include <orchestrator/core/ArgumentKinds.hpp>
#include <orchestrator/core/Json.hpp>
#include <orchestrator/core/PermissionsJson.hpp>

namespace orchestrator
{

namespace
{
    using namespace std::string_view_literals;
    using permissions::detail::JsonSettings;

    /// The input is a string_view: nothing terminates it. A run's settings were written by settingsJsonOf().
    constexpr auto ReadOptions = glz::opts { .null_terminated = false };

    /// What `standard` allows: editing, building, testing and committing in the worktree, and reading the
    /// forge. Pushing and opening a pull request still ask.
    constexpr auto StandardAllow = std::array {
        "Bash(git status:*)"sv,
        "Bash(git diff:*)"sv,
        "Bash(git log:*)"sv,
        "Bash(git show:*)"sv,
        "Bash(git add:*)"sv,
        "Bash(git commit:*)"sv,
        "Bash(git switch:*)"sv,
        "Bash(git rebase:*)"sv,
        "Bash(git fetch:*)"sv,
        "Bash(cmake:*)"sv,
        "Bash(ctest:*)"sv,
        "Bash(gh pr view:*)"sv,
        "Bash(gh pr checks:*)"sv,
        "Bash(gh run view:*)"sv,
        "Bash(gh issue view:*)"sv,
        "Skill(contour-workflows:*)"sv,
        "Read"sv,
        "Grep"sv,
        "Glob"sv,
    };

    /// What `standard` refuses outright, whatever a profile built on it allows.
    constexpr auto StandardDeny = std::array {
        "Bash(sudo:*)"sv,
        "Bash(rm -rf:*)"sv,
        "Bash(git push --force:*)"sv,
        "Bash(git reset --hard:*)"sv,
    };

    /// What `trusted` adds to `standard`: publishing the agent's own branch.
    constexpr auto TrustedAllow = std::array {
        "Bash(git push:*)"sv,
        "Bash(gh pr create:*)"sv,
        "Bash(gh pr edit:*)"sv,
    };

    constexpr auto BuiltinProfiles = std::array {
        BuiltinProfile { .name = DefaultProfileName,
                         .extends = StandardProfileName,
                         .mode = PermissionMode::Auto,
                         .allow = {},
                         .deny = {} },
        BuiltinProfile { .name = StandardProfileName,
                         .extends = {},
                         .mode = PermissionMode::AcceptEdits,
                         .allow = StandardAllow,
                         .deny = StandardDeny },
        BuiltinProfile { .name = "trusted",
                         .extends = StandardProfileName,
                         .mode = std::nullopt,
                         .allow = TrustedAllow,
                         .deny = {} },
        BuiltinProfile {
            .name = "ask", .extends = {}, .mode = PermissionMode::Default, .allow = {}, .deny = {} },
    };

    [[nodiscard]] std::vector<std::string> stringsOf(std::span<std::string_view const> rules)
    {
        return rules | std::views::transform([](std::string_view rule) { return std::string { rule }; })
               | std::ranges::to<std::vector>();
    }

    [[nodiscard]] ProfileDef definitionOf(BuiltinProfile const& row)
    {
        return ProfileDef { .name = std::string { row.name },
                            .extends = row.extends.empty() ? std::nullopt
                                                           : std::optional { std::string { row.extends } },
                            .mode = row.mode,
                            .allow = stringsOf(row.allow),
                            .deny = stringsOf(row.deny) };
    }

    [[nodiscard]] bool isControl(char c) noexcept
    {
        return static_cast<unsigned char>(c) < 0x20 || c == '\x7f';
    }

    /// Appends to @p rules those of @p more it lacks, in order.
    void appendMissing(std::vector<std::string>& rules, std::span<std::string const> more)
    {
        for (auto const& rule: more)
            if (!std::ranges::contains(rules, rule))
                rules.push_back(rule);
    }

    /// Resolves @p profile among @p profiles, walking its `extends`; a walk longer than there are profiles
    /// went round a cycle.
    [[nodiscard]] std::expected<PermissionProfile, ProfileError> resolveOne(
        std::span<ProfileDef const> profiles, ProfileDef const& profile, std::size_t depth = 0)
    {
        if (depth > profiles.size())
            return std::unexpected(ProfileError::ProfileCycle);
        auto resolved = PermissionProfile { .name = profile.name };
        if (profile.extends)
        {
            auto const parent = std::ranges::find(profiles, *profile.extends, &ProfileDef::name);
            if (parent == profiles.end())
                return std::unexpected(ProfileError::UnknownProfile);
            auto inherited = resolveOne(profiles, *parent, depth + 1);
            if (!inherited)
                return inherited;
            resolved.mode = inherited->mode;
            resolved.allow = std::move(inherited->allow);
            resolved.deny = std::move(inherited->deny);
        }
        resolved.mode = profile.mode.value_or(resolved.mode);
        appendMissing(resolved.allow, profile.allow);
        appendMissing(resolved.deny, profile.deny);
        return resolved;
    }

    /// What a run's stored settings let its agent do.
    struct SettingsSummary
    {
        PermissionModeName const& mode;
        std::size_t allowed;
        std::size_t denied;
    };

    [[nodiscard]] std::optional<SettingsSummary> summaryOfSettings(std::string_view settingsJson)
    {
        auto const settings = json::readDocument<ReadOptions, JsonSettings>(settingsJson);
        if (!settings)
            return std::nullopt;
        auto const row = std::ranges::find(
            PermissionModeNames, settings->permissions.defaultMode, &PermissionModeName::word);
        if (row == PermissionModeNames.end())
            return std::nullopt;
        return SettingsSummary { .mode = *row,
                                 .allowed = settings->permissions.allow.size(),
                                 .denied = settings->permissions.deny.size() };
    }
} // namespace

std::optional<PermissionMode> permissionModeOf(std::string_view word) noexcept
{
    auto const row = std::ranges::find(PermissionModeNames, word, &PermissionModeName::word);
    return row != PermissionModeNames.end() ? std::optional { row->mode } : std::nullopt;
}

std::span<BuiltinProfile const> builtinProfiles() noexcept
{
    return BuiltinProfiles;
}

PermissionProfile const* findProfile(std::span<PermissionProfile const> profiles,
                                     std::string_view name) noexcept
{
    auto const profile = std::ranges::find(profiles, name, &PermissionProfile::name);
    return profile != profiles.end() ? &*profile : nullptr;
}

bool isPermissionRule(std::string_view rule) noexcept
{
    auto const open = rule.find('(');
    auto const tool = rule.substr(0, open);
    auto const isToolCharacter = [](char c) {
        return detail::isAlphanumeric(c) || c == '_' || c == '-';
    };
    if (tool.empty() || !detail::isAlphanumeric(tool.front()) || detail::isDigit(tool.front())
        || !std::ranges::all_of(tool, isToolCharacter))
        return false;
    if (open == std::string_view::npos)
        return true;
    if (!rule.ends_with(')'))
        return false;
    auto const specifier = rule.substr(open + 1, rule.size() - open - 2);
    return !specifier.empty()
           && !std::ranges::any_of(specifier, [](char c) { return c == '(' || c == ')' || isControl(c); });
}

std::expected<std::vector<PermissionProfile>, ProfileProblem> resolveProfiles(
    std::span<ProfileDef const> configured)
{
    auto profiles = BuiltinProfiles | std::views::transform(definitionOf) | std::ranges::to<std::vector>();
    for (auto const& profile: configured)
    {
        if (auto const builtin = std::ranges::find(profiles, profile.name, &ProfileDef::name);
            builtin != profiles.end())
            *builtin = profile;
        else
            profiles.push_back(profile);
    }
    // A configured profile is resolved first, so that a problem is reported where it is written: a built-in
    // profile fails only through a configured one it builds on.
    for (auto const& profile: configured)
        if (auto const one = resolveOne(profiles, profile); !one)
            return std::unexpected(ProfileProblem { .profile = profile.name, .error = one.error() });
    auto resolved = std::vector<PermissionProfile> {};
    resolved.reserve(profiles.size());
    for (auto const& profile: profiles)
    {
        auto one = resolveOne(profiles, profile);
        if (!one)
            return std::unexpected(ProfileProblem { .profile = profile.name, .error = one.error() });
        resolved.push_back(std::move(*one));
    }
    return resolved;
}

std::string settingsJsonOf(PermissionProfile const& profile)
{
    auto const row = std::ranges::find(PermissionModeNames, profile.mode, &PermissionModeName::mode);
    auto const settings = JsonSettings { .permissions = { .defaultMode = std::string { row->word },
                                                          .allow = profile.allow,
                                                          .deny = profile.deny } };
    auto text = std::string {};
    if (glz::write_json(settings, text))
        return std::string { EmptySettingsJson };
    return text;
}

std::string permissionsText(std::string_view profile, std::string_view settingsJson)
{
    if (profile.empty())
        return {};
    auto const summary = summaryOfSettings(settingsJson);
    if (!summary)
        return std::string { profile };
    if (summary->allowed == 0 && summary->denied == 0)
        return std::format("{} · {}", profile, summary->mode.effect);
    return std::format(
        "{} · {} allowed · {} denied · {}", profile, summary->allowed, summary->denied, summary->mode.effect);
}

} // namespace orchestrator
