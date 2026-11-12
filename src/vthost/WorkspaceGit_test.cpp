// SPDX-License-Identifier: Apache-2.0
#include <vtbackend/testing/InMemoryGitFileSystem.hpp>

#include <vtpty/MockPty.hpp>

#include <core/async/testing/ManualExecutor.hpp>
#include <core/net/EventLoop.hpp>
#include <core/net/IoBackend.hpp>
#include <core/platform/Clock.hpp>
#include <core/platform/testing/InMemoryFileSystem.hpp>
#include <core/testing/Environment.hpp>
#include <core/testing/ScopedTempDir.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <chrono>
#include <deque>
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
#include <system_error>
#include <tuple>
#include <utility>
#include <vector>

#include <vthost/CommandRunner.hpp>
#include <vthost/SessionHost.hpp>
#include <vthost/WorkspaceGit.hpp>
#include <vthost/testing/FakeCommandRunner.hpp>

#ifndef _WIN32
    #include <vtpty/Process.hpp>
    #include <vtpty/ProcessPlacement.hpp>
    #include <vtpty/test/ProcessTesting.hpp>

    #include <vthost/testing/HandoverTesting.hpp>
#endif

using vthost::GitInfo;
using vthost::WorkspaceGitTracker;
using vtworkspace::WorkspaceId;

namespace
{
/// Answers from a script and records every directory it was asked about.
struct FakeProbe final: vthost::IGitStatusProbe
{
    [[nodiscard]] std::optional<GitInfo> probe(std::filesystem::path const& cwd) const override
    {
        asked.push_back(cwd);
        return answer(cwd);
    }

    mutable std::vector<std::filesystem::path> asked;
    /// By default the answer names the directory, so a test sees whose answer it was told.
    std::function<std::optional<GitInfo>(std::filesystem::path const&)> answer = [](auto const& cwd) {
        return GitInfo { .branch = cwd.filename().string() };
    };
};

/// A tracker whose executor and loop hand-off a test drains by hand.
struct TrackerHarness
{
    FakeProbe probe;
    core::async::testing::ManualExecutor executor;
    std::deque<std::function<void()>> posted;
    std::vector<std::pair<uint64_t, std::optional<GitInfo>>> told;
    WorkspaceGitTracker tracker { probe,
                                  executor,
                                  [this](std::function<void()> job) { posted.push_back(std::move(job)); },
                                  [this](WorkspaceId workspace, std::optional<GitInfo> const& info) {
                                      told.emplace_back(workspace.value, info);
                                  } };

    /// Runs what the executor holds, then what it handed to the loop, until neither has more.
    void settle()
    {
        while (executor.pending() > 0 || !posted.empty())
        {
            executor.drain();
            while (!posted.empty())
            {
                auto job = std::move(posted.front());
                posted.pop_front();
                job();
            }
        }
    }
};
} // namespace

TEST_CASE("parseLeftRightCount reads behind then ahead", "[vthost][git]")
{
    auto const counts = vthost::parseLeftRightCount("3\t7\n");
    REQUIRE(counts.has_value());
    CHECK(counts->first == 3);  // upstream-only commits: behind
    CHECK(counts->second == 7); // HEAD-only commits: ahead
    CHECK(vthost::parseLeftRightCount("0\t0") == std::pair<uint32_t, uint32_t> { 0, 0 });
}

TEST_CASE("garbage parses to nullopt", "[vthost][git]")
{
    for (auto const* text:
         { "", "\n", "3", "3\t", "\t7", "a\tb", "3\t7\t9", "-1\t2", "3\t7x", "99999999999\t1" })
        CHECK_FALSE(vthost::parseLeftRightCount(text).has_value());
}

namespace
{
struct ProbeFixture
{
    core::platform::testing::InMemoryFileSystem files;
    vtbackend::test::InMemoryGitFileSystem git { files };
    vthost::testing::FakeCommandRunner runner;
    std::unique_ptr<vthost::IGitStatusProbe> probe = vthost::makeGitStatusProbe(git, runner);
};
} // namespace

