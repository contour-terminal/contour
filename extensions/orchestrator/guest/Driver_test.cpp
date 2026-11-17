// SPDX-License-Identifier: Apache-2.0
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <format>
#include <fstream>
#include <functional>
#include <map>
#include <optional>
#include <ranges>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

#include <orchestrator/core/Engine.hpp>
#include <orchestrator/core/Fixtures.hpp>
#include <orchestrator/guest/Driver.hpp>
#include <orchestrator/guest/HostCalls.hpp>

using namespace std::string_literals;
using namespace std::string_view_literals;

namespace
{

using namespace orchestrator;
using namespace orchestrator::guest;

// {{{ the world the tests run in

constexpr auto WorktreeRootPath = "/home/u/worktrees"sv;
constexpr auto ContourPath = "/home/u/projects/contour"sv;
constexpr auto Session = "123e4567-e89b-12d3-a456-426614174000"sv;
/// 2026-11-13 15:08:47 UTC.
constexpr auto WallMs = uint64_t { 1'794'582'527'000 };
constexpr auto StartMonoMs = uint64_t { 1'000 };
constexpr auto Minute = uint64_t { 60'000 };
/// What one procTake() returns at most.
constexpr auto PageBytes = std::size_t { 65'536 };

/// The configuration: one repository, with a `base` when @p base is given.
[[nodiscard]] std::string configJson(std::string_view base = {})
{
    auto const baseKey = base.empty() ? std::string {} : std::format(R"("base": "{}",)", base);
    return std::format(R"({{ "worktree_root": "~/worktrees", "models": ["opus", "sonnet"],
  "repos": [ {{ "name": "contour", "path": "~/projects/contour", "forge": "github", {}
                "issues": [ {{ "source": "forge", "query": "assignee:@me" }} ] }} ],
  "tasks": [
    {{ "id": "fix_ci", "key": "f", "label": "fix CI", "applies_to": "pr", "model": "sonnet",
       "prompt": "/contour-workflows:fix-ci {{url}}" }},
    {{ "id": "issue", "key": "w", "label": "work on issue", "applies_to": "issue", "model": "opus",
       "prompt": "/contour-workflows:work-issue {{url}}" }} ] }})",
                       baseKey);
}

[[nodiscard]] HostInfo hostInfo()
{
    return HostInfo { .home = "/home/u",
                      .worktreeRoot = std::string(WorktreeRootPath),
                      .repoPaths = { std::string(ContourPath) },
                      .utcOffsetMinutes = 0 };
}

/// What a declared command answers in the fake.
struct Answer
{
    int32_t exitCode = 0;   ///< Its exit code.
    std::string stdoutText; ///< What it prints.
};

/// The recorded answers of every command a poll runs.
[[nodiscard]] std::map<std::string, Answer, std::less<>> pollAnswers()
{
    return {
        { "gh-auth", Answer { .exitCode = 0, .stdoutText = {} } },
        { "agent-setup-check",
          Answer { .exitCode = 0,
                   .stdoutText = readFixture(FixtureRoot::Testdata, "contour/setup-hooks-present.json") } },
        { "gh-default-branch",
          Answer { .exitCode = 0,
                   .stdoutText = readFixture(FixtureRoot::Testdata, "github/default-branch.json") } },
        { "gh-prs",
          Answer { .exitCode = 0,
                   .stdoutText = readFixture(FixtureRoot::Testdata, "github/prs-open.json") } },
        { "gh-issues",
          Answer { .exitCode = 0,
                   .stdoutText = readFixture(FixtureRoot::Testdata, "github/issues-open.json") } },
        { "git-worktree-list",
          Answer { .exitCode = 0,
                   .stdoutText = readFixture(FixtureRoot::Testdata, "git/worktree-list.porcelain-z") } },
    };
}

// }}}
// {{{ the fake host

/// A process in the fake's table.
struct FakeProcess
{
    uint32_t handle = 0;              ///< Its handle.
    std::string tag;                  ///< Its tag.
    std::string command;              ///< Its declared command.
    std::optional<ProcResult> result; ///< std::nullopt while it runs.
    std::string stdoutText;           ///< Its standard output, once finished.
    std::string stderrText;           ///< Its standard error, once finished.
};

/// The daemon world's host: a store, a process table completed on demand, and recordings of every call.
class FakeGuestHost final: public IGuestHost
{
  public:
    ClockReading clock { .monoMs = StartMonoMs, .wallMs = WallMs }; ///< What now() reads.
    std::map<std::string, std::string, std::less<>> store;          ///< The store.
    std::vector<FakeProcess> processes;                             ///< The processes not released.
    std::map<std::string, Answer, std::less<>>
        answers;                             ///< What completeRunning() finishes each command with.
    std::optional<HostError> procStartError; ///< What every procStart() answers, if set.
    std::optional<HostError> storeSetError;  ///< What every storeSet() answers, if set.
    std::expected<std::vector<PaneInfo>, HostError> paneListing = std::unexpected(HostError::NotSupportedYet);

