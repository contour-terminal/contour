// SPDX-License-Identifier: Apache-2.0
//
// The supervisor over a fake launcher and a manual clock: every timeout is the clock's, and sync() makes each
// step of a case deterministic.
#include <core/Clock.hpp>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <future>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include <vtextension/wasm/BackendConfig.hpp>
#include <vtextension/wasm/remote/HostConnector.hpp>
#include <vtextension/wasm/remote/process/ExtensionHostSupervisor.hpp>
#include <vtextension/wasm/remote/process/ProcessLauncher.hpp>
#include <vtextension/wasm/remote/process/testing/FakeProcessLauncher.hpp>

using vtextension::wasm::HostTimeouts;
using vtextension::wasm::remote::HostAttachment;
using vtextension::wasm::remote::HostStep;
using vtextension::wasm::remote::process::ExitKind;
using vtextension::wasm::remote::process::ExitStatus;
using vtextension::wasm::remote::process::ExtensionHostSupervisor;
using vtextension::wasm::remote::process::KillReason;
using vtextension::wasm::remote::process::LaunchError;
using vtextension::wasm::remote::process::LaunchSpec;
using vtextension::wasm::remote::process::ShutdownReapBudget;
using vtextension::wasm::remote::process::SupervisedHelper;
using vtextension::wasm::remote::process::SupervisorConfig;
using vtextension::wasm::remote::process::testing::FakeProcessLauncher;
using namespace std::chrono_literals;

namespace
{

/// @return What a fake launch is asked to start; the fake starts nothing of it.
[[nodiscard]] LaunchSpec fakeSpec()
{
    return LaunchSpec { .executable = "helper",
                        .inherited = {},
                        .arguments = [](auto /*handles*/) { return std::vector<std::string> {}; },
                        .environment = {},
                        .caps = {} };
}

/// @return How many kills the supervisor sent for @p reason.
[[nodiscard]] std::size_t killsFor(ExtensionHostSupervisor const& supervisor, KillReason reason)
{
    return supervisor.counters().kills.at(std::to_underlying(reason));
}

/// A supervisor over a fake launcher, timed by a manual clock.
struct Rig
{
    explicit Rig(std::chrono::milliseconds teardownGrace = 0ms): grace { teardownGrace } {}

    std::chrono::milliseconds grace;
    std::shared_ptr<FakeProcessLauncher> fake = std::make_shared<FakeProcessLauncher>();
    core::ManualClock clock {};
    std::unique_ptr<ExtensionHostSupervisor> supervisor = [this] {
        auto started = ExtensionHostSupervisor::start(
            SupervisorConfig { .launcher = fake, .clock = &clock, .teardownGrace = grace });
        REQUIRE(started.has_value());
        return std::move(*started);
    }();
    std::shared_ptr<std::atomic<int>> deaths = std::make_shared<std::atomic<int>>(0);

    /// @return A helper launched through the mailbox, which must start.
    [[nodiscard]] SupervisedHelper launch() const
    {
        auto helper = supervisor->mailbox()->launch(fakeSpec());
        REQUIRE(helper.has_value());
        return std::move(*helper);
    }

    /// @return An attachment counting its helper's deaths in `deaths`.
    [[nodiscard]] HostAttachment countingDeaths() const
    {
        return HostAttachment { .onDeath = [deaths = deaths] { deaths->fetch_add(1); } };
    }

    /// Moves the clock on by @p delta, and lets the supervisor see it.
    void advance(core::SteadyDuration delta)
    {
        clock.advance(delta);
        supervisor->sync();
    }

    /// Lets the supervisor process everything and wait once: the fake's lastDeadline() then is what it waits
    /// with when idle.
    void settle() const
    {
        supervisor->sync();
        supervisor->sync();
    }
};

/// One step and its default budget.
struct StepRow
{
    HostStep step;
    std::chrono::milliseconds budget;
    char const* name;
};

} // namespace

TEST_CASE("Supervisor.launchRunsOnItsThread", "[vtextension]")
{
    auto rig = Rig {};
    auto const helper = rig.launch();
    CHECK(rig.fake->launchThreads() == std::vector { rig.supervisor->threadId() });
    CHECK(helper.processId() == vtextension::wasm::remote::process::testing::FirstFakeProcessId);
    rig.supervisor->sync();
    CHECK(rig.supervisor->counters().launches == 1);
    CHECK(rig.supervisor->liveProcesses() == 1);
}

