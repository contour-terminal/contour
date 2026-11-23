// SPDX-License-Identifier: Apache-2.0
//
// PipedProcessRunner on Windows, run against process_runner_probe: a console program that runs its arguments
// as a sequence of verbs. Where a case ports one of ProcessRunner_test.cpp's POSIX cases, it keeps that
// case's name, so that the two suites read side by side.
//
// Not ported, each for a reason of the platform's:
// - "a child ignoring SIGTERM is killed after the grace": Windows has no SIGTERM, so no graceful phase -- a
//   timed-out or cancelled process's job is terminated at once.
// - "a leader reaped by someone else completes with its status lost", and cancelling such a process: no one
//   else can reap a process while the runner holds its handle.
// - "a child has no controlling terminal": POSIX sessions; a Windows child gets a console that is never
// shown.
// - "a relative program is found in the directory entered by handle": Windows refuses a relative program path
//   (it can hand CreateProcessW only a path, never a handle), as "a relative program path is refused" checks.
#ifdef _WIN32

    #include <vtpty/DirectoryHandle.hpp>
    #include <vtpty/ProcessRunner.hpp>

    #include <core/Utf8Path.hpp>
    #include <core/platform/Clock.hpp>
    #include <core/testing/EnvHelper.hpp>
    #include <core/testing/ScopedTempDir.hpp>

    #include <catch2/catch_test_macros.hpp>
    #include <catch2/generators/catch_generators.hpp>

    #include <algorithm>
    #include <chrono>
    #include <cstddef>
    #include <cstdint>
    #include <filesystem>
    #include <format>
    #include <fstream>
    #include <future>
    #include <map>
    #include <memory>
    #include <optional>
    #include <ranges>
    #include <string>
    #include <utility>
    #include <vector>

    #include <Windows.h>

using namespace std::chrono_literals;
using vtpty::PipedProcessRunner;
using vtpty::ProcessCompletion;
using vtpty::ProcessExit;
using vtpty::ProcessSpec;
using vtpty::ProcessStartError;
using vtpty::ProcessToken;

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

/// @return @p path in UTF-8, as a spec spells paths.
[[nodiscard]] std::string utf8(std::filesystem::path const& path)
{
    return core::pathToUtf8(path);
}

/// Copies the probe to @p target, creating its directory.
void copyProbe(std::filesystem::path const& target)
{
    std::filesystem::create_directories(target.parent_path());
    std::filesystem::copy_file(std::filesystem::path { VTPTY_PROCESS_RUNNER_PROBE },
                               target,
                               std::filesystem::copy_options::overwrite_existing);
}

/// @return @p text split into its lines, each ended by a newline, sorted.
[[nodiscard]] std::vector<std::string> sortedLines(std::string const& text)
{
    auto lines = text | std::views::split('\n') | std::ranges::to<std::vector<std::string>>();
    // The newline that ends the last line starts no line of its own.
    if (!lines.empty() && lines.back().empty())
        lines.pop_back();
    std::ranges::sort(lines);
    return lines;
}

/// How long isDead() waits for a process to end, in milliseconds.
constexpr auto DeathWait = DWORD { 5000 };

/// How many pump rounds a process ended at once, or crashed, may take to complete: far fewer than a grace,
/// or Windows Error Reporting, would hold it for.
constexpr auto ImmediateEndRounds = 20;

/// The exit code of the probe's `crash`: the code of the exception it raises.
constexpr auto UnhandledExceptionCode = static_cast<int32_t>(0xE0000042);

/// How long process_runner_host has to start the probe and end itself, in milliseconds.
constexpr auto HostWait = DWORD { 30000 };

/// The exit code process_runner_host ends itself with, once the probe runs.
constexpr auto HostDiedHoldingTheRunner = DWORD { 7 };

/// @return The pid the probe wrote to @p file; std::nullopt until it has.
[[nodiscard]] std::optional<DWORD> readPid(std::filesystem::path const& file)
{
    auto pid = DWORD { 0 };
    if (auto stream = std::ifstream { file }; stream >> pid)
        return pid;
    return std::nullopt;
}