    std::vector<std::string> calls;                             ///< The calls the order of which tests check.
    std::vector<std::pair<LogLevel, std::string>> logged;       ///< Every log().
    std::vector<uint32_t> wakes;                                ///< Every wake().
    std::vector<std::pair<std::string, std::string>> summaries; ///< Every summarySet().
    std::vector<std::string> started;   ///< The command of every procStart() that started.
    std::vector<std::string> paneOpens; ///< The tag of every paneOpen().

    /// Adds a finished process.
    uint32_t addFinished(std::string_view tag,
                         std::string_view command,
                         int32_t exitCode,
                         std::string stdoutText,
                         std::string stderrText = {})
    {
        auto& process = processes.emplace_back(FakeProcess { .handle = _nextHandle++,
                                                             .tag = std::string(tag),
                                                             .command = std::string(command),
                                                             .result = std::nullopt,
                                                             .stdoutText = {},
                                                             .stderrText = {} });
        finish(process, exitCode, std::move(stdoutText), std::move(stderrText));
        return process.handle;
    }

    /// Adds a running process.
    uint32_t addRunning(std::string_view tag, std::string_view command)
    {
        processes.push_back(FakeProcess { .handle = _nextHandle++,
                                          .tag = std::string(tag),
                                          .command = std::string(command),
                                          .result = std::nullopt,
                                          .stdoutText = {},
                                          .stderrText = {} });
        return processes.back().handle;
    }

    /// Finishes a running process.
    static void finish(FakeProcess& process,
                       int32_t exitCode,
                       std::string stdoutText,
                       std::string stderrText = {})
    {
        process.result = ProcResult { .completion = ProcCompletion::Exited,
                                      .exitCode = exitCode,
                                      .stdoutBytes = static_cast<uint32_t>(stdoutText.size()),
                                      .stdoutExtent = OutputExtent::Whole,
                                      .stderrBytes = static_cast<uint32_t>(stderrText.size()),
                                      .stderrExtent = OutputExtent::Whole };
        process.stdoutText = std::move(stdoutText);
        process.stderrText = std::move(stderrText);
    }

    /// @return The running process of a command, if any.
    [[nodiscard]] FakeProcess* running(std::string_view command)
    {
        auto const found = std::ranges::find_if(processes, [&](FakeProcess const& process) {
            return process.command == command && !process.result;
        });
        return found != processes.end() ? &*found : nullptr;
    }

    /// Finishes every running process whose command has an answer.
    /// @return How many it finished.
    std::size_t completeRunning()
    {
        auto count = std::size_t { 0 };
        for (auto& process: processes)
        {
            if (process.result)
                continue;
            auto const answer = answers.find(process.command);
            if (answer == answers.end())
                continue;
            finish(process, answer->second.exitCode, answer->second.stdoutText);
            ++count;
        }
        return count;
    }

    /// @return How many times @p command was started.
    [[nodiscard]] std::size_t startsOf(std::string_view command) const
    {
        return static_cast<std::size_t>(std::ranges::count(started, command));
    }

    /// @return The position of the first call @p call in calls; calls.size() if there is none.
    [[nodiscard]] std::size_t positionOf(std::string_view call) const
    {
        return static_cast<std::size_t>(std::ranges::distance(calls.begin(), std::ranges::find(calls, call)));
    }

    void log(LogLevel level, std::string_view message) override
    {
        logged.emplace_back(level, std::string(message));
    }

    void wake(uint32_t afterMs) override { wakes.push_back(afterMs); }

    [[nodiscard]] ClockReading now() override { return clock; }

    [[nodiscard]] std::expected<uint32_t, HostError> procStart(std::string_view command,
                                                               std::span<Argument const> /*arguments*/,
                                                               std::string_view /*cwd*/,
                                                               std::string_view tag) override
    {
        if (procStartError)
            return std::unexpected(*procStartError);
        started.emplace_back(command);
        return addRunning(tag, command);
    }

    [[nodiscard]] std::expected<std::vector<ProcInfo>, HostError> procs() override
    {
        auto listed = std::vector<ProcInfo> {};
        for (auto const& process: processes)
            listed.push_back(ProcInfo { .handle = process.handle,
                                        .tag = process.tag,
                                        .command = process.command,
                                        .result = process.result });
        return listed;
    }

