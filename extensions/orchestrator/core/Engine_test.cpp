// SPDX-License-Identifier: Apache-2.0
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <format>
#include <map>
#include <optional>
#include <ranges>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

#include <orchestrator/core/Commands.hpp>
#include <orchestrator/core/Engine.hpp>
#include <orchestrator/core/EngineDetail.hpp>
#include <orchestrator/core/Fixtures.hpp>
#include <orchestrator/core/Hooks.hpp>
#include <orchestrator/core/Overloaded.hpp>
#include <orchestrator/core/Run.hpp>

using namespace std::string_literals;
using namespace std::string_view_literals;

namespace
{

using namespace orchestrator;

// {{{ the world the tests run in

constexpr auto WorktreeRoot = "/home/u/worktrees"sv;
constexpr auto ContourPath = "/home/u/projects/contour"sv;
constexpr auto OtherPath = "/home/u/projects/other"sv;
constexpr auto ThirdPath = "/home/u/projects/third"sv;
constexpr auto Session = "123e4567-e89b-12d3-a456-426614174000"sv;
constexpr auto Sha = "e6e8346832d4ba7a25fbaa38301c781b5cbb5ebe"sv;
/// 2026-11-13 15:08:47 UTC.
constexpr auto DispatchWallMs = uint64_t { 1'794'582'527'000 };
constexpr auto StartMonoMs = uint64_t { 1'000 };
constexpr auto Minute = uint64_t { 60'000 };
constexpr auto PrId = "pr-2375-20261113-150847"sv;
constexpr auto CleanLongStatus = "On branch agent/x\nnothing to commit, working tree clean\n"sv;

/// What a test changes about the configuration.
struct Settings
{
    std::string_view limits {}; ///< Extra top-level keys, each followed by a comma.
    std::string_view repos = R"([ { "name": "contour", "path": "~/projects/contour", "forge": "github",
                                     "issues": [ { "source": "forge", "query": "assignee:@me" } ] } ])";
    std::vector<std::string_view> repoPaths = { ContourPath };
};

[[nodiscard]] std::string configJson(Settings const& settings)
{
    return std::format(R"({{ "worktree_root": "~/worktrees", "models": ["opus", "sonnet"], {}
  "repos": {},
  "tasks": [
    {{ "id": "fix_ci", "key": "f", "label": "fix CI", "applies_to": "pr", "model": "sonnet",
       "prompt": "/contour-workflows:fix-ci {{url}}" }},
    {{ "id": "issue", "key": "w", "label": "work on issue", "applies_to": "issue", "model": "opus",
       "prompt": "/contour-workflows:work-issue {{url}}" }} ] }})",
                       settings.limits,
                       settings.repos);
}

[[nodiscard]] HostInfo hostFor(Settings const& settings, std::string_view root = WorktreeRoot)
{
    auto host = HostInfo {
        .home = "/home/u", .worktreeRoot = std::string(root), .repoPaths = {}, .utcOffsetMinutes = 0
    };
    for (auto const path: settings.repoPaths)
        host.repoPaths.emplace_back(path);
    return host;
}

[[nodiscard]] StartContext contextFor(Settings const& settings)
{
    return StartContext { .monoMs = StartMonoMs,
                          .wallMs = DispatchWallMs,
                          .host = hostFor(settings),
                          .configJson = configJson(settings),
                          .stored = {},
                          .inFlight = {},
                          .finished = {},
                          .panes = {},
                          .paneListing = PaneListing::Tagged };
}

[[nodiscard]] ProcessOutcome ok(std::string_view stdoutText = {})
{
    return ProcessOutcome { .start = StartResult::Started,
                            .completion = Completion::Exited,
                            .exitCode = 0,
                            .extent = OutputExtent::Whole,
                            .stdoutText = std::string(stdoutText),
                            .stderrText = {} };
}

[[nodiscard]] ProcessOutcome failed(int32_t exitCode, std::string_view stderrText = {})
{
    auto outcome = ok();
    outcome.exitCode = exitCode;
    outcome.stderrText = std::string(stderrText);
    return outcome;
}

[[nodiscard]] ProcessOutcome cwdMissing()
{
    auto outcome = ok();
    outcome.start = StartResult::CwdMissing;
    outcome.exitCode = std::nullopt;
    return outcome;
}

[[nodiscard]] ProcessOutcome truncated(std::string_view stdoutText)
{
    auto outcome = ok(stdoutText);
    outcome.extent = OutputExtent::Truncated;
    return outcome;
}

/// `git worktree list --porcelain -z` of a repository with its main checkout and @p extra entries.
[[nodiscard]] std::string worktreeList(std::string_view main, std::string_view extra = {})
{
    return std::format("worktree {}\0HEAD {}\0branch refs/heads/master\0\0{}"sv, main, Sha, extra);
}

[[nodiscard]] std::string agentWorktree(std::string_view path, std::string_view branch)
{
    return std::format("worktree {}\0HEAD {}\0branch refs/heads/{}\0\0"sv, path, Sha, branch);
}

[[nodiscard]] std::string worktreeOf(std::string_view repo, std::string_view id)
{
    return std::format("{}/{}/{}", WorktreeRoot, repo, id);
}

[[nodiscard]] std::string intactGitFile(std::string_view main, std::string_view id)
{
    return std::format("gitdir: {}/.git/worktrees/{}\n", main, id);
}

/// What `git ls-files -v --stage` prints for an index with one ordinary file.
constexpr auto CleanIndex = "H 100644 78981922613b2afb6025042ff6bd878ac1994e85 0\tf\n"sv;

/// What `git rev-parse --path-format=absolute --git-common-dir` prints in a worktree of @p main.
[[nodiscard]] std::string commonDirOf(std::string_view main)
{
    return std::format("{}/.git\n", main);
}

[[nodiscard]] Dispatch prDispatch(uint64_t number = 2375, std::string_view repo = "contour")
{
    return Dispatch { .repo = std::string(repo),
                      .item = ChangeRequestRef { number },
                      .url = std::format("https://github.com/contour-terminal/contour/pull/{}", number),
                      .task = "fix_ci",
                      .model = "",
                      .note = std::nullopt,
                      .session = std::string(Session),
                      .wallMs = DispatchWallMs };
}

[[nodiscard]] Dispatch issueDispatch(uint64_t number = 2356, std::string_view repo = "contour")
{
    return Dispatch { .repo = std::string(repo),
                      .item = IssueRef { number },
                      .url = std::format("https://github.com/contour-terminal/contour/issues/{}", number),
                      .task = "issue",
                      .model = "",
                      .note = std::nullopt,
                      .session = std::string(Session),
                      .wallMs = DispatchWallMs };
}

// }}}
// {{{ the harness

template <typename T>
[[nodiscard]] std::vector<T> all(std::span<Effect const> effects)
{
    auto found = std::vector<T> {};
    for (auto const& effect: effects)
        if (auto const* const typed = std::get_if<T>(&effect))
            found.push_back(*typed);
    return found;
}

template <typename T>
[[nodiscard]] std::size_t countOf(std::span<Effect const> effects)
{
    return static_cast<std::size_t>(std::ranges::count_if(
        effects, [](Effect const& effect) { return std::holds_alternative<T>(effect); }));
}

[[nodiscard]] std::vector<std::string_view> commandsIn(std::span<Effect const> effects)
{
    auto names = std::vector<std::string_view> {};
    for (auto const& effect: effects)
        if (auto const* const command = std::get_if<RunCommand>(&effect))
            names.push_back(command->command);
    return names;
}

[[nodiscard]] bool isUnder(std::string_view path, std::string_view root)
{
    return path.size() > root.size() + 1 && path.starts_with(root) && path[root.size()] == '/';
}

[[nodiscard]] std::string_view runOfTag(std::string_view tag)
{
    return tag.substr(0, tag.find('/'));
}

/// Drives an engine, checks every effect list it returns against the rules all scenarios share, and
/// answers the calls it makes.
class Harness
{
  public:
    explicit Harness(StartContext context):
        _root { context.host.worktreeRoot },
        _repoPaths { context.host.repoPaths },
        _panes { context.panes },
        _now { context.monoMs }
    {
        for (auto const& entry: context.stored)
            _stored[entry.key.substr(4)] = entry.value;
        _pending.reserve(16);
        for (auto const& tag: context.inFlight)
        {
            _foreignInFlight.push_back(tag);
            _deadlines[tag] = _now + static_cast<uint64_t>((LongTimeout + CallDeadlineMargin).count());
        }
        auto started = Engine::start(std::move(context));
        _engine.emplace(std::move(started.engine));
        accept(started.effects);
        _last = std::move(started.effects);
    }

    [[nodiscard]] Engine const& engine() const { return *_engine; }
    [[nodiscard]] std::span<Effect const> last() const { return _last; }
    [[nodiscard]] uint64_t now() const { return _now; }
    /// @return The last wake asked for.
    [[nodiscard]] uint64_t wakeAt() const { return _wakeAt; }

    std::vector<Effect> feed(Input const& input)
    {
        std::visit(
            [&](auto const& arrived) {
                if constexpr (requires { arrived.monoMs; })
                    _now = std::max(_now, arrived.monoMs);
            },
            input);
        // The engine abandons a call whose reply is overdue, before it looks at the input.
        auto const* const reply = std::get_if<ProcessFinished>(&input);
        auto const overdue = [&](std::string const& tag) {
            if (reply != nullptr && reply->tag == tag)
                return false;
            auto const deadline = _deadlines.find(tag);
            return deadline != _deadlines.end() && deadline->second <= _now;
        };
        std::erase_if(_pending, [&](RunCommand const& command) { return overdue(command.tag); });
        std::erase_if(_foreignInFlight, overdue);
        if (auto const* const listed = std::get_if<PanesListed>(&input))
            _panes = listed->panes;
        if (auto const* const finished = std::get_if<ProcessFinished>(&input))
        {
            std::erase_if(_pending, [&](RunCommand const& command) { return command.tag == finished->tag; });
            std::erase(_foreignInFlight, finished->tag);
        }
        _last = _engine->step(input);
        accept(_last);
        return _last;
    }

    /// Wakes the engine @p ms after the last wake.
    std::vector<Effect> wake(uint64_t ms)
    {
        return feed(Wake { .monoMs = _now + ms, .wallMs = DispatchWallMs + (_now + ms - StartMonoMs) });
    }

    /// Lists the panes, asked for now.
    std::vector<Effect> list(std::vector<PaneFact> panes, PaneListing listing = PaneListing::Tagged)
    {
        _now += 1;
        return feed(PanesListed { .panes = std::move(panes), .requestedMonoMs = _now, .listing = listing });
    }

    [[nodiscard]] std::optional<RunCommand> pending(std::string_view command) const
    {
        auto const found = std::ranges::find(_pending, command, &RunCommand::command);
        if (found == _pending.end())
            return std::nullopt;
        return *found;
    }

    [[nodiscard]] std::size_t pendingCount() const { return _pending.size(); }

    /// @return The store as the effects so far left it.
    [[nodiscard]] std::vector<StoredEntry> stored() const
    {
        auto entries = std::vector<StoredEntry> {};
        for (auto const& [id, value]: _stored)
            entries.push_back(StoredEntry { .key = runKey(id), .value = value });
        return entries;
    }

    /// Answers the oldest unanswered @p command.
    std::vector<Effect> finish(std::string_view command, ProcessOutcome outcome)
    {
        auto const found = pending(command);
        CAPTURE(command);
        REQUIRE(found);
        // An answer other than the main checkout's .git keeps the worktree: nothing more may run there until
        // its .git file is read intact again.
        if (command == "git-common-dir"
            && (outcome.exitCode != 0 || outcome.extent != OutputExtent::Whole
                || !commonDirIsMain(outcome.stdoutText, mainOf(found->cwd))))
            _tainted.insert(found->cwd);
        // A submodule in the index keeps it too: git status would enter the submodule's own repository.
        if (command == "git-index-flags" && outcome.stdoutText.contains(" 160000 "))
            _tainted.insert(found->cwd);
        return feed(ProcessFinished { .tag = found->tag, .outcome = std::move(outcome) });
    }

