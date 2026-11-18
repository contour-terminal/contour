// SPDX-License-Identifier: Apache-2.0
//
// The orchestrator's cleanup commands against real git: what they print in the states the removal decision
// tells apart, what they leave alone, and what an agent who controlled the worktree cannot make them run.
// The commands, their environment and their roots come from the orchestrator's manifest, run through
// DeclaredProcesses on the native runner and directory opener exactly as the daemon runs them; git sees
// none of the user's configuration (GitSandbox).
#include <core/Environment.hpp>
#include <core/platform/NativeFileSystem.hpp>

#include <catch2/catch_message.hpp>
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <charconv>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <format>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <ranges>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <vtextension/CommandPolicy.hpp>
#include <vtextension/DaemonWorld.hpp>
#include <vtextension/DeclaredProcesses.hpp>
#include <vtextension/Manifest.hpp>
#include <vtextension/ProcessService.hpp>
#include <vtextension/RootPointer.hpp>
#include <vtextension/testing/FakeGrantStore.hpp>
#include <vtextension/testing/GitSandbox.hpp>
#include <vtextension/testing/OrchestratorManifest.hpp>

using namespace std::string_view_literals;
using namespace vtextension;
using vtextension::testing::FakeGrantStore;
using vtextension::testing::GitEnvironment;
using vtextension::testing::GitSandbox;
using vtextension::testing::GitSandboxError;
using vtextension::testing::orchestratorManifest;

namespace
{

/// What the host calls the installation under test, and the digest it is granted for.
constexpr auto HostId = "orchestrator@sandbox"sv;
constexpr auto GrantDigest = "sandbox"sv;

/// git's empty tree: the attribute source the cleanup commands read `.gitattributes` from.
constexpr auto EmptyTree = "4b825dc642cb6eb9a060e54bf8d69288fbee4904"sv;

/// The commands run in a worktree an agent controlled, before anything of it is removed.
constexpr auto CleanupCommands = std::array {
    "git-common-dir"sv,      "git-index-flags"sv, "git-status"sv,          "git-head-reachable"sv,
    "git-operation-state"sv, "git-clean"sv,       "git-worktree-remove"sv,
};

/// Commands that run in a repository or a worktree the orchestrator is setting up, not tearing down.
constexpr auto SetupCommands = std::array {
    "git-worktree-add"sv,
    "git-worktree-add-branch"sv,
    "gh-pr-checkout"sv,
    "git-fetch"sv,
};

// Copied from `StateLines` in extensions/orchestrator/core/OperationState.cpp, which vtextension cannot
// include. A git that changes this wording fails here first; change both together.
constexpr auto StateLines = std::array {
    "interactive rebase in progress; onto "sv,
    "rebase in progress; onto "sv,
    "You are currently rebasing"sv,
    "You are currently editing a commit while rebasing"sv,
    "You are currently splitting a commit while rebasing"sv,
    "You have unmerged paths."sv,
    "All conflicts fixed but you are still merging."sv,
    "You are currently cherry-picking commit "sv,
    "Cherry-pick currently in progress."sv,
    "You are currently reverting commit "sv,
    "Revert currently in progress."sv,
    "You are currently bisecting"sv,
    "You are in the middle of an am session."sv,
};

// Copied from `FirstLines` in extensions/orchestrator/core/OperationState.cpp, under the same rule.
constexpr auto FirstLines = std::array {
    "On branch "sv,
    "HEAD detached at "sv,
    "HEAD detached from "sv,
    "Not currently on any branch."sv,
    "interactive rebase in progress; onto "sv,
    "rebase in progress; onto "sv,
};

/// A sandbox, or the case skipped with the reason when this machine cannot have one.
[[nodiscard]] std::unique_ptr<GitSandbox> sandboxOrSkip()
{
    auto made = GitSandbox::create(core::LiveEnvironment {});
    if (!made && made.error() == GitSandboxError::GitNotFound)
        SKIP("git not found on PATH");
    if (!made && made.error() == GitSandboxError::Unsupported)
        SKIP("this platform has no directory handles");
    REQUIRE(made.has_value());
    return std::move(*made);
}

[[nodiscard]] std::vector<std::string_view> linesOf(std::string_view text)
{
    auto lines = std::vector<std::string_view> {};
    for (auto const part: std::views::split(text, '\n'))
        if (!part.empty())
            lines.emplace_back(part.begin(), part.end());
    return lines;
}

[[nodiscard]] bool hasLineStartingWith(std::string_view text, std::string_view prefix)
{
    return std::ranges::any_of(linesOf(text),
                               [prefix](std::string_view line) { return line.starts_with(prefix); });
}

[[nodiscard]] bool hasLine(std::string_view text, std::string_view wanted)
{
    return std::ranges::contains(linesOf(text), wanted);
}

[[nodiscard]] std::optional<std::string_view> valueOf(GitEnvironment const& environment,
                                                      std::string_view name)
{
    auto const entry = std::ranges::find(environment, name, &GitEnvironment::value_type::first);
    if (entry == environment.end())
        return std::nullopt;
    return entry->second;
}

/// @return The `GIT_CONFIG_KEY_n`/`GIT_CONFIG_VALUE_n` pairs `GIT_CONFIG_COUNT` makes git read.
[[nodiscard]] std::vector<std::pair<std::string_view, std::string_view>> configPairsOf(
    GitEnvironment const& environment)
{
    auto pairs = std::vector<std::pair<std::string_view, std::string_view>> {};
    auto const countText = valueOf(environment, "GIT_CONFIG_COUNT").value_or("0");
    auto count = std::size_t { 0 };
    REQUIRE(std::from_chars(countText.data(), countText.data() + countText.size(), count).ec == std::errc {});
    for (auto const index: std::views::iota(std::size_t { 0 }, count))
    {
        auto const key = valueOf(environment, std::format("GIT_CONFIG_KEY_{}", index));
        auto const value = valueOf(environment, std::format("GIT_CONFIG_VALUE_{}", index));
        REQUIRE(key.has_value());
        REQUIRE(value.has_value());
        pairs.emplace_back(*key, *value);
    }
    return pairs;
}

/// Wraps the sandbox's runner, keeping each spec's argv and environment. Only git is run: `gh` would reach
/// the network and `contour` the user's daemon, so their starts are recorded and answered ProgramNotFound.
class RecordingRunner final: public vtpty::IProcessRunner
{
  public:
    /// One start() the runner was asked for.
    struct Recorded
    {
        std::vector<std::string> argv; ///< The program and its arguments.
        GitEnvironment environment;    ///< The child's whole environment.
    };