TEST_CASE("the probe asks git for the counts of a branch", "[vthost][git]")
{
    auto f = ProbeFixture {};
    f.files.addFile("/r/.git/HEAD", "ref: refs/heads/main\n");
    f.files.addDirectory("/r/src");
    f.runner.answerAll({ .exitCode = 0, .standardOutput = "2\t5\n" });
    auto const info = f.probe->probe("/r/src");
    REQUIRE(info.has_value());
    CHECK(info->kind == vtbackend::GitHeadKind::Branch);
    CHECK(info->branch == "main");
    CHECK(info->behind == 2);
    CHECK(info->ahead == 5);
    REQUIRE(f.runner.calls.size() == 1);
    CHECK(f.runner.calls.front()
          == std::vector<std::string> {
              "git", "-C", "/r/src", "rev-list", "--left-right", "--count", "@{upstream}...HEAD" });
}

namespace
{
/// Records the deadline every command was run within, and fails any command run without one.
struct DeadlineRecordingRunner final: vthost::CommandRunner
{
    std::vector<std::chrono::milliseconds> deadlines;

    [[nodiscard]] std::expected<vthost::CommandOutcome, std::error_code> run(
        std::span<std::string const> /*argv*/) override
    {
        return std::unexpected(std::make_error_code(std::errc::operation_not_supported));
    }

    [[nodiscard]] std::expected<vthost::CommandOutcome, std::error_code> runWithin(
        std::span<std::string const> /*argv*/, std::chrono::milliseconds deadline) override
    {
        deadlines.push_back(deadline);
        return vthost::CommandOutcome { .exitCode = 0, .standardOutput = "1\t2\n" };
    }
};
} // namespace

TEST_CASE("the probe bounds git to five seconds", "[vthost][git]")
{
    auto files = core::platform::testing::InMemoryFileSystem {};
    auto git = vtbackend::test::InMemoryGitFileSystem { files };
    auto runner = DeadlineRecordingRunner {};
    auto const probe = vthost::makeGitStatusProbe(git, runner);
    files.addFile("/r/.git/HEAD", "ref: refs/heads/main\n");
    auto const info = probe->probe("/r");
    REQUIRE(info.has_value());
    REQUIRE(runner.deadlines.size() == 1);
    CHECK(runner.deadlines.front() == std::chrono::milliseconds { 5000 });
    CHECK(info->behind == 1);
    CHECK(info->ahead == 2);
}

TEST_CASE("a git that outlives its deadline gives a branch without counts", "[vthost][git]")
{
    auto f = ProbeFixture {};
    f.files.addFile("/r/.git/HEAD", "ref: refs/heads/topic\n");
    f.runner.failToStart(std::make_error_code(std::errc::timed_out));
    auto const info = f.probe->probe("/r");
    REQUIRE(info.has_value());
    CHECK(info->branch == "topic");
    CHECK_FALSE(info->behind.has_value());
}

TEST_CASE("no upstream gives a branch without counts", "[vthost][git]")
{
    auto f = ProbeFixture {};
    f.files.addFile("/r/.git/HEAD", "ref: refs/heads/topic\n");
    f.runner.answerAll({ .exitCode = 128, .standardOutput = "" });
    auto const info = f.probe->probe("/r");
    REQUIRE(info.has_value());
    CHECK(info->branch == "topic");
    CHECK_FALSE(info->ahead.has_value());
    CHECK_FALSE(info->behind.has_value());
}

TEST_CASE("a missing git gives a branch without counts", "[vthost][git]")
{
    auto f = ProbeFixture {};
    f.files.addFile("/r/.git/HEAD", "ref: refs/heads/topic\n");
    f.runner.failToStart(std::make_error_code(std::errc::no_such_file_or_directory));
    auto const info = f.probe->probe("/r");
    REQUIRE(info.has_value());
    CHECK(info->branch == "topic");
    CHECK_FALSE(info->ahead.has_value());
}