    /// Answers the oldest unanswered read of a path ending in @p suffix.
    std::vector<Effect> read(std::string_view suffix, std::expected<std::string, FileReadError> content)
    {
        auto const found =
            std::ranges::find_if(_reads, [&](ReadFile const& file) { return file.path.ends_with(suffix); });
        CAPTURE(suffix);
        REQUIRE(found != _reads.end());
        auto file = std::move(*found);
        _reads.erase(found);
        if (file.path.ends_with("/.git"))
        {
            auto const worktree = file.path.substr(0, file.path.size() - 5);
            auto const id = worktree.substr(worktree.rfind('/') + 1);
            auto const intact = content && gitFileIsIntact(*content, mainOf(file.path), worktree);
            if (intact)
                _tainted.erase(worktree);
            else if (content)
                _tainted.insert(worktree);
            static_cast<void>(id);
        }
        return feed(FileRead { .tag = file.tag, .content = std::move(content) });
    }

    [[nodiscard]] bool hasRead(std::string_view suffix) const
    {
        return std::ranges::any_of(_reads, [&](ReadFile const& file) { return file.path.ends_with(suffix); });
    }

    /// Answers every pending poll: logged in, hooks present, every repository's lists.
    void boot()
    {
        while (true)
        {
            auto const next = std::ranges::find_if(
                _pending, [](RunCommand const& command) { return command.tag.starts_with("poll/"); });
            if (next == _pending.end())
                return;
            auto tag = next->tag;
            auto outcome = answerFor(*next);
            feed(ProcessFinished { .tag = std::move(tag), .outcome = std::move(outcome) });
        }
    }

    /// Dispatches @p request and prepares it until its tab opens.
    void runToRunning(Dispatch const& request, std::string_view pane = "p1")
    {
        feed(request);
        finish("git-fetch", ok());
        if (pending("git-worktree-add"))
            finish("git-worktree-add", ok());
        else
            finish("git-worktree-add-branch", ok());
        if (pending("gh-pr-checkout"))
            finish("gh-pr-checkout", ok());
        auto const opened = all<OpenPane>(_last);
        REQUIRE(opened.size() == 1);
        feed(PaneOpenedFor { .run = opened.front().run, .pane = std::string(pane) });
    }

    /// The pane is gone; the run goes through the removal checks and is removed.
    void endCleanly(std::string_view id, std::string_view main = ContourPath)
    {
        list({});
        read("/.git", intactGitFile(main, id));
        finish("git-common-dir", ok(commonDirOf(main)));
        finish("git-index-flags", ok(CleanIndex));
        finish("git-head-reachable", ok("refs/heads/agent/x\n"));
        finish("git-operation-state", ok(CleanLongStatus));
        finish("git-status", ok());
        finish("git-worktree-remove", ok());
    }

    [[nodiscard]] Run const* run(std::string_view id) const
    {
        auto const runs = _engine->runs();
        auto const found = std::ranges::find(runs, id, &Run::id);
        return found != runs.end() ? &*found : nullptr;
    }

    [[nodiscard]] std::size_t commandsIssued(std::string_view command) const
    {
        return static_cast<std::size_t>(std::ranges::count(_issued, command));
    }

  private:
    [[nodiscard]] static ProcessOutcome answerFor(RunCommand const& command)
    {
        if (command.command == "agent-setup-check")
            return ok(readFixture(FixtureRoot::Testdata, "contour/setup-hooks-present.json"));
        if (command.command == "git-worktree-list")
            return ok(worktreeList(command.cwd));
        if (command.command == "gh-default-branch")
            return ok(readFixture(FixtureRoot::Testdata, "github/default-branch.json"));
        if (command.command == "gh-prs")
            return ok(readFixture(FixtureRoot::Testdata, "github/prs-open.json"));
        if (command.command == "gh-issues")
            return ok(readFixture(FixtureRoot::Testdata, "github/issues-open.json"));
        return ok();
    }

    [[nodiscard]] std::string mainOf(std::string_view path) const
    {
        // Every test repository's worktrees lie under `<root>/<repo>/`.
        auto const rest = path.substr(WorktreeRoot.size() + 1);
        auto const repo = rest.substr(0, rest.find('/'));
        for (auto const& candidate: _repoPaths)
            if (candidate.ends_with(std::format("/{}", repo)))
                return candidate;
        return std::string(ContourPath);
    }

    /// The rules every effect list keeps, whatever the scenario.
    void accept(std::span<Effect const> effects)
    {
        for (auto const& effect: effects)
            std::visit(
                Overloaded {
                    [&](RunCommand const& command) {
                        checkCommand(command);
                        auto const timeout = findCommand(command.command)->timeout == Timeout::Short
                                                 ? ShortTimeout
                                                 : LongTimeout;
                        _deadlines[command.tag] =
                            _now + static_cast<uint64_t>((timeout + CallDeadlineMargin).count());
                        _pending.push_back(command);
                        _issued.push_back(command.command);
                    },
                    [&](ReadFile const& file) {
                        CAPTURE(file.path);
                        CHECK(isUnder(file.path, _root));
                        if (file.path.ends_with("/.git"))
                            checkWriteAhead(file.tag);
                        _reads.push_back(file);
                    },
                    [&](OpenPane const& pane) { checkPane(pane); },
                    [&](StoreSet const& set) {
                        CAPTURE(set.key);
                        REQUIRE(set.key.starts_with("run/"));
                        CHECK(set.value.size() <= MaxStoredRunBytes);
                        CHECK(runFromJson(set.value));
                        _stored[set.key.substr(4)] = set.value;
                    },
                    [&](StoreRemove const& remove) { _stored.erase(remove.key.substr(4)); },
                    [&](WakeAt const& wake) {
                        _wakeAt = wake.monoMs;
                        // A wake at or before now would come back at once, and again.
                        CHECK(wake.monoMs > _now);
                        // The runs this configuration covers: one outside it is left alone.
                        auto const tracked = [&](Run const& run) {
                            return isUnfinished(run) && std::ranges::contains(_repoPaths, run.repoPath)
                                   && isUnder(run.worktree, std::format("{}/{}", _root, run.repo));
                        };
                        if (std::ranges::any_of(_engine ? _engine->runs() : std::span<Run const> {}, tracked))
                            CHECK(wake.monoMs <= _now + static_cast<uint64_t>(ReconcileEvery.count()));
                    },
                    [](auto const&) {},
                },
                effect);
        CHECK(_pending.size() + _foreignInFlight.size() <= MaxInFlight);
    }

    void checkArguments(std::span<std::string_view const> argv, std::span<Argument const> arguments)
    {
        auto placeholders = std::set<std::string_view> {};
        for (auto const word: argv)
            if (auto const name = placeholderName(word))
                placeholders.insert(*name);
        auto names = std::set<std::string_view> {};
        for (auto const& argument: arguments)
        {
            names.insert(argument.name);
            auto const* const decl = findArgumentDecl(argument.name);
            CAPTURE(argument.name, argument.value);
            REQUIRE(decl != nullptr);
            CHECK(argumentAccepts(decl->kind, argument.value));
            if (decl->kind == ArgumentKind::Path)
                CHECK(isUnder(argument.value, _root));
        }
        CHECK(names == placeholders);
    }

    void checkCwd(Root root, std::string_view cwd)
    {
        CAPTURE(cwd);
        CHECK(argumentAccepts(ArgumentKind::Path, cwd));
        if (root == Root::Repos)
            CHECK(std::ranges::contains(_repoPaths, cwd));
        else
            CHECK((cwd == _root || isUnder(cwd, _root)));
    }

    void checkCommand(RunCommand const& command)
    {
        CAPTURE(command.tag, command.command);
        auto const* const decl = findCommand(command.command);
        REQUIRE(decl != nullptr);
        checkArguments(decl->argv, command.arguments);
        checkCwd(decl->root, command.cwd);
        CHECK(std::ranges::none_of(_pending,
                                   [&](RunCommand const& other) { return other.tag == command.tag; }));
        CHECK(!_tainted.contains(command.cwd));
        if (!command.tag.starts_with("poll/"))
        {
            checkWriteAhead(command.tag);
            if (command.command == "git-worktree-remove")
                CHECK(std::ranges::none_of(
                    _panes, [&](PaneFact const& pane) { return pane.tag == runOfTag(command.tag); }));
        }
    }

    void checkPane(OpenPane const& pane)
    {
        CAPTURE(pane.run);
        auto const* const decl = findPaneTemplate(pane.paneTemplate);
        REQUIRE(decl != nullptr);
        checkArguments(decl->argv, pane.arguments);
        checkCwd(decl->root, pane.cwd);
        auto const stored = _stored.find(pane.run);
        REQUIRE(stored != _stored.end());
        auto const run = runFromJson(stored->second);
        REQUIRE(run);
        CHECK(run->state == RunState { Preparing { PrepareStep::OpenPane } });
    }

    /// The stored run already names the call it awaits.
    void checkWriteAhead(std::string_view tag)
    {
        CAPTURE(tag);
        auto const stored = _stored.find(std::string(runOfTag(tag)));
        REQUIRE(stored != _stored.end());
        auto const run = runFromJson(stored->second);
        REQUIRE(run);
        CHECK(callTag(*run) == tag);
    }

    std::string _root;
    std::vector<std::string> _repoPaths;
    std::vector<PaneFact> _panes;
    std::optional<Engine> _engine;
    std::vector<Effect> _last;
    std::vector<RunCommand> _pending;
    std::vector<std::string> _foreignInFlight;
    std::vector<ReadFile> _reads;
    std::vector<std::string_view> _issued;
    std::map<std::string, std::string, std::less<>> _stored;
    std::set<std::string, std::less<>> _tainted;
    std::map<std::string, uint64_t, std::less<>> _deadlines;
    uint64_t _now = 0;
    uint64_t _wakeAt = 0;
};

[[nodiscard]] Harness booted(Settings const& settings = {})
{
    auto harness = Harness { contextFor(settings) };
    harness.boot();
    return harness;
}

[[nodiscard]] PaneFact paneOf(std::string_view id,
                              std::string_view pane = "p1",
                              ProgramLiveness program = ProgramLiveness::Alive,
                              std::optional<AgentStatus> status = AgentStatus::Working)
{
    return PaneFact {
        .pane = std::string(pane), .tag = std::string(id), .status = status, .program = program
    };
}

template <typename State>
[[nodiscard]] bool holds(Run const* run)
{
    return run != nullptr && std::holds_alternative<State>(run->state);
}

[[nodiscard]] std::optional<Finished> finishedOf(Run const* run)
{
    if (run == nullptr)
        return std::nullopt;
    if (auto const* const finished = std::get_if<Finished>(&run->state))
        return *finished;
    return std::nullopt;
}

[[nodiscard]] bool mentions(std::span<std::string const> lines, std::string_view text)
{
    return std::ranges::any_of(lines, [&](std::string const& line) { return line.contains(text); });
}

/// A stored run of the PR dispatch in @p state.
[[nodiscard]] Run storedRun(RunState state, std::string_view id = PrId)
{
    return Run { .id = std::string(id),
                 .repo = "contour",
                 .repoPath = std::string(ContourPath),
                 .forge = Forge::GitHub,
                 .remote = "origin",
                 .base = "master",
                 .item = ChangeRequestRef { 2375 },
                 .url = "https://github.com/contour-terminal/contour/pull/2375",
                 .task = "fix_ci",
                 .model = "sonnet",
                 .prompt = "/contour-workflows:fix-ci https://github.com/contour-terminal/contour/pull/2375",
                 .session = std::string(Session),
                 .branch = std::format("agent/{}", id),
                 .worktree = worktreeOf("contour", id),
                 .pane = std::nullopt,
                 .state = std::move(state),
                 .lastStatus = std::nullopt,
                 .failure = std::nullopt,
                 .boardPosts = 0,
                 .startedWallMs = DispatchWallMs,
                 .attempt = 7 };
}

