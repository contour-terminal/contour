// SPDX-License-Identifier: Apache-2.0
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <format>
#include <set>
#include <string_view>

#include <orchestrator/core/Commands.hpp>
#include <orchestrator/core/Model.hpp>
#include <orchestrator/core/Provider.hpp>

using namespace std::string_view_literals;

namespace
{

using namespace orchestrator;

/// A provider's command names, each paired with the role it plays in the row.
struct CommandRole
{
    std::string_view role;
    std::string_view (*of)(Provider const&);
};

constexpr auto CommandRoles = std::array {
    CommandRole { .role = "authCheck", .of = [](Provider const& p) { return p.authCheck; } },
    CommandRole { .role = "listChangeRequests",
                  .of = [](Provider const& p) { return p.listChangeRequests; } },
    CommandRole { .role = "listIssues", .of = [](Provider const& p) { return p.listIssues; } },
    CommandRole { .role = "defaultBranch", .of = [](Provider const& p) { return p.defaultBranch; } },
    CommandRole { .role = "fetch", .of = [](Provider const& p) { return p.fetch; } },
    CommandRole { .role = "addWorktree", .of = [](Provider const& p) { return p.addWorktree; } },
    CommandRole { .role = "addBranchWorktree", .of = [](Provider const& p) { return p.addBranchWorktree; } },
    CommandRole { .role = "checkout", .of = [](Provider const& p) { return p.checkout; } },
};

/// The commands whose `--limit` ListLimit must equal: AtLimit is only true if both say the same number.
constexpr auto LimitedRoles = std::array { "listChangeRequests"sv, "listIssues"sv };

} // namespace

TEST_CASE("Provider.github_names_only_declared_commands", "[provider]")
{
    auto const* github = findProvider(Forge::GitHub);
    REQUIRE(github != nullptr);
    CHECK(github->forge == Forge::GitHub);
    CHECK(github->name == "GitHub");
    CHECK(github->changeRequests != nullptr);
    CHECK(github->issues != nullptr);
    CHECK(github->defaultBranchOf != nullptr);

    for (auto const& row: CommandRoles)
    {
        auto const name = row.of(*github);
        CAPTURE(row.role, name);
        CHECK(findCommand(name) != nullptr);
    }
}

TEST_CASE("Provider.github_command_names", "[provider]")
{
    struct Row
    {
        std::string_view role;
        std::string_view expected;
    };
    static constexpr auto Rows = std::array {
        Row { .role = "authCheck", .expected = "gh-auth" },
        Row { .role = "listChangeRequests", .expected = "gh-prs" },
        Row { .role = "listIssues", .expected = "gh-issues" },
        Row { .role = "defaultBranch", .expected = "gh-default-branch" },
        Row { .role = "fetch", .expected = "git-fetch" },
        Row { .role = "addWorktree", .expected = "git-worktree-add" },
        Row { .role = "addBranchWorktree", .expected = "git-worktree-add-branch" },
        Row { .role = "checkout", .expected = "gh-pr-checkout" },
    };
    auto const* github = findProvider(Forge::GitHub);
    REQUIRE(github != nullptr);
    for (auto const& row: Rows)
    {
        CAPTURE(row.role, row.expected);
        auto const role = std::ranges::find(CommandRoles, row.role, &CommandRole::role);
        REQUIRE(role != CommandRoles.end());
        CHECK(role->of(*github) == row.expected);
    }
}

TEST_CASE("Provider.list_commands_ask_for_ListLimit", "[provider]")
{
    auto const limit = std::format("{}", ListLimit);
    for (auto const& provider: providers())
    {
        for (auto const role: LimitedRoles)
        {
            auto const name = std::ranges::find(CommandRoles, role, &CommandRole::role)->of(provider);
            CAPTURE(provider.name, role, name);
            auto const* command = findCommand(name);
            REQUIRE(command != nullptr);
            auto const flag = std::ranges::find(command->argv, "--limit"sv);
            REQUIRE(flag != command->argv.end());
            REQUIRE(std::next(flag) != command->argv.end());
            CHECK(*std::next(flag) == limit);
        }
    }
}

TEST_CASE("Provider.one_row_per_forge", "[provider]")
{
    auto forges = std::set<Forge> {};
    for (auto const& provider: providers())
    {
        CAPTURE(provider.name);
        CHECK(forges.insert(provider.forge).second);
        CHECK(findProvider(provider.forge) == &provider);
    }
}

TEST_CASE("Provider.gitlab_has_no_row_yet", "[provider]")
{
    CHECK(findProvider(Forge::GitLab) == nullptr);
}
