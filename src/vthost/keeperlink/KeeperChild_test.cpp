// SPDX-License-Identifier: Apache-2.0
#include <vtpty/ProcessPlacement.hpp>
#include <vtpty/test/ProcessTesting.hpp>

#include <core/net/EventLoop.hpp>
#include <core/net/IoBackend.hpp>

#include <catch2/catch_test_macros.hpp>

#include <cerrno>
#include <chrono>
#include <csignal>
#include <filesystem>
#include <format>
#include <fstream>
#include <functional>
#include <iterator>
#include <memory>
#include <optional>
#include <random>
#include <string>
#include <tuple>
#include <variant>

#include <vthost/keeperlink/KeeperChannel.hpp>
#include <vthost/keeperlink/KeeperChild.hpp>
#include <vthost/keeperlink/KeeperLink.hpp>
#include <vthost/keeperlink/KeeperPtyFactory.hpp>
#include <vthost/testing/KeeperFixture.hpp>

using namespace std::chrono_literals;
using vthost::keeperlink::KeeperChannel;
using vthost::keeperlink::KeeperChild;
using vthost::keeperlink::KeeperLink;
using vthost::keeperlink::KeeperPtyFactory;
using vthost::testing::HarnessStart;
using vthost::testing::KeeperFixture;

namespace keeper = vthost::keeper;
namespace fs = std::filesystem;

namespace
{

/// A real keeper, whose daemon the test plays: its own process spawns through the keeper, as a daemon does.
struct KeeperChildHarness
{
    KeeperFixture keeper { HarnessStart::Relays };
    std::unique_ptr<core::net::IoBackend> backend = core::net::makeDefaultBackend();
    core::net::EventLoop loop { *backend };
    int losses = 0;
    std::optional<KeeperLink> link;
    std::optional<KeeperPtyFactory> factory;