TEST_CASE("not a repo gives nullopt", "[vthost][git]")
{
    auto f = ProbeFixture {};
    f.files.addDirectory("/plain");
    CHECK_FALSE(f.probe->probe("/plain").has_value());
    CHECK(f.runner.calls.empty());
}

TEST_CASE("detached head gives kind Detached", "[vthost][git]")
{
    auto f = ProbeFixture {};
    f.files.addFile("/r/.git/HEAD", "a1b2c3d4e5f60718293a4b5c6d7e8f9012345678\n");
    auto const info = f.probe->probe("/r");
    REQUIRE(info.has_value());
    CHECK(info->kind == vtbackend::GitHeadKind::Detached);
    CHECK(info->branch == "a1b2c3d");
    CHECK(f.runner.calls.empty()); // a commit has no upstream to count against
}

TEST_CASE("the probe's git runs under an environment that takes no index lock", "[vthost][git]")
{
    auto const environment =
        core::testing::FakeEnvironment { { { "PATH", "/bin" }, { "HOME", "/home/u" }, { "SECRET", "x" } } };
    auto const entries = vthost::gitProbeEnvironment(environment);
    CHECK(entries
          == std::vector<std::string> {
              "PATH=/bin", "HOME=/home/u", "GIT_OPTIONAL_LOCKS=0", "GIT_TERMINAL_PROMPT=0", "GIT_ASKPASS=" });
}

TEST_CASE("the probe runs off the loop thread", "[vthost][git]")
{
    auto h = TrackerHarness {};
    h.tracker.refresh(WorkspaceId { 1 }, "/r");
    // Queued, not run: nothing is probed, and nothing told, until the executor is drained.
    CHECK(h.executor.pending() == 1);
    CHECK(h.probe.asked.empty());
    CHECK(h.told.empty());

    h.executor.drain();
    CHECK(h.probe.asked.size() == 1);
    CHECK(h.told.empty()); // the answer waits for the loop
    REQUIRE(h.posted.size() == 1);
    h.posted.front()();
    REQUIRE(h.told.size() == 1);
    CHECK(h.told.front().first == 1);
    CHECK(h.told.front().second == GitInfo { .branch = "r" });
    CHECK(h.tracker.infoOf(WorkspaceId { 1 }) == GitInfo { .branch = "r" });
}

TEST_CASE("a second refresh while one is in flight probes once more with the newest cwd", "[vthost][git]")
{
    auto h = TrackerHarness {};
    h.tracker.refresh(WorkspaceId { 1 }, "/a");
    h.executor.drain();
    REQUIRE(h.posted.size() == 1);

    // The first probe has run but its answer is not home yet.
    h.tracker.refresh(WorkspaceId { 1 }, "/b");
    h.tracker.refresh(WorkspaceId { 1 }, "/c");
    CHECK(h.executor.pending() == 0); // coalesced: no second probe is queued beside the running one

    h.settle();
    CHECK(h.probe.asked == std::vector<std::filesystem::path> { "/a", "/c" });
    // The stale answer for /a is not told; exactly the newest directory's is.
    REQUIRE(h.told.size() == 1);
    CHECK(h.told.front().second == GitInfo { .branch = "c" });
    CHECK(h.tracker.infoOf(WorkspaceId { 1 }) == GitInfo { .branch = "c" });
}

TEST_CASE("an unchanged result does not notify", "[vthost][git]")
{
    auto h = TrackerHarness {};
    h.tracker.refresh(WorkspaceId { 1 }, "/r");
    h.settle();
    h.tracker.refresh(WorkspaceId { 1 }, "/r");
    h.settle();
    CHECK(h.probe.asked.size() == 2);
    CHECK(h.told.size() == 1);

    h.probe.answer = [](auto const&) {
        return GitInfo { .branch = "next", .ahead = 1 };
    };
    h.tracker.refresh(WorkspaceId { 1 }, "/r");
    h.settle();
    CHECK(h.told.size() == 2);
}

