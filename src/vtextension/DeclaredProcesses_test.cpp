// SPDX-License-Identifier: Apache-2.0
//
// The declared-process policy over a fake runner and a fake filesystem, with the orchestrator's manifest from
// the conformance corpus: what is refused and why, where commands run, what they see, and how their results
// are paged, kept and released.
#include <vtpty/DirectoryHandle.hpp>
#include <vtpty/ProcessRunner.hpp>

#include <core/platform/Clock.hpp>
#include <core/testing/ScopedTempDir.hpp>

#include <catch2/catch_message.hpp>
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <expected>
#include <filesystem>
#include <format>
#include <functional>
#include <latch>
#include <memory>
#include <mutex>
#include <optional>
#include <ranges>
#include <set>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include <vtextension/CommandPolicy.hpp>
#include <vtextension/DaemonWorld.hpp>
#include <vtextension/DeclaredProcesses.hpp>
#include <vtextension/Manifest.hpp>
#include <vtextension/ProcessService.hpp>
#include <vtextension/RootResolution.hpp>
#include <vtextension/SelfPrograms.hpp>
#include <vtextension/UserSettableEnvironment.hpp>
#include <vtextension/testing/FakeDirectoryOpener.hpp>
#include <vtextension/testing/FakeGrantStore.hpp>
#include <vtextension/testing/FakeProcessRunner.hpp>
#include <vtextension/testing/OrchestratorManifest.hpp>

using namespace std::chrono_literals;
using namespace std::string_view_literals;
using namespace vtextension;
using vtextension::testing::FakeDirectoryOpener;
using vtextension::testing::FakeGrantStore;
using vtextension::testing::FakeProcessRunner;
using vtextension::testing::orchestratorManifest;

namespace
{

/// What the host calls the installation under test, and the digest it is granted for.
constexpr auto HostId = "orchestrator@/m"sv;
constexpr auto GrantDigest = "g1"sv;

constexpr std::size_t KiB = 1024;
constexpr std::size_t MiB = KiB * KiB;

[[nodiscard]] std::vector<ResolvedRoot> defaultRoots()
{
    return { { .label = "worktrees", .paths = { "/w" } }, { .label = "repos", .paths = { "/r/contour" } } };
}

/// The orchestrator's declarations over @p roots, granted as ("orchestrator@/m", "g1").
[[nodiscard]] DeclaredProcessesConfig configWith(std::vector<ResolvedRoot> roots, ProcessLimits limits = {})
{
    return DeclaredProcessesConfig {
        .manifest = orchestratorManifest(),
        .roots = std::move(roots),
        .inheritedEnvironment = { "PATH=/usr/bin", "GIT_CONFIG_COUNT=7", "CONTOUR_PANE=1" },
        .hostId = std::string(HostId),
        .grantDigest = std::string(GrantDigest),
        .timeouts = {},
        .limits = limits
    };
}

/// One DeclaredProcesses over fakes, granted, with `/w`, `/w/contour`, `/w/contour/pr-1` and `/r/contour`.
struct Fixture
{
    explicit Fixture(std::vector<ResolvedRoot> roots = defaultRoots(), ProcessLimits limits = {}):
        Fixture { configWith(std::move(roots), limits) }
    {
    }

    explicit Fixture(DeclaredProcessesConfig config)
    {
        for (auto const* path: { "/w", "/w/contour", "/w/contour/pr-1", "/r/contour" })
            directories.add(path);
        grants.record(HostId, GrantDigest, GrantState::Granted);
        auto created = DeclaredProcesses::create(
            std::move(config),
            DeclaredProcessesServices {
                .runner = runner, .directories = directories, .grants = grants, .onFinished = [this] {
                    ++finished;
                    if (hint)
                        hint();
                } });
        REQUIRE(created.has_value());
        processes = std::move(*created);
    }

    [[nodiscard]] std::expected<ProcessHandle, ProcessError> start(std::string_view command,
                                                                   ConfigMap const& arguments,
                                                                   std::string_view cwd,
                                                                   std::string_view tag = "t") const
    {
        return processes->start(command, arguments, cwd, tag);
    }

    /// Starts `gh-auth` in the repository @p count times.
    void startMany(std::size_t count) const
    {
        for ([[maybe_unused]] auto const _: std::views::iota(std::size_t { 0 }, count))
            REQUIRE(start("gh-auth", {}, "/r/contour").has_value());
    }

    /// Starts and finishes @p count `gh-auth` processes, a batch of at most eight at a time.
    void finishMany(std::size_t count)
    {
        for ([[maybe_unused]] auto const _: std::views::iota(std::size_t { 0 }, count))
        {
            REQUIRE(start("gh-auth", {}, "/r/contour").has_value());
            runner.exitWith(runner.started().size() - 1, "ok");
        }
    }

    FakeProcessRunner runner;
    FakeDirectoryOpener directories;
    FakeGrantStore grants;
    std::atomic<int> finished = 0;
    std::function<void()> hint; ///< Called by onFinished after counting, on the completing thread.
    std::unique_ptr<DeclaredProcesses> processes;
};

using Environment = std::vector<std::pair<std::string, std::string>>;

[[nodiscard]] std::vector<std::string_view> valuesOf(Environment const& environment, std::string_view name)
{
    auto values = std::vector<std::string_view> {};
    for (auto const& [entryName, value]: environment)
        if (entryName == name)
            values.emplace_back(value);
    return values;
}

[[nodiscard]] vtpty::ProcessExit exitOf(vtpty::ProcessCompletion completion,
                                        std::string standardOutput,
                                        std::string standardError)
{
    return vtpty::ProcessExit { .completion = completion,
                                .exitCode = std::nullopt,
                                .signal = 15,
                                .standardOutput = { .bytes = std::move(standardOutput) },
                                .standardError = { .bytes = std::move(standardError) } };
}

} // namespace

