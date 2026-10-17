// SPDX-License-Identifier: Apache-2.0
#include <vtpty/DetachedProcess.hpp>
#include <vtpty/MockPty.hpp>
#include <vtpty/Process.hpp>
#include <vtpty/ProcessPlacement.hpp>
#include <vtpty/test/ProcessTesting.hpp>
#include <vtpty/test/PtyReading.hpp>

#include <core/platform/Clock.hpp>
#include <core/testing/ScopedTempDir.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <csignal>
#include <expected>
#include <filesystem>
#include <format>
#include <fstream>
#include <future>
#include <iterator>
#include <memory>
#include <optional>
#include <ranges>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <utility>
#include <variant>
#include <vector>

#ifndef _WIN32
    #include <sys/wait.h>

    #include <unistd.h>
#endif

using namespace std::chrono_literals;
using vtpty::testing::execInfo;
#ifndef _WIN32
using vtpty::testing::printed;
#endif

namespace
{

/// A PTY that refuses to start, as one does when the system has run out of them.
class UnstartablePty final: public vtpty::MockPty
{
  public:
    using vtpty::MockPty::MockPty;

    [[nodiscard]] vtpty::StartResult start() override
    {
        return std::unexpected(vtpty::StartFailure { .error = vtpty::StartError::PtyAllocationFailed,
                                                     .detail = "No PTY left." });
    }
};

/// @return A Process for a shell on @p pty, not yet started.
vtpty::Process shellOn(std::unique_ptr<vtpty::Pty> pty)
{
    return vtpty::Process {
        execInfo("/bin/sh"), std::move(pty), /*escapeSandbox=*/false, std::make_shared<vtpty::NoPlacement>()
    };
}

auto constexpr PageSize = vtpty::PageSize { vtpty::LineCount(24), vtpty::ColumnCount(80) };

#ifndef _WIN32
/// Holds every child it is handed, parked, until the test lets it go.
class RecordingPlacement final: public vtpty::ProcessPlacement
{
  public:
    /// @param childOomScoreAdjust What each child is to set its oom_score_adj to, if anything.
    explicit RecordingPlacement(std::optional<int> childOomScoreAdjust = std::nullopt):
        _childOomScoreAdjust { childOomScoreAdjust }
    {
    }

    void placeThenRelease(vtpty::ParkedChild child) noexcept override { _parked.push_back(std::move(child)); }

    [[nodiscard]] std::optional<int> childOomScoreAdjust() const noexcept override
    {
        return _childOomScoreAdjust;
    }

    /// @return The pids of the children handed over so far, in spawn order.
    [[nodiscard]] std::vector<int> pids() const
    {
        auto result = std::vector<int> {};
        for (auto const& child: _parked)
            result.push_back(child.pid());
        return result;
    }

    /// Releases the child with @p pid, keeping it recorded.
    void release(int pid)
    {
        for (auto& child: _parked)
            if (child.pid() == pid)
                child.release();
    }

    /// Destroys every recorded child without releasing it first: their destructors must do it.
    void drop() { _parked.clear(); }

  private:
    std::optional<int> _childOomScoreAdjust;
    std::vector<vtpty::ParkedChild> _parked;
};

/// Drops every child @p placement still holds when it goes out of scope. Declared after the
/// Process, so it runs first: ~Process waits for its child, which a held gate would park forever --
/// a failed REQUIRE would hang the suite instead of reporting.
class DropOnExit
{
  public:
    explicit DropOnExit(RecordingPlacement& placement): _placement { placement } {}
    DropOnExit(DropOnExit const&) = delete;
    DropOnExit& operator=(DropOnExit const&) = delete;
    DropOnExit(DropOnExit&&) = delete;
    DropOnExit& operator=(DropOnExit&&) = delete;
    ~DropOnExit() { _placement.drop(); }

