// SPDX-License-Identifier: Apache-2.0
#include <catch2/catch_test_macros.hpp>

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <ranges>
#include <set>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include <orchestrator/core/Config.hpp>
#include <orchestrator/core/Model.hpp>

using namespace std::string_view_literals;
using namespace std::chrono_literals;

namespace
{

using namespace orchestrator;

/// The configuration's example, as the host hands it over, without `base`.
constexpr auto Example = R"({
  "worktree_root": "~/worktrees",
  "models": ["opus", "sonnet", "haiku"],
  "max_agents": 4,
  "max_agents_per_repo": 2,
  "poll_every": "2m",
  "status_board_after": "15m",
  "finished_retention": "30d",
  "repos": [
    { "path": "~/repo/contour", "forge": "github", "remote": "origin",
      "issues": [ { "source": "forge", "query": "assignee:@me" } ] }
  ],
  "tasks": [
    { "id": "fix_ci", "key": "f", "label": "fix CI", "applies_to": "pr", "model": "sonnet",
      "prompt": "/contour-workflows:fix-ci {url}" },
    { "id": "address_review", "key": "r", "label": "address review", "applies_to": "pr", "model": "opus",
      "prompt": "/contour-workflows:address-review {url}" },
    { "id": "issue", "key": "w", "label": "work on issue", "applies_to": "issue", "model": "opus",
      "prompt": "/contour-workflows:work-issue {url}" }
  ]
})"sv;

/// Only what has no default.
constexpr auto Minimal = R"({
  "worktree_root": "~/worktrees",
  "repos": [ { "path": "~/repo/contour", "forge": "github" } ],
  "tasks": [ { "id": "issue", "key": "w", "label": "work on issue", "applies_to": "issue", "model": "opus",
               "prompt": "/contour-workflows:work-issue {url}" } ]
})"sv;

constexpr auto WithoutTasks = R"({
  "worktree_root": "~/worktrees",
  "repos": [ { "path": "~/repo/contour", "forge": "github" } ]
})"sv;

constexpr auto WithoutRepos = R"({
  "worktree_root": "~/worktrees",
  "tasks": [ { "id": "issue", "key": "w", "label": "work on issue", "applies_to": "issue", "model": "opus",
               "prompt": "/contour-workflows:work-issue {url}" } ]
})"sv;

constexpr auto WithoutWorktreeRoot = R"({
  "repos": [ { "path": "~/repo/contour", "forge": "github" } ],
  "tasks": [ { "id": "issue", "key": "w", "label": "work on issue", "applies_to": "issue", "model": "opus",
               "prompt": "/contour-workflows:work-issue {url}" } ]
})"sv;

constexpr auto Home = "/home/u"sv;
constexpr auto WorktreeRoot = "/home/u/worktrees"sv;
constexpr auto ContourPath = "/home/u/repo/contour"sv;
constexpr auto OneRepo = std::array { ContourPath };
constexpr auto NoRepos = std::array<std::string_view, 0> {};
constexpr auto TwoRepos = std::array { "/home/u/repo/other"sv, ContourPath };
constexpr auto SameRepoTwice = std::array { ContourPath, ContourPath };
constexpr auto TwoContours = std::array { "/home/u/b/contour"sv, ContourPath };
constexpr auto RepoInWorktreeRoot = std::array { "/home/u/worktrees/contour"sv };
constexpr auto RelativeRepo = std::array { "repo/contour"sv };
constexpr auto UnnamedRepo = std::array { "/home/u/repo/my repo"sv };
constexpr auto NestedRepos = std::array { "/home/u/repo/contour/vendor/lib"sv, ContourPath };
constexpr auto SiblingOfWorktreeRoot = std::array { "/home/u/worktrees-old/contour"sv };

/// A configuration a row parses: a document, with one substring replaced, and what the host resolved.
struct Input
{
    std::string_view json = Example;
    std::string_view find = {}; ///< Replaced once by `replace`; empty: the document as it is.
    std::string_view replace = {};
    std::string_view worktreeRoot = WorktreeRoot;
    std::span<std::string_view const> repoPaths = OneRepo;
};

[[nodiscard]] std::string documentOf(Input const& input)
{
    auto text = std::string { input.json };
    if (input.find.empty())
        return text;
    auto const at = text.find(input.find);
    REQUIRE(at != std::string::npos);
    text.replace(at, input.find.size(), input.replace);
    return text;
}

