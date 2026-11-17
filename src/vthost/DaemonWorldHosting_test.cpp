// SPDX-License-Identifier: Apache-2.0
//
// How a SessionHost makes and stops its daemon-scoped extensions: through a fake, in a build with
// extensions or without.
#include <vtbackend/screen/Settings.hpp>
#include <vtbackend/screen/Terminal.hpp>

#include <vtpty/MockPty.hpp>

#include <core/Environment.hpp>
#include <core/net/EventLoop.hpp>
#include <core/net/IoBackend.hpp>

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <format>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <ranges>
#include <string>
#include <tuple>
#include <vector>

#include <vthost/ExtensionHosting.hpp>
#include <vthost/SessionHost.hpp>
#include <vthost/testing/FakeDaemonWorldHosting.hpp>

using vthost::testing::FakeDaemonWorldHosting;

namespace
{

/// @return A host on @p loop, its sessions on mock PTYs set up as @p settings says, with @p daemonWorld.
[[nodiscard]] std::unique_ptr<vthost::SessionHost> hostOn(core::net::EventLoop& loop,
                                                          vthost::DaemonWorldFactory daemonWorld,
                                                          vtbackend::Settings settings = {})
{
    return std::make_unique<vthost::SessionHost>(
        loop,
        [](vtbackend::PageSize size, std::optional<vtpty::Process::ExecInfo> const&) {
            return std::make_unique<vtpty::MockPty>(size);
        },
        std::move(settings),
        core::defaultEnvironment(),
        vthost::SessionHostConfig { .pumping = vthost::SessionPumping::Manual,
                                    .daemonWorld = std::move(daemonWorld) });
}

/// @return Settings whose indicator status line is @p left alone.
[[nodiscard]] vtbackend::Settings showingIndicator(std::string left)
{
    auto settings = vtbackend::Settings {};
    settings.statusDisplayType = vtbackend::StatusDisplayType::Indicator;
    settings.indicatorStatusLine.left = std::move(left);
    settings.indicatorStatusLine.middle.clear();
    settings.indicatorStatusLine.right.clear();
    return settings;
}

/// @return The indicator status line's row of @p terminal, trailing blanks trimmed.
[[nodiscard]] std::string rowOf(vtbackend::Terminal& terminal)
{
    auto const _ = std::lock_guard { terminal };
    return terminal.indicatorStatusLineDisplay().grid().lineAt(vtbackend::LineOffset(0)).toUtf8Trimmed();
}

/// A client mirroring the screens, counting each session's updates.
struct Mirror final: vthost::SessionStreamEvents
{
    std::map<uint64_t, int> updates;
    void sessionScreenUpdated(vtworkspace::SessionId session) override { ++updates[session.value]; }
};

/// A host whose daemon world is a fake, two sessions showing `orchestrator`'s summary, and one that does not.
struct SummaryHarness
{
    SummaryHarness()
    {
        for ([[maybe_unused]] auto const _: std::views::iota(0, 2))
        {
            auto const* tab = host->createTab();
            REQUIRE(tab != nullptr);
            showing.push_back(tab->rootPane()->session());
        }
    }

    /// Turns the loop once, running what was posted to it.
    void turn() { std::ignore = loop.runOnce(std::chrono::milliseconds { 0 }); }

    std::unique_ptr<core::net::IoBackend> source = core::net::makeDefaultBackend();
    core::net::EventLoop loop { *source };
    std::shared_ptr<std::vector<std::string>> journal = std::make_shared<std::vector<std::string>>();
    std::shared_ptr<FakeDaemonWorldHosting> world;
    std::unique_ptr<vthost::SessionHost> host = hostOn(
        loop,
        [this](vthost::DaemonWorldEvents events, vthost::OwnerPost post) {
            world = std::make_shared<FakeDaemonWorldHosting>(journal, std::move(post), std::move(events));
            return world;
        },
        showingIndicator("[{DaemonExtension:orchestrator}]"));
    std::vector<vtworkspace::SessionId> showing;
};

} // namespace