  private:
    RecordingPlacement& _placement;
};

/// @return A not yet started Process running `/bin/sh` with @p arguments, placed by @p placement, giving
///         its child @p hangupGrace to end on a hangup.
vtpty::Process shellRunning(std::vector<std::string> arguments,
                            std::shared_ptr<vtpty::ProcessPlacement> placement,
                            vtpty::HangupGrace hangupGrace = {})
{
    return vtpty::Process { execInfo("/bin/sh", std::move(arguments)),
                            vtpty::createPty(PageSize, std::nullopt),
                            /*escapeSandbox=*/false,
                            std::move(placement),
                            hangupGrace };
}

/// Kills @p process's child when it goes out of scope. Declared after whatever waits for that child, so it
/// runs first: a failed REQUIRE then ends the test, rather than leaving it waiting on a child that ignores
/// its hangup, under a grace whose clock stands still.
class KillOnExit
{
  public:
    explicit KillOnExit(vtpty::Process& process): _process { process } {}
    KillOnExit(KillOnExit const&) = delete;
    KillOnExit& operator=(KillOnExit const&) = delete;
    KillOnExit(KillOnExit&&) = delete;
    KillOnExit& operator=(KillOnExit&&) = delete;
    ~KillOnExit() { _process.terminate(vtpty::Process::TerminationHint::Kill); }

  private:
    vtpty::Process& _process;
};

/// Hangs up @p process and waits for it.
/// @return How it ended.
std::optional<vtpty::Process::ExitStatus> hangUp(vtpty::Process& process)
{
    process.terminate(vtpty::Process::TerminationHint::Hangup);
    return process.wait();
}

/// @return Whether @p status says the child was ended by SIGHUP.
bool endedByHangup(std::optional<vtpty::Process::ExitStatus> const& status)
{
    auto const* const signalled = status ? std::get_if<vtpty::Process::SignalExit>(&*status) : nullptr;
    return signalled != nullptr && signalled->signum == SIGHUP;
}

    #ifdef __linux__
/// @return The oom_score_adj of process @p pid. Read as a stream: /proc reports its files as empty.
int oomScoreAdjustOf(int pid)
{
    auto value = 0;
    auto in = std::ifstream { std::format("/proc/{}/oom_score_adj", pid) };
    REQUIRE(in >> value);
    return value;
}

/// @return The executable process @p pid runs.
std::filesystem::path executableOf(int pid)
{
    return std::filesystem::read_symlink(std::format("/proc/{}/exe", pid));
}
    #endif
#endif

} // namespace

TEST_CASE("Process.withoutChild", "[process]")
{
    // A Process has a child only once start() has spawned one. Before that -- and for good, when start()
    // failed before it got that far -- there is nobody to ask about, wait for or signal. It held pid 0
    // meanwhile, which POSIX reads as "the caller's whole process group": asking about it could reap an
    // unrelated child, terminating it hung up the terminal's own process group, and destroying it
    // dereferenced the empty exit status that waitpid() came back with.
    //
    // alive() is required to be false before terminate() is called, so that a regression fails here
    // rather than hanging up the process group the test runs in.

    SECTION("never started")
    {
        auto process = shellOn(std::make_unique<vtpty::MockPty>(PageSize));
        CHECK_FALSE(process.checkStatus().has_value());
        CHECK_FALSE(process.wait().has_value());
        REQUIRE_FALSE(process.alive());
        process.terminate(vtpty::Process::TerminationHint::Hangup);
#ifndef _WIN32
        // Nobody's directory to report -- and on a PTY that is not a UnixPty, nobody's to look up.
        CHECK(process.workingDirectory() == ".");
#endif
    }

    SECTION("start() failed before spawning")
    {
        auto process = shellOn(std::make_unique<UnstartablePty>(PageSize));
        REQUIRE_FALSE(process.start().has_value());
        CHECK_FALSE(process.checkStatus().has_value());
        CHECK_FALSE(process.wait().has_value());
        REQUIRE_FALSE(process.alive());
        process.terminate(vtpty::Process::TerminationHint::Hangup);
    }

    // Leaving either section destroys the Process, which has to return rather than wait on nobody.
}

#ifndef _WIN32
TEST_CASE("Process.withChild", "[process]")
{
    // The counterpart: once start() has spawned a child, it is that child -- and only that child -- which
    // is asked about, signalled and waited for.
    auto process = shellRunning({ "-c", "exec sleep 30" }, std::make_shared<vtpty::NoPlacement>());
    REQUIRE(process.start().has_value());
    CHECK(process.alive());
    CHECK_FALSE(process.checkStatus().has_value());

    CHECK(endedByHangup(hangUp(process)));
    CHECK_FALSE(process.alive());
}

