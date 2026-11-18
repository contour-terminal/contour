// SPDX-License-Identifier: Apache-2.0
//
// The declared-process policy over a fake runner and a fake filesystem, with the orchestrator's manifest from
// the conformance corpus: what is refused and why, where commands run, what they see, and how their results
// are paged, kept and released.
#include <catch2/catch_message.hpp>
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <expected>
#include <filesystem>
#include <functional>
#include <latch>
#include <memory>
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
    explicit Fixture(std::vector<ResolvedRoot> roots = defaultRoots(), ProcessLimits limits = {})
    {
        for (auto const* path: { "/w", "/w/contour", "/w/contour/pr-1", "/r/contour" })
            directories.add(path);
        grants.record(HostId, GrantDigest, GrantState::Granted);
        auto created = DeclaredProcesses::create(
            configWith(std::move(roots), limits),
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