TEST_CASE("DeclaredProcesses: every refusal", "[daemon][processes]")
{
    struct Row
    {
        std::string_view name;
        std::string_view command;
        ConfigMap arguments;
        std::string_view cwd;
        std::function<void(Fixture&)> setup;
        ProcessError error;
    };
    auto const nothing = [](Fixture&) {
    };
    auto const rows = std::vector<Row> {
        { "undeclared", "nope", {}, "/r/contour", nothing, ProcessError::NotDeclared },
        { "pending",
          "gh-auth",
          {},
          "/r/contour",
          [](Fixture& f) { f.grants.record(HostId, GrantDigest, GrantState::Pending); },
          ProcessError::Pending },
        { "denied",
          "gh-auth",
          {},
          "/r/contour",
          [](Fixture& f) { f.grants.record(HostId, GrantDigest, GrantState::Denied); },
          ProcessError::Denied },
        { "remote not a name",
          "git-fetch",
          { { "remote", "ext::sh" } },
          "/r/contour",
          nothing,
          ProcessError::ArgumentRejected },
        { "text like an option",
          "gh-issues",
          { { "query", "-label:x" } },
          "/r/contour",
          nothing,
          ProcessError::ArgumentRejected },
        { "an extra name",
          "git-fetch",
          { { "remote", "origin" }, { "x", "1" } },
          "/r/contour",
          nothing,
          ProcessError::ArgumentRejected },
        { "a declared name the command does not use",
          "git-fetch",
          { { "remote", "origin" }, { "query", "q" } },
          "/r/contour",
          nothing,
          ProcessError::ArgumentRejected },
        { "a placeholder without a value",
          "git-fetch",
          {},
          "/r/contour",
          nothing,
          ProcessError::ArgumentRejected },
        { "relative cwd", "gh-auth", {}, "rel", nothing, ProcessError::CwdOutsideRoots },
        { "a repository command in a worktree",
          "git-fetch",
          { { "remote", "origin" } },
          "/w/contour/pr-1",
          nothing,
          ProcessError::CwdOutsideRoots },
        { "a worktree command in a repository",
          "gh-pr-checkout",
          { { "number", "1" }, { "branch", "agent/1-20261115-120000" } },
          "/r/contour",
          nothing,
          ProcessError::CwdOutsideRoots },
        { "a symlink out of the roots",
          "git-status",
          {},
          "/w/link",
          [](Fixture& f) { f.directories.link("/w/link", "/etc"); },
          ProcessError::CwdOutsideRoots },
        { "a missing cwd", "gh-auth", {}, "/r/contour/gone", nothing, ProcessError::CwdMissing },
        { "a ninth while eight run",
          "gh-auth",
          {},
          "/r/contour",
          [](Fixture& f) { f.startMany(8); },
          ProcessError::TooMany },
        { "thirty-two untaken results",
          "gh-auth",
          {},
          "/r/contour",
          [](Fixture& f) { f.finishMany(32); },
          ProcessError::TooMany },
    };
    for (auto const& row: rows)
    {
        INFO(row.name);
        auto fixture = Fixture {};
        row.setup(fixture);
        auto const startsBefore = fixture.runner.started().size();
        CHECK(fixture.start(row.command, row.arguments, row.cwd) == std::unexpected(row.error));
        CHECK(fixture.runner.started().size() == startsBefore);
    }
}

namespace
{
/// Starts `agent-transcript`, whose program is `contour`, and `gh-auth`, whose program is `gh`, under
/// @p config with every root present and the grant recorded.
/// @return The argv of each, in that order.
[[nodiscard]] std::vector<std::vector<std::string>> argvsOfContourAndGh(DeclaredProcessesConfig config)
{
    config.inheritedEnvironment = { "PATH=/usr/bin:/bin" };
    auto runner = FakeProcessRunner {};
    auto directories = FakeDirectoryOpener {};
    for (auto const* path: { "/w", "/w/contour", "/w/contour/pr-1", "/r/contour" })
        directories.add(path);
    auto grants = FakeGrantStore {};
    grants.record(HostId, GrantDigest, GrantState::Granted);
    auto processes = DeclaredProcesses::create(
        std::move(config),
        DeclaredProcessesServices {
            .runner = runner, .directories = directories, .grants = grants, .onFinished = [] {} });
    REQUIRE(processes.has_value());

    REQUIRE((*processes)
                ->start("agent-transcript",
                        { { "session", "00000000-0000-4000-8000-000000000001" },
                          { "path", "/w/contour/pr-1" },
                          { "page", "1" },
                          { "format", "1" } },
                        "/w",
                        "t")
                .has_value());
    REQUIRE((*processes)->start("gh-auth", {}, "/r/contour", "t").has_value());

    auto argvs = std::vector<std::vector<std::string>> {};
    for (auto const& started: runner.started())
        argvs.push_back(started.spec.argv);
    return argvs;
}
} // namespace