[[nodiscard]] HostInfo hostOf(Input const& input)
{
    auto paths = std::vector<std::string> {};
    for (auto const path: input.repoPaths)
        paths.emplace_back(path);
    return HostInfo { .home = std::string { Home },
                      .worktreeRoot = std::string { input.worktreeRoot },
                      .repoPaths = std::move(paths),
                      .utcOffsetMinutes = 60 };
}

[[nodiscard]] std::expected<Config, ConfigError> parse(Input const& input)
{
    return Config::parse(documentOf(input), hostOf(input));
}

} // namespace

TEST_CASE("Config.example", "[config]")
{
    auto const config = parse(Input {});
    REQUIRE(config.has_value());

    CHECK(config->worktreeRoot() == WorktreeRoot);
    REQUIRE(config->repos().size() == 1);
    auto const& repo = config->repos()[0];
    CHECK(repo.name == "contour");
    CHECK(repo.path == ContourPath);
    CHECK(repo.forge == Forge::GitHub);
    CHECK(repo.remote == "origin");
    CHECK_FALSE(repo.base.has_value());
    CHECK(repo.issueQueries == std::vector<std::string> { "assignee:@me" });
    CHECK(config->findRepo("contour") == &repo);
    CHECK(config->findRepo("other") == nullptr);

    REQUIRE(config->tasks().size() == 3);
    auto const& fixCi = config->tasks()[0];
    CHECK(fixCi.id == "fix_ci");
    CHECK(fixCi.key == 'f');
    CHECK(fixCi.label == "fix CI");
    CHECK(fixCi.appliesTo == AppliesTo::PullRequest);
    CHECK(fixCi.model == "sonnet");
    CHECK(fixCi.prompt == "/contour-workflows:fix-ci {url}");
    REQUIRE(config->firstTaskFor(AppliesTo::PullRequest) != nullptr);
    CHECK(config->firstTaskFor(AppliesTo::PullRequest)->id == "fix_ci");
    REQUIRE(config->firstTaskFor(AppliesTo::Issue) != nullptr);
    CHECK(config->firstTaskFor(AppliesTo::Issue)->id == "issue");
    CHECK(config->findTask("address_review") == &config->tasks()[1]);
    CHECK(config->findTask("rebase") == nullptr);
}

TEST_CASE("Config.defaults", "[config]")
{
    auto const config = parse(Input { .json = Minimal });
    REQUIRE(config.has_value());

    CHECK(config->maxAgents() == 4);
    CHECK(config->maxAgentsPerRepo() == 2);
    CHECK(config->pollEvery() == 2min);
    CHECK(config->statusBoardAfter() == 15min);
    CHECK(config->finishedRetention() == std::chrono::days { 30 });
    CHECK(std::ranges::equal(config->models(), std::array { "opus"sv, "sonnet"sv, "haiku"sv }));
    CHECK(config->repos()[0].remote == "origin");
    CHECK(config->repos()[0].issueQueries.empty());
    CHECK(config->firstTaskFor(AppliesTo::PullRequest) == nullptr);
}

namespace
{

struct AcceptRow
{
    std::string_view label;
    Input input;
};

constexpr auto AcceptRows = std::array {
    AcceptRow { .label = "base: master is kept",
                .input = { .find = R"("remote": "origin",)",
                           .replace = R"("remote": "origin", "base": "master",)" } },
    AcceptRow { .label = "most specific root: a worktree root inside a repository",
                .input = { .worktreeRoot = "/home/u/repo/contour/worktrees" } },
    AcceptRow { .label = "a sibling sharing a prefix is not inside the worktree root",
                .input = { .repoPaths = SiblingOfWorktreeRoot } },
    AcceptRow { .label = "status_board_after 0 disables the board",
                .input = { .find = R"("15m")", .replace = "0" } },
    AcceptRow {
        .label = "nested repositories: a directory belongs to its most specific root",
        .input = { .find = R"("repos": [)",
                   .replace = R"("repos": [ { "path": "~/repo/contour/vendor/lib", "forge": "github" },)",
                   .repoPaths = NestedRepos } },
    AcceptRow { .label = "a written name for a directory whose name is no Name",
                .input = { .find = R"("path":)",
                           .replace = R"("name": "mine", "path":)",
                           .repoPaths = UnnamedRepo } },
    AcceptRow { .label = "an explicit name",
                .input = { .find = R"("path":)", .replace = R"("name": "terminal", "path":)" } },
};

} // namespace