    [[nodiscard]] std::expected<Chunk, HostError> procTake(uint32_t handle,
                                                           ProcStream stream,
                                                           uint32_t offset) override
    {
        calls.push_back(std::format(
            "procTake {} {} {}", handle, stream == ProcStream::Stdout ? "stdout" : "stderr", offset));
        auto const* const process = find(handle);
        if (process == nullptr || !process->result)
            return std::unexpected(HostError::Gone);
        auto const& text = stream == ProcStream::Stdout ? process->stdoutText : process->stderrText;
        if (offset > text.size())
            return std::unexpected(HostError::BadArgument);
        auto const page = std::string_view(text).substr(offset, PageBytes);
        return Chunk { .bytes = std::string(page),
                       .remaining = static_cast<uint32_t>(text.size() - offset - page.size()) };
    }

    [[nodiscard]] std::expected<void, HostError> procRelease(uint32_t handle) override
    {
        calls.push_back(std::format("procRelease {}", handle));
        if (std::erase_if(processes, [&](FakeProcess const& process) { return process.handle == handle; })
            == 0)
            return std::unexpected(HostError::Gone);
        return {};
    }

    [[nodiscard]] std::expected<std::optional<std::string>, HostError> storeGet(std::string_view key) override
    {
        auto const found = store.find(key);
        return found != store.end() ? std::optional { found->second } : std::nullopt;
    }

    [[nodiscard]] std::expected<void, HostError> storeSet(std::string_view key,
                                                          std::string_view value) override
    {
        calls.push_back(std::format("storeSet {}", key));
        if (storeSetError)
            return std::unexpected(*storeSetError);
        store.insert_or_assign(std::string(key), std::string(value));
        return {};
    }

    [[nodiscard]] std::expected<void, HostError> storeRemove(std::string_view key) override
    {
        calls.push_back(std::format("storeRemove {}", key));
        if (auto const found = store.find(key); found != store.end())
            store.erase(found);
        return {};
    }

    [[nodiscard]] std::expected<std::vector<std::string>, HostError> storeKeys(
        std::string_view prefix) override
    {
        auto keys = std::vector<std::string> {};
        for (auto const& key: store | std::views::keys)
            if (key.starts_with(prefix))
                keys.push_back(key);
        return keys;
    }

    [[nodiscard]] std::expected<std::string, HostError> paneOpen(std::string_view /*paneTemplate*/,
                                                                 std::span<Argument const> /*arguments*/,
                                                                 std::string_view /*cwd*/,
                                                                 std::string_view /*title*/,
                                                                 std::string_view tag) override
    {
        paneOpens.emplace_back(tag);
        return std::unexpected(HostError::NotSupportedYet);
    }

    [[nodiscard]] std::expected<void, HostError> paneClose(std::string_view /*pane*/) override
    {
        return std::unexpected(HostError::NotSupportedYet);
    }

    [[nodiscard]] std::expected<std::vector<PaneInfo>, HostError> panes() override { return paneListing; }

    [[nodiscard]] std::expected<std::string, FileError> fileRead(std::string_view /*root*/,
                                                                 std::string_view /*path*/) override
    {
        return std::unexpected(FileError::NotSupportedYet);
    }

    [[nodiscard]] std::expected<void, HostError> summarySet(std::string_view shortText,
                                                            std::string_view detail) override
    {
        summaries.emplace_back(shortText, detail);
        return {};
    }

  private:
    [[nodiscard]] FakeProcess const* find(uint32_t handle) const
    {
        auto const found = std::ranges::find(processes, handle, &FakeProcess::handle);
        return found != processes.end() ? &*found : nullptr;
    }

    uint32_t _nextHandle = 1;
};

/// Records what the driver feeds the engine and what the engine asks for.
class RecordingObserver final: public IDriverObserver
{
  public:
    std::vector<StartContext> starts; ///< Every start's context.
    std::vector<Input> inputs;        ///< Every input stepped.
    std::vector<Effect> effects;      ///< Every effect, of the starts and the steps.

    void starting(StartContext const& context) override { starts.push_back(context); }

    void started(std::span<Effect const> started) override
    {
        effects.insert(effects.end(), started.begin(), started.end());
    }

    void stepped(Input const& input, std::span<Effect const> stepped) override
    {
        inputs.push_back(input);
        effects.insert(effects.end(), stepped.begin(), stepped.end());
    }

    /// @return Every input of type T.
    template <typename T>
    [[nodiscard]] std::vector<T> inputsOf() const
    {
        auto found = std::vector<T> {};
        for (auto const& input: inputs)
            if (auto const* const typed = std::get_if<T>(&input))
                found.push_back(*typed);
        return found;
    }