TEST_CASE("DeclaredProcesses: contour runs as the daemon's own executable", "[daemon][processes]")
{
    // A launchd agent's, an AppImage's or a .dmg's PATH holds no `contour`.
    constexpr auto Executable = "/Applications/Contour.app/Contents/MacOS/contour"sv;
    auto config = configWith(defaultRoots());
    config.programs = programTableOf({}, selfProgramsOf(std::filesystem::path { Executable }));
    auto const argvs = argvsOfContourAndGh(std::move(config));

    REQUIRE(argvs.size() == 2);
    auto const& transcript = argvs[0];
    REQUIRE(transcript.size() > 3);
    CHECK(transcript[0] == Executable);
    CHECK(transcript[1] == "agent");
    CHECK(transcript[2] == "transcript");
    // Another program is still found on the command's PATH.
    CHECK(argvs[1].front() == "gh");
}

TEST_CASE("DeclaredProcesses: a configured program runs its executable, before a self program",
          "[daemon][processes]")
{
    auto const configured = std::array { ProgramPath { .name = "gh", .path = "/home/u/.local/bin/gh" },
                                         ProgramPath { .name = "contour", .path = "/home/u/src/contour" } };
    auto config = configWith(defaultRoots());
    config.programs = programTableOf(configured, selfProgramsOf("/opt/contour/bin/contour"));
    auto const argvs = argvsOfContourAndGh(std::move(config));

    REQUIRE(argvs.size() == 2);
    // The user's choice wins over the daemon's own executable.
    CHECK(argvs[0].front() == "/home/u/src/contour");
    CHECK(argvs[1].front() == "/home/u/.local/bin/gh");
}

#ifndef _WIN32
TEST_CASE("DeclaredProcesses: a PATH without contour still runs the transcript command",
          "[daemon][processes]")
{
    // A real process: the daemon's PATH names an empty directory, and its own executable stands in for
    // contour -- /bin/echo here, so that what ran is what it prints.
    auto const directory = core::testing::ScopedTempDir { "contour-self-program" };
    auto const worktrees = std::filesystem::canonical(directory.path());
    auto const emptyPath = worktrees / "bin";
    std::filesystem::create_directory(emptyPath);
    auto runner = vtpty::ThreadedProcessRunner::create(core::platform::defaultSteadyClock(), {});
    REQUIRE(runner.has_value());
    auto grants = FakeGrantStore {};
    grants.record(HostId, GrantDigest, GrantState::Granted);
    auto mutex = std::mutex {};
    auto ended = std::condition_variable {};
    auto finished = 0;
    auto config = configWith({ { .label = "worktrees", .paths = { worktrees.string() } },
                               { .label = "repos", .paths = { (worktrees / "repo").string() } } });
    config.inheritedEnvironment = { std::format("PATH={}", emptyPath.string()) };
    config.programs = programTableOf({}, selfProgramsOf("/bin/echo"));
    auto processes =
        DeclaredProcesses::create(std::move(config),
                                  DeclaredProcessesServices { .runner = **runner,
                                                              .directories = vtpty::nativeDirectoryOpener(),
                                                              .grants = grants,
                                                              .onFinished = [&] {
                                                                  {
                                                                      auto const lock =
                                                                          std::scoped_lock { mutex };
                                                                      ++finished;
                                                                  }
                                                                  ended.notify_all();
                                                              } });
    REQUIRE(processes.has_value());

    auto const handle = (*processes)
                            ->start("agent-transcript",
                                    { { "session", "00000000-0000-4000-8000-000000000001" },
                                      { "path", (worktrees / "contour" / "run-1").string() },
                                      { "page", "2" },
                                      { "format", "1" } },
                                    worktrees.string(),
                                    "t");
    REQUIRE(handle.has_value());
    {
        auto lock = std::unique_lock { mutex };
        REQUIRE(ended.wait_for(lock, std::chrono::seconds { 30 }, [&] { return finished == 1; }));
    }
    auto const infos = (*processes)->list();
    REQUIRE(infos.size() == 1);
    REQUIRE(infos.front().result.has_value());
    CHECK(infos.front().result->exitCode == 0);
    auto const printed = (*processes)->take(*handle, ProcStream::Stdout, 0);
    REQUIRE(printed.has_value());
    CHECK(printed->bytes
          == std::format("agent transcript --session 00000000-0000-4000-8000-000000000001 --cwd {} --page 2 "
                         "--format 1 --json\n",
                         (worktrees / "contour" / "run-1").string()));
}
#endif

TEST_CASE("DeclaredProcesses: most specific root", "[daemon][processes]")
{
    auto fixture = Fixture { { { .label = "worktrees", .paths = { "/r/contour/.claude/worktrees" } },
                               { .label = "repos", .paths = { "/r/contour" } } } };
    fixture.directories.add("/r/contour/.claude/worktrees");
    fixture.directories.add("/r/contour/.claude/worktrees/x");

    CHECK(fixture.start("git-fetch", { { "remote", "origin" } }, "/r/contour/.claude/worktrees/x")
          == std::unexpected(ProcessError::CwdOutsideRoots));
    REQUIRE(fixture.start("git-status", {}, "/r/contour/.claude/worktrees/x").has_value());
    REQUIRE(fixture.runner.started().size() == 1);
    CHECK(fixture.runner.started().front().spec.workingDirectory->resolvedPath()
          == "/r/contour/.claude/worktrees/x");
}

