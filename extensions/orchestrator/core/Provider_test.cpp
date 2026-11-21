// SPDX-License-Identifier: Apache-2.0
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <format>
#include <set>
#include <string_view>
#include <vector>

#include <orchestrator/core/Commands.hpp>
#include <orchestrator/core/Config.hpp>
#include <orchestrator/core/IssueSources.hpp>
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

/// How a tool spells the most entries a list asks for: `gh` and `acli` say `--limit`, `glab` `--per-page`.
constexpr auto LimitFlags = std::array { "--limit"sv, "--per-page"sv };

} // namespace

TEST_CASE("every provider names declared commands", "[provider]")
{
    CHECK(providers().size() == 2);
    for (auto const& provider: providers())
    {
        CAPTURE(provider.name);
        CHECK(provider.changeRequests != nullptr);
        CHECK(provider.issues != nullptr);
        CHECK(provider.defaultBranchOf != nullptr);
        for (auto const& row: CommandRoles)
        {
            auto const name = row.of(provider);
            CAPTURE(row.role, name);
            CHECK(findCommand(name) != nullptr);
        }
    }
}

TEST_CASE("a provider's commands share its verification", "[provider]")
{
    for (auto const& provider: providers())
    {
        for (auto const& row: CommandRoles)
        {
            auto const name = row.of(provider);
            CAPTURE(provider.name, row.role, name);
            auto const* const command = findCommand(name);
            REQUIRE(command != nullptr);
            // git's commands, which every forge's row shares, are Recorded: their output is not the forge's.
            if (command->argv.front() != "git")
                CHECK(command->verification == provider.verification);
            else
                CHECK(command->verification == Verification::Recorded);
        }
    }
}

TEST_CASE("an issue source's commands share its verification", "[provider]")
{
    for (auto const& source: issueSources())
    {
        for (auto const name: { source.command, source.authCheck })
        {
            CAPTURE(source.name, name);
            if (name.empty()) // the forge provider's own, checked above
                continue;
            auto const* const command = findCommand(name);
            REQUIRE(command != nullptr);
            CHECK(command->verification == source.verification);
        }
        // Its query fills a declared Text argument the command names, and a source with a command maps it.
        auto const* const argument = findArgumentDecl(source.argument);
        REQUIRE(argument != nullptr);
        CHECK(argument->kind == ArgumentKind::Text);
        CHECK(source.command.empty() == (source.map == nullptr));
        if (auto const* const command = findCommand(source.command))
            CHECK(std::ranges::contains(command->argv, std::format("{{{}}}", source.argument)));
        if (auto const* const auth = findCommand(source.authCheck))
            CHECK(auth->output == OutputUse::ExitStatusOnly);
    }
    CHECK(findIssueSource(issueSources(), "forge") != nullptr);
    CHECK(findIssueSource(issueSources(), "jira") != nullptr);
    CHECK(findIssueSource(issueSources(), "linear") == nullptr);
}

TEST_CASE("an auth check's output is never read", "[provider]")
{
    for (auto const& provider: providers())
    {
        CAPTURE(provider.name);
        auto const* const command = findCommand(provider.authCheck);
        REQUIRE(command != nullptr);
        CHECK(command->output == OutputUse::ExitStatusOnly);
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
            auto const flag = std::ranges::find_first_of(command->argv, LimitFlags);
            REQUIRE(flag != command->argv.end());
            REQUIRE(std::next(flag) != command->argv.end());
            CHECK(*std::next(flag) == limit);
        }
    }
    for (auto const& source: issueSources())
    {
        CAPTURE(source.name);
        if (auto const* const command = findCommand(source.command))
        {
            auto const flag = std::ranges::find_first_of(command->argv, LimitFlags);
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

TEST_CASE("an unmeasured forge is refused by the configuration", "[provider]")
{
    auto const* const gitlab = findProvider(Forge::GitLab);
    REQUIRE(gitlab != nullptr);
    CHECK(gitlab->verification == Verification::Unmeasured);

    constexpr auto Json = R"({ "worktree_root": "~/worktrees",
  "repos": [ { "path": "~/repo/contour", "forge": "gitlab" } ],
  "tasks": [ { "id": "issue", "key": "w", "label": "work on issue", "applies_to": "issue", "model": "opus",
               "prompt": "/contour-workflows:work-issue {url}" } ] })"sv;
    auto const host = HostInfo { .home = "/home/u",
                                 .worktreeRoot = "/home/u/worktrees",
                                 .repoPaths = { "/home/u/repo/contour" },
                                 .utcOffsetMinutes = 0 };
    auto const config = Config::parse(Json, host, sourceTables());
    REQUIRE(!config);
    CHECK(config.error().key == "repos[0].forge");
    CHECK(config.error().problem == ConfigProblem::Unrecorded);
    CHECK(message(config.error(), sourceTables())
          == "repos[0].forge: is not yet supported: GitLab has no recorded fixture");
    // The forges a configuration may name are the Recorded ones.
    CHECK(describe(ConfigProblem::UnknownForge, sourceTables()) == "is not a supported forge: github");

    SECTION("a Recorded row is accepted")
    {
        auto recorded = std::vector<Provider>(providers().begin(), providers().end());
        for (auto& row: recorded)
            row.verification = Verification::Recorded;
        auto const accepted =
            Config::parse(Json, host, SourceTables { .providers = recorded, .issueSources = issueSources() });
        REQUIRE(accepted);
        CHECK(accepted->repos().front().forge == Forge::GitLab);
        // The refusal of an unknown forge lists the rows the configuration was checked against.
        CHECK(describe(ConfigProblem::UnknownForge,
                       SourceTables { .providers = recorded, .issueSources = issueSources() })
              == "is not a supported forge: github, gitlab");
    }
}