[[nodiscard]] StoredEntry entryOf(Run const& run)
{
    return StoredEntry { .key = runKey(run.id), .value = toJson(run) };
}

// }}}

} // namespace

// {{{ 1. start

TEST_CASE("start checks the login and the hooks before any list", "[orchestrator][engine]")
{
    auto harness = Harness { contextFor({}) };
    auto const first = commandsIn(harness.last());
    CHECK(first == std::vector { "agent-setup-check"sv, "gh-auth"sv, "git-worktree-list"sv });
    CHECK(harness.pending("gh-auth")->cwd == ContourPath);
    CHECK(harness.pending("agent-setup-check")->cwd == WorktreeRoot);
    CHECK(countOf<WakeAt>(harness.last()) == 1);
    CHECK(countOf<PublishSummary>(harness.last()) == 1);

    harness.finish("agent-setup-check",
                   ok(readFixture(FixtureRoot::Testdata, "contour/setup-hooks-present.json")));
    CHECK(harness.engine().hooks() == HooksState::Present);
    CHECK(commandsIn(harness.last()).empty());

    auto const lists = commandsIn(harness.finish("gh-auth", ok()));
    CHECK(lists == std::vector { "gh-default-branch"sv, "gh-prs"sv, "gh-issues"sv });
    CHECK(harness.pending("gh-issues")->arguments.front().value == "assignee:@me");

    auto const filled =
        harness.finish("gh-prs", ok(readFixture(FixtureRoot::Testdata, "github/prs-open.json")));
    CHECK(harness.engine().changeRequests("contour").size() == 5);
    CHECK(countOf<PublishSummary>(filled) == 1);
    CHECK(countOf<Redraw>(filled) == 1);
    CHECK(all<PublishSummary>(filled).front().shortText.contains("✗2"));

    // The host creates a missing worktree root: the engine needs nothing else for it.
    auto settings = Settings {};
    auto missingRoot = contextFor(settings);
    missingRoot.host.worktreeRoot = "/home/u/no-such-root";
    auto const other = Harness { std::move(missingRoot) };
    CHECK(commandsIn(other.last()) == first);
    CHECK(other.engine().problems().empty());
}

// }}}
// {{{ 2. and 3. dispatch

TEST_CASE("a PR dispatch fetches, adds a detached worktree, checks the PR out and opens the agent",
          "[orchestrator][engine]")
{
    auto harness = booted();
    auto const dispatched = harness.feed(prDispatch());
    REQUIRE(dispatched.size() >= 2);
    CHECK(std::holds_alternative<StoreSet>(dispatched[0]));
    auto const fetch = std::get<RunCommand>(dispatched[1]);
    CHECK(fetch.command == "git-fetch");
    CHECK(fetch.cwd == ContourPath);

    auto const added = all<RunCommand>(harness.finish("git-fetch", ok()));
    REQUIRE(added.size() == 1);
    CHECK(added.front().command == "git-worktree-add");
    CHECK(added.front().cwd == ContourPath);
    auto const argument = [](RunCommand const& command, std::string_view name) {
        auto const found = std::ranges::find(command.arguments, name, &Argument::name);
        return found != command.arguments.end() ? found->value : std::string {};
    };
    CHECK(argument(added.front(), "path") == worktreeOf("contour", PrId));
    CHECK(argument(added.front(), "base") == "origin/master");

    auto const checkout = all<RunCommand>(harness.finish("git-worktree-add", ok()));
    REQUIRE(checkout.size() == 1);
    CHECK(checkout.front().command == "gh-pr-checkout");
    CHECK(checkout.front().cwd == worktreeOf("contour", PrId));
    CHECK(argument(checkout.front(), "branch") == std::format("agent/{}", PrId));
    CHECK(argument(checkout.front(), "number") == "2375");

    auto const opened = all<OpenPane>(harness.finish("gh-pr-checkout", ok()));
    REQUIRE(opened.size() == 1);
    CHECK(opened.front().paneTemplate == "agent");
    CHECK(opened.front().cwd == worktreeOf("contour", PrId));

    harness.feed(PaneOpenedFor { .run = std::string(PrId), .pane = "p1" });
    CHECK(holds<Running>(harness.run(PrId)));
}

TEST_CASE("an issue dispatch adds a branch and waits for the default branch", "[orchestrator][engine]")
{
    SECTION("before the default branch is known")
    {
        auto harness = Harness { contextFor({}) };
        harness.finish("gh-auth", ok());
        auto const refused = all<DispatchRefused>(harness.feed(issueDispatch()));
        REQUIRE(refused.size() == 1);
        CHECK(refused.front().reason == DispatchRefusal::BaseUnknown);
    }
    SECTION("with a configured base")
    {
        auto harness = Harness { contextFor({ .repos = R"([ { "name": "contour", "path": "~/projects/contour",
                                                             "forge": "github", "base": "main" } ])" }) };
        CHECK(!harness.pending("gh-default-branch"));
        auto const effects = harness.feed(issueDispatch());
        CHECK(countOf<DispatchRefused>(effects) == 0);
        auto const added = all<RunCommand>(harness.finish("git-fetch", ok()));
        REQUIRE(added.size() == 1);
        CHECK(added.front().command == "git-worktree-add-branch");
        CHECK(std::ranges::contains(added.front().arguments, "origin/main"s, &Argument::value));
        CHECK(countOf<OpenPane>(harness.finish("git-worktree-add-branch", ok())) == 1);
        CHECK(harness.commandsIssued("gh-pr-checkout") == 0);
    }
}

// }}}
// {{{ 4. limits

TEST_CASE("dispatch limits count active runs only", "[orchestrator][engine]")
{
    struct Row
    {
        std::string_view name;
        std::string_view limits;
        DispatchRefusal expected;
    };
    static constexpr auto Rows = std::array {
        Row {
            .name = "max_agents", .limits = R"("max_agents": 1,)", .expected = DispatchRefusal::AgentLimit },
        Row { .name = "max_agents_per_repo",
              .limits = R"("max_agents_per_repo": 1,)",
              .expected = DispatchRefusal::RepoLimit },
    };
    for (auto const& row: Rows)
    {
        CAPTURE(row.name);
        auto settings =
            Settings { .limits = row.limits,
                       .repos = R"([ { "name": "contour", "path": "~/projects/contour", "forge": "github" },
                                                { "name": "other", "path": "~/projects/other", "forge": "github" } ])",
                       .repoPaths = { ContourPath, OtherPath } };
        auto harness = booted(settings);
        harness.feed(prDispatch(2375));
        auto const refused = all<DispatchRefused>(harness.feed(prDispatch(2369)));
        REQUIRE(refused.size() == 1);
        CHECK(refused.front().reason == row.expected);
        auto const elsewhere = harness.feed(prDispatch(2369, "other"));
        CHECK(countOf<DispatchRefused>(elsewhere) == (row.expected == DispatchRefusal::RepoLimit ? 0U : 1U));
    }
}

TEST_CASE("the same item active refuses; finished does not", "[orchestrator][engine]")
{
    enum class Stage : uint8_t
    {
        Preparing,
        Running,
        Blocked,
        Done,
        Finished,
    };
    struct Row
    {
        Stage stage = Stage::Preparing;
        std::optional<DispatchRefusal> expected;
    };
    static constexpr auto Rows = std::array {
        Row { .stage = Stage::Preparing, .expected = DispatchRefusal::AlreadyRunning },
        Row { .stage = Stage::Running, .expected = DispatchRefusal::AlreadyRunning },
        Row { .stage = Stage::Blocked, .expected = DispatchRefusal::AlreadyRunning },
        Row { .stage = Stage::Done, .expected = DispatchRefusal::AlreadyRunning },
        Row { .stage = Stage::Finished, .expected = std::nullopt },
    };
    for (auto const& row: Rows)
    {
        CAPTURE(row.stage);
        auto harness = booted({ .limits = R"("max_agents": 1,)" });
        if (row.stage == Stage::Preparing)
            harness.feed(prDispatch());
        else
            harness.runToRunning(prDispatch());
        if (row.stage == Stage::Blocked)
            harness.list({ paneOf(PrId, "p1", ProgramLiveness::Alive, AgentStatus::Blocked) });
        if (row.stage == Stage::Done)
            harness.list({ paneOf(PrId, "p1", ProgramLiveness::Alive, AgentStatus::Done) });
        if (row.stage == Stage::Finished)
            harness.endCleanly(PrId);
        auto later = prDispatch();
        later.wallMs += 5'000;
        auto const refused = all<DispatchRefused>(harness.feed(later));
        if (row.expected)
        {
            REQUIRE(refused.size() == 1);
            CHECK(refused.front().reason == *row.expected);
        }
        else
            CHECK(refused.empty());
    }
}

TEST_CASE("dispatch refusals name what is unknown", "[orchestrator][engine]")
{
    struct Row
    {
        std::string_view name;
        std::string_view repo;
        std::string_view task;
        std::string_view model;
        DispatchRefusal expected;
    };
    static constexpr auto Rows = std::array {
        Row { .name = "repo",
              .repo = "nope",
              .task = "fix_ci",
              .model = "",
              .expected = DispatchRefusal::UnknownRepo },
        Row { .name = "task",
              .repo = "contour",
              .task = "nope",
              .model = "",
              .expected = DispatchRefusal::UnknownTask },
        Row { .name = "task for issues",
              .repo = "contour",
              .task = "issue",
              .model = "",
              .expected = DispatchRefusal::UnknownTask },
        Row { .name = "model",
              .repo = "contour",
              .task = "fix_ci",
              .model = "haiku",
              .expected = DispatchRefusal::UnknownModel },
    };
    auto harness = booted();
    for (auto const& row: Rows)
    {
        CAPTURE(row.name);
        auto request = prDispatch();
        request.repo = row.repo;
        request.task = row.task;
        request.model = row.model;
        auto const refused = all<DispatchRefused>(harness.feed(request));
        REQUIRE(refused.size() == 1);
        CHECK(refused.front().reason == row.expected);
    }
    auto badUrl = prDispatch();
    badUrl.url = "http://example.com/x";
    CHECK(all<DispatchRefused>(harness.feed(badUrl)).front().reason == DispatchRefusal::BadPrompt);
    auto unlisted = prDispatch(99);
    CHECK(all<DispatchRefused>(harness.feed(unlisted)).front().reason == DispatchRefusal::BaseUnknown);
    CHECK(harness.engine().runs().empty());
}

// }}}
// {{{ 5. to 7. endings

TEST_CASE("a clean ending removes the worktree and frees the slot", "[orchestrator][engine]")
{
    auto harness = booted({ .limits = R"("max_agents": 1,)" });
    harness.runToRunning(prDispatch());
    harness.list({});
    CHECK(harness.hasRead(std::format("{}/.git", PrId)));
    harness.read("/.git", intactGitFile(ContourPath, PrId));
    harness.finish("git-common-dir", ok(commonDirOf(ContourPath)));
    harness.finish("git-index-flags", ok(CleanIndex));
    harness.finish("git-head-reachable", ok("refs/heads/agent/x\n"));
    harness.finish("git-operation-state", ok(CleanLongStatus));
    harness.finish("git-status", ok());
    auto const removed = harness.finish("git-worktree-remove", ok());
    auto const finished = finishedOf(harness.run(PrId));
    REQUIRE(finished);
    CHECK(finished->ended == Ended::NeverReported);
    CHECK(finished->fate == Fate::Removed);
    CHECK(countOf<StoreSet>(removed) == 1);
    CHECK(countOf<StoreRemove>(removed) == 0);
    REQUIRE(countOf<PublishSummary>(removed) == 1);
    CHECK(all<PublishSummary>(removed).front().shortText.contains("✓1 new"));

    auto another = prDispatch(2369);
    CHECK(countOf<DispatchRefused>(harness.feed(another)) == 0);
}

