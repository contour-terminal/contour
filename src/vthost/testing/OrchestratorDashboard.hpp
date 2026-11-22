// SPDX-License-Identifier: Apache-2.0
#pragma once

/// @file
/// The orchestrator's dashboard as the end-to-end cases open it from a window, and the upgrade and recovery
/// they put it through: on POSIX, where the C++ guests are built.

#include <core/async/Task.hpp>
#include <core/net/EventLoop.hpp>
#include <core/net/testing/CoroTestSupport.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <ranges>
#include <string>
#include <string_view>
#include <tuple>
#include <utility>
#include <variant>
#include <vector>

#include <vthost/SessionHost.hpp>
#include <vthost/handover/HandoverRestore.hpp>
#include <vthost/handover/HandoverToken.hpp>
#include <vthost/testing/OrchestratorWorld.hpp>
#include <vthost/testing/ServedEndToEnd.hpp>
#include <vthost/testing/ServedTesting.hpp>

namespace vthost::testing
{

/// The longest a window waits, in 1 ms loop ticks, for what it counts on.
inline constexpr auto WaitTicks = 60'000;

/// @return A host that takes over @p image, its daemon world @p world's.
[[nodiscard]] inline std::unique_ptr<LiveHost> takeOver(handover::HandoverImage image,
                                                        OrchestratorWorld& world)
{
    auto config = world.hostConfig();
    config.handover = std::move(image);
    return std::make_unique<LiveHost>(world.ptys(), std::move(config));
}

/// @return Whether @p page shows the dashboard: the Fleet's rounded box, titled with its count of runs.
[[nodiscard]] inline bool showsDashboard(std::string_view page)
{
    return std::ranges::any_of(pageLines(page), [](std::string const& line) {
        return line.starts_with("╭─ Agents ") && line.ends_with("╮");
    });
}

/// @return The session of the served tab in @p host's workspace @p workspace, if there is one.
[[nodiscard]] inline std::optional<vtworkspace::SessionId> servedIn(SessionHost const& host,
                                                                    uint64_t workspace)
{
    for (auto const id: host.sessionIds())
    {
        auto const* const holder = std::get<0>(host.model().findSessionLeaf(id));
        if (host.servedBy(id) != nullptr && holder != nullptr && holder->id().value == workspace)
            return id;
    }
    return std::nullopt;
}

/// @return How many times @p log was told to reveal @p session.
[[nodiscard]] inline std::size_t revealsOf(ClientLog const& log, vtworkspace::SessionId session)
{
    return static_cast<std::size_t>(std::ranges::count_if(log.extensionEvents, [&](auto const& event) {
        auto const* const reveal = std::get_if<proto::RevealPane>(&event);
        return reveal != nullptr && reveal->session == session.value;
    }));
}

/// The orchestrator's dashboard as a window opened it.
struct OpenedDashboard
{
    std::optional<vtworkspace::SessionId> session; ///< Its tab's session, once it showed the dashboard.
    uint64_t workspace = 0;                        ///< The window's workspace, which holds it.
    std::size_t questions = 0; ///< How many grant questions the window was asked before it opened it.
};

/// What a window does once it opened the dashboard.
enum class ThenOpen : uint8_t
{
    Leave = 0, ///< It leaves.
    Again,     ///< It invokes "open" again, waits until it is told to reveal the dashboard, and leaves.
};

/// What a window that opened the dashboard twice saw of the second "open".
struct SecondOpen
{
    std::size_t sessionsBefore = 0; ///< The daemon's sessions, as it invoked it.
    std::size_t sessionsAfter = 0;  ///< The daemon's sessions, once it was told to reveal the dashboard.
    std::size_t reveals = 0;        ///< How many reveals of the dashboard the second "open" brought.
};

/// The window's part: grants the orchestrator if it is asked, waits until its palette offers "open" with the
/// extension running, invokes it, and waits until its workspace's served tab shows the dashboard -- answering
/// every command the orchestrator runs meanwhile; then does what @p then says.
inline core::async::Task<void> openAndLeave(LiveHost* live,
                                            OrchestratorWorld* world,
                                            Connection* window,
                                            OpenedDashboard* opened,
                                            ThenOpen then,
                                            SecondOpen* second)
{
    auto const answering = [world] {
        std::ignore = world->processRunner.answerWaiting();
    };
    auto const offered = [&] {
        answering();
        return offersRunning(window->log, OrchestratorName, "open");
    };
    std::ignore = co_await core::net::testing::waitUntil(
        &live->loop, [&] { return offered() || !window->log.requests.empty(); }, WaitTicks);
    if (!window->log.requests.empty())
    {
        auto const& request = window->log.requests.front();
        window->client->answerDaemonExtension(request.name, request.grantDigest, GrantState::Granted);
        std::ignore = co_await core::net::testing::waitUntil(&live->loop, offered, WaitTicks);
    }
    opened->questions = window->log.requests.size();
    opened->workspace = window->client->selectedWorkspace();

    window->client->invokeExtensionAction(std::string(OrchestratorName), "open");
    std::ignore = co_await core::net::testing::waitUntil(
        &live->loop,
        [&] {
            answering();
            auto const session = servedIn(live->host, opened->workspace);
            if (session && showsDashboard(live->pageText(*session)))
                opened->session = session;
            return opened->session.has_value();
        },
        WaitTicks);

    if (then == ThenOpen::Again && opened->session)
    {
        auto const revealed = revealsOf(window->log, *opened->session);
        second->sessionsBefore = live->host.sessionCount();
        window->client->invokeExtensionAction(std::string(OrchestratorName), "open");
        std::ignore = co_await core::net::testing::waitUntil(
            &live->loop,
            [&] {
                answering();
                return revealsOf(window->log, *opened->session) > revealed;
            },
            WaitTicks);
        second->reveals = revealsOf(window->log, *opened->session) - revealed;
        second->sessionsAfter = live->host.sessionCount();
    }
    window->client->detach();
}

/// Opens the orchestrator's dashboard from a new window on @p live, as "Open Agents" in its palette does.
/// @param intent Which workspace the window asks to be shown.
/// @param then What the window does once the dashboard is drawn.
/// @param second Where what it saw of a second "open" is written, for ThenOpen::Again.
/// @return The dashboard, once it is drawn.
[[nodiscard]] inline OpenedDashboard openDashboard(LiveHost& live,
                                                   OrchestratorWorld& world,
                                                   proto::AttachIntent intent = {},
                                                   ThenOpen then = ThenOpen::Leave,
                                                   SecondOpen* second = nullptr)
{
    auto window = world.connect(live.loop, live.host, ClientKind::Window, intent);
    auto opened = OpenedDashboard {};
    live.loop.blockOn(
        core::net::testing::allOf(window->server->run(),
                                  window->client->run(),
                                  openAndLeave(&live, &world, window.get(), &opened, then, second)));
    REQUIRE(opened.session.has_value());
    CHECK(live.host.servedBy(*opened.session) != nullptr);
    return opened;
}

/// @return Whether @p before, a daemon world @p world made, was replaced by another instance, its own
/// guest's.
[[nodiscard]] inline bool reinstantiated(std::weak_ptr<vthost::DaemonWorldExtensions> const& before,
                                         OrchestratorWorld const& world)
{
    // Owner order compares the control blocks, which @p before keeps from being reused.
    return before.expired() && !world.current.expired()
           && (before.owner_before(world.current) || world.current.owner_before(before));
}

/// Spawns two shells side by side in one tab of @p live.
/// @return Their sessions, once both are drawn.
[[nodiscard]] inline std::vector<vtworkspace::SessionId> spawnShells(LiveHost& live)
{
    live.spawnTwo();
    auto shells = live.host.sessionIds();
    REQUIRE(live.eventually([&] {
        return std::ranges::all_of(
            shells, [&](vtworkspace::SessionId id) { return live.pageText(id).contains("ready"); });
    }));
    return shells;
}

/// Checks that @p live's @p shells take input and draw it.
inline void checkShellsAnswer(LiveHost& live, std::vector<vtworkspace::SessionId> const& shells)
{
    for (auto const id: shells)
        live.host.writeInput(id, "after-the-dashboard");
    REQUIRE(live.eventually([&] {
        return std::ranges::all_of(shells, [&](vtworkspace::SessionId id) {
            return live.pageText(id).contains("after-the-dashboard");
        });
    }));
}

/// Upgrades @p u's daemon, its dashboard @p dashboard open, up to the exec; then the old image goes, its
/// guest with it, as the exec ends them.
/// @return The image the new daemon takes over.
[[nodiscard]] inline handover::HandoverImage upgradeWithDashboard(ServedUpgrade& u,
                                                                  vtworkspace::SessionId dashboard)
{
    auto recorder = EndRecorder {};
    auto subscription = std::optional<ScopedStreamSubscription> {};
    subscription.emplace(
        u.live->host, recorder, &SessionHost::subscribeStream, &SessionHost::unsubscribeStream);
    auto image = std::optional<handover::HandoverImage> {};
    u.replacer.onReplace = [&] {
        auto const& [program, argv] = u.replacer.replacements.back();
        auto const fd = handover::handoverTokenOf(argv.back()).and_then(handover::parseHandoverDescriptor);
        REQUIRE(fd.has_value());
        auto decoded = handover::readHandover(u.memory, *fd);
        REQUIRE(decoded.has_value());
        image = *std::move(decoded);
        releaseForAdoption(u.live->host);
        throw ImageReplaced {};
    };
    auto upgrade = u.orchestrator(u.handoff);
    REQUIRE_THROWS_AS(std::ignore = u.run(upgrade, handover::UpgradeTarget { .imageId = u.imageId }),
                      ImageReplaced);

    // The upgrade closed the dashboard, with a close of its own kind, and captured everything else.
    CHECK(std::ranges::contains(recorder.ends, std::pair { dashboard, SessionEnd::Upgrade }));
    REQUIRE(image.has_value());
    CHECK_FALSE(std::ranges::contains(image->sessions, dashboard, &handover::HandoverSession::id));
    u.replacer.onReplace = {};
    return *std::move(image);
}

} // namespace vthost::testing