TEST_CASE("Process.killEndsAChildThatRefusesToEnd", "[process]")
{
    // A child that ignores a hangup and a termination alike -- as a shell does under `trap '' HUP`, an
    // interactive bash ignoring SIGTERM besides -- still ends when killed. The ignored signals carry over
    // the exec into sleep.
    auto process = shellRunning({ "-c", "trap '' HUP TERM; echo ready; exec sleep 30" },
                                std::make_shared<vtpty::NoPlacement>());
    REQUIRE(process.start().has_value());
    REQUIRE(printed(process, "ready"));

    process.terminate(vtpty::Process::TerminationHint::Hangup);
    process.terminate(vtpty::Process::TerminationHint::Normal);
    std::this_thread::sleep_for(100ms);
    CHECK(process.alive());

    process.terminate(vtpty::Process::TerminationHint::Kill);
    auto const status = process.wait();
    auto const* const signalled = status ? std::get_if<vtpty::Process::SignalExit>(&*status) : nullptr;
    REQUIRE(signalled != nullptr);
    CHECK(signalled->signum == SIGKILL);
}

TEST_CASE("Process.closeEndsAChildThatIgnoresTheHangup", "[process]")
{
    // Closing a Process's terminal hangs its child up. A session's exit watcher waits for the child to end,
    // and freeing the session -- on the GUI thread, for a closed tab -- waits for that watcher: a child that
    // ignores the hangup is killed once a short grace has passed, rather than waited for for good. Not
    // before: the grace runs on a clock that only the test moves.
    auto const grace = 50ms;
    auto clock = core::platform::ManualClock {};
    auto process = shellRunning({ "-c", "trap '' HUP TERM; echo ready; exec sleep 30" },
                                std::make_shared<vtpty::NoPlacement>(),
                                vtpty::HangupGrace { .duration = grace, .clock = clock });
    REQUIRE(process.start().has_value());
    REQUIRE(printed(process, "ready"));
    auto waiting = std::future<void> {};
    auto const killOnExit = KillOnExit { process };

    process.close();
    std::this_thread::sleep_for(5 * grace); // time enough for a grace kept on any clock but its own
    CHECK(process.alive());

    clock.advance(grace);
    waiting = std::async(std::launch::async, [&process] { process.waitForClosed(); });
    REQUIRE(waiting.wait_for(30s) == std::future_status::ready);

    auto const status = process.checkStatus();
    auto const* const signalled = status ? std::get_if<vtpty::Process::SignalExit>(&*status) : nullptr;
    REQUIRE(signalled != nullptr);
    CHECK(signalled->signum == SIGKILL);
}

TEST_CASE("Process.closeGivesAChildTheGraceToEndOnTheHangup", "[process]")
{
    // The grace is the child's: one that takes a moment to end on the hangup -- a shell saving its
    // history, say -- ends as it chose to, and is not killed under it. Its clock stands still here, so the
    // grace cannot run out however long the child takes.
    auto clock = core::platform::ManualClock {};
    auto process = std::optional<vtpty::Process> {};
    process.emplace(
        execInfo("/bin/sh",
                 { "-c", "trap 'sleep 0.5; exit 7' HUP; echo ready; while :; do sleep 0.01; done" }),
        vtpty::createPty(PageSize, std::nullopt),
        /*escapeSandbox=*/false,
        std::make_shared<vtpty::NoPlacement>(),
        vtpty::HangupGrace { .duration = 200ms, .clock = clock });
    REQUIRE(process->start().has_value());
    REQUIRE(printed(*process, "ready"));

    process->close();
    auto const status = process->wait();
    auto const* const exited = status ? std::get_if<vtpty::Process::NormalExit>(&*status) : nullptr;
    REQUIRE(exited != nullptr);
    CHECK(exited->exitCode == 7);

    // And it ends with the child: with its clock standing still, nothing else ends the grace that
    // ~Process waits for.
    auto destroyed = std::async(std::launch::async, [&process] { process.reset(); });
    auto const endedWithTheChild = destroyed.wait_for(30s) == std::future_status::ready;
    clock.advance(200ms); // ends a grace that did not, and so the test
    CHECK(endedWithTheChild);
}

