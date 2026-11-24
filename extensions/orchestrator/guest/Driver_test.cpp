// SPDX-License-Identifier: Apache-2.0
#include <core/tui/TestHelpers.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <expected>
#include <filesystem>
#include <format>
#include <fstream>
#include <functional>
#include <map>
#include <optional>
#include <ranges>
#include <set>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

#include <orchestrator/core/Engine.hpp>
#include <orchestrator/core/FailureWords.hpp>
#include <orchestrator/core/Fixtures.hpp>
#include <orchestrator/guest/Driver.hpp>
#include <orchestrator/guest/HostCalls.hpp>
#include <orchestrator/view/Band.hpp>
#include <orchestrator/view/Dashboard.hpp>
#include <orchestrator/view/ViewFixtures.hpp>

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
/// What a frame starts with: the synchronized-output bracket.
constexpr auto SynchronizedOutput = "\x1b[?2026h"sv;
/// What a frame ends with.
constexpr auto SynchronizedOutputEnd = "\x1b[?2026l"sv;
/// The size the fake opens a tab at, unless a test says otherwise.
constexpr auto OpenCols = uint32_t { 120 };
constexpr auto OpenRows = uint32_t { 40 };

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
        { "claude-plugins",
          Answer { .exitCode = 0,
                   .stdoutText = readFixture(FixtureRoot::Testdata, "claude/plugin-list.json") } },
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
    std::expected<FileContent, FileError> fileContent =
        std::unexpected(FileError::NotSupportedYet); ///< What every fileRead() answers.
    std::vector<std::string> fileReads;              ///< The path of every fileRead().
    std::expected<std::vector<PaneInfo>, HostError> paneListing = std::unexpected(HostError::NotSupportedYet);

    std::vector<std::string> calls;                             ///< The calls the order of which tests check.
    std::vector<std::pair<LogLevel, std::string>> logged;       ///< Every log().
    std::vector<uint32_t> wakes;                                ///< Every wake().
    std::vector<std::pair<std::string, std::string>> summaries; ///< Every summarySet().
    std::vector<std::string> started;                      ///< The command of every procStart() that started.
    std::vector<std::string> paneOpens;                    ///< The tag of every paneOpen().
    std::vector<std::optional<std::string>> paneOpenNears; ///< The near of every paneOpen().
    std::vector<std::string> paneCloses;                   ///< Every paneClose().
    std::vector<std::pair<std::string, uint32_t>> paneReveals;  ///< Every paneReveal(): pane and origin.
    std::vector<std::pair<std::string, std::string>> paneSends; ///< Every paneSend(): pane and text.
    std::expected<void, HostError> paneSendAnswer {};           ///< What paneSend() and paneKey() answer.
    std::vector<std::pair<std::string, PaneKey>> paneKeys;      ///< Every paneKey(): pane and key.
    std::vector<std::pair<std::string, uint32_t>> urlOpens;     ///< Every urlOpen(): link and origin.
    std::expected<void, HostError> urlOpenAnswer {};            ///< What urlOpen() answers.

    std::vector<TabInfo> tabList;                                  ///< What tabs() lists.
    std::pair<uint32_t, uint32_t> openSize { OpenCols, OpenRows }; ///< The size tabOpen() opens at.
    std::map<std::string, std::deque<std::string>, std::less<>>
        typed;                                               ///< Per tab, what each tabRead() answers.
    std::set<std::string, std::less<>> failingWrites;        ///< The tabs whose tabWrite() fails.
    std::vector<ActionInfo> queuedActions;                   ///< What the next actionsTake() answers.
    std::vector<std::pair<std::string, uint32_t>> tabOpens;  ///< Every tabOpen(): scheme and origin.
    std::vector<std::pair<std::string, std::string>> writes; ///< Every tabWrite() that succeeded.
    view::test::CountingUuids ids; ///< The session ids handed to the driver beside this host.

    /// @return Everything written to @p tab, in order.
    [[nodiscard]] std::string writtenTo(std::string_view tab) const
    {
        auto text = std::string {};
        for (auto const& [to, bytes]: writes)
            if (to == tab)
                text += bytes;
        return text;
    }

    /// @return How many writes reached @p tab.
    [[nodiscard]] std::size_t writesTo(std::string_view tab) const
    {
        return static_cast<std::size_t>(std::ranges::count(writes | std::views::keys, tab));
    }

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

    [[nodiscard]] std::expected<std::string, HostError> paneOpen(
        std::string_view /*paneTemplate*/,
        std::span<Argument const> /*arguments*/,
        std::string_view /*cwd*/,
        std::string_view /*title*/,
        std::string_view tag,
        std::optional<std::string_view> nearTab) override
    {
        paneOpens.emplace_back(tag);
        paneOpenNears.push_back(nearTab.transform([](std::string_view tab) { return std::string { tab }; }));
        return std::unexpected(HostError::NotSupportedYet);
    }

    [[nodiscard]] std::expected<void, HostError> paneClose(std::string_view pane) override
    {
        paneCloses.emplace_back(pane);
        return std::unexpected(HostError::NotSupportedYet);
    }

    [[nodiscard]] std::expected<std::vector<PaneInfo>, HostError> panes() override { return paneListing; }

    [[nodiscard]] std::expected<void, HostError> paneReveal(std::string_view pane, uint32_t origin) override
    {
        paneReveals.emplace_back(std::string(pane), origin);
        return {};
    }

    [[nodiscard]] std::expected<void, HostError> paneSend(std::string_view pane,
                                                          std::string_view text) override
    {
        paneSends.emplace_back(std::string(pane), std::string(text));
        return paneSendAnswer;
    }

    [[nodiscard]] std::expected<void, HostError> urlOpen(std::string_view url, uint32_t origin) override
    {
        urlOpens.emplace_back(std::string(url), origin);
        return urlOpenAnswer;
    }

    [[nodiscard]] std::expected<void, HostError> paneKey(std::string_view pane, PaneKey key) override
    {
        paneKeys.emplace_back(std::string(pane), key);
        return paneSendAnswer;
    }

    [[nodiscard]] std::expected<FileContent, FileError> fileRead(std::string_view /*root*/,
                                                                 std::string_view path) override
    {
        fileReads.emplace_back(path);
        return fileContent;
    }

    [[nodiscard]] std::expected<void, HostError> summarySet(std::string_view shortText,
                                                            std::string_view detail) override
    {
        summaries.emplace_back(shortText, detail);
        return {};
    }

    [[nodiscard]] std::expected<std::string, HostError> tabOpen(std::string_view scheme,
                                                                uint32_t origin) override
    {
        tabOpens.emplace_back(std::string(scheme), origin);
        // The host reveals the tab already open, and answers its id.
        if (auto const open = std::ranges::find(tabList, scheme, &TabInfo::scheme); open != tabList.end())
            return open->tab;
        auto& tab = tabList.emplace_back(TabInfo { .tab = std::format("tab-{}", _nextTab++),
                                                   .scheme = std::string(scheme),
                                                   .cols = openSize.first,
                                                   .rows = openSize.second });
        return tab.tab;
    }

    [[nodiscard]] std::expected<void, HostError> tabReveal(std::string_view /*tab*/,
                                                           uint32_t /*origin*/) override
    {
        return {};
    }

    [[nodiscard]] std::expected<std::vector<TabInfo>, HostError> tabs() override { return tabList; }

    [[nodiscard]] std::expected<std::string, HostError> tabRead(std::string_view tab) override
    {
        auto const found = typed.find(tab);
        if (found == typed.end() || found->second.empty())
            return std::string {};
        auto bytes = std::move(found->second.front());
        found->second.pop_front();
        return bytes;
    }

    [[nodiscard]] std::expected<void, HostError> tabWrite(std::string_view tab,
                                                          std::string_view bytes) override
    {
        if (failingWrites.contains(tab))
            return std::unexpected(HostError::Gone);
        writes.emplace_back(std::string(tab), std::string(bytes));
        return {};
    }

    [[nodiscard]] std::expected<std::vector<ActionInfo>, HostError> actionsTake() override
    {
        return std::exchange(queuedActions, {});
    }

  private:
    [[nodiscard]] FakeProcess const* find(uint32_t handle) const
    {
        auto const found = std::ranges::find(processes, handle, &FakeProcess::handle);
        return found != processes.end() ? &*found : nullptr;
    }

    uint32_t _nextHandle = 1;
    uint32_t _nextTab = 1;
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
                                 .nearTab = std::nullopt,
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
            .nearTab = std::nullopt,
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
// {{{ the served tab