/// @return Whether the process @p pid has ended, waiting a bounded time for a kill already made to take
///         effect. One that cannot be opened is gone.
[[nodiscard]] bool isDead(DWORD pid)
{
    auto* const process = OpenProcess(SYNCHRONIZE | PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (process == nullptr)
        return true;
    auto const waited = WaitForSingleObject(process, DeathWait);
    CloseHandle(process);
    return waited == WAIT_OBJECT_0;
}

/// Ends the process @p pid, if it still runs: what a test leaves running past the runner, it cleans up.
void killPid(DWORD pid)
{
    auto* const process = OpenProcess(PROCESS_TERMINATE, FALSE, pid);
    if (process == nullptr)
        return;
    TerminateProcess(process, 1);
    CloseHandle(process);
}

/// A PipedProcessRunner on a ManualClock; a temporary directory holding a copy of the probe in `bin` and the
/// directory processes run in, `work`; and what each process's handler was called with.
struct RunnerFixture
{
    RunnerFixture()
    {
        copyProbe(bin / "probe.exe");
        std::filesystem::create_directories(work);
    }

    core::testing::ScopedTempDir directory { "contour-processrunner" };
    std::filesystem::path bin = directory.path() / "bin";
    std::filesystem::path work = directory.path() / "work";
    core::platform::ManualClock clock;
    vtpty::ProcessRunnerTuning tuning {};
    std::unique_ptr<PipedProcessRunner> runner = makeRunner(clock, tuning);
    std::map<ProcessToken, ProcessExit> finished;

    /// @return A spec running @p argv in `work`, under PATH=<bin> alone.
    [[nodiscard]] ProcessSpec spec(std::vector<std::string> argv,
                                   std::chrono::milliseconds timeout = 30s) const
    {
        auto opened = vtpty::nativeDirectoryOpener().open(utf8(work));
        REQUIRE(opened.has_value());
        return ProcessSpec { .argv = std::move(argv),
                             .workingDirectory = std::move(*opened),
                             .environment = { { "PATH", utf8(bin) } },
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

    /// @return The pid the probe wrote to @p file, pumping until it has.
    [[nodiscard]] DWORD awaitPid(std::filesystem::path const& file)
    {
        pumpUntil([&] { return readPid(file).has_value(); });
        auto const pid = readPid(file);
        REQUIRE(pid.has_value());
        return pid.value_or(0);
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

    /// @return Why @p spec could not be started; fails the test if it was.
    [[nodiscard]] ProcessStartError refusal(ProcessSpec spec) const
    {
        auto const started =
            runner->start(std::move(spec), [](ProcessToken, ProcessExit const&) { FAIL("no handler"); });
        REQUIRE_FALSE(started.has_value());
        return started.error();
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

} // namespace

TEST_CASE("PipedProcessRunner: exit codes", "[processrunner]")
{
    auto fixture = RunnerFixture {};
    auto const ok = fixture.run({ "probe", "exit", "0" });
    CHECK(ok.completion == ProcessCompletion::Exited);
    CHECK(ok.exitCode == 0);
    CHECK_FALSE(ok.signal.has_value());

    auto const three = fixture.run({ "probe", "exit", "3" });
    CHECK(three.completion == ProcessCompletion::Exited);
    CHECK(three.exitCode == 3);
    CHECK(fixture.runner->running() == 0);
}

TEST_CASE("PipedProcessRunner: both streams", "[processrunner]")
{
    auto fixture = RunnerFixture {};
    auto const exit = fixture.run({ "probe", "out", "out", "err", "err" });
    CHECK(exit.completion == ProcessCompletion::Exited);
    CHECK(exit.standardOutput.bytes == "out\n");
    CHECK(exit.standardOutput.extent == vtpty::OutputExtent::Whole);
    CHECK(exit.standardError.bytes == "err\n");
    CHECK(exit.standardError.extent == vtpty::OutputExtent::Whole);
}

TEST_CASE("PipedProcessRunner: output past the cap is drained and cut", "[processrunner]")
{
    auto fixture = RunnerFixture {};
    auto const exit = fixture.run({ "probe", "zeros", "2097152" });
    CHECK(exit.completion == ProcessCompletion::Exited);
    CHECK(exit.exitCode == 0);
    CHECK(exit.standardOutput.bytes.size() == OneMebibyte);
    CHECK(exit.standardOutput.extent == vtpty::OutputExtent::Truncated);
    CHECK(exit.standardError.extent == vtpty::OutputExtent::Whole);
}

TEST_CASE("PipedProcessRunner: a discarded stream stays empty however much is written", "[processrunner]")
{
    auto fixture = RunnerFixture {};
    auto spec = fixture.spec({ "probe", "zeros-err", "67108864", "out", "done" });
    spec.standardError = vtpty::StreamCapture::Discard;
    // 64 MiB through a 64 KiB pipe buffer: a thousand rounds and more.
    auto const exit = fixture.runToCompletion(fixture.start(std::move(spec)), [] {}, 5000);
    CHECK(exit.completion == ProcessCompletion::Exited);
    CHECK(exit.exitCode == 0);
    CHECK(exit.standardOutput.bytes == "done\n");
    CHECK(exit.standardError.bytes.empty());
    CHECK(exit.standardError.extent == vtpty::OutputExtent::Truncated);
}

TEST_CASE("PipedProcessRunner: the child sees exactly the given environment", "[processrunner]")
{
    auto const leaked = core::testing::ScopedEnv { "CONTOUR_PROCESSRUNNER_TEST_LEAK", "leaked" };
    auto fixture = RunnerFixture {};
    auto spec = fixture.spec({ "probe", "env" });
    spec.environment.emplace_back("ONLY_GIVEN", "yes");
    spec.environment.emplace_back("WITH_SPACE", "a b");
    auto expected = std::vector<std::string> {};
    for (auto const& [name, value]: spec.environment)
        expected.push_back(std::format("{}={}", name, value));
    std::ranges::sort(expected);

    auto const exit = fixture.runToCompletion(fixture.start(std::move(spec)));

    CHECK(sortedLines(exit.standardOutput.bytes) == expected);
}

TEST_CASE("PipedProcessRunner: an empty environment still starts", "[processrunner]")
{
    auto fixture = RunnerFixture {};
    auto spec = fixture.spec({ utf8(fixture.bin / "probe.exe"), "env" });
    spec.environment = {};
    auto const exit = fixture.runToCompletion(fixture.start(std::move(spec)));
    CHECK(exit.completion == ProcessCompletion::Exited);
    CHECK(exit.exitCode == 0);
    CHECK(exit.standardOutput.bytes.empty());
}

TEST_CASE("PipedProcessRunner: PATH is found whatever its case", "[processrunner]")
{
    auto fixture = RunnerFixture {};
    auto spec = fixture.spec({ "probe", "exit", "5" });
    spec.environment = { { "Path", utf8(fixture.bin) } };
    CHECK(fixture.runToCompletion(fixture.start(std::move(spec))).exitCode == 5);
}

TEST_CASE("PipedProcessRunner: a program that is not installed", "[processrunner]")
{
    auto fixture = RunnerFixture {};
    CHECK(fixture.refusal(fixture.spec({ "contour-no-such-program-here" }))
          == ProcessStartError::ProgramNotFound);
    CHECK(fixture.runner->running() == 0);
}

TEST_CASE("PipedProcessRunner: a program planted in the directory is never found on the PATH",
          "[processrunner]")
{
    // An agent writes in its worktree: a probe.exe it drops there, or in a directory a relative PATH entry
    // names, must not run in place of the real one. Only absolute entries are searched.
    auto fixture = RunnerFixture {};
    copyProbe(fixture.work / "probe.exe");
    copyProbe(fixture.work / "bin" / "probe.exe");
    copyProbe(fixture.work / "only-planted.exe");
    // The patterns, not the paths: a generator is made once, and each run has a temporary directory of its
    // own.
    auto const pattern = GENERATE(as<std::string> {}, ";{}", "{};;", "bin;{}", ".;{}", "C:;{}");
    auto const bin = utf8(fixture.bin);
    auto const path = std::vformat(pattern, std::make_format_args(bin));
    CAPTURE(path);

    // The real one runs.
    auto real = fixture.spec({ "probe", "self" });
    real.environment = { { "PATH", path } };
    auto const exit = fixture.runToCompletion(fixture.start(std::move(real)));
    CHECK(exit.completion == ProcessCompletion::Exited);
    CHECK(exit.standardOutput.bytes == utf8(fixture.bin / "probe.exe") + "\n");

    // A program named by its absolute path runs whatever the PATH holds.
    auto absolute = fixture.spec({ utf8(fixture.bin / "probe.exe"), "out", "self" });
    absolute.environment = { { "PATH", path } };
    auto const absoluteToken = fixture.start(std::move(absolute));
    CHECK(fixture.runToCompletion(absoluteToken).standardOutput.bytes == "self\n");

    // One only the directory has is not found at all.
    auto plantedOnly = fixture.spec({ "only-planted" });
    plantedOnly.environment = { { "PATH", path } };
    CHECK(fixture.refusal(std::move(plantedOnly)) == ProcessStartError::ProgramNotFound);
}

TEST_CASE("PipedProcessRunner: the test binary's own directory is not searched", "[processrunner]")
{
    // CreateProcessW's own search would look in the directory of the program that calls it first.
    auto fixture = RunnerFixture {};
    CHECK(fixture.refusal(fixture.spec({ "vtpty_test" })) == ProcessStartError::ProgramNotFound);
}

TEST_CASE("PipedProcessRunner: a batch file is never run", "[processrunner]")
{
    // CreateProcessW runs a .cmd or .bat through cmd.exe, which reads its arguments by rules of its own.
    auto fixture = RunnerFixture {};
    auto const marker = fixture.directory.path() / "batch-ran";
    for (auto const* const name: { "tool.cmd", "tool.bat" })
        std::ofstream { fixture.bin / name } << std::format("echo ran > \"{}\"\r\n", utf8(marker));

    CHECK(fixture.refusal(fixture.spec({ "tool.cmd" })) == ProcessStartError::ProgramNotFound);
    CHECK(fixture.refusal(fixture.spec({ "tool.bat" })) == ProcessStartError::ProgramNotFound);
    CHECK(fixture.refusal(fixture.spec({ utf8(fixture.bin / "tool.cmd") }))
          == ProcessStartError::ProgramNotFound);
    std::filesystem::remove(fixture.bin / "tool.bat");
    CHECK(fixture.refusal(fixture.spec({ "tool" })) == ProcessStartError::ProgramNotFound);
    CHECK_FALSE(std::filesystem::exists(marker));
}

TEST_CASE("PipedProcessRunner: a relative program path is refused", "[processrunner]")
{
    // POSIX resolves one in the child, after it entered the held directory by handle; Windows can only hand
    // CreateProcessW a path, which a directory swapped in under the same name could redirect.
    auto fixture = RunnerFixture {};
    copyProbe(fixture.work / "probe.exe");
    copyProbe(fixture.work / "bin" / "probe.exe");
    CHECK(fixture.refusal(fixture.spec({ ".\\probe.exe", "exit", "0" }))
          == ProcessStartError::ProgramNotFound);
    CHECK(fixture.refusal(fixture.spec({ "bin\\probe.exe" })) == ProcessStartError::ProgramNotFound);
}

TEST_CASE("PipedProcessRunner: arguments reach the child exactly", "[processrunner]")
{
    auto fixture = RunnerFixture {};
    auto const exit = fixture.run(
        { "probe", "args", "a b", "", "q\"uote", "back\\\\", "&", "|", "%PATH%", "^x", "tail\\" });
    CHECK(exit.standardOutput.bytes == "a b||q\"uote|back\\\\|&|||%PATH%|^x|tail\\\n");
}

TEST_CASE("PipedProcessRunner: an over-long command line is refused before it is started", "[processrunner]")
{
    auto fixture = RunnerFixture {};
    CHECK(fixture.refusal(fixture.spec({ "probe", "args", std::string(33000, 'x') }))
          == ProcessStartError::SpawnFailed);
}

TEST_CASE("PipedProcessRunner: the child runs in the held directory", "[processrunner]")
{
    auto fixture = RunnerFixture {};
    auto spec = fixture.spec({ "probe", "cwd" });
    auto const resolved = spec.workingDirectory->resolvedPath();
    CHECK(fixture.runToCompletion(fixture.start(std::move(spec))).standardOutput.bytes == resolved + "\n");
}

TEST_CASE("PipedProcessRunner: a removed working directory is refused", "[processrunner]")
{
    auto fixture = RunnerFixture {};
    auto const gone = fixture.work / "gone";
    std::filesystem::create_directories(gone);
    auto opened = vtpty::nativeDirectoryOpener().open(utf8(gone));
    REQUIRE(opened.has_value());
    std::filesystem::remove(gone);

    auto spec = fixture.spec({ "probe", "cwd" });
    spec.workingDirectory = std::move(*opened);
    CHECK(fixture.refusal(std::move(spec)) == ProcessStartError::DirectoryMissing);
}

TEST_CASE("PipedProcessRunner: a directory replaced at its name is refused, not entered", "[processrunner]")
{
    auto fixture = RunnerFixture {};
    auto const named = fixture.work / "a";
    std::filesystem::create_directories(named);
    auto opened = vtpty::nativeDirectoryOpener().open(utf8(named));
    REQUIRE(opened.has_value());

    // Another directory takes the name.
    std::filesystem::rename(named, fixture.work / "a.old");
    std::filesystem::create_directories(named);

    auto spec = fixture.spec({ "probe", "cwd" });
    spec.workingDirectory = std::move(*opened);
    CHECK(fixture.refusal(std::move(spec)) == ProcessStartError::DirectoryMissing);
}

TEST_CASE("PipedProcessRunner: an unbacked working directory is refused", "[processrunner]")
{
    auto fixture = RunnerFixture {};
    auto spec = fixture.spec({ "probe", "cwd" });
    spec.workingDirectory = vtpty::DirectoryHandle::unbacked(utf8(fixture.work));
    CHECK(fixture.refusal(std::move(spec)) == ProcessStartError::SpawnFailed);
}

TEST_CASE("PipedProcessRunner: stdin is empty", "[processrunner]")
{
    auto fixture = RunnerFixture {};
    CHECK(fixture.run({ "probe", "stdin" }).standardOutput.bytes == "0\n");
}

TEST_CASE("PipedProcessRunner: a crash status arrives as a negative exit code", "[processrunner]")
{
    auto fixture = RunnerFixture {};
    auto const exit = fixture.run({ "probe", "exit", "-1073741819" });
    CHECK(exit.completion == ProcessCompletion::Exited);
    CHECK(exit.exitCode == static_cast<int32_t>(0xC0000005));
}

TEST_CASE("PipedProcessRunner: exit code 259 is an exit, not a running process", "[processrunner]")
{
    // 259 is STILL_ACTIVE: what GetExitCodeProcess() answers for a process that runs, too.
    auto fixture = RunnerFixture {};
    auto const exit = fixture.run({ "probe", "exit", "259" });
    CHECK(exit.completion == ProcessCompletion::Exited);
    CHECK(exit.exitCode == 259);
}

TEST_CASE("PipedProcessRunner: a child that closes its streams completes when it exits", "[processrunner]")
{
    auto fixture = RunnerFixture {};
    auto const exit =
        fixture.run({ "probe", "out", "a", "close-out", "close-err", "sleep", "300", "exit", "4" });
    CHECK(exit.completion == ProcessCompletion::Exited);
    CHECK(exit.exitCode == 4);
    CHECK(exit.standardOutput.bytes == "a\n");
}

TEST_CASE("PipedProcessRunner: a crashing child ends at once, its exception code its exit code",
          "[processrunner]")
{
    // Its job lets no unhandled exception wait on Windows Error Reporting.
    auto fixture = RunnerFixture {};
    auto const exit =
        fixture.runToCompletion(fixture.start(fixture.spec({ "probe", "crash" })), [] {}, ImmediateEndRounds);
    CHECK(exit.completion == ProcessCompletion::Exited);
    CHECK(exit.exitCode == UnhandledExceptionCode);
}

TEST_CASE("PipedProcessRunner: a GUI-subsystem program's output is captured", "[processrunner]")
{
    auto fixture = RunnerFixture {};
    auto const exit = fixture.run({ utf8(std::filesystem::path { VTPTY_GUI_SUBSYSTEM_ECHO }), "x", "y z" });
    CHECK(exit.completion == ProcessCompletion::Exited);
    CHECK(exit.standardOutput.bytes == "x|y z\n");
}

TEST_CASE("PipedProcessRunner: two processes complete independently", "[processrunner]")
{
    auto fixture = RunnerFixture {};
    fixture.tuning.orphanedOutputGrace = 1h;
    fixture.runner = makeRunner(fixture.clock, fixture.tuning);
    auto const one = fixture.start(fixture.spec({ "probe", "exit", "1" }));
    auto const two = fixture.start(fixture.spec({ "probe", "sleep", "2000", "exit", "2" }));
    CHECK(one != two);
    CHECK(fixture.runner->running() == 2);
    // One completes while two still runs: two did not inherit one's pipe.
    CHECK(fixture.runToCompletion(one).exitCode == 1);
    CHECK_FALSE(fixture.finished.contains(two));
    CHECK(fixture.runner->running() == 1);
    CHECK(fixture.runToCompletion(two).exitCode == 2);
    CHECK(fixture.runner->running() == 0);
}

TEST_CASE("PipedProcessRunner: no handler runs inside start", "[processrunner]")
{
    auto fixture = RunnerFixture {};
    auto const token = fixture.start(fixture.spec({ "probe", "exit", "0" }));
    CHECK(fixture.finished.empty());
    auto const exit = fixture.runToCompletion(token);
    CHECK(exit.completion == ProcessCompletion::Exited);
    CHECK(exit.exitCode == 0);
}

TEST_CASE("PipedProcessRunner: a timeout terminates", "[processrunner]")
{
    auto fixture = RunnerFixture {};
    auto const pidFile = fixture.directory.path() / "pid";
    auto const token = fixture.start(fixture.spec({ "probe", "pid", utf8(pidFile), "sleep", "600000" }));
    auto const pid = fixture.awaitPid(pidFile);
    CHECK(fixture.runner->running() == 1);

    fixture.clock.advance(30s + 1ms);
    // Ended at once: the clock never moves on to a grace, so a process waiting for one would not complete
    // within these few rounds.
    auto const exit = fixture.runToCompletion(token, [] {}, ImmediateEndRounds);
    CHECK(exit.completion == ProcessCompletion::TimedOut);
    CHECK_FALSE(exit.exitCode.has_value());
    CHECK_FALSE(exit.signal.has_value());
    CHECK(isDead(pid));
}

TEST_CASE("PipedProcessRunner: a timeout kills a grandchild that holds no pipe", "[processrunner]")
{
    auto fixture = RunnerFixture {};
    auto const grandFile = fixture.directory.path() / "grandchild";
    auto const ready = fixture.directory.path() / "ready";
    auto const token = fixture.start(fixture.spec({ "probe",
                                                    "spawn-quiet",
                                                    utf8(grandFile),
                                                    "pid",
                                                    utf8(ready),
                                                    "sleep",
                                                    "600000",
                                                    "--",
                                                    "sleep",
                                                    "600000" }));
    auto const grandchild = fixture.awaitPid(grandFile);
    CHECK(fixture.awaitPid(ready) == grandchild);

    fixture.clock.advance(30s + 1ms);
    auto const exit = fixture.runToCompletion(token);
    CHECK(exit.completion == ProcessCompletion::TimedOut);
    CHECK_FALSE(exit.exitCode.has_value());
    CHECK(isDead(grandchild));
}

TEST_CASE("PipedProcessRunner: cancel", "[processrunner]")
{
    auto fixture = RunnerFixture {};
    auto const pidFile = fixture.directory.path() / "pid";
    auto const token = fixture.start(fixture.spec({ "probe", "pid", utf8(pidFile), "sleep", "600000" }));
    auto const pid = fixture.awaitPid(pidFile);
    fixture.runner->cancel(token);
    auto const exit = fixture.runToCompletion(token);
    CHECK(exit.completion == ProcessCompletion::Cancelled);
    CHECK_FALSE(exit.exitCode.has_value());
    CHECK_FALSE(exit.signal.has_value());
    CHECK(isDead(pid));

    // A token that has completed is ignored.
    fixture.runner->cancel(token);
    CHECK(fixture.runner->running() == 0);
}

TEST_CASE("PipedProcessRunner: cancelling a process that already exited keeps its exit code",
          "[processrunner]")
{
    auto fixture = RunnerFixture {};
    fixture.tuning.orphanedOutputGrace = 1h;
    fixture.runner = makeRunner(fixture.clock, fixture.tuning);
    auto const grandFile = fixture.directory.path() / "grandchild";
    auto const leaderFile = fixture.directory.path() / "leader";
    // The leader exits 6; the grandchild holds its pipe.
    auto const token = fixture.start(fixture.spec({ "probe",
                                                    "spawn",
                                                    utf8(grandFile),
                                                    "sleep",
                                                    "600000",
                                                    "--",
                                                    "pid",
                                                    utf8(leaderFile),
                                                    "exit",
                                                    "6" }));
    auto const grandchild = fixture.awaitPid(grandFile);
    auto const leader = fixture.awaitPid(leaderFile);
    REQUIRE(isDead(leader));
    for ([[maybe_unused]] auto const round: std::views::iota(0, 5))
        fixture.runner->pump(50ms);
    REQUIRE(fixture.runner->running() == 1);

    fixture.runner->cancel(token);
    auto const exit = fixture.runToCompletion(token);
    CHECK(exit.completion == ProcessCompletion::Cancelled);
    CHECK(exit.exitCode == 6);
    CHECK_FALSE(exit.signal.has_value());
    CHECK(isDead(grandchild));
}

TEST_CASE("PipedProcessRunner: an orphaned grandchild does not hold the reply", "[processrunner]")
{
    auto fixture = RunnerFixture {};
    auto const grandFile = fixture.directory.path() / "grandchild";
    auto const token = fixture.start(
        fixture.spec({ "probe", "spawn", utf8(grandFile), "sleep", "600000", "--", "out", "done" }, 1h));
    // The leader's exit is seen in some round; each round moves the clock on by the grace, so the round after
    // that finds it spent.
    auto const exit =
        fixture.runToCompletion(token, [&] { fixture.clock.advance(fixture.tuning.orphanedOutputGrace); });
    CHECK(exit.completion == ProcessCompletion::Exited);
    CHECK(exit.exitCode == 0);
    CHECK(exit.standardOutput.bytes == "done\n");
    auto const grandchild = readPid(grandFile);
    REQUIRE(grandchild.has_value());
    CHECK(isDead(*grandchild));
}

TEST_CASE("PipedProcessRunner: a background process outlives a leader that exits by itself",
          "[processrunner]")
{
    // As on POSIX: a daemon a command starts (git's fsmonitor, gpg-agent) is not ended with it. Only the
    // runner ending a process -- timeout, cancel, destruction, the orphan's grace -- ends its tree.
    auto fixture = RunnerFixture {};
    auto const grandFile = fixture.directory.path() / "grandchild";
    auto const token = fixture.start(fixture.spec(
        { "probe", "spawn-quiet", utf8(grandFile), "sleep", "600000", "--", "out", "done", "exit", "0" }));
    auto const exit = fixture.runToCompletion(token);
    auto const grandchild = readPid(grandFile);
    REQUIRE(grandchild.has_value());
    auto const outlived = !isDead(*grandchild);
    killPid(*grandchild);

    CHECK(exit.completion == ProcessCompletion::Exited);
    CHECK(exit.exitCode == 0);
    CHECK(exit.standardOutput.bytes == "done\n");
    CHECK(fixture.runner->running() == 0);
    CHECK(outlived);
}

TEST_CASE("PipedProcessRunner: destruction kills the process tree", "[processrunner]")
{
    auto fixture = RunnerFixture {};
    auto const grandFile = fixture.directory.path() / "grandchild";
    auto const pidFile = fixture.directory.path() / "pid";
    std::ignore = fixture.start(fixture.spec({ "probe",
                                               "spawn",
                                               utf8(grandFile),
                                               "sleep",
                                               "600000",
                                               "--",
                                               "pid",
                                               utf8(pidFile),
                                               "sleep",
                                               "600000" }));
    auto const grandchild = fixture.awaitPid(grandFile);
    auto const leader = fixture.awaitPid(pidFile);

    fixture.runner.reset();
    CHECK(isDead(leader));
    CHECK(isDead(grandchild));
    CHECK(fixture.finished.empty());
}

TEST_CASE("PipedProcessRunner: the children die with the process that runs them", "[processrunner]")
{
    // process_runner_host starts the probe on a runner, then terminates itself: no destructor runs, and the
    // system closing its handles -- the job's among them -- is all that can end the probe.
    auto const directory = core::testing::ScopedTempDir { "contour-processrunner" };
    auto const probe = directory.path() / "bin" / "probe.exe";
    copyProbe(probe);
    auto const pidFile = directory.path() / "pid";
    auto commandLine = std::format(L"\"{}\" \"{}\" \"{}\"",
                                   std::filesystem::path { VTPTY_PROCESS_RUNNER_HOST }.wstring(),
                                   probe.wstring(),
                                   pidFile.wstring());
    auto startup = STARTUPINFOW {};
    startup.cb = sizeof(startup);
    auto info = PROCESS_INFORMATION {};
    REQUIRE(CreateProcessW(nullptr,
                           commandLine.data(),
                           nullptr,
                           nullptr,
                           FALSE,
                           CREATE_NO_WINDOW,
                           nullptr,
                           nullptr,
                           &startup,
                           &info)
            != FALSE);
    CloseHandle(info.hThread);
    auto const waited = WaitForSingleObject(info.hProcess, HostWait);
    auto code = DWORD { 0 };
    GetExitCodeProcess(info.hProcess, &code);
    CloseHandle(info.hProcess);
    REQUIRE(waited == WAIT_OBJECT_0);
    REQUIRE(code == HostDiedHoldingTheRunner);

    auto const probePid = readPid(pidFile);
    REQUIRE(probePid.has_value());
    CHECK(isDead(*probePid));
}

TEST_CASE("PipedProcessRunner: an inheritable handle of the daemon does not reach the child",
          "[processrunner]")
{
    auto fixture = RunnerFixture {};
    auto inheritable = SECURITY_ATTRIBUTES { .nLength = sizeof(SECURITY_ATTRIBUTES),
                                             .lpSecurityDescriptor = nullptr,
                                             .bInheritHandle = TRUE };
    auto* read = HANDLE { nullptr };
    auto* write = HANDLE { nullptr };
    REQUIRE(CreatePipe(&read, &write, &inheritable, 0) != FALSE);
    auto const token = fixture.start(fixture.spec({ "probe", "sleep", "600000" }));
    fixture.runner->pump(0ms);

    // Had the child inherited the write end, closing ours would not break the pipe.
    CloseHandle(write);
    auto available = DWORD { 0 };
    auto const peeked = PeekNamedPipe(read, nullptr, 0, nullptr, &available, nullptr);
    auto const error = GetLastError();
    CloseHandle(read);
    CHECK(peeked == FALSE);
    CHECK(error == ERROR_BROKEN_PIPE);

    fixture.runner->cancel(token);
    CHECK(fixture.runToCompletion(token).completion == ProcessCompletion::Cancelled);
}

TEST_CASE("ThreadedProcessRunner: runs to completion", "[processrunner][threading]")
{
    auto const directory = core::testing::ScopedTempDir { "contour-processrunner" };
    copyProbe(directory.path() / "probe.exe");
    auto opened = vtpty::nativeDirectoryOpener().open(utf8(directory.path()));
    REQUIRE(opened.has_value());
    auto exited = std::promise<ProcessExit> {};
    auto future = exited.get_future();
    {
        auto runner = vtpty::ThreadedProcessRunner::create(core::platform::defaultSteadyClock(), {});
        REQUIRE(runner.has_value());
        auto const started = (*runner)->start(
            ProcessSpec { .argv = { "probe", "out", "x" },
                          .workingDirectory = std::move(*opened),
                          .environment = { { "PATH", utf8(directory.path()) } },
                          .timeout = 30s,
                          .outputCap = OneMebibyte },
            [&exited](ProcessToken, ProcessExit exit) { exited.set_value(std::move(exit)); });
        REQUIRE(started.has_value());
        auto const exit = future.get();
        CHECK(exit.completion == ProcessCompletion::Exited);
        CHECK(exit.standardOutput.bytes == "x\n");
    } // Destruction joins the thread.
}

#endif