TEST_CASE("DeclaredProcesses: a path argument resolves through its existing ancestor", "[daemon][processes]")
{
    auto fixture = Fixture {};
    auto const path = "/w/contour/pr-1-20261115-120000"sv;
    REQUIRE(fixture
                .start("git-worktree-add",
                       { { "path", std::string(path) }, { "base", "origin/main" } },
                       "/r/contour")
                .has_value());
    CHECK(fixture.runner.started().front().spec.argv
          == std::vector<std::string> {
              "git", "worktree", "add", "--detach", std::string(path), "origin/main" });

    fixture.directories.link("/w/escape", "/tmp");
    CHECK(fixture.start(
              "git-worktree-add", { { "path", "/w/escape/new" }, { "base", "origin/main" } }, "/r/contour")
          == std::unexpected(ProcessError::ArgumentRejected));
    CHECK(fixture.start(
              "git-worktree-add", { { "path", "/r/contour/x" }, { "base", "origin/main" } }, "/r/contour")
          == std::unexpected(ProcessError::ArgumentRejected));
    CHECK(fixture.start(
              "git-worktree-add", { { "path", "/w/../etc" }, { "base", "origin/main" } }, "/r/contour")
          == std::unexpected(ProcessError::ArgumentRejected));

    // The resolved path is what the command is given.
    fixture.directories.link("/w/alias", "/w/contour");
    REQUIRE(fixture
                .start("git-worktree-add",
                       { { "path", "/w/alias/pr-2" }, { "base", "origin/main" } },
                       "/r/contour")
                .has_value());
    CHECK(fixture.runner.started().back().spec.argv.at(4) == "/w/contour/pr-2");
}

TEST_CASE("DeclaredProcesses: command-env-for replaces command-env by name", "[daemon][processes]")
{
    auto fixture = Fixture {};
    REQUIRE(fixture.start("git-status", {}, "/w/contour/pr-1").has_value());
    REQUIRE(
        fixture.start("git-worktree-add", { { "path", "/w/contour/pr-2" }, { "base", "main" } }, "/r/contour")
            .has_value());

    auto const& status = fixture.runner.started().at(0).spec.environment;
    CHECK(valuesOf(status, "GIT_CONFIG_COUNT") == std::vector { "3"sv });
    CHECK(valuesOf(status, "GIT_CONFIG_KEY_2") == std::vector { "core.attributesFile"sv });
    CHECK(valuesOf(status, "GIT_ATTR_SOURCE").size() == 1);
    CHECK(valuesOf(status, "PATH") == std::vector { "/usr/bin"sv });
    CHECK(valuesOf(status, "CONTOUR_PANE").empty());
    auto names = std::set<std::string> {};
    for (auto const& entry: status)
        names.insert(entry.first);
    CHECK(names.size() == status.size());

    auto const& add = fixture.runner.started().at(1).spec.environment;
    CHECK(valuesOf(add, "GIT_CONFIG_COUNT") == std::vector { "2"sv });
    CHECK(valuesOf(add, "GIT_ATTR_SOURCE").empty());
}

TEST_CASE("DeclaredProcesses: timeout classes", "[daemon][processes]")
{
    auto fixture = Fixture {};
    REQUIRE(fixture.start("gh-auth", {}, "/r/contour").has_value());
    REQUIRE(fixture.start("gh-prs", {}, "/r/contour").has_value());
    auto const& started = fixture.runner.started();
    CHECK(started.at(0).spec.timeout == 30s);
    CHECK(started.at(1).spec.timeout == 10min);
    CHECK(started.at(0).spec.outputCap == MiB);
    CHECK(started.at(0).spec.standardOutput == vtpty::StreamCapture::Capture);
    CHECK(started.at(0).spec.standardError == vtpty::StreamCapture::Capture);
}

TEST_CASE("DeclaredProcesses: a command's dropped output lines reach the runner", "[daemon][processes]")
{
    auto fixture = Fixture {};
    REQUIRE(fixture.start("git-index-flags", {}, "/w/contour/pr-1").has_value());
    REQUIRE(fixture.start("git-status", {}, "/w/contour/pr-1").has_value());
    auto const& started = fixture.runner.started();
    CHECK(started.at(0).spec.droppedOutputLines == std::vector<std::string> { "H 10", "H 12" });
    CHECK(started.at(1).spec.droppedOutputLines.empty());
}

TEST_CASE("DeclaredProcesses: a root declared create is created before its first command",
          "[daemon][processes]")
{
    auto fixture = Fixture {};
    fixture.directories.directories.clear();

    REQUIRE(fixture.start("git-status", {}, "/w").has_value());
    REQUIRE(fixture.start("git-status", {}, "/w").has_value());
    CHECK(fixture.directories.created == std::vector<std::string> { "/w" });

    CHECK(fixture.start("git-fetch", { { "remote", "origin" } }, "/r/contour")
          == std::unexpected(ProcessError::CwdMissing));
    CHECK(fixture.directories.created == std::vector<std::string> { "/w" });
}

