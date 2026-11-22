// SPDX-License-Identifier: Apache-2.0
#include <vtpty/DirectoryHandle.hpp>
#include <vtpty/ProcessRunner.hpp>

#include <core/platform/Clock.hpp>
#include <core/testing/EnvHelper.hpp>
#include <core/testing/ScopedTempDir.hpp>

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include <algorithm>
#include <cerrno>
#include <charconv>
#include <chrono>
#include <csignal>
#include <cstddef>
#include <filesystem>
#include <format>
#include <fstream>
#include <future>
#include <map>
#include <memory>
#include <optional>
#include <ranges>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#ifndef _WIN32
    #include <sys/types.h>
    #include <sys/wait.h>

    #include <poll.h>
#endif

using namespace std::chrono_literals;
using core::platform::SteadyTimePoint;
using vtpty::PipedProcessRunner;
using vtpty::ProcessCompletion;
using vtpty::ProcessExit;
using vtpty::ProcessSpec;
using vtpty::ProcessToken;

#ifndef _WIN32

namespace
{

constexpr auto OneMebibyte = std::size_t { 1024 } * 1024;

/// @return A PipedProcessRunner on @p clock.
[[nodiscard]] std::unique_ptr<PipedProcessRunner> makeRunner(core::platform::IClock& clock,
                                                             vtpty::ProcessRunnerTuning tuning = {})
{
    auto runner = PipedProcessRunner::create(clock, tuning);
    REQUIRE(runner.has_value());
    return std::move(*runner);
}

/// @return The pid a script wrote to @p file; std::nullopt until it has.
[[nodiscard]] std::optional<pid_t> readPid(std::filesystem::path const& file)
{
    auto text = std::string {};
    std::getline(std::ifstream { file }, text);
    auto pid = pid_t {};
    if (text.empty() || std::from_chars(text.data(), text.data() + text.size(), pid).ec != std::errc {})
        return std::nullopt;
    return pid;
}

/// @return Whether @p pid, no child of ours, is dead: gone, or (on Linux) a zombie its new parent has not
///         reaped yet. Waits a bounded time for a signal already sent to take effect.
[[nodiscard]] bool isDead(pid_t pid)
{
    auto const deadNow = [pid] {
        if (::kill(pid, 0) != 0 && errno == ESRCH)
            return true;
        auto stat = std::string {};
        std::getline(std::ifstream { std::format("/proc/{}/stat", pid) }, stat);
        auto const state = stat.rfind(") ");
        return state != std::string::npos && state + 2 < stat.size() && stat[state + 2] == 'Z';
    };
    for ([[maybe_unused]] auto const slice: std::views::iota(0, 500))
    {
        if (deadNow())
            return true;
        ::poll(nullptr, 0, 10);
    }
    return false;
}

/// A PipedProcessRunner on a ManualClock, the temporary directory its processes run in, and what each
/// process's handler was called with.
struct RunnerFixture
{
    RunnerFixture() = default;

    core::testing::ScopedTempDir directory { "contour-processrunner" };
    core::platform::ManualClock clock;
    vtpty::ProcessRunnerTuning tuning {};
    std::unique_ptr<PipedProcessRunner> runner = makeRunner(clock, tuning);
    std::map<ProcessToken, ProcessExit> finished;

    /// @return A spec running @p argv in the fixture's directory, under PATH=/usr/bin:/bin alone.
    [[nodiscard]] ProcessSpec spec(std::vector<std::string> argv,
                                   std::chrono::milliseconds timeout = 30s) const
    {
        auto opened = vtpty::nativeDirectoryOpener().open(directory.path().string());
        REQUIRE(opened.has_value());
        return ProcessSpec { .argv = std::move(argv),
                             .workingDirectory = std::move(*opened),
                             .environment = { { "PATH", "/usr/bin:/bin" } },
                             .timeout = timeout,
                             .outputCap = OneMebibyte };
    }

    /// Pumps until @p condition holds, failing after 200 rounds.
    template <typename Condition>
    void pumpUntil(Condition condition)
    {
        for ([[maybe_unused]] auto const round: std::views::iota(0, 200))
        {
            if (condition())
                return;
            runner->pump(100ms);
        }
        REQUIRE(condition());
    }