TEST_CASE("leaving a repository is told, and never being in one is not", "[vthost][git]")
{
    auto h = TrackerHarness {};
    h.probe.answer = [](auto const&) {
        return std::nullopt;
    };
    h.tracker.refresh(WorkspaceId { 1 }, "/plain");
    h.settle();
    CHECK(h.told.empty());

    h.probe.answer = [](auto const&) {
        return GitInfo { .branch = "main" };
    };
    h.tracker.refresh(WorkspaceId { 1 }, "/r");
    h.settle();
    h.probe.answer = [](auto const&) {
        return std::nullopt;
    };
    h.tracker.refresh(WorkspaceId { 1 }, "/plain");
    h.settle();
    REQUIRE(h.told.size() == 2);
    CHECK_FALSE(h.told.back().second.has_value());
}

TEST_CASE("a forgotten workspace's answer is dropped", "[vthost][git]")
{
    auto h = TrackerHarness {};
    h.tracker.refresh(WorkspaceId { 1 }, "/r");
    h.executor.drain();
    h.tracker.forget(WorkspaceId { 1 });
    h.settle();
    CHECK(h.told.empty());
    CHECK_FALSE(h.tracker.infoOf(WorkspaceId { 1 }).has_value());
}

TEST_CASE("a tracker that is gone tells nobody", "[vthost][git]")
{
    auto probe = FakeProbe {};
    auto executor = core::async::testing::ManualExecutor {};
    auto posted = std::deque<std::function<void()>> {};
    auto told = 0;
    {
        auto tracker =
            WorkspaceGitTracker { probe,
                                  executor,
                                  [&](std::function<void()> job) { posted.push_back(std::move(job)); },
                                  [&](WorkspaceId, std::optional<GitInfo> const&) { ++told; } };
        tracker.refresh(WorkspaceId { 1 }, "/r");
        executor.drain();
    }
    for (auto& job: posted)
        job();
    CHECK(told == 0);
}

namespace
{
/// A client the host counts as a following window.
struct FollowerClient final: vthost::SessionStreamEvents
{
    [[nodiscard]] bool mirrorsScreens() const noexcept override { return false; }
    [[nodiscard]] bool followsDaemon() const noexcept override { return true; }
};

/// A client that is attached but follows nothing, as a plain terminal attach.
struct PlainClient final: vthost::SessionStreamEvents
{
    [[nodiscard]] bool mirrorsScreens() const noexcept override { return false; }
};

/// Records what the host tells of workspaces' git state.
struct GitRecorder final: vthost::SessionStreamEvents
{
    [[nodiscard]] bool mirrorsScreens() const noexcept override { return false; }
    void workspaceGitChanged(vthost::WorkspaceGitChangedSignal const& signal) override
    {
        seen.push_back(signal);
    }
    std::vector<vthost::WorkspaceGitChangedSignal> seen;
};

struct HostGitFixture
{
    core::platform::ManualClock clock {};
    std::unique_ptr<core::net::IoBackend> source = core::net::makeDefaultBackend();
    core::net::EventLoop loop { *source, clock };
    FakeProbe probe;
    core::async::testing::ManualExecutor executor;
    vthost::SessionHost host { loop,
                               [](vtbackend::PageSize size, std::optional<vtpty::Process::ExecInfo> const&) {
                                   return std::make_unique<vtpty::MockPty>(size);
                               },
                               vtbackend::Settings {},
                               core::defaultEnvironment(),
                               vthost::SessionHostConfig { .pumping = vthost::SessionPumping::Manual,
                                                           .workspaceGit = vthost::WorkspaceGitServices {
                                                               .probe = probe, .executor = executor } } };

    /// Turns the loop and drains the executor until neither has more to do.
    void settle()
    {
        turn();
        while (executor.pending() > 0)
        {
            executor.drain();
            turn();
        }
    }