    /// @return Every effect of type T.
    template <typename T>
    [[nodiscard]] std::vector<T> effectsOf() const
    {
        auto found = std::vector<T> {};
        for (auto const& effect: effects)
            if (auto const* const typed = std::get_if<T>(&effect))
                found.push_back(*typed);
        return found;
    }
};

// }}}
// {{{ a run, prepared by an engine of the test's own

/// A run's call that finished while the guest was away: the store as it was when the call was made, the call
/// and its answer.
struct AwayCall
{
    std::map<std::string, std::string, std::less<>> store; ///< The store before the answer is stepped.
    RunCommand command;                                    ///< The call.
    ProcessOutcome answer;                                 ///< Its answer.
};

/// The answer a dispatch's run gets to its commands: success, and the listing a worktree check reads.
[[nodiscard]] ProcessOutcome runAnswer(RunCommand const& command)
{
    auto outcome = ProcessOutcome { .start = StartResult::Started,
                                    .completion = Completion::Exited,
                                    .exitCode = 0,
                                    .extent = OutputExtent::Whole,
                                    .stdoutText = {},
                                    .stderrText = {} };
    if (command.command == "git-worktree-list"sv)
        outcome.stdoutText = std::format(
            "worktree {}\0HEAD e6e8346832d4ba7a25fbaa38301c781b5cbb5ebe\0branch refs/heads/master\0\0"sv,
            ContourPath);
    return outcome;
}

/// Dispatches an issue on an engine of the test's own, answers its run's commands, and stops before the
/// answer whose step asks for an effect of type T.
template <typename T>
[[nodiscard]] AwayCall awayCallBefore()
{
    auto context = StartContext { .monoMs = StartMonoMs,
                                  .wallMs = WallMs,
                                  .host = hostInfo(),
                                  .configJson = configJson("master"),
                                  .stored = {},
                                  .inFlight = {},
                                  .finished = {},
                                  .panes = {},
                                  .paneListing = PaneListing::Tagged };
    auto started = Engine::start(std::move(context));
    auto engine = std::move(started.engine);
    auto away = AwayCall {};
    auto calls = std::vector<RunCommand> {};
    auto const apply = [&](std::span<Effect const> effects) {
        for (auto const& effect: effects)
        {
            if (auto const* const set = std::get_if<StoreSet>(&effect))
                away.store.insert_or_assign(set->key, set->value);
            else if (auto const* const remove = std::get_if<StoreRemove>(&effect))
                away.store.erase(remove->key);
            else if (auto const* const command = std::get_if<RunCommand>(&effect);
                     command != nullptr && !command->tag.starts_with("poll/"))
                calls.push_back(*command);
        }
    };
    apply(started.effects);
    apply(engine.step(Dispatch { .repo = "contour",
                                 .item = IssueRef { 2356 },
                                 .url = "https://github.com/contour-terminal/contour/issues/2356",
                                 .task = "issue",
                                 .model = "",
                                 .note = std::nullopt,
                                 .session = std::string(Session),
                                 .wallMs = WallMs,
                                 .monoMs = StartMonoMs }));
    for ([[maybe_unused]] auto const _: std::views::iota(0, 20))
    {
        REQUIRE_FALSE(calls.empty());
        auto const command = calls.front();
        calls.erase(calls.begin());
        auto const answer = runAnswer(command);
        auto const effects =
            engine.step(ProcessFinished { .tag = command.tag, .outcome = answer, .monoMs = StartMonoMs });
        if (std::ranges::any_of(effects,
                                [](Effect const& effect) { return std::holds_alternative<T>(effect); }))
        {
            away.command = command;
            away.answer = answer;
            return away;
        }
        apply(effects);
    }
    FAIL("no answer asked for the effect");
    return away;
}

/// Puts a call that finished while the guest was away into the fake.
uint32_t putAway(FakeGuestHost& host, AwayCall const& away)
{
    host.store = away.store;
    return host.addFinished(
        away.command.tag, away.command.command, *away.answer.exitCode, away.answer.stdoutText);
}

// }}}

// {{{ the store a measurement of the host starts from

/// How many runs the measured store holds: each waits for its worktree, which a slot of MaxInFlight runs.
constexpr auto MeasuredRuns = MaxInFlight;

/// Where the measured store is kept, as the host's [.measure] case reads it.
constexpr auto MeasuredStoreFixture = "store/runs-awaiting-worktree.tsv"sv;

/// The configuration the measured store was made with, on one line: room for every measured run.
constexpr auto MeasuredConfig =
    R"({"worktree_root":"~/worktrees","models":["opus","sonnet"],"max_agents":4,"max_agents_per_repo":4,)"
    R"("repos":[{"name":"contour","path":"~/projects/contour","forge":"github","base":"master",)"
    R"("issues":[{"source":"forge","query":"assignee:@me"}]}],)"
    R"("tasks":[{"id":"issue","key":"w","label":"work on issue","applies_to":"issue","model":"opus",)"
    R"("prompt":"/contour-workflows:work-issue {url}"}]})"sv;

/// The store the host's [.measure] case seeds before each timed `update(procs)`, as text: a `config` line
/// with MeasuredConfig, then one `run` line per run -- its store key, the tag and the command of the call it
/// waits for (its worktree's `git worktree add`), and its stored value -- tab-separated. Made by an engine
/// of the test's own: MeasuredRuns issues dispatched, each one's fetch answered.
[[nodiscard]] std::string measuredStoreText()
{
    auto started = Engine::start(StartContext { .monoMs = StartMonoMs,
                                                .wallMs = WallMs,
                                                .host = hostInfo(),
                                                .configJson = std::string(MeasuredConfig),
                                                .stored = {},
                                                .inFlight = {},
                                                .finished = {},
                                                .panes = {},
                                                .paneListing = PaneListing::Tagged });
    REQUIRE(started.engine.problems().empty());
    auto engine = std::move(started.engine);
    auto store = std::map<std::string, std::string, std::less<>> {};
    auto calls = std::vector<RunCommand> {};
    auto const apply = [&](std::span<Effect const> effects) {
        for (auto const& effect: effects)
        {
            if (auto const* const set = std::get_if<StoreSet>(&effect))
                store.insert_or_assign(set->key, set->value);
            else if (auto const* const command = std::get_if<RunCommand>(&effect))
                calls.push_back(*command);
        }
    };
    // The poll's commands answer at once, so that every slot is free for the runs.
    apply(started.effects);
    auto const answers = pollAnswers();
    for ([[maybe_unused]] auto const _: std::views::iota(0, 32))
    {
        auto const polls = std::exchange(calls, {});
        if (polls.empty())
            break;
        for (auto const& poll: polls)
        {
            auto const answer = answers.find(poll.command);
            apply(engine.step(ProcessFinished {
                .tag = poll.tag,
                .outcome = ProcessOutcome { .start = StartResult::Started,
                                            .completion = Completion::Exited,
                                            .exitCode = 0,
                                            .extent = OutputExtent::Whole,
                                            .stdoutText = answer != answers.end() ? answer->second.stdoutText
                                                                                  : std::string {},
                                            .stderrText = {} },
                .monoMs = StartMonoMs }));
        }
    }
    REQUIRE(calls.empty());

    auto text = std::format("config\t{}\n", MeasuredConfig);
    for (auto const index: std::views::iota(std::size_t { 0 }, MeasuredRuns))
    {
        apply(engine.step(Dispatch {
            .repo = "contour",
            .item = IssueRef { 3001 + index },
            .url = std::format("https://github.com/contour-terminal/contour/issues/{}", 3001 + index),
            .task = "issue",
            .model = "",
            .note = std::nullopt,
            .session = std::format("123e4567-e89b-12d3-a456-4266141740{:02}", index),
            .wallMs = WallMs,
            .monoMs = StartMonoMs }));
        REQUIRE(calls.size() == 1);
        auto const fetch = std::exchange(calls, {}).front();
        REQUIRE(fetch.command == "git-fetch"sv);
        apply(engine.step(
            ProcessFinished { .tag = fetch.tag, .outcome = runAnswer(fetch), .monoMs = StartMonoMs }));
        REQUIRE(calls.size() == 1);
        auto const worktree = std::exchange(calls, {}).front();
        REQUIRE(worktree.command.starts_with("git-worktree-add"));
        auto const key = runKey(worktree.tag.substr(0, worktree.tag.find('/')));
        REQUIRE(store.contains(key));
        text += std::format(
            "run\t{}\t{}\t{}\t{}\n", key, worktree.tag, worktree.command, store.find(key)->second);
    }
    return text;
}

// }}}

} // namespace