TEST_CASE("a reported run ends as completed", "[orchestrator][engine]")
{
    auto harness = booted();
    harness.runToRunning(prDispatch());
    harness.list({ paneOf(PrId) });
    harness.endCleanly(PrId);
    auto const finished = finishedOf(harness.run(PrId));
    REQUIRE(finished);
    CHECK(finished->ended == Ended::Completed);
    CHECK(finished->fate == Fate::Removed);
}

TEST_CASE("a modified worktree is kept, then removed once clean", "[orchestrator][engine]")
{
    struct Row
    {
        std::string_view name;
    };
    static constexpr auto Rows = std::array { Row { .name = "next poll" }, Row { .name = "retry" } };
    for (auto const& row: Rows)
    {
        CAPTURE(row.name);
        auto harness = booted();
        harness.runToRunning(prDispatch());
        harness.list({ paneOf(PrId) });
        harness.list({});
        harness.read("/.git", intactGitFile(ContourPath, PrId));
        harness.finish("git-common-dir", ok(commonDirOf(ContourPath)));
        harness.finish("git-index-flags", ok(CleanIndex));
        harness.finish("git-head-reachable", ok("refs/heads/agent/x\n"));
        harness.finish("git-operation-state", ok(CleanLongStatus));
        harness.finish("git-status", ok(" M f\n"));
        auto const kept = finishedOf(harness.run(PrId));
        REQUIRE(kept);
        CHECK(kept->fate == Fate::Kept);
        CHECK(harness.commandsIssued("git-worktree-remove") == 0);

        if (row.name == "retry")
            harness.feed(RetryRemovalOf { .run = std::string(PrId) });
        else
            harness.wake(2 * Minute);
        harness.read("/.git", intactGitFile(ContourPath, PrId));
        harness.finish("git-common-dir", ok(commonDirOf(ContourPath)));
        harness.finish("git-index-flags", ok(CleanIndex));
        harness.finish("git-head-reachable", ok("refs/heads/agent/x\n"));
        harness.finish("git-operation-state", ok(CleanLongStatus));
        harness.finish("git-status", ok());
        harness.finish("git-worktree-remove", ok());
        auto const removed = finishedOf(harness.run(PrId));
        REQUIRE(removed);
        CHECK(removed->fate == Fate::Removed);
        CHECK(removed->ended == Ended::Completed);
    }
}

TEST_CASE("a worktree removed by hand ends the run; anything else keeps it", "[orchestrator][engine]")
{
    struct Row
    {
        FileReadError error = FileReadError::DirectoryMissing;
        Fate fate = Fate::Removed;
        std::optional<Ended> ended;
    };
    static constexpr auto Rows = std::array {
        Row { .error = FileReadError::DirectoryMissing, .fate = Fate::Removed, .ended = Ended::WorktreeGone },
        Row { .error = FileReadError::NotRegular, .fate = Fate::Kept, .ended = std::nullopt },
        Row { .error = FileReadError::Unreadable, .fate = Fate::Kept, .ended = std::nullopt },
    };
    for (auto const& row: Rows)
    {
        CAPTURE(row.error);
        auto harness = booted();
        harness.runToRunning(prDispatch());
        harness.list({ paneOf(PrId) });
        harness.list({});
        harness.read("/.git", std::unexpected { row.error });
        auto const finished = finishedOf(harness.run(PrId));
        REQUIRE(finished);
        CHECK(finished->fate == row.fate);
        if (row.ended)
            CHECK(finished->ended == *row.ended);
        CHECK(harness.commandsIssued("git-head-reachable") == 0);
    }
}

TEST_CASE("a running run with a listed pane is never ended by the worktree list", "[orchestrator][engine]")
{
    auto harness = booted();
    harness.runToRunning(prDispatch());
    harness.list({ paneOf(PrId) });
    for (auto const& printed: std::array { ""s, worktreeList(ContourPath), "garbage"s })
    {
        harness.wake(2 * Minute);
        harness.finish("git-worktree-list", ok(printed));
        harness.boot();
        harness.list({ paneOf(PrId) });
        CHECK(holds<Running>(harness.run(PrId)));
    }
}

// }}}
// {{{ 8. and 9. lost hints and cold restart

TEST_CASE("lost hints: a pane gone is seen in the listing after a later wake", "[orchestrator][engine]")
{
    auto harness = booted();
    harness.runToRunning(prDispatch());
    harness.list({ paneOf(PrId) });
    // The hint that the pane went away was lost; the slow timer still lists the panes.
    harness.wake(static_cast<uint64_t>(ReconcileEvery.count()));
    auto const effects = harness.list({});
    CHECK(countOf<ReadFile>(effects) == 1);
    CHECK(holds<Ending>(harness.run(PrId)));
}

TEST_CASE("cold restart: a running run whose agent is gone has its pane closed, then ends",
          "[orchestrator][engine]")
{
    auto context = contextFor({});
    auto stored = storedRun(Running {});
    stored.pane = "p1";
    stored.lastStatus = AgentStatus::Working;
    context.stored = { entryOf(stored) };
    context.panes = { paneOf(PrId, "p1", ProgramLiveness::Gone) };
    auto harness = Harness { std::move(context) };
    auto const closes = all<ClosePane>(harness.last());
    REQUIRE(closes.size() == 1);
    CHECK(closes.front().pane == "p1");
    CHECK(holds<Running>(harness.run(PrId)));
    harness.boot();
    harness.endCleanly(PrId);
    auto const finished = finishedOf(harness.run(PrId));
    REQUIRE(finished);
    CHECK(finished->ended == Ended::Completed);
}

// }}}
// {{{ 10. guest restart mid-dispatch

TEST_CASE("guest restart mid-dispatch", "[orchestrator][engine]")
{
    enum class Known : uint8_t
    {
        InFlight,
        Finished,
        Neither,
    };
    /// What the start's pane listing holds.
    enum class Tab : uint8_t
    {
        Unlisted, ///< No tab tagged with the run.
        Alive,    ///< The run's tab, its agent running.
    };
    /// Where a run waiting for its tab stands once started.
    enum class Settled : uint8_t
    {
        NotChecked, ///< The row checks the effects only.
        Running,    ///< It adopted the listed tab.
        Closing,    ///< It closes whatever tab is tagged with it, then waits for none to be listed.
    };
    struct Row
    {
        std::string_view name;
        RunState state;
        Known known;
        std::string_view next; ///< The command, or ".git" for the read, that follows; empty: none.
        Tab tab = Tab::Unlisted;
        Settled settled = Settled::NotChecked;
    };
    auto const ending = [](EndStep step) {
        return Ending { .step = step,
                        .cause = Ended::Completed,
                        .resumedFrom = std::nullopt,
                        .removal = Removal::NotIssued };
    };
    auto const rows = std::array {
        Row { .name = "fetch, in flight",
              .state = Preparing { PrepareStep::Fetch },
              .known = Known::InFlight,
              .next = "git-worktree-add" },
        Row { .name = "fetch, finished",
              .state = Preparing { PrepareStep::Fetch },
              .known = Known::Finished,
              .next = "git-worktree-add" },
        Row { .name = "fetch, lost",
              .state = Preparing { PrepareStep::Fetch },
              .known = Known::Neither,
              .next = "" },
        Row { .name = "worktree-add, in flight",
              .state = Preparing { PrepareStep::WorktreeAdd },
              .known = Known::InFlight,
              .next = "gh-pr-checkout" },
        Row { .name = "worktree-add, finished",
              .state = Preparing { PrepareStep::WorktreeAdd },
              .known = Known::Finished,
              .next = "gh-pr-checkout" },
        Row { .name = "worktree-add, lost",
              .state = Preparing { PrepareStep::WorktreeAdd },
              .known = Known::Neither,
              .next = ".git" },
        Row { .name = "checkout, in flight",
              .state = Preparing { PrepareStep::Checkout },
              .known = Known::InFlight,
              .next = "" },
        Row { .name = "checkout, finished",
              .state = Preparing { PrepareStep::Checkout },
              .known = Known::Finished,
              .next = "" },
        Row { .name = "checkout, lost",
              .state = Preparing { PrepareStep::Checkout },
              .known = Known::Neither,
              .next = ".git" },
        // Reconciling comes before the lost calls are resolved: a run waiting for its tab adopts a listed one
        // instead of being told its opening was lost.
        Row { .name = "open-pane, its tab listed",
              .state = Preparing { PrepareStep::OpenPane },
              .known = Known::Neither,
              .next = "",
              .tab = Tab::Alive,
              .settled = Settled::Running },
        Row { .name = "open-pane, no tab listed",
              .state = Preparing { PrepareStep::OpenPane },
              .known = Known::Neither,
              .next = "",
              .tab = Tab::Unlisted,
              .settled = Settled::Closing },
        Row { .name = "verify, lost",
              .state = ending(EndStep::Verify),
              .known = Known::Neither,
              .next = ".git" },
        Row { .name = "common-dir, finished",
              .state = ending(EndStep::CommonDir),
              .known = Known::Finished,
              .next = ".git" },
        Row { .name = "index-flags, finished",
              .state = ending(EndStep::IndexFlags),
              .known = Known::Finished,
              .next = "git-head-reachable" },
        Row { .name = "index-flags, lost",
              .state = ending(EndStep::IndexFlags),
              .known = Known::Neither,
              .next = ".git" },
        Row { .name = "status, in flight",
              .state = ending(EndStep::Status),
              .known = Known::InFlight,
              .next = "git-worktree-remove" },
        Row { .name = "status, finished",
              .state = ending(EndStep::Status),
              .known = Known::Finished,
              .next = "git-worktree-remove" },
        Row { .name = "status, lost",
              .state = ending(EndStep::Status),
              .known = Known::Neither,
              .next = ".git" },
        Row { .name = "remove, in flight",
              .state = ending(EndStep::Remove),
              .known = Known::InFlight,
              .next = "" },
        Row { .name = "remove, finished",
              .state = ending(EndStep::Remove),
              .known = Known::Finished,
              .next = "" },
        Row { .name = "remove, lost",
              .state = ending(EndStep::Remove),
              .known = Known::Neither,
              .next = ".git" },
    };
    for (auto const& row: rows)
    {
        CAPTURE(row.name);
        auto stored = storedRun(row.state);
        auto const tag = *callTag(stored);
        auto context = contextFor({});
        context.stored = { entryOf(stored) };
        if (row.known == Known::InFlight)
            context.inFlight = { tag };
        if (row.known == Known::Finished)
            context.finished = { FinishedResult { .tag = tag, .outcome = ok() } };
        if (row.tab == Tab::Alive)
            context.panes = { paneOf(PrId, "p1") };
        auto harness = Harness { std::move(context) };
        auto const runEffects = [&](std::span<Effect const> effects) {
            auto found = std::vector<std::string> {};
            for (auto const& command: all<RunCommand>(effects))
                if (!command.tag.starts_with("poll/"))
                    found.emplace_back(command.command);
            found.insert(found.end(), countOf<ReadFile>(effects), ".git");
            return found;
        };
        auto const started = runEffects(harness.last());
        auto const expectNext = [&](std::vector<std::string> const& effects) {
            if (row.next.empty())
                CHECK(effects.empty());
            else
                CHECK(effects == std::vector { std::string(row.next) });
        };
        if (row.known == Known::InFlight)
        {
            CHECK(started.empty());
            CHECK(harness.run(PrId)->state == row.state);
            auto const answered = harness.feed(ProcessFinished { .tag = tag, .outcome = ok() });
            expectNext(runEffects(answered));
        }
        else
            expectNext(started);
        CHECK(countOf<ClosePane>(harness.last()) == 0);
        switch (row.settled)
        {
            case Settled::NotChecked: break;
            case Settled::Running:
                CHECK(holds<Running>(harness.run(PrId)));
                CHECK(harness.run(PrId)->pane == "p1");
                continue;
            case Settled::Closing:
                CHECK(harness.run(PrId)->state
                      == RunState { Ending { .step = EndStep::ClosePane,
                                             .cause = Ended::PreparationFailed,
                                             .resumedFrom = std::nullopt } });
                continue;
        }
        if (row.known == Known::Neither && row.next.empty())
        {
            auto const finished = finishedOf(harness.run(PrId));
            REQUIRE(finished);
            CHECK(finished->ended == Ended::PreparationFailed);
        }
    }
}

