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
#include <orchestrator/core/IssueSources.hpp>
#include <orchestrator/core/Overloaded.hpp>
#include <orchestrator/core/Provider.hpp>
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
    std::string_view limits {};    ///< Extra top-level keys, each followed by a comma.
    std::string_view fixCiKeys {}; ///< Extra keys of the fix_ci task, each followed by a comma.
    std::string_view repos = R"([ { "name": "contour", "path": "~/projects/contour", "forge": "github",
                                     "issues": [ { "source": "forge", "query": "assignee:@me" } ] } ])";
    std::vector<std::string_view> repoPaths = { ContourPath };
};

[[nodiscard]] std::string configJson(Settings const& settings)
{
    return std::format(R"({{ "worktree_root": "~/worktrees", "models": ["opus", "sonnet"], {}
  "repos": {},
  "tasks": [
    {{ {} "id": "fix_ci", "key": "f", "label": "fix CI", "applies_to": "pr", "model": "sonnet",
       "prompt": "/contour-workflows:fix-ci {{url}}" }},
    {{ "id": "issue", "key": "w", "label": "work on issue", "applies_to": "issue", "model": "opus",
       "prompt": "/contour-workflows:work-issue {{url}}" }} ] }})",
                       settings.limits,
                       settings.repos,
                       settings.fixCiKeys);
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
                      .nearTab = std::nullopt,
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
                      .nearTab = std::nullopt,
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
    /// @return The last PublishSummary carried out; std::nullopt before the first.
    [[nodiscard]] std::optional<PublishSummary> const& lastSummary() const { return _lastSummary; }

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
        answerStatsReads();
        return _last;
    }

    /// Leaves statistics reads pending for the test to answer, instead of failing each at once.
    void holdStatsReads() { _answersStats = false; }

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
    [[nodiscard]] std::span<RunCommand const> pendingCommands() const { return _pending; }

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
        auto effects = feed(ProcessFinished { .tag = found->tag, .outcome = std::move(outcome) });
        // The plugins check follows the hooks check; a case about something else has it answered with the
        // recorded list, as if the plugins were installed.
        if (command == "agent-setup-check" && answersPluginsCheck == PluginsCheck::Answered
            && pending("claude-plugins"))
            effects =
                finish("claude-plugins", ok(readFixture(FixtureRoot::Testdata, "claude/plugin-list.json")));
        return effects;
    }

    /// Whether finish() answers the plugins check that a hooks check's answer issues.
    enum class PluginsCheck : uint8_t
    {
        Answered = 0, ///< Answered with the recorded list at once.
        Left,         ///< Left pending, for the case to answer.
    };
    PluginsCheck answersPluginsCheck =
        PluginsCheck::Answered; ///< For finish(): what the hooks check leads to.

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

    /// Answers the oldest unanswered read whose tag ends in @p tagSuffix, as read @p modifiedWallMs.
    std::vector<Effect> readTagged(std::string_view tagSuffix,
                                   std::expected<std::string, FileReadError> content,
                                   uint64_t modifiedWallMs = 0)
    {
        auto const found =
            std::ranges::find_if(_reads, [&](ReadFile const& file) { return file.tag.ends_with(tagSuffix); });
        CAPTURE(tagSuffix);
        REQUIRE(found != _reads.end());
        auto const tag = found->tag;
        _reads.erase(found);
        return feed(FileRead { .tag = tag,
                               .content = std::move(content),
                               .modifiedWallMs = modifiedWallMs,
                               .monoMs = _now,
                               .wallMs = DispatchWallMs + (_now - StartMonoMs) });
    }

    /// @return The unanswered reads whose tag ends in @p tagSuffix.
    [[nodiscard]] std::vector<ReadFile> readsTagged(std::string_view tagSuffix) const
    {
        auto reads = std::vector<ReadFile> {};
        for (auto const& file: _reads)
            if (file.tag.ends_with(tagSuffix))
                reads.push_back(file);
        return reads;
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
    /// Fails every pending statistics read, unless holdStatsReads() was called: they change no run, and a
    /// test about something else should not have to answer them. What they leave behind is carried out but
    /// not returned: the step that issued them is what the test looks at.
    void answerStatsReads()
    {
        while (_answersStats)
        {
            auto const read = std::ranges::find_if(_pending, [](RunCommand const& command) {
                return command.command == "agent-stats" || command.command == "agent-record";
            });
            if (read == _pending.end())
                return;
            auto const tag = read->tag;
            _pending.erase(read);
            accept(_engine->step(ProcessFinished { .tag = tag, .outcome = failed(1, "not in this test") }));
        }
    }

    [[nodiscard]] static ProcessOutcome answerFor(RunCommand const& command)
    {
        if (command.command == "agent-setup-check")
            return ok(readFixture(FixtureRoot::Testdata, "contour/setup-hooks-present.json"));
        if (command.command == "claude-plugins")
            return ok(readFixture(FixtureRoot::Testdata, "claude/plugin-list.json"));
        if (command.command == "git-worktree-list")
            return ok(worktreeList(command.cwd));
        if (command.command == "gh-default-branch")
            return ok(readFixture(FixtureRoot::Testdata, "github/default-branch.json"));
        if (command.command == "gh-prs")
            return ok(readFixture(FixtureRoot::Testdata, "github/prs-open.json"));
        if (command.command == "gh-issues")
            return ok(readFixture(FixtureRoot::Testdata, "github/issues-open.json"));
        if (command.command == "acli-issues")
            return ok(readFixture(FixtureRoot::Testdata, "jira/issues-open.json"));
        if (command.command == "glab-default-branch")
            return ok(readFixture(FixtureRoot::Testdata, "gitlab/default-branch.json"));
        if (command.command == "glab-mrs")
            return ok(readFixture(FixtureRoot::Testdata, "gitlab/mrs-open.json"));
        if (command.command == "glab-issues")
            return ok(readFixture(FixtureRoot::Testdata, "gitlab/issues-open.json"));
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
                        // The folds are the one entry that is no run.
                        if (set.key == FoldsKey)
                            return;
                        REQUIRE(set.key.starts_with("run/"));
                        CHECK(set.value.size() <= MaxStoredRunBytes);
                        CHECK(runFromJson(set.value));
                        _stored[set.key.substr(4)] = set.value;
                    },
                    [&](StoreRemove const& remove) { _stored.erase(remove.key.substr(4)); },
                    [&](PublishSummary const& summary) { _lastSummary = summary; },
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
        // A poll, a transcript read or a statistics read changes no run: no stored state awaits it.
        if (!command.tag.starts_with("poll/") && !command.tag.ends_with("#transcript")
            && !command.tag.ends_with("#stats") && !command.tag.ends_with("#record"))
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
    bool _answersStats = true; ///< Whether statistics reads are failed at once (holdStatsReads()).
    std::optional<PublishSummary> _lastSummary;
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
        .pane = std::string(pane), .tag = std::string(id), .status = status, .message = {}, .program = program
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
    harness.answersPluginsCheck = Harness::PluginsCheck::Left;
    auto const first = commandsIn(harness.last());
    CHECK(first == std::vector { "agent-setup-check"sv, "gh-auth"sv, "git-worktree-list"sv });
    CHECK(harness.pending("gh-auth")->cwd == ContourPath);
    CHECK(harness.pending("agent-setup-check")->cwd == WorktreeRoot);
    CHECK(countOf<WakeAt>(harness.last()) == 1);
    CHECK(countOf<PublishSummary>(harness.last()) == 1);

    harness.finish("agent-setup-check",
                   ok(readFixture(FixtureRoot::Testdata, "contour/setup-hooks-present.json")));
    CHECK(harness.engine().hooks() == HooksState::Present);
    // The plugins are checked once the hooks are, where the hooks check ran.
    CHECK(commandsIn(harness.last()) == std::vector { "claude-plugins"sv });
    CHECK(harness.pending("claude-plugins")->cwd == WorktreeRoot);
    harness.finish("claude-plugins", ok(readFixture(FixtureRoot::Testdata, "claude/plugin-list.json")));
    CHECK(harness.engine().missingPlugins().empty());
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

TEST_CASE("published() is the last summary", "[orchestrator][engine]")
{
    auto const harness = booted();
    REQUIRE(harness.lastSummary());
    CHECK(harness.engine().published().shortText == harness.lastSummary()->shortText);
    CHECK(harness.engine().published().detail == harness.lastSummary()->detail);
    CHECK(harness.engine().published().shortText.contains("✗2"));
}

TEST_CASE("repositories() lists the configured names in order", "[orchestrator][engine]")
{
    auto const settings = Settings {
        .repos = R"([ { "name": "zeta", "path": "~/projects/contour", "forge": "github", "issues": [] },
                      { "name": "alpha", "path": "~/projects/other", "forge": "github", "issues": [] } ])",
        .repoPaths = { ContourPath, OtherPath },
    };
    auto const harness = Harness { contextFor(settings) };
    CHECK(std::ranges::equal(harness.engine().repositories(), std::array { "zeta"sv, "alpha"sv }));
    REQUIRE(harness.engine().config() != nullptr);
    CHECK(harness.engine().config()->maxAgents() == DefaultMaxAgents);

    auto unconfigured = contextFor({});
    unconfigured.configJson = "{}";
    auto const refused = Harness { std::move(unconfigured) };
    CHECK(refused.engine().repositories().empty());
    CHECK(refused.engine().config() == nullptr);
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

TEST_CASE("preview equals what dispatch stores", "[orchestrator][engine]")
{
    for (auto const& request: { prDispatch(2375), issueDispatch(2356) })
    {
        auto harness = booted();
        auto withNote = request;
        withNote.note = "keep the change small";
        auto const preview = harness.engine().preview(withNote);
        REQUIRE(preview.has_value());
        CHECK(harness.engine().runs().empty()); // a preview changes nothing

        harness.feed(withNote);
        auto const* const stored = harness.run(preview->run.id);
        REQUIRE(stored != nullptr);
        CHECK(stored->branch == preview->run.branch);
        CHECK(stored->worktree == preview->run.worktree);
        CHECK(stored->prompt == preview->run.prompt);
        CHECK(stored->session == preview->run.session);

        // The argv the agent's tab is opened with is the one the preview showed.
        harness.finish("git-fetch", ok());
        harness.finish(harness.pending("git-worktree-add") ? "git-worktree-add" : "git-worktree-add-branch",
                       ok());
        if (harness.pending("gh-pr-checkout"))
            harness.finish("gh-pr-checkout", ok());
        auto const opened = all<OpenPane>(harness.last());
        REQUIRE(opened.size() == 1);
        auto const* const pane = findPaneTemplate(opened.front().paneTemplate);
        REQUIRE(pane != nullptr);
        CHECK(substitute(pane->argv, opened.front().arguments) == preview->agentArgv);
        CHECK(preview->agentArgv.back() == stored->prompt);
    }
}