TEST_CASE("Config.accepts", "[config]")
{
    for (auto const& row: AcceptRows)
    {
        CAPTURE(row.label);
        auto const config = parse(row.input);
        REQUIRE(config.has_value());
    }

    auto const withBase = parse(AcceptRows[0].input);
    REQUIRE(withBase.has_value());
    CHECK(withBase->repos()[0].base == std::optional<std::string> { "master" });
    auto const nestedRoot = parse(AcceptRows[1].input);
    REQUIRE(nestedRoot.has_value());
    CHECK(nestedRoot->worktreeRoot() == "/home/u/repo/contour/worktrees");
    auto const disabled = parse(AcceptRows[3].input);
    REQUIRE(disabled.has_value());
    CHECK(disabled->statusBoardAfter() == 0ms);
    auto const nested = parse(AcceptRows[4].input);
    REQUIRE(nested.has_value());
    REQUIRE(nested->repos().size() == 2);
    CHECK(nested->repos()[0].name == "lib");
    CHECK(nested->repos()[1].name == "contour");
    auto const unnamed = parse(AcceptRows[5].input);
    REQUIRE(unnamed.has_value());
    CHECK(unnamed->repos()[0].name == "mine");
    auto const named = parse(AcceptRows[6].input);
    REQUIRE(named.has_value());
    CHECK(named->repos()[0].name == "terminal");
}