TEST_CASE("DeclaredProcesses: a create root inside another root is used but never created",
          "[daemon][processes]")
{
    auto const roots = std::vector<ResolvedRoot> { { .label = "worktrees", .paths = { "/r/contour/wt" } },
                                                   { .label = "repos", .paths = { "/r/contour" } } };
    auto fixture = Fixture { roots };

    CHECK(ensureRoot(fixture.directories, roots, "worktrees")
          == std::unexpected(RootError::CreateInsideRoot));
    CHECK(fixture.start("git-status", {}, "/r/contour/wt") == std::unexpected(ProcessError::CwdMissing));
    CHECK(fixture.directories.created.empty());

    fixture.directories.add("/r/contour/wt");
    CHECK(ensureRoot(fixture.directories, roots, "worktrees").has_value());
    CHECK(fixture.start("git-status", {}, "/r/contour/wt").has_value());
    CHECK(fixture.directories.created.empty());
}

TEST_CASE("DeclaredProcesses: a missing program is refused, never gone", "[daemon][processes]")
{
    for (auto const error:
         { vtpty::ProcessStartError::ProgramNotFound, vtpty::ProcessStartError::SpawnFailed })
    {
        auto fixture = Fixture {};
        fixture.runner.startError = error;
        CHECK(fixture.start("gh-auth", {}, "/r/contour") == std::unexpected(ProcessError::SpawnFailed));
        CHECK(fixture.processes->list().empty());
    }
    // A worktree path that is a file, or that may not be opened, is no directory: neither a refused start nor
    // a vanished worktree.
    for (auto const error: { vtpty::DirectoryError::NotADirectory, vtpty::DirectoryError::AccessDenied })
    {
        auto fixture = Fixture {};
        fixture.directories.failures.emplace("/w/contour/pr-1", error);
        CHECK(fixture.start("git-status", {}, "/w/contour/pr-1")
              == std::unexpected(ProcessError::CwdNotADirectory));
    }
    {
        auto fixture = Fixture {};
        fixture.runner.startError = vtpty::ProcessStartError::DirectoryMissing;
        CHECK(fixture.start("gh-auth", {}, "/r/contour") == std::unexpected(ProcessError::CwdMissing));
        fixture.runner.startError = vtpty::ProcessStartError::Unsupported;
        CHECK(fixture.start("gh-auth", {}, "/r/contour") == std::unexpected(ProcessError::Unsupported));
    }

    CHECK(hostErrorOf(ProcessError::SpawnFailed) == HostError::BadArgument);
    CHECK(hostErrorOf(ProcessError::CwdMissing) == HostError::Gone);
    CHECK(hostErrorOf(ProcessError::CwdNotADirectory) == HostError::NotADirectory);
    auto const gone = std::ranges::count_if(ProcessErrorToHostError, [](ProcessErrorRow const& row) {
        return row.hostError == HostError::Gone && row.error != ProcessError::UnknownHandle;
    });
    CHECK(gone == 1);
}

TEST_CASE("DeclaredProcesses: a cancelled process reads as signalled", "[daemon][processes]")
{
    auto fixture = Fixture {};
    REQUIRE(fixture.start("gh-auth", {}, "/r/contour").has_value());
    fixture.runner.complete(0, exitOf(vtpty::ProcessCompletion::Cancelled, "", ""));
    auto const listed = fixture.processes->list();
    REQUIRE(listed.size() == 1);
    REQUIRE(listed.front().result.has_value());
    CHECK(listed.front().result->completion == ProcCompletion::Signalled);
    CHECK_FALSE(listed.front().result->exitCode.has_value());
}

TEST_CASE("DeclaredProcesses: paging", "[daemon][processes]")
{
    auto fixture = Fixture {};
    auto const handle = fixture.start("gh-prs", {}, "/r/contour");
    REQUIRE(handle.has_value());
    CHECK(fixture.processes->take(*handle, ProcStream::Stdout, 0)
          == std::unexpected(ProcessError::StillRunning));

    fixture.runner.exitWith(0, std::string(150 * KiB, 'x'), "warning");
    auto const& processes = *fixture.processes;
    auto const page = [&](std::size_t offset) {
        return processes.take(*handle, ProcStream::Stdout, offset);
    };
    CHECK(page(0)->bytes.size() == 64 * KiB);
    CHECK(page(0)->remaining == 86 * KiB);
    CHECK(page(65536)->bytes.size() == 64 * KiB);
    CHECK(page(131072)->bytes.size() == 22 * KiB);
    CHECK(page(131072)->remaining == 0);
    CHECK(page(200000) == OutputPage {});
    CHECK(processes.take(*handle, ProcStream::Stderr, 0)
          == OutputPage { .bytes = "warning", .remaining = 0 });
    CHECK(processes.take(ProcessHandle { 999 }, ProcStream::Stdout, 0)
          == std::unexpected(ProcessError::UnknownHandle));

    auto const listed = processes.list();
    REQUIRE(listed.size() == 1);
    CHECK(listed.front().result
          == ProcessResult { .completion = ProcCompletion::Exited,
                             .exitCode = 0,
                             .stdoutBytes = 150 * KiB,
                             .stdoutExtent = OutputExtent::Whole,
                             .stderrBytes = 7,
                             .stderrExtent = OutputExtent::Whole });
}