/// A driver loaded on a fake whose every poll is answered, so that the dashboard has rows.
struct Served
{
    Served() { host.answers = pollAnswers(); }

    /// Loads the driver and answers its poll.
    void load()
    {
        driver.emplace(Driver::load(host, host.ids, configJson(), info, observer));
        while (host.completeRunning() > 0)
            driver->update(Hint::Procs);
    }

    /// Invokes an action, as the user does from the palette.
    void invoke(std::string_view action, uint32_t origin = 7)
    {
        host.queuedActions.push_back(ActionInfo { .id = std::string(action), .origin = origin });
        driver->update(Hint::Actions);
    }

    /// Types @p bytes into @p tab, as one chunk the next tabRead() answers, and tells the driver.
    void type(std::string_view bytes, std::string_view tab = "tab-1")
    {
        host.typed[std::string(tab)].emplace_back(bytes);
        driver->update(Hint::Tabs);
    }

    [[nodiscard]] view::Dashboard const& dashboard(std::string_view tab = "tab-1") const
    {
        auto const* const found = driver->dashboard(tab);
        REQUIRE(found != nullptr);
        return *found;
    }

    FakeGuestHost host;
    RecordingObserver observer;
    HostInfo info = hostInfo(); ///< What the host resolved, handed to load().
    std::optional<Driver> driver;
};

/// @return How often @p needle occurs in @p text.
[[nodiscard]] std::size_t countOf(std::string_view text, std::string_view needle)
{
    auto count = std::size_t { 0 };
    auto at = text.find(needle);
    while (at != std::string_view::npos)
    {
        ++count;
        at = text.find(needle, at + needle.size());
    }
    return count;
}