TEST_CASE("Process.parkedUntilReleased", "[process][placement]")
{
    #ifdef __linux__
    // Raising one's own oom_score_adj takes no privilege; lowering it would.
    auto const wanted = std::min(oomScoreAdjustOf(::getpid()) + 7, 1000);
    auto placement = std::make_shared<RecordingPlacement>(wanted);
    #else
    auto placement = std::make_shared<RecordingPlacement>();
    #endif
    auto process = shellRunning({ "-c", "printf ready; exec sleep 30" }, placement);
    auto const dropOnExit = DropOnExit { *placement };
    REQUIRE(process.start().has_value());
    REQUIRE(placement->pids().size() == 1);
    auto const pid = placement->pids().front();
    #ifdef __linux__
    // Still the forked copy of this test binary: the child has not reached exec().
    CHECK(executableOf(pid) == executableOf(::getpid()));
    #endif

    placement->release(pid);
    CHECK(printed(process, "ready"));
    #ifdef __linux__
    CHECK(executableOf(pid) != executableOf(::getpid()));
    // The child set the oom_score_adj its placement asked for before parking, and exec() kept it.
    CHECK(oomScoreAdjustOf(pid) == wanted);
    #endif

    (void) hangUp(process);
}

TEST_CASE("Process.releasedWhenThePlacementDropsIt", "[process][placement]")
{
    auto placement = std::make_shared<RecordingPlacement>();
    auto process = shellRunning({ "-c", "printf ready; exec sleep 30" }, placement);
    REQUIRE(process.start().has_value());
    placement->drop();
    CHECK(printed(process, "ready"));
    (void) hangUp(process);
}

TEST_CASE("Process.concurrentSpawnsDoNotWaitOnEachOther", "[process][placement]")
{
    // The second child is forked while the first is parked, and so inherits the parent's end of the
    // first one's gate. Releasing the first must not depend on the second.
    auto placement = std::make_shared<RecordingPlacement>();
    auto first = shellRunning({ "-c", "printf first; exec sleep 30" }, placement);
    auto second = shellRunning({ "-c", "printf second; exec sleep 30" }, placement);
    auto const dropOnExit = DropOnExit { *placement };
    REQUIRE(first.start().has_value());
    REQUIRE(second.start().has_value());
    auto const pids = placement->pids();
    REQUIRE(pids.size() == 2);

    placement->release(pids.at(0));
    CHECK(printed(first, "first"));

    placement->release(pids.at(1));
    CHECK(printed(second, "second"));

    for (auto* const process: { &first, &second })
    {
        (void) hangUp(*process);
    }
}

TEST_CASE("Process.terminateWhileParked", "[process][placement]")
{
    // A tab closed before its shell was released: the parked child dies, wait() returns, and the
    // later release of a child nobody is reading for must not raise SIGPIPE here.
    auto* const previous = std::signal(SIGPIPE, SIG_DFL);
    auto placement = std::make_shared<RecordingPlacement>();
    auto process = shellRunning({ "-c", "exec sleep 30" }, placement);
    auto const dropOnExit = DropOnExit { *placement };
    REQUIRE(process.start().has_value());

    CHECK(endedByHangup(hangUp(process)));

    placement->drop();
    std::signal(SIGPIPE, previous);
}

namespace
{

/// @return A not yet started Process running @p program, as configured, with @p arguments.
vtpty::Process running(std::string program, std::vector<std::string> arguments)
{
    return vtpty::testing::unplacedProcess(execInfo(std::move(program), std::move(arguments)), PageSize);
}

} // namespace

TEST_CASE("Process.readsAProgramThatNamesNoneAsACommandLine", "[process]")
{
    // A `shell:` setting written "bash -l" names no program: it runs as the command line it spells,
    // as a POSIX shell reads one, its words ahead of the configured arguments.
    auto process = running(R"(/bin/sh -c 'printf "<%s>" "$0" "$@"' zero)", { "one two" });
    REQUIRE(process.start().has_value());
    CHECK(printed(process, "<zero><one two>"));
    (void) hangUp(process);
}