    explicit RecordingRunner(vtpty::IProcessRunner& inner): _inner { inner } {}

    [[nodiscard]] std::expected<vtpty::ProcessToken, vtpty::ProcessStartError> start(
        vtpty::ProcessSpec spec, vtpty::CompletionHandler done) override
    {
        {
            auto const lock = std::scoped_lock { _mutex };
            _recorded.push_back(Recorded { .argv = spec.argv, .environment = spec.environment });
        }
        if (spec.argv.empty() || spec.argv.front() != "git")
            return std::unexpected(vtpty::ProcessStartError::ProgramNotFound);
        return _inner.start(std::move(spec), std::move(done));
    }

    void cancel(vtpty::ProcessToken token) override { _inner.cancel(token); }

    /// @return Every start() so far, in order.
    [[nodiscard]] std::vector<Recorded> recorded() const
    {
        auto const lock = std::scoped_lock { _mutex };
        return _recorded;
    }

  private:
    vtpty::IProcessRunner& _inner;
    mutable std::mutex _mutex;
    std::vector<Recorded> _recorded;
};

/// How a declared command ended, and what it printed.
struct CommandResult
{
    ProcCompletion completion = ProcCompletion::Exited;
    std::optional<int32_t> exitCode;
    std::string standardOutput;
    std::string standardError;
};

/// The orchestrator's roots over a sandbox: `worktree_root` as @p worktreeRoot spells it, the repository.
[[nodiscard]] std::vector<ResolvedRoot> rootsOver(Manifest const& manifest,
                                                  GitSandbox const& sandbox,
                                                  std::filesystem::path const& worktreeRoot)
{
    auto const config = std::format(R"({{"worktree_root":"{}","repos":[{{"path":"{}"}}]}})",
                                    worktreeRoot.string(),
                                    sandbox.repository().string());
    auto roots = resolveRoots(
        manifest, config, (sandbox.root() / "home").string(), core::platform::NativeFileSystem::instance());
    REQUIRE(roots.has_value());
    return std::move(*roots);
}

/// The orchestrator's declared processes over a sandbox, granted, on the native runner and opener.
class DeclaredCleanup
{
  public:
    /// @param sandbox What the commands run in; outlives this.
    /// @param worktreeRoot `worktree_root` as configured.
    DeclaredCleanup(GitSandbox const& sandbox, std::filesystem::path const& worktreeRoot):
        _recorder { sandbox.runner() },
        _roots { rootsOver(orchestratorManifest(), sandbox, worktreeRoot) },
        _processes { make(sandbox) }
    {
        _grants.record(HostId, GrantDigest, GrantState::Granted);
    }

    /// Like DeclaredCleanup(sandbox, sandbox.worktreeRoot()).
    explicit DeclaredCleanup(GitSandbox const& sandbox): DeclaredCleanup(sandbox, sandbox.worktreeRoot()) {}

    DeclaredCleanup(DeclaredCleanup const&) = delete;
    DeclaredCleanup(DeclaredCleanup&&) = delete;
    DeclaredCleanup& operator=(DeclaredCleanup const&) = delete;
    DeclaredCleanup& operator=(DeclaredCleanup&&) = delete;
    ~DeclaredCleanup() = default;