    /// @return The pid the script started with @p file as its "$1" wrote there, pumping until it has.
    [[nodiscard]] pid_t awaitPid(std::filesystem::path const& file)
    {
        pumpUntil([&] { return readPid(file).has_value(); });
        return *readPid(file);
    }

    /// Starts @p spec, recording its exit in finished.
    [[nodiscard]] ProcessToken start(ProcessSpec spec)
    {
        auto token = runner->start(std::move(spec), [this](ProcessToken token, ProcessExit exit) {
            finished.emplace(token, std::move(exit));
        });
        REQUIRE(token.has_value());
        return *token;
    }

    /// Pumps until @p token's handler has been called, failing after @p rounds rounds.
    /// @param eachRound Runs before every round.
    /// @param rounds How many rounds it may take; one round reads at most a pipe's buffer per stream.
    /// @return What the handler was called with.
    template <typename Round = decltype([] {})>
    [[nodiscard]] ProcessExit runToCompletion(ProcessToken token, Round eachRound = {}, int rounds = 200)
    {
        for ([[maybe_unused]] auto const round: std::views::iota(0, rounds))
        {
            if (auto const done = finished.find(token); done != finished.end())
                return done->second;
            eachRound();
            runner->pump(100ms);
        }
        FAIL("the process did not complete in time");
        return {};
    }

    /// @return What @p argv's process ended with, run to completion.
    [[nodiscard]] ProcessExit run(std::vector<std::string> argv)
    {
        return runToCompletion(start(spec(std::move(argv))));
    }
};

/// @return Whether a process with @p pid exists (a zombie included).
[[nodiscard]] bool processExists(pid_t pid)
{
    return ::kill(pid, 0) == 0 || errno != ESRCH;
}

} // namespace

TEST_CASE("PipedProcessRunner: exit codes", "[processrunner]")
{
    auto fixture = RunnerFixture {};
    auto const ok = fixture.run({ "sh", "-c", "exit 0" });
    CHECK(ok.completion == ProcessCompletion::Exited);
    CHECK(ok.exitCode == 0);
    CHECK_FALSE(ok.signal.has_value());

    auto const three = fixture.run({ "sh", "-c", "exit 3" });
    CHECK(three.completion == ProcessCompletion::Exited);
    CHECK(three.exitCode == 3);
    CHECK(fixture.runner->running() == 0);
}

TEST_CASE("PipedProcessRunner: both streams", "[processrunner]")
{
    auto fixture = RunnerFixture {};
    auto const exit = fixture.run({ "sh", "-c", "echo out; echo err >&2" });
    CHECK(exit.completion == ProcessCompletion::Exited);
    CHECK(exit.standardOutput.bytes == "out\n");
    CHECK(exit.standardOutput.extent == vtpty::OutputExtent::Whole);
    CHECK(exit.standardError.bytes == "err\n");
    CHECK(exit.standardError.extent == vtpty::OutputExtent::Whole);
}

TEST_CASE("PipedProcessRunner: output past the cap is drained and cut", "[processrunner]")
{
    auto fixture = RunnerFixture {};
    auto const exit = fixture.run({ "head", "-c", "2097152", "/dev/zero" });
    CHECK(exit.completion == ProcessCompletion::Exited);
    CHECK(exit.exitCode == 0);
    CHECK(exit.standardOutput.bytes.size() == OneMebibyte);
    CHECK(exit.standardOutput.extent == vtpty::OutputExtent::Truncated);
    CHECK(exit.standardError.extent == vtpty::OutputExtent::Whole);
}

TEST_CASE("PipedProcessRunner: a timeout terminates", "[processrunner]")
{
    auto fixture = RunnerFixture {};
    auto const pidFile = fixture.directory.path() / "pid";
    auto const token = fixture.start(
        fixture.spec({ "sh", "-c", R"(echo $$ > "$1"; exec sleep 1000)", "sh", pidFile.string() }));
    auto const pid = fixture.awaitPid(pidFile);
    CHECK(fixture.runner->running() == 1);

    fixture.clock.advance(30s + 1ms);
    // The leader ends on SIGTERM: the group's SIGKILL follows at once, not at the end of the grace, which the
    // clock never reaches here.
    auto const exit = fixture.runToCompletion(token);
    CHECK(fixture.clock.now() < SteadyTimePoint {} + 30s + 1ms + fixture.tuning.terminateGrace);
    CHECK(isDead(pid));
    CHECK(exit.completion == ProcessCompletion::TimedOut);
    CHECK(exit.signal == SIGTERM);
    CHECK_FALSE(exit.exitCode.has_value());
}