TEST_CASE("DeclaredProcesses: a truncated stream reads as truncated", "[daemon][processes]")
{
    auto fixture = Fixture {};
    REQUIRE(fixture.start("gh-prs", {}, "/r/contour").has_value());
    auto exit = exitOf(vtpty::ProcessCompletion::Exited, std::string(MiB, 'o'), "");
    exit.exitCode = 0;
    exit.standardOutput.extent = vtpty::OutputExtent::Truncated;
    fixture.runner.complete(0, std::move(exit));
    auto const listed = fixture.processes->list();
    REQUIRE(listed.front().result.has_value());
    CHECK(listed.front().result->stdoutExtent == OutputExtent::Truncated);
    CHECK(listed.front().result->stderrExtent == OutputExtent::Whole);
}

TEST_CASE("DeclaredProcesses: both streams together keep at most the process cap", "[daemon][processes]")
{
    auto fixture =
        Fixture { defaultRoots(), ProcessLimits { .streamCap = MiB, .processCap = MiB + (MiB / 2) } };
    auto const handle = fixture.start("gh-prs", {}, "/r/contour");
    REQUIRE(handle.has_value());
    fixture.runner.exitWith(0, std::string(MiB, 'o'), std::string(MiB, 'e'));
    auto const listed = fixture.processes->list();
    REQUIRE(listed.front().result.has_value());
    CHECK(listed.front().result->stdoutBytes == MiB);
    CHECK(listed.front().result->stdoutExtent == OutputExtent::Whole);
    CHECK(listed.front().result->stderrBytes == MiB / 2);
    CHECK(listed.front().result->stderrExtent == OutputExtent::Truncated);
    CHECK(fixture.processes->take(*handle, ProcStream::Stderr, MiB / 2) == OutputPage {});
}

TEST_CASE("DeclaredProcesses: results are kept until released, oldest first past the total",
          "[daemon][processes]")
{
    auto fixture = Fixture {};
    auto handles = std::vector<ProcessHandle> {};
    for (auto const index: std::views::iota(std::size_t { 0 }, std::size_t { 9 }))
    {
        auto const handle = fixture.start("gh-prs", {}, "/r/contour");
        REQUIRE(handle.has_value());
        handles.push_back(*handle);
        fixture.runner.exitWith(index, std::string(MiB, 'o'), std::string(MiB, 'e'));
    }
    auto listed = fixture.processes->list();
    REQUIRE(listed.size() == 8);
    CHECK(listed.front().handle == handles.at(1));
    CHECK(fixture.processes->take(handles.front(), ProcStream::Stdout, 0)
          == std::unexpected(ProcessError::UnknownHandle));

    REQUIRE(fixture.processes->release(handles.at(1)).has_value());
    CHECK(fixture.processes->list().size() == 7);
    CHECK(fixture.processes->release(handles.at(1)) == std::unexpected(ProcessError::UnknownHandle));
    CHECK(fixture.processes->take(handles.at(1), ProcStream::Stdout, 0)
          == std::unexpected(ProcessError::UnknownHandle));
    CHECK(fixture.runner.cancelled().empty());
}

TEST_CASE("DeclaredProcesses: release stops a running process, destruction the rest", "[daemon][processes]")
{
    auto fixture = Fixture {};
    auto const first = fixture.start("gh-auth", {}, "/r/contour");
    auto const second = fixture.start("gh-auth", {}, "/r/contour");
    REQUIRE(first.has_value());
    REQUIRE(second.has_value());

    REQUIRE(fixture.processes->release(*first).has_value());
    CHECK(fixture.runner.cancelled() == std::vector { fixture.runner.started().at(0).token });
    fixture.runner.complete(0, exitOf(vtpty::ProcessCompletion::Cancelled, "", ""));
    CHECK(fixture.finished == 0);
    CHECK(fixture.processes->list().size() == 1);

    fixture.processes.reset();
    CHECK(fixture.runner.cancelled().size() == 2);
    CHECK(fixture.runner.cancelled().back() == fixture.runner.started().at(1).token);
    fixture.runner.complete(1, exitOf(vtpty::ProcessCompletion::Cancelled, "", ""));
    CHECK(fixture.finished == 0);
}

TEST_CASE("DeclaredProcesses: tags and the hint", "[daemon][processes]")
{
    auto fixture = Fixture {};
    auto const prs = fixture.start("gh-prs", {}, "/r/contour", "poll");
    auto const status = fixture.start("git-status", {}, "/w/contour/pr-1", "run:pr-1");
    REQUIRE(prs.has_value());
    REQUIRE(status.has_value());

    CHECK(fixture.processes->list()
          == std::vector<ProcessInfo> {
              { .handle = *prs, .tag = "poll", .command = "gh-prs", .result = std::nullopt },
              { .handle = *status, .tag = "run:pr-1", .command = "git-status", .result = std::nullopt },
          });
    fixture.runner.exitWith(1, "");
    CHECK(fixture.finished == 1);
    fixture.runner.exitWith(0, "[]");
    CHECK(fixture.finished == 2);
}

TEST_CASE("DeclaredProcesses: roots are checked at creation", "[daemon][processes]")
{
    auto runner = FakeProcessRunner {};
    auto directories = FakeDirectoryOpener {};
    auto grants = FakeGrantStore {};
    auto const make = [&](std::vector<ResolvedRoot> roots) {
        return DeclaredProcesses::create(
            configWith(std::move(roots)),
            DeclaredProcessesServices {
                .runner = runner, .directories = directories, .grants = grants, .onFinished = {} });
    };
    // worktree_root configured as the repository itself.
    CHECK(make({ { .label = "worktrees", .paths = { "/r/contour" } },
                 { .label = "repos", .paths = { "/r/contour" } } })
              .error()
          == RootError::EqualRoots);
    CHECK(make({ { .label = "repos", .paths = { "/r/contour" } } }).error() == RootError::Unresolved);
    CHECK(make(defaultRoots()).has_value());
}