    /// Starts @p command in @p cwd.
    [[nodiscard]] std::expected<ProcessHandle, ProcessError> start(std::string_view command,
                                                                   std::filesystem::path const& cwd,
                                                                   ConfigMap const& arguments = {})
    {
        auto started = _processes->start(command, arguments, cwd.string(), "cleanup");
        if (started)
        {
            auto const lock = std::scoped_lock { _mutex };
            ++_started;
        }
        return started;
    }

    /// Waits for @p handle, the one process running, to end; takes what it printed and releases it.
    [[nodiscard]] CommandResult finish(ProcessHandle handle)
    {
        {
            auto lock = std::unique_lock { _mutex };
            _ended.wait(lock, [this] { return _finished >= _started; });
        }
        auto const infos = _processes->list();
        auto const info = std::ranges::find(infos, handle, &ProcessInfo::handle);
        REQUIRE(info != infos.end());
        REQUIRE(info->result.has_value());
        auto result = CommandResult { .completion = info->result->completion,
                                      .exitCode = info->result->exitCode,
                                      .standardOutput = take(handle, ProcStream::Stdout),
                                      .standardError = take(handle, ProcStream::Stderr) };
        REQUIRE(_processes->release(handle).has_value());
        return result;
    }

    /// Runs @p command in @p cwd to its end.
    [[nodiscard]] CommandResult run(std::string_view command,
                                    std::filesystem::path const& cwd,
                                    ConfigMap const& arguments = {})
    {
        auto const handle = start(command, cwd, arguments);
        INFO(command);
        REQUIRE(handle.has_value());
        auto result = finish(*handle);
        INFO(result.standardError);
        CHECK(result.completion == ProcCompletion::Exited);
        return result;
    }

    [[nodiscard]] RecordingRunner const& recorder() const noexcept { return _recorder; }
    [[nodiscard]] std::vector<ResolvedRoot> const& roots() const noexcept { return _roots; }

  private:
    [[nodiscard]] std::unique_ptr<DeclaredProcesses> make(GitSandbox const& sandbox)
    {
        auto created = DeclaredProcesses::create(
            DeclaredProcessesConfig { .manifest = orchestratorManifest(),
                                      .roots = _roots,
                                      .inheritedEnvironment = sandbox.environmentEntries(),
                                      .hostId = std::string(HostId),
                                      .grantDigest = std::string(GrantDigest),
                                      .timeouts = {},
                                      .limits = {} },
            DeclaredProcessesServices { .runner = _recorder,
                                        .directories = vtpty::nativeDirectoryOpener(),
                                        .grants = _grants,
                                        .onFinished = [this] {
                                            {
                                                auto const lock = std::scoped_lock { _mutex };
                                                ++_finished;
                                            }
                                            _ended.notify_all();
                                        } });
        REQUIRE(created.has_value());
        return std::move(*created);
    }

    [[nodiscard]] std::string take(ProcessHandle handle, ProcStream stream) const
    {
        auto bytes = std::string {};
        while (true)
        {
            auto page = _processes->take(handle, stream, bytes.size());
            REQUIRE(page.has_value());
            bytes += page->bytes;
            if (page->remaining == 0)
                return bytes;
        }
    }

    RecordingRunner _recorder;
    FakeGrantStore _grants;
    std::mutex _mutex;
    std::condition_variable _ended;
    std::size_t _started = 0;
    std::size_t _finished = 0;
    std::vector<ResolvedRoot> _roots;
    std::unique_ptr<DeclaredProcesses> _processes; ///< Last: destroyed first, while what it calls lives.
};

/// Where a rebase starts: git words some of its status lines differently for a detached HEAD.
enum class Head : uint8_t
{
    Detached, ///< As the orchestrator's worktrees are made (`git worktree add --detach`).
    OnBranch, ///< On a branch made there first.
};

/// Runs git in one directory of a sandbox: the steps that bring a worktree into the state a case needs.
class GitIn
{
  public:
    GitIn(GitSandbox const& sandbox, std::filesystem::path directory):
        _sandbox { sandbox }, _directory { std::move(directory) }
    {
    }

    /// Runs a step that must succeed.
    void run(std::vector<std::string> argv, GitEnvironment const& extra = {}) const
    {
        auto shown = std::string {};
        for (auto const& word: argv)
            shown += std::format("{} ", word);
        auto const result = _sandbox.runGit(_directory, std::move(argv), extra);
        INFO(shown << ": " << result.standardError);
        REQUIRE(result.exitCode == 0);
    }

    /// Runs a step that stops on a conflict: its exit status is not the point.
    void attempt(std::vector<std::string> argv, GitEnvironment const& extra = {}) const
    {
        [[maybe_unused]] auto const result = _sandbox.runGit(_directory, std::move(argv), extra);
    }