TEST_CASE("PipedProcessRunner: a timeout kills a grandchild that ignores SIGTERM and holds no pipe",
          "[processrunner]")
{
    auto fixture = RunnerFixture {};
    std::ofstream { fixture.directory.path() / "stubborn.sh" }
        << "trap '' TERM\n: > \"$1\"\nexec sleep 1000\n";
    auto const pidFile = fixture.directory.path() / "pid";
    auto const ready = fixture.directory.path() / "ready";
    auto const token = fixture.start(
        fixture.spec({ "sh",
                       "-c",
                       R"(sh stubborn.sh "$2" </dev/null >/dev/null 2>&1 & echo $! > "$1"; wait)",
                       "sh",
                       pidFile.string(),
                       ready.string() }));
    auto const grandchild = fixture.awaitPid(pidFile);
    fixture.pumpUntil([&] { return std::filesystem::exists(ready); });

    fixture.clock.advance(30s + 1ms);
    fixture.runner->pump(0ms);
    fixture.clock.advance(fixture.tuning.terminateGrace);
    auto const exit = fixture.runToCompletion(token);
    CHECK(exit.completion == ProcessCompletion::TimedOut);
    CHECK(exit.signal == SIGTERM);
    CHECK(isDead(grandchild));
}

TEST_CASE("PipedProcessRunner: a child ignoring SIGTERM is killed after the grace", "[processrunner]")
{
    auto fixture = RunnerFixture {};
    // The pid file says the trap is set: SIGTERM must not land before it is.
    auto const ready = fixture.directory.path() / "ready";
    auto const token = fixture.start(fixture.spec(
        { "sh", "-c", "trap '' TERM; echo > \"$1\"; exec sleep 1000", "sh", ready.string() }, 30s));
    for ([[maybe_unused]] auto const round: std::views::iota(0, 200))
    {
        if (std::filesystem::exists(ready))
            break;
        fixture.runner->pump(100ms);
    }
    REQUIRE(std::filesystem::exists(ready));

    fixture.clock.advance(30s + 1ms);
    fixture.runner->pump(0ms);
    CHECK(fixture.runner->running() == 1);

    fixture.clock.advance(fixture.tuning.terminateGrace + 1ms);
    auto const exit = fixture.runToCompletion(token);
    CHECK(exit.completion == ProcessCompletion::TimedOut);
    CHECK(exit.signal == SIGKILL);
}

TEST_CASE("PipedProcessRunner: destruction kills and reaps the process group", "[processrunner]")
{
    auto fixture = RunnerFixture {};
    auto const pidFile = fixture.directory.path() / "pid";
    std::ignore = fixture.start(
        fixture.spec({ "sh", "-c", R"(sleep 1000 & echo $! > "$1"; wait)", "sh", pidFile.string() }));
    auto const pid = fixture.awaitPid(pidFile);
    REQUIRE(processExists(pid));

    fixture.runner.reset();
    // The grandchild, not only the leader, is gone -- and reaped, not left a zombie.
    CHECK(::kill(pid, 0) == -1);
    CHECK(errno == ESRCH);
    CHECK(fixture.finished.empty());
}

TEST_CASE("PipedProcessRunner: cancel", "[processrunner]")
{
    auto fixture = RunnerFixture {};
    auto const pidFile = fixture.directory.path() / "pid";
    auto const token = fixture.start(
        fixture.spec({ "sh", "-c", R"(echo $$ > "$1"; exec sleep 1000)", "sh", pidFile.string() }));
    std::ignore = fixture.awaitPid(pidFile);
    fixture.runner->cancel(token);
    // The leader ends on SIGTERM, and the process completes before the clock reaches the end of the grace.
    auto const exit = fixture.runToCompletion(token);
    CHECK(exit.completion == ProcessCompletion::Cancelled);
    CHECK(exit.signal == SIGTERM);

    // A token that has completed is ignored.
    fixture.runner->cancel(token);
    CHECK(fixture.runner->running() == 0);
}