    /// Turns the loop a few times, as a tick of its timers and what they posted need.
    void turn()
    {
        for ([[maybe_unused]] auto const i: std::views::iota(0, 4))
            std::ignore = loop.runOnce(std::chrono::milliseconds { 0 });
    }
};
} // namespace

namespace
{
/// A host with one followed tab whose shell reported /r/a, its first look-up done.
struct FollowedHost
{
    HostGitFixture f;
    vtworkspace::Tab const* tab = nullptr;
    FollowerClient follower;
    GitRecorder recorder;

    FollowedHost()
    {
        tab = f.host.createTab();
        REQUIRE(tab != nullptr);
        f.host.subscribeStream(&recorder);
        report(*tab, "/r/a");
        f.host.noteClientAttached(&follower);
        f.settle();
    }

    ~FollowedHost()
    {
        f.host.noteClientDetached(&follower);
        f.host.unsubscribeStream(&recorder);
    }

    FollowedHost(FollowedHost const&) = delete;
    FollowedHost(FollowedHost&&) = delete;
    FollowedHost& operator=(FollowedHost const&) = delete;
    FollowedHost& operator=(FollowedHost&&) = delete;

    /// The shell of @p of's active pane reports @p directory (OSC 7), and what that queued is settled.
    void report(vtworkspace::Tab const& of, std::string_view directory)
    {
        auto* terminal = f.host.terminal(of.activePane()->session());
        REQUIRE(terminal != nullptr);
        terminal->writeToScreen(std::format("\033]7;file://{}\033\\", directory));
        f.settle();
    }
};
} // namespace

TEST_CASE("a workspace is looked up while no window follows", "[vthost][git][host]")
{
    auto f = HostGitFixture {};
    auto const* tab = f.host.createTab();
    REQUIRE(tab != nullptr);
    auto* terminal = f.host.terminal(tab->activePane()->session());
    REQUIRE(terminal != nullptr);
    terminal->writeToScreen("\033]7;file:///r/a\033\\");
    f.settle();
    CHECK(f.probe.asked == std::vector<std::filesystem::path> { "/r/a" });
    CHECK(f.host.gitInfoOf(f.host.workspaceId()).has_value());
}

TEST_CASE("switching the active tab queues a probe", "[vthost][git][host]")
{
    auto h = FollowedHost {};
    auto const first = h.tab->id();
    auto const* second = h.f.host.createTab();
    REQUIRE(second != nullptr);
    h.report(*second, "/r/b");
    h.f.probe.asked.clear();
    REQUIRE(h.f.executor.pending() == 0);

    h.f.host.model().activateTab(h.f.host.workspaceId(), first);
    CHECK(h.f.executor.pending() == 1);
    h.f.settle();
    CHECK(h.f.probe.asked == std::vector<std::filesystem::path> { "/r/a" });
    CHECK(h.f.host.gitInfoOf(h.f.host.workspaceId()) == GitInfo { .branch = "a" });
}

TEST_CASE("switching the active pane queues a probe", "[vthost][git][host]")
{
    auto h = FollowedHost {};
    auto const firstLeaf = h.tab->activePane()->id();
    h.f.host.splitActivePane(h.tab->id(), vtworkspace::SplitState::Vertical);
    REQUIRE(h.tab->activePane()->id() != firstLeaf);
    h.report(*h.tab, "/r/b"); // the new pane's shell
    h.f.probe.asked.clear();
    REQUIRE(h.f.executor.pending() == 0);

    h.f.host.model().setActivePane(h.tab->id(), firstLeaf);
    CHECK(h.f.executor.pending() == 1);
    h.f.settle();
    CHECK(h.f.probe.asked == std::vector<std::filesystem::path> { "/r/a" });
}

TEST_CASE("a finished command on the active pane queues a probe", "[vthost][git][host]")
{
    auto h = FollowedHost {};
    h.f.probe.asked.clear();
    REQUIRE(h.f.executor.pending() == 0);
    auto* terminal = h.f.host.terminal(h.tab->activePane()->session());
    REQUIRE(terminal != nullptr);
    terminal->writeToScreen("\033]133;A\007$ \033]133;B\007make\r\n\033]133;C\007out\r\n\033]133;D;0\007");
    h.f.turn();
    CHECK(h.f.executor.pending() == 1);
    h.f.settle();
    CHECK(h.f.probe.asked == std::vector<std::filesystem::path> { "/r/a" });
}