    /// @return What @p argv printed to standard output; it must succeed.
    [[nodiscard]] std::string output(std::vector<std::string> argv) const
    {
        auto result = _sandbox.runGit(_directory, std::move(argv));
        REQUIRE(result.exitCode == 0);
        while (result.standardOutput.ends_with('\n'))
            result.standardOutput.pop_back();
        return result.standardOutput;
    }

    /// Writes @p text to @p file.
    void write(std::string_view file, std::string_view text) const
    {
        GitSandbox::writeFile(_directory / file, text);
    }

    /// Writes @p text to @p file and commits it.
    void commit(std::string_view file, std::string_view text, std::string_view message) const
    {
        write(file, text);
        run({ "git", "add", "--", std::string(file) });
        run({ "git", "commit", "-q", "-m", std::string(message) });
    }

    /// From the detached HEAD (`f` is `a`): `work`, checked out, changes `f` one way; `side` changes it
    /// another, then adds `g`.
    void diverge() const
    {
        run({ "git", "checkout", "-q", "-b", "work" });
        run({ "git", "branch", "side" });
        commit("f", "a\nwork\n", "work");
        run({ "git", "checkout", "-q", "side" });
        commit("f", "a\nside\n", "side");
        commit("g", "g\n", "g");
        run({ "git", "checkout", "-q", "work" });
    }

    /// Stops a `rebase -i` of a new commit at an `edit`, on @p head.
    void stopAtEdit(Head head) const
    {
        if (head == Head::OnBranch)
            run({ "git", "checkout", "-q", "-b", "work" });
        commit("f", "a\nb\n", "b");
        auto const commit = output({ "git", "rev-parse", "HEAD" });
        attempt({ "git", "rebase", "-q", "-i", "HEAD~1" },
                { { "GIT_SEQUENCE_EDITOR", std::format("echo edit {} >", commit) } });
    }

    /// Commits two changes to `f`, the second building on the first, so that reverting the first
    /// conflicts.
    void twoChanges() const
    {
        commit("f", "a\nb\n", "b");
        commit("f", "a\nb\nc\n", "c");
    }

  private:
    GitSandbox const& _sandbox;
    std::filesystem::path _directory;
};

/// Selects the sentinel filter by @p selectFilter, checks that a plain `git status` runs it, then runs every
/// cleanup command — the unforced remove last — and checks that none of them did.
void requireNoFilterRuns(GitSandbox const& sandbox, std::function<void(GitIn const&)> const& selectFilter)
{
    auto const worktree = GitIn(sandbox, sandbox.worktree());
    selectFilter(worktree);
    std::filesystem::remove(sandbox.sentinel());
    GitSandbox::makeStatDirty(sandbox.worktree() / "f");

    // The fixture is live: git as the user runs it reads `f` through the filter.
    worktree.attempt({ "git", "status", "--porcelain" });
    REQUIRE(std::filesystem::exists(sandbox.sentinel()));
    std::filesystem::remove(sandbox.sentinel());
    GitSandbox::makeStatDirty(sandbox.worktree() / "f");

    auto cleanup = DeclaredCleanup(sandbox);
    for (auto const command: CleanupCommands)
    {
        auto const arguments = command == "git-worktree-remove"
                                   ? ConfigMap { { "path", sandbox.worktree().string() } }
                                   : ConfigMap {};
        auto const result = cleanup.run(command, sandbox.worktree(), arguments);
        INFO(command << ": " << result.standardError);
        CHECK(result.exitCode == 0);
        CHECK_FALSE(std::filesystem::exists(sandbox.sentinel()));
    }
    // The unforced remove found the worktree clean, through its own `git status`, and removed it.
    CHECK_FALSE(std::filesystem::exists(sandbox.worktree()));
}

} // namespace