TEST_CASE("Supervisor.stepTimeoutsKill", "[vtextension]")
{
    constexpr auto Defaults = HostTimeouts {};
    auto const rows = std::array {
        StepRow { .step = HostStep::Handshake, .budget = Defaults.handshake, .name = "handshake" },
        StepRow { .step = HostStep::Load, .budget = Defaults.load, .name = "load" },
        StepRow { .step = HostStep::Instantiate, .budget = Defaults.instantiate, .name = "instantiate" },
    };
    REQUIRE(rows[0].budget == 2000ms);
    REQUIRE(rows[1].budget == 10000ms);
    REQUIRE(rows[2].budget == 1000ms);
    for (auto const& row: rows)
    {
        DYNAMIC_SECTION(row.name)
        {
            auto rig = Rig {};
            auto helper = rig.launch();
            helper.attach(rig.countingDeaths());
            auto const watch = helper.watch(row.step, row.budget);
            rig.advance(row.budget - 1ms);
            CHECK(rig.fake->kills().empty());
            CHECK(rig.deaths->load() == 0);

            rig.advance(1ms);
            CHECK(rig.fake->kills() == std::vector<std::size_t> { 0 });
            CHECK(rig.deaths->load() == 1);
            CHECK(killsFor(*rig.supervisor, KillReason::StepTimeout) == 1);
            // Its own kill: no unclean exit, and its death is told once, however it is then reaped.
            CHECK(rig.supervisor->counters().reaped == 1);
            CHECK(rig.supervisor->counters().uncleanExits == 0);
        }
    }
}

TEST_CASE("Supervisor.watchIsTimedFromItsCaller", "[vtextension]")
{
    // The budget runs from watch(), not from when the supervisor gets to the command.
    auto rig = Rig {};
    auto helper = rig.launch();
    auto const watch = helper.watch(HostStep::Load, 10ms);
    rig.clock.advance(10ms);
    rig.supervisor->sync();
    CHECK(rig.fake->kills() == std::vector<std::size_t> { 0 });
}

TEST_CASE("Supervisor.disarmedWatchNeverFires", "[vtextension]")
{
    auto rig = Rig {};
    auto helper = rig.launch();
    {
        auto const watch = helper.watch(HostStep::Load, 10ms);
    }
    // Disarmed before the time passes: the supervisor may otherwise look at the clock in between.
    rig.supervisor->sync();
    rig.advance(1h);
    CHECK(rig.fake->kills().empty());
    rig.settle();
    CHECK(rig.fake->lastDeadline() == std::nullopt);
}

TEST_CASE("Supervisor.armedWatchIsWaitedFor", "[vtextension]")
{
    auto rig = Rig {};
    auto helper = rig.launch();
    auto const watch = helper.watch(HostStep::Instantiate, 1000ms);
    rig.settle();
    CHECK(rig.fake->lastDeadline() == core::SteadyTimePoint {} + 1000ms);
}

TEST_CASE("Supervisor.exitIsObservedAndReaped", "[vtextension]")
{
    auto rig = Rig {};
    auto helper = rig.launch();
    helper.attach(rig.countingDeaths());
    rig.supervisor->sync();
    rig.fake->exit(0, ExitStatus { .kind = ExitKind::Exited, .value = 3 });
    rig.supervisor->sync();

    auto const counters = rig.supervisor->counters();
    CHECK(counters.deathsObserved == 1);
    CHECK(counters.reaped == 1);
    CHECK(counters.uncleanExits == 1);
    CHECK(rig.deaths->load() == 1);
    CHECK(rig.supervisor->liveProcesses() == 0);
    // Reaped: a kill now reaches nothing.
    helper.kill();
    rig.supervisor->sync();
    CHECK(rig.fake->kills().empty());
}

TEST_CASE("Supervisor.attachAfterDeathFiresAtOnce", "[vtextension]")
{
    auto rig = Rig {};
    auto helper = rig.launch();
    rig.fake->exit(0, ExitStatus { .kind = ExitKind::Exited, .value = 0 });
    rig.supervisor->sync();
    CHECK(rig.supervisor->counters().reaped == 1);
    CHECK(rig.supervisor->counters().uncleanExits == 0);

    helper.attach(rig.countingDeaths());
    rig.supervisor->sync();
    CHECK(rig.deaths->load() == 1);
}

TEST_CASE("Supervisor.requestedKillIsNotUnclean", "[vtextension]")
{
    auto rig = Rig {};
    auto helper = rig.launch();
    helper.attach(rig.countingDeaths());
    helper.kill();
    rig.supervisor->sync();
    CHECK(rig.fake->kills() == std::vector<std::size_t> { 0 });
    // The fake ended it as SIGKILL does.
    auto const counters = rig.supervisor->counters();
    CHECK(counters.reaped == 1);
    CHECK(counters.uncleanExits == 0);
    CHECK(killsFor(*rig.supervisor, KillReason::Requested) == 1);
    CHECK(rig.deaths->load() == 1);
}

TEST_CASE("Supervisor.crashAfterAKillIsStillUnclean", "[vtextension]")
{
    // Only the status a kill ends a process with is the supervisor's doing.
    auto rig = Rig {};
    auto helper = rig.launch();
    rig.fake->survivesKill(0);
    helper.kill();
    rig.supervisor->sync();
    rig.fake->exit(0, ExitStatus { .kind = ExitKind::Signalled, .value = 11 });
    rig.supervisor->sync();
    CHECK(rig.supervisor->counters().uncleanExits == 1);
}