TEST_CASE("preview reports the limits", "[orchestrator][engine]")
{
    auto harness = booted(Settings { .limits = R"("max_agents_per_repo": 4,)" });
    for (auto const number: { 2375, 2369, 2346 })
        harness.feed(prDispatch(static_cast<uint64_t>(number)));
    auto const fourth = harness.engine().preview(prDispatch(2114));
    REQUIRE(fourth.has_value());
    CHECK(fourth->active == 3);
    CHECK(fourth->maxAgents == 4);

    harness.feed(prDispatch(2114));
    CHECK(harness.engine().activeRuns() == 4);
    CHECK(harness.engine().preview(prDispatch(1847)) == std::unexpected { DispatchRefusal::AgentLimit });
    CHECK(harness.engine().preview(prDispatch(2375)) == std::unexpected { DispatchRefusal::AlreadyRunning });
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

TEST_CASE("a new run in the second of a finished one gets an id of its own", "[orchestrator][engine]")
{
    auto harness = booted({});
    harness.runToRunning(prDispatch());
    harness.endCleanly(PrId);
    // The same second: the clock stood still.
    auto const refused = all<DispatchRefused>(harness.feed(prDispatch()));
    CHECK(refused.empty());
    auto const runs = harness.engine().runs();
    CHECK(std::ranges::contains(runs, PrId, &Run::id));
    CHECK(std::ranges::contains(runs, std::format("{}-2", PrId), &Run::id));
}

TEST_CASE("a new run in the second of an orphan's id gets an id of its own", "[orchestrator][engine]")
{
    // A worktree no run accounts for holds the plain id's path or its branch: `git worktree add -b` would
    // refuse either, so the new run takes the next ordinal.
    struct Row
    {
        std::string_view name;
        std::string path;
        std::string branch;
    };
    auto const rows = std::array {
        Row { .name = "its path and branch",
              .path = worktreeOf("contour", PrId),
              .branch = std::format("agent/{}", PrId) },
        Row { .name = "its branch alone",
              .path = worktreeOf("contour", "moved-by-hand"),
              .branch = std::format("agent/{}", PrId) },
        Row { .name = "its path alone", .path = worktreeOf("contour", PrId), .branch = "something-else" },
    };
    for (auto const& row: rows)
    {
        CAPTURE(row.name);
        auto harness = Harness { contextFor({}) };
        harness.finish("git-worktree-list",
                       ok(worktreeList(ContourPath, agentWorktree(row.path, row.branch))));
        harness.boot();
        REQUIRE(harness.engine().orphans("contour").size() == 1);
        // The clock stands still at the second the orphan's id names.
        auto const refused = all<DispatchRefused>(harness.feed(prDispatch()));
        CHECK(refused.empty());
        auto const runs = harness.engine().runs();
        CHECK(!std::ranges::contains(runs, PrId, &Run::id));
        CHECK(std::ranges::contains(runs, std::format("{}-2", PrId), &Run::id));
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

TEST_CASE("end_done_after ends a run that stayed done that long", "[orchestrator][engine]")
{
    auto harness = booted({ .limits = R"("end_done_after": "10m",)" });
    harness.runToRunning(prDispatch());
    harness.list({ paneOf(PrId, "p1", ProgramLiveness::Alive, AgentStatus::Done) });
    REQUIRE(holds<Done>(harness.run(PrId)));

    // Not before: the engine asks to be woken when the wait ends.
    CHECK(countOf<ClosePane>(harness.wake((10 * Minute) - 1'000)) == 0);
    CHECK(holds<Done>(harness.run(PrId)));
    auto const ended = harness.wake(1'000);
    CHECK(countOf<ClosePane>(ended) == 1);
    REQUIRE(holds<Ending>(harness.run(PrId)));

    // It then ends as a run whose tab went after it reported: completed, and its worktree checked as ever.
    harness.endCleanly(PrId);
    auto const finished = finishedOf(harness.run(PrId));
    REQUIRE(finished);
    CHECK(finished->ended == Ended::Completed);
    CHECK(finished->fate == Fate::Removed);
}

TEST_CASE("end_done_after waits again for an agent that went back to work", "[orchestrator][engine]")
{
    auto harness = booted({ .limits = R"("end_done_after": "10m",)" });
    harness.runToRunning(prDispatch());
    harness.list({ paneOf(PrId, "p1", ProgramLiveness::Alive, AgentStatus::Done) });
    harness.wake(5 * Minute);
    // The user answered it: it works again, then reports done once more.
    harness.list({ paneOf(PrId, "p1", ProgramLiveness::Alive, AgentStatus::Working) });
    harness.list({ paneOf(PrId, "p1", ProgramLiveness::Alive, AgentStatus::Done) });
    CHECK(countOf<ClosePane>(harness.wake(9 * Minute)) == 0);
    CHECK(holds<Done>(harness.run(PrId)));
    CHECK(countOf<ClosePane>(harness.wake(1 * Minute)) == 1);
}

TEST_CASE("a task's end_done_after overrides the global one", "[orchestrator][engine]")
{
    // The task's own wait, with no global one.
    auto ownWait = booted({ .fixCiKeys = R"("end_done_after": "5m",)" });
    ownWait.runToRunning(prDispatch());
    ownWait.list({ paneOf(PrId, "p1", ProgramLiveness::Alive, AgentStatus::Done) });
    CHECK(countOf<ClosePane>(ownWait.wake((5 * Minute) - 1'000)) == 0);
    CHECK(countOf<ClosePane>(ownWait.wake(1'000)) == 1);

    // The task's 0 keeps its runs waiting for the user, whatever the global wait.
    auto never =
        booted({ .limits = R"("end_done_after": "10m",)", .fixCiKeys = R"("end_done_after": "0",)" });
    never.runToRunning(prDispatch());
    never.list({ paneOf(PrId, "p1", ProgramLiveness::Alive, AgentStatus::Done) });
    CHECK(countOf<ClosePane>(never.wake(24 * 60 * Minute)) == 0);
    CHECK(holds<Done>(never.run(PrId)));
}

TEST_CASE("without end_done_after a done run waits for the user", "[orchestrator][engine]")
{
    auto harness = booted();
    harness.runToRunning(prDispatch());
    harness.list({ paneOf(PrId, "p1", ProgramLiveness::Alive, AgentStatus::Done) });
    CHECK(countOf<ClosePane>(harness.wake(24 * 60 * Minute)) == 0);
    CHECK(holds<Done>(harness.run(PrId)));
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
        // The start knows no main checkout yet: the run waits for a listing to name it, then checks again
        // from the .git file on.
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
                if (!command.tag.starts_with("poll/") && command.command != "agent-stats"
                    && command.command != "agent-record")
                    found.emplace_back(command.command);
            found.insert(found.end(), countOf<ReadFile>(effects), ".git");
            return found;
        };
        auto const started = runEffects(harness.last());
        auto const expectNext = [&](std::vector<std::string> const& effects) {
            if (row.next.empty())
                CHECK(effects.empty());
            else if (row.next != ".git")
                CHECK(effects == std::vector { std::string(row.next) });
            else
            {
                // The checks start from the .git file, which is compared against the main checkout: they wait
                // for the listing that names it.
                CHECK(effects.empty());
                auto const* const waiting = std::get_if<Ending>(&harness.run(PrId)->state);
                REQUIRE(waiting != nullptr);
                CHECK(waiting->step == EndStep::MainCheckout);
                harness.boot();
                CHECK(harness.hasRead("/.git"));
                CHECK(std::get<Ending>(harness.run(PrId)->state).step == EndStep::Verify);
            }
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
    // The lost status starts the checks again, once a listing names the main checkout.
    CHECK_FALSE(harness.hasRead("/.git"));
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
    auto const keptReason = std::vector { "git worktree remove failed: a tab of the run is still open"s };
    auto const kept = finishedOf(harness.run(PrId));
    REQUIRE(kept);
    CHECK(kept->fate == Fate::Kept);
    CHECK(kept->entries == keptReason);
    // A poll that knows the main checkout checks it again, the tab still listed: every check passes, and the
    // removal is withheld again.
    harness.boot();
    harness.wake(10 * Minute);
    harness.boot();
    harness.read("/.git", intactGitFile(ContourPath, PrId));
    harness.finish("git-common-dir", ok(commonDirOf(ContourPath)));
    harness.finish("git-index-flags", ok(CleanIndex));
    harness.finish("git-head-reachable", ok("refs/heads/agent/x\n"));
    harness.finish("git-operation-state", ok(CleanLongStatus));
    harness.finish("git-status", ok());
    CHECK(harness.commandsIssued("git-status") == 1);
    CHECK(harness.commandsIssued("git-worktree-remove") == 0);
    auto const again = finishedOf(harness.run(PrId));
    REQUIRE(again);
    CHECK(again->fate == Fate::Kept);
    CHECK(again->entries == keptReason);
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

TEST_CASE("a kept run survives a restart with its reason", "[orchestrator][engine]")
{
    // The start knows no main checkout until the first worktree listing answers: a re-check issued before
    // then could only replace the recorded reason with a transient one.
    struct Row
    {
        std::string_view name;
        Ended ended;
        std::string_view entry;
    };
    static constexpr auto Rows = std::array {
        Row { .name = "a modified file", .ended = Ended::Completed, .entry = " M a.txt" },
        Row { .name = "ended by you, an altered .git file",
              .ended = Ended::EndedByYou,
              .entry = ".git file was changed; nothing was run there" },
    };
    for (auto const& row: Rows)
    {
        CAPTURE(row.name);
        auto const kept = storedRun(Finished { .ended = row.ended,
                                               .fate = Fate::Kept,
                                               .entries = { std::string(row.entry) },
                                               .seen = Seen::Opened,
                                               .endedWallMs = DispatchWallMs });
        auto context = contextFor({});
        context.stored = { entryOf(kept) };
        auto harness = Harness { std::move(context) };
        // Whatever the start asked of the worktree is answered before the listing.
        if (harness.hasRead("/.git"))
            harness.read("/.git", intactGitFile(ContourPath, PrId));
        CHECK(harness.run(PrId)->state == kept.state);
        harness.boot();
        CHECK(harness.run(PrId)->state == kept.state);
        // Once the main checkout is known, the next poll checks it again, keeping what it recorded.
        harness.wake(10 * Minute);
        harness.boot();
        harness.read("/.git", intactGitFile(ContourPath, PrId));
        CHECK(harness.pending("git-common-dir"));
        auto const* const ending = std::get_if<Ending>(&harness.run(PrId)->state);
        REQUIRE(ending != nullptr);
        REQUIRE(ending->resumedFrom);
        CHECK(ending->resumedFrom->entries == std::vector { std::string(row.entry) });
    }
}

TEST_CASE("a run whose tab vanished while the extension was down waits for the checkout, then runs the full "
          "checks",
          "[orchestrator][engine]")
{
    struct Row
    {
        std::string_view name;
        std::string_view operation; ///< What `git-operation-state` prints.
        Fate fate;
        std::vector<std::string> entries;
    };
    auto const rows = std::array {
        Row { .name = "a rebase in progress",
              .operation = "rebase in progress; onto <hash>\nYou are currently rebasing branch 'agent/x' on "
                           "'<hash>'.\n",
              .fate = Fate::Kept,
              .entries = { "rebase in progress" } },
        Row { .name = "nothing left", .operation = CleanLongStatus, .fate = Fate::Removed, .entries = {} },
    };
    for (auto const& row: rows)
    {
        CAPTURE(row.name);
        auto stored = storedRun(Running {});
        stored.pane = "p1";
        stored.lastStatus = AgentStatus::Working;
        auto context = contextFor({});
        context.stored = { entryOf(stored) };
        context.paneListing = PaneListing::Tagged;
        auto harness = Harness { std::move(context) };
        // The tab is gone, but nothing is decided about the worktree before a listing names the main
        // checkout.
        CHECK_FALSE(harness.hasRead("/.git"));
        CHECK(harness.run(PrId)->state
              == RunState { Ending { .step = EndStep::MainCheckout,
                                     .cause = Ended::Completed,
                                     .resumedFrom = std::nullopt,
                                     .removal = Removal::NotIssued } });
        harness.boot();
        harness.read("/.git", intactGitFile(ContourPath, PrId));
        harness.finish("git-common-dir", ok(commonDirOf(ContourPath)));
        harness.finish("git-index-flags", ok(CleanIndex));
        harness.finish("git-head-reachable", ok("refs/heads/agent/x\n"));
        harness.finish("git-operation-state", ok(row.operation));
        if (row.fate == Fate::Removed)
        {
            harness.finish("git-status", ok());
            harness.finish("git-worktree-remove", ok());
        }
        auto const finished = finishedOf(harness.run(PrId));
        REQUIRE(finished);
        CHECK(finished->ended == Ended::Completed);
        CHECK(finished->fate == row.fate);
        CHECK(finished->entries == row.entries);
    }
}

TEST_CASE("a run waiting for the main checkout settles only on a definitive answer", "[orchestrator][engine]")
{
    /// How the repository's worktree listing answers, every time it is asked.
    enum class Listing : uint8_t
    {
        NoMainCheckout, ///< It answers, naming no main checkout: a bare repository.
        DirectoryGone,  ///< The repository's directory is gone.
        Fails,          ///< It fails.
    };
    struct Row
    {
        std::string_view name;
        Listing listing;
        uint32_t answers; ///< How many of its answers settle the run.
    };
    static constexpr auto Rows = std::array {
        Row { .name = "no main checkout listed", .listing = Listing::NoMainCheckout, .answers = 1 },
        Row { .name = "the repository's directory is gone", .listing = Listing::DirectoryGone, .answers = 1 },
        Row { .name = "the listing keeps failing",
              .listing = Listing::Fails,
              .answers = MaxWorktreeListFailures },
    };
    for (auto const& row: Rows)
    {
        CAPTURE(row.name);
        auto stored = storedRun(Running {});
        stored.pane = "p1";
        stored.lastStatus = AgentStatus::Working;
        auto context = contextFor({});
        context.stored = { entryOf(stored) };
        context.paneListing = PaneListing::Tagged;
        auto harness = Harness { std::move(context) };
        auto const outcome = [&] {
            switch (row.listing)
            {
                case Listing::NoMainCheckout: return ok(std::format("worktree {}\0bare\0\0"sv, ContourPath));
                case Listing::DirectoryGone: return cwdMissing();
                case Listing::Fails: return failed(128, "fatal: not a git repository");
            }
            return ok();
        }();
        for (auto const answer: std::views::iota(uint32_t { 1 }, row.answers + 1))
        {
            CAPTURE(answer);
            CHECK_FALSE(harness.hasRead("/.git"));
            REQUIRE(holds<Ending>(harness.run(PrId)));
            if (answer > 1)
                harness.wake(31 * Minute);
            harness.finish("git-worktree-list", outcome);
            harness.boot();
        }
        harness.read("/.git", intactGitFile(ContourPath, PrId));
        auto const finished = finishedOf(harness.run(PrId));
        REQUIRE(finished);
        CHECK(finished->fate == Fate::Kept);
        CHECK(finished->entries == std::vector { "the main checkout is not known; nothing was run there"s });
        CHECK(harness.commandsIssued("git-common-dir") == 0);
    }
}

// }}}
// {{{ 14. status boards

TEST_CASE("past the threshold, a running agent's wakes read its status board until it exists",
          "[orchestrator][engine]")
{
    auto harness = booted();
    harness.runToRunning(prDispatch());
    harness.list({ paneOf(PrId) });
    auto const opened = harness.run(PrId)->runningSinceMs;
    // The first read is a wake of its own: the engine asks to be woken at the threshold.
    CHECK(harness.wakeAt() <= opened + (15 * Minute));
    CHECK_FALSE(harness.hasRead("/STATUS.md"));
    harness.wake(14 * Minute);
    CHECK_FALSE(harness.hasRead("/STATUS.md"));

    for ([[maybe_unused]] auto const wake: std::views::iota(0, 5))
    {
        harness.wake(Minute);
        CHECK(harness.hasRead("/STATUS.md"));
        // Only a wake reads: an answer, a listing or a refresh does not.
        harness.read("/STATUS.md", std::unexpected { FileReadError::DirectoryMissing });
        CHECK_FALSE(harness.hasRead("/STATUS.md"));
        harness.list({ paneOf(PrId) });
        CHECK_FALSE(harness.hasRead("/STATUS.md"));
        harness.feed(RefreshNow { .monoMs = harness.now() });
        CHECK_FALSE(harness.hasRead("/STATUS.md"));
        harness.boot();
    }

    // A board that exists stops the reads.
    harness.wake(Minute);
    harness.read("/STATUS.md", "# Status\n"s);
    for ([[maybe_unused]] auto const wake: std::views::iota(0, 5))
    {
        harness.wake(Minute);
        CHECK_FALSE(harness.hasRead("/STATUS.md"));
    }
}

TEST_CASE("no status board is read while blocked or done, nor with a threshold of 0",
          "[orchestrator][engine]")
{
    SECTION("blocked or done")
    {
        for (auto const status: std::array { AgentStatus::Blocked, AgentStatus::Done })
        {
            CAPTURE(status);
            auto harness = booted();
            harness.runToRunning(prDispatch());
            harness.list({ paneOf(PrId, "p1", ProgramLiveness::Alive, status) });
            harness.wake(20 * Minute);
            CHECK_FALSE(harness.hasRead("/STATUS.md"));
        }
    }
    SECTION("disabled")
    {
        auto harness = booted({ .limits = R"("status_board_after": "0",)" });
        harness.runToRunning(prDispatch());
        harness.list({ paneOf(PrId) });
        for ([[maybe_unused]] auto const wake: std::views::iota(0, 40))
        {
            harness.wake(Minute);
            CHECK_FALSE(harness.hasRead("/STATUS.md"));
        }
    }
}

TEST_CASE("ShowBoard reads now and every 5 s while shown", "[orchestrator][engine][board]")
{
    constexpr auto Period = static_cast<uint64_t>(BoardRefreshEvery.count());
    auto harness = booted();
    harness.runToRunning(prDispatch());
    harness.list({ paneOf(PrId) });
    auto const id = std::string { PrId };
    auto const shownAt = harness.now();

    harness.feed(ShowBoard { .run = id, .tab = "tab-1", .monoMs = shownAt });
    auto reads = harness.readsTagged("#board-view");
    REQUIRE(reads.size() == 1);
    CHECK(reads.front().tag == id + "#board-view");
    CHECK(reads.front().path == worktreeOf("contour", PrId) + "/STATUS.md");
    CHECK(harness.wakeAt() == shownAt + Period);
    harness.readTagged("#board-view", "# Status\n"s);

    harness.feed(Wake { .monoMs = shownAt + Period - 1, .wallMs = DispatchWallMs });
    CHECK(harness.readsTagged("#board-view").empty());
    harness.feed(Wake { .monoMs = shownAt + Period, .wallMs = DispatchWallMs });
    CHECK(harness.readsTagged("#board-view").size() == 1);
    CHECK(harness.wakeAt() == shownAt + (2 * Period));
    harness.readTagged("#board-view", "# Status\n"s);

    harness.feed(HideBoard { .tab = "tab-1", .monoMs = harness.now() });
    CHECK(harness.engine().board(id) == nullptr);
    harness.feed(Wake { .monoMs = shownAt + (2 * Period), .wallMs = DispatchWallMs });
    CHECK(harness.readsTagged("#board-view").empty());
}

TEST_CASE("a board read keeps its bytes and time", "[orchestrator][engine][board]")
{
    constexpr auto Modified = uint64_t { 1'700'000'000'000 };
    auto harness = booted();
    harness.runToRunning(prDispatch());
    auto const id = std::string { PrId };
    harness.feed(ShowBoard { .run = id, .tab = "tab-1", .monoMs = harness.now() });
    REQUIRE(harness.engine().board(id) != nullptr);
    CHECK(harness.engine().board(id)->state == BoardState::Unread);

    auto const effects = harness.readTagged("#board-view", "# Status\n\x1b[2Jraw"s, Modified);

    auto const* const board = harness.engine().board(id);
    REQUIRE(board != nullptr);
    CHECK(board->state == BoardState::Read);
    // Kept raw: it is sanitised when drawn.
    CHECK(board->bytes == "# Status\n\x1b[2Jraw");
    CHECK(board->modifiedWallMs == Modified);
    CHECK(board->readMonoMs == harness.now());
    CHECK(board->readWallMs == DispatchWallMs + (harness.now() - StartMonoMs));
    CHECK_FALSE(board->error);
    CHECK(std::ranges::any_of(effects,
                              [](Effect const& effect) { return std::holds_alternative<Redraw>(effect); }));
    CHECK(harness.engine().hasBoard(id));

    // The same board read again changes nothing the dashboard draws.
    harness.wake(BoardRefreshEvery.count());
    CHECK(std::ranges::none_of(harness.readTagged("#board-view", "# Status\n\x1b[2Jraw"s, Modified),
                               [](Effect const& effect) { return std::holds_alternative<Redraw>(effect); }));
}

TEST_CASE("a board read that fails keeps the reason", "[orchestrator][engine][board]")
{
    struct Case
    {
        FileReadError error = FileReadError::Unreadable;
        BoardState state = BoardState::Unavailable;
        std::optional<FileReadError> kept = std::nullopt;
    };
    for (auto const& [error, state, kept]: std::array {
             Case { .error = FileReadError::TooLarge,
                    .state = BoardState::Unavailable,
                    .kept = FileReadError::TooLarge },
             Case { .error = FileReadError::Escapes,
                    .state = BoardState::Unavailable,
                    .kept = FileReadError::Escapes },
             Case { .error = FileReadError::Unreadable,
                    .state = BoardState::Unavailable,
                    .kept = FileReadError::Unreadable },
             Case { .error = FileReadError::NotRegular, .state = BoardState::Absent, .kept = std::nullopt },
             Case {
                 .error = FileReadError::DirectoryMissing, .state = BoardState::Gone, .kept = std::nullopt },
         })
    {
        CAPTURE(error);
        auto harness = booted();
        harness.runToRunning(prDispatch());
        auto const id = std::string { PrId };
        harness.feed(ShowBoard { .run = id, .tab = "tab-1", .monoMs = harness.now() });
        harness.readTagged("#board-view", std::unexpected { error });
        auto const* const board = harness.engine().board(id);
        REQUIRE(board != nullptr);
        CHECK(board->state == state);
        CHECK(board->error == kept);
        CHECK(board->bytes.empty());
        CHECK_FALSE(harness.engine().hasBoard(id));
    }
}

TEST_CASE("the threshold read only marks the row", "[orchestrator][engine][board]")
{
    auto harness = booted();
    harness.runToRunning(prDispatch());
    harness.list({ paneOf(PrId) });
    auto const id = std::string { PrId };
    CHECK_FALSE(harness.engine().hasBoard(id));
    harness.wake(15 * Minute);
    REQUIRE(harness.readsTagged("/board").size() == 1);
    harness.readTagged("/board", "# Status\n"s);
    CHECK(harness.engine().hasBoard(id));
    CHECK(harness.engine().board(id) == nullptr);
    CHECK(harness.readsTagged("#board-view").empty());
}

TEST_CASE("showing a Finished run's board marks it seen", "[orchestrator][engine][board]")
{
    auto context = contextFor({});
    context.stored.push_back(entryOf(storedRun(Finished { .ended = Ended::Completed,
                                                          .fate = Fate::Removed,
                                                          .entries = {},
                                                          .seen = Seen::New,
                                                          .endedWallMs = DispatchWallMs },
                                               PrId)));
    auto harness = Harness { std::move(context) };
    harness.boot();
    REQUIRE(finishedOf(harness.run(PrId))->seen == Seen::New);

    harness.feed(ShowBoard { .run = std::string { PrId }, .tab = "tab-1", .monoMs = harness.now() });

    CHECK(finishedOf(harness.run(PrId))->seen == Seen::Opened);
    // Its worktree is gone with it: the board says so.
    harness.readTagged("#board-view", std::unexpected { FileReadError::DirectoryMissing });
    CHECK(harness.engine().board(PrId)->state == BoardState::Gone);
}

TEST_CASE("a finished run's shown board is not read again", "[orchestrator][engine][board]")
{
    constexpr auto Period = static_cast<uint64_t>(BoardRefreshEvery.count());
    auto context = contextFor({});
    context.stored.push_back(entryOf(storedRun(Finished { .ended = Ended::Completed,
                                                          .fate = Fate::Kept,
                                                          .entries = { "M a.txt" },
                                                          .seen = Seen::New,
                                                          .endedWallMs = DispatchWallMs },
                                               PrId)));
    auto harness = Harness { std::move(context) };
    harness.boot();
    auto const shownAt = harness.now();

    // Shown, it is read once: its agent writes no more.
    harness.feed(ShowBoard { .run = std::string { PrId }, .tab = "tab-1", .monoMs = shownAt });
    REQUIRE(harness.readsTagged("#board-view").size() == 1);
    harness.readTagged("#board-view", "# Status\n"s);
    CHECK(harness.engine().board(PrId)->state == BoardState::Read);
    CHECK(harness.wakeAt() != shownAt + Period);

    harness.feed(Wake { .monoMs = shownAt + (3 * Period), .wallMs = DispatchWallMs });
    CHECK(harness.readsTagged("#board-view").empty());
    CHECK(harness.engine().board(PrId)->state == BoardState::Read);
    // Shown anew, it is read again.
    harness.feed(ShowBoard { .run = std::string { PrId }, .tab = "tab-1", .monoMs = harness.now() });
    CHECK(harness.readsTagged("#board-view").size() == 1);
}

TEST_CASE("a run's worktree is kept until a finished run removed it", "[orchestrator][run]")
{
    auto run = storedRun(Running {}, PrId);
    CHECK(keepsWorktree(run));
    run.state = Finished { .ended = Ended::Completed,
                           .fate = Fate::Kept,
                           .entries = {},
                           .seen = Seen::New,
                           .endedWallMs = DispatchWallMs };
    CHECK(keepsWorktree(run));
    std::get<Finished>(run.state).fate = Fate::Removed;
    CHECK_FALSE(keepsWorktree(run));
}

TEST_CASE("two tabs showing one board read it once per period", "[orchestrator][engine][board]")
{
    constexpr auto Period = static_cast<uint64_t>(BoardRefreshEvery.count());
    auto harness = booted();
    harness.runToRunning(prDispatch());
    auto const id = std::string { PrId };
    auto const shownAt = harness.now();
    harness.feed(ShowBoard { .run = id, .tab = "tab-1", .monoMs = shownAt });
    harness.readTagged("#board-view", "# Status\n"s);
    harness.feed(ShowBoard { .run = id, .tab = "tab-2", .monoMs = shownAt + 1000 });
    CHECK(harness.readsTagged("#board-view").empty());

    harness.feed(Wake { .monoMs = shownAt + Period, .wallMs = DispatchWallMs });
    CHECK(harness.readsTagged("#board-view").size() == 1);
    harness.readTagged("#board-view", "# Status\n"s);

    // Shown again by a tab that shows it, it is read now: the user asked for it.
    harness.feed(ShowBoard { .run = id, .tab = "tab-2", .monoMs = shownAt + Period + 1000 });
    CHECK(harness.readsTagged("#board-view").size() == 1);
}

TEST_CASE("HideBoard for one tab keeps the other's", "[orchestrator][engine][board]")
{
    constexpr auto Period = static_cast<uint64_t>(BoardRefreshEvery.count());
    auto harness = booted();
    harness.runToRunning(prDispatch());
    auto const id = std::string { PrId };
    harness.feed(ShowBoard { .run = id, .tab = "tab-1", .monoMs = harness.now() });
    harness.readTagged("#board-view", "# Status\n"s);
    harness.feed(ShowBoard { .run = id, .tab = "tab-2", .monoMs = harness.now() });

    harness.feed(HideBoard { .tab = "tab-1", .monoMs = harness.now() });
    REQUIRE(harness.engine().board(id) != nullptr);
    harness.wake(Period);
    CHECK(harness.readsTagged("#board-view").size() == 1);
    harness.readTagged("#board-view", "# Status\n"s);

    // Hidden by its last tab, it is read no more.
    harness.feed(HideBoard { .tab = "tab-2", .monoMs = harness.now() });
    CHECK(harness.engine().board(id) == nullptr);
    harness.wake(Period);
    CHECK(harness.readsTagged("#board-view").empty());
}

// }}}
// {{{ 15. problems and back-off

TEST_CASE("a failed login backs off and lists nothing", "[orchestrator][engine]")
{
    auto harness = Harness { contextFor({}) };
    harness.finish("agent-setup-check", ok("[]"));
    harness.finish("git-worktree-list", ok(worktreeList(ContourPath)));
    harness.finish("gh-auth", failed(1, readFixture(FixtureRoot::Testdata, "github/auth-failed.stderr")));
    CHECK(mentions(harness.engine().problems(), "GitHub: the login check failed (run gh auth status)"));
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
    CHECK(!mentions(harness.engine().problems(), "GitHub: the login check failed (run gh auth status)"));
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

TEST_CASE("the hooks problem line says what is lost", "[orchestrator][engine][hooks]")
{
    auto harness = Harness { contextFor({}) };
    harness.finish("agent-setup-check",
                   ok(readFixture(FixtureRoot::Testdata, "contour/setup-claude-absent.json")));
    REQUIRE(harness.engine().hooks() == HooksState::Missing);
    // Runs still end correctly without them: what is lost is the blocked and permission states.
    CHECK(std::ranges::contains(harness.engine().problems(), std::string { HooksMissingProblem }));
    CHECK(mentions(harness.engine().problems(), "blocked and permission states may not show"));
    CHECK_FALSE(mentions(harness.engine().problems(), "every run would end"));
}

TEST_CASE("hooksStateOf reads the setup dry run", "[orchestrator][engine][hooks]")
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
        Row { .name = "no ~/.claude at all",
              .text = readFixture(FixtureRoot::Testdata, "contour/setup-claude-absent.json"),
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

TEST_CASE("orphan worktrees are listed", "[orchestrator][engine]")
{
    auto harness = Harness { contextFor({}) };
    auto const orphanPath = worktreeOf("contour", "pr-1-20260101-000000");
    harness.finish("git-worktree-list",
                   ok(worktreeList(ContourPath, agentWorktree(orphanPath, "agent/pr-1-20260101-000000"))));
    auto const found = harness.engine().orphans("contour");
    REQUIRE(found.size() == 1);
    CHECK(found.front().path == orphanPath);
    CHECK(harness.engine().mainCheckoutOf("contour") == ContourPath);
    // Listed although gh is logged out.
    harness.finish("gh-auth", failed(1, "not logged in"));
    CHECK(harness.engine().orphans("contour").size() == 1);
}

TEST_CASE("a worktree a run just removed is no orphan, and is listed again at once", "[orchestrator][engine]")
{
    // The poll's last listing was asked for while the run ran, so it names the worktree the run then removed:
    // until a listing asked for after the removal arrives, the finished run accounts for it.
    auto harness = booted();
    harness.runToRunning(prDispatch());
    harness.feed(RefreshNow { .monoMs = harness.now() });
    auto const worktree = agentWorktree(worktreeOf("contour", PrId), std::format("agent/{}", PrId));
    harness.finish("git-worktree-list", ok(worktreeList(ContourPath, worktree)));
    REQUIRE(harness.engine().orphans("contour").empty());

    harness.endCleanly(PrId);
    REQUIRE(finishedOf(harness.run(PrId)));
    CHECK(harness.engine().orphans("contour").empty());
    REQUIRE(harness.pending("git-worktree-list"));

    // The fresh listing no longer names it; nothing more is asked.
    harness.finish("git-worktree-list", ok(worktreeList(ContourPath)));
    CHECK(harness.engine().orphans("contour").empty());
    CHECK_FALSE(harness.pending("git-worktree-list"));
}

TEST_CASE("a worktree a later listing still names for a removed run is an orphan", "[orchestrator][engine]")
{
    // A guest fault between storing the removal and the command can leave the worktree behind: a listing
    // asked for after the run finished shows it.
    auto harness = booted();
    harness.runToRunning(prDispatch());
    harness.endCleanly(PrId);
    REQUIRE(harness.pending("git-worktree-list"));
    auto const worktree = agentWorktree(worktreeOf("contour", PrId), std::format("agent/{}", PrId));
    harness.finish("git-worktree-list", ok(worktreeList(ContourPath, worktree)));
    CHECK(harness.engine().orphans("contour").size() == 1);
    CHECK_FALSE(harness.pending("git-worktree-list"));
}

TEST_CASE("orphans are not problem lines", "[orchestrator][engine]")
{
    // The dashboard lists them in a section of their own; a problem line as well would show each twice.
    auto harness = Harness { contextFor({}) };
    auto const problemsBefore =
        std::vector<std::string>(harness.engine().problems().begin(), harness.engine().problems().end());
    auto const listed =
        harness.finish("git-worktree-list",
                       ok(worktreeList(ContourPath,
                                       agentWorktree(worktreeOf("contour", "pr-1-20260101-000000"),
                                                     "agent/pr-1-20260101-000000"))));
    REQUIRE(harness.engine().orphans("contour").size() == 1);
    // Their section is drawn from orphans(): a new one asks for a frame.
    CHECK(countOf<Redraw>(listed) == 1);
    CHECK(!mentions(harness.engine().problems(), "orphan"));
    CHECK(!mentions(harness.engine().problems(), "pr-1-20260101-000000"));
    CHECK(std::ranges::equal(harness.engine().problems(), problemsBefore));
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
        // Nothing is read or run there until a listing names the main checkout: the run waits for it.
        CHECK_FALSE(harness.hasRead("/.git"));
        auto const* const waiting = std::get_if<Ending>(&harness.run(PrId)->state);
        REQUIRE(waiting != nullptr);
        CHECK(waiting->step == EndStep::MainCheckout);
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
    oldVersion.replace(oldVersion.find(R"("v":3)"), 5, R"("v":2)");
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
    harness.wake(14 * Minute);
    CHECK_FALSE(harness.hasRead("/STATUS.md"));
    harness.wake(Minute);
    CHECK(harness.hasRead("/STATUS.md"));
}

// }}}
// {{{ timing, deadlines and the driver contract

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

TEST_CASE("a dispatch's nearTab reaches its OpenPane", "[orchestrator][engine][panes]")
{
    for (auto const& nearTab: { std::optional<std::string> { "tab-7" }, std::optional<std::string> {} })
    {
        CAPTURE(nearTab.value_or("none"));
        auto harness = booted();
        auto dispatch = prDispatch();
        dispatch.nearTab = nearTab;
        harness.feed(dispatch);
        CHECK(harness.run(PrId)->nearTab == nearTab);
        harness.finish("git-fetch", ok());
        harness.finish("git-worktree-add", ok());
        auto const opened = all<OpenPane>(harness.finish("gh-pr-checkout", ok()));
        REQUIRE(opened.size() == 1);
        CHECK(opened.front().nearTab == nearTab);
    }
}

TEST_CASE("pane-open answering limit fails the run through the removal checks",
          "[orchestrator][engine][panes]")
{
    // A live upgrade runs: the host answers limit, and the run is not retried.
    auto harness = booted();
    harness.feed(prDispatch());
    harness.finish("git-fetch", ok());
    harness.finish("git-worktree-add", ok());
    REQUIRE(all<OpenPane>(harness.finish("gh-pr-checkout", ok())).size() == 1);

    harness.feed(PaneOpenFailedFor { .run = std::string(PrId), .why = "limit", .monoMs = harness.now() });

    auto const* const run = harness.run(PrId);
    REQUIRE(run != nullptr);
    REQUIRE(holds<Ending>(run));
    CHECK(std::get<Ending>(run->state).cause == Ended::PreparationFailed);
    REQUIRE(run->failure);
    CHECK(run->failure->step == PrepareStep::OpenPane);
    CHECK(run->failure->why == "limit");
    CHECK(!countsAsActive(*run));
}

TEST_CASE("nothing deletes from a worktree before a listing is known", "[orchestrator][engine][panes]")
{
    auto stored = storedRun(Ending { .step = EndStep::Status,
                                     .cause = Ended::Completed,
                                     .resumedFrom = std::nullopt,
                                     .removal = Removal::NotIssued });
    auto context = contextFor({});
    context.stored = { entryOf(stored) };
    context.finished = { FinishedResult { .tag = *callTag(stored), .outcome = ok() } };
    context.paneListing = PaneListing::Unknown;
    auto harness = Harness { std::move(context) };
    CHECK(harness.commandsIssued("git-worktree-remove") == 0);
    auto const kept = finishedOf(harness.run(PrId));
    REQUIRE(kept);
    CHECK(kept->fate == Fate::Kept);
    CHECK(kept->entries == std::vector { "git worktree remove failed: the open tabs are not known yet"s });
}

TEST_CASE("start with an unknown listing skips reconciliation", "[orchestrator][engine][panes]")
{
    auto const contextWith = [](PaneListing listing) {
        auto stored = storedRun(Running {});
        stored.pane = "p1";
        auto context = contextFor({});
        context.stored = { entryOf(stored) };
        context.paneListing = listing;
        return context;
    };
    SECTION("at start")
    {
        auto harness = Harness { contextWith(PaneListing::Unknown) };
        CHECK(holds<Running>(harness.run(PrId)));
        CHECK(countOf<ClosePane>(harness.last()) == 0);
    }
    SECTION("a later listing of nothing known")
    {
        auto harness = Harness { contextWith(PaneListing::Unknown) };
        REQUIRE(holds<Running>(harness.run(PrId)));
        // A PanesListed left defaulted, or fed Unknown, reconciles nothing either.
        harness.feed(PanesListed { .panes = {}, .requestedMonoMs = harness.now() });
        CHECK(holds<Running>(harness.run(PrId)));
        harness.list({}, PaneListing::Unknown);
        CHECK(holds<Running>(harness.run(PrId)));
        // The first known listing does.
        harness.list({});
        CHECK(holds<Ending>(harness.run(PrId)));
    }
}

TEST_CASE("start resolves only the calls the stored runs awaited", "[orchestrator][engine]")
{
    auto stored = storedRun(Running {});
    stored.pane = "p1";
    stored.lastStatus = AgentStatus::Working;
    auto context = contextFor({});
    context.stored = { entryOf(stored) };
    // The tab is gone: reconciling starts the removal checks, which must not be told they were lost. They
    // wait for the listing that names the main checkout, then read the .git file once.
    auto harness = Harness { std::move(context) };
    CHECK(countOf<ReadFile>(harness.last()) == 0);
    REQUIRE(holds<Ending>(harness.run(PrId)));
    CHECK(harness.run(PrId)->attempt == stored.attempt);
    harness.boot();
    CHECK(harness.hasRead("/.git"));
    REQUIRE(holds<Ending>(harness.run(PrId)));
    CHECK(std::get<Ending>(harness.run(PrId)->state).step == EndStep::Verify);
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

TEST_CASE("the engine knows a run's main checkout once a listing names it", "[orchestrator][engine]")
{
    /// Where the stored run's repository stands.
    enum class Repo : uint8_t
    {
        Configured,   ///< Configured, at the run's directory.
        Moved,        ///< Configured, at another directory.
        Unconfigured, ///< No longer configured: the run's own directory stands for its main checkout.
    };
    /// Whether the worktree listings answered.
    enum class Listed : uint8_t
    {
        No,
        Yes,
    };
    struct Row
    {
        std::string_view name;
        Repo repo;
        Listed listed;
        std::optional<CheckoutUnknown> unknown; ///< std::nullopt: known.
    };
    static constexpr auto Rows = std::array {
        Row { .name = "configured, not listed yet",
              .repo = Repo::Configured,
              .listed = Listed::No,
              .unknown = CheckoutUnknown::Pending },
        Row { .name = "configured, listed",
              .repo = Repo::Configured,
              .listed = Listed::Yes,
              .unknown = std::nullopt },
        Row { .name = "moved, listed",
              .repo = Repo::Moved,
              .listed = Listed::Yes,
              .unknown = CheckoutUnknown::Unavailable },
        Row { .name = "no longer configured",
              .repo = Repo::Unconfigured,
              .listed = Listed::No,
              .unknown = std::nullopt },
    };
    for (auto const& row: Rows)
    {
        CAPTURE(row.name);
        auto stored = storedRun(Done {});
        if (row.repo == Repo::Moved)
            stored.repoPath = std::string(OtherPath);
        if (row.repo == Repo::Unconfigured)
            stored.repo = "gone";
        auto context = contextFor({});
        context.stored = { entryOf(stored) };
        auto harness = Harness { std::move(context) };
        if (row.listed == Listed::Yes)
            harness.boot();
        CHECK(harness.engine().unknownCheckout(stored) == row.unknown);
    }
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

TEST_CASE("a run outside the configuration is never tried again, and may be dismissed",
          "[orchestrator][engine]")
{
    auto const settings =
        Settings { .repos = R"([ { "name": "other", "path": "~/projects/other", "forge": "github" } ])",
                   .repoPaths = { OtherPath } };
    auto const finishedAs = [](Ended ended, Fate fate, std::string_view id) {
        return storedRun(Finished { .ended = ended,
                                    .fate = fate,
                                    .entries = { " M src/a.cpp" },
                                    .seen = Seen::New,
                                    .endedWallMs = 0 },
                         id);
    };
    // Ended by the user, so the engine works on it: only the scope keeps x from running its checks.
    constexpr auto KeptId = "pr-2375-20261113-150848"sv;
    constexpr auto RemovedId = "pr-2375-20261113-150849"sv;
    auto const kept = finishedAs(Ended::EndedByYou, Fate::Kept, KeptId);
    auto const removed = finishedAs(Ended::Completed, Fate::Removed, RemovedId);
    auto context = contextFor(settings);
    context.stored = { entryOf(kept), entryOf(removed) };
    auto harness = Harness { std::move(context) };
    harness.boot();
    CHECK(harness.engine().scopeOf(kept) == RunScope::Outside);
    CHECK(harness.engine().scopeOf(removed) == RunScope::Outside);

    auto const retried = harness.feed(RetryRemovalOf { .run = std::string(KeptId) });
    CHECK(countOf<RunCommand>(retried) == 0);
    CHECK(countOf<ReadFile>(retried) == 0);
    CHECK(holds<Finished>(harness.run(KeptId)));
    // Nor by the poll that checks the kept worktrees inside the configuration again.
    auto const polled = harness.wake(2 * Minute);
    CHECK(std::ranges::none_of(all<RunCommand>(polled),
                               [&](RunCommand const& command) { return command.tag.starts_with(KeptId); }));
    CHECK(countOf<ReadFile>(polled) == 0);
    harness.boot();
    CHECK(holds<Finished>(harness.run(KeptId)));
    CHECK(!harness.hasRead("/.git"));
    for (auto const command: { "git-common-dir", "git-index-flags", "git-status", "git-worktree-remove" })
        CHECK(harness.commandsIssued(command) == 0);

    // Left alone by the engine, and still dismissed: a dismissal runs nothing.
    CHECK(std::ranges::contains(all<StoreRemove>(harness.feed(DismissRun { .run = std::string(RemovedId) })),
                                runKey(RemovedId),
                                &StoreRemove::key));
    CHECK(harness.run(RemovedId) == nullptr);
}

TEST_CASE("a refused configuration places no run outside it", "[orchestrator][engine]")
{
    auto context = contextFor({});
    context.configJson = R"({ "worktree_root": 7 })";
    auto const stored = storedRun(Running {});
    context.stored = { entryOf(stored) };
    auto harness = Harness { std::move(context) };
    REQUIRE(harness.engine().config() == nullptr);
    CHECK(harness.engine().scopeOf(stored) == RunScope::Unknown);
    // With nothing configured, nothing runs: x asks for no check.
    auto const retried = harness.feed(RetryRemovalOf { .run = std::string(PrId) });
    CHECK(countOf<RunCommand>(retried) == 0);
    CHECK(countOf<ReadFile>(retried) == 0);
}

TEST_CASE("a run dispatched under the configuration is inside it", "[orchestrator][engine]")
{
    auto harness = Harness { contextFor({}) };
    harness.boot();
    auto const stored = storedRun(Running {});
    CHECK(harness.engine().scopeOf(stored) == RunScope::Inside);
    auto moved = stored;
    moved.worktree += "-moved";
    CHECK(harness.engine().scopeOf(moved) == RunScope::Outside);
    moved = stored;
    moved.repoPath = std::string(OtherPath);
    CHECK(harness.engine().scopeOf(moved) == RunScope::Outside);
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

namespace
{

/// What each command of a poll answers.
[[nodiscard]] ProcessOutcome pollAnswerOf(RunCommand const& command)
{
    if (command.command == "agent-setup-check")
        return ok(readFixture(FixtureRoot::Testdata, "contour/setup-hooks-present.json"));
    if (command.command == "claude-plugins")
        return ok(readFixture(FixtureRoot::Testdata, "claude/plugin-list.json"));
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

/// Answers every command @p effects ask for, and those the answers ask for, at @p monoMs.
/// @return Every effect the answers asked for.
[[nodiscard]] std::vector<Effect> answerAll(Engine& engine, std::vector<Effect> effects, uint64_t monoMs)
{
    auto asked = std::vector<Effect> {};
    for ([[maybe_unused]] auto const _: std::views::iota(0, 32))
    {
        auto const commands = all<RunCommand>(effects);
        if (commands.empty())
            return asked;
        auto next = std::vector<Effect> {};
        for (auto const& command: commands)
        {
            auto const stepped = engine.step(
                ProcessFinished { .tag = command.tag, .outcome = pollAnswerOf(command), .monoMs = monoMs });
            next.insert(next.end(), stepped.begin(), stepped.end());
        }
        asked.insert(asked.end(), next.begin(), next.end());
        effects = std::move(next);
    }
    FAIL("the poll kept running commands");
    return asked;
}

/// @return The tag of every RunCommand of @p effects, in order.
[[nodiscard]] std::vector<std::string> runTagsOf(std::span<Effect const> effects)
{
    auto tags = std::vector<std::string> {};
    for (auto const& command: all<RunCommand>(effects))
        tags.push_back(command.tag);
    return tags;
}

} // namespace

TEST_CASE("a restart takes over the poll a previous instance left running", "[orchestrator][engine]")
{
    auto const prs = readFixture(FixtureRoot::Testdata, "github/prs-open.json");

    SECTION("a list the host still runs is awaited, not run again")
    {
        auto context = contextFor(Settings {});
        context.inFlight = { "poll/contour/prs"s };
        auto started = Engine::start(std::move(context));
        auto const tags = runTagsOf(started.effects);
        CHECK_FALSE(std::ranges::contains(tags, "poll/contour/prs"sv));
        CHECK_FALSE(std::ranges::contains(tags, "poll/auth"sv));
        CHECK(std::ranges::contains(tags, "poll/contour/issues/0"sv));

        auto const effects = started.engine.step(
            ProcessFinished { .tag = "poll/contour/prs", .outcome = ok(prs), .monoMs = StartMonoMs });
        CHECK(started.engine.changeRequests("contour").size() == 5);
        CHECK_FALSE(std::ranges::contains(runTagsOf(effects), "poll/contour/prs"sv));
    }

    SECTION("a list that finished is taken in")
    {
        auto context = contextFor(Settings {});
        context.finished = { FinishedResult { .tag = "poll/contour/prs", .outcome = ok(prs) } };
        auto const started = Engine::start(std::move(context));
        auto const tags = runTagsOf(started.effects);
        CHECK(started.engine.changeRequests("contour").size() == 5);
        CHECK_FALSE(std::ranges::contains(tags, "poll/contour/prs"sv));
        CHECK_FALSE(std::ranges::contains(tags, "poll/auth"sv));
        // The lists the previous instance had not run are run now.
        CHECK(std::ranges::contains(tags, "poll/contour/issues/0"sv));
        CHECK(std::ranges::contains(tags, "poll/contour/default-branch"sv));
        CHECK(std::ranges::contains(tags, "poll/contour/worktrees"sv));
    }

    SECTION("the login check the host still runs is awaited")
    {
        auto context = contextFor(Settings {});
        context.inFlight = { "poll/auth"s };
        auto started = Engine::start(std::move(context));
        CHECK_FALSE(std::ranges::contains(runTagsOf(started.effects), "poll/auth"sv));

        auto const effects = started.engine.step(
            ProcessFinished { .tag = "poll/auth", .outcome = ok(), .monoMs = StartMonoMs });
        CHECK(std::ranges::contains(runTagsOf(effects), "poll/contour/prs"sv));
    }

    SECTION("a list of a repository no longer configured starts a poll of its own")
    {
        auto context = contextFor(Settings {});
        context.inFlight = { "poll/gone/prs"s };
        context.finished = { FinishedResult { .tag = "poll/gone/issues/0", .outcome = ok() } };
        auto const started = Engine::start(std::move(context));
        CHECK(std::ranges::contains(runTagsOf(started.effects), "poll/auth"sv));
    }
}

TEST_CASE("a resumed poll ends, and the next one is scheduled after it", "[orchestrator][engine]")
{
    auto const pollEvery = static_cast<uint64_t>(DefaultPollEvery.count());

    SECTION("every command of it answers: the next poll is one interval after the last answer")
    {
        auto context = contextFor(Settings {});
        context.inFlight = { "poll/contour/prs"s };
        context.finished = { FinishedResult {
            .tag = "poll/contour/issues/0",
            .outcome = ok(readFixture(FixtureRoot::Testdata, "github/issues-open.json")) } };
        auto started = Engine::start(std::move(context));
        auto& engine = started.engine;
        // Well inside every command's deadline.
        auto const answeredMs = StartMonoMs + 1'000;
        auto asked = answerAll(engine, started.effects, answeredMs);
        auto const last = engine.step(
            ProcessFinished { .tag = "poll/contour/prs",
                              .outcome = ok(readFixture(FixtureRoot::Testdata, "github/prs-open.json")),
                              .monoMs = answeredMs });
        asked.insert(asked.end(), last.begin(), last.end());

        // The wake the engine asks for last is the next poll's.
        auto const wakes = all<WakeAt>(asked);
        REQUIRE_FALSE(wakes.empty());
        CHECK(wakes.back().monoMs == answeredMs + pollEvery);
        CHECK(engine.problems().empty());
        auto const early =
            engine.step(Wake { .monoMs = answeredMs + pollEvery - 1, .wallMs = DispatchWallMs });
        CHECK(all<RunCommand>(early).empty());
        auto const due = engine.step(Wake { .monoMs = answeredMs + pollEvery, .wallMs = DispatchWallMs });
        CHECK(std::ranges::contains(runTagsOf(due), "poll/contour/prs"sv));
    }

    SECTION("a list that never answers is abandoned, and the failed poll's successor is backed off")
    {
        auto context = contextFor(Settings {});
        context.inFlight = { "poll/contour/prs"s };
        auto started = Engine::start(std::move(context));
        auto& engine = started.engine;
        static_cast<void>(answerAll(engine, started.effects, StartMonoMs));

        auto const overdueMs =
            StartMonoMs + static_cast<uint64_t>((LongTimeout + CallDeadlineMargin).count()) + 1;
        auto const abandoned = engine.step(Wake { .monoMs = overdueMs, .wallMs = DispatchWallMs });
        CHECK(std::ranges::any_of(engine.problems(), [](std::string const& problem) {
            return problem.contains("the pull request list failed");
        }));
        // One failure doubles the interval.
        auto const wakes = all<WakeAt>(abandoned);
        REQUIRE(wakes.size() == 1);
        CHECK(wakes.front().monoMs == overdueMs + (2 * pollEvery));
        auto const early =
            engine.step(Wake { .monoMs = overdueMs + (2 * pollEvery) - 1, .wallMs = DispatchWallMs });
        CHECK(all<RunCommand>(early).empty());
        // The login is checked again first.
        auto const due =
            engine.step(Wake { .monoMs = overdueMs + (2 * pollEvery), .wallMs = DispatchWallMs });
        CHECK(std::ranges::contains(runTagsOf(due), "poll/auth"sv));
    }
}

TEST_CASE("a restart runs no list whose result it takes in", "[orchestrator][engine]")
{

    SECTION("the login check and a list finished: the check's answer issues the other lists only")
    {
        auto context = contextFor(Settings {});
        context.finished = {
            FinishedResult { .tag = "poll/auth", .outcome = ok() },
            FinishedResult { .tag = "poll/contour/prs",
                             .outcome = ok(readFixture(FixtureRoot::Testdata, "github/prs-open.json")) },
        };
        auto started = Engine::start(std::move(context));
        // Every command of the start, and every one queued behind them, until the poll ends.
        auto effects = started.effects;
        auto const asked = answerAll(started.engine, started.effects, StartMonoMs + 1'000);
        effects.insert(effects.end(), asked.begin(), asked.end());
        auto const tags = runTagsOf(effects);
        CHECK_FALSE(std::ranges::contains(tags, "poll/contour/prs"sv));
        CHECK_FALSE(std::ranges::contains(tags, "poll/auth"sv));
        for (auto const& tag: tags)
        {
            CAPTURE(tag);
            CHECK(std::ranges::count(tags, tag) == 1);
        }
        CHECK(started.engine.changeRequests("contour").size() == 5);
    }

    SECTION("a worktree list finished with no forge command: the poll starts without it")
    {
        auto context = contextFor(Settings {});
        context.finished = { FinishedResult { .tag = "poll/contour/worktrees",
                                              .outcome = ok(worktreeList(ContourPath)) } };
        auto const started = Engine::start(std::move(context));
        auto const tags = runTagsOf(started.effects);
        CHECK(std::ranges::contains(tags, "poll/auth"sv));
        CHECK_FALSE(std::ranges::contains(tags, "poll/contour/worktrees"sv));
    }
}

// {{{ transcripts

namespace
{

[[nodiscard]] std::string finishedId(int index)
{
    return std::format("pr-{}-20261113-150847", 2375 + index);
}

/// A booted engine that knows @p count Finished runs, none of them seen yet.
[[nodiscard]] Harness withFinishedRuns(int count)
{
    auto context = contextFor({});
    for (auto const index: std::views::iota(0, count))
        context.stored.push_back(entryOf(storedRun(Finished { .ended = Ended::Completed,
                                                              .fate = Fate::Removed,
                                                              .entries = {},
                                                              .seen = Seen::New,
                                                              .endedWallMs = DispatchWallMs },
                                                   finishedId(index))));
    auto harness = Harness { std::move(context) };
    harness.boot();
    return harness;
}

[[nodiscard]] std::string_view argumentOf(RunCommand const& command, std::string_view name)
{
    auto const found = std::ranges::find(command.arguments, name, &Argument::name);
    return found != command.arguments.end() ? std::string_view { found->value } : std::string_view {};
}

[[nodiscard]] std::string basicPage()
{
    return readFixture(FixtureRoot::Contract, "agent-transcript/format-1/basic-page-1.json");
}

} // namespace

TEST_CASE("ReadTranscript runs the command in the worktree root", "[orchestrator][engine][transcript]")
{
    auto harness = withFinishedRuns(1);
    auto const id = finishedId(0);

    harness.feed(ReadTranscript { .run = id, .tab = {}, .page = 2, .monoMs = harness.now() });

    auto const command = harness.pending("agent-transcript");
    REQUIRE(command);
    CHECK(command->tag == id + "#transcript");
    CHECK(command->cwd == WorktreeRoot);
    CHECK(argumentOf(*command, "session") == Session);
    CHECK(argumentOf(*command, "path") == worktreeOf("contour", id));
    CHECK(argumentOf(*command, "page") == "2");
    CHECK(argumentOf(*command, "format") == "1");
    auto const* view = harness.engine().transcript("", id);
    REQUIRE(view != nullptr);
    CHECK(view->state == FetchState::Loading);
    CHECK(view->page == 2);

    harness.finish("agent-transcript", ok(basicPage()));
    view = harness.engine().transcript("", id);
    REQUIRE(view != nullptr);
    CHECK(view->state == FetchState::Loaded);
    REQUIRE(view->content);
    CHECK(view->content->entries.size() == 8);
    CHECK(view->failure.empty());

    // An unknown run, or page 0, asks for nothing.
    harness.feed(
        ReadTranscript { .run = "pr-1-20261113-150847", .tab = {}, .page = 1, .monoMs = harness.now() });
    harness.feed(ReadTranscript { .run = id, .tab = {}, .page = 0, .monoMs = harness.now() });
    CHECK_FALSE(harness.pending("agent-transcript"));
}

TEST_CASE("a transcript page is kept per run, four runs at most", "[orchestrator][engine][transcript]")
{
    auto harness = withFinishedRuns(5);
    for (auto const index: std::views::iota(0, 5))
    {
        harness.feed(
            ReadTranscript { .run = finishedId(index), .tab = {}, .page = 1, .monoMs = harness.now() });
        harness.finish("agent-transcript", ok(basicPage()));
    }

    CHECK(harness.engine().transcript("", finishedId(0)) == nullptr);
    for (auto const index: std::views::iota(1, 5))
    {
        CAPTURE(index);
        auto const* const view = harness.engine().transcript("", finishedId(index));
        REQUIRE(view != nullptr);
        CHECK(view->state == FetchState::Loaded);
    }

    // Asking again keeps a run's page while the next loads, and makes it the newest: the oldest goes next.
    harness.feed(ReadTranscript { .run = finishedId(1), .tab = {}, .page = 1, .monoMs = harness.now() });
    auto const* const reloading = harness.engine().transcript("", finishedId(1));
    REQUIRE(reloading != nullptr);
    CHECK(reloading->state == FetchState::Loading);
    CHECK(reloading->content.has_value());
    harness.finish("agent-transcript", ok(basicPage()));
    harness.feed(ReadTranscript { .run = finishedId(0), .tab = {}, .page = 1, .monoMs = harness.now() });
    CHECK(harness.engine().transcript("", finishedId(1)) != nullptr);
    CHECK(harness.engine().transcript("", finishedId(2)) == nullptr);
}

TEST_CASE("a page asked for while another loads is read next", "[orchestrator][engine][transcript]")
{
    auto harness = withFinishedRuns(1);
    auto const id = finishedId(0);
    harness.feed(ReadTranscript { .run = id, .tab = {}, .page = 1, .monoMs = harness.now() });
    harness.feed(ReadTranscript { .run = id, .tab = {}, .page = 2, .monoMs = harness.now() });
    CHECK(harness.pendingCount() == 1);

    harness.finish("agent-transcript", ok(basicPage()));

    auto const next = harness.pending("agent-transcript");
    REQUIRE(next);
    CHECK(argumentOf(*next, "page") == "2");
    CHECK(harness.engine().transcript("", id)->state == FetchState::Loading);
}

TEST_CASE("two tabs page one run, each on its own page", "[orchestrator][engine][transcript]")
{
    auto harness = withFinishedRuns(1);
    auto const id = finishedId(0);
    harness.feed(ReadTranscript { .run = id, .tab = "tab-1", .page = 1, .monoMs = harness.now() });
    harness.feed(ReadTranscript { .run = id, .tab = "tab-2", .page = 2, .monoMs = harness.now() });
    // One page of a run is read at a time.
    CHECK(harness.pendingCount() == 1);

    harness.finish("agent-transcript", ok(basicPage()));
    auto const* const first = harness.engine().transcript("tab-1", id);
    REQUIRE(first != nullptr);
    CHECK(first->state == FetchState::Loaded);
    CHECK(first->page == 1);
    // The second tab's page is asked for next, and lands in that tab alone.
    auto const next = harness.pending("agent-transcript");
    REQUIRE(next);
    CHECK(argumentOf(*next, "page") == "2");
    harness.finish("agent-transcript", ok(basicPage()));
    auto const* const second = harness.engine().transcript("tab-2", id);
    REQUIRE(second != nullptr);
    CHECK(second->state == FetchState::Loaded);
    CHECK(second->page == 2);
    CHECK(harness.engine().transcript("tab-1", id)->page == 1);
    CHECK(harness.pendingCount() == 0);
}

TEST_CASE("a transcript a tab shows is kept while another tab reads five more",
          "[orchestrator][engine][transcript]")
{
    auto harness = withFinishedRuns(6);
    harness.feed(ReadTranscript { .run = finishedId(0), .tab = "tab-1", .page = 1, .monoMs = harness.now() });
    harness.finish("agent-transcript", ok(basicPage()));
    // Another window's tab reads five runs, one after another.
    for (auto const index: std::views::iota(1, 6))
    {
        harness.feed(
            ReadTranscript { .run = finishedId(index), .tab = "tab-2", .page = 1, .monoMs = harness.now() });
        harness.finish("agent-transcript", ok(basicPage()));
    }

    auto const* const shown = harness.engine().transcript("tab-1", finishedId(0));
    REQUIRE(shown != nullptr);
    CHECK(shown->state == FetchState::Loaded);
    // The other tab's oldest went, its last stays.
    CHECK(harness.engine().transcript("tab-2", finishedId(1)) == nullptr);
    CHECK(harness.engine().transcript("tab-2", finishedId(5)) != nullptr);
}

TEST_CASE("a closed tab frees the transcripts kept for it", "[orchestrator][engine][transcript]")
{
    auto harness = withFinishedRuns(5);
    for (auto const index: std::views::iota(0, 3))
    {
        harness.feed(
            ReadTranscript { .run = finishedId(index), .tab = "tab-2", .page = 1, .monoMs = harness.now() });
        harness.finish("agent-transcript", ok(basicPage()));
    }
    harness.feed(ReadTranscript { .run = finishedId(3), .tab = "tab-1", .page = 1, .monoMs = harness.now() });
    harness.finish("agent-transcript", ok(basicPage()));

    harness.feed(TabClosed { .tab = "tab-2", .monoMs = harness.now() });

    for (auto const index: std::views::iota(0, 3))
        CHECK(harness.engine().transcript("tab-2", finishedId(index)) == nullptr);
    // Its slots are free: a tab reading another run keeps tab-1's and every one it asked for.
    harness.feed(ReadTranscript { .run = finishedId(4), .tab = "tab-3", .page = 1, .monoMs = harness.now() });
    harness.finish("agent-transcript", ok(basicPage()));
    CHECK(harness.engine().transcript("tab-1", finishedId(3)) != nullptr);
    CHECK(harness.engine().transcript("tab-3", finishedId(4)) != nullptr);
}

TEST_CASE("a page whose tab closed lands in another tab that shows the run",
          "[orchestrator][engine][transcript]")
{
    auto harness = withFinishedRuns(1);
    auto const id = finishedId(0);
    harness.feed(ReadTranscript { .run = id, .tab = "tab-1", .page = 1, .monoMs = harness.now() });
    harness.feed(ReadTranscript { .run = id, .tab = "tab-2", .page = 1, .monoMs = harness.now() });
    harness.feed(TabClosed { .tab = "tab-1", .monoMs = harness.now() });
    CHECK(harness.pendingCount() == 1);

    harness.finish("agent-transcript", ok(basicPage()));

    // The page read for the closed tab is the one tab-2 waits for: it is not read twice.
    auto const* const view = harness.engine().transcript("tab-2", id);
    REQUIRE(view != nullptr);
    CHECK(view->state == FetchState::Loaded);
    CHECK(harness.pendingCount() == 0);
    CHECK(harness.engine().transcript("tab-1", id) == nullptr);
}

TEST_CASE("a failed transcript keeps its sanitised reason", "[orchestrator][engine][transcript]")
{
    auto harness = withFinishedRuns(1);
    auto const id = finishedId(0);

    harness.feed(ReadTranscript { .run = id, .tab = {}, .page = 1, .monoMs = harness.now() });
    harness.finish("agent-transcript", failed(3, "no session log for x \x1b[31min /wt\nsecond line\n"));

    auto const* view = harness.engine().transcript("", id);
    REQUIRE(view != nullptr);
    CHECK(view->state == FetchState::Failed);
    CHECK(view->failure.starts_with("no session log for x"));
    CHECK_FALSE(view->failure.contains('\x1b'));
    CHECK_FALSE(view->failure.contains("second line"));

    // A verb that exits 0 without a page fails too.
    harness.feed(ReadTranscript { .run = id, .tab = {}, .page = 1, .monoMs = harness.now() });
    harness.finish("agent-transcript", ok("not json"));
    view = harness.engine().transcript("", id);
    REQUIRE(view != nullptr);
    CHECK(view->state == FetchState::Failed);
    CHECK_FALSE(view->failure.empty());
}

TEST_CASE("a format mismatch is its own state", "[orchestrator][engine][transcript]")
{
    auto harness = withFinishedRuns(1);
    auto const id = finishedId(0);

    harness.feed(ReadTranscript { .run = id, .tab = {}, .page = 1, .monoMs = harness.now() });
    harness.finish("agent-transcript", failed(2, "unsupported transcript format 1; this contour prints 2\n"));
    CHECK(harness.engine().transcript("", id)->state == FetchState::FormatMismatch);

    harness.feed(ReadTranscript { .run = id, .tab = {}, .page = 1, .monoMs = harness.now() });
    harness.finish("agent-transcript", ok(R"({"format":2,"pages":"many"})"));
    CHECK(harness.engine().transcript("", id)->state == FetchState::FormatMismatch);
}

TEST_CASE("reading a Finished run's transcript marks it seen", "[orchestrator][engine][transcript]")
{
    auto harness = withFinishedRuns(1);
    auto const id = finishedId(0);
    REQUIRE(finishedOf(harness.run(id))->seen == Seen::New);

    harness.feed(ReadTranscript { .run = id, .tab = {}, .page = 1, .monoMs = harness.now() });

    CHECK(finishedOf(harness.run(id))->seen == Seen::Opened);
}

// }}}

// {{{ GitLab, as a recording would enable it

namespace
{

/// The shipped provider rows, each marked Recorded, as the recording step leaves GitLab's.
[[nodiscard]] std::span<Provider const> recordedProviders()
{
    static auto const rows = [] {
        auto recorded = std::vector<Provider>(providers().begin(), providers().end());
        for (auto& row: recorded)
            row.verification = Verification::Recorded;
        return recorded;
    }();
    return rows;
}

/// One GitLab repository, accepted because its row is passed as Recorded.
[[nodiscard]] StartContext gitlabContext()
{
    auto context = contextFor(
        Settings { .repos = R"([ { "name": "contour", "path": "~/projects/contour", "forge": "gitlab",
                        "issues": [ { "source": "forge", "query": "crash" } ] } ])" });
    context.sources.providers = recordedProviders();
    return context;
}

} // namespace

TEST_CASE("the engine polls glab-mrs and checks out with glab-mr-checkout", "[orchestrator][engine][gitlab]")
{
    auto harness = Harness { gitlabContext() };
    CHECK(harness.pending("glab-auth"));
    CHECK(!harness.pending("gh-auth"));
    harness.boot();
    CHECK(harness.commandsIssued("glab-default-branch") == 1);
    CHECK(harness.commandsIssued("glab-mrs") == 1);
    CHECK(harness.commandsIssued("glab-issues") == 1);
    CHECK(harness.commandsIssued("gh-prs") == 0);
    CHECK(harness.engine().problems().empty());
    CHECK(harness.engine().changeRequests("contour").size() == 3);

    constexpr auto Id = "pr-41-20261113-150847"sv;
    auto request = prDispatch(41);
    request.url = "https://gitlab.example.com/group/project/-/merge_requests/41";
    harness.feed(request);
    harness.finish("git-fetch", ok());
    auto const checkout = all<RunCommand>(harness.finish("git-worktree-add", ok()));
    REQUIRE(checkout.size() == 1);
    CHECK(checkout.front().command == "glab-mr-checkout");
    CHECK(checkout.front().cwd == worktreeOf("contour", Id));
    CHECK(std::ranges::contains(
        checkout.front().arguments, "41"sv, [](Argument const& a) -> std::string_view { return a.value; }));
    auto const opened = all<OpenPane>(harness.finish("glab-mr-checkout", ok()));
    CHECK(opened.size() == 1);
}

TEST_CASE("an auth check's output is never kept", "[orchestrator][engine][gitlab]")
{
    constexpr auto Secret = "glpat-0123456789abcdefghij"sv;
    auto harness = Harness { gitlabContext() };
    harness.finish("agent-setup-check", ok("[]"));
    harness.finish("git-worktree-list", ok(worktreeList(ContourPath)));
    auto const effects = harness.finish(
        "glab-auth",
        failed(1, std::format("x gitlab.example.com: api call failed: token {} rejected", Secret)));
    CHECK(harness.commandsIssued("glab-mrs") == 0);
    CHECK(std::ranges::contains(harness.engine().problems(),
                                "GitLab: the login check failed (run glab auth status)"sv));
    CHECK(!mentions(harness.engine().problems(), Secret));
    CHECK(!mentions(harness.engine().problems(), "api call failed"));
    REQUIRE(harness.lastSummary());
    CHECK(!harness.lastSummary()->shortText.contains(Secret));
    CHECK(!harness.lastSummary()->detail.contains(Secret));
    for (auto const& entry: harness.stored())
        CHECK(!entry.value.contains(Secret));
}

// }}}

// {{{ Jira, as a recording would enable it

namespace
{

/// The shipped issue sources, each marked Recorded, as the recording step leaves Jira's.
[[nodiscard]] std::span<IssueSourceRow const> recordedIssueSources()
{
    static auto const rows = [] {
        auto recorded = std::vector<IssueSourceRow>(issueSources().begin(), issueSources().end());
        for (auto& row: recorded)
            row.verification = Verification::Recorded;
        return recorded;
    }();
    return rows;
}

/// A GitHub repository whose issues come from its forge and from Jira.
[[nodiscard]] StartContext jiraContext()
{
    auto context = contextFor(
        Settings { .repos = R"j([ { "name": "contour", "path": "~/projects/contour", "forge": "github",
                        "issues": [ { "source": "forge", "query": "assignee:@me" },
                                    { "source": "jira", "jql": "assignee = currentUser()",
                                      "site": "https://example.atlassian.net/" } ] } ])j" });
    context.sources.issueSources = recordedIssueSources();
    return context;
}

} // namespace

TEST_CASE("a jira query runs acli in the repository", "[orchestrator][engine][jira]")
{
    auto harness = Harness { jiraContext() };
    harness.finish("agent-setup-check", ok("[]"));
    harness.finish("git-worktree-list", ok(worktreeList(ContourPath)));
    harness.finish("gh-auth", ok());
    harness.finish("acli-auth", ok());
    auto const acli = harness.pending("acli-issues");
    REQUIRE(acli);
    CHECK(acli->cwd == ContourPath);
    REQUIRE(acli->arguments.size() == 1);
    CHECK(acli->arguments.front().name == "jql");
    CHECK(acli->arguments.front().value == "assignee = currentUser()");
    CHECK(harness.pending("gh-issues"));
    harness.boot();
    CHECK(!mentions(harness.engine().problems(), "issue list"));
    auto const jiraItems =
        std::ranges::count_if(harness.engine().unclaimedIssues("contour"),
                              [](Issue const& issue) { return std::holds_alternative<JiraRef>(issue.item); });
    CHECK(jiraItems == 2);
    auto const found =
        std::ranges::find_if(harness.engine().unclaimedIssues("contour"), [](Issue const& issue) {
            return issue.item == ItemRef { JiraRef { .key = "ABC-123" } };
        });
    REQUIRE(found != harness.engine().unclaimedIssues("contour").end());
    CHECK(found->url == "https://example.atlassian.net/browse/ABC-123");
}

TEST_CASE("each issue source's login check runs once in the poll", "[orchestrator][engine][jira]")
{
    // Two repositories list from Jira: its login is checked once, in the first.
    auto context = contextFor(
        Settings { .repos = R"j([ { "name": "contour", "path": "~/projects/contour", "forge": "github",
                        "issues": [ { "source": "jira", "jql": "a", "site": "https://example.atlassian.net" } ] },
                      { "name": "other", "path": "~/projects/other", "forge": "github",
                        "issues": [ { "source": "jira", "jql": "b", "site": "https://example.atlassian.net" } ] } ])j",
                   .repoPaths = { ContourPath, OtherPath } });
    context.sources.issueSources = recordedIssueSources();
    auto harness = Harness { std::move(context) };
    auto const check = harness.pending("acli-auth");
    REQUIRE(check);
    CHECK(check->cwd == ContourPath);
    CHECK(harness.commandsIssued("acli-auth") == 1);
    CHECK(harness.pending("gh-auth"));

    SECTION("a failed check says how to see why, and never what it printed")
    {
        constexpr auto Secret = "ATATT3xFfGF0-secret"sv;
        harness.finish("acli-auth", failed(1, std::format("token {} expired", Secret)));
        CHECK(std::ranges::contains(harness.engine().problems(),
                                    "Jira: the login check failed (run acli jira auth status)"sv));
        CHECK(!mentions(harness.engine().problems(), Secret));
    }
    SECTION("a passed check leaves nothing")
    {
        harness.boot();
        CHECK(!mentions(harness.engine().problems(), "login check"));
        CHECK(harness.commandsIssued("acli-auth") == 1);
    }
}

TEST_CASE("an unmeasured issue source's login check never runs", "[orchestrator][engine][jira]")
{
    // The shipped Jira row is Unmeasured: the configuration naming it is refused, and nothing of it runs.
    auto harness = Harness {
        contextFor(
            Settings { .repos = R"j([ { "name": "contour", "path": "~/projects/contour", "forge": "github",
                        "issues": [ { "source": "jira", "jql": "a", "site": "https://example.atlassian.net" } ] } ])j" })
    };
    harness.boot();
    CHECK(harness.commandsIssued("acli-auth") == 0);
    CHECK(harness.commandsIssued("acli-issues") == 0);
}

TEST_CASE("a missing plugin is a problem, and installing it runs add, install and the check again",
          "[orchestrator][engine][plugins]")
{
    auto harness = Harness { contextFor({}) };
    harness.answersPluginsCheck = Harness::PluginsCheck::Left;
    harness.finish("agent-setup-check",
                   ok(readFixture(FixtureRoot::Testdata, "contour/setup-hooks-present.json")));
    harness.finish("claude-plugins",
                   ok(readFixture(FixtureRoot::Testdata, "claude/plugin-list-without.json")));
    REQUIRE(harness.engine().missingPlugins().size() == 1);
    CHECK(harness.engine().missingPlugins().front().id == "contour-workflows@contour-terminal");
    CHECK(mentions(harness.engine().problems(), "lacks the plugin contour-workflows@contour-terminal"));

    auto const added = all<RunCommand>(harness.feed(InstallPlugins {}));
    REQUIRE(added.size() == 1);
    CHECK(added.front().command == "claude-marketplace-add");
    CHECK(added.front().cwd == WorktreeRoot);
    REQUIRE(added.front().arguments.size() == 1);
    CHECK(added.front().arguments.front().name == "source");
    CHECK(added.front().arguments.front().value == "contour-terminal/claude-marketplace");
    CHECK(harness.engine().installingPlugins());
    // A second confirmation while it runs starts nothing more.
    CHECK(all<RunCommand>(harness.feed(InstallPlugins {})).empty());

    SECTION("installed: the check finds it, and the problem goes")
    {
        // A marketplace Claude Code knows already fails to be added again; the install still runs.
        auto const installed =
            all<RunCommand>(harness.finish("claude-marketplace-add", failed(1, "already added")));
        REQUIRE(installed.size() == 1);
        CHECK(installed.front().command == "claude-plugin-install");
        REQUIRE(installed.front().arguments.size() == 1);
        CHECK(installed.front().arguments.front().name == "plugin");
        CHECK(installed.front().arguments.front().value == "contour-workflows@contour-terminal");
        auto const checked = all<RunCommand>(harness.finish("claude-plugin-install", ok()));
        REQUIRE(checked.size() == 1);
        CHECK(checked.front().command == "claude-plugins");
        CHECK_FALSE(harness.engine().installingPlugins());
        harness.finish("claude-plugins", ok(readFixture(FixtureRoot::Testdata, "claude/plugin-list.json")));
        CHECK(harness.engine().missingPlugins().empty());
        CHECK_FALSE(mentions(harness.engine().problems(), "lacks the plugin"));
    }
    SECTION("a failed install says how to run it by hand")
    {
        harness.finish("claude-marketplace-add", ok());
        harness.finish("claude-plugin-install", failed(1, "network unreachable"));
        harness.finish("claude-plugins",
                       ok(readFixture(FixtureRoot::Testdata, "claude/plugin-list-without.json")));
        CHECK(mentions(harness.engine().problems(),
                       "run by hand: claude plugin install contour-workflows@contour-terminal"));
        CHECK(mentions(harness.engine().problems(), "lacks the plugin contour-workflows@contour-terminal"));
    }
}

TEST_CASE("plugins: [] checks for none", "[orchestrator][engine][plugins]")
{
    auto harness = Harness { contextFor({ .limits = R"("plugins": [],)" }) };
    harness.answersPluginsCheck = Harness::PluginsCheck::Left;
    harness.finish("agent-setup-check",
                   ok(readFixture(FixtureRoot::Testdata, "contour/setup-hooks-present.json")));
    CHECK_FALSE(harness.pending("claude-plugins"));
    CHECK(harness.engine().missingPlugins().empty());
}

TEST_CASE("every agent's system prompt asks for its board, unless status_board is off",
          "[orchestrator][engine][board]")
{
    auto const argumentOf = [](OpenPane const& pane, std::string_view name) {
        auto const at = std::ranges::find(pane.arguments, name, &Argument::name);
        return at != pane.arguments.end() ? std::optional { at->value } : std::nullopt;
    };
    auto const openedWith = [](Settings const& settings) {
        auto harness = Harness { contextFor(settings) };
        harness.boot();
        harness.feed(prDispatch());
        harness.finish("git-fetch", ok());
        harness.finish("git-worktree-add", ok());
        auto const opened = all<OpenPane>(harness.finish("gh-pr-checkout", ok()));
        REQUIRE(opened.size() == 1);
        return opened.front();
    };

    SECTION("by default, the default instruction in the agent template")
    {
        auto const pane = openedWith({});
        CHECK(pane.paneTemplate == "agent");
        CHECK(argumentOf(pane, "board") == std::string { DefaultBoardInstruction });
    }
    SECTION("its own words")
    {
        auto const pane = openedWith({ .limits = R"("status_board": "Keep NOTES.md current.",)" });
        CHECK(pane.paneTemplate == "agent");
        CHECK(argumentOf(pane, "board") == "Keep NOTES.md current.");
    }
    SECTION("off: the plain template, and no board argument")
    {
        auto const pane = openedWith({ .limits = R"("status_board": "off",)" });
        CHECK(pane.paneTemplate == "agent-plain");
        CHECK_FALSE(argumentOf(pane, "board").has_value());
    }
}

TEST_CASE("a topic dispatches on a new branch named from its text, with the text as its prompt",
          "[orchestrator][engine][topic]")
{
    auto harness = Harness { contextFor({ .repos = R"([ { "name": "contour", "path": "~/projects/contour",
                                                         "forge": "github", "base": "main" } ])" }) };
    constexpr auto Id = "topic-investigate-the-flaky-test-20261113-150847"sv;
    auto request = issueDispatch();
    request.item = TopicRef {};
    request.url.clear();
    request.topic = "Investigate the flaky test";
    request.task = "topic";
    CHECK(countOf<DispatchRefused>(harness.feed(request)) == 0);
    auto const runs = harness.engine().runs();
    auto const found = std::ranges::find(runs, Id, &Run::id);
    REQUIRE(found != runs.end());
    auto const* const run = &*found;
    CHECK(run->item == ItemRef { TopicRef { .slug = "investigate-the-flaky-test" } });
    CHECK(run->topic == "Investigate the flaky test");
    CHECK(run->url.empty());
    CHECK(run->prompt == "Investigate the flaky test");
    CHECK(run->branch == std::format("agent/{}", Id));

    auto const added = all<RunCommand>(harness.finish("git-fetch", ok()));
    REQUIRE(added.size() == 1);
    CHECK(added.front().command == "git-worktree-add-branch");
    CHECK(std::ranges::contains(added.front().arguments, "origin/main"s, &Argument::value));

    SECTION("the same topic again, while the first runs, is refused")
    {
        auto again = request;
        again.session = "11111111-2222-4333-8444-555555555555";
        auto const refused = all<DispatchRefused>(harness.feed(again));
        REQUIRE(refused.size() == 1);
        CHECK(refused.front().reason == DispatchRefusal::AlreadyRunning);
    }
    SECTION("an empty topic is refused")
    {
        auto empty = request;
        empty.topic = "  ";
        auto const refused = all<DispatchRefused>(harness.feed(empty));
        REQUIRE(refused.size() == 1);
        CHECK(refused.front().reason == DispatchRefusal::BadPrompt);
    }
}

TEST_CASE("a jira item dispatches on a new branch named from its key", "[orchestrator][engine][jira]")
{
    auto harness = Harness { jiraContext() };
    harness.boot();
    constexpr auto Id = "abc-123-20261113-150847"sv;
    auto request = issueDispatch();
    request.item = JiraRef { .key = "ABC-123" };
    request.url = "https://example.atlassian.net/browse/ABC-123";
    harness.feed(request);
    auto const added = all<RunCommand>(harness.finish("git-fetch", ok()));
    REQUIRE(added.size() == 1);
    CHECK(added.front().command == "git-worktree-add-branch");
    CHECK(added.front().cwd == ContourPath);
    auto const argument = [&](std::string_view name) {
        auto const at = std::ranges::find(added.front().arguments, name, &Argument::name);
        return at != added.front().arguments.end() ? at->value : std::string {};
    };
    CHECK(argument("branch") == std::format("agent/{}", Id));
    CHECK(argument("path") == worktreeOf("contour", Id));
    CHECK(argument("base") == "origin/master");
    auto const opened = all<OpenPane>(harness.finish("git-worktree-add-branch", ok()));
    CHECK(opened.size() == 1);
}

// }}}

// {{{ statistics

namespace
{

[[nodiscard]] std::string statsReport(std::string_view name = "basic.json")
{
    return readFixture(FixtureRoot::Contract, std::format("agent-stats/format-1/{}", name));
}

} // namespace

TEST_CASE("a live agent's statistics are read a minute after it started, then every minute",
          "[orchestrator][engine][stats]")
{
    auto harness = booted();
    harness.holdStatsReads();
    harness.runToRunning(prDispatch());
    CHECK_FALSE(harness.pending("agent-stats"));

    harness.wake(Minute);
    auto const read = harness.pending("agent-stats");
    REQUIRE(read);
    CHECK(argumentOf(*read, "session") == harness.run(PrId)->session);
    CHECK(argumentOf(*read, "path") == harness.run(PrId)->worktree);
    CHECK(argumentOf(*read, "format") == "1");
    auto const answered = harness.finish("agent-stats", ok(statsReport("running.json")));
    REQUIRE(harness.run(PrId)->stats.has_value());
    CHECK(harness.run(PrId)->stats->apiCalls == 4);
    // Stored with the run, so that it outlives the log.
    REQUIRE(countOf<StoreSet>(answered) == 1);
    CHECK(all<StoreSet>(answered).front().value.contains(R"("apiCalls":4)"));

    // The same report again stores nothing; the next is a minute later.
    CHECK_FALSE(harness.pending("agent-stats"));
    harness.wake(Minute);
    CHECK(countOf<StoreSet>(harness.finish("agent-stats", ok(statsReport("running.json")))) == 0);
}

TEST_CASE("a finished run's statistics are read once more, with the cost", "[orchestrator][engine][stats]")
{
    auto harness = booted();
    harness.holdStatsReads();
    harness.runToRunning(prDispatch());
    harness.list({ paneOf(PrId) });
    harness.endCleanly(PrId);
    REQUIRE(finishedOf(harness.run(PrId)));
    REQUIRE(harness.pending("agent-stats"));
    harness.finish("agent-stats", ok(statsReport()));
    REQUIRE(harness.run(PrId)->stats.has_value());
    CHECK(harness.run(PrId)->stats->cost.has_value());
    harness.wake(10 * Minute);
    CHECK_FALSE(harness.pending("agent-stats"));
}

TEST_CASE("a report that cannot be read leaves the last one standing", "[orchestrator][engine][stats]")
{
    auto harness = booted();
    harness.holdStatsReads();
    harness.runToRunning(prDispatch());
    harness.wake(Minute);
    harness.finish("agent-stats", ok(statsReport("running.json")));
    harness.wake(Minute);
    CHECK(countOf<StoreSet>(harness.finish("agent-stats", failed(2, "no session log for x"))) == 0);
    harness.wake(Minute);
    CHECK(countOf<StoreSet>(harness.finish("agent-stats", ok(R"({"format":2})"))) == 0);
    REQUIRE(harness.run(PrId)->stats.has_value());
    CHECK(harness.run(PrId)->stats->apiCalls == 4);
}

TEST_CASE("a finished run goes to the file of finished runs once, with its last statistics",
          "[orchestrator][engine][stats]")
{
    auto harness = booted();
    harness.holdStatsReads();
    harness.runToRunning(prDispatch());
    harness.list({ paneOf(PrId) });
    harness.endCleanly(PrId);
    // The statistics first: the record carries them.
    CHECK_FALSE(harness.pending("agent-record"));
    harness.finish("agent-stats", ok(statsReport()));
    auto const record = harness.pending("agent-record");
    REQUIRE(record);
    auto const line = argumentOf(*record, "record");
    CHECK(line.starts_with(R"({"format":1,"run":")"));
    CHECK(line.contains(std::format(R"("run":"{}")", PrId)));
    CHECK(line.contains(R"("usd":0.123457)"));
    CHECK(argumentOf(*record, "format") == "1");
    auto const written = harness.finish("agent-record", ok());
    CHECK(harness.run(PrId)->ledger == Ledger::Written);
    REQUIRE(countOf<StoreSet>(written) == 1);
    CHECK(all<StoreSet>(written).front().value.contains(R"("ledger":"written")"));
    harness.wake(10 * Minute);
    CHECK_FALSE(harness.pending("agent-record"));
}

TEST_CASE("a record contour refused is not asked for again by the same instance",
          "[orchestrator][engine][stats]")
{
    auto harness = booted();
    harness.holdStatsReads();
    harness.runToRunning(prDispatch());
    harness.list({ paneOf(PrId) });
    harness.endCleanly(PrId);
    harness.finish("agent-stats", failed(1, "no session log"));
    CHECK(countOf<StoreSet>(harness.finish("agent-record", failed(1, "cannot be written"))) == 0);
    CHECK(harness.run(PrId)->ledger == Ledger::Pending);
    harness.wake(10 * Minute);
    CHECK_FALSE(harness.pending("agent-record"));
}

TEST_CASE("statistics are read one at a time", "[orchestrator][engine][stats]")
{
    auto harness = booted();
    harness.holdStatsReads();
    harness.runToRunning(prDispatch());
    harness.runToRunning(prDispatch(2369), "p2");
    harness.wake(Minute);
    auto const reads = [&] {
        return std::ranges::count(harness.pendingCommands(), "agent-stats"sv, &RunCommand::command);
    };
    CHECK(reads() == 1);
    harness.finish("agent-stats", ok(statsReport("running.json")));
    CHECK(reads() == 1);
    harness.finish("agent-stats", ok(statsReport("running.json")));
    CHECK(reads() == 0);
}

// }}}

// {{{ folds

TEST_CASE("a group the user folds is stored, and unfolded again", "[orchestrator][engine][folds]")
{
    auto harness = booted();
    CHECK_FALSE(harness.engine().isFolded("issues/contour"));
    auto const folded = harness.feed(ToggleFold { .group = "issues/contour", .monoMs = harness.now() });
    CHECK(harness.engine().isFolded("issues/contour"));
    REQUIRE(countOf<StoreSet>(folded) == 1);
    CHECK(all<StoreSet>(folded).front().key == FoldsKey);
    CHECK(all<StoreSet>(folded).front().value == R"(["issues/contour"])");
    CHECK(countOf<Redraw>(folded) == 1);

    auto const unfolded = harness.feed(ToggleFold { .group = "issues/contour", .monoMs = harness.now() });
    CHECK_FALSE(harness.engine().isFolded("issues/contour"));
    REQUIRE(countOf<StoreSet>(unfolded) == 1);
    CHECK(all<StoreSet>(unfolded).front().value == "[]");
}

TEST_CASE("the folds a previous instance stored are folded again", "[orchestrator][engine][folds]")
{
    auto context = contextFor({});
    context.stored = { StoredEntry { .key = std::string { FoldsKey },
                                     .value = R"(["prs/contour","bad key\n",""])" } };
    auto harness = Harness { std::move(context) };
    CHECK(harness.engine().isFolded("prs/contour"));
    CHECK_FALSE(harness.engine().isFolded("bad key\n"));
    // The store's other keys are not runs it cannot read: nothing is reported.
    CHECK(harness.engine().problems().empty());
}

// }}}
