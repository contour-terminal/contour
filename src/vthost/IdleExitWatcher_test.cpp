// SPDX-License-Identifier: Apache-2.0
#include <vtpty/MockPty.hpp>

#include <core/net/EventLoop.hpp>
#include <core/net/IoBackend.hpp>
#include <core/platform/Clock.hpp>

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <memory>
#include <tuple>

#include <vthost/ClientCensus.hpp>
#include <vthost/IdleExitWatcher.hpp>
#include <vthost/SessionHost.hpp>
#include <vtworkspace/Pane.hpp>
#include <vtworkspace/Tab.hpp>

using namespace std::chrono_literals;

namespace
{

/// An empty host on a loop whose clock the case advances, plus the census the watcher counts clients in.
struct IdleHarness
{
    core::platform::ManualClock clock {};
    std::unique_ptr<core::net::IoBackend> source = core::net::makeDefaultBackend();
    core::net::EventLoop loop { *source, clock };
    vthost::SessionHost host { loop,
                               [](vtbackend::PageSize size, std::optional<vtpty::Process::ExecInfo> const&) {
                                   return std::make_unique<vtpty::MockPty>(size);
                               },
                               vtbackend::Settings {},
                               core::defaultEnvironment(),
                               vthost::SessionHostConfig { .pumping = vthost::SessionPumping::Manual } };
    vthost::ClientCensus census;

    /// Creates a tab and returns its only pane's session.
    [[nodiscard]] vtworkspace::SessionId createSession()
    {
        auto* tab = host.createTab();
        REQUIRE(tab != nullptr);
        return tab->rootPane()->session();
    }

    void pass(std::chrono::seconds seconds)
    {
        clock.advance(seconds);
        std::ignore = loop.runUntilIdle();
    }
};

} // namespace

TEST_CASE("daemonActivity is idle only with no sessions and no clients", "[vthost][idle]")
{
    STATIC_CHECK(vthost::daemonActivity(0, 0) == vthost::DaemonActivity::Idle);
    STATIC_CHECK(vthost::daemonActivity(1, 0) == vthost::DaemonActivity::Busy);
    STATIC_CHECK(vthost::daemonActivity(0, 1) == vthost::DaemonActivity::Busy);
}

TEST_CASE("an idle daemon exits once the grace elapses", "[vthost][idle]")
{
    auto h = IdleHarness {};
    auto exits = 0;
    auto watcher = vthost::IdleExitWatcher { h.host, h.census, h.loop, 10s, [&] { ++exits; } };
    CHECK(watcher.isArmed());
    h.pass(9s);
    CHECK(exits == 0);
    h.pass(1s);
    CHECK(exits == 1);
    CHECK_FALSE(watcher.isArmed());
}

TEST_CASE("a client attaching within the grace keeps the daemon", "[vthost][idle]")
{
    auto h = IdleHarness {};
    auto exits = 0;
    auto watcher = vthost::IdleExitWatcher { h.host, h.census, h.loop, 10s, [&] { ++exits; } };
    auto const ticket = h.census.admit();
    CHECK_FALSE(watcher.isArmed());
    h.pass(60s);
    CHECK(exits == 0);
}

TEST_CASE("an attach in the turn the deadline fires keeps the daemon", "[vthost][idle]")
{
    auto h = IdleHarness {};
    auto exits = 0;
    auto watcher = vthost::IdleExitWatcher { h.host, h.census, h.loop, 10s, [&] { ++exits; } };
    h.clock.advance(10s);
    auto const ticket = h.census.admit();
    std::ignore = h.loop.runUntilIdle();
    CHECK(exits == 0);
}

TEST_CASE("a session created after the deadline passed keeps the daemon", "[vthost][idle]")
{
    // Creating a session fires no event the watcher hears, so only the re-check at fire time sees it.
    auto h = IdleHarness {};
    auto exits = 0;
    auto watcher = vthost::IdleExitWatcher { h.host, h.census, h.loop, 10s, [&] { ++exits; } };
    h.clock.advance(10s);
    std::ignore = h.createSession();
    std::ignore = h.loop.runUntilIdle();
    CHECK(exits == 0);
}

TEST_CASE("the last client leaving a daemon with no sessions re-arms the grace", "[vthost][idle]")
{
    auto h = IdleHarness {};
    auto exits = 0;
    auto watcher = vthost::IdleExitWatcher { h.host, h.census, h.loop, 10s, [&] { ++exits; } };
    {
        auto const ticket = h.census.admit();
        CHECK_FALSE(watcher.isArmed());
    }
    CHECK(watcher.isArmed());
    h.pass(10s);
    CHECK(exits == 1);
}

TEST_CASE("a closed session re-arms only one pump later", "[vthost][idle]")
{
    auto h = IdleHarness {};
    auto exits = 0;
    auto const session = h.createSession();
    auto watcher = vthost::IdleExitWatcher { h.host, h.census, h.loop, 10s, [&] { ++exits; } };
    CHECK_FALSE(watcher.isArmed());
    h.host.handleSessionExit(session);
    CHECK_FALSE(watcher.isArmed());
    std::ignore = h.loop.runOnce();
    CHECK(watcher.isArmed());
}

TEST_CASE("a watcher is not a screen mirror", "[vthost][idle]")
{
    auto h = IdleHarness {};
    auto const watcher = vthost::IdleExitWatcher { h.host, h.census, h.loop, 10s, [] {} };
    CHECK_FALSE(watcher.mirrorsScreens());
}