TEST_CASE("a stale reply to an earlier attempt is ignored", "[orchestrator][engine]")
{
    auto stored = storedRun(Ending { .step = EndStep::Status,
                                     .cause = Ended::Completed,
                                     .resumedFrom = std::nullopt,
                                     .removal = Removal::NotIssued });
    stored.attempt = 3;
    auto stale = stored;
    stale.attempt = 2;
    auto context = contextFor({});
    context.stored = { entryOf(stored) };
    // The earlier attempt's empty status would mean "remove".
    context.finished = { FinishedResult { .tag = *callTag(stale), .outcome = ok() } };
    auto harness = Harness { std::move(context) };
    CHECK(harness.hasRead("/.git"));
    CHECK(harness.commandsIssued("git-worktree-remove") == 0);
    harness.boot();
    harness.read("/.git", intactGitFile(ContourPath, PrId));
    harness.finish("git-common-dir", ok(commonDirOf(ContourPath)));
    harness.finish("git-index-flags", ok(CleanIndex));
    harness.finish("git-head-reachable", ok("refs/heads/agent/x\n"));
    harness.finish("git-operation-state", ok(CleanLongStatus));
    auto const status = harness.pending("git-status");
    REQUIRE(status);
    auto const replayed = harness.feed(ProcessFinished { .tag = *callTag(stale), .outcome = ok() });
    CHECK(commandsIn(replayed).empty());
    CHECK(harness.run(PrId)->state
          == RunState { Ending { .step = EndStep::Status,
                                 .cause = Ended::Completed,
                                 .resumedFrom = std::nullopt,
                                 .removal = Removal::NotIssued } });
}

TEST_CASE("nothing deletes from a worktree while a tab of its run is listed", "[orchestrator][engine]")
{
    auto const status = Ending { .step = EndStep::Status,
                                 .cause = Ended::Completed,
                                 .resumedFrom = std::nullopt,
                                 .removal = Removal::NotIssued };
    auto stored = storedRun(status);
    auto context = contextFor({});
    context.stored = { entryOf(stored) };
    context.finished = { FinishedResult { .tag = *callTag(stored), .outcome = ok() } };
    context.panes = { paneOf(PrId, "p9") };
    auto harness = Harness { std::move(context) };
    CHECK(harness.commandsIssued("git-worktree-remove") == 0);
    // Kept, and already being checked again by the start's poll.
    REQUIRE(harness.run(PrId) != nullptr);
    auto const* const ending = std::get_if<Ending>(&harness.run(PrId)->state);
    REQUIRE(ending != nullptr);
    REQUIRE(ending->resumedFrom);
    CHECK(ending->resumedFrom->fate == Fate::Kept);
    CHECK(ending->resumedFrom->entries
          == std::vector { "git worktree remove failed: a tab of the run is still open"s });
}

TEST_CASE("a listing of the driver's own records never ends a close it did not record",
          "[orchestrator][engine]")
{
    struct Row
    {
        PaneListing listing = PaneListing::OwnRecordsOnly;
        std::size_t reads = 0;
    };
    static constexpr auto Rows = std::array {
        Row { .listing = PaneListing::OwnRecordsOnly, .reads = 0 },
        Row { .listing = PaneListing::Tagged, .reads = 1 },
    };
    for (auto const& row: Rows)
    {
        CAPTURE(row.listing);
        auto context = contextFor({});
        context.stored = { entryOf(storedRun(Ending { .step = EndStep::ClosePane,
                                                      .cause = Ended::EndedByYou,
                                                      .resumedFrom = std::nullopt,
                                                      .removal = Removal::NotIssued })) };
        context.paneListing = PaneListing::OwnRecordsOnly;
        auto harness = Harness { std::move(context) };
        CHECK(countOf<ReadFile>(harness.last()) == 0);
        harness.boot();
        CHECK(countOf<ReadFile>(harness.list({}, row.listing)) == row.reads);
    }
}

// }}}
// {{{ 11. reachability and operations

TEST_CASE("an unreachable HEAD or an operation in progress keeps the worktree", "[orchestrator][engine]")
{
    struct Row
    {
        std::string_view name;
        std::string_view reachable;
        std::string_view operation;
    };
    static constexpr auto Rows = std::array {
        Row { .name = "detached HEAD in no ref", .reachable = "", .operation = "" },
        Row { .name = "rebase in progress",
              .reachable = "refs/heads/agent/x\n",
              .operation = "rebase in progress; onto <hash>\nYou are currently rebasing branch 'agent/x' on "
                           "'<hash>'.\n" },
    };
    for (auto const& row: Rows)
    {
        CAPTURE(row.name);
        auto harness = booted();
        harness.runToRunning(prDispatch());
        harness.list({ paneOf(PrId) });
        harness.list({});
        harness.read("/.git", intactGitFile(ContourPath, PrId));
        harness.finish("git-common-dir", ok(commonDirOf(ContourPath)));
        harness.finish("git-index-flags", ok(CleanIndex));
        harness.finish("git-head-reachable", ok(row.reachable));
        if (!row.operation.empty())
            harness.finish("git-operation-state", ok(row.operation));
        auto const finished = finishedOf(harness.run(PrId));
        REQUIRE(finished);
        CHECK(finished->fate == Fate::Kept);
        CHECK(harness.commandsIssued("git-status") == 0);
        CHECK(harness.commandsIssued("git-worktree-remove") == 0);
    }
}

// }}}
// {{{ 12. ending a Done run

TEST_CASE("ending a Done run closes its tab, then checks the worktree", "[orchestrator][engine]")
{
    SECTION("EndRun")
    {
        auto harness = booted();
        harness.runToRunning(prDispatch());
        harness.list({ paneOf(PrId, "p1", ProgramLiveness::Alive, AgentStatus::Done) });
        REQUIRE(holds<Done>(harness.run(PrId)));
        auto const closes = all<ClosePane>(harness.feed(EndRun { .run = std::string(PrId) }));
        REQUIRE(closes.size() == 1);
        CHECK(closes.front().pane == "p1");
        harness.endCleanly(PrId);
        CHECK(finishedOf(harness.run(PrId))->ended == Ended::EndedByYou);
    }
    SECTION("closing the tab")
    {
        auto harness = booted();
        harness.runToRunning(prDispatch());
        harness.list({ paneOf(PrId, "p1", ProgramLiveness::Alive, AgentStatus::Done) });
        harness.endCleanly(PrId);
        CHECK(finishedOf(harness.run(PrId))->ended == Ended::Completed);
    }
    SECTION("a preparing run is not ended")
    {
        auto harness = booted();
        harness.feed(prDispatch());
        CHECK(harness.feed(EndRun { .run = std::string(PrId) }).empty());
        CHECK(holds<Preparing>(harness.run(PrId)));
    }
}

// }}}
// {{{ 13. the Finished list

TEST_CASE("finished entries are new until seen and purged after the retention", "[orchestrator][engine]")
{
    auto harness = booted({ .limits = R"("finished_retention": "1h",)" });
    harness.runToRunning(prDispatch());
    harness.list({ paneOf(PrId) });
    harness.endCleanly(PrId);
    CHECK(finishedOf(harness.run(PrId))->seen == Seen::New);
    harness.feed(MarkRunSeen { .run = std::string(PrId) });
    CHECK(finishedOf(harness.run(PrId))->seen == Seen::Opened);

    // A kept one of another item, ended at the same time.
    harness.runToRunning(prDispatch(2369), "p2");
    auto const keptId = "pr-2369-20261113-150847"sv;
    harness.list({ paneOf(keptId, "p2") });
    harness.list({});
    harness.read("/.git", intactGitFile(ContourPath, keptId));
    harness.finish("git-common-dir", ok(commonDirOf(ContourPath)));
    harness.finish("git-index-flags", ok(CleanIndex));
    harness.finish("git-head-reachable", ok(""));
    REQUIRE(finishedOf(harness.run(keptId))->fate == Fate::Kept);

    auto const purged = harness.wake(61 * Minute);
    CHECK(harness.run(PrId) == nullptr);
    CHECK(std::ranges::contains(all<StoreRemove>(purged), runKey(PrId), &StoreRemove::key));
    CHECK(harness.run(keptId) != nullptr);
}

// }}}
// {{{ 14. status boards

TEST_CASE("the status board is requested at the threshold and re-posted", "[orchestrator][engine]")
{
    auto harness = booted();
    harness.runToRunning(prDispatch());
    harness.list({ paneOf(PrId) });
    CHECK(countOf<PostInbox>(harness.wake(14 * Minute)) == 0);
    auto const first = harness.wake(Minute + 1);
    REQUIRE(countOf<PostInbox>(first) == 1);
    CHECK(all<PostInbox>(first).front().message == "status-board");
    CHECK(all<PostInbox>(first).front().pane == "p1");
    CHECK(countOf<StoreSet>(first) == 1);
    CHECK(countOf<ReadFile>(first) == 0);

    auto posts = std::size_t { 1 };
    for ([[maybe_unused]] auto const wake: std::views::iota(0, 30))
    {
        auto const effects = harness.wake(Minute);
        CHECK(countOf<ReadFile>(effects) == 1);
        while (harness.hasRead("/STATUS.md"))
            harness.read("/STATUS.md", std::unexpected { FileReadError::DirectoryMissing });
        posts += countOf<PostInbox>(effects);
    }
    CHECK(posts == BoardMaxPosts);

    // A board that exists stops the reads.
    harness.wake(Minute);
    harness.read("/STATUS.md", "# Status\n"s);
    CHECK(countOf<ReadFile>(harness.wake(Minute)) == 0);
}

TEST_CASE("no status board while blocked, on request within budget, none with 0", "[orchestrator][engine]")
{
    SECTION("blocked or done")
    {
        for (auto const status: std::array { AgentStatus::Blocked, AgentStatus::Done })
        {
            CAPTURE(status);
            auto harness = booted();
            harness.runToRunning(prDispatch());
            harness.list({ paneOf(PrId, "p1", ProgramLiveness::Alive, status) });
            CHECK(countOf<PostInbox>(harness.wake(20 * Minute)) == 0);
        }
    }
    SECTION("requested")
    {
        auto harness = booted();
        harness.runToRunning(prDispatch());
        harness.list({ paneOf(PrId) });
        auto posts = std::size_t { 0 };
        for ([[maybe_unused]] auto const request: std::views::iota(0, 5))
            posts += countOf<PostInbox>(harness.feed(RequestBoard { .run = std::string(PrId) }));
        CHECK(posts == BoardMaxPosts);
        harness.wake(61 * Minute);
        CHECK(countOf<PostInbox>(harness.feed(RequestBoard { .run = std::string(PrId) })) == 1);
    }
    SECTION("disabled")
    {
        auto harness = booted({ .limits = R"("status_board_after": "0",)" });
        harness.runToRunning(prDispatch());
        harness.list({ paneOf(PrId) });
        auto posts = std::size_t { 0 };
        for ([[maybe_unused]] auto const wake: std::views::iota(0, 40))
            posts += countOf<PostInbox>(harness.wake(Minute));
        CHECK(posts == 0);
    }
}

// }}}
// {{{ 15. problems and back-off

