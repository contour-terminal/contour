// SPDX-License-Identifier: Apache-2.0
#include <contour/remote/DaemonLink.hpp>
#include <contour/remote/testing/DaemonFixture.hpp>
#include <contour/session/TerminalSessionManager.hpp>
#include <contour/test/AppFixture.hpp>
#include <contour/test/GuiTestFixtures.hpp>
#include <contour/window/WindowController.hpp>

#include <QtCore/QPointer>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <expected>
#include <memory>
#include <optional>
#include <ranges>
#include <span>
#include <string>
#include <vector>

#include <vthost/Transport.hpp>
#include <vtworkspace/Pane.hpp>
#include <vtworkspace/SessionModel.hpp>
#include <vtworkspace/Tab.hpp>

using namespace std::chrono_literals;
using namespace contour::remote::testing;

namespace
{

/// An application that records what a link asks of it. A window it opens is a manager-minted controller that
/// claims the link's staged daemon workspace at once, as a spawned window's Main.qml does while it loads.
class FakeLinkHost final: public contour::remote::IDaemonLinkHost
{
  public:
    explicit FakeLinkHost(contour::session::TerminalSessionManager& manager): _manager(manager) {}

    void openWindowFor(contour::remote::DaemonLink& link, QScreen* /*screen*/) override
    {
        auto& window = *opened.emplace_back(std::make_unique<contour::test::ScopedController>(_manager));
        adoptions.push_back(link.bindPendingWindow(*window.controller));
    }
    void closeWindow(vtworkspace::WorkspaceId window) override { closed.push_back(window); }
    void postNotice(contour::remote::DaemonLink& /*link*/, contour::window::Notice notice) override
    {
        notices.push_back(std::move(notice));
    }
    void upgradeDaemon(contour::remote::DaemonLink& /*link*/) override { ++upgrades; }
    void restartDaemon(contour::remote::DaemonLink& /*link*/) override { ++restarts; }
    void linkEnded(contour::remote::DaemonLink& link) override { ended.push_back(&link); }

    std::vector<std::unique_ptr<contour::test::ScopedController>> opened; ///< Every window opened, in order.
    std::vector<contour::remote::WindowAdoption> adoptions;               ///< What the link said to each.
    std::vector<vtworkspace::WorkspaceId> closed;                         ///< Every window asked closed.
    std::vector<contour::window::Notice> notices;
    int upgrades = 0;
    int restarts = 0;
    std::vector<contour::remote::DaemonLink*> ended; ///< Every linkEnded(), in order.

  private:
    contour::session::TerminalSessionManager& _manager;
};

/// The emulation settings a link states, whose history limit is @p historyLines.
/// @param historyLines The history limit.
[[nodiscard]] vtbackend::Settings settingsWithHistory(int historyLines)
{
    auto settings = vtbackend::Settings {};
    settings.historyLimits.capacity = vtbackend::LineCount(historyLines);
    return settings;
}

/// What a test link to @p endpoint is built with.
/// @param endpoint The daemon.
/// @param timeline What the link's controller stamps.
/// @param hostLabel The host label the link is configured with.
/// @param sessionSettings Asked for the link's session settings; empty for none.
[[nodiscard]] contour::remote::DaemonLinkConfig linkConfig(
    vthost::TcpEndpoint endpoint,
    TestTimeline& timeline,
    std::string hostLabel = {},
    contour::remote::NativeController::SessionSettingsSource sessionSettings = {})
{
    // A link with a source states what it says from the first hello on, as the application's does.
    auto attachSettings = std::optional<vtbackend::Settings> {};
    if (sessionSettings)
        if (auto resolved = sessionSettings())
            attachSettings = *std::move(resolved);
    return contour::remote::DaemonLinkConfig {
        .transport = std::make_shared<vthost::TlsTransportFactory>(std::move(endpoint)),
        .intent = vthost::proto::AttachIntent { .kind = vthost::proto::AttachIntentKind::AllWorkspaces,
                                                .workspaces = {} },
        .hostLabel = std::move(hostLabel),
        .locality = contour::window::DaemonLocality::Remote,
        .profileName = {},
        .workingDirectory = {},
        .attachSettings = std::move(attachSettings),
        .sessionSettings = std::move(sessionSettings),
        .colorPreference = std::nullopt,
        .clientRelease = "test",
        .clock = &core::platform::defaultSteadyClock(),
        .timeline = &timeline.timeline,
        .powerMonitor = nullptr,
        .backoff = {},
    };
}

/// A test app and a link on it: the windows the link adopts make their sessions through its controller, the
/// others through the app's mock factory.
struct LinkRig
{
    TestTimeline timeline;
    /// Declared before the app: the panes the manager holds unregister from the link's controller as they go.
    std::unique_ptr<contour::remote::DaemonLink> link;
    contour::test::TestApp app { std::make_unique<contour::test::MockPtySessionFactory>() };
    FakeLinkHost host { app.manager() };