TEST_CASE("Process.runsAProgramPathWithSpacesAsItIs", "[process]")
{
    // A program path that names a program is never split, at its spaces or anywhere else.
    auto const directory = core::testing::ScopedTempDir { "contour-process" };
    auto const program = directory.path() / "with space" / "sh";
    std::filesystem::create_directory(program.parent_path());
    std::filesystem::copy_file("/bin/sh", program);

    auto process = running(program.string(), { "-c", "printf 'ran as %s' it-is" });
    REQUIRE(process.start().has_value());
    CHECK(printed(process, "ran as it-is"));
    (void) hangUp(process);
}

TEST_CASE("Process.runsARelativeProgramFromItsWorkingDirectory", "[process]")
{
    // The child changes to its working directory before it execs, so that is where "./bin/my shell" is.
    // Asked of this process's own directory instead, it named nothing there, and was split at its space
    // into "./bin/my", which did not run.
    auto const directory = core::testing::ScopedTempDir { "contour-process" };
    std::filesystem::create_directory(directory.path() / "bin");
    std::filesystem::copy_file("/bin/sh", directory.path() / "bin" / "my shell");

    auto exec = execInfo("./bin/my shell", { "-c", "printf 'ran as %s' it-is" });
    exec.workingDirectory = directory.path();
    auto process = vtpty::testing::unplacedProcess(exec, PageSize);
    REQUIRE(process.start().has_value());
    CHECK(printed(process, "ran as it-is"));
    (void) hangUp(process);
}

TEST_CASE("Process.fallsBackFromAProgramThatIsNotThere", "[process]")
{
    // A single word that names no program has no command line to read: the child says so, and runs the
    // login shell instead.
    auto process = running("contour-no-such-program", {});
    REQUIRE(process.start().has_value());
    CHECK(printed(process, "Failed to spawn contour-no-such-program"));
    (void) hangUp(process);
}

namespace
{

/// @return Whether @p file comes to read @p expected within ten seconds.
bool eventuallyReads(std::filesystem::path const& file, std::string_view expected)
{
    auto const deadline = std::chrono::steady_clock::now() + std::chrono::seconds { 10 };
    while (std::chrono::steady_clock::now() < deadline)
    {
        auto in = std::ifstream { file };
        auto const text = std::string { std::istreambuf_iterator<char> { in }, {} };
        if (text == expected)
            return true;
        std::this_thread::sleep_for(std::chrono::milliseconds { 10 });
    }
    return false;
}

/// @return What runs `/bin/sh -c SCRIPT TARGET`: the script reads @p target as "$0".
vtpty::DetachedCommand shellScript(std::string_view script, std::filesystem::path const& target)
{
    return vtpty::DetachedCommand { .image = "/bin/sh",
                                    .argv = { "sh", "-c", std::string { script }, target.string() },
                                    .standardInput = {},
                                    .output = vtpty::DetachedOutput::Inherited };
}

} // namespace

TEST_CASE("startDetached.placed before it runs", "[process][placement]")
{
    // Handed to the placement parked, as a session's shell is, so that nothing it starts is born in this
    // process's resource domain.
    auto const directory = core::testing::ScopedTempDir { "contour-detached" };
    auto const target = directory.path() / "ran.txt";
    auto placement = RecordingPlacement {};
    auto const dropOnExit = DropOnExit { placement };

    auto const pid = vtpty::startDetached(shellScript(R"(echo ran > "$0")", target), placement);
    REQUIRE(pid.has_value());
    REQUIRE(placement.pids() == std::vector { *pid });
    CHECK_FALSE(std::filesystem::exists(target)); // parked: it cannot have run
    #ifdef __linux__
    CHECK(executableOf(*pid) == executableOf(::getpid())); // still the forked copy of this test
    #endif

    placement.release(*pid);
    CHECK(eventuallyReads(target, "ran\n"));
}