TEST_CASE("a failed login backs off and lists nothing", "[orchestrator][engine]")
{
    auto harness = Harness { contextFor({}) };
    harness.finish("agent-setup-check", ok("[]"));
    harness.finish("git-worktree-list", ok(worktreeList(ContourPath)));
    harness.finish("gh-auth", failed(1, readFixture(FixtureRoot::Testdata, "github/auth-failed.stderr")));
    CHECK(mentions(harness.engine().problems(), "gh is not logged in"));
    CHECK(harness.commandsIssued("gh-prs") == 0);
    auto expected = Minute * 4;
    for ([[maybe_unused]] auto const poll: std::views::iota(0, 5))
    {
        CHECK(harness.wakeAt() - harness.now() == expected);
        harness.feed(Wake { .monoMs = harness.wakeAt(), .wallMs = DispatchWallMs });
        harness.finish("git-worktree-list", ok(worktreeList(ContourPath)));
        harness.finish("gh-auth", failed(1, "not logged in"));
        CHECK(harness.commandsIssued("gh-prs") == 0);
        expected = std::min(expected * 2, static_cast<uint64_t>(MaxPollBackoff.count()));
    }
    CHECK(expected == static_cast<uint64_t>(MaxPollBackoff.count()));
    CHECK(harness.wakeAt() - harness.now() == expected);
    harness.feed(Wake { .monoMs = harness.wakeAt(), .wallMs = DispatchWallMs });
    harness.boot();
    CHECK(!mentions(harness.engine().problems(), "gh is not logged in"));
    CHECK(harness.wakeAt() - harness.now() == 2 * Minute);

    // A clean poll lists without asking for the login again.
    harness.feed(Wake { .monoMs = harness.wakeAt(), .wallMs = DispatchWallMs });
    CHECK(!harness.pending("gh-auth"));
    CHECK(harness.pending("gh-prs"));
}

TEST_CASE("bad lists", "[orchestrator][engine]")
{
    struct Row
    {
        std::string_view name;
        ProcessOutcome outcome;
        std::string_view expected;
    };
    auto const rows = std::array {
        Row { .name = "not JSON", .outcome = ok("<html>"), .expected = "could not be read" },
        Row { .name = "non-zero", .outcome = failed(1, "HTTP 502"), .expected = "HTTP 502" },
        Row { .name = "directory missing", .outcome = cwdMissing(), .expected = "does not exist" },
    };
    for (auto const& row: rows)
    {
        CAPTURE(row.name);
        auto harness = booted();
        REQUIRE(harness.engine().changeRequests("contour").size() == 5);
        harness.wake(2 * Minute);
        harness.finish("gh-prs", row.outcome);
        CHECK(mentions(harness.engine().problems(), row.expected));
        CHECK(mentions(harness.engine().problems(), "the pull request list is incomplete"));
        harness.boot();
        // The next poll checks the login again, after twice the interval.
        CHECK(harness.wakeAt() - harness.now() == 4 * Minute);
        harness.feed(Wake { .monoMs = harness.wakeAt(), .wallMs = DispatchWallMs });
        CHECK(harness.pending("gh-auth"));
    }
}

TEST_CASE("a cut list is incomplete and no failure", "[orchestrator][engine]")
{
    auto const fixture = readFixture(FixtureRoot::Testdata, "github/prs-open.json");
    struct Row
    {
        std::string_view name;
        std::string_view output;
        std::size_t listed;        ///< The pull requests listed afterwards.
        std::string_view expected; ///< In a problem line.
    };
    auto const rows = std::array {
        Row { .name = "cut in the last entry: the entries before it",
              .output = std::string_view { fixture }.substr(0, fixture.rfind("[1847,")), // the last row
              .listed = 4,
              .expected =
                  "the pull request list is incomplete: its output was cut at the host's limit after 4" },
        Row { .name = "cut in the first entry: the previous list",
              .output = std::string_view { fixture }.substr(0, 40),
              .listed = 5,
              .expected =
                  "the pull request list is incomplete: its output was cut at the host's limit before" },
    };
    for (auto const& row: rows)
    {
        CAPTURE(row.name);
        auto harness = booted();
        REQUIRE(harness.engine().changeRequests("contour").size() == 5);
        harness.wake(2 * Minute);
        harness.finish("gh-prs", truncated(row.output));
        CHECK(harness.engine().changeRequests("contour").size() == row.listed);
        CHECK(mentions(harness.engine().problems(), row.expected));
        CHECK(!mentions(harness.engine().problems(), "failed"));
        harness.boot();
        // The host would cut it at the same place again: no back-off, and no new login check.
        CHECK(harness.wakeAt() - harness.now() == 2 * Minute);
        harness.feed(Wake { .monoMs = harness.wakeAt(), .wallMs = DispatchWallMs });
        CHECK(!harness.pending("gh-auth"));
        CHECK(harness.pending("gh-prs"));
    }
}

TEST_CASE("a list at the limit or with skipped entries is shown with a notice", "[orchestrator][engine]")
{
    auto many = std::string("[");
    for (auto const number: std::views::iota(1, 101))
        many +=
            std::format(R"({}[{},"t","https://github.com/o/r/pull/{}","h","master",false,false,"",[],[]])",
                        number == 1 ? "" : ",",
                        number,
                        number);
    many += "]";
    auto const skipped = R"([[1,"t","https://github.com/o/r/pull/1","h","..bad",false,false,"",[],[]]])"s;
    struct Row
    {
        std::string_view name;
        std::string output;
        std::string_view expected;
    };
    auto const rows = std::array {
        Row { .name = "100", .output = many, .expected = "shows the first 100" },
        Row { .name = "skipped", .output = skipped, .expected = "left out 1" },
    };
    for (auto const& row: rows)
    {
        CAPTURE(row.name);
        auto harness = Harness { contextFor({}) };
        harness.finish("gh-auth", ok());
        harness.finish("gh-prs", ok(row.output));
        CHECK(mentions(harness.engine().problems(), row.expected));
    }
}

TEST_CASE("a missing directory and missing hooks are problems", "[orchestrator][engine]")
{
    auto harness = Harness { contextFor({}) };
    harness.finish("gh-auth", cwdMissing());
    CHECK(mentions(harness.engine().problems(), "/home/u/projects/contour does not exist"));
    harness.finish("agent-setup-check",
                   ok(readFixture(FixtureRoot::Testdata, "contour/setup-hooks-missing.json")));
    CHECK(harness.engine().hooks() == HooksState::Missing);
    CHECK(mentions(harness.engine().problems(), "hooks are missing"));
}

TEST_CASE("hooksStateOf reads the setup dry run", "[orchestrator][engine]")
{
    struct Row
    {
        std::string_view name;
        std::string text;
        HooksState expected;
    };
    auto const rows = std::array {
        Row { .name = "present",
              .text = readFixture(FixtureRoot::Testdata, "contour/setup-hooks-present.json"),
              .expected = HooksState::Present },
        Row { .name = "missing",
              .text = readFixture(FixtureRoot::Testdata, "contour/setup-hooks-missing.json"),
              .expected = HooksState::Missing },
        Row { .name = "no claude-code",
              .text = R"([{"agent":"codex","selection":"skipped"}])",
              .expected = HooksState::Missing },
        Row { .name = "claude-code skipped",
              .text = R"([{"agent":"claude-code","selection":"skipped"}])",
              .expected = HooksState::Missing },
        Row { .name = "not JSON", .text = "nope", .expected = HooksState::Unknown },
        Row { .name = "cut", .text = R"([{"agent":"claude-code")", .expected = HooksState::Unknown },
    };
    for (auto const& row: rows)
    {
        CAPTURE(row.name);
        CHECK(hooksStateOf(row.text) == row.expected);
    }
}

// }}}
// {{{ 16. concurrency

TEST_CASE("never more than MaxInFlight commands, and runs ahead of polls", "[orchestrator][engine]")
{
    auto settings = Settings {
        .limits = "",
        .repos =
            R"([ { "name": "contour", "path": "~/projects/contour", "forge": "github", "base": "master" },
                                            { "name": "other", "path": "~/projects/other", "forge": "github" },
                                            { "name": "third", "path": "~/projects/third", "forge": "github" } ])",
        .repoPaths = { ContourPath, OtherPath, ThirdPath }
    };
    auto harness = Harness { contextFor(settings) };
    CHECK(harness.pendingCount() == MaxInFlight);
    // gh-auth answered: six lists queue behind the third worktree list.
    harness.finish("gh-auth", ok());
    CHECK(harness.pendingCount() == MaxInFlight);
    auto const dispatched = harness.feed(issueDispatch());
    CHECK(countOf<DispatchRefused>(dispatched) == 0);
    harness.finish("agent-setup-check", ok("[]"));
    // The freed slot goes to the run's fetch, though polls were queued first.
    REQUIRE(harness.pending("git-fetch"));
    CHECK(harness.pendingCount() == MaxInFlight);
}

// }}}
// {{{ 17. orphans

TEST_CASE("orphan worktrees are listed and reported", "[orchestrator][engine]")
{
    auto harness = Harness { contextFor({}) };
    auto const orphanPath = worktreeOf("contour", "pr-1-20260101-000000");
    harness.finish("git-worktree-list",
                   ok(worktreeList(ContourPath, agentWorktree(orphanPath, "agent/pr-1-20260101-000000"))));
    auto const found = harness.engine().orphans("contour");
    REQUIRE(found.size() == 1);
    CHECK(found.front().path == orphanPath);
    CHECK(mentions(harness.engine().problems(), "orphan worktree"));
    CHECK(harness.engine().mainCheckoutOf("contour") == ContourPath);
    // Listed although gh is logged out.
    harness.finish("gh-auth", failed(1, "not logged in"));
    CHECK(harness.engine().orphans("contour").size() == 1);
}

// }}}
// {{{ 19. bad configuration

TEST_CASE("a bad configuration is one problem line and refuses every dispatch", "[orchestrator][engine]")
{
    auto context = contextFor({});
    context.configJson = R"({ "worktree_root": 7 })";
    auto harness = Harness { std::move(context) };
    CHECK(harness.engine().problems().size() == 1);
    CHECK(commandsIn(harness.last()).empty());
    auto const refused = all<DispatchRefused>(harness.feed(prDispatch()));
    REQUIRE(refused.size() == 1);
    CHECK(refused.front().reason == DispatchRefusal::NotConfigured);
    CHECK(commandsIn(harness.wake(10 * Minute)).empty());
}

TEST_CASE("a bad configuration runs no command whatever the user asks for", "[orchestrator][engine]")
{
    // Ended by the user, so the engine works on it outside any configuration.
    auto kept = storedRun(Finished { .ended = Ended::EndedByYou,
                                     .fate = Fate::Kept,
                                     .entries = { " M a.txt" },
                                     .seen = Seen::New,
                                     .endedWallMs = DispatchWallMs });
    struct Row
    {
        std::string_view name;
        Input input;
    };
    auto const rows = std::array {
        Row { .name = "retry the removal",
              .input = RetryRemovalOf { .run = std::string(PrId), .monoMs = 0 } },
        Row { .name = "end the run", .input = EndRun { .run = std::string(PrId), .monoMs = 0 } },
        Row { .name = "refresh", .input = RefreshNow { .monoMs = 0 } },
        Row { .name = "a wake", .input = Wake { .monoMs = 10 * Minute, .wallMs = DispatchWallMs } },
    };
    for (auto const& row: rows)
    {
        CAPTURE(row.name);
        auto context = contextFor({});
        context.configJson = R"({ "worktree_root": 7 })";
        context.stored = { entryOf(kept) };
        auto harness = Harness { std::move(context) };
        auto const effects = harness.feed(row.input);
        CHECK(commandsIn(effects).empty());
        CHECK(countOf<ReadFile>(effects) == 0);
        CHECK(harness.run(PrId)->state == kept.state);
    }
}

// }}}
// {{{ 20. tampered worktree