    /// Builds the link to @p endpoint.
    /// @param endpoint The daemon.
    /// @param hostLabel The host label the link is configured with.
    /// @param sessionSettings Asked for the link's session settings; empty for none.
    explicit LinkRig(vthost::TcpEndpoint endpoint,
                     std::string hostLabel = {},
                     contour::remote::NativeController::SessionSettingsSource sessionSettings = {})
    {
        // In the body: the link is declared before the app and host it is built on.
        link = std::make_unique<contour::remote::DaemonLink>(
            contour::remote::DaemonLink::Id { 1 },
            linkConfig(std::move(endpoint), timeline, std::move(hostLabel), std::move(sessionSettings)),
            host,
            app.manager());
    }

    LinkRig(LinkRig const&) = delete;
    LinkRig& operator=(LinkRig const&) = delete;
    LinkRig(LinkRig&&) = delete;
    LinkRig& operator=(LinkRig&&) = delete;

    ~LinkRig()
    {
        if (link)
            link->stop();
        host.opened.clear();
        drainGuiEvents();
    }
};

/// @return The sessions @p window shows, in tab order.
[[nodiscard]] std::vector<contour::session::TerminalSession*> sessionsOf(
    contour::session::TerminalSessionManager& manager, vtworkspace::WorkspaceId window)
{
    auto sessions = std::vector<contour::session::TerminalSession*> {};
    if (auto const* const workspace = manager.model().workspace(window))
        for (auto const index: std::views::iota(0, workspace->tabCount()))
            for (auto* const session: manager.sessionsOfTab(workspace->tabAt(index)))
                sessions.push_back(session);
    return sessions;
}

/// @return The sessions @p window shows, watched for their destruction.
[[nodiscard]] std::vector<QPointer<contour::session::TerminalSession>> watchedSessionsOf(
    contour::session::TerminalSessionManager& manager, vtworkspace::WorkspaceId window)
{
    auto watched = std::vector<QPointer<contour::session::TerminalSession>> {};
    for (auto* const session: sessionsOf(manager, window))
        watched.emplace_back(session);
    REQUIRE(!watched.empty());
    return watched;
}

/// Ends each closed session still alive, as its exit watcher does once it sees the closed device: no
/// session of a test runs one (none is started).
/// @param sessions The sessions of a window that was closed.
void endClosedSessions(std::span<QPointer<contour::session::TerminalSession> const> sessions)
{
    for (auto const& session: sessions)
        if (session)
            session->onClosed();
}

/// Seeds the daemon with @p count workspaces of one tab each.
/// @return Their ids, ascending.
std::vector<uint64_t> seedWorkspaces(DaemonFixture& daemon, int count)
{
    std::ignore = daemon.seedSession("first");
    return daemon.onDaemon([&daemon, count] {
        auto ids = std::vector<uint64_t> { daemon.host->workspaceId().value };
        for ([[maybe_unused]] auto const each: std::views::iota(1, count))
            ids.push_back(daemon.host->createWorkspace()->id().value);
        return ids;
    });
}

} // namespace

TEST_CASE("DaemonLink: start connects and reconcile asks the host for one window per further workspace",
          "[attach][link]")
{
    auto daemon = DaemonFixture {};
    std::ignore = seedWorkspaces(daemon, 3);
    auto rig = LinkRig { daemon.endpoint(), "home" };
    auto& link = *rig.link;
    auto const boot = contour::test::ScopedController { rig.app.manager() };

    link.start();
    // The label is the attach's only once it settled.
    CHECK(link.hostLabel().empty());
    REQUIRE(link.awaitAttached(10s).has_value());
    CHECK(link.isAttached());
    CHECK(link.hostLabel() == "home");

    // The layouts arrive, and each reconcile the link runs on them maps the primary workspace onto the boot
    // window and asks for a window for each of the other two.
    REQUIRE(waitUntil([&] { return rig.host.opened.size() == 2; }));
    link.reconcile(); // idempotent: nothing more is opened
    CHECK(rig.host.opened.size() == 2);
    CHECK(rig.host.adoptions
          == std::vector { contour::remote::WindowAdoption::Adopted,
                           contour::remote::WindowAdoption::Adopted });
    auto const windows = std::vector<vtworkspace::WorkspaceId>(link.windows().begin(), link.windows().end());
    CHECK(windows == std::vector { boot.id, rig.host.opened[0]->id, rig.host.opened[1]->id });
    // Every window it serves is bound to it, until it stops.
    for (auto const window: windows)
        CHECK(rig.app.manager().bindingOf(window) == &link);
    link.stop();
    for (auto const window: windows)
        CHECK(rig.app.manager().bindingOf(window) == nullptr);
}