TEST_CASE("Supervisor.dropKillsAndReaps", "[vtextension]")
{
    auto rig = Rig {};
    {
        auto const helper = rig.launch();
    }
    rig.supervisor->sync();
    CHECK(killsFor(*rig.supervisor, KillReason::Dropped) == 1);
    CHECK(rig.supervisor->counters().reaped == 1);
    CHECK(rig.supervisor->counters().uncleanExits == 0);
    CHECK(rig.supervisor->liveProcesses() == 0);
}

TEST_CASE("Supervisor.dropWaitsTheTeardownGrace", "[vtextension]")
{
    auto rig = Rig { 2000ms };
    {
        auto helper = rig.launch();
        helper.attach(rig.countingDeaths());
    }
    rig.supervisor->sync(); // the grace runs from when the supervisor lets it go
    rig.advance(1999ms);
    CHECK(rig.fake->kills().empty());
    rig.advance(1ms);
    CHECK(killsFor(*rig.supervisor, KillReason::Dropped) == 1);
    CHECK(rig.supervisor->counters().reaped == 1);
    // Let go: its attachment was forgotten with it.
    CHECK(rig.deaths->load() == 0);
}

TEST_CASE("Supervisor.helperExitingWithinTheGraceIsNotKilled", "[vtextension]")
{
    auto rig = Rig { 2000ms };
    {
        auto const helper = rig.launch();
    }
    rig.supervisor->sync();
    rig.fake->exit(0, ExitStatus { .kind = ExitKind::Exited, .value = 0 });
    rig.advance(1h);
    CHECK(rig.fake->kills().empty());
    CHECK(rig.supervisor->counters().reaped == 1);
    CHECK(rig.supervisor->counters().uncleanExits == 0);
    CHECK(rig.supervisor->liveProcesses() == 0);
}

TEST_CASE("Supervisor.idleNeverWakes", "[vtextension]")
{
    auto rig = Rig {};
    auto const helper = rig.launch();
    rig.settle();
    CHECK(rig.fake->lastDeadline() == std::nullopt);
}

TEST_CASE("Supervisor.stopIsBounded", "[vtextension]")
{
    auto rig = Rig {};
    auto helpers = std::vector<SupervisedHelper> {};
    for ([[maybe_unused]] auto const index: { 0, 1, 2 })
        helpers.push_back(rig.launch());
    rig.fake->survivesKill(2);
    auto stopping = std::async(std::launch::async, [&] { rig.supervisor.reset(); });

    // The reap budget is the manual clock's: it passes as the test moves the clock on, and wakes the
    // supervisor to look.
    auto rounds = 0;
    while (stopping.wait_for(10ms) != std::future_status::ready && ++rounds < 100)
    {
        rig.clock.advance(ShutdownReapBudget);
        helpers.front().nudge();
    }
    REQUIRE(stopping.wait_for(1s) == std::future_status::ready);
    CHECK(rig.fake->kills().size() == 3);
}

TEST_CASE("Supervisor.mailboxOutlivesTheSupervisor", "[vtextension]")
{
    auto rig = Rig {};
    auto const mailbox = rig.supervisor->mailbox();
    auto helper = rig.launch();
    rig.supervisor.reset();
    CHECK(rig.fake->kills() == std::vector<std::size_t> { 0 }); // the supervisor's own, as it stopped

    helper.kill();
    {
        auto const watch = helper.watch(HostStep::Load, 1ms);
    }
    helper.attach(rig.countingDeaths());
    helper.nudge();
    {
        auto const dropped = std::move(helper);
    }
    CHECK(rig.fake->kills().size() == 1);
    CHECK(rig.deaths->load() == 0);
    CHECK(mailbox->launch(fakeSpec()).error() == LaunchError::Stopped);
}

TEST_CASE("Supervisor.failedWaitStopsEveryHelper", "[vtextension]")
{
    // The set cannot wait any more: the supervisor says so once, stops every helper and starts no more,
    // and still processes its commands until it is destroyed.
    auto rig = Rig {};
    auto helper = rig.launch();
    helper.attach(rig.countingDeaths());
    rig.supervisor->sync();
    rig.fake->failWaits(LaunchError::Io);
    rig.supervisor->sync();
    CHECK(killsFor(*rig.supervisor, KillReason::Shutdown) == 1);
    CHECK(rig.deaths->load() == 1);
    CHECK(rig.supervisor->counters().reaped == 1);
    CHECK(rig.supervisor->counters().uncleanExits == 0);
    CHECK(rig.supervisor->mailbox()->launch(fakeSpec()).error() == LaunchError::Stopped);
    CHECK(rig.fake->launches().size() == 1);
}

TEST_CASE("Supervisor.moveHandsTheHelperOn", "[vtextension]")
{
    auto rig = Rig {};
    auto first = rig.launch();
    auto second = std::move(first);
    rig.supervisor->sync();
    CHECK(rig.fake->kills().empty());
    second = rig.launch(); // lets the first go
    rig.supervisor->sync();
    CHECK(killsFor(*rig.supervisor, KillReason::Dropped) == 1);
    CHECK(rig.supervisor->liveProcesses() == 1);
}