TEST_CASE("tampered worktree", "[orchestrator][engine]")
{
    SECTION("an altered .git file")
    {
        auto harness = booted();
        harness.runToRunning(prDispatch());
        harness.list({ paneOf(PrId) });
        harness.list({});
        harness.read("/.git", "gitdir: /tmp/evil/.git/worktrees/x\n"s);
        REQUIRE(finishedOf(harness.run(PrId))->fate == Fate::Kept);
        // Polls re-check it: still altered, nothing runs there (the harness checks every command's cwd).
        for ([[maybe_unused]] auto const poll: std::views::iota(0, 3))
        {
            harness.wake(2 * Minute);
            harness.boot();
            harness.read("/.git", "gitdir: /tmp/evil/.git/worktrees/x\n"s);
        }
        CHECK(harness.commandsIssued("git-head-reachable") == 0);
        // Repaired: the checks run again.
        harness.feed(RetryRemovalOf { .run = std::string(PrId) });
        harness.read("/.git", intactGitFile(ContourPath, PrId));
        CHECK(harness.pending("git-common-dir"));
    }
    SECTION("a redirected common directory")
    {
        // The agent pointed `<main>/.git/worktrees/<id>/commondir` at a directory whose config defines a
        // clean filter and whose info/attributes names it. The .git file is untouched, so it reads intact;
        // git's own answer is the only thing that shows the redirect, and nothing that reads attributes may
        // run after it (the harness checks every command's cwd).
        struct Row
        {
            std::string_view name;
            ProcessOutcome answer;
            std::string_view entry;
        };
        auto const rows = std::array {
            Row {
                .name = "redirected",
                .answer = ok("/tmp/evil.git\n"),
                .entry =
                    "git uses another directory than the main checkout's .git; nothing more was run there" },
            Row { .name = "failed",
                  .answer = failed(128, "fatal: not a git repository"),
                  .entry = "the common-directory check failed: fatal: not a git repository" },
            Row { .name = "truncated",
                  .answer = truncated(commonDirOf(ContourPath)),
                  .entry = "the common-directory check failed: its output was cut at the host's limit" },
        };
        for (auto const& row: rows)
        {
            CAPTURE(row.name);
            auto harness = booted();
            harness.runToRunning(prDispatch());
            harness.list({ paneOf(PrId) });
            harness.list({});
            harness.read("/.git", intactGitFile(ContourPath, PrId));
            harness.finish("git-common-dir", row.answer);
            auto const kept = finishedOf(harness.run(PrId));
            REQUIRE(kept);
            CHECK(kept->fate == Fate::Kept);
            CHECK(kept->entries == std::vector<std::string> { std::string(row.entry) });
            // Polls re-check it: still redirected, nothing more runs there.
            for ([[maybe_unused]] auto const poll: std::views::iota(0, 3))
            {
                harness.wake(2 * Minute);
                harness.boot();
                harness.read("/.git", intactGitFile(ContourPath, PrId));
                harness.finish("git-common-dir", row.answer);
            }
            for (auto const command: { "git-head-reachable"sv,
                                       "git-operation-state"sv,
                                       "git-index-flags"sv,
                                       "git-status"sv,
                                       "git-clean"sv,
                                       "git-worktree-remove"sv })
            {
                CAPTURE(command);
                CHECK(harness.commandsIssued(command) == 0);
            }
            CHECK(finishedOf(harness.run(PrId))->fate == Fate::Kept);
        }
    }
    SECTION("a submodule in the index")
    {
        // The agent committed a nested repository as a gitlink; its own config and info/attributes name a
        // clean filter. `git status --ignore-submodules=none` would run it (measured: harness/r11.sh), so the
        // index check runs first and nothing that reads attributes follows.
        constexpr auto WithSubmodule = "H 100644 78981922613b2afb6025042ff6bd878ac1994e85 0\ta.txt\n"
                                       "H 160000 63343ca2825356cbfa4a67b36f8969e87218a37e 0\tsub\n"sv;
        auto harness = booted();
        harness.runToRunning(prDispatch());
        harness.list({ paneOf(PrId) });
        harness.list({});
        for ([[maybe_unused]] auto const check: std::views::iota(0, 3))
        {
            harness.read("/.git", intactGitFile(ContourPath, PrId));
            harness.finish("git-common-dir", ok(commonDirOf(ContourPath)));
            harness.finish("git-index-flags", ok(WithSubmodule));
            auto const kept = finishedOf(harness.run(PrId));
            REQUIRE(kept);
            CHECK(kept->fate == Fate::Kept);
            CHECK(kept->entries
                  == std::vector<std::string> {
                      "a submodule is in the index: git status would run git in its own "
                      "repository, and git worktree remove refuses it" });
            harness.wake(2 * Minute);
            harness.boot();
        }
        for (auto const command: { "git-head-reachable"sv,
                                   "git-operation-state"sv,
                                   "git-status"sv,
                                   "git-clean"sv,
                                   "git-worktree-remove"sv })
        {
            CAPTURE(command);
            CHECK(harness.commandsIssued(command) == 0);
        }
    }
    SECTION("an unknown main checkout")
    {
        auto context = contextFor({});
        context.stored = { entryOf(storedRun(Ending { .step = EndStep::Verify,
                                                      .cause = Ended::Completed,
                                                      .resumedFrom = std::nullopt,
                                                      .removal = Removal::NotIssued })) };
        auto harness = Harness { std::move(context) };
        harness.read("/.git", intactGitFile(ContourPath, PrId));
        auto const finished = finishedOf(harness.run(PrId));
        REQUIRE(finished);
        CHECK(finished->fate == Fate::Kept);
        CHECK(harness.commandsIssued("git-common-dir") == 0);
        CHECK(harness.commandsIssued("git-head-reachable") == 0);
    }
}

// }}}
// {{{ 21. never reported

TEST_CASE("an agent that never reported is shown as failed and can be dismissed", "[orchestrator][engine]")
{
    auto harness = booted();
    harness.runToRunning(prDispatch());
    harness.list({ paneOf(PrId, "p1", ProgramLiveness::Alive, std::nullopt) });
    harness.endCleanly(PrId);
    auto const finished = finishedOf(harness.run(PrId));
    REQUIRE(finished);
    CHECK(finished->ended == Ended::NeverReported);
    auto const dismissed = harness.feed(DismissRun { .run = std::string(PrId) });
    CHECK(std::ranges::contains(all<StoreRemove>(dismissed), runKey(PrId), &StoreRemove::key));
    CHECK(harness.run(PrId) == nullptr);
}

// }}}
// {{{ 22. and 23. stored state

TEST_CASE("config change is not a removal", "[orchestrator][engine]")
{
    auto stored = storedRun(Running {});
    stored.pane = "p1";
    stored.worktree = std::format("/home/u/old-worktrees/contour/{}", PrId);
    auto context = contextFor({});
    context.stored = { entryOf(stored) };
    auto harness = Harness { std::move(context) };
    harness.boot();
    harness.list({});
    harness.wake(10 * Minute);
    harness.list({});
    CHECK(mentions(harness.engine().problems(), "outside the current configuration"));
    CHECK(holds<Running>(harness.run(PrId)));
    CHECK(harness.commandsIssued("git-status") == 0);
    CHECK(!harness.hasRead("/.git"));
}

TEST_CASE("unreadable store entries are kept and reported", "[orchestrator][engine]")
{
    auto good = storedRun(Running {});
    good.pane = "p1";
    auto oldVersion = toJson(good);
    oldVersion.replace(oldVersion.find(R"("v":2)"), 5, R"("v":1)");
    auto context = contextFor({});
    context.stored = { StoredEntry { .key = "run/garbage", .value = "garbage" },
                       StoredEntry { .key = "run/old", .value = oldVersion },
                       entryOf(good) };
    context.panes = { paneOf(PrId) };
    auto harness = Harness { std::move(context) };
    CHECK(mentions(harness.engine().problems(), "run/garbage"));
    CHECK(mentions(harness.engine().problems(), "run/old"));
    CHECK(countOf<StoreRemove>(harness.last()) == 0);
    CHECK(holds<Running>(harness.run(PrId)));
}

TEST_CASE("after a reboot the board threshold counts from the start", "[orchestrator][engine]")
{
    auto good = storedRun(Running {});
    good.pane = "p1";
    good.lastStatus = AgentStatus::Working;
    auto context = contextFor({});
    context.monoMs = 5;
    context.stored = { entryOf(good) };
    context.panes = { paneOf(PrId) };
    auto harness = Harness { std::move(context) };
    harness.boot();
    CHECK(countOf<PostInbox>(harness.wake(14 * Minute)) == 0);
    CHECK(countOf<PostInbox>(harness.wake(Minute)) == 1);
}

// }}}
// {{{ timing, deadlines and the driver contract

TEST_CASE("a full hourly board budget waits for its oldest post instead of waking at once",
          "[orchestrator][engine]")
{
    auto harness = booted();
    harness.runToRunning(prDispatch());
    harness.list({ paneOf(PrId) });
    auto const opened = harness.run(PrId)->runningSinceMs;
    auto const at = [&](uint64_t minute) {
        return harness.feed(
            Wake { .monoMs = opened + (minute * Minute), .wallMs = DispatchWallMs + (minute * Minute) });
    };
    CHECK(countOf<PostInbox>(at(15)) == 1);
    CHECK(countOf<PostInbox>(at(20)) == 1);
    auto const requested =
        harness.feed(RequestBoard { .run = std::string(PrId), .monoMs = opened + (21 * Minute) });
    CHECK(countOf<PostInbox>(requested) == 1);
    // The repost is due at 26 minutes, but the hour holds three posts already: it waits until 75.
    auto posts = std::size_t { 0 };
    for (auto const minute: std::views::iota(uint64_t { 26 }, uint64_t { 75 }))
    {
        auto const effects = at(minute);
        posts += countOf<PostInbox>(effects);
        CHECK(countOf<ReadFile>(effects) <= 1);
        harness.boot();
        CHECK(harness.wakeAt() >= harness.now() + static_cast<uint64_t>(ReconcileEvery.count()));
    }
    CHECK(posts == 0);
    CHECK(countOf<PostInbox>(at(75)) == 1);
    CHECK(harness.run(PrId)->boardPosts == BoardMaxPosts);
}

TEST_CASE("a tab opened after a listing was asked for is not ended by that listing", "[orchestrator][engine]")
{
    auto harness = booted();
    harness.feed(prDispatch());
    harness.finish("git-fetch", ok());
    harness.finish("git-worktree-add", ok());
    harness.finish("gh-pr-checkout", ok());
    // The driver asks for a listing, then the tab opens, then the listing (made before it) arrives.
    auto const requested = harness.now() + 10;
    harness.feed(PaneOpenedFor { .run = std::string(PrId), .pane = "p1", .monoMs = requested + 5 });
    auto const effects = harness.feed(
        PanesListed { .panes = {}, .requestedMonoMs = requested, .listing = PaneListing::Tagged });
    CHECK(countOf<ReadFile>(effects) == 0);
    CHECK(holds<Running>(harness.run(PrId)));
}

TEST_CASE("a command the host refuses to start is fed back as Refused", "[orchestrator][engine]")
{
    auto refused = ok();
    refused.start = StartResult::Refused;
    refused.exitCode = std::nullopt;
    refused.stderrText = "too many processes";

    auto harness = booted();
    harness.feed(prDispatch());
    harness.finish("git-fetch", refused);
    auto const finished = finishedOf(harness.run(PrId));
    REQUIRE(finished);
    CHECK(finished->ended == Ended::PreparationFailed);
    CHECK(harness.run(PrId)->failure->why == "too many processes");

    auto login = Harness { contextFor({}) };
    login.finish("agent-setup-check", ok("[]"));
    login.finish("git-worktree-list", ok(worktreeList(ContourPath)));
    login.finish("gh-auth", refused);
    CHECK(mentions(login.engine().problems(), "too many processes"));
    CHECK(login.wakeAt() - login.now() == 4 * Minute);
}

