// SPDX-License-Identifier: Apache-2.0
#include <vtpty/MockPty.hpp>
#include <vtpty/Process.hpp>
#include <vtpty/ProcessPlacement.hpp>
#include <vtpty/test/PtyReading.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <chrono>
#include <csignal>
#include <expected>
#include <filesystem>
#include <format>
#include <fstream>
#include <memory>
#include <optional>
#include <ranges>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

#ifndef _WIN32
    #include <unistd.h>
#endif

using namespace std::chrono_literals;
using vtpty::testing::printed;

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
std::unique_ptr<vtpty::Process> shellOn(std::unique_ptr<vtpty::Pty> pty)
{
    return std::make_unique<vtpty::Process>(vtpty::Process::ExecInfo { .program = "/bin/sh",
                                                                       .arguments = {},
                                                                       .workingDirectory = {},
                                                                       .env = {},
                                                                       .removedEnvironment = {} },
                                            std::move(pty),
                                            /*escapeSandbox=*/false,
                                            std::make_shared<vtpty::NoPlacement>());
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

/// @return A not yet started Process running `/bin/sh` with @p arguments, placed by @p placement.
std::unique_ptr<vtpty::Process> shellRunning(std::vector<std::string> arguments,
                                             std::shared_ptr<vtpty::ProcessPlacement> placement)
{
    return std::make_unique<vtpty::Process>(vtpty::Process::ExecInfo { .program = "/bin/sh",
                                                                       .arguments = std::move(arguments),
                                                                       .workingDirectory = {},
                                                                       .env = {},
                                                                       .removedEnvironment = {} },
                                            vtpty::createPty(PageSize, std::nullopt),
                                            /*escapeSandbox=*/false,
                                            std::move(placement));
}

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
        CHECK_FALSE(process->checkStatus().has_value());
        CHECK_FALSE(process->wait().has_value());
        REQUIRE_FALSE(process->alive());
        process->terminate(vtpty::Process::TerminationHint::Hangup);
#ifndef _WIN32
        // Nobody's directory to report -- and on a PTY that is not a UnixPty, nobody's to look up.
        CHECK(process->workingDirectory() == ".");
#endif
    }

    SECTION("start() failed before spawning")
    {
        auto process = shellOn(std::make_unique<UnstartablePty>(PageSize));
        REQUIRE_FALSE(process->start().has_value());
        CHECK_FALSE(process->checkStatus().has_value());
        CHECK_FALSE(process->wait().has_value());
        REQUIRE_FALSE(process->alive());
        process->terminate(vtpty::Process::TerminationHint::Hangup);
    }

    // Leaving either section destroys the Process, which has to return rather than wait on nobody.
}

#ifndef _WIN32
TEST_CASE("Process.withChild", "[process]")
{
    // The counterpart: once start() has spawned a child, it is that child -- and only that child -- which
    // is asked about, signalled and waited for.
    auto process = shellRunning({ "-c", "exec sleep 30" }, std::make_shared<vtpty::NoPlacement>());
    REQUIRE(process->start().has_value());
    CHECK(process->alive());
    CHECK_FALSE(process->checkStatus().has_value());

    CHECK(endedByHangup(hangUp(*process)));
    CHECK_FALSE(process->alive());
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
    REQUIRE(process->start().has_value());
    REQUIRE(placement->pids().size() == 1);
    auto const pid = placement->pids().front();
    #ifdef __linux__
    // Still the forked copy of this test binary: the child has not reached exec().
    CHECK(executableOf(pid) == executableOf(::getpid()));
    #endif

    placement->release(pid);
    CHECK(printed(*process, "ready"));
    #ifdef __linux__
    CHECK(executableOf(pid) != executableOf(::getpid()));
    // The child set the oom_score_adj its placement asked for before parking, and exec() kept it.
    CHECK(oomScoreAdjustOf(pid) == wanted);
    #endif

    (void) hangUp(*process);
}

TEST_CASE("Process.releasedWhenThePlacementDropsIt", "[process][placement]")
{
    auto placement = std::make_shared<RecordingPlacement>();
    auto process = shellRunning({ "-c", "printf ready; exec sleep 30" }, placement);
    REQUIRE(process->start().has_value());
    placement->drop();
    CHECK(printed(*process, "ready"));
    (void) hangUp(*process);
}

TEST_CASE("Process.concurrentSpawnsDoNotWaitOnEachOther", "[process][placement]")
{
    // The second child is forked while the first is parked, and so inherits the parent's end of the
    // first one's gate. Releasing the first must not depend on the second.
    auto placement = std::make_shared<RecordingPlacement>();
    auto first = shellRunning({ "-c", "printf first; exec sleep 30" }, placement);
    auto second = shellRunning({ "-c", "printf second; exec sleep 30" }, placement);
    auto const dropOnExit = DropOnExit { *placement };
    REQUIRE(first->start().has_value());
    REQUIRE(second->start().has_value());
    REQUIRE(placement->pids().size() == 2);

    placement->release(placement->pids()[0]);
    CHECK(printed(*first, "first"));

    placement->release(placement->pids()[1]);
    CHECK(printed(*second, "second"));

    for (auto* const process: { first.get(), second.get() })
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
    REQUIRE(process->start().has_value());

    CHECK(endedByHangup(hangUp(*process)));

    placement->drop();
    std::signal(SIGPIPE, previous);
}
#endif