TEST_CASE("load takes a finished result in contiguous pages", "[guest]")
{
    auto host = FakeGuestHost {};
    auto prs = readFixture(FixtureRoot::Testdata, "github/prs-open.json");
    constexpr auto PaddedBytes = std::size_t { 150 } * 1024;
    REQUIRE(prs.size() < PaddedBytes);
    // JSON ignores the trailing blanks; they make the result three pages long.
    prs.resize(PaddedBytes, ' ');
    auto const handle = host.addFinished("poll/contour/prs", "gh-prs", 0, prs);
    auto observer = RecordingObserver {};

    auto const driver = Driver::load(host, configJson(), hostInfo(), observer);

    auto takes = std::vector<std::string> {};
    for (auto const& call: host.calls)
        if (call.starts_with("procTake") || call.starts_with("procRelease"))
            takes.push_back(call);
    CHECK(takes
          == std::vector {
              std::format("procTake {} stdout 0", handle),
              std::format("procTake {} stdout 65536", handle),
              std::format("procTake {} stdout 131072", handle),
              std::format("procTake {} stderr 0", handle),
              std::format("procRelease {}", handle),
          });
    REQUIRE(observer.starts.size() == 1);
    REQUIRE(observer.starts.front().finished.size() == 1);
    auto const& finished = observer.starts.front().finished.front();
    CHECK(finished.tag == "poll/contour/prs");
    CHECK(finished.outcome.start == StartResult::Started);
    CHECK(finished.outcome.exitCode == 0);
    CHECK(finished.outcome.extent == OutputExtent::Whole);
    CHECK(finished.outcome.stdoutText == prs);
    CHECK(driver.engine().changeRequests("contour").size() == 5);
    CHECK(std::ranges::find(host.processes, handle, &FakeProcess::handle) == host.processes.end());
    // Its list is taken in, not run again.
    CHECK(host.startsOf("gh-prs") == 0);
}

