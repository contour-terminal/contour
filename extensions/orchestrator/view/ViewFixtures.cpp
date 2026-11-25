// SPDX-License-Identifier: Apache-2.0
#include <orchestrator/view/ViewFixtures.hpp>

#include <core/tui/TestHelpers.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <chrono>
#include <deque>
#include <format>
#include <iterator>
#include <ranges>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

#include <orchestrator/core/Fixtures.hpp>
#include <orchestrator/view/Theme.hpp>

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

    /// @return What `git worktree list --porcelain -z` prints for the repository checked out at @p checkout.
    [[nodiscard]] std::string worktreeListing(World const& world, std::string_view checkout)
    {
        auto listing = std::format("worktree {}\0HEAD {}\0branch refs/heads/master\0\0"sv, checkout, Sha);
        if (checkout != ContourPath)
            return listing;
        for (auto const& entry: world.worktrees)
            listing +=
                entry.branch
                    ? std::format(
                          "worktree {}\0HEAD {}\0branch refs/heads/{}\0\0"sv, entry.path, Sha, *entry.branch)
                    : std::format("worktree {}\0HEAD {}\0detached\0\0"sv, entry.path, Sha);
        return listing;
    }

    [[nodiscard]] ProcessOutcome answerFor(World const& world, RunCommand const& command)
    {
        for (auto const& own: world.answers)
            if (own.command == command.command
                && (own.argumentContains.empty()
                    || std::ranges::any_of(command.arguments, [&](Argument const& argument) {
                           return argument.value.contains(own.argumentContains);
                       })))
                return listed(own.answer);
        if (command.command == "agent-setup-check")
        {
            auto const set = std::ranges::find(command.arguments, EnvSetArgument, &Argument::name);
            auto const own =
                set != command.arguments.end() ? world.hooksBySet.find(set->value) : world.hooksBySet.end();
            return ok(readFixture(FixtureRoot::Testdata,
                                  own != world.hooksBySet.end() ? own->second : world.hooks));
        }
        if (command.command == "claude-plugins")
            return ok(readFixture(FixtureRoot::Testdata, world.plugins));
        if (command.command == "git-worktree-list")
            return ok(worktreeListing(world, command.cwd));
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

    /// Answers @p queue and every command the answers lead to, as @p world says; a command World::held names
    /// goes to @p held instead, unless that is nullptr.
    void answerAll(Engine& engine,
                   World const& world,
                   std::deque<RunCommand>& queue,
                   std::vector<RunCommand>* held)
    {
        while (!queue.empty())
        {
            auto command = std::move(queue.front());
            queue.pop_front();
            if (held != nullptr && std::ranges::contains(world.held, command.command))
            {
                held->push_back(std::move(command));
                continue;
            }
            auto outcome = answerFor(world, command);
            queueCommands(engine.step(ProcessFinished { .tag = std::move(command.tag),
                                                        .outcome = std::move(outcome),
                                                        .monoMs = StartMonoMs }),
                          queue);
        }
    }
} // namespace