TEST_CASE("DaemonLink: lost workspaces close their windows", "[attach][link][reconnect]")
{
    auto daemon = DaemonFixture {};
    auto const workspaces = seedWorkspaces(daemon, 2);
    auto rig = LinkRig { daemon.endpoint() };
    auto& link = *rig.link;
    auto const boot = contour::test::ScopedController { rig.app.manager() };
    link.start();
    REQUIRE(link.awaitAttached(10s).has_value());
    REQUIRE(waitUntil([&] { return rig.host.opened.size() == 1; }));
    auto const lostWindow = rig.host.opened.front()->id;

    // Closed on the daemon while this client was away: its only session ended.
    daemon.whileDisconnected([&] {
        auto* workspace = daemon.host->model().workspace(vtworkspace::WorkspaceId { workspaces[1] });
        daemon.host->handleSessionExit(workspace->tabAt(0)->rootPane()->session());
    });
    REQUIRE(waitUntil([&] { return !rig.host.closed.empty(); }, 1000));
    CHECK(rig.host.closed == std::vector { lostWindow });
}

TEST_CASE("DaemonLink: stop is idempotent and stops the controller", "[attach][link]")
{
    auto daemon = DaemonFixture {};
    std::ignore = seedWorkspaces(daemon, 1);
    auto rig = LinkRig { daemon.endpoint() };
    auto& link = *rig.link;
    link.start();
    REQUIRE(link.awaitAttached(10s).has_value());
    REQUIRE(daemon.waitUntilOnDaemon([&] { return daemon.live.size() == 1; }));

    link.stop();
    link.stop();
    // The daemon saw this client go.
    CHECK(daemon.waitUntilOnDaemon([&] { return daemon.live.empty(); }));
}

TEST_CASE("DaemonLink: a connect that fails is given up, and adopts no window", "[attach][link]")
{
    auto daemon = DaemonFixture {};
    daemon.refuseConnections();
    auto rig = LinkRig { daemon.endpoint(), "home" };
    auto& link = *rig.link;
    auto const window = contour::test::ScopedController { rig.app.manager() };
    link.start();

    CHECK_FALSE(link.awaitAttached(100ms).has_value());
    CHECK_FALSE(link.isAttached());
    CHECK(link.liveController() == nullptr);
    CHECK(link.hostLabel().empty());
    CHECK(link.bindPendingWindow(*window.controller) == contour::remote::WindowAdoption::Declined);
    link.stop(); // nothing left to stop
}

TEST_CASE("DaemonLink: closing its last window stops and releases the link", "[attach][link]")
{
    auto daemon = DaemonFixture {};
    std::ignore = seedWorkspaces(daemon, 1);
    auto rig = LinkRig { daemon.endpoint(), "home" };
    auto& link = *rig.link;
    // A local window stays open: the link's last window is not the process's.
    auto const local = contour::test::ScopedController { rig.app.manager() };
    auto const boot = contour::test::ScopedController { rig.app.manager() };
    link.start();
    REQUIRE(link.awaitAttached(10s).has_value());
    REQUIRE(link.bindPendingWindow(*boot.controller) == contour::remote::WindowAdoption::Adopted);
    REQUIRE(waitUntil([&] { return !sessionsOf(rig.app.manager(), boot.id).empty(); }));
    REQUIRE(daemon.waitUntilOnDaemon([&] { return daemon.live.size() == 1; }));
    auto const panes = watchedSessionsOf(rig.app.manager(), boot.id);

    boot.controller->closeWindow();

    // It stopped at once: it serves nothing, shows no host, and the daemon saw this client go.
    CHECK(rig.host.ended == std::vector { &link });
    CHECK(link.windows().empty());
    CHECK_FALSE(link.isAttached());
    CHECK(link.hostLabel().empty());
    CHECK(rig.app.manager().bindingOf(boot.id) == nullptr);
    CHECK(daemon.waitUntilOnDaemon([&] { return daemon.live.empty(); }));
    // Not destroyed yet: the closed window's pane still holds its pty.
    drainGuiEvents();
    CHECK_FALSE(link.isFinished());
    CHECK(rig.host.ended.size() == 1);
    // It may be destroyed once the closed window's panes let its controller go, and asks again then.
    endClosedSessions(panes);
    CHECK(waitUntil([&] {
        drainGuiEvents();
        return link.isFinished();
    }));
    CHECK(rig.host.ended.size() >= 2);
    CHECK(std::ranges::all_of(rig.host.ended, [&](auto const* ended) { return ended == &link; }));
}