TEST_CASE("a workspace discarded while its probe runs is told of nobody", "[vthost][git][host]")
{
    auto h = FollowedHost {};
    auto* other = h.f.host.createWorkspace();
    REQUIRE(other != nullptr);
    auto const otherId = other->id();
    auto const tabId = other->activeTab()->id();
    h.report(*other->activeTab(), "/r/z");
    h.recorder.seen.clear();

    // A probe queued for it, then the workspace goes before the answer is taken.
    h.f.host.refreshWorkspaceGit(otherId);
    REQUIRE(h.f.executor.pending() == 1);
    h.f.host.model().closeTab(otherId, tabId);
    h.f.host.discardWorkspace(otherId);
    h.f.settle();
    CHECK(h.recorder.seen.empty());
    CHECK_FALSE(h.f.host.gitInfoOf(otherId).has_value());
}

TEST_CASE("the 30 s timer runs once a workspace's directory is known, follower or not", "[vthost][git][host]")
{
    auto f = HostGitFixture {};
    auto const* tab = f.host.createTab();
    REQUIRE(tab != nullptr);
    // Until a directory is known there is nothing to look up, and the timer stays off.
    CHECK_FALSE(f.host.pollsWorkspaceGit());

    // A client that follows nothing, and one that follows, change nothing about it.
    auto plain = PlainClient {};
    f.host.noteClientAttached(&plain);
    auto follower = FollowerClient {};
    f.host.noteClientAttached(&follower);
    f.host.noteClientDetached(&follower);
    CHECK_FALSE(f.host.pollsWorkspaceGit());

    auto* terminal = f.host.terminal(tab->activePane()->session());
    REQUIRE(terminal != nullptr);
    terminal->writeToScreen("\033]7;file:///r/a\033\\");
    f.settle();
    CHECK(f.host.pollsWorkspaceGit());

    // It re-arms itself after each tick, and probes again.
    f.probe.asked.clear();
    f.clock.advance(vthost::WorkspaceGitPollInterval + std::chrono::seconds { 1 });
    f.settle();
    CHECK(f.host.pollsWorkspaceGit());
    CHECK(f.probe.asked == std::vector<std::filesystem::path> { "/r/a" });
    f.host.noteClientDetached(&plain);
}

TEST_CASE("a tick of the timer probes the active pane's directory and tells what changed",
          "[vthost][git][host]")
{
    auto f = HostGitFixture {};
    auto const* tab = f.host.createTab();
    REQUIRE(tab != nullptr);
    auto const workspace = f.host.workspaceId();
    auto recorder = GitRecorder {};
    f.host.subscribeStream(&recorder);
    REQUIRE_FALSE(f.host.gitInfoOf(workspace).has_value());

    // The shell reports where it is, as OSC 7 does; MockPty has no process to ask.
    auto* terminal = f.host.terminal(tab->rootPane()->session());
    REQUIRE(terminal != nullptr);
    terminal->writeToScreen("\033]7;file:///r/src\033\\");

    auto follower = FollowerClient {};
    f.host.noteClientAttached(&follower); // a fresh follower is brought up to date at once
    CHECK(f.executor.pending() == 1);
    f.settle();
    REQUIRE(recorder.seen.size() == 1);
    CHECK(recorder.seen.front().workspace == workspace);
    CHECK(recorder.seen.front().git == GitInfo { .branch = "src" });
    // Every probe asked for the directory the shell reported (the report itself asked for one more).
    CHECK(std::ranges::all_of(f.probe.asked, [](auto const& asked) { return asked == "/r/src"; }));
    CHECK(f.host.gitInfoOf(workspace) == GitInfo { .branch = "src" });

    // An unchanged answer at the next tick is not told again.
    auto const probesBefore = f.probe.asked.size();
    f.clock.advance(vthost::WorkspaceGitPollInterval + std::chrono::seconds { 1 });
    f.settle();
    CHECK(f.probe.asked.size() == probesBefore + 1);
    CHECK(recorder.seen.size() == 1);
    f.host.noteClientDetached(&follower);
    f.host.unsubscribeStream(&recorder);
}