TEST_CASE("load lists running processes as in flight", "[guest]")
{
    auto host = FakeGuestHost {};
    host.addRunning("poll/auth", "gh-auth");
    auto observer = RecordingObserver {};

    [[maybe_unused]] auto const driver = Driver::load(host, configJson(), hostInfo(), observer);

    REQUIRE(observer.starts.size() == 1);
    CHECK(observer.starts.front().inFlight == std::vector { "poll/auth"s });
    CHECK(observer.starts.front().finished.empty());
    CHECK(std::ranges::none_of(host.calls,
                               [](std::string const& call) { return call.starts_with("procTake"); }));
    // The login check the host still runs is not started again.
    CHECK(host.startsOf("gh-auth") == 0);
}

TEST_CASE("a result is released after its step's store writes", "[guest]")
{
    auto const away = awayCallBefore<StoreSet>();
    auto host = FakeGuestHost {};

    SECTION("finished while the guest was away")
    {
        auto const handle = putAway(host, away);
        [[maybe_unused]] auto const driver = Driver::load(host, configJson("master"), hostInfo());
        auto const set = host.positionOf(
            std::format("storeSet run/{}", away.command.tag.substr(0, away.command.tag.find('/'))));
        auto const release = host.positionOf(std::format("procRelease {}", handle));
        REQUIRE(set < host.calls.size());
        REQUIRE(release < host.calls.size());
        CHECK(set < release);
    }

    SECTION("finished while the guest ran")
    {
        host.store = away.store;
        auto const handle = host.addRunning(away.command.tag, away.command.command);
        auto driver = Driver::load(host, configJson("master"), hostInfo());
        host.calls.clear();
        FakeGuestHost::finish(host.processes.front(), 0, away.answer.stdoutText);
        REQUIRE(host.processes.front().handle == handle);

        driver.update(Hint::Procs);

        auto const set = host.positionOf(
            std::format("storeSet run/{}", away.command.tag.substr(0, away.command.tag.find('/'))));
        auto const release = host.positionOf(std::format("procRelease {}", handle));
        REQUIRE(set < host.calls.size());
        REQUIRE(release < host.calls.size());
        CHECK(set < release);
    }
}

TEST_CASE("a finished process with no Procs hint is consumed on the next Wake", "[guest]")
{
    auto host = FakeGuestHost {};
    auto observer = RecordingObserver {};
    auto driver = Driver::load(host, configJson(), hostInfo(), observer);
    auto* const auth = host.running("gh-auth");
    REQUIRE(auth != nullptr);
    auto const handle = auth->handle;
    FakeGuestHost::finish(*auth, 0, {});

    driver.update(Hint::Wake);

    auto const finished = observer.inputsOf<ProcessFinished>();
    REQUIRE(finished.size() == 1);
    CHECK(finished.front().tag == "poll/auth");
    CHECK(finished.front().monoMs == StartMonoMs);
    CHECK(host.positionOf(std::format("procRelease {}", handle)) < host.calls.size());
    // It is consumed once: a later hint does not step it again.
    driver.update(Hint::Procs);
    CHECK(observer.inputsOf<ProcessFinished>().size() == 1);
}

TEST_CASE("a missing program is refused, never gone", "[guest]")
{
    auto host = FakeGuestHost {};
    host.procStartError = HostError::BadArgument;
    auto observer = RecordingObserver {};

    [[maybe_unused]] auto const driver = Driver::load(host, configJson(), hostInfo(), observer);

    auto const finished = observer.inputsOf<ProcessFinished>();
    REQUIRE_FALSE(finished.empty());
    for (auto const& reply: finished)
    {
        CAPTURE(reply.tag);
        CHECK(reply.outcome.start == StartResult::Refused);
        CHECK_FALSE(reply.outcome.exitCode.has_value());
    }
}