TEST_CASE("DaemonLink: closing one of two windows keeps the link serving the other", "[attach][link]")
{
    auto daemon = DaemonFixture {};
    std::ignore = seedWorkspaces(daemon, 2);
    auto rig = LinkRig { daemon.endpoint(), "home" };
    auto& link = *rig.link;
    auto const boot = contour::test::ScopedController { rig.app.manager() };
    link.start();
    REQUIRE(link.awaitAttached(10s).has_value());
    REQUIRE(link.bindPendingWindow(*boot.controller) == contour::remote::WindowAdoption::Adopted);
    REQUIRE(waitUntil([&] { return rig.host.opened.size() == 1; }));
    auto const second = rig.host.opened.front()->id;
    REQUIRE(link.windows().size() == 2);

    rig.host.opened.front()->controller->closeWindow();
    drainGuiEvents();

    CHECK(rig.host.ended.empty());
    CHECK(std::vector(link.windows().begin(), link.windows().end()) == std::vector { boot.id });
    CHECK(link.isAttached());
    CHECK_FALSE(link.isFinished());
    CHECK(rig.app.manager().bindingOf(boot.id) == &link);
    CHECK(rig.app.manager().bindingOf(second) == nullptr);
    CHECK(rig.app.manager().hostLabelOf(boot.id) == "home");
    CHECK(daemon.onDaemon([&] { return daemon.live.size(); }) == 1);
}

TEST_CASE("DaemonLink: what its controller signals after stop() is ignored", "[attach][link][reconnect]")
{
    using vthost::client::ConnectionState;
    auto daemon = DaemonFixture {};
    std::ignore = seedWorkspaces(daemon, 1);
    auto rig = LinkRig { daemon.endpoint() };
    auto& link = *rig.link;
    auto& manager = rig.app.manager();
    auto const boot = contour::test::ScopedController { manager };
    link.start();
    REQUIRE(link.awaitAttached(10s).has_value());
    REQUIRE(link.bindPendingWindow(*boot.controller) == contour::remote::WindowAdoption::Adopted);
    drainGuiEvents();
    auto const* const controller = link.liveController();
    REQUIRE(controller != nullptr);

    // The connection goes, and what the controller says of it is still queued when the link stops.
    daemon.refuseConnections();
    REQUIRE(waitUntil(
        [&] { return controller->connectionState() != ConnectionState::Connected; }, 400, GuiEvents::Held));
    link.stop();
    drainGuiEvents();

    // Nothing was kept for the stopped link: a binding at its address starts out connected, as any does.
    manager.bindWindow(boot.id, link);
    CHECK(manager.hostConnection(boot.id) == ConnectionState::Connected);
    manager.releaseBinding(link);
}

TEST_CASE("DaemonLink: a link destroyed unstopped releases its windows", "[attach][link]")
{
    auto daemon = DaemonFixture {};
    auto rig = LinkRig { daemon.endpoint() };
    auto& manager = rig.app.manager();
    auto const window = contour::test::ScopedController { manager };
    manager.bindWindow(window.id, *rig.link);
    REQUIRE(manager.bindingOf(window.id) == rig.link.get());

    rig.link.reset();

    CHECK(manager.bindingOf(window.id) == nullptr);
}

TEST_CASE("DaemonLink: a configuration reload restates the session settings", "[attach][link][reconnect]")
{
    using vthost::client::ConnectionState;
    auto daemon = DaemonFixture {};
    std::ignore = seedWorkspaces(daemon, 1);
    auto historyLines = 1000;
    auto rig = LinkRig { daemon.endpoint(),
                         {},
                         [&historyLines]() -> std::expected<vtbackend::Settings, std::string> {
                             return settingsWithHistory(historyLines);
                         } };
    auto& link = *rig.link;
    link.start();
    REQUIRE(link.awaitAttached(10s).has_value());

    historyLines = 2000;
    link.onConfigurationReloaded();

    // The next connection's hello states the reloaded settings, not those of the launch.
    auto const* const controller = link.liveController();
    daemon.dropConnections();
    REQUIRE(waitUntil([&] { return controller->connectionState() == ConnectionState::Reconnecting; }));
    REQUIRE(waitUntil([&] { return daemon.clientFrames.helloHistories().size() >= 2; }));
    auto const hellos = daemon.clientFrames.helloHistories();
    CHECK(hellos.front() == 1000);
    CHECK(hellos.back() == 2000);
}