namespace
{

struct RejectRow
{
    std::string_view label;
    Input input;
    std::string_view key;
    ConfigProblem problem;
};

constexpr auto TwoRepoJson = R"("repos": [ { "path": "~/repo/other", "forge": "github" },)"sv;

// clang-format off
constexpr auto RejectRows = std::array {
    RejectRow { "not JSON", { .find = "\n}", .replace = "\n" }, "", ConfigProblem::NotJson },
    RejectRow { "trailing garbage", { .find = "\n}", .replace = "\n} x" }, "", ConfigProblem::NotJson },
    RejectRow { "empty input", { .json = "" }, "", ConfigProblem::NotJson },
    RejectRow { "not an object", { .json = "[]" }, "", ConfigProblem::WrongType },
    RejectRow { "a wrong type", { .find = R"("max_agents": 4)", .replace = R"("max_agents": "four")" }, "max_agents", ConfigProblem::WrongType },
    RejectRow { "an unknown key", { .find = R"("max_agents": 4)", .replace = R"("max_agent": 4)" }, "max_agent", ConfigProblem::UnknownKey },
    RejectRow { "a nested unknown key", { .find = R"("query")", .replace = R"("querystring")" }, "repos[0].issues[0].querystring", ConfigProblem::UnknownKey },
    RejectRow { "an unknown task key", { .find = R"("applies_to": "issue")", .replace = R"("applies_to": "issue", "colour": "red")" }, "tasks[2].colour", ConfigProblem::UnknownKey },
    RejectRow { "a number where a string belongs", { .find = R"("remote": "origin")", .replace = R"("remote": 1)" }, "repos[0].remote", ConfigProblem::WrongType },
    RejectRow { "a boolean where a string belongs", { .find = R"("key": "w")", .replace = R"("key": true)" }, "tasks[2].key", ConfigProblem::WrongType },
    RejectRow { "a number in models", { .find = R"("sonnet", "haiku")", .replace = R"(2024, "haiku")" }, "models[1]", ConfigProblem::WrongType },
    RejectRow { "a string where an array belongs", { .find = R"(["opus", "sonnet", "haiku"])", .replace = R"("opus")" }, "models", ConfigProblem::WrongType },
    RejectRow { "a repository that is no object", { .find = R"("repos": [)", .replace = R"("repos": [ "~/repo/other",)" }, "repos[0]", ConfigProblem::WrongType },
    RejectRow { "a fractional limit", { .find = R"("max_agents": 4)", .replace = R"("max_agents": 4.5)" }, "max_agents", ConfigProblem::WrongType },
    RejectRow { "a limit beyond 32 bits", { .find = R"("max_agents": 4)", .replace = R"("max_agents": 5000000000)" }, "max_agents", ConfigProblem::WrongType },
    RejectRow { "a negative limit", { .find = R"("max_agents": 4)", .replace = R"("max_agents": -1)" }, "max_agents", ConfigProblem::WrongType },
    RejectRow { "no worktree_root", { .json = WithoutWorktreeRoot }, "worktree_root", ConfigProblem::Missing },
    RejectRow { "a relative worktree root", { .worktreeRoot = "worktrees" }, "worktree_root", ConfigProblem::NotAbsolutePath },
    RejectRow { "repos differ from the host's paths", { .repoPaths = TwoRepos }, "repos", ConfigProblem::CountMismatch },
    RejectRow { "no repos", { .json = WithoutRepos, .repoPaths = NoRepos }, "repos", ConfigProblem::Missing },
    RejectRow { "empty repos", { .json = Minimal, .find = R"([ { "path": "~/repo/contour", "forge": "github" } ])", .replace = "[]", .repoPaths = NoRepos }, "repos", ConfigProblem::Empty },
    RejectRow { "a relative repository path", { .repoPaths = RelativeRepo }, "repos[0].path", ConfigProblem::NotAbsolutePath },
    RejectRow { "a repository equal to worktree_root", { .worktreeRoot = ContourPath }, "repos[0].path", ConfigProblem::EqualRoots },
    RejectRow { "a repository inside worktree_root", { .repoPaths = RepoInWorktreeRoot }, "repos[0].path", ConfigProblem::InsideWorktreeRoot },
    RejectRow { "two equal repositories", { .find = R"("repos": [)", .replace = TwoRepoJson, .repoPaths = SameRepoTwice }, "repos[1].path", ConfigProblem::EqualRoots },
    RejectRow { "a duplicate directory name", { .find = R"("repos": [)", .replace = R"("repos": [ { "path": "~/b/contour", "forge": "github" },)", .repoPaths = TwoContours }, "repos[1].path", ConfigProblem::DuplicateDirectoryName },
    RejectRow { "a directory name another repository was given", { .find = R"("repos": [)", .replace = R"("repos": [ { "name": "contour", "path": "~/repo/other", "forge": "github" },)", .repoPaths = TwoRepos }, "repos[1].path", ConfigProblem::DuplicateDirectoryName },
    RejectRow { "a duplicate written name", { .find = "\"repos\": [\n    { \"path\"", .replace = R"("repos": [ { "name": "terminal", "path": "~/repo/other", "forge": "github" }, { "name": "terminal", "path")", .repoPaths = TwoRepos }, "repos[1].name", ConfigProblem::Duplicate },
    RejectRow { "a directory name that is no Name", { .repoPaths = UnnamedRepo }, "repos[0].path", ConfigProblem::UnnamedDirectory },
    RejectRow { "a non-zero number as poll_every", { .find = R"("2m")", .replace = "120" }, "poll_every", ConfigProblem::NotADuration },
    RejectRow { "a name that is no Name", { .find = R"("path":)", .replace = R"("name": "con tour", "path":)" }, "repos[0].name", ConfigProblem::NotAName },
    RejectRow { "a repository without a path", { .find = R"("path": "~/repo/contour",)", .replace = "" }, "repos[0].path", ConfigProblem::Missing },
    RejectRow { "a repository without a forge", { .find = R"("forge": "github",)", .replace = "" }, "repos[0].forge", ConfigProblem::Missing },
    RejectRow { "forge: bitbucket", { .find = R"("github")", .replace = R"("bitbucket")" }, "repos[0].forge", ConfigProblem::UnknownForge },
    RejectRow { "forge: gitlab has no provider yet", { .find = R"("github")", .replace = R"("gitlab")" }, "repos[0].forge", ConfigProblem::UnknownForge },
    RejectRow { "a remote that is no Name", { .find = R"("origin")", .replace = R"("-origin")" }, "repos[0].remote", ConfigProblem::NotAName },
    RejectRow { "a base that is no Ref", { .find = R"("remote": "origin",)", .replace = R"("remote": "origin", "base": "a..b",)" }, "repos[0].base", ConfigProblem::NotARef },
    RejectRow { "an issue source other than forge", { .find = R"("source": "forge")", .replace = R"("source": "jira")" }, "repos[0].issues[0].source", ConfigProblem::UnsupportedSource },
    RejectRow { "an issue query starting with -", { .find = R"("assignee:@me")", .replace = R"("-label:x assignee:@me")" }, "repos[0].issues[0].query", ConfigProblem::LeadingDash },
    RejectRow { "an empty issue query", { .find = R"("assignee:@me")", .replace = R"("")" }, "repos[0].issues[0].query", ConfigProblem::Empty },
    RejectRow { "an issue query holding a NUL", { .find = R"("assignee:@me")", .replace = R"("assignee:@me\u0000")" }, "repos[0].issues[0].query", ConfigProblem::NotText },
    RejectRow { "max_agents 0", { .find = R"("max_agents": 4)", .replace = R"("max_agents": 0)" }, "max_agents", ConfigProblem::Zero },
    RejectRow { "max_agents_per_repo 0", { .find = R"("max_agents_per_repo": 2)", .replace = R"("max_agents_per_repo": 0)" }, "max_agents_per_repo", ConfigProblem::Zero },
    RejectRow { "poll_every below 30s", { .find = R"("2m")", .replace = R"("29s")" }, "poll_every", ConfigProblem::TooShort },
    RejectRow { "poll_every 0", { .find = R"("2m")", .replace = "0" }, "poll_every", ConfigProblem::TooShort },
    RejectRow { "2 minutes", { .find = R"("2m")", .replace = R"("2 minutes")" }, "poll_every", ConfigProblem::NotADuration },
    RejectRow { "a number with no unit", { .find = R"("30d")", .replace = "30" }, "finished_retention", ConfigProblem::NotADuration },
    RejectRow { "empty models", { .find = R"(["opus", "sonnet", "haiku"])", .replace = "[]" }, "models", ConfigProblem::Empty },
    RejectRow { "a model that is no Name", { .find = R"("haiku")", .replace = R"("-haiku")" }, "models[2]", ConfigProblem::NotAName },
    RejectRow { "a bracketed model", { .find = R"("haiku")", .replace = R"("opus[1m]")" }, "models[2]", ConfigProblem::BracketedModel },
    RejectRow { "a duplicate model", { .find = R"("haiku")", .replace = R"("opus")" }, "models[2]", ConfigProblem::Duplicate },
    RejectRow { "no tasks", { .json = WithoutTasks }, "tasks", ConfigProblem::Missing },
    RejectRow { "an empty task table", { .json = R"({ "worktree_root": "~/worktrees", "repos": [ { "path": "~/repo/contour", "forge": "github" } ], "tasks": [] })" }, "tasks", ConfigProblem::Empty },
    RejectRow { "a task id that is no Name", { .find = R"("fix_ci")", .replace = R"("fix ci")" }, "tasks[0].id", ConfigProblem::NotAName },
    RejectRow { "a duplicate task id", { .find = R"("address_review")", .replace = R"("fix_ci")" }, "tasks[1].id", ConfigProblem::Duplicate },
    RejectRow { "a task without a label", { .find = R"("label": "fix CI",)", .replace = "" }, "tasks[0].label", ConfigProblem::Missing },
    RejectRow { "an empty label", { .find = R"("fix CI")", .replace = R"("")" }, "tasks[0].label", ConfigProblem::Empty },
    RejectRow { "a duplicate task key", { .find = R"("key": "r")", .replace = R"("key": "f")" }, "tasks[1].key", ConfigProblem::Duplicate },
    RejectRow { "task key d", { .find = R"("key": "f")", .replace = R"("key": "d")" }, "tasks[0].key", ConfigProblem::ReservedKey },
    RejectRow { "task key e", { .find = R"("key": "f")", .replace = R"("key": "e")" }, "tasks[0].key", ConfigProblem::ReservedKey },
    RejectRow { "task key b", { .find = R"("key": "f")", .replace = R"("key": "b")" }, "tasks[0].key", ConfigProblem::ReservedKey },
    RejectRow { "task key t", { .find = R"("key": "f")", .replace = R"("key": "t")" }, "tasks[0].key", ConfigProblem::ReservedKey },
    RejectRow { "task key x", { .find = R"("key": "f")", .replace = R"("key": "x")" }, "tasks[0].key", ConfigProblem::ReservedKey },
    RejectRow { "task key /", { .find = R"("key": "f")", .replace = R"("key": "/")" }, "tasks[0].key", ConfigProblem::ReservedKey },
    RejectRow { "task key ?", { .find = R"("key": "f")", .replace = R"("key": "?")" }, "tasks[0].key", ConfigProblem::ReservedKey },
    RejectRow { "task key Tab", { .find = R"("key": "f")", .replace = R"("key": "\t")" }, "tasks[0].key", ConfigProblem::ReservedKey },
    RejectRow { "task key Enter", { .find = R"("key": "f")", .replace = R"("key": "\r")" }, "tasks[0].key", ConfigProblem::ReservedKey },
    RejectRow { "task key newline", { .find = R"("key": "f")", .replace = R"("key": "\n")" }, "tasks[0].key", ConfigProblem::ReservedKey },
    RejectRow { "task key of two characters", { .find = R"("key": "f")", .replace = R"("key": "ff")" }, "tasks[0].key", ConfigProblem::NotOneKey },
    RejectRow { "task key space", { .find = R"("key": "f")", .replace = R"("key": " ")" }, "tasks[0].key", ConfigProblem::NotOneKey },
    RejectRow { "an unknown applies_to", { .find = R"("applies_to": "pr")", .replace = R"("applies_to": "mr")" }, "tasks[0].applies_to", ConfigProblem::UnknownItemKind },
    RejectRow { "a task model that is no Name", { .find = R"("model": "sonnet")", .replace = R"("model": "son net")" }, "tasks[0].model", ConfigProblem::NotAName },
    RejectRow { "a bracketed task model", { .find = R"("model": "sonnet")", .replace = R"("model": "sonnet[1m]")" }, "tasks[0].model", ConfigProblem::BracketedModel },
    RejectRow { "a task model not in models", { .find = R"("model": "sonnet")", .replace = R"("model": "claude-opus-5-5")" }, "tasks[0].model", ConfigProblem::UnlistedModel },
    RejectRow { "a prompt without {url}", { .find = R"("/contour-workflows:fix-ci {url}")", .replace = R"("/contour-workflows:fix-ci")" }, "tasks[0].prompt", ConfigProblem::MissingUrl },
    RejectRow { "a prompt starting with -", { .find = R"("/contour-workflows:fix-ci {url}")", .replace = R"("-p {url}")" }, "tasks[0].prompt", ConfigProblem::LeadingDash },
};
// clang-format on

} // namespace