TEST_CASE("exactly one reply per RunCommand", "[guest]")
{
    struct Row
    {
        HostError error;   ///< What procStart() answers.
        StartResult start; ///< What the reply says.
    };
    static constexpr auto Rows = std::array {
        Row { .error = HostError::Gone, .start = StartResult::CwdMissing },
        Row { .error = HostError::Limit, .start = StartResult::Refused },
        Row { .error = HostError::NotGranted, .start = StartResult::Refused },
    };
    for (auto const& row: Rows)
    {
        CAPTURE(nameOf(row.error));
        auto host = FakeGuestHost {};
        host.procStartError = row.error;
        auto observer = RecordingObserver {};
        auto driver = Driver::load(host, configJson(), hostInfo(), observer);
        // Two more polls, each refused the same way.
        for ([[maybe_unused]] auto const _: std::views::iota(0, 2))
        {
            host.clock.monoMs += (60 * Minute);
            driver.update(Hint::Wake);
        }

        auto commands = std::map<std::string, std::size_t> {};
        for (auto const& command: observer.effectsOf<RunCommand>())
            ++commands[command.tag];
        auto replies = std::map<std::string, std::size_t> {};
        for (auto const& reply: observer.inputsOf<ProcessFinished>())
        {
            CHECK(reply.outcome.start == row.start);
            ++replies[reply.tag];
        }
        CHECK_FALSE(commands.empty());
        CHECK(replies == commands);
    }
}

TEST_CASE("exactly one reply per OpenPane", "[guest]")
{
    auto const away = awayCallBefore<OpenPane>();
    auto host = FakeGuestHost {};
    putAway(host, away);
    auto observer = RecordingObserver {};

    [[maybe_unused]] auto const driver = Driver::load(host, configJson("master"), hostInfo(), observer);

    auto const opens = observer.effectsOf<OpenPane>();
    REQUIRE(opens.size() == 1);
    CHECK(host.paneOpens == std::vector { opens.front().run });
    auto const failed = observer.inputsOf<PaneOpenFailedFor>();
    REQUIRE(failed.size() == 1);
    CHECK(failed.front().run == opens.front().run);
    CHECK(failed.front().why == "not-supported-yet");
    CHECK(observer.inputsOf<PaneOpenedFor>().empty());
}