/// @return The index into BandBreakpoints of a band.
[[nodiscard]] std::size_t shownBreakpointOf(view::Band band)
{
    auto const found = std::ranges::find(view::BandBreakpoints, band, &view::BandBreakpoint::band);
    return static_cast<std::size_t>(std::ranges::distance(view::BandBreakpoints.begin(), found));
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

    auto const driver = Driver::load(host, host.ids, configJson(), hostInfo(), observer);

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

TEST_CASE("an auth check's output is released unread", "[guest]")
{
    auto host = FakeGuestHost {};
    auto const handle = host.addFinished("poll/auth", "gh-auth", 1, "token gho_secret");
    auto observer = RecordingObserver {};

    [[maybe_unused]] auto const driver = Driver::load(host, host.ids, configJson(), hostInfo(), observer);

    CHECK(std::ranges::none_of(host.calls,
                               [](std::string const& call) { return call.starts_with("procTake"); }));
    CHECK(std::ranges::contains(host.calls, std::format("procRelease {}", handle)));
    REQUIRE(observer.starts.size() == 1);
    REQUIRE(observer.starts.front().finished.size() == 1);
    auto const& outcome = observer.starts.front().finished.front().outcome;
    CHECK(outcome.exitCode == 1);
    CHECK(outcome.stdoutText.empty());
    CHECK(outcome.stderrText.empty());
}

TEST_CASE("load lists running processes as in flight", "[guest]")
{
    auto host = FakeGuestHost {};
    host.addRunning("poll/auth", "gh-auth");
    auto observer = RecordingObserver {};

    [[maybe_unused]] auto const driver = Driver::load(host, host.ids, configJson(), hostInfo(), observer);

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
        [[maybe_unused]] auto const driver = Driver::load(host, host.ids, configJson("master"), hostInfo());
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
        auto driver = Driver::load(host, host.ids, configJson("master"), hostInfo());
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
    auto driver = Driver::load(host, host.ids, configJson(), hostInfo(), observer);
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

    [[maybe_unused]] auto const driver = Driver::load(host, host.ids, configJson(), hostInfo(), observer);

    auto const finished = observer.inputsOf<ProcessFinished>();
    REQUIRE_FALSE(finished.empty());
    for (auto const& reply: finished)
    {
        CAPTURE(reply.tag);
        CHECK(reply.outcome.start == StartResult::Refused);
        CHECK_FALSE(reply.outcome.exitCode.has_value());
        // The refusal's name, which tells the engine a run's set was refused.
        CHECK(reply.outcome.stderrText == refusedCommandText(BadArgumentRefusal));
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
        auto driver = Driver::load(host, host.ids, configJson(), hostInfo(), observer);
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

    [[maybe_unused]] auto const driver =
        Driver::load(host, host.ids, configJson("master"), hostInfo(), observer);

    auto const opens = observer.effectsOf<OpenPane>();
    REQUIRE(opens.size() == 1);
    CHECK(host.paneOpens == std::vector { opens.front().run });
    auto const failed = observer.inputsOf<PaneOpenFailedFor>();
    REQUIRE(failed.size() == 1);
    CHECK(failed.front().run == opens.front().run);
    CHECK(failed.front().why == "not-supported-yet");
    CHECK(observer.inputsOf<PaneOpenedFor>().empty());
}

TEST_CASE("every error the daemon world answers has plain words on the dashboard", "[guest]")
{
    // A run's failure carries the code its host call answered (nameOf); the dashboard must never show one
    // bare.
    auto const hasRow = [](std::string_view code) {
        return std::ranges::contains(failureWords(), code, &FailureWords::code);
    };
    for (auto const value: std::views::iota(std::size_t { 0 }, HostErrorCount))
    {
        auto const code = nameOf(static_cast<HostError>(value));
        INFO(code);
        CHECK(hasRow(code));
    }
    for (auto const value: std::views::iota(std::size_t { 0 }, FileErrorCount))
    {
        auto const code = nameOf(static_cast<FileError>(value));
        INFO(code);
        CHECK(hasRow(code));
    }
}

TEST_CASE("a refused pane-open lists the panes at once", "[guest][panes]")
{
    // A host that lists panes, but refuses this one (a Flatpak, Windows): nothing was opened, so no Panes
    // hint follows, and the run must not wait for the next poll to learn that no tab is tagged with it.
    auto const away = awayCallBefore<OpenPane>();
    auto host = FakeGuestHost {};
    host.paneListing = std::vector<PaneInfo> {};
    putAway(host, away);
    auto observer = RecordingObserver {};

    auto driver = Driver::load(host, host.ids, configJson("master"), hostInfo(), observer);
    // A listing in the millisecond the run changed would tell it nothing: the guest asks to wake a moment
    // later, and lists then.
    REQUIRE_FALSE(host.wakes.empty());
    CHECK(host.wakes.back() == 1);
    host.clock.monoMs += host.wakes.back();
    driver.update(Hint::Wake);

    auto const failed = std::ranges::find_if(
        observer.inputs, [](Input const& input) { return std::holds_alternative<PaneOpenFailedFor>(input); });
    REQUIRE(failed != observer.inputs.end());
    CHECK(std::ranges::any_of(std::next(failed), observer.inputs.end(), [](Input const& input) {
        return std::holds_alternative<PanesListed>(input);
    }));
    // Past the wait for its tab, on to the removal checks.
    REQUIRE(host.store.size() == 1);
    auto const run = runFromJson(host.store.begin()->second);
    REQUIRE(run.has_value());
    auto const* const ending = std::get_if<Ending>(&run->state);
    REQUIRE(ending != nullptr);
    CHECK(ending->cause == Ended::PreparationFailed);
    CHECK(ending->step != EndStep::ClosePane);
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
        auto driver = Driver::load(host, host.ids, configJson(), hostInfo(), observer);
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
        auto driver = Driver::load(host, host.ids, configJson(), hostInfo(), observer);
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
                                                    .message = {},
                                                    .program = ProgramLiveness::Alive } };
        [[maybe_unused]] auto const driver = Driver::load(host, host.ids, configJson(), hostInfo(), observer);
        REQUIRE(observer.starts.size() == 1);
        CHECK(observer.starts.front().paneListing == PaneListing::Tagged);
        CHECK(observer.starts.front().panes
              == std::vector { PaneFact { .pane = "p1",
                                          .tag = "pr-1-20261113-150847",
                                          .status = AgentStatus::Working,
                                          .message = {},
                                          .program = ProgramLiveness::Alive } });
    }
}

TEST_CASE("the Panes hint lists the panes", "[guest][panes]")
{
    auto host = FakeGuestHost {};
    host.paneListing = std::vector<PaneInfo> {};
    auto observer = RecordingObserver {};
    auto driver = Driver::load(host, host.ids, configJson(), hostInfo(), observer);
    observer.inputs.clear();
    host.paneListing = std::vector { PaneInfo { .pane = "p1",
                                                .tag = "pr-1-20261113-150847",
                                                .status = AgentStatus::Asking,
                                                .message = {},
                                                .program = ProgramLiveness::Alive } };

    driver.update(Hint::Panes);

    auto const listed = observer.inputsOf<PanesListed>();
    REQUIRE(listed.size() == 1);
    CHECK(listed.front().listing == PaneListing::Tagged);
    CHECK(listed.front().panes
          == std::vector { PaneFact { .pane = "p1",
                                      .tag = "pr-1-20261113-150847",
                                      .status = AgentStatus::Asking,
                                      .message = {},
                                      .program = ProgramLiveness::Alive } });
}

TEST_CASE("a listing answered limit tells the engine nothing", "[guest][panes]")
{
    auto host = FakeGuestHost {};
    host.paneListing = std::vector<PaneInfo> {};
    auto observer = RecordingObserver {};
    auto driver = Driver::load(host, host.ids, configJson(), hostInfo(), observer);
    observer.inputs.clear();
    host.paneListing = std::unexpected(HostError::Limit);

    driver.update(Hint::Panes);
    driver.update(Hint::Wake);

    // No PanesListed, and no fall back to the driver's own records: nothing was listed.
    CHECK(observer.inputsOf<PanesListed>().empty());
}