TEST_CASE("Config.rejects", "[config]")
{
    for (auto const& row: RejectRows)
    {
        CAPTURE(row.label, row.key);
        auto const config = parse(row.input);
        REQUIRE_FALSE(config.has_value());
        CHECK(config.error().key == row.key);
        CHECK(config.error().problem == row.problem);
    }
}

TEST_CASE("Config.messages", "[config]")
{
    // Every problem has a message of its own; the message leads with the key.
    auto seen = std::set<std::string> {};
    for (auto const& row: RejectRows)
    {
        CAPTURE(row.label);
        auto const description = describe(row.problem);
        CHECK_FALSE(description.empty());
        seen.insert(description);
        auto const config = parse(row.input);
        REQUIRE_FALSE(config.has_value());
        CHECK(message(config.error()).starts_with(row.key));
    }
    auto problems = std::set<ConfigProblem> {};
    for (auto const& row: RejectRows)
        problems.insert(row.problem);
    CHECK(seen.size() == problems.size());
}

namespace
{

struct DurationRow
{
    std::string_view text;
    std::optional<std::chrono::milliseconds> expected;
};

constexpr auto DurationRows = std::array {
    DurationRow { .text = "90s", .expected = 90s },
    DurationRow { .text = "2m", .expected = 2min },
    DurationRow { .text = "1h", .expected = 1h },
    DurationRow { .text = "30d", .expected = std::chrono::days { 30 } },
    DurationRow { .text = "0", .expected = 0ms },
    DurationRow { .text = "0s", .expected = 0ms },
    DurationRow { .text = "999999999d", .expected = std::chrono::days { 999'999'999 } },
    DurationRow { .text = "1000000000s", .expected = std::nullopt },
    DurationRow { .text = "", .expected = std::nullopt },
    DurationRow { .text = "s", .expected = std::nullopt },
    DurationRow { .text = "2", .expected = std::nullopt },
    DurationRow { .text = "2 minutes", .expected = std::nullopt },
    DurationRow { .text = "2min", .expected = std::nullopt },
    DurationRow { .text = "1h30m", .expected = std::nullopt },
    DurationRow { .text = "-1s", .expected = std::nullopt },
    DurationRow { .text = "2M", .expected = std::nullopt },
    DurationRow { .text = "00", .expected = std::nullopt },
};

} // namespace

TEST_CASE("Config.parseDuration", "[config]")
{
    for (auto const& row: DurationRows)
    {
        CAPTURE(row.text);
        CHECK(parseDuration(row.text) == row.expected);
    }
}