TEST_CASE("PipedProcessRunner: a leader reaped by someone else completes with its status lost",
          "[processrunner]")
{
    auto fixture = RunnerFixture {};
    auto const pidFile = fixture.directory.path() / "pid";
    auto const token = fixture.start(
        fixture.spec({ "sh", "-c", R"(echo $$ > "$1"; exec sleep 1000)", "sh", pidFile.string() }));
    auto const pid = fixture.awaitPid(pidFile);
    REQUIRE(::kill(pid, SIGKILL) == 0);
    auto status = 0;
    REQUIRE(::waitpid(pid, &status, 0) == pid);

    auto const exit = fixture.runToCompletion(token);
    CHECK(exit.completion == ProcessCompletion::StatusLost);
    CHECK_FALSE(exit.exitCode.has_value());
    CHECK_FALSE(exit.signal.has_value());
    CHECK(fixture.runner->running() == 0);
}

TEST_CASE("PipedProcessRunner: cancelling a process whose leader was reaped elsewhere completes it at once",
          "[processrunner]")
{
    auto fixture = RunnerFixture {};
    auto const pidFile = fixture.directory.path() / "pid";
    auto const background = fixture.directory.path() / "background";
    // The background sleep holds the pipes once the leader is gone.
    auto const token =
        fixture.start(fixture.spec({ "sh",
                                     "-c",
                                     R"(sleep 1000 & echo $! > "$2"; echo $$ > "$1"; exec sleep 1000)",
                                     "sh",
                                     pidFile.string(),
                                     background.string() }));
    auto const pid = fixture.awaitPid(pidFile);
    auto const holder = fixture.awaitPid(background);
    REQUIRE(::kill(pid, SIGKILL) == 0);
    auto status = 0;
    REQUIRE(::waitpid(pid, &status, 0) == pid);
    fixture.runner->pump(0ms);

    fixture.runner->cancel(token);
    fixture.runner->pump(0ms);
    auto const done = fixture.finished.find(token);
    REQUIRE(done != fixture.finished.end());
    CHECK(done->second.completion == ProcessCompletion::Cancelled);
    // Nothing was signalled: the group's id was free once its leader was reaped. The test ends what is left.
    CHECK(::kill(holder, SIGKILL) == 0);
}

TEST_CASE("PipedProcessRunner: a discarded stream stays empty however much is written", "[processrunner]")
{
    auto fixture = RunnerFixture {};
    auto spec = fixture.spec({ "sh", "-c", "head -c 67108864 /dev/zero >&2; echo done" });
    spec.standardError = vtpty::StreamCapture::Discard;
    // 64 MiB through a 64 KiB pipe buffer: a thousand rounds and more.
    auto const exit = fixture.runToCompletion(fixture.start(std::move(spec)), [] {}, 5000);
    CHECK(exit.completion == ProcessCompletion::Exited);
    CHECK(exit.exitCode == 0);
    CHECK(exit.standardOutput.bytes == "done\n");
    CHECK(exit.standardError.bytes.empty());
    CHECK(exit.standardError.extent == vtpty::OutputExtent::Truncated);
}

TEST_CASE("PipedProcessRunner: a child has no controlling terminal", "[processrunner]")
{
    // In a session of its own, /dev/tty cannot be opened: a read of it fails rather than stopping the child.
    auto fixture = RunnerFixture {};
    auto const exit = fixture.run({ "sh", "-c", "read line </dev/tty; echo $?" });
    CHECK(exit.completion == ProcessCompletion::Exited);
    CHECK_FALSE(exit.standardOutput.bytes.empty());
    CHECK(exit.standardOutput.bytes != "0\n");
}