TEST_CASE("a load whose listing answers limit ends no run", "[guest][panes]")
{
    // The host has not restored its sessions yet (a boot right after an upgrade): its listing answers limit.
    auto served = Served {};
    auto const run = view::test::storedRun(Running {}, "pr-2375-20261113-150847", ChangeRequestRef { 2375 });
    served.host.store.insert_or_assign(runKey(run.id), toJson(run));
    served.host.paneListing = std::unexpected(HostError::Limit);
    served.load();

    REQUIRE(served.observer.starts.size() == 1);
    CHECK(served.observer.starts.front().paneListing == PaneListing::Unknown);
    CHECK(served.observer.starts.front().panes.empty());
    auto const stored = [&] {
        auto const found = served.host.store.find(runKey(run.id));
        REQUIRE(found != served.host.store.end());
        auto read = runFromJson(found->second);
        REQUIRE(read.has_value());
        return *read;
    };
    CHECK(std::holds_alternative<Running>(stored().state));
    CHECK(served.host.paneCloses.empty());
    // Expected at every boot after an upgrade: logged, but not as a warning.
    CHECK(std::ranges::any_of(served.host.logged, [](auto const& entry) {
        return entry.first == LogLevel::Debug && entry.second == "panes: limit";
    }));
    CHECK(std::ranges::none_of(served.host.logged, [](auto const& entry) {
        return entry.first == LogLevel::Warn && entry.second.starts_with("panes:");
    }));

    // Once the host has reconciled, its Panes hint lists the pane: the run goes on as it was.
    served.host.paneListing = std::vector { PaneInfo { .pane = *run.pane,
                                                       .tag = run.id,
                                                       .status = AgentStatus::Working,
                                                       .message = {},
                                                       .program = ProgramLiveness::Alive } };
    served.driver->update(Hint::Panes);
    CHECK(std::holds_alternative<Running>(stored().state));

    // And a listing without it ends the run, through the removal checks.
    served.host.clock.monoMs += 1'000;
    served.host.paneListing = std::vector<PaneInfo> {};
    served.driver->update(Hint::Panes);
    INFO("state " << stored().state.index());
    CHECK_FALSE(std::holds_alternative<Running>(stored().state));
    CHECK_FALSE(countsAsActive(stored()));
}

TEST_CASE("OpenPane passes nearTab to pane-open", "[guest][panes]")
{
    auto const away = awayCallBefore<OpenPane>();
    auto host = FakeGuestHost {};
    putAway(host, away);
    auto observer = RecordingObserver {};

    [[maybe_unused]] auto const driver =
        Driver::load(host, host.ids, configJson("master"), hostInfo(), observer);

    auto const opens = observer.effectsOf<OpenPane>();
    REQUIRE(opens.size() == 1);
    REQUIRE(host.paneOpenNears.size() == 1);
    CHECK(host.paneOpenNears.front() == opens.front().nearTab);
}

TEST_CASE("a failed store-set is logged, not fatal", "[guest]")
{
    auto const away = awayCallBefore<StoreSet>();
    auto host = FakeGuestHost {};
    putAway(host, away);
    host.storeSetError = HostError::Gone;
    auto observer = RecordingObserver {};

    auto driver = Driver::load(host, host.ids, configJson("master"), hostInfo(), observer);

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
    auto driver = Driver::load(host, host.ids, configJson(), hostInfo(), observer);
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
        auto driver = Driver::load(host, host.ids, configJson(), hostInfo());
        auto* const auth = host.running("gh-auth");
        REQUIRE(auth != nullptr);
        FakeGuestHost::finish(*auth, 0, {});
        driver.update(Hint::Procs);
        REQUIRE(host.running("gh-prs") != nullptr);
    }
    auto* const prs = host.running("gh-prs");
    FakeGuestHost::finish(*prs, 0, host.answers.at("gh-prs").stdoutText);

    auto const driver = Driver::load(host, host.ids, configJson(), hostInfo());

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

// {{{ the served tab

TEST_CASE("open opens one tab and draws it", "[guest]")
{
    auto served = Served {};
    served.load();
    REQUIRE(served.host.writes.empty());

    served.invoke("open", 7);

    CHECK(served.host.tabOpens == std::vector { std::pair { "orchestrator"s, uint32_t { 7 } } });
    CHECK(served.driver->servedTabs() == 1);
    // One frame, in as many pieces as a host call carries: core::tui positions the cursor per cell, so that a
    // full frame at 120x40 is just over 64 KiB.
    REQUIRE(served.host.writesTo("tab-1") == served.host.writes.size());
    auto const frame = served.host.writtenTo("tab-1");
    CHECK(frame.starts_with(SynchronizedOutput));
    // Then bracketed paste and the mouse are asked for and the hidden cursor comes to rest, which draws
    // nothing (@see view::BracketedPasteOn, view::MouseReportingOn, view::RestingCursor).
    CHECK(frame.ends_with(std::format("{}{}{}{}",
                                      SynchronizedOutputEnd,
                                      view::BracketedPasteOn,
                                      view::MouseReportingOn,
                                      view::RestingCursor)));
    CHECK(countOf(frame, SynchronizedOutput) == 1);
}

TEST_CASE("a second open reveals instead of opening", "[guest]")
{
    auto served = Served {};
    served.load();
    served.invoke("open", 7);
    auto const* const first = served.driver->dashboard("tab-1");
    REQUIRE(first != nullptr);
    served.host.writes.clear();

    served.invoke("open", 9);

    CHECK(served.host.tabOpens.size() == 2);
    CHECK(served.driver->servedTabs() == 1);
    CHECK(served.driver->dashboard("tab-1") == first);
    CHECK(served.host.writes.empty());
}

TEST_CASE("actions queued before load are taken by it", "[guest]")
{
    auto served = Served {};
    served.host.queuedActions.push_back(ActionInfo { .id = "open", .origin = 3 });
    served.host.queuedActions.push_back(ActionInfo { .id = "no-such-action", .origin = 3 });

    served.load();

    CHECK(served.host.tabOpens.size() == 1);
    CHECK(served.driver->servedTabs() == 1);
    CHECK(served.host.writtenTo("tab-1").starts_with(SynchronizedOutput));
    CHECK(std::ranges::any_of(served.host.logged, [](auto const& entry) {
        return entry.first == LogLevel::Warn && entry.second.contains("no-such-action");
    }));
}

TEST_CASE("refresh reads the lists again", "[guest]")
{
    auto served = Served {};
    served.load();
    REQUIRE(served.host.startsOf("gh-prs") == 1);

    served.invoke("refresh");
    while (served.host.completeRunning() > 0)
        served.driver->update(Hint::Procs);

    CHECK(served.observer.inputsOf<RefreshNow>().size() == 1);
    CHECK(served.host.startsOf("gh-prs") == 2);
    // No tab is opened for it.
    CHECK(served.host.tabOpens.empty());
}