TEST_CASE("wake is asked for the engine's latest timer only", "[guest]")
{
    SECTION("the timer")
    {
        auto timer = WakeTimer {};
        CHECK_FALSE(timer.unasked().has_value());
        timer.set(30'000);
        CHECK(timer.unasked() == 30'000);
        timer.asked(30'000);
        CHECK_FALSE(timer.unasked().has_value());
        // A WakeAt replaces the one before, earlier or later.
        timer.set(10'000);
        CHECK(timer.unasked() == 10'000);
        timer.asked(10'000);
        timer.set(10'000);
        CHECK_FALSE(timer.unasked().has_value());
        timer.set(20'000);
        CHECK(timer.unasked() == 20'000);
        timer.asked(20'000);

        CHECK_FALSE(timer.takeDue(19'999).has_value());
        CHECK(timer.takeDue(20'000) == 20'000);
        CHECK_FALSE(timer.takeDue(20'000).has_value());
        CHECK_FALSE(timer.unasked().has_value());
        timer.set(30'000);
        timer.asked(30'000);
        timer.requestUsed();
        CHECK(timer.unasked() == 30'000);

        CHECK(WakeTimer::delayUntil(30'000, 10'000) == 20'000);
        CHECK(WakeTimer::delayUntil(10'000, 30'000) == 0);
        CHECK(WakeTimer::delayUntil(uint64_t { 1 } << 40U, 0) == UINT32_MAX);
    }

    SECTION("the driver")
    {
        auto host = FakeGuestHost {};
        auto observer = RecordingObserver {};
        auto driver = Driver::load(host, configJson(), hostInfo(), observer);
        auto const wakeAts = observer.effectsOf<WakeAt>();
        REQUIRE(wakeAts.size() == 1);
        CHECK(host.wakes == std::vector { WakeTimer::delayUntil(wakeAts.front().monoMs, StartMonoMs) });

        // Nothing is due: nothing is stepped, and the used-up request is asked for again.
        driver.update(Hint::Wake);
        CHECK(observer.inputs.empty());
        CHECK(host.wakes.size() == 2);
        CHECK(host.wakes.back() == host.wakes.front());

        // A step that asks for the same time asks the host for nothing.
        auto* const auth = host.running("gh-auth");
        REQUIRE(auth != nullptr);
        FakeGuestHost::finish(*auth, 1, {});
        driver.update(Hint::Procs);
        CHECK(host.wakes.size() == 2 + observer.effectsOf<WakeAt>().size() - 1);
    }
}

TEST_CASE("panes not supported yet lists own records only", "[guest]")
{
    auto host = FakeGuestHost {};
    auto observer = RecordingObserver {};

    SECTION("not supported yet")
    {
        auto driver = Driver::load(host, configJson(), hostInfo(), observer);
        REQUIRE(observer.starts.size() == 1);
        CHECK(observer.starts.front().paneListing == PaneListing::OwnRecordsOnly);
        CHECK(observer.starts.front().panes.empty());

        auto const requestedMs = StartMonoMs + (10 * Minute);
        host.clock.monoMs = requestedMs;
        driver.update(Hint::Wake);

        REQUIRE(observer.inputs.size() >= 2);
        CHECK(std::holds_alternative<Wake>(observer.inputs.front()));
        auto const listed = observer.inputsOf<PanesListed>();
        REQUIRE(listed.size() == 1);
        CHECK(listed.front().listing == PaneListing::OwnRecordsOnly);
        CHECK(listed.front().panes.empty());
        CHECK(listed.front().requestedMonoMs == requestedMs);
    }

    SECTION("a tagged listing")
    {
        host.paneListing = std::vector { PaneInfo { .pane = "p1",
                                                    .tag = "pr-1-20261113-150847",
                                                    .status = AgentStatus::Working,
                                                    .program = ProgramLiveness::Alive } };
        [[maybe_unused]] auto const driver = Driver::load(host, configJson(), hostInfo(), observer);
        REQUIRE(observer.starts.size() == 1);
        CHECK(observer.starts.front().paneListing == PaneListing::Tagged);
        CHECK(observer.starts.front().panes
              == std::vector { PaneFact { .pane = "p1",
                                          .tag = "pr-1-20261113-150847",
                                          .status = AgentStatus::Working,
                                          .program = ProgramLiveness::Alive } });
    }
}

TEST_CASE("a failed store-set is logged, not fatal", "[guest]")
{
    auto const away = awayCallBefore<StoreSet>();
    auto host = FakeGuestHost {};
    putAway(host, away);
    host.storeSetError = HostError::Gone;
    auto observer = RecordingObserver {};

    auto driver = Driver::load(host, configJson("master"), hostInfo(), observer);

    auto const errors =
        std::ranges::count(host.logged, LogLevel::Error, &std::pair<LogLevel, std::string>::first);
    CHECK(errors == std::ranges::count_if(host.calls, [](std::string const& call) {
              return call.starts_with("storeSet");
          }));
    CHECK(errors >= 1);
    // The next hint is served.
    auto* const auth = host.running("gh-auth");
    REQUIRE(auth != nullptr);
    FakeGuestHost::finish(*auth, 0, {});
    driver.update(Hint::Procs);
    CHECK(std::ranges::any_of(observer.inputsOf<ProcessFinished>(),
                              [](ProcessFinished const& finished) { return finished.tag == "poll/auth"; }));
}

TEST_CASE("the summary reaches the host", "[guest]")
{
    auto host = FakeGuestHost {};
    host.answers = pollAnswers();
    auto observer = RecordingObserver {};
    auto driver = Driver::load(host, configJson(), hostInfo(), observer);
    while (host.completeRunning() > 0)
        driver.update(Hint::Procs);

    auto const published = observer.effectsOf<PublishSummary>();
    REQUIRE_FALSE(published.empty());
    REQUIRE_FALSE(host.summaries.empty());
    CHECK(host.summaries.back() == std::pair { published.back().shortText, published.back().detail });
    CHECK(host.summaries.back().first.starts_with("⬢ agents"));
    CHECK(driver.engine().changeRequests("contour").size() == 5);
    for (auto const& [command, answer]: pollAnswers())
    {
        CAPTURE(command);
        CHECK(host.startsOf(command) == 1);
    }
}

TEST_CASE("a restarted guest picks the result up by tag", "[guest]")
{
    auto host = FakeGuestHost {};
    host.answers = pollAnswers();
    {
        auto driver = Driver::load(host, configJson(), hostInfo());
        auto* const auth = host.running("gh-auth");
        REQUIRE(auth != nullptr);
        FakeGuestHost::finish(*auth, 0, {});
        driver.update(Hint::Procs);
        REQUIRE(host.running("gh-prs") != nullptr);
    }
    auto* const prs = host.running("gh-prs");
    FakeGuestHost::finish(*prs, 0, host.answers.at("gh-prs").stdoutText);

    auto const driver = Driver::load(host, configJson(), hostInfo());

    CHECK(host.startsOf("gh-prs") == 1);
    CHECK(host.startsOf("gh-auth") == 1);
    CHECK(driver.engine().changeRequests("contour").size() == 5);
}

TEST_CASE("the measured store is what the engine stores", "[guest]")
{
    INFO("regenerate with: orchestrator_test \"[.regenerate]\"");
    CHECK(measuredStoreText() == readFixture(FixtureRoot::Testdata, MeasuredStoreFixture));
}

TEST_CASE("regenerate the measured store", "[guest][.regenerate]")
{
    auto file = std::ofstream(std::filesystem::path(ORCHESTRATOR_TESTDATA_DIR) / MeasuredStoreFixture,
                              std::ios::binary);
    REQUIRE(file);
    file << measuredStoreText();
}