TEST_CASE("startDetached.detached from this process", "[process][placement]")
{
    // Nothing here waits for it or reaps it, and a hangup of this process's session does not reach it.
    auto placement = RecordingPlacement {};
    auto const dropOnExit = DropOnExit { placement };
    auto const pid = vtpty::startDetached(
        vtpty::DetachedCommand {
            .image = "/bin/sh", .argv = { "sh", "-c", "exit 0" }, .standardInput = {}, .output = {} },
        placement);
    REQUIRE(pid.has_value());

    auto status = 0;
    CHECK(::waitpid(*pid, &status, WNOHANG) == -1);
    CHECK(errno == ECHILD);
    #ifdef __linux__
    CHECK(::getsid(*pid) != ::getsid(0));
    #endif
}

    #ifdef __linux__
TEST_CASE("startDetached.reads the given file and discards its output", "[process][placement]")
{
    auto const directory = core::testing::ScopedTempDir { "contour-detached" };
    auto const input = directory.path() / "input.txt";
    std::ofstream { input } << "piped\n";
    auto const target = directory.path() / "copied.txt";
    // What the shell's own standard output is, as /proc names it, written after what it read.
    auto command =
        shellScript(R"(cat > "$0.part"; readlink /proc/$$/fd/1 >> "$0.part"; mv "$0.part" "$0")", target);
    command.standardInput = input;
    command.output = vtpty::DetachedOutput::Discarded;
    auto placement = vtpty::NoPlacement {};

    REQUIRE(vtpty::startDetached(command, placement).has_value());
    CHECK(eventuallyReads(target, "piped\n/dev/null\n"));
}
    #endif

TEST_CASE("startDetached.an input that cannot be read starts nothing", "[process][placement]")
{
    auto const directory = core::testing::ScopedTempDir { "contour-detached" };
    auto command = shellScript("exit 0", directory.path() / "unused.txt");
    command.standardInput = directory.path() / "no-such-input.txt";
    auto placement = RecordingPlacement {};

    auto const pid = vtpty::startDetached(command, placement);
    REQUIRE_FALSE(pid.has_value());
    CHECK(pid.error() == std::errc::no_such_file_or_directory);
    CHECK(placement.pids().empty());
}

    #ifdef __linux__
TEST_CASE("startDetached.starts the program with no signal blocked", "[process][placement]")
{
    // The daemon blocks SIGINT and SIGTERM on every thread, for a thread of its own to sigwait() for them.
    auto blocked = sigset_t {};
    sigemptyset(&blocked);
    sigaddset(&blocked, SIGINT);
    sigaddset(&blocked, SIGTERM);
    auto previous = sigset_t {};
    REQUIRE(pthread_sigmask(SIG_BLOCK, &blocked, &previous) == 0);

    auto const directory = core::testing::ScopedTempDir { "contour-detached" };
    auto const target = directory.path() / "mask.txt";
    auto placement = vtpty::NoPlacement {};
    auto const pid = vtpty::startDetached(
        shellScript(R"(grep SigBlk /proc/self/status > "$0.part"; mv "$0.part" "$0")", target), placement);
    pthread_sigmask(SIG_SETMASK, &previous, nullptr);

    REQUIRE(pid.has_value());
    CHECK(eventuallyReads(target, "SigBlk:\t0000000000000000\n"));
}

TEST_CASE("startDetached.sets the oom_score_adj its placement supplies", "[process][placement]")
{
    auto const directory = core::testing::ScopedTempDir { "contour-detached" };
    auto const target = directory.path() / "ready.txt";
    // Raising one's own oom_score_adj takes no privilege; lowering it would.
    auto const wanted = std::min(oomScoreAdjustOf(::getpid()) + 7, 1000);
    auto placement = RecordingPlacement { wanted };
    auto const dropOnExit = DropOnExit { placement };

    auto const pid =
        vtpty::startDetached(shellScript(R"(echo ready > "$0"; exec sleep 30)", target), placement);
    REQUIRE(pid.has_value());
    placement.release(*pid);
    REQUIRE(eventuallyReads(target, "ready\n"));
    CHECK(oomScoreAdjustOf(*pid) == wanted);
    ::kill(*pid, SIGKILL); // started by this test, and nobody's child here: nothing else ends it
}
    #endif
#endif