TEST_CASE("show-blocked opens the tab on the first blocked agent", "[guest]")
{
    constexpr auto RunningId = "pr-2375-20261113-150847"sv;
    constexpr auto BlockedId = "pr-2379-20261113-151500"sv;
    auto served = Served {};
    auto const runs = std::array {
        view::test::storedRun(Running {}, RunningId, ChangeRequestRef { 2375 }),
        view::test::storedRun(Blocked {}, BlockedId, ChangeRequestRef { 2379 }),
    };
    auto listing = std::vector<PaneInfo> {};
    for (auto const& run: runs)
    {
        served.host.store.insert_or_assign(runKey(run.id), toJson(run));
        listing.push_back(PaneInfo {
            .pane = *run.pane,
            .tag = run.id,
            .status = std::holds_alternative<Blocked>(run.state) ? AgentStatus::Asking : AgentStatus::Working,
            .message = {},
            .program = ProgramLiveness::Alive });
    }
    served.host.paneListing = listing;
    served.load();
    served.invoke("open");
    // The blocked run comes first in the Fleet: another is selected, as the user may have left it.
    CHECK(served.driver->dashboard("tab-1")->select(RunningId).empty());
    REQUIRE(served.dashboard().selectedRun() != BlockedId);

    served.invoke("show-blocked");

    CHECK(served.host.tabOpens.size() == 2);
    CHECK(served.driver->servedTabs() == 1);
    CHECK(served.dashboard().selectedRun() == BlockedId);
}

TEST_CASE("show-blocked selects the blocked run and reads its board instead", "[guest][board]")
{
    constexpr auto RunningId = "pr-2375-20261113-150847"sv;
    constexpr auto BlockedId = "pr-2379-20261113-151500"sv;
    auto served = Served {};
    auto listing = std::vector<PaneInfo> {};
    for (auto const& run: { view::test::storedRun(Running {}, RunningId, ChangeRequestRef { 2375 }),
                            view::test::storedRun(Blocked {}, BlockedId, ChangeRequestRef { 2379 }) })
    {
        served.host.store.insert_or_assign(runKey(run.id), toJson(run));
        listing.push_back(PaneInfo {
            .pane = *run.pane,
            .tag = run.id,
            .status = std::holds_alternative<Blocked>(run.state) ? AgentStatus::Asking : AgentStatus::Working,
            .message = {},
            .program = ProgramLiveness::Alive });
    }
    served.host.paneListing = listing;
    served.host.fileContent = FileContent { .bytes = "# hi", .modifiedUnixMs = 1 };
    served.load();
    served.invoke("open");
    CHECK(served.driver->dashboard("tab-1")->select(RunningId).empty());
    served.type("b\t");
    REQUIRE(served.dashboard().interaction().place == view::Place::Focus);
    REQUIRE(served.driver->engine().board(RunningId) != nullptr);

    served.invoke("show-blocked");

    // The Fleet has the keys again, on the blocked run, whose board the Focus reads instead.
    CHECK(served.dashboard().interaction().place == view::Place::Fleet);
    CHECK(served.dashboard().selectedRun() == BlockedId);
    auto const shown = served.observer.inputsOf<ShowBoard>();
    REQUIRE_FALSE(shown.empty());
    CHECK(shown.back().run == BlockedId);
    CHECK(shown.back().tab == "tab-1");
    CHECK(served.driver->engine().board(RunningId) == nullptr);
}

namespace
{

/// Stores one running agent, @p run, for @p served's guest to load, and lists its pane on the host.
/// @return The run stored.
[[nodiscard]] Run storeRunningAgent(Served& served, std::string_view run)
{
    auto stored = view::test::storedRun(Running {}, run, ChangeRequestRef { 2375 });
    REQUIRE(stored.pane.has_value());
    served.host.store.insert_or_assign(runKey(stored.id), toJson(stored));
    served.host.paneListing = std::vector { PaneInfo { .pane = *stored.pane,
                                                       .tag = stored.id,
                                                       .status = AgentStatus::Working,
                                                       .message = {},
                                                       .program = ProgramLiveness::Alive } };
    return stored;
}

} // namespace

TEST_CASE("the driver remembers the origin a dashboard was opened from", "[guest][driver]")
{
    constexpr auto RunId = "pr-2375-20261113-150847"sv;
    auto served = Served {};
    auto const run = storeRunningAgent(served, RunId);
    served.load();
    served.invoke("open", 7);
    auto const stepped = served.observer.inputs.size();

    served.driver->apply("tab-1", view::RevealAgentAction { .run = std::string(RunId) });

    CHECK(served.host.paneReveals == std::vector { std::pair { *run.pane, uint32_t { 7 } } });
    CHECK(served.observer.inputs.size() == stepped); // the host's to carry out, never the engine's

    // The window that revealed the dashboard last is the one its agents go to.
    served.invoke("show-blocked", 9);
    served.driver->apply("tab-1", view::RevealAgentAction { .run = std::string(RunId) });
    CHECK(served.host.paneReveals.back() == std::pair { *run.pane, uint32_t { 9 } });

    // A run with no agent tab asks the host for nothing.
    served.driver->apply("tab-1", view::RevealAgentAction { .run = "no-such-run" });
    CHECK(served.host.paneReveals.size() == 2);
}

TEST_CASE("a reveal without a remembered origin passes 0", "[guest][driver]")
{
    constexpr auto RunId = "pr-2375-20261113-150847"sv;
    auto served = Served {};
    auto const run = storeRunningAgent(served, RunId);
    // A dashboard the guest finds open as it loads: no action of this guest opened it.
    served.host.tabList = { TabInfo { .tab = "tab-1", .scheme = "orchestrator", .cols = 120, .rows = 40 } };
    served.load();
    REQUIRE(served.driver->servedTabs() == 1);

    served.driver->apply("tab-1", view::RevealAgentAction { .run = std::string(RunId) });

    CHECK(served.host.paneReveals == std::vector { std::pair { *run.pane, uint32_t { 0 } } });
}

TEST_CASE("the driver hands the dashboards its uuid source", "[guest][driver]")
{
    auto served = Served {};
    served.host.tabList = { TabInfo { .tab = "tab-1", .scheme = "orchestrator", .cols = 120, .rows = 40 },
                            TabInfo { .tab = "tab-2", .scheme = "orchestrator", .cols = 80, .rows = 24 } };
    served.load();
    REQUIRE(served.driver->servedTabs() == 2);

    for (auto const tab: { "tab-1"sv, "tab-2"sv })
    {
        CAPTURE(tab);
        CHECK(&served.dashboard(tab).ids() == &served.host.ids);
        CHECK(served.dashboard(tab).tab() == tab);
    }
    CHECK(served.host.ids.taken == 0); // taken only as a form opens
}