#ifndef _WIN32
namespace
{
/// Holds every child it is handed between fork() and exec(), still in this process's directory, until it is
/// let go.
class ParkingPlacement final: public vtpty::ProcessPlacement
{
  public:
    void placeThenRelease(vtpty::ParkedChild child) noexcept override
    {
        auto const _ = std::lock_guard { _mutex };
        _parked.push_back(std::move(child));
    }

    [[nodiscard]] std::optional<int> childOomScoreAdjust() const noexcept override { return std::nullopt; }

    /// @return How many children it holds.
    [[nodiscard]] std::size_t parked() const
    {
        auto const _ = std::lock_guard { _mutex };
        return _parked.size();
    }

    /// Lets every child it holds go on to exec its program.
    void releaseAll()
    {
        auto const _ = std::lock_guard { _mutex };
        for (auto& child: _parked)
            child.release();
    }

  private:
    mutable std::mutex _mutex;
    std::vector<vtpty::ParkedChild> _parked;
};

/// Lets every child of a placement go as it goes out of scope. Declared after the host, so it runs first: the
/// host's teardown waits for its children, which a parked one would keep it doing forever.
class ReleaseOnExit
{
  public:
    explicit ReleaseOnExit(ParkingPlacement& placement): _placement { placement } {}
    ReleaseOnExit(ReleaseOnExit const&) = delete;
    ReleaseOnExit& operator=(ReleaseOnExit const&) = delete;
    ReleaseOnExit(ReleaseOnExit&&) = delete;
    ReleaseOnExit& operator=(ReleaseOnExit&&) = delete;
    ~ReleaseOnExit() { _placement.releaseAll(); }

  private:
    ParkingPlacement& _placement;
};
} // namespace

TEST_CASE("a new shell's workspace is looked up where the shell starts and not where the daemon runs",
          "[vthost][git][host][threading]")
{
    // The look-up the new tab starts comes while the shell's child is still between fork() and exec(), in the
    // daemon's own directory: it must not name that one -- a daemon started from a repository would state
    // that repository's branch for a workspace in another, until the next poll.
    auto const directory = core::testing::ScopedTempDir { "contour-git" };
    auto const start = std::filesystem::canonical(directory.path());
    REQUIRE(start != std::filesystem::canonical(std::filesystem::current_path()));
    auto probe = FakeProbe {};
    auto executor = core::async::testing::ManualExecutor {};
    auto placement = std::make_shared<ParkingPlacement>();
    auto live = vthost::testing::LiveHost {
        [&](vtpty::PageSize size) -> std::unique_ptr<vtpty::Pty> {
            auto exec = vtpty::testing::execInfo("/bin/sh", { "-c", vthost::testing::CatScript });
            exec.workingDirectory = start;
            return std::make_unique<vtpty::Process>(
                exec, vtpty::createPty(size, std::nullopt), /*escapeSandbox=*/false, placement);
        },
        vthost::SessionHostConfig {
            .workspaceGit = vthost::WorkspaceGitServices { .probe = probe, .executor = executor } }
    };
    auto const release = ReleaseOnExit { *placement };

    REQUIRE(live.host.createTab() != nullptr);
    REQUIRE(live.eventually([&] { return placement->parked() == 1; }));
    live.host.refreshWorkspaceGit(live.host.workspaceId());
    executor.drain();
    REQUIRE_FALSE(probe.asked.empty());
    CHECK(std::ranges::all_of(probe.asked, [&](auto const& asked) { return asked == start; }));
}
#endif