TEST_CASE("ContourGuiApp: config reload reaches every link", "[contour][app][attach][link][reconnect]")
{
    using vthost::client::ConnectionState;
    auto daemonA = DaemonFixture {};
    auto daemonB = DaemonFixture {};
    std::ignore = seedWorkspaces(daemonA, 1);
    std::ignore = seedWorkspaces(daemonB, 1);
    auto timelineA = TestTimeline {};
    auto timelineB = TestTimeline {};
    auto historyLines = 1000;
    auto const settings = [&historyLines]() -> std::expected<vtbackend::Settings, std::string> {
        return settingsWithHistory(historyLines);
    };
    auto app = contour::test::TestApp { std::make_unique<contour::test::MockPtySessionFactory>() };
    auto& linkA = app.app().startDaemonLink(linkConfig(daemonA.endpoint(), timelineA, {}, settings));
    auto& linkB = app.app().startDaemonLink(linkConfig(daemonB.endpoint(), timelineB, {}, settings));
    REQUIRE(linkA.awaitAttached(10s).has_value());
    REQUIRE(linkB.awaitAttached(10s).has_value());

    historyLines = 2000;
    app.app().onConfigurationReloaded();

    // Each link's next hello states the reloaded settings, the second link's as much as the first's.
    for (auto* const daemon: { &daemonA, &daemonB })
    {
        daemon->dropConnections();
        REQUIRE(waitUntil([&] { return daemon->clientFrames.helloHistories().size() >= 2; }));
        CHECK(daemon->clientFrames.helloHistories().front() == 1000);
        CHECK(daemon->clientFrames.helloHistories().back() == 2000);
    }
}

TEST_CASE("ContourGuiApp: a link that ended is destroyed once its panes are gone",
          "[contour][app][attach][link]")
{
    auto daemon = DaemonFixture {};
    std::ignore = seedWorkspaces(daemon, 1);
    auto timeline = TestTimeline {};
    auto app = contour::test::TestApp { std::make_unique<contour::test::MockPtySessionFactory>() };
    auto& link = app.app().startDaemonLink(linkConfig(daemon.endpoint(), timeline, "home"));
    // A local window stays open: the link's last window is not the process's.
    auto const local = contour::test::ScopedController { app.manager() };
    auto const window = contour::test::ScopedController { app.manager() };
    REQUIRE(link.awaitAttached(10s).has_value());
    REQUIRE(link.bindPendingWindow(*window.controller) == contour::remote::WindowAdoption::Adopted);
    REQUIRE(waitUntil([&] { return !sessionsOf(app.manager(), window.id).empty(); }));
    REQUIRE(app.app().daemonLinks().size() == 1);
    auto const panes = watchedSessionsOf(app.manager(), window.id);

    window.controller->closeWindow();
    drainGuiEvents();
    // Kept while the closed window's pane holds its pty, destroyed once it is gone.
    CHECK(app.app().daemonLinks().size() == 1);
    endClosedSessions(panes);
    CHECK(waitUntil([&] {
        drainGuiEvents();
        return app.app().daemonLinks().empty();
    }));
    CHECK(daemon.waitUntilOnDaemon([&] { return daemon.live.empty(); }));
}

TEST_CASE("the attach boot window adopts the primary daemon workspace", "[contour][app][attach]")
{
    using contour::remote::primaryDaemonWorkspaceToAdopt;

    // The boot window (no OS window mapped yet) adopts the primary — lowest-id — daemon workspace,
    // so Main.qml adopts it instead of authoring a spurious fresh tab on the daemon.
    CHECK(primaryDaemonWorkspaceToAdopt(/*anyWindowMapped=*/false, { 7, 12, 30 }) == 7);

    // A later OS window (one already mapped) is NOT the boot window — it adopts nothing here (it is
    // handled by the staged pending-workspace path instead).
    CHECK_FALSE(primaryDaemonWorkspaceToAdopt(/*anyWindowMapped=*/true, { 7, 12 }).has_value());

    // No daemon workspace reported yet: nothing to bind now — the caller still claims the boot window
    // and the link's reconcile binds it once the first layout arrives.
    CHECK_FALSE(primaryDaemonWorkspaceToAdopt(/*anyWindowMapped=*/false, {}).has_value());
}