TEST_CASE("SessionHost: the daemon world is made once, and stopped before the loop drains",
          "[daemon][extensions]")
{
    auto const source = core::net::makeDefaultBackend();
    auto loop = core::net::EventLoop { *source };
    auto const journal = std::make_shared<std::vector<std::string>>();
    auto made = std::shared_ptr<FakeDaemonWorldHosting> {};
    auto calls = 0;
    auto host = hostOn(loop, [&](vthost::DaemonWorldEvents const& /*events*/, vthost::OwnerPost post) {
        ++calls;
        made = std::make_shared<FakeDaemonWorldHosting>(journal, std::move(post));
        return made;
    });
    CHECK(calls == 1);
    REQUIRE(made != nullptr);

    // What it posts runs on the loop.
    made->post("first");
    CHECK(journal->empty());
    std::ignore = loop.runOnce(std::chrono::milliseconds { 0 });
    CHECK(*journal == std::vector<std::string> { "first" });
    CHECK(calls == 1);

    // A job still on its way when the host goes is drained after the extensions stopped.
    made->post("late");
    host.reset();
    CHECK(*journal == std::vector<std::string> { "first", "stop", "late" });
    CHECK(calls == 1);
}

TEST_CASE("SessionHost: an empty daemon-world factory constructs nothing", "[daemon][extensions]")
{
    auto const source = core::net::makeDefaultBackend();
    auto loop = core::net::EventLoop { *source };
    auto host = hostOn(loop, {});
    // A host with daemon-scoped extensions must not go during a turn of its loop: it drains the loop as it
    // goes (a checked precondition). One without them has nothing to stop or drain, so it may.
    loop.post([&host] { host.reset(); });
    std::ignore = loop.runOnce(std::chrono::milliseconds { 0 });
    CHECK(host == nullptr);
}

TEST_CASE("SessionHost: a published summary repaints every row that shows it", "[daemon][summary]")
{
    auto h = SummaryHarness {};
    auto mirror = Mirror {};
    h.host->subscribeStream(&mirror);
    for (auto const session: h.showing)
        CHECK(rowOf(*h.host->terminal(session)) == "[]");

    SECTION("each session's indicator row holds the text after one loop turn")
    {
        h.world->publishSummary("orchestrator", "agents 1/4");
        for (auto const session: h.showing)
            CHECK(rowOf(*h.host->terminal(session)) == "[]");
        h.turn();
        for (auto const session: h.showing)
        {
            CHECK(rowOf(*h.host->terminal(session)) == "[agents 1/4]");
            CHECK(mirror.updates[session.value] == 1);
        }
    }

    SECTION("ten publishes before the turn are one redraw")
    {
        for (auto const i: std::views::iota(1, 11))
            h.world->publishSummary("orchestrator", std::format("agents {}/4", i));
        h.turn();
        h.turn();
        for (auto const session: h.showing)
        {
            CHECK(rowOf(*h.host->terminal(session)) == "[agents 10/4]");
            CHECK(mirror.updates[session.value] == 1);
        }
    }

    h.host->unsubscribeStream(&mirror);
}

TEST_CASE("SessionHost: a session not showing the summary is not repainted for it", "[daemon][summary]")
{
    auto const source = core::net::makeDefaultBackend();
    auto loop = core::net::EventLoop { *source };
    auto const journal = std::make_shared<std::vector<std::string>>();
    auto world = std::shared_ptr<FakeDaemonWorldHosting> {};
    auto host = hostOn(
        loop,
        [&](vthost::DaemonWorldEvents events, vthost::OwnerPost post) {
            world = std::make_shared<FakeDaemonWorldHosting>(journal, std::move(post), std::move(events));
            return world;
        },
        showingIndicator("{VTType}"));
    auto const* tab = host->createTab();
    REQUIRE(tab != nullptr);
    auto mirror = Mirror {};
    host->subscribeStream(&mirror);
    world->publishSummary("orchestrator", "agents 1/4");
    std::ignore = loop.runOnce(std::chrono::milliseconds { 0 });
    CHECK(mirror.updates.empty());
    host->unsubscribeStream(&mirror);
}

TEST_CASE("SessionHost: extension status is the daemon world's", "[daemon][summary]")
{
    auto h = SummaryHarness {};
    h.world->statuses.emplace("orchestrator",
                              vthost::DaemonExtensionStatus { .state = vthost::DaemonExtensionState::Running,
                                                              .summary = "agents 1/4",
                                                              .detail = "#1 running",
                                                              .problem = {} });
    auto* const control = h.host->daemonExtensionControl();
    REQUIRE(control != nullptr);
    CHECK(control->status("orchestrator") == h.world->statuses.at("orchestrator"));
    CHECK_FALSE(control->status("nope").has_value());
}