TEST_CASE("a confirmed form dispatches with a fresh session", "[guest][driver][form]")
{
    auto served = Served {};
    served.load();
    served.invoke("open");
    auto* const dashboard = served.driver->dashboard("tab-1");
    REQUIRE(dashboard != nullptr);
    dashboard->openLauncherOn("contour", ChangeRequestRef { 2375 }, "fix_ci");
    REQUIRE(dashboard->launcher().has_value());
    auto const session = dashboard->launcher()->form->session;
    CHECK(session == "00000001-0000-4000-8000-000000000000");
    served.host.clock.wallMs += 5'000;

    served.type("\r");

    auto const dispatched = served.observer.inputsOf<Dispatch>();
    REQUIRE(dispatched.size() == 1);
    CHECK(dispatched.front().session == session);
    CHECK(dispatched.front().nearTab == std::optional<std::string> { "tab-1" });
    CHECK(dispatched.front().wallMs == served.host.clock.wallMs);
    CHECK(dispatched.front().task == "fix_ci");
    CHECK_FALSE(served.dashboard().launcher().has_value());
    // The run starts: its fetch is the first command it asks for.
    CHECK(served.host.startsOf("git-fetch") == 1);
}

TEST_CASE("a refused dispatch is drawn", "[guest][driver][form]")
{
    auto served = Served {};
    served.load();
    served.invoke("open");
    served.host.writes.clear();

    // A repository the configuration does not name: the engine refuses what the form could not see.
    served.driver->apply(
        "tab-1",
        view::DispatchAction { .repo = "elsewhere",
                               .item = ChangeRequestRef { 2375 },
                               .url = "https://github.com/contour-terminal/contour/pull/2375",
                               .task = "fix_ci",
                               .model = "sonnet",
                               .note = std::nullopt,
                               .session = "00000001-0000-4000-8000-000000000000",
                               .nearTab = "tab-1" });
    served.driver->update(Hint::Tabs);

    CHECK(served.host.writesTo("tab-1") > 0); // drawn, not only remembered
    CHECK(
        core::tui::test::canvasToString(served.dashboard().rendered())
            .contains(std::format("dispatch refused: {}", view::refusalText(DispatchRefusal::UnknownRepo))));
}

TEST_CASE("every declared action is carried out", "[guest]")
{
    auto served = Served {};
    served.load();

    for (auto const& action: actionDecls())
        served.invoke(action.id);

    CHECK(served.host.tabOpens.size() == 2); // open and show-blocked
    CHECK(served.observer.inputsOf<RefreshNow>().size() == 1);
    CHECK(std::ranges::none_of(served.host.logged,
                               [](auto const& entry) { return entry.second.starts_with("unknown action"); }));
}

