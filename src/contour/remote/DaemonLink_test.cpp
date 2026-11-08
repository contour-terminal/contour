// SPDX-License-Identifier: Apache-2.0
#include <contour/remote/DaemonLink.hpp>
#include <contour/remote/RoutingSessionFactory.hpp>
#include <contour/remote/testing/DaemonFixture.hpp>
#include <contour/session/TerminalSessionManager.hpp>
#include <contour/test/AppFixture.hpp>
#include <contour/test/GuiTestFixtures.hpp>
#include <contour/window/WindowController.hpp>

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <cstdint>
#include <memory>
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

    std::vector<std::unique_ptr<contour::test::ScopedController>> opened; ///< Every window opened, in order.
    std::vector<contour::remote::WindowAdoption> adoptions;               ///< What the link said to each.
    std::vector<vtworkspace::WorkspaceId> closed;                         ///< Every window asked closed.
    std::vector<contour::window::Notice> notices;
    int upgrades = 0;
    int restarts = 0;

  private:
    contour::session::TerminalSessionManager& _manager;
};

/// A test app whose sessions are created through a router, as the real app's are, so that the link's
/// controller can be made the delegate once the link exists.
struct LinkRig
{
    TestTimeline timeline;
    /// Declared before the app: the panes the manager holds unregister from the link's controller as they go.
    std::unique_ptr<contour::remote::DaemonLink> link;
    contour::remote::RoutingSessionFactory* router = nullptr;
    contour::test::TestApp app { makeRouter(router) };
    FakeLinkHost host { app.manager() };

    /// Builds the link to @p endpoint and routes the app's sessions to its controller.
    /// @param endpoint The daemon.
    /// @param hostLabel The host label the link is configured with.
    LinkRig(vthost::TcpEndpoint endpoint, std::string hostLabel = {})
    {
        link = std::make_unique<contour::remote::DaemonLink>(
            contour::remote::DaemonLink::Id { 1 },
            contour::remote::DaemonLinkConfig {
                .transport = std::make_shared<vthost::TlsTransportFactory>(std::move(endpoint)),
                .intent =
                    vthost::proto::AttachIntent { .kind = vthost::proto::AttachIntentKind::AllWorkspaces,
                                                  .workspaces = {} },
                .hostLabel = std::move(hostLabel),
                .locality = contour::window::DaemonLocality::Remote,
                .profileName = {},
                .workingDirectory = {},
                .attachSettings = std::nullopt,
                .sessionSettings = {},
                .colorPreference = std::nullopt,
                .clientRelease = "test",
                .clock = &core::platform::defaultSteadyClock(),
                .timeline = &timeline.timeline,
                .powerMonitor = nullptr,
                .backoff = {},
            },
            host,
            app.manager());
        router->setDelegate(link->liveController());
    }

    LinkRig(LinkRig const&) = delete;
    LinkRig& operator=(LinkRig const&) = delete;
    LinkRig(LinkRig&&) = delete;
    LinkRig& operator=(LinkRig&&) = delete;

    ~LinkRig()
    {
        link->stop();
        router->setDelegate(nullptr);
        host.opened.clear();
        drainGuiEvents();
    }

  private:
    [[nodiscard]] static std::unique_ptr<contour::session::SessionFactory> makeRouter(
        contour::remote::RoutingSessionFactory*& observed)
    {
        auto router = std::make_unique<contour::remote::RoutingSessionFactory>(
            std::make_unique<contour::test::MockPtySessionFactory>());
        observed = router.get();
        return router;
    }
};

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