DashboardFixture::DashboardFixture(World const& world, core::tui::Size size):
    world { world },
    engine { bootEngine(world, held) },
    // Six minutes after the engine started, as the guest's clock would read it.
    clock { core::SteadyTimePoint { std::chrono::milliseconds { StartMonoMs + uint64_t { 360'000 } } } },
    dashboard { engine, dashboardTheme(), clock, size, ids, std::string { Tab } }
{
}

void DashboardFixture::answerHeld()
{
    auto queue = std::deque<RunCommand> { held.begin(), held.end() };
    held.clear();
    answerAll(engine, world, queue, nullptr);
    dashboard.engineChanged();
}

void DashboardFixture::poll()
{
    auto queue = std::deque<RunCommand> {};
    queueCommands(engine.step(RefreshNow { .monoMs = monoMs() }), queue);
    answerAll(engine, world, queue, nullptr);
    dashboard.engineChanged();
}

std::vector<UiAction> DashboardFixture::escapeTimesOut()
{
    clock.advance(std::chrono::milliseconds { 60 });
    auto actions = dashboard.dueActions();
    static_cast<void>(dashboard.frame());
    return actions;
}

ProcessOutcome printed(std::string stdoutText)
{
    return ProcessOutcome { .start = StartResult::Started,
                            .completion = Completion::Exited,
                            .exitCode = 0,
                            .extent = OutputExtent::Whole,
                            .stdoutText = std::move(stdoutText),
                            .stderrText = {} };
}

ProcessOutcome failedWith(std::string stderrText)
{
    return ProcessOutcome { .start = StartResult::Started,
                            .completion = Completion::Exited,
                            .exitCode = 1,
                            .extent = OutputExtent::Whole,
                            .stdoutText = {},
                            .stderrText = std::move(stderrText) };
}

uint64_t DashboardFixture::monoMs() const
{
    return static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(clock.now().time_since_epoch()).count());
}

std::vector<RunCommand> DashboardFixture::carryOut(std::vector<UiAction> const& actions,
                                                   Answers const& answers)
{
    auto commands = std::vector<RunCommand> {};
    auto const tab = std::string { Tab };
    for (auto const& action: actions)
    {
        auto const now = monoMs();
        auto const wall = DispatchWallMs + (now - StartMonoMs);
        auto effects = std::vector<Effect> {};
        if (auto const* const read = std::get_if<ReadTranscriptAction>(&action))
            effects = engine.step(
                ReadTranscript { .run = read->run, .tab = tab, .page = read->page, .monoMs = now });
        else if (auto const* const show = std::get_if<ShowBoardAction>(&action))
            effects = engine.step(ShowBoard { .run = show->run, .tab = tab, .monoMs = now });
        else if (std::holds_alternative<HideBoardAction>(action))
            effects = engine.step(HideBoard { .tab = tab, .monoMs = now });
        else if (auto const* const fold = std::get_if<ToggleFoldAction>(&action))
            effects = engine.step(ToggleFold { .group = fold->group, .monoMs = now });
        // What the answers lead to is followed too: a statistics read, failed at once, may have held the
        // transcript's back in the queue.
        while (!effects.empty())
        {
            auto next = std::vector<Effect> {};
            for (auto const& effect: effects)
            {
                if (auto const* const file = std::get_if<ReadFile>(&effect); file != nullptr && answers.board)
                    std::ranges::move(engine.step(FileRead { .tag = file->tag,
                                                             .content = *answers.board,
                                                             .modifiedWallMs = answers.boardModifiedWallMs,
                                                             .monoMs = now,
                                                             .wallMs = wall }),
                                      std::back_inserter(next));
                auto const* const command = std::get_if<RunCommand>(&effect);
                if (command == nullptr)
                    continue;
                auto const reply = [&](ProcessOutcome const& outcome) {
                    std::ranges::move(engine.step(ProcessFinished {
                                          .tag = command->tag, .outcome = outcome, .monoMs = now }),
                                      std::back_inserter(next));
                };
                if (command->command == "agent-stats" || command->command == "agent-record")
                    reply(failedWith("not in this test"));
                else if (command->command == "agent-transcript")
                {
                    commands.push_back(*command);
                    if (answers.transcript)
                        reply(*answers.transcript);
                }
            }
            effects = std::move(next);
        }
    }
    dashboard.engineChanged();
    return commands;
}

std::vector<RunCommand> DashboardFixture::settle(Answers const& answers)
{
    return carryOut(dashboard.dueActions(), answers);
}

std::string DashboardFixture::screen()
{
    static_cast<void>(dashboard.frame());
    return core::tui::test::canvasToString(dashboard.rendered());
}

Run outsideConfiguration(Run run)
{
    run.repo = "retired";
    run.repoPath = "/home/u/projects/retired";
    run.worktree = std::format("{}/retired/{}", WorktreeRoot, run.id);
    return run;
}

World workWorld()
{
    return World { .repos = std::string { WorkRepository },
                   .prs = { "github/prs-work.json" },
                   .stored = { storedRun(Running {}, WorkRunId, ChangeRequestRef { 2376 }) },
                   .answers = {
                       CommandAnswer { .command = "acli-issues",
                                       .argumentContains = "key in",
                                       .answer = { "jira/issues-lookup-work.json" } },
                       CommandAnswer { .command = "acli-issues", .answer = { "jira/issues-work.json" } },
                   } };
}

std::string manyPrs(int count)
{
    constexpr auto DayMinutes = 24 * 60;
    auto json = std::string { "[" };
    for (auto const number: std::views::iota(1, count + 1))
        json += std::format(R"({}[{},"title {}","https://github.com/example/contour/pull/{}","h{}","master",)"
                            R"(false,false,"",[],[],"MERGEABLE","2026-11-12T{:02}:{:02}:00Z"])",
                            number == 1 ? "" : ",",
                            number,
                            number,
                            number,
                            number,
                            (DayMinutes - number) / 60,
                            (DayMinutes - number) % 60);
    return json + "]";
}

Engine bootEngine(World const& world)
{
    auto held = std::vector<RunCommand> {};
    return bootEngine(world, held);
}

Engine bootEngine(World const& world, std::vector<RunCommand>& held)
{
    auto context = StartContext { .monoMs = StartMonoMs,
                                  .wallMs = DispatchWallMs,
                                  .host = HostInfo { .home = "/home/u",
                                                     .worktreeRoot = std::string { WorktreeRoot },
                                                     .repoPaths = world.repoPaths,
                                                     .utcOffsetMinutes = 0,
                                                     .selfPrograms = world.selfPrograms },
                                  .configJson = world.configJson.value_or(configJsonOf(world)),
                                  .stored = {},
                                  .inFlight = {},
                                  .finished = {},
                                  .panes = {},
                                  .paneListing = PaneListing::Tagged };
    for (auto const& run: world.stored)
    {
        context.stored.push_back(StoredEntry { .key = runKey(run.id), .value = toJson(run) });
        auto const message = world.paneMessages.find(run.id);
        if (countsAsActive(run))
            context.panes.push_back(
                PaneFact { .pane = std::format("p-{}", run.id),
                           .tag = run.id,
                           .status = run.lastStatus,
                           .message = message != world.paneMessages.end() ? message->second : std::string {},
                           .program = ProgramLiveness::Alive });
    }
    auto started = Engine::start(std::move(context));
    auto engine = std::move(started.engine);
    auto queue = std::deque<RunCommand> {};
    queueCommands(started.effects, queue);
    answerAll(engine, world, queue, &held);
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
            else if constexpr (std::is_same_v<std::decay_t<decltype(ref)>, JiraRef>)
                return std::format("https://jira.example.com/browse/{}", ref.key);
            else
                return std::string {}; // A topic has no web page.
        },
        item);
    // A topic's text, as its slug spells it with spaces.
    auto topic = std::string {};
    if (auto const* const ref = std::get_if<TopicRef>(&item))
        std::ranges::replace_copy(ref->slug, std::back_inserter(topic), '-', ' ');
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
                 .topic = std::move(topic),
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