TEST_CASE("PipedProcessRunner: a relative program is found in the directory entered by handle",
          "[processrunner]")
{
    auto fixture = RunnerFixture {};
    auto const writeProgram = [](std::filesystem::path const& directory, std::string_view which) {
        std::filesystem::create_directories(directory);
        auto const file = directory / "prog";
        std::ofstream { file } << std::format("#!/bin/sh\necho prog-{}\n", which);
        std::filesystem::permissions(file, std::filesystem::perms::owner_all);
    };
    auto const named = fixture.directory.path() / "a";
    writeProgram(named, "a");
    auto opened = vtpty::nativeDirectoryOpener().open(named.string());
    REQUIRE(opened.has_value());
    auto byPath = fixture.spec({ "./prog" });
    byPath.workingDirectory = std::move(*opened);

    // Another directory takes the name, with a program of the same name in it.
    std::filesystem::rename(named, fixture.directory.path() / "a.old");
    writeProgram(named, "b");

    auto const token = fixture.start(std::move(byPath));
    CHECK(fixture.runToCompletion(token).standardOutput.bytes == "prog-a\n");
}

TEST_CASE("PipedProcessRunner: a program planted in the directory is never found on the PATH",
          "[processrunner]")
{
    // An agent writes in its worktree: a `true` it drops there, or in a directory a relative PATH entry
    // names, must not run in place of the real one when the daemon's PATH has an empty entry (the working
    // directory, as execvp() reads it) or a relative one. Only absolute entries are searched.
    auto fixture = RunnerFixture {};
    auto const marker = fixture.directory.path() / "planted-ran";
    for (auto const& planted: { fixture.directory.path() / "true",
                                fixture.directory.path() / "bin" / "true",
                                fixture.directory.path() / "only-planted" })
    {
        std::filesystem::create_directories(planted.parent_path());
        std::ofstream { planted } << std::format("#!/bin/sh\necho planted\n: > '{}'\n", marker.string());
        std::filesystem::permissions(planted, std::filesystem::perms::owner_all);
    }
    auto const path = GENERATE(
        as<std::string> {}, ":/usr/bin:/bin", "/usr/bin::/bin", "bin:/usr/bin:/bin", ".:/usr/bin:/bin");
    CAPTURE(path);

    // The real one runs.
    auto real = fixture.spec({ "true" });
    real.environment = { { "PATH", path } };
    auto const exit = fixture.runToCompletion(fixture.start(std::move(real)));
    CHECK(exit.completion == ProcessCompletion::Exited);
    CHECK(exit.standardOutput.bytes.empty());

    // A program named by its absolute path -- as `contour` is mapped to the daemon's own executable -- runs
    // whatever the PATH holds.
    auto absolute = fixture.spec({ "/bin/sh", "-c", "echo self" });
    absolute.environment = { { "PATH", path } };
    auto const absoluteToken = fixture.start(std::move(absolute));
    CHECK(fixture.runToCompletion(absoluteToken).standardOutput.bytes == "self\n");

    // One only the directory has is not found at all.
    auto plantedOnly = fixture.spec({ "only-planted" });
    plantedOnly.environment = { { "PATH", path } };
    auto const started = fixture.runner->start(std::move(plantedOnly),
                                               [](ProcessToken, ProcessExit const&) { FAIL("no handler"); });
    REQUIRE_FALSE(started.has_value());
    CHECK(started.error() == vtpty::ProcessStartError::ProgramNotFound);

    CHECK_FALSE(std::filesystem::exists(marker));
}

TEST_CASE("PipedProcessRunner: the child sees exactly the given environment", "[processrunner]")
{
    auto const leaked = core::testing::ScopedEnv { "CONTOUR_PROCESSRUNNER_TEST_LEAK", "leaked" };
    auto fixture = RunnerFixture {};
    auto spec = fixture.spec({ "env" });
    spec.environment.emplace_back("ONLY_GIVEN", "yes");
    spec.environment.emplace_back("WITH_SPACE", "a b");
    auto expected = std::vector<std::string> {};
    for (auto const& [name, value]: spec.environment)
        expected.push_back(std::format("{}={}", name, value));
    std::ranges::sort(expected);

    auto const exit = fixture.runToCompletion(fixture.start(std::move(spec)));

    auto lines = std::vector<std::string> {};
    auto stream = std::istringstream { exit.standardOutput.bytes };
    for (auto line = std::string {}; std::getline(stream, line);)
        lines.push_back(line);
    std::ranges::sort(lines);
    CHECK(lines == expected);
}