TEST_CASE("an agent's age is redrawn by the wake it asks for", "[guest]")
{
    auto served = Served {};
    auto const run = view::test::storedRun(Running {}, "pr-2375-20261113-150847", ChangeRequestRef { 2375 });
    served.host.store.insert_or_assign(runKey(run.id), toJson(run));
    served.host.paneListing = std::vector { PaneInfo { .pane = *run.pane,
                                                       .tag = run.id,
                                                       .status = AgentStatus::Working,
                                                       .message = {},
                                                       .program = ProgramLiveness::Alive } };
    served.load();
    served.invoke("open");
    served.host.writes.clear();
    // The wakes it asks for come within a second; the selected agent's board, which its details show, may ask
    // for one before its age changes. Following them redraws the age.
    for ([[maybe_unused]] auto const wake: std::views::iota(0, 3))
    {
        REQUIRE_FALSE(served.host.wakes.empty());
        auto const asked = served.host.wakes.back();
        CHECK(asked <= 1'000);
        served.host.clock.monoMs += asked;
        served.driver->update(Hint::Wake);
        if (!served.host.writtenTo("tab-1").empty())
            break;
    }

    CHECK(served.host.writtenTo("tab-1").starts_with(SynchronizedOutput));
}

TEST_CASE("input reaches the engine", "[guest]")
{
    auto served = Served {};
    served.load();
    served.invoke("open");
    REQUIRE(served.host.startsOf("gh-prs") == 1);

    // Refresh is the palette's: ^K, its name, Enter.
    served.type("\x0brefresh\r");
    while (served.host.completeRunning() > 0)
        served.driver->update(Hint::Procs);

    CHECK(served.observer.inputsOf<RefreshNow>().size() == 1);
    CHECK(served.host.startsOf("gh-prs") == 2);
}

TEST_CASE("a resize redraws at the new band", "[guest]")
{
    auto served = Served {};
    served.host.openSize = { 80, 24 };
    served.load();
    served.invoke("open");
    CHECK(served.dashboard().breakpoint() == shownBreakpointOf(view::Band::Medium));
    served.host.writes.clear();

    served.host.tabList.front().cols = 120;
    served.host.tabList.front().rows = 40;
    served.driver->update(Hint::Tabs);

    CHECK(served.dashboard().breakpoint() == shownBreakpointOf(view::Band::Wide));
    CHECK(served.host.writtenTo("tab-1").starts_with(SynchronizedOutput));
}

TEST_CASE("a closed tab drops its dashboard", "[guest]")
{
    auto served = Served {};
    served.load();
    served.invoke("open");
    served.host.writes.clear();

    served.host.tabList.clear();
    served.driver->update(Hint::Tabs);

    CHECK(served.driver->servedTabs() == 0);
    CHECK(served.driver->dashboard("tab-1") == nullptr);
    CHECK(served.host.writes.empty());
}

namespace
{

/// Gives @p served a running agent, its tab open: a board can be shown for it.
void addAgent(Served& served)
{
    auto const run = view::test::storedRun(Running {}, "pr-2375-20261113-150847", ChangeRequestRef { 2375 });
    served.host.store.insert_or_assign(runKey(run.id), toJson(run));
    served.host.paneListing = std::vector { PaneInfo { .pane = *run.pane,
                                                       .tag = run.id,
                                                       .status = AgentStatus::Working,
                                                       .message = {},
                                                       .program = ProgramLiveness::Alive } };
}

/// @return How many ReadFile effects the engine asked for a shown board.
[[nodiscard]] std::size_t boardViewReads(RecordingObserver const& observer)
{
    return static_cast<std::size_t>(std::ranges::count_if(observer.effects, [](Effect const& effect) {
        auto const* const read = std::get_if<ReadFile>(&effect);
        return read != nullptr && read->tag.ends_with("#board-view");
    }));
}

} // namespace

TEST_CASE("a Windows host's file-read path is /-separated", "[guest][board]")
{
    constexpr auto Root = R"(C:\wt)"sv;
    constexpr auto Main = R"(C:\src\contour)"sv;
    constexpr auto Id = "pr-2375-20261113-150847"sv;
    auto served = Served {};
    addAgent(served);
    auto run = view::test::storedRun(Running {}, Id, ChangeRequestRef { 2375 });
    run.repoPath = std::string(Main);
    run.worktree = std::format(R"({}\contour\{})", Root, Id);
    served.host.store.insert_or_assign(runKey(run.id), toJson(run));
    served.info = HostInfo { .home = R"(C:\Users\u)",
                             .worktreeRoot = std::string(Root),
                             .repoPaths = { std::string(Main) },
                             .utcOffsetMinutes = 0 };
    served.load();
    served.invoke("open");

    served.driver->apply("tab-1", view::ShowBoardAction { .run = std::string(Id) });

    REQUIRE(served.host.fileReads.size() == 1);
    CHECK(served.host.fileReads.front() == std::format("contour/{}/STATUS.md", Id));
}

TEST_CASE("a file read carries its modification time", "[guest][board]")
{
    constexpr auto Modified = uint64_t { 1'700'000'000'000 };
    auto served = Served {};
    addAgent(served);
    served.host.fileContent = FileContent { .bytes = "# hi", .modifiedUnixMs = Modified };
    served.load();
    served.invoke("open");

    served.driver->apply("tab-1", view::ShowBoardAction { .run = "pr-2375-20261113-150847" });

    REQUIRE(served.host.fileReads.size() == 1);
    CHECK(served.host.fileReads.front().ends_with("pr-2375-20261113-150847/STATUS.md"));
    auto const reads = served.observer.inputsOf<FileRead>();
    REQUIRE(reads.size() == 1);
    CHECK(reads.front().modifiedWallMs == Modified);
    CHECK(reads.front().wallMs == served.host.clock.wallMs);
    CHECK(reads.front().content == "# hi");
    auto const* const board = served.driver->engine().board("pr-2375-20261113-150847");
    REQUIRE(board != nullptr);
    CHECK(board->state == BoardState::Read);
    CHECK(board->modifiedWallMs == Modified);
}

TEST_CASE("a served tab that closes stops showing its board and its transcript", "[guest][board]")
{
    auto served = Served {};
    addAgent(served);
    served.host.fileContent = FileContent { .bytes = "# hi", .modifiedUnixMs = 1 };
    served.load();
    served.invoke("open");
    served.driver->apply("tab-1", view::ShowBoardAction { .run = "pr-2375-20261113-150847" });
    served.driver->apply("tab-1", view::ReadTranscriptAction { .run = "pr-2375-20261113-150847", .page = 1 });
    REQUIRE(boardViewReads(served.observer) == 1);
    REQUIRE(served.driver->engine().transcript("tab-1", "pr-2375-20261113-150847") != nullptr);

    served.host.tabList.clear();
    served.driver->update(Hint::Tabs);

    auto const closed = served.observer.inputsOf<TabClosed>();
    REQUIRE(closed.size() == 1);
    CHECK(closed.front().tab == "tab-1");
    CHECK(served.driver->engine().board("pr-2375-20261113-150847") == nullptr);
    CHECK(served.driver->engine().transcript("tab-1", "pr-2375-20261113-150847") == nullptr);
    served.host.clock.monoMs += 10'000;
    served.driver->update(Hint::Wake);
    CHECK(boardViewReads(served.observer) == 1);
}

TEST_CASE("the selected run's board is read through the engine, and a held ESC leaves the Focus",
          "[guest][board]")
{
    constexpr auto Run = "pr-2375-20261113-150847"sv;
    auto served = Served {};
    addAgent(served);
    served.host.fileContent = FileContent { .bytes = "# hi", .modifiedUnixMs = 1 };
    served.load();
    served.invoke("open");
    CHECK(served.driver->dashboard("tab-1")->select(Run).empty());

    served.type("b\t");
    auto const shown = served.observer.inputsOf<ShowBoard>();
    REQUIRE(shown.size() == 1);
    CHECK(shown.front().run == Run);
    CHECK(shown.front().tab == "tab-1");
    CHECK(served.dashboard().interaction().place == view::Place::Focus);
    CHECK(core::tui::test::canvasToString(served.dashboard().rendered()).contains("updated"));

    served.type("\x1b");
    REQUIRE_FALSE(served.host.wakes.empty());
    served.host.clock.monoMs += served.host.wakes.back();
    served.driver->update(Hint::Wake);

    // Back in the Fleet the agent is still selected, and the Focus reads its board on.
    CHECK(served.observer.inputsOf<HideBoard>().empty());
    CHECK(served.driver->engine().board(Run) != nullptr);
    CHECK(served.dashboard().interaction().place == view::Place::Fleet);
}

TEST_CASE("ReadTranscriptAction reaches the engine", "[guest][transcript]")
{
    constexpr auto Run = "pr-2375-20261113-150847"sv;
    auto served = Served {};
    addAgent(served);
    served.load();
    served.invoke("open");

    served.driver->apply("tab-1", view::ReadTranscriptAction { .run = std::string { Run }, .page = 2 });

    auto const reads = served.observer.inputsOf<ReadTranscript>();
    REQUIRE(reads.size() == 1);
    CHECK(reads.front().run == Run);
    CHECK(reads.front().page == 2);
    CHECK(reads.front().tab == "tab-1");
    CHECK(reads.front().monoMs == served.host.clock.monoMs);
    CHECK(served.host.startsOf("agent-transcript") == 1);
    auto const* const view = served.driver->engine().transcript("tab-1", Run);
    REQUIRE(view != nullptr);
    CHECK(view->page == 2);
}

TEST_CASE("a message is typed into its run's agent tab, and a refusal is said", "[guest][message]")
{
    constexpr auto Run = "pr-2375-20261113-150847"sv;
    auto served = Served {};
    addAgent(served);
    served.load();
    served.invoke("open");

    served.driver->apply("tab-1",
                         view::SendMessageAction { .run = std::string { Run }, .text = "rebase first" });
    REQUIRE(served.host.paneSends.size() == 1);
    CHECK(served.host.paneSends.front().first == std::format("p-{}", Run));
    CHECK(served.host.paneSends.front().second == "rebase first");

    served.host.paneSendAnswer = std::unexpected(HostError::BadArgument);
    served.driver->apply("tab-1", view::SendMessageAction { .run = std::string { Run }, .text = "x" });
    CHECK(served.dashboard().interaction().notice == "message not sent: the host will not type this text");

    served.driver->apply("tab-1", view::SendMessageAction { .run = "no-such-run", .text = "x" });
    CHECK(served.host.paneSends.size() == 2);
    CHECK(served.dashboard().interaction().notice == "message not sent: its agent has no tab any more");
}

TEST_CASE("a quick reply presses its key in the asking agent's tab, and a refusal is said", "[guest][reply]")
{
    constexpr auto Run = "pr-2375-20261113-150847"sv;
    auto served = Served {};
    addAgent(served);
    served.load();
    served.invoke("open");
    auto const reply = view::QuickReplyAction { .run = std::string { Run },
                                                .key = '1',
                                                .label = "yes",
                                                .press = PaneKey::Digit1,
                                                .then = AfterReply::Wait };

    // Not asking: nothing is pressed, as a key where no menu is up would be typed into the agent's prompt.
    served.driver->apply("tab-1", reply);
    CHECK(served.host.paneKeys.empty());
    CHECK(served.dashboard().interaction().notice == "pr-2375 is no longer asking");

    served.host.paneListing->front().status = AgentStatus::Asking;
    served.driver->update(Hint::Panes);
    REQUIRE(served.driver->engine().runs().front().lastStatus == AgentStatus::Asking);
    served.driver->apply("tab-1", reply);
    CHECK(served.host.paneKeys == std::vector { std::pair { std::format("p-{}", Run), PaneKey::Digit1 } });
    CHECK(served.driver->engine().runs().front().lastStatus == AgentStatus::Asking);

    // A manifest without pane-input: the host refuses, in its own words.
    served.host.paneSendAnswer = std::unexpected(HostError::BadArgument);
    served.driver->apply("tab-1", reply);
    CHECK(served.dashboard().interaction().notice
          == "reply not sent: the extension may not press keys in its tabs");
    served.host.paneSendAnswer = {};

    // A reply that awaits a reason turns the run Waiting.
    served.driver->apply("tab-1",
                         view::QuickReplyAction { .run = std::string { Run },
                                                  .key = '3',
                                                  .label = "no, and say why",
                                                  .press = PaneKey::Escape,
                                                  .then = AfterReply::Message });
    CHECK(served.host.paneKeys.back().second == PaneKey::Escape);
    CHECK(served.observer.inputsOf<AwaitMessage>().size() == 1);
    CHECK(served.driver->engine().runs().front().lastStatus == AgentStatus::Waiting);
}

TEST_CASE("o opens the link through url-open, and a refusal is said", "[guest][url-open]")
{
    auto served = Served {};
    served.load();
    served.invoke("open");
    auto const open = view::OpenUrlAction { .url = "https://github.com/o/r/pull/7", .label = "#7" };
    served.driver->apply("tab-1", open);
    REQUIRE(served.host.urlOpens.size() == 1);
    CHECK(served.host.urlOpens.front().first == "https://github.com/o/r/pull/7");
    CHECK(served.dashboard().interaction().notice == "opening #7 on your machine");

    served.host.urlOpenAnswer = std::unexpected(HostError::NotSupportedYet);
    served.driver->apply("tab-1", open);
    CHECK(served.dashboard().interaction().notice == "not opened: this terminal cannot open links");
}

TEST_CASE("a key split across two tab reads moves once", "[guest]")
{
    auto served = Served {};
    addAgent(served);
    served.load();
    served.invoke("open");
    REQUIRE(served.dashboard().interaction().selected == 0);

    served.type("\x1b[");
    CHECK(served.dashboard().interaction().selected == 0);
    served.type("B");

    CHECK(served.dashboard().interaction().selected == 1);
    CHECK(served.dashboard().interaction().filter.empty());
    CHECK(served.dashboard().interaction().filtering == view::Filtering::Off);
}

TEST_CASE("a held ESC is the Escape key once the wake it asked for comes", "[guest]")
{
    auto served = Served {};
    served.load();
    served.invoke("open");
    served.type("/x\x1b");
    REQUIRE(served.dashboard().interaction().filtering == view::Filtering::Typing);
    REQUIRE_FALSE(served.host.wakes.empty());
    auto const asked = served.host.wakes.back();
    CHECK(asked <= 50);

    served.host.clock.monoMs += asked;
    served.driver->update(Hint::Wake);

    CHECK(served.dashboard().interaction().filtering == view::Filtering::Off);
    CHECK(served.dashboard().interaction().filter.empty());
}

TEST_CASE("a restarted guest redraws every open tab", "[guest]")
{
    auto served = Served {};
    served.host.tabList = {
        TabInfo { .tab = "tab-1", .scheme = "orchestrator", .cols = 120, .rows = 40 },
        TabInfo { .tab = "tab-2", .scheme = "orchestrator", .cols = 50, .rows = 20 },
    };
    served.load();
    REQUIRE(served.driver->servedTabs() == 2);
    served.host.writes.clear();

    served.driver.reset();
    served.driver.emplace(Driver::load(served.host, served.host.ids, configJson(), hostInfo()));

    for (auto const tab: { "tab-1"sv, "tab-2"sv })
    {
        CAPTURE(tab);
        CHECK(served.host.writtenTo(tab).starts_with(SynchronizedOutput));
    }
}

TEST_CASE("nothing changed writes nothing", "[guest]")
{
    auto served = Served {};
    served.load();
    served.invoke("open");
    served.host.writes.clear();

    served.driver->update(Hint::Tabs);
    served.driver->update(Hint::Procs);
    served.driver->update(Hint::Panes);
    served.type("k");

    CHECK(served.host.writes.empty());
}

TEST_CASE("a frame is written in pieces a host call carries", "[guest]")
{
    auto served = Served {};
    served.host.openSize = { 400, 200 };
    served.load();
    served.invoke("open");

    REQUIRE(served.host.writesTo("tab-1") > 1);
    for (auto const& bytes: served.host.writes | std::views::values)
        CHECK(bytes.size() <= MaxTabWriteBytes);
    CHECK(served.host.writtenTo("tab-1").starts_with(SynchronizedOutput));
}

TEST_CASE("a failed write drops nothing else", "[guest]")
{
    auto served = Served {};
    served.host.tabList = {
        TabInfo { .tab = "tab-1", .scheme = "orchestrator", .cols = 120, .rows = 40 },
        TabInfo { .tab = "tab-2", .scheme = "orchestrator", .cols = 120, .rows = 40 },
    };
    served.host.failingWrites.insert("tab-1");

    served.load();

    CHECK(served.host.writesTo("tab-1") == 0);
    CHECK(served.host.writtenTo("tab-2").starts_with(SynchronizedOutput));
    CHECK(served.driver->servedTabs() == 2);
}

// }}}