TEST_CASE("GIT_ATTR_SOURCE and core.attributesFile reach exactly the cleanup commands",
          "[vtextension][git][cleanup]")
{
    auto const sandbox = sandboxOrSkip();
    // Shown on a passing run too, for whoever reads a CI log: which git ran, over which system configuration.
    WARN(std::format("{}; system configuration: {}", sandbox->gitVersion(), sandbox->systemConfiguration()));
    CHECK(sandbox->gitVersion().starts_with("git version "));

    GitIn(*sandbox, sandbox->repository())
        .run({ "git", "remote", "add", "origin", sandbox->repository().string() });
    using Value = std::pair<std::string_view, std::string>;
    auto const values = std::array {
        std::pair { "number"sv, std::string("1") },
        std::pair { "session"sv, std::string("01234567-89ab-4cde-8f01-23456789abcd") },
        std::pair { "remote"sv, std::string("origin") },
        std::pair { "model"sv, std::string("model") },
        std::pair { "branch"sv, std::string("agent/environment") },
        std::pair { "base"sv, std::string("main") },
        std::pair { "path"sv, (sandbox->worktreeRoot() / "environment").string() },
        std::pair { "query"sv, std::string("query") },
    };

    auto const manifest = orchestratorManifest();
    for (auto const& table: { std::span<std::string_view const>(CleanupCommands),
                              std::span<std::string_view const>(SetupCommands) })
        for (auto const name: table)
            CHECK(std::ranges::contains(manifest.commands, name, &DeclaredCommand::name));

    auto cleanup = DeclaredCleanup(*sandbox);
    for (auto const& command: manifest.commands)
    {
        INFO(command.name);
        auto arguments = ConfigMap {};
        for (std::string_view const word: command.argvTemplate)
        {
            if (!word.starts_with('{') || !word.ends_with('}'))
                continue;
            auto const name = word.substr(1, word.size() - 2);
            auto const value = std::ranges::find(values, name, &Value::first);
            REQUIRE(value != values.end());
            arguments.emplace_back(std::string(name), value->second);
        }
        auto const cwd = command.root == "repos" ? sandbox->repository() : sandbox->worktree();
        auto const before = cleanup.recorder().recorded().size();
        auto const started = cleanup.start(command.name, cwd, arguments);
        auto const recorded = cleanup.recorder().recorded();
        REQUIRE(recorded.size() == before + 1);
        if (started)
        {
            [[maybe_unused]] auto const ended = cleanup.finish(*started);
        }

        auto const& environment = recorded.back().environment;
        auto const pairs = configPairsOf(environment);
        auto const attributesFile = std::ranges::find(
            pairs, "core.attributesFile"sv, &std::pair<std::string_view, std::string_view>::first);
        if (std::ranges::contains(CleanupCommands, std::string_view(command.name)))
        {
            CHECK(valueOf(environment, "GIT_ATTR_SOURCE") == EmptyTree);
            REQUIRE(attributesFile != pairs.end());
            CHECK(attributesFile->second == "/dev/null");
            CHECK(valueOf(environment, "GIT_OPTIONAL_LOCKS") == "0");
        }
        else
        {
            CHECK_FALSE(valueOf(environment, "GIT_ATTR_SOURCE").has_value());
            CHECK(attributesFile == pairs.end());
            CHECK_FALSE(valueOf(environment, "GIT_OPTIONAL_LOCKS").has_value());
        }
    }
}

TEST_CASE("detached-HEAD commit is kept", "[vtextension][git][cleanup]")
{
    auto const sandbox = sandboxOrSkip();
    auto cleanup = DeclaredCleanup(*sandbox);
    auto const worktree = GitIn(*sandbox, sandbox->worktree());

    // Checked out from `main`, HEAD is in a ref.
    auto const before = cleanup.run("git-head-reachable", sandbox->worktree());
    CHECK(before.exitCode == 0);
    CHECK_FALSE(linesOf(before.standardOutput).empty());

    worktree.commit("g", "g\n", "only on the detached HEAD");
    auto const reachable = cleanup.run("git-head-reachable", sandbox->worktree());
    CHECK(reachable.exitCode == 0);
    CHECK(reachable.standardOutput.empty());
    // The trap the reachability check exists for: nothing for git status to report.
    auto const status = cleanup.run("git-status", sandbox->worktree());
    CHECK(status.exitCode == 0);
    CHECK(status.standardOutput.empty());
}