TEST_CASE("PipedProcessRunner: a program that is not installed", "[processrunner]")
{
    auto fixture = RunnerFixture {};
    auto const started = fixture.runner->start(fixture.spec({ "contour-no-such-program-here" }),
                                               [](ProcessToken, ProcessExit const&) { FAIL("no handler"); });
    REQUIRE_FALSE(started.has_value());
    CHECK(started.error() == vtpty::ProcessStartError::ProgramNotFound);
    CHECK(fixture.runner->running() == 0);
}

TEST_CASE("PipedProcessRunner: an orphaned grandchild does not hold the reply", "[processrunner]")
{
    auto fixture = RunnerFixture {};
    auto const token = fixture.start(fixture.spec({ "sh", "-c", "sleep 1000 & echo done" }, 1h));
    // The leader's exit is seen in some round; each round moves the clock on by the grace, so the round after
    // that finds it spent.
    auto const exit =
        fixture.runToCompletion(token, [&] { fixture.clock.advance(fixture.tuning.orphanedOutputGrace); });
    CHECK(exit.completion == ProcessCompletion::Exited);
    CHECK(exit.exitCode == 0);
    CHECK(exit.standardOutput.bytes == "done\n");
}

TEST_CASE("PipedProcessRunner: two processes complete independently", "[processrunner]")
{
    auto fixture = RunnerFixture {};
    auto const one = fixture.start(fixture.spec({ "sh", "-c", "exit 1" }));
    auto const two = fixture.start(fixture.spec({ "sh", "-c", "exit 2" }));
    CHECK(one != two);
    CHECK(fixture.runner->running() == 2);
    CHECK(fixture.runToCompletion(one).exitCode == 1);
    CHECK(fixture.runToCompletion(two).exitCode == 2);
    CHECK(fixture.runner->running() == 0);
}

TEST_CASE("PipedProcessRunner: no handler runs inside start", "[processrunner]")
{
    auto fixture = RunnerFixture {};
    auto const token = fixture.start(fixture.spec({ "true" }));
    CHECK(fixture.finished.empty());
    CHECK(fixture.runToCompletion(token).exitCode == 0);
}

TEST_CASE("ThreadedProcessRunner: runs to completion", "[processrunner][threading]")
{
    auto const directory = core::testing::ScopedTempDir { "contour-processrunner" };
    auto opened = vtpty::nativeDirectoryOpener().open(directory.path().string());
    REQUIRE(opened.has_value());
    auto exited = std::promise<ProcessExit> {};
    auto future = exited.get_future();
    {
        auto runner = vtpty::ThreadedProcessRunner::create(core::platform::defaultSteadyClock(), {});
        REQUIRE(runner.has_value());
        auto const started = (*runner)->start(
            ProcessSpec { .argv = { "sh", "-c", "echo x" },
                          .workingDirectory = std::move(*opened),
                          .environment = { { "PATH", "/usr/bin:/bin" } },
                          .timeout = 30s,
                          .outputCap = OneMebibyte },
            [&exited](ProcessToken, ProcessExit exit) { exited.set_value(std::move(exit)); });
        REQUIRE(started.has_value());
        auto const exit = future.get();
        CHECK(exit.completion == ProcessCompletion::Exited);
        CHECK(exit.standardOutput.bytes == "x\n");
    } // Destruction joins the thread.
}

#else

TEST_CASE("PipedProcessRunner: start is unsupported", "[processrunner]")
{
    auto clock = core::platform::ManualClock {};
    auto runner = PipedProcessRunner::create(clock, {});
    REQUIRE(runner.has_value());
    auto const started = (*runner)->start(
        ProcessSpec { .argv = { "cmd" }, .workingDirectory = std::nullopt, .environment = {} },
        [](ProcessToken, ProcessExit) {});
    REQUIRE_FALSE(started.has_value());
    CHECK(started.error() == vtpty::ProcessStartError::Unsupported);
}

#endif