    /// @param hangupGrace How long the keeper gives a closed child before it kills it.
    /// @param placement Places each child before it runs.
    explicit KeeperChildHarness(
        std::chrono::milliseconds hangupGrace,
        std::shared_ptr<vtpty::ProcessPlacement> placement = std::make_shared<vtpty::NoPlacement>())
    {
        link.emplace(loop, KeeperChannel { keeper.takeChannel() }, vthost::keeper::ChannelVersion, [this] {
            ++losses;
        });
        link->post(keeper::Serving { .hungRestart = keeper::HungRestart::Off, .heartbeatTimeoutMs = 30'000 });
        factory.emplace(*link, std::move(placement), hangupGrace);
    }

    KeeperChildHarness(KeeperChildHarness const&) = delete;
    KeeperChildHarness& operator=(KeeperChildHarness const&) = delete;
    KeeperChildHarness(KeeperChildHarness&&) = delete;
    KeeperChildHarness& operator=(KeeperChildHarness&&) = delete;

    ~KeeperChildHarness()
    {
        // A clean end, not a crash: the keeper restarts nothing, and ends the shells as it goes.
        link->post(keeper::Shutdown {});
        factory.reset();
        link.reset();
    }

    /// @return A started child running @p exec.
    [[nodiscard]] std::unique_ptr<KeeperChild> start(vtpty::Process::ExecInfo exec)
    {
        auto child = factory->make(vtpty::PageSize { vtpty::LineCount(24), vtpty::ColumnCount(80) },
                                   std::move(exec),
                                   vtpty::SandboxEscape::Contained);
        auto const started = child->start();
        INFO(keeper.log());
        REQUIRE(started.has_value());
        return child;
    }

    /// Turns the loop until @p done holds, for at most @p bound.
    /// @return Whether it came to hold.
    [[nodiscard]] bool turnUntil(std::function<bool()> const& done, std::chrono::milliseconds bound = 10s)
    {
        auto const deadline = std::chrono::steady_clock::now() + bound;
        while (!done())
        {
            if (std::chrono::steady_clock::now() >= deadline)
                return false;
            std::ignore = loop.runOnce(10ms);
        }
        return true;
    }
};

/// @return Whether @p pid names a process (a zombie counts) as signal 0 finds it.
[[nodiscard]] bool exists(int64_t pid)
{
    return ::kill(static_cast<pid_t>(pid), 0) == 0 || errno != ESRCH;
}

/// A program that ignores the hangup, as a shell's `nohup` child would, and sleeps.
[[nodiscard]] vtpty::Process::ExecInfo hangupIgnoringSleep()
{
    return vtpty::testing::execInfo("/bin/sh", { "-c", "trap '' HUP; exec sleep 30" });
}

} // namespace

TEST_CASE("a session spawned through the keeper runs and reports its exit status", "[keeper][keeperlink]")
{
    auto h = KeeperChildHarness { 200ms };
    auto const child = h.start(vtpty::testing::execInfo("/bin/sh", { "-c", "exit 7" }));
    REQUIRE(child->session().has_value());
    CHECK(child->pid() > 0);
    CHECK(child->crossing().kind == vtpty::CrossingKind::Keeper);
    CHECK(child->crossing().keeperSession == static_cast<uint64_t>(*child->session()));

    INFO(h.keeper.log());
    REQUIRE(h.turnUntil([&] { return child->checkStatus().has_value(); }));
    auto const status = *child->checkStatus();
    REQUIRE(std::holds_alternative<vtpty::NormalExit>(status));
    CHECK(std::get<vtpty::NormalExit>(status).exitCode == 7);
    CHECK(child->spawnProgress() == vtpty::SpawnProgress::Settled);
    CHECK(h.losses == 0);
}

TEST_CASE("cwd and foreground name answer for a keeper child", "[keeper][keeperlink]")
{
    auto h = KeeperChildHarness { 200ms };
    auto const directory =
        fs::canonical(fs::temp_directory_path()) / std::format("keeper-cwd-{}", std::random_device {}());
    fs::create_directories(directory);
    auto exec = vtpty::testing::execInfo("sleep", { "30" });
    exec.workingDirectory = directory;
    auto child = h.start(exec);
    // Whether its spawn is still in flight or settled already: never the keeper's own directory.
    CHECK(child->workingDirectory() == directory.string());

    INFO(h.keeper.log());
    CHECK(h.turnUntil([&] { return child->foregroundProcessName() == "sleep"; }));
    CHECK(child->workingDirectory() == directory.string());
    CHECK_FALSE(child->checkStatus().has_value());

    auto const pid = child->pid();
    child.reset();
    CHECK(h.turnUntil([&] { return !exists(pid); }, 10s));
    fs::remove_all(directory);
}

TEST_CASE("closing a keeper child hangs it up and the keeper kills it after the grace",
          "[keeper][keeperlink]")
{
    auto h = KeeperChildHarness { 500ms };
    auto const child = h.start(hangupIgnoringSleep());
    INFO(h.keeper.log());
    // Running the program, its hangup ignored.
    REQUIRE(h.turnUntil([&] { return child->foregroundProcessName() == "sleep"; }));

    auto const closedAt = std::chrono::steady_clock::now();
    child->close();
    CHECK(child->isClosed());
    // It ignores the hangup, and lives on until the keeper kills it. Measured by when it is gone, never by
    // whether it is there at some moment, which a loaded machine reaches late: the first look that finds it
    // gone is at or after its death, so the span can only be overstated, and the kill can only have come no
    // earlier than the grace allows (the Release left the loop after closedAt).
    REQUIRE(h.turnUntil([&] { return !exists(child->pid()); }, 10s));
    auto const goneAt = std::chrono::steady_clock::now();
    CHECK(goneAt - closedAt >= 500ms);
    CHECK(h.losses == 0);
}

TEST_CASE("destroying a keeper child never blocks", "[keeper][keeperlink]")
{
    auto h = KeeperChildHarness { 5'000ms };
    auto child = h.start(hangupIgnoringSleep());
    INFO(h.keeper.log());
    REQUIRE(h.turnUntil([&] { return child->foregroundProcessName() == "sleep"; }));
    auto const pid = child->pid();

    // Its program ignores the hangup and is killed only after the grace: a destructor that waited for it
    // would take that long.
    auto const before = std::chrono::steady_clock::now();
    child.reset();
    CHECK(std::chrono::steady_clock::now() - before < 1s);
    CHECK(exists(pid));
    CHECK(h.turnUntil([&] { return !exists(pid); }, 15s));
}

TEST_CASE("terminating a keeper child goes through the keeper", "[keeper][keeperlink]")
{
    auto h = KeeperChildHarness { 5'000ms };
    INFO(h.keeper.log());

    SECTION("Normal asks it to end")
    {
        auto const child = h.start(vtpty::testing::execInfo("sleep", { "30" }));
        REQUIRE(h.turnUntil([&] { return child->foregroundProcessName() == "sleep"; }));
        child->terminate(vtpty::TerminationHint::Normal);
        REQUIRE(h.turnUntil([&] { return child->checkStatus().has_value(); }));
        CHECK(std::get<vtpty::SignalExit>(*child->checkStatus()).signum == SIGTERM);
        CHECK_FALSE(child->isClosed());
    }

    SECTION("Kill ends it outright, a hangup ignored or not")
    {
        auto const child = h.start(hangupIgnoringSleep());
        REQUIRE(h.turnUntil([&] { return child->foregroundProcessName() == "sleep"; }));
        child->terminate(vtpty::TerminationHint::Kill);
        REQUIRE(h.turnUntil([&] { return child->checkStatus().has_value(); }));
        CHECK(std::get<vtpty::SignalExit>(*child->checkStatus()).signum == SIGKILL);
    }

    SECTION("Hangup closes it")
    {
        auto const child = h.start(vtpty::testing::execInfo("sleep", { "30" }));
        REQUIRE(h.turnUntil([&] { return child->foregroundProcessName() == "sleep"; }));
        child->terminate(vtpty::TerminationHint::Hangup);
        CHECK(child->isClosed());
        // Hung up as its terminal closed, well within the grace.
        CHECK(h.turnUntil([&] { return !exists(child->pid()); }, 3s));
    }
}

TEST_CASE("a keeper child runs under the daemon's environment, none of the keeper's", "[keeper][keeperlink]")
{
    auto h = KeeperChildHarness { 200ms };
    INFO(h.keeper.log());
    // 4: not set, as in this process; 3: the keeper's own leaked into it.
    auto const child =
        h.start(vtpty::testing::execInfo("/bin/sh",
                                         { "-c",
                                           std::format("if [ -n \"${{{}+x}}\" ]; then exit 3; fi; exit 4",
                                                       vthost::testing::KeeperOnlyVariable) }));
    REQUIRE(h.turnUntil([&] { return child->checkStatus().has_value(); }));
    CHECK(std::get<vtpty::NormalExit>(*child->checkStatus()).exitCode == 4);

    // The control: the keeper does run its children under the variable -- its daemon, whose environment it
    // is -- so the shell's not seeing it is the spawn's doing, not the variable's absence.
#ifdef __linux__
    REQUIRE_FALSE(h.keeper.launches().empty());
    auto file = std::ifstream { std::format("/proc/{}/environ", h.keeper.launches().front()) };
    auto const entries =
        std::string { std::istreambuf_iterator<char> { file }, std::istreambuf_iterator<char> {} };
    CHECK(entries.contains(std::format("{}=1", vthost::testing::KeeperOnlyVariable)));
#endif
}

TEST_CASE("a keeper child's spawn is in progress until its placement releases it", "[keeper][keeperlink]")
{
    auto const placement = std::make_shared<vtpty::testing::HoldingPlacement>();
    auto h = KeeperChildHarness { 200ms, placement };
    INFO(h.keeper.log());
    auto const child = h.start(vtpty::testing::execInfo("sleep", { "30" }));
    // Parked on its gate, before its terminal is set up.
    std::ignore = h.turnUntil([] { return false; }, 200ms);
    CHECK(child->spawnProgress() == vtpty::SpawnProgress::InProgress);

    placement->releaseAll();
    CHECK(h.turnUntil([&] { return child->spawnProgress() == vtpty::SpawnProgress::Settled; }));
}