TEST_CASE("DeclaredProcesses: a path argument is strictly below its root, its parents made by handle",
          "[daemon][processes]")
{
    auto fixture = Fixture {};
    CHECK(fixture.start("git-worktree-add", { { "path", "/w" }, { "base", "main" } }, "/r/contour")
          == std::unexpected(ProcessError::ArgumentRejected));
    CHECK(fixture.directories.created.empty());

    REQUIRE(fixture
                .start("git-worktree-add",
                       { { "path", "/w/newrepo/deeper/pr-3" }, { "base", "main" } },
                       "/r/contour")
                .has_value());
    CHECK(fixture.runner.started().back().spec.argv.at(4) == "/w/newrepo/deeper/pr-3");
    // Every missing directory above the leaf is created; the leaf is left to git.
    CHECK(fixture.directories.created == std::vector<std::string> { "/w/newrepo", "/w/newrepo/deeper" });
}

TEST_CASE("DeclaredProcesses: a create root behind a symlink into another root is never created",
          "[daemon][processes]")
{
    auto const roots = std::vector<ResolvedRoot> { { .label = "worktrees", .paths = { "/x/wt" } },
                                                   { .label = "repos", .paths = { "/r/contour" } } };
    auto fixture = Fixture { roots };
    fixture.directories.link("/x", "/r/contour");

    CHECK(ensureRoot(fixture.directories, roots, "worktrees")
          == std::unexpected(RootError::CreateInsideRoot));
    CHECK(fixture.start("git-status", {}, "/x/wt") == std::unexpected(ProcessError::CwdMissing));
    CHECK(fixture.directories.created.empty());
}

TEST_CASE("DeclaredProcesses: a root that cannot be created is no directory to run in, never gone",
          "[daemon][processes]")
{
    auto fixture = Fixture {};
    // `/w` is missing, and the system refuses to create it.
    fixture.directories.directories.erase("/w");
    fixture.directories.createFailures.emplace("/w", vtpty::DirectoryError::AccessDenied);
    CHECK(fixture.start("git-status", {}, "/w") == std::unexpected(ProcessError::CwdNotADirectory));
    CHECK(fixture.directories.created.empty());

    // A path argument's parent refused the same way.
    auto other = Fixture {};
    other.directories.createFailures.emplace("/w/newrepo", vtpty::DirectoryError::NotADirectory);
    CHECK(other.start("git-worktree-add", { { "path", "/w/newrepo/pr-3" }, { "base", "main" } }, "/r/contour")
          == std::unexpected(ProcessError::CwdNotADirectory));
    CHECK(other.processes->list().empty());
}

