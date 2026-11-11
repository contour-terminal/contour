// SPDX-License-Identifier: Apache-2.0
#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <csignal>
#include <filesystem>
#include <fstream>
#include <functional>
#include <string>
#include <thread>
#include <vector>

#include <vthost/SocketPath.hpp>
#include <vthost/testing/KeeperFixture.hpp>

/// @file
/// The keeper with real processes: how it bounds a crashing daemon's restarts, and how it stops.

using namespace std::chrono_literals;
using vthost::testing::HarnessLifecycle;
using vthost::testing::HarnessSetup;
using vthost::testing::HarnessStart;
using vthost::testing::HarnessTimings;
using vthost::testing::KeeperFixture;

namespace fs = std::filesystem;

namespace
{

[[nodiscard]] bool eventually(std::function<bool()> const& done, std::chrono::milliseconds bound = 20s)
{
    auto const deadline = std::chrono::steady_clock::now() + bound;
    while (!done())
    {
        if (std::chrono::steady_clock::now() >= deadline)
            return false;
        std::this_thread::sleep_for(20ms);
    }
    return true;
}

/// @return Whether process @p pid still exists (a zombie included).
[[nodiscard]] bool exists(int pid)
{
    return pid > 0 && ::kill(pid, 0) == 0;
}

/// The grace period the crash-loop cases give their keeper: long enough to look at what it keeps, short
/// enough for a test.
constexpr auto Grace = 3s;

/// A keeper whose restarts crash before serving, with one shell that writes its pid to `shell.pid`.
[[nodiscard]] HarnessSetup crashLoop()
{
    return HarnessSetup {
        .start = HarnessStart::ServesThenCrashes,
        .panes = { "echo $$ > @DIR@/shell.pid; exec sleep 300" },
        .snapshot = vthost::testing::HarnessSnapshot::None,
        .tcp = vthost::testing::HarnessTcp::Off,
        .softDescriptorLimit = std::nullopt,
        .timings =
            HarnessTimings { .backoffMs = 50,
                             .graceMs = static_cast<uint64_t>(
                                 std::chrono::duration_cast<std::chrono::milliseconds>(Grace).count()) },
    };
}

/// @return The pid the fixture's shell wrote; 0 until it did.
[[nodiscard]] int shellOf(KeeperFixture const& f)
{
    auto file = std::ifstream { f.directory() / "shell.pid" };
    auto pid = 0;
    file >> pid;
    return pid;
}

/// Kills the serving daemon and waits until the keeper gave up: five crashes, the first daemon's and four
/// restarts'.
void crashUntilGivenUp(KeeperFixture& f)
{
    REQUIRE(eventually([&] { return shellOf(f) > 0; }));
    f.killDaemon(SIGKILL);
    REQUIRE(eventually([&] { return f.log().contains("giving up on restarting it"); }));
}

} // namespace

TEST_CASE("a daemon that crashes on every start gives up after five, and the shells live", "[keeper][crash]")
{
    auto f = KeeperFixture { crashLoop() };
    INFO(f.log());
    crashUntilGivenUp(f);
    CHECK(f.launches().size() == 5);
    CHECK(f.log().contains("the daemon crashed 5 times within 300 s"));
    // The shells are kept through the grace period, and nothing else is started in it.
    std::this_thread::sleep_for(Grace / 2);
    CHECK(exists(shellOf(f)));
    CHECK(f.launches().size() == 5);
    CHECK(exists(f.keeperPid()));
}

TEST_CASE("after the grace the keeper ends the shells, unlinks the socket and exits non-zero",
          "[keeper][crash]")
{
    auto f = KeeperFixture { crashLoop() };
    INFO(f.log());
    crashUntilGivenUp(f);
    auto const shell = shellOf(f);
    REQUIRE(fs::exists(f.socketPath()));

    auto const exited = f.awaitKeeperExit(Grace + 20s);
    REQUIRE(exited.has_value());
    CHECK(*exited != 0);
    CHECK(f.launches().size() == 6); // the one retry, which crashed too
    CHECK_FALSE(exists(shell));
    CHECK_FALSE(fs::exists(f.socketPath()));
    CHECK_FALSE(fs::exists(vthost::nativeSocketPath(f.socketPath())));
}

namespace
{
/// @return A keeper with one shell that writes its pid to `shell.pid`, set up as @p start, @p lifecycle and
///         @p timings say.
[[nodiscard]] HarnessSetup withShell(HarnessStart start,
                                     HarnessTimings timings = {},
                                     HarnessLifecycle lifecycle = HarnessLifecycle::Persistent)
{
    return HarnessSetup { .start = start,
                          .panes = { "echo $$ > @DIR@/shell.pid; exec sleep 300" },
                          .snapshot = vthost::testing::HarnessSnapshot::None,
                          .tcp = vthost::testing::HarnessTcp::Off,
                          .softDescriptorLimit = std::nullopt,
                          .timings = timings,
                          .snapshotIntervalMs = 0,
                          .lifecycle = lifecycle };
}
} // namespace