TEST_CASE("an operation in progress is reported", "[vtextension][git][cleanup]")
{
    struct Row
    {
        std::string_view prefix;
        std::function<void(GitIn const&)> enter;
    };
    auto const interactiveRebase = [](GitIn const& git) {
        git.diverge();
        git.run({ "git", "checkout", "-q", "side" });
        git.attempt({ "git", "rebase", "-q", "-i", "work" }, { { "GIT_SEQUENCE_EDITOR", "true" } });
    };
    auto const conflictingMerge = [](GitIn const& git) {
        git.diverge();
        git.attempt({ "git", "merge", "-q", "side" });
    };
    auto const rows = std::vector<Row> {
        { "interactive rebase in progress; onto "sv, interactiveRebase },
        { "rebase in progress; onto "sv,
          [](GitIn const& git) {
              git.diverge();
              git.run({ "git", "checkout", "-q", "side" });
              git.attempt({ "git", "rebase", "-q", "--apply", "work" });
          } },
        { "You are currently rebasing"sv, interactiveRebase },
        { "You are currently editing a commit while rebasing"sv,
          [](GitIn const& git) { git.stopAtEdit(Head::OnBranch); } },
        { "You are currently splitting a commit while rebasing"sv,
          [](GitIn const& git) {
              git.stopAtEdit(Head::OnBranch);
              git.run({ "git", "reset", "-q", "HEAD^" });
          } },
        { "You have unmerged paths."sv, conflictingMerge },
        { "All conflicts fixed but you are still merging."sv,
          [conflictingMerge](GitIn const& git) {
              conflictingMerge(git);
              git.write("f", "a\nresolved\n");
              git.run({ "git", "add", "f" });
          } },
        { "You are currently cherry-picking commit "sv,
          [](GitIn const& git) {
              git.diverge();
              git.attempt({ "git", "cherry-pick", "side~1" });
          } },
        { "Cherry-pick currently in progress."sv,
          [](GitIn const& git) {
              // The first of two conflicts and is committed by hand: the sequence is still in progress.
              git.diverge();
              git.attempt({ "git", "cherry-pick", "side~1", "side" });
              git.write("f", "a\nresolved\n");
              git.run({ "git", "add", "f" });
              git.run({ "git", "commit", "-q", "--no-edit" });
          } },
        { "You are currently reverting commit "sv,
          [](GitIn const& git) {
              git.twoChanges();
              git.attempt({ "git", "revert", "--no-edit", "HEAD~1" });
          } },
        { "Revert currently in progress."sv,
          [](GitIn const& git) {
              // The first of two conflicts and is committed by hand: the sequence is still in progress.
              git.twoChanges();
              git.attempt({ "git", "revert", "--no-edit", "HEAD~1", "HEAD" });
              git.write("f", "a\n");
              git.run({ "git", "add", "f" });
              git.run({ "git", "commit", "-q", "--no-edit" });
          } },
        { "You are currently bisecting"sv,
          [](GitIn const& git) {
              git.run({ "git", "bisect", "start" });
              git.run({ "git", "bisect", "bad" });
          } },
        { "You are in the middle of an am session."sv,
          [](GitIn const& git) {
              git.diverge();
              auto const patch = git.output({ "git", "format-patch", "-1", "side~1", "-o", "../patches" });
              git.attempt({ "git", "am", "-q", patch });
          } },
    };
    for (auto const prefix: StateLines)
        CHECK(std::ranges::count(rows, prefix, &Row::prefix) == 1);

    for (auto const& row: rows)
    {
        INFO(row.prefix);
        auto const sandbox = sandboxOrSkip();
        row.enter(GitIn(*sandbox, sandbox->worktree()));
        auto cleanup = DeclaredCleanup(*sandbox);
        auto const state = cleanup.run("git-operation-state", sandbox->worktree());
        INFO(state.standardOutput);
        CHECK(state.exitCode == 0);
        auto const lines = linesOf(state.standardOutput);
        REQUIRE_FALSE(lines.empty());
        CHECK(std::ranges::any_of(FirstLines,
                                  [&](std::string_view shape) { return lines.front().starts_with(shape); }));
        CHECK(hasLineStartingWith(state.standardOutput, row.prefix));
    }
}

TEST_CASE("a rebase stopped on a detached HEAD is reported by its first line", "[vtextension][git][cleanup]")
{
    // On a detached HEAD — how the orchestrator makes its worktrees — git words the `edit` stop "editing a
    // commit during a rebase", which no StateLines entry names: the rebase is found by the header line.
    auto const sandbox = sandboxOrSkip();
    GitIn(*sandbox, sandbox->worktree()).stopAtEdit(Head::Detached);
    auto cleanup = DeclaredCleanup(*sandbox);
    auto const state = cleanup.run("git-operation-state", sandbox->worktree());
    INFO(state.standardOutput);
    CHECK(state.exitCode == 0);
    auto const lines = linesOf(state.standardOutput);
    REQUIRE_FALSE(lines.empty());
    CHECK(lines.front().starts_with("interactive rebase in progress; onto "));
}

TEST_CASE("the first line is one the parser allows", "[vtextension][git][cleanup]")
{
    auto const detachedByCheckout = [](GitSandbox const& sandbox) {
        auto const detached = sandbox.worktreeRoot() / "detached";
        GitIn(sandbox, sandbox.repository())
            .run({ "git", "worktree", "add", "-q", "-b", "agent/detached", detached.string(), "main" });
        GitIn(sandbox, detached).run({ "git", "checkout", "-q", "--detach" });
        return detached;
    };
    struct Row
    {
        std::string_view name;
        std::string_view firstLine;
        std::function<std::filesystem::path(GitSandbox const&)> enter;
    };
    auto const rows = std::array {
        Row { "a clean worktree on a branch",
              "On branch "sv,
              [](GitSandbox const& sandbox) {
                  auto const branched = sandbox.worktreeRoot() / "branched";
                  GitIn(sandbox, sandbox.repository())
                      .run(
                          { "git", "worktree", "add", "-q", "-b", "agent/clean", branched.string(), "main" });
                  return branched;
              } },
        // As the orchestrator makes it: `worktree add --detach` writes no checkout to HEAD's reflog, which is
        // where git reads "detached at" from.
        Row { "a clean worktree added detached",
              "Not currently on any branch."sv,
              [](GitSandbox const& sandbox) { return sandbox.worktree(); } },
        Row { "a clean worktree detached by checkout",
              "HEAD detached at "sv,
              [detachedByCheckout](GitSandbox const& sandbox) { return detachedByCheckout(sandbox); } },
        Row { "a clean worktree detached by checkout that committed",
              "HEAD detached from "sv,
              [detachedByCheckout](GitSandbox const& sandbox) {
                  auto const worktree = detachedByCheckout(sandbox);
                  GitIn(sandbox, worktree).commit("g", "g\n", "g");
                  return worktree;
              } },
    };
    for (auto const& row: rows)
    {
        INFO(row.name);
        CHECK(std::ranges::contains(FirstLines, row.firstLine));
        auto const sandbox = sandboxOrSkip();
        auto const worktree = row.enter(*sandbox);
        auto cleanup = DeclaredCleanup(*sandbox);
        auto const state = cleanup.run("git-operation-state", worktree);
        INFO(state.standardOutput);
        CHECK(state.exitCode == 0);
        auto const lines = linesOf(state.standardOutput);
        REQUIRE_FALSE(lines.empty());
        CHECK(lines.front().starts_with(row.firstLine));
        for (auto const prefix: StateLines)
            CHECK_FALSE(hasLineStartingWith(state.standardOutput, prefix));
    }
}