TEST_CASE("a reply that never comes is abandoned at its deadline", "[orchestrator][engine]")
{
    SECTION("a poll")
    {
        auto harness = Harness { contextFor({}) };
        harness.finish("agent-setup-check", ok("[]"));
        harness.finish("git-worktree-list", ok(worktreeList(ContourPath)));
        REQUIRE(harness.pending("gh-auth"));
        CHECK(harness.wakeAt()
              <= harness.now() + static_cast<uint64_t>((ShortTimeout + CallDeadlineMargin).count()));
        harness.wake(static_cast<uint64_t>((ShortTimeout + CallDeadlineMargin).count()));
        CHECK(!harness.pending("gh-auth"));
        CHECK(mentions(harness.engine().problems(), "no reply came in time"));
        // A late reply is dropped; polling goes on after the back-off.
        CHECK(harness.feed(ProcessFinished { .tag = "poll/auth", .outcome = ok() }).empty());
        CHECK(harness.wakeAt() - harness.now() == 4 * Minute);
        harness.feed(Wake { .monoMs = harness.wakeAt(), .wallMs = DispatchWallMs });
        CHECK(harness.pending("gh-auth"));
    }
    SECTION("a run's step")
    {
        auto harness = booted();
        harness.feed(prDispatch());
        REQUIRE(harness.pending("git-fetch"));
        harness.wake(static_cast<uint64_t>((LongTimeout + CallDeadlineMargin).count()));
        auto const finished = finishedOf(harness.run(PrId));
        REQUIRE(finished);
        CHECK(finished->ended == Ended::PreparationFailed);
        CHECK(!harness.pending("git-fetch"));
        // Its slot is free again: a late reply is dropped.
        CHECK(commandsIn(
                  harness.feed(ProcessFinished { .tag = std::format("{}/fetch#1", PrId), .outcome = ok() }))
                  .empty());
        CHECK(holds<Finished>(harness.run(PrId)));
    }
}

TEST_CASE("a tab that never opens is given up at its deadline", "[orchestrator][engine]")
{
    /// What the driver answers the OpenPane with.
    enum class Reply : uint8_t
    {
        None,             ///< Nothing: the reply was lost.
        OpenedAtDeadline, ///< PaneOpenedFor, arriving just as the deadline passes.
        OpenedEarly,      ///< PaneOpenedFor, before the deadline.
    };
    struct Row
    {
        std::string_view name;
        Reply reply;
        std::string_view state; ///< "running" or "closing".
    };
    static constexpr auto Rows = std::array {
        Row { .name = "no reply", .reply = Reply::None, .state = "closing" },
        Row { .name = "a reply at the deadline still counts",
              .reply = Reply::OpenedAtDeadline,
              .state = "running" },
        Row { .name = "a reply before it settles the opening",
              .reply = Reply::OpenedEarly,
              .state = "running" },
    };
    auto const deadline = static_cast<uint64_t>((PaneOpenTimeout + CallDeadlineMargin).count());
    for (auto const& row: Rows)
    {
        CAPTURE(row.name);
        auto harness = booted();
        harness.feed(prDispatch());
        harness.finish("git-fetch", ok());
        harness.finish("git-worktree-add", ok());
        harness.finish("gh-pr-checkout", ok());
        REQUIRE(all<OpenPane>(harness.last()).size() == 1);
        REQUIRE(harness.run(PrId)->state == RunState { Preparing { PrepareStep::OpenPane } });
        auto const opened = harness.now() + deadline;
        CHECK(harness.wakeAt() <= opened);
        if (row.reply == Reply::OpenedEarly)
            harness.feed(PaneOpenedFor { .run = std::string(PrId), .pane = "p1", .monoMs = harness.now() });
        if (row.reply == Reply::OpenedAtDeadline)
            harness.feed(PaneOpenedFor { .run = std::string(PrId), .pane = "p1", .monoMs = opened });
        else
            harness.wake(deadline);
        if (row.state == "running")
        {
            CHECK(holds<Running>(harness.run(PrId)));
            continue;
        }
        auto const* const run = harness.run(PrId);
        REQUIRE(run != nullptr);
        CHECK(run->state
              == RunState { Ending { .step = EndStep::ClosePane,
                                     .cause = Ended::PreparationFailed,
                                     .resumedFrom = std::nullopt } });
        REQUIRE(run->failure);
        CHECK(run->failure->step == PrepareStep::OpenPane);
        CHECK(run->failure->why == "no reply came in time");
        CHECK(!countsAsActive(*run));
        // A late reply changes nothing; the tab it names is closed once it is listed.
        harness.feed(PaneOpenedFor { .run = std::string(PrId), .pane = "p1", .monoMs = harness.now() });
        CHECK(holds<Ending>(harness.run(PrId)));
        auto const closes = all<ClosePane>(harness.list({ paneOf(PrId, "p1") }));
        REQUIRE(closes.size() == 1);
        CHECK(closes.front().pane == "p1");
    }
}

TEST_CASE("start resolves only the calls the stored runs awaited", "[orchestrator][engine]")
{
    auto stored = storedRun(Running {});
    stored.pane = "p1";
    stored.lastStatus = AgentStatus::Working;
    auto context = contextFor({});
    context.stored = { entryOf(stored) };
    // The tab is gone: reconciling starts the removal checks, which must not be told they were lost.
    auto harness = Harness { std::move(context) };
    CHECK(countOf<ReadFile>(harness.last()) == 1);
    REQUIRE(holds<Ending>(harness.run(PrId)));
    CHECK(harness.run(PrId)->attempt == stored.attempt + 1);
}

TEST_CASE("an ending run neither blocks its item nor holds a slot", "[orchestrator][engine]")
{
    auto harness = booted({ .limits = R"("max_agents": 1,)" });
    harness.runToRunning(prDispatch());
    harness.list({});
    REQUIRE(holds<Ending>(harness.run(PrId)));
    auto again = prDispatch();
    again.wallMs += 5'000;
    CHECK(countOf<DispatchRefused>(harness.feed(again)) == 0);
}

TEST_CASE("a run of a repository no longer configured can still be ended", "[orchestrator][engine]")
{
    auto settings =
        Settings { .limits = R"("max_agents": 1,)",
                   .repos = R"([ { "name": "other", "path": "~/projects/other", "forge": "github" } ])",
                   .repoPaths = { OtherPath } };
    auto stored = storedRun(Running {});
    stored.pane = "p1";
    stored.lastStatus = AgentStatus::Done;
    stored.state = Done {};
    auto context = contextFor(settings);
    context.stored = { entryOf(stored) };
    context.panes = { paneOf(PrId, "p1", ProgramLiveness::Alive, AgentStatus::Done) };
    auto harness = Harness { std::move(context) };
    harness.boot();
    CHECK(mentions(harness.engine().problems(), "outside the current configuration"));
    // It holds the only slot until it is ended.
    CHECK(all<DispatchRefused>(harness.feed(prDispatch(2375, "other"))).front().reason
          == DispatchRefusal::AgentLimit);

    auto const closes = all<ClosePane>(harness.feed(EndRun { .run = std::string(PrId) }));
    REQUIRE(closes.size() == 1);
    CHECK(closes.front().pane == "p1");
    harness.endCleanly(PrId);
    auto const finished = finishedOf(harness.run(PrId));
    REQUIRE(finished);
    CHECK(finished->ended == Ended::EndedByYou);
    CHECK(finished->fate == Fate::Removed);
    CHECK(!mentions(harness.engine().problems(), "outside the current configuration"));
    auto later = prDispatch(2375, "other");
    later.wallMs += 5'000;
    CHECK(countOf<DispatchRefused>(harness.feed(later)) == 0);
}

// }}}

// {{{ fix round 2

TEST_CASE("a run ended outside the configuration completes after a restart", "[orchestrator][engine]")
{
    auto const settings =
        Settings { .limits = R"("finished_retention": "1h",)",
                   .repos = R"([ { "name": "other", "path": "~/projects/other", "forge": "github" } ])",
                   .repoPaths = { OtherPath } };
    auto stored = storedRun(Done {});
    stored.pane = "p1";
    stored.lastStatus = AgentStatus::Done;
    auto context = contextFor(settings);
    context.stored = { entryOf(stored) };
    context.panes = { paneOf(PrId, "p1", ProgramLiveness::Alive, AgentStatus::Done) };
    auto first = Harness { std::move(context) };
    first.boot();
    first.feed(EndRun { .run = std::string(PrId) });
    REQUIRE(holds<Ending>(first.run(PrId)));

    SECTION("restarted mid-ending")
    {
        auto restart = contextFor(settings);
        restart.stored = first.stored();
        auto second = Harness { std::move(restart) };
        // The tab is gone: the ending goes on to the removal checks.
        CHECK(second.hasRead("/.git"));
        second.boot();
        second.read("/.git", intactGitFile(ContourPath, PrId));
        second.finish("git-common-dir", ok(commonDirOf(ContourPath)));
        second.finish("git-index-flags", ok(CleanIndex));
        second.finish("git-head-reachable", ok("refs/heads/agent/x\n"));
        second.finish("git-operation-state", ok(CleanLongStatus));
        second.finish("git-status", ok());
        second.finish("git-worktree-remove", ok());
        auto const finished = finishedOf(second.run(PrId));
        REQUIRE(finished);
        CHECK(finished->ended == Ended::EndedByYou);
        CHECK(finished->fate == Fate::Removed);
    }
    SECTION("restarted once finished")
    {
        first.endCleanly(PrId);
        REQUIRE(finishedOf(first.run(PrId)));
        auto restart = contextFor(settings);
        restart.stored = first.stored();
        auto dismissed = Harness { restart };
        CHECK(!mentions(dismissed.engine().problems(), "outside the current configuration"));
        CHECK(std::ranges::contains(all<StoreRemove>(dismissed.feed(DismissRun { .run = std::string(PrId) })),
                                    runKey(PrId),
                                    &StoreRemove::key));
        auto purged = Harness { std::move(restart) };
        purged.boot();
        CHECK(std::ranges::contains(
            all<StoreRemove>(purged.wake(61 * Minute)), runKey(PrId), &StoreRemove::key));
        CHECK(purged.run(PrId) == nullptr);
    }
}

TEST_CASE("a reply arriving just after its deadline still counts", "[orchestrator][engine]")
{
    auto harness = booted();
    harness.feed(prDispatch());
    auto const fetch = harness.pending("git-fetch");
    REQUIRE(fetch);
    auto const late = harness.now() + static_cast<uint64_t>((LongTimeout + CallDeadlineMargin).count()) + 1;
    auto const effects = harness.feed(ProcessFinished { .tag = fetch->tag, .outcome = ok(), .monoMs = late });
    CHECK(std::ranges::contains(commandsIn(effects), "git-worktree-add"sv));
    CHECK(holds<Preparing>(harness.run(PrId)));
}

TEST_CASE("pollInterval doubles up to the back-off limit and never shortens", "[orchestrator][engine]")
{
    struct Row
    {
        uint64_t every = 0;
        uint32_t failures = 0;
        uint64_t expected = 0;
    };
    constexpr auto Min = uint64_t { 60'000 };
    static constexpr auto Rows = std::array {
        Row { .every = 2 * Min, .failures = 0, .expected = 2 * Min },
        Row { .every = 2 * Min, .failures = 1, .expected = 4 * Min },
        Row { .every = 2 * Min, .failures = 3, .expected = 16 * Min },
        Row { .every = 2 * Min, .failures = 4, .expected = 30 * Min },
        Row { .every = 2 * Min, .failures = 4'000'000'000U, .expected = 30 * Min },
        Row { .every = 60 * Min, .failures = 2, .expected = 60 * Min },
    };
    for (auto const& row: Rows)
    {
        CAPTURE(row.every, row.failures);
        CHECK(engine::detail::pollInterval(row.every, row.failures) == row.expected);
    }
}

// }}}
