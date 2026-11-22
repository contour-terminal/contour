// SPDX-License-Identifier: Apache-2.0
#include <orchestrator/core/Permissions.hpp>

#include <algorithm>
#include <array>
#include <expected>
#include <format>
#include <map>
#include <optional>
#include <ranges>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <glaze/glaze.hpp>
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

    [[nodiscard]] std::vector<std::string> rulesOf(std::span<std::string_view const> rules)
    {
        return rules | std::views::transform([](std::string_view rule) { return std::string { rule }; })
               | std::ranges::to<std::vector>();
    }

    [[nodiscard]] bool isToolCharacter(char c) noexcept
    {
        return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_'
               || c == '-';
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

    /// Resolves profiles by name, remembering each once resolved.
    class Resolver
    {
      public:
        explicit Resolver(std::span<PermissionProfile const> profiles) noexcept: _profiles { profiles } {}

        /// @return The profile of @p name resolved, or why not.
        [[nodiscard]] std::expected<PermissionProfile, ProfileProblem> resolve(std::string const& name)
        {
            if (auto const done = _resolved.find(name); done != _resolved.end())
                return done->second;
            auto const* const profile = find(name);
            if (std::ranges::contains(_chain, name) || profile == nullptr)
                return std::unexpected(
                    ProfileProblem { .profile = name, .error = ProfileError::ProfileCycle });
            _chain.push_back(name);
            auto resolved = PermissionProfile { .name = name };
            if (profile->extends)
            {
                if (find(*profile->extends) == nullptr)
                    return std::unexpected(
                        ProfileProblem { .profile = name, .error = ProfileError::UnknownProfile });
                auto parent = resolve(*profile->extends);
                if (!parent)
                    return std::unexpected(ProfileProblem { .profile = name, .error = parent.error().error });
                resolved.mode = parent->mode;
                resolved.allow = std::move(parent->allow);
                resolved.deny = std::move(parent->deny);
            }
            resolved.mode = profile->mode.value_or(resolved.mode.value_or(PermissionMode::Default));
            appendMissing(resolved.allow, profile->allow);
            appendMissing(resolved.deny, profile->deny);
            _chain.pop_back();
            _resolved.emplace(name, resolved);
            return resolved;
        }

      private:
        [[nodiscard]] PermissionProfile const* find(std::string_view name) const noexcept
        {
            auto const profile = std::ranges::find(_profiles, name, &PermissionProfile::name);
            return profile != _profiles.end() ? &*profile : nullptr;
        }

        std::span<PermissionProfile const> _profiles;
        std::vector<std::string> _chain;
        std::map<std::string, PermissionProfile, std::less<>> _resolved;
    };
} // namespace

std::optional<PermissionMode> permissionModeOf(std::string_view word) noexcept
{
    auto const row = std::ranges::find(PermissionModeNames, word, &PermissionModeName::word);
    return row != PermissionModeNames.end() ? std::optional { row->mode } : std::nullopt;
}

std::string_view nameOf(PermissionMode mode) noexcept
{
    auto const row = std::ranges::find(PermissionModeNames, mode, &PermissionModeName::mode);
    return row != PermissionModeNames.end() ? row->word : std::string_view {};
}

std::vector<PermissionProfile> builtinProfiles()
{
    return {
        PermissionProfile { .name = std::string { DefaultProfileName },
                            .mode = PermissionMode::AcceptEdits,
                            .allow = rulesOf(StandardAllow),
                            .deny = rulesOf(StandardDeny) },
        PermissionProfile { .name = "trusted",
                            .extends = std::string { DefaultProfileName },
                            .allow = rulesOf(TrustedAllow) },
        PermissionProfile { .name = "ask", .mode = PermissionMode::Default },
    };
}

bool isPermissionRule(std::string_view rule) noexcept
{
    auto const open = rule.find('(');
    auto const tool = rule.substr(0, open);
    auto const isLetter = [](char c) {
        return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
    };
    if (tool.empty() || !isLetter(tool.front()) || !std::ranges::all_of(tool, isToolCharacter))
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
    std::span<PermissionProfile const> configured)
{
    auto profiles = builtinProfiles();
    for (auto const& profile: configured)
    {
        if (auto const builtin = std::ranges::find(profiles, profile.name, &PermissionProfile::name);
            builtin != profiles.end())
            *builtin = profile;
        else
            profiles.push_back(profile);
    }
    auto resolver = Resolver { profiles };
    auto resolved = std::vector<PermissionProfile> {};
    resolved.reserve(profiles.size());
    for (auto const& profile: profiles)
    {
        auto one = resolver.resolve(profile.name);
        if (!one)
            return std::unexpected(std::move(one.error()));
        resolved.push_back(std::move(*one));
    }
    return resolved;
}

std::string settingsJsonOf(PermissionProfile const& resolved)
{
    auto const settings =
        JsonSettings { .permissions = { .defaultMode = std::string { nameOf(
                                            resolved.mode.value_or(PermissionMode::Default)) },
                                        .allow = resolved.allow,
                                        .deny = resolved.deny } };
    auto text = std::string {};
    if (glz::write_json(settings, text))
        return "{}";
    return text;
}

std::string permissionsText(std::string_view profile, std::string_view settingsJson)
{
    // What each mode leaves to the agent, as the line says it.
    constexpr auto ModeTexts = std::array {
        "Claude Code asks for the rest"sv, // Default
        "edits need no approval"sv,        // AcceptEdits
        "it plans and changes nothing"sv,  // Plan
    };
    static_assert(ModeTexts.size() == PermissionModeNames.size(), "ModeTexts has one row per PermissionMode");
    if (profile.empty())
        return {};
    auto const summary = summaryOfSettings(settingsJson);
    if (!summary)
        return std::string { profile };
    auto const modeText = ModeTexts[std::to_underlying(summary->mode)];
    if (summary->allowed == 0 && summary->denied == 0)
        return std::format("{} · {}", profile, modeText);
    return std::format(
        "{} · {} allowed · {} denied · {}", profile, summary->allowed, summary->denied, modeText);
}

std::optional<SettingsSummary> summaryOfSettings(std::string_view settingsJson)
{
    auto const settings = json::readDocument<ReadOptions, JsonSettings>(settingsJson);
    if (!settings)
        return std::nullopt;
    auto const mode = permissionModeOf(settings->permissions.defaultMode);
    if (!mode)
        return std::nullopt;
    return SettingsSummary { .mode = *mode,
                             .allowed = settings->permissions.allow.size(),
                             .denied = settings->permissions.deny.size() };
}

} // namespace orchestrator