TEST_CASE("SIGTERM to the keeper writes the final snapshot and restarts nothing", "[keeper][stop]")
{
    // Written only as the daemon ends: the interval is far longer than the test.
    auto f = KeeperFixture { HarnessSetup { .start = HarnessStart::Serves,
                                            .panes = { "echo FINAL-SNAPSHOT-MARKER; exec sleep 300" },
                                            .snapshot = vthost::testing::HarnessSnapshot::Kept,
                                            .tcp = vthost::testing::HarnessTcp::Off,
                                            .softDescriptorLimit = std::nullopt,
                                            .timings = {},
                                            .snapshotIntervalMs = 3'600'000,
                                            .lifecycle = HarnessLifecycle::Persistent } };
    INFO(f.log());
    REQUIRE_FALSE(fs::exists(f.snapshotPath()));
    ::kill(f.keeperPid(), SIGTERM);
    auto const exited = f.awaitKeeperExit(30s);
    REQUIRE(exited.has_value());
    CHECK(*exited == 0);
    CHECK(fs::exists(f.snapshotPath())); // the daemon's last write, as a stop asks for
    CHECK(f.launches().size() == 1);     // a stop is no crash
    CHECK(f.log().contains("asked to stop"));
}

TEST_CASE("StopDaemon ends the keeper", "[keeper][stop]")
{
    auto f = KeeperFixture { withShell(HarnessStart::Serves) };
    INFO(f.log());
    REQUIRE(eventually([&] { return shellOf(f) > 0; }));
    auto const shell = shellOf(f);
    REQUIRE(f.client()->stop(vthost::proto::StopReason::User).has_value());
    auto const exited = f.awaitKeeperExit(30s);
    REQUIRE(exited.has_value());
    CHECK(*exited == 0);
    CHECK(f.launches().size() == 1);
    CHECK_FALSE(exists(shell)); // hung up, and reaped by the keeper
    CHECK_FALSE(fs::exists(f.socketPath()));
}

TEST_CASE("an idle ExitWhenIdle daemon ends its keeper", "[keeper][stop]")
{
    auto f = KeeperFixture { HarnessSetup { .start = HarnessStart::Serves,
                                            .panes = {},
                                            .snapshot = vthost::testing::HarnessSnapshot::None,
                                            .tcp = vthost::testing::HarnessTcp::Off,
                                            .softDescriptorLimit = std::nullopt,
                                            .timings = {},
                                            .snapshotIntervalMs = 0,
                                            .lifecycle = HarnessLifecycle::ExitWhenIdle } };
    INFO(f.log());
    auto const exited = f.awaitKeeperExit(30s);
    REQUIRE(exited.has_value());
    CHECK(*exited == 0);
    CHECK(f.launches().size() == 1);
    CHECK_FALSE(fs::exists(f.socketPath()));
}

TEST_CASE("a daemon that ignores a forwarded stop is killed after the bound", "[keeper][stop]")
{
    constexpr auto Bound = 2s;
    auto f = KeeperFixture { withShell(
        HarnessStart::IgnoresStop,
        HarnessTimings { .backoffMs = 0,
                         .graceMs = 0,
                         .daemonExitBoundMs = static_cast<uint64_t>(
                             std::chrono::duration_cast<std::chrono::milliseconds>(Bound).count()) }) };
    INFO(f.log());
    REQUIRE(eventually([&] { return shellOf(f) > 0; }));
    auto const shell = shellOf(f);
    auto const daemon = f.daemonPid();
    auto const stopped = std::chrono::steady_clock::now();
    ::kill(f.keeperPid(), SIGTERM);
    auto const exited = f.awaitKeeperExit(30s);
    REQUIRE(exited.has_value());
    CHECK(*exited == 0);
    CHECK(std::chrono::steady_clock::now() - stopped >= Bound);
    CHECK(f.log().contains("did not end within 2 s of being stopped; killing it"));
    CHECK(f.launches() == std::vector { daemon }); // killed for the stop, not restarted
    CHECK_FALSE(exists(shell));
    CHECK_FALSE(fs::exists(f.socketPath())); // the keeper removed what the killed daemon could not
}

TEST_CASE("stop followed at once by start succeeds", "[keeper][stop]")
{
    // Review Focus 2: the second keeper finds the label free as soon as the first daemon stopped answering,
    // while the first keeper still hangs up its shells.
    auto f = KeeperFixture { withShell(HarnessStart::Serves) };
    INFO(f.log());
    REQUIRE(eventually([&] { return shellOf(f) > 0; }));
    auto const first = f.daemonPid();
    REQUIRE(f.client()->stop(vthost::proto::StopReason::User).has_value());
    f.startSecondKeeper();
    auto second = 0;
    CHECK(eventually([&] {
        second = f.daemonPid();
        return second > 0 && second != first;
    }));
    CHECK(f.secondKeeperRuns()); // not refused for a held label
    auto const exited = f.awaitKeeperExit(30s);
    REQUIRE(exited.has_value());
    CHECK(*exited == 0);
    CHECK(f.daemonPid() == second); // the first keeper's teardown leaves the new socket alone
}
