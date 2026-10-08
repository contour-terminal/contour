// SPDX-License-Identifier: Apache-2.0
#include <vtpty/MockPty.hpp>
#include <vtpty/Process.hpp>
#include <vtpty/ProcessPlacement.hpp>

#include <crispy/BufferObject.hpp>

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
    return std::make_unique<vtpty::Process>(
        vtpty::Process::ExecInfo { .program = "/bin/sh", .arguments = {}, .workingDirectory = {}, .env = {} },
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
    void placeThenRelease(vtpty::ParkedChild child) override { _parked.push_back(std::move(child)); }

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
    std::vector<vtpty::ParkedChild> _parked;
};

/// @return A not yet started Process running `/bin/sh` with @p arguments, placed by @p placement.
std::unique_ptr<vtpty::Process> shellRunning(std::vector<std::string> arguments,
                                             std::shared_ptr<vtpty::ProcessPlacement> placement)
{
    return std::make_unique<vtpty::Process>(
        vtpty::Process::ExecInfo {
            .program = "/bin/sh", .arguments = std::move(arguments), .workingDirectory = {}, .env = {} },
        vtpty::createPty(PageSize, std::nullopt),
        /*escapeSandbox=*/false,
        std::move(placement));
}

/// Reads @p pty until @p needle shows up, for at most about 15 seconds.
/// @return What was read.
std::string drainUntil(vtpty::Pty& pty, std::string_view needle)
{
    auto pool = crispy::BufferObjectPool<char> { 4096 };
    auto collected = std::string {};
    for ([[maybe_unused]] auto const attempt: std::views::iota(0, 300))
    {
        if (collected.contains(needle))
            break;
        auto const storage = pool.allocateBufferObject();
        if (auto const result = pty.read(*storage, 50ms, 4096); result && !result->data.empty())
            collected.append(result->data);
    }
    return collected;
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
    auto process = std::make_unique<vtpty::Process>(
        vtpty::Process::ExecInfo {
            .program = "/bin/sh", .arguments = { "-c", "exec sleep 30" }, .workingDirectory = {}, .env = {} },
        vtpty::createPty(PageSize, std::nullopt),
        /*escapeSandbox=*/false,
        std::make_shared<vtpty::NoPlacement>());
    REQUIRE(process->start().has_value());
    CHECK(process->alive());
    CHECK_FALSE(process->checkStatus().has_value());

    process->terminate(vtpty::Process::TerminationHint::Hangup);

    auto const status = process->wait();
    REQUIRE(status.has_value());
    auto const* const signalled = std::get_if<vtpty::Process::SignalExit>(&*status);
    REQUIRE(signalled != nullptr);
    CHECK(signalled->signum == SIGHUP);
    CHECK_FALSE(process->alive());
}
TEST_CASE("Process.parkedUntilReleased", "[process][placement]")
{
    auto placement = std::make_shared<RecordingPlacement>();
    auto process = shellRunning({ "-c", "printf ready; exec sleep 30" }, placement);
    REQUIRE(process->start().has_value());
    REQUIRE(placement->pids().size() == 1);
    auto const pid = placement->pids().front();
    #ifdef __linux__
    // Still the forked copy of this test binary: the child has not reached exec().
    CHECK(executableOf(pid) == executableOf(::getpid()));
    #endif

    placement->release(pid);
    CHECK(drainUntil(*process, "ready").contains("ready"));
    #ifdef __linux__
    CHECK(executableOf(pid) != executableOf(::getpid()));
    // The child raised its own oom_score_adj before parking, and exec() kept it.
    CHECK(oomScoreAdjustOf(pid) == std::min(oomScoreAdjustOf(::getpid()) + 100, 1000));
    #endif

    process->terminate(vtpty::Process::TerminationHint::Hangup);
    (void) process->wait();
}

TEST_CASE("Process.releasedWhenThePlacementDropsIt", "[process][placement]")
{
    auto placement = std::make_shared<RecordingPlacement>();
    auto process = shellRunning({ "-c", "printf ready; exec sleep 30" }, placement);
    REQUIRE(process->start().has_value());
    placement->drop();
    CHECK(drainUntil(*process, "ready").contains("ready"));
    process->terminate(vtpty::Process::TerminationHint::Hangup);
    (void) process->wait();
}

TEST_CASE("Process.concurrentSpawnsDoNotWaitOnEachOther", "[process][placement]")
{
    // The second child is forked while the first is parked, and so inherits the parent's end of the
    // first one's gate. Releasing the first must not depend on the second.
    auto placement = std::make_shared<RecordingPlacement>();
    auto first = shellRunning({ "-c", "printf first; exec sleep 30" }, placement);
    auto second = shellRunning({ "-c", "printf second; exec sleep 30" }, placement);
    REQUIRE(first->start().has_value());
    REQUIRE(second->start().has_value());
    REQUIRE(placement->pids().size() == 2);

    placement->release(placement->pids()[0]);
    CHECK(drainUntil(*first, "first").contains("first"));

    placement->release(placement->pids()[1]);
    CHECK(drainUntil(*second, "second").contains("second"));

    for (auto* const process: { first.get(), second.get() })
    {
        process->terminate(vtpty::Process::TerminationHint::Hangup);
        (void) process->wait();
    }
}

TEST_CASE("Process.terminateWhileParked", "[process][placement]")
{
    // A tab closed before its shell was released: the parked child dies, wait() returns, and the
    // later release of a child nobody is reading for must not raise SIGPIPE here.
    auto* const previous = std::signal(SIGPIPE, SIG_DFL);
    auto placement = std::make_shared<RecordingPlacement>();
    auto process = shellRunning({ "-c", "exec sleep 30" }, placement);
    REQUIRE(process->start().has_value());

    process->terminate(vtpty::Process::TerminationHint::Hangup);
    auto const status = process->wait();
    REQUIRE(status.has_value());
    auto const* const signalled = std::get_if<vtpty::Process::SignalExit>(&*status);
    REQUIRE(signalled != nullptr);
    CHECK(signalled->signum == SIGHUP);

    placement->drop();
    std::signal(SIGPIPE, previous);
}
#endif