TEST_CASE("DeclaredProcesses: destruction waits for a running hint", "[daemon][processes][threading]")
{
    auto fixture = Fixture {};
    REQUIRE(fixture.start("gh-auth", {}, "/r/contour").has_value());
    // Still running when the service is destroyed: its cancel() tells that the destructor has begun.
    REQUIRE(fixture.start("gh-auth", {}, "/r/contour").has_value());

    auto entered = std::latch { 1 };
    auto release = std::latch { 1 };
    auto destructorBegun = std::latch { 1 };
    auto destroyed = std::atomic<bool> { false };
    auto destroyedDuringHint = std::atomic<bool> { false };
    fixture.hint = [&] {
        entered.count_down();
        release.wait();
        // Gives a destructor that does not wait every chance to return while the hint still runs.
        for ([[maybe_unused]] auto const _: std::views::iota(0, 200'000))
        {
            if (destroyed.load())
                break;
            std::this_thread::yield();
        }
        destroyedDuringHint = destroyed.load();
    };
    fixture.runner.onCancel = [&](vtpty::ProcessToken) {
        destructorBegun.count_down();
    };

    auto completing = std::jthread { [&] { fixture.runner.exitWith(0, "ok"); } };
    entered.wait();
    auto destroying = std::jthread { [&] {
        fixture.processes.reset();
        destroyed = true;
    } };
    destructorBegun.wait();
    release.count_down();
    completing.join();
    destroying.join();

    CHECK_FALSE(destroyedDuringHint.load());
    CHECK(destroyed.load());
    CHECK(fixture.finished == 1);
}

TEST_CASE("DeclaredProcesses: a refused start creates nothing", "[daemon][processes]")
{
    auto fixture = Fixture {};
    fixture.directories.directories.erase("/w");
    fixture.directories.directories.erase("/w/contour");
    auto const arguments = ConfigMap { { "path", "/w/contour/pr-1-20261115-120000" }, { "base", "main" } };

    // In the wrong directory.
    fixture.directories.add("/elsewhere");
    CHECK(fixture.start("git-worktree-add", arguments, "/elsewhere")
          == std::unexpected(ProcessError::CwdOutsideRoots));
    CHECK(fixture.directories.created.empty());

    // Over the limit.
    fixture.startMany(8);
    CHECK(fixture.start("git-worktree-add", arguments, "/r/contour")
          == std::unexpected(ProcessError::TooMany));
    CHECK(fixture.directories.created.empty());

    // Accepted: the path's root and its parent are created, the leaf is not.
    fixture.runner.exitWith(0, "");
    REQUIRE(fixture.start("git-worktree-add", arguments, "/r/contour").has_value());
    CHECK(fixture.directories.created == std::vector<std::string> { "/w", "/w/contour" });
    CHECK(fixture.runner.started().back().spec.argv.at(4) == "/w/contour/pr-1-20261115-120000");
}

namespace
{

/// A command `who` that takes CLAUDE_CONFIG_DIR and NO_PROXY from the set its `env-set` argument names, and a
/// command `plain` that takes no set.
constexpr auto SetsManifest = "name = sets\n"
                              "version = 2\n"
                              "root worktrees = config:worktree_root\n"
                              "arg env-set = name\n"
                              "arg model = name\n"
                              "command who short @worktrees = claude --model {model}\n"
                              "command plain short @worktrees = claude --model {model}\n"
                              "command-env-set who {env-set} = CLAUDE_CONFIG_DIR NO_PROXY\n"sv;

/// The user's sets: `work`, which holds an unlisted COPILOT_HOME and no NO_PROXY, and `broken`, whose value
/// fails its shape (as a set that bypassed checkEnvironmentSets() would).
[[nodiscard]] std::vector<EnvironmentSet> userSets()
{
    return { EnvironmentSet { .name = "work",
                              .variables = { { .name = "CLAUDE_CONFIG_DIR", .value = "/home/u/.claude-work" },
                                             { .name = "COPILOT_HOME", .value = "/home/u/.copilot" } } },
             EnvironmentSet { .name = "broken",
                              .variables = { { .name = "CLAUDE_CONFIG_DIR", .value = "relative" } } } };
}

/// A Fixture over SetsManifest and userSets(), with NO_PROXY inherited from the daemon.
[[nodiscard]] std::unique_ptr<Fixture> setsFixture()
{
    auto manifest = parseManifestText(SetsManifest);
    REQUIRE(manifest.has_value());
    auto config = configWith(defaultRoots());
    config.manifest = std::move(*manifest);
    config.inheritedEnvironment = { "PATH=/usr/bin", "NO_PROXY=corp.example.com" };
    config.environmentSets = userSets();
    return std::make_unique<Fixture>(std::move(config));
}

} // namespace

TEST_CASE("DeclaredProcesses: start applies the listed variables of the set the call names",
          "[daemon][processes][sets]")
{
    auto const f = setsFixture();
    REQUIRE(f->start("who", { { "model", "opus" }, { "env-set", "work" } }, "/w/contour").has_value());

    REQUIRE(f->runner.started().size() == 1);
    auto const& spec = f->runner.started().front().spec;
    // The set argument is no placeholder: it chooses the set, and the argv does not see it.
    CHECK(spec.argv == std::vector<std::string> { "claude", "--model", "opus" });
    CHECK(valuesOf(spec.environment, "CLAUDE_CONFIG_DIR") == std::vector { "/home/u/.claude-work"sv });
    // Held by the set, but not listed by the command's binding.
    CHECK(valuesOf(spec.environment, "COPILOT_HOME").empty());
    // Listed, but not held by the set: inherited from the daemon as any other variable.
    CHECK(valuesOf(spec.environment, "NO_PROXY") == std::vector { "corp.example.com"sv });
}

TEST_CASE("DeclaredProcesses: start without a set argument applies no set", "[daemon][processes][sets]")
{
    auto const f = setsFixture();
    REQUIRE(f->start("who", { { "model", "opus" } }, "/w/contour").has_value());

    REQUIRE(f->runner.started().size() == 1);
    auto const& spec = f->runner.started().front().spec;
    CHECK(spec.argv == std::vector<std::string> { "claude", "--model", "opus" });
    CHECK(valuesOf(spec.environment, "CLAUDE_CONFIG_DIR").empty());
    CHECK(valuesOf(spec.environment, "NO_PROXY") == std::vector { "corp.example.com"sv });
}

TEST_CASE("DeclaredProcesses: start refuses a set it cannot apply", "[daemon][processes][sets]")
{
    struct Row
    {
        std::string_view what;
        std::string_view command;
        ConfigMap arguments;
    };
    auto const rows = std::vector<Row> {
        { .what = "no such set",
          .command = "who",
          .arguments = { { "model", "opus" }, { "env-set", "gone" } } },
        { .what = "not a name",
          .command = "who",
          .arguments = { { "model", "opus" }, { "env-set", "bad name" } } },
        { .what = "a value failing its shape",
          .command = "who",
          .arguments = { { "model", "opus" }, { "env-set", "broken" } } },
        { .what = "a command without a binding",
          .command = "plain",
          .arguments = { { "model", "opus" }, { "env-set", "work" } } },
        { .what = "the set argument twice",
          .command = "who",
          .arguments = { { "model", "opus" }, { "env-set", "work" }, { "env-set", "work" } } },
    };
    for (auto const& row: rows)
    {
        INFO(row.what);
        auto const f = setsFixture();
        CHECK(f->start(row.command, row.arguments, "/w/contour")
              == std::unexpected(ProcessError::ArgumentRejected));
        CHECK(f->runner.started().empty());
    }
}
