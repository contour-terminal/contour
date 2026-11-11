// SPDX-License-Identifier: Apache-2.0
#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <csignal>
#include <filesystem>
#include <fstream>
#include <functional>
#include <string>
#include <thread>

#include <vthost/SocketPath.hpp>
#include <vthost/testing/KeeperFixture.hpp>

/// @file
/// The keeper with real processes: how it bounds a crashing daemon's restarts, and how it stops.

using namespace std::chrono_literals;
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