TEST_CASE("status.showUntrackedFiles=no in the worktree does not hide an untracked file",
          "[vtextension][git][cleanup]")
{
    auto const sandbox = sandboxOrSkip();
    auto const worktree = GitIn(*sandbox, sandbox->worktree());
    worktree.run({ "git", "config", "status.showUntrackedFiles", "no" });
    worktree.write("new.txt", "new\n");
    // The configuration is in effect: a plain git status hides the file.
    CHECK_FALSE(hasLine(worktree.output({ "git", "status", "--porcelain" }), "?? new.txt"));

    auto cleanup = DeclaredCleanup(*sandbox);
    auto const status = cleanup.run("git-status", sandbox->worktree());
    CHECK(status.exitCode == 0);
    CHECK(hasLine(status.standardOutput, "?? new.txt"));
}

TEST_CASE("a clean filter is not run by the cleanup commands", "[vtextension][git][cleanup]")
{
    struct Row
    {
        std::string_view name;
        std::function<void(GitSandbox const&, GitIn const&)> select;
    };
    auto const rows = std::array {
        Row { "the worktree's .gitattributes",
              [](GitSandbox const&, GitIn const& worktree) {
                  worktree.commit(".gitattributes", "f filter=sentinel\n", "attributes");
              } },
        Row { "core.attributesFile",
              [](GitSandbox const& sandbox, GitIn const& worktree) {
                  auto const attributes = sandbox.root() / "attributes";
                  GitSandbox::writeFile(attributes, "f filter=sentinel\n");
                  worktree.run({ "git", "config", "core.attributesFile", attributes.string() });
              } },
        Row { "XDG git/attributes",
              [](GitSandbox const& sandbox, GitIn const&) {
                  GitSandbox::writeFile(sandbox.xdgConfigHome() / "git" / "attributes",
                                        "f filter=sentinel\n");
              } },
    };
    for (auto const& row: rows)
    {
        INFO(row.name);
        auto const sandbox = sandboxOrSkip();
        requireNoFilterRuns(*sandbox, [&](GitIn const& worktree) { row.select(*sandbox, worktree); });
    }
}

TEST_CASE("a filter in the main repository's info/attributes still runs", "[vtextension][git][cleanup]")
{
    // The documented residual: git reads the common directory's info/attributes, and no variable turns it
    // off. Writing there needs the main repository, which the agent's worktree does not grant.
    auto const sandbox = sandboxOrSkip();
    GitSandbox::writeFile(sandbox->repository() / ".git" / "info" / "attributes", "f filter=sentinel\n");
    GitSandbox::makeStatDirty(sandbox->worktree() / "f");
    std::filesystem::remove(sandbox->sentinel());

    auto cleanup = DeclaredCleanup(*sandbox);
    auto const status = cleanup.run("git-status", sandbox->worktree());
    CHECK(status.exitCode == 0);
    CHECK(std::filesystem::exists(sandbox->sentinel()));
}

TEST_CASE("a redirected commondir is visible to git-common-dir", "[vtextension][git][cleanup]")
{
    auto const sandbox = sandboxOrSkip();
    auto const elsewhere = sandbox->root() / "elsewhere.git";
    std::filesystem::copy(
        sandbox->repository() / ".git", elsewhere, std::filesystem::copy_options::recursive);
    // The copied config defines the filter; its info/attributes selects it for everything.
    GitSandbox::writeFile(elsewhere / "info" / "attributes", "* filter=sentinel\n");
    GitSandbox::writeFile(sandbox->repository() / ".git" / "worktrees" / "wt" / "commondir",
                          std::format("{}\n", elsewhere.string()));
    GitSandbox::makeStatDirty(sandbox->worktree() / "f");
    std::filesystem::remove(sandbox->sentinel());

    auto cleanup = DeclaredCleanup(*sandbox);
    auto const commonDir = cleanup.run("git-common-dir", sandbox->worktree());
    CHECK(commonDir.exitCode == 0);
    auto const lines = linesOf(commonDir.standardOutput);
    REQUIRE(lines.size() == 1);
    CHECK(lines.front() != (sandbox->repository() / ".git").string());
    CHECK(lines.front() == elsewhere.string());
    CHECK_FALSE(std::filesystem::exists(sandbox->sentinel()));

    // The redirection is live: a plain git status runs the filter the redirected config names.
    GitIn(*sandbox, sandbox->worktree()).attempt({ "git", "status", "--porcelain" });
    CHECK(std::filesystem::exists(sandbox->sentinel()));
}

