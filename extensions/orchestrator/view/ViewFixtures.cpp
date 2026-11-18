// SPDX-License-Identifier: Apache-2.0
#include <orchestrator/view/ViewFixtures.hpp>

#include <catch2/catch_test_macros.hpp>

#include <deque>
#include <format>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

#include <orchestrator/core/Fixtures.hpp>

namespace orchestrator::view::test
{

namespace
{
    constexpr auto Sha = "e6e8346832d4ba7a25fbaa38301c781b5cbb5ebe"sv;
    constexpr auto Session = "123e4567-e89b-12d3-a456-426614174000"sv;

    [[nodiscard]] std::string configJsonOf(World const& world)
    {
        return std::format(R"({{ "worktree_root": "~/worktrees", "models": ["opus", "sonnet"],
  "repos": {},
  "tasks": [
    {{ "id": "fix_ci", "key": "f", "label": "fix CI", "applies_to": "pr", "model": "sonnet",
       "prompt": "/contour-workflows:fix-ci {{url}}" }},
    {{ "id": "issue", "key": "w", "label": "work on issue", "applies_to": "issue", "model": "opus",
       "prompt": "/contour-workflows:work-issue {{url}}" }} ] }})",
                           world.repos);
    }

    [[nodiscard]] ProcessOutcome ok(std::string text)
    {
        return ProcessOutcome { .start = StartResult::Started,
                                .completion = Completion::Exited,
                                .exitCode = 0,
                                .extent = OutputExtent::Whole,
                                .stdoutText = std::move(text),
                                .stderrText = {} };
    }

    [[nodiscard]] ProcessOutcome listed(ListAnswer const& answer)
    {
        if (answer.text)
            return ok(*answer.text);
        if (!answer.fixture.empty())
            return ok(readFixture(FixtureRoot::Testdata, answer.fixture));
        auto outcome = ok({});
        outcome.exitCode = 1;
        outcome.stderrText = readFixture(FixtureRoot::Testdata, "github/auth-failed.stderr");
        return outcome;
    }

    [[nodiscard]] ProcessOutcome answerFor(World const& world, RunCommand const& command)
    {
        if (command.command == "agent-setup-check")
            return ok(readFixture(FixtureRoot::Testdata, "contour/setup-hooks-present.json"));
        if (command.command == "git-worktree-list")
            return ok(std::format("worktree {}\0HEAD {}\0branch refs/heads/master\0\0"sv, command.cwd, Sha));
        if (command.command == "gh-default-branch")
            return ok(readFixture(FixtureRoot::Testdata, "github/default-branch.json"));
        if (command.command == "gh-prs")
            return listed(world.prs);
        if (command.command == "gh-issues")
            return listed(world.issues);
        return ok({});
    }

    void queueCommands(std::vector<Effect> const& effects, std::deque<RunCommand>& queue)
    {
        for (auto const& effect: effects)
            if (auto const* const command = std::get_if<RunCommand>(&effect))
                queue.push_back(*command);
    }
} // namespace

Run outsideConfiguration(Run run)
{
    run.repo = "retired";
    run.repoPath = "/home/u/projects/retired";
    run.worktree = std::format("{}/retired/{}", WorktreeRoot, run.id);
    return run;
}

Engine bootEngine(World const& world)
{
    auto context = StartContext { .monoMs = StartMonoMs,
                                  .wallMs = DispatchWallMs,
                                  .host = HostInfo { .home = "/home/u",
                                                     .worktreeRoot = std::string { WorktreeRoot },
                                                     .repoPaths = world.repoPaths,
                                                     .utcOffsetMinutes = 0 },
                                  .configJson = world.configJson.value_or(configJsonOf(world)),
                                  .stored = {},
                                  .inFlight = {},
                                  .finished = {},
                                  .panes = {},
                                  .paneListing = PaneListing::Tagged };
    for (auto const& run: world.stored)
    {
        context.stored.push_back(StoredEntry { .key = runKey(run.id), .value = toJson(run) });
        if (countsAsActive(run))
            context.panes.push_back(PaneFact { .pane = std::format("p-{}", run.id),
                                               .tag = run.id,
                                               .status = run.lastStatus,
                                               .program = ProgramLiveness::Alive });
    }
    auto started = Engine::start(std::move(context));
    auto engine = std::move(started.engine);
    auto queue = std::deque<RunCommand> {};
    queueCommands(started.effects, queue);
    while (!queue.empty())
    {
        auto command = std::move(queue.front());
        queue.pop_front();
        auto outcome = answerFor(world, command);
        queueCommands(engine.step(ProcessFinished { .tag = std::move(command.tag),
                                                    .outcome = std::move(outcome),
                                                    .monoMs = StartMonoMs }),
                      queue);
    }
    return engine;
}

Run storedRun(RunState state, std::string_view id, ItemRef item, std::string_view task)
{
    auto const url = std::visit(
        [](auto const& ref) {
            if constexpr (std::is_same_v<std::decay_t<decltype(ref)>, ChangeRequestRef>)
                return std::format("https://github.com/contour-terminal/contour/pull/{}", ref.number);
            else if constexpr (std::is_same_v<std::decay_t<decltype(ref)>, IssueRef>)
                return std::format("https://github.com/contour-terminal/contour/issues/{}", ref.number);
            else
                return std::format("https://jira.example.com/browse/{}", ref.key);
        },
        item);
    auto const isActive = std::holds_alternative<Running>(state) || std::holds_alternative<Blocked>(state)
                          || std::holds_alternative<Done>(state);
    return Run { .id = std::string { id },
                 .repo = "contour",
                 .repoPath = std::string { ContourPath },
                 .forge = Forge::GitHub,
                 .remote = "origin",
                 .base = "master",
                 .item = std::move(item),
                 .url = url,
                 .task = std::string { task },
                 .model = "sonnet",
                 .prompt = std::format("/contour-workflows:fix-ci {}", url),
                 .session = std::string { Session },
                 .branch = std::format("agent/{}", id),
                 .worktree = std::format("{}/contour/{}", WorktreeRoot, id),
                 .pane = isActive ? std::optional { std::format("p-{}", id) } : std::nullopt,
                 .state = std::move(state),
                 .lastStatus = isActive ? std::optional { AgentStatus::Working } : std::nullopt,
                 .failure = std::nullopt,
                 .startedWallMs = DispatchWallMs,
                 .attempt = 3 };
}

void checkGolden(std::string_view name, std::string const& screen)
{
    auto const path = std::format("{}.screen", name);
    auto const expected = readFixtureIfPresent(FixtureRoot::Screens, path);
    if (updatingGoldens())
    {
        writeFixture(FixtureRoot::Screens, path, screen + "\n");
        return;
    }
    INFO("golden " << path << "; set ORCHESTRATOR_UPDATE_GOLDENS=1 to write it");
    REQUIRE(expected.has_value());
    CHECK(screen + "\n" == *expected);
}

} // namespace orchestrator::view::test