TEST_CASE("a committed submodule shows mode 160000 in git-index-flags", "[vtextension][git][cleanup]")
{
    auto const sandbox = sandboxOrSkip();
    auto const worktree = GitIn(*sandbox, sandbox->worktree());
    auto const head = worktree.output({ "git", "rev-parse", "HEAD" });
    worktree.run({ "git", "update-index", "--add", "--cacheinfo", std::format("160000,{},sub", head) });
    worktree.run({ "git", "commit", "-q", "-m", "submodule" });

    auto cleanup = DeclaredCleanup(*sandbox);
    auto const flags = cleanup.run("git-index-flags", sandbox->worktree());
    INFO(flags.standardOutput);
    CHECK(flags.exitCode == 0);
    CHECK(std::ranges::any_of(linesOf(flags.standardOutput), [](std::string_view line) {
        return line.starts_with("H 160000 ") && line.ends_with("\tsub");
    }));
}

TEST_CASE("an assume-unchanged file shows h", "[vtextension][git][cleanup]")
{
    auto const sandbox = sandboxOrSkip();
    GitIn(*sandbox, sandbox->worktree()).run({ "git", "update-index", "--assume-unchanged", "f" });

    auto cleanup = DeclaredCleanup(*sandbox);
    auto const flags = cleanup.run("git-index-flags", sandbox->worktree());
    INFO(flags.standardOutput);
    CHECK(flags.exitCode == 0);
    CHECK(std::ranges::any_of(linesOf(flags.standardOutput), [](std::string_view line) {
        return line.starts_with("h ") && line.ends_with("\tf");
    }));
}

TEST_CASE("a symlinked worktree_root resolves to one root", "[vtextension][git][cleanup]")
{
    auto const sandbox = sandboxOrSkip();
    auto const link = sandbox->root() / "link";
    std::filesystem::create_directory_symlink(sandbox->worktreeRoot(), link);
    GitIn(*sandbox, sandbox->worktree()).write("only-here.txt", "x\n");

    auto cleanup = DeclaredCleanup(*sandbox, link);
    auto const worktrees = std::ranges::find(cleanup.roots(), "worktrees"sv, &ResolvedRoot::label);
    REQUIRE(worktrees != cleanup.roots().end());
    CHECK(worktrees->paths == std::vector { sandbox->worktreeRoot().string() });

    for (auto const& cwd: { link / "wt", sandbox->worktree() })
    {
        INFO(cwd.string());
        auto const status = cleanup.run("git-status", cwd);
        CHECK(status.exitCode == 0);
        CHECK(hasLine(status.standardOutput, "?? only-here.txt"));
        // The worktree belongs to `worktrees`: a command of `repos` may not start there.
        CHECK(cleanup.start("git-fetch", cwd, { { "remote", "origin" } })
              == std::unexpected(ProcessError::CwdOutsideRoots));
    }
}

TEST_CASE("worktree remove without --force refuses a dirty tree", "[vtextension][git][cleanup]")
{
    auto const sandbox = sandboxOrSkip();
    GitIn(*sandbox, sandbox->worktree()).write("f", "changed\n");

    auto cleanup = DeclaredCleanup(*sandbox);
    auto const removed =
        cleanup.run("git-worktree-remove", sandbox->worktree(), { { "path", sandbox->worktree().string() } });
    REQUIRE(removed.exitCode.has_value());
    CHECK(removed.exitCode != 0);
    CHECK(std::filesystem::is_directory(sandbox->worktree()));
    CHECK(std::filesystem::exists(sandbox->worktree() / "f"));
}

TEST_CASE("git clean removes only STATUS.md", "[vtextension][git][cleanup]")
{
    auto const sandbox = sandboxOrSkip();
    auto const worktree = GitIn(*sandbox, sandbox->worktree());
    worktree.write("STATUS.md", "status\n");
    worktree.write("other.txt", "other\n");
    worktree.write("f", "changed\n");

    auto cleanup = DeclaredCleanup(*sandbox);
    auto const cleaned = cleanup.run("git-clean", sandbox->worktree());
    CHECK(cleaned.exitCode == 0);
    CHECK_FALSE(std::filesystem::exists(sandbox->worktree() / "STATUS.md"));
    CHECK(std::filesystem::exists(sandbox->worktree() / "other.txt"));
    CHECK(hasLine(cleanup.run("git-status", sandbox->worktree()).standardOutput, " M f"));
}
