// SPDX-License-Identifier: Apache-2.0
#include <contour/command/ContextMenu.hpp>
#include <contour/config/Actions.hpp>
#include <contour/remote/DaemonLink.hpp>
#include <contour/remote/SwitchGrants.hpp>
#include <contour/remote/testing/DaemonFixture.hpp>
#include <contour/session/TerminalSessionManager.hpp>
#include <contour/test/AppFixture.hpp>
#include <contour/test/GuiTestFixtures.hpp>
#include <contour/window/WindowController.hpp>

#include <QtCore/QPointer>

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <expected>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <ranges>
#include <span>
#include <string>
#include <string_view>
#include <tuple>
#include <vector>

#include <vthost/Transport.hpp>
#include <vthost/WorkspaceCapture.hpp>
#include <vthost/WorkspaceVisibility.hpp>
#include <vtworkspace/Pane.hpp>
#include <vtworkspace/SessionModel.hpp>
#include <vtworkspace/Tab.hpp>

using namespace std::chrono_literals;
using namespace contour::remote::testing;

namespace
{

/// An application that records what a link asks of it. A window it opens is a manager-minted controller that
/// asks the attach-window binder at once, as a spawned window's Main.qml does while it loads. A link it is
/// asked to start for a hand-over it owns, and tells the link it came from how that went, as the
/// application does.
class FakeLinkHost final: public contour::remote::IDaemonLinkHost
{
  public:
    /// @param manager The manager the windows live in.
    /// @param handOverLinks Where the links it starts are kept: declared before the manager's app, as their
    ///        windows' panes unregister from them as they go.
    FakeLinkHost(contour::session::TerminalSessionManager& manager,
                 std::vector<std::unique_ptr<contour::remote::DaemonLink>>& handOverLinks):
        handOverLinks(handOverLinks), _manager(manager)
    {
    }

    FakeLinkHost(FakeLinkHost const&) = delete;
    FakeLinkHost& operator=(FakeLinkHost const&) = delete;
    FakeLinkHost(FakeLinkHost&&) = delete;
    FakeLinkHost& operator=(FakeLinkHost&&) = delete;

    ~FakeLinkHost() override
    {
        // While the manager the links serve windows of exists.
        for (auto const& link: handOverLinks)
            link->stop();
    }

    void startHandOverLink(contour::remote::DaemonLinkConfig config) override
    {
        if (adjustHandOver)
            adjustHandOver(config);
        handOverLinks.push_back(std::make_unique<contour::remote::DaemonLink>(
            contour::remote::DaemonLink::Id { _nextId++ }, std::move(config), *this, _manager));
        contour::remote::passOnLaunchResume(links());
    }

    void handOverSettled(contour::remote::HandOver handOver,
                         contour::remote::HandOverOutcome outcome,
                         std::string const& reason) override
    {
        outcomes.push_back(outcome);
        if (auto* const from = linkById(handOver.from))
            from->completeHandOver(handOver.workspace, outcome, reason);
    }

    /// @return The link of @p id, the launch's or one this host started; null for none.
    [[nodiscard]] contour::remote::DaemonLink* linkById(contour::remote::DaemonLink::Id id) const
    {
        if (launchLink != nullptr && launchLink->id() == id)
            return launchLink;
        auto const found = std::ranges::find(handOverLinks, id, &contour::remote::DaemonLink::id);
        return found != handOverLinks.end() ? found->get() : nullptr;
    }

    void openWindowFor(QScreen* /*screen*/) override
    {
        auto& window = *opened.emplace_back(std::make_unique<contour::test::ScopedController>(_manager));
        adoptions.push_back(_manager.consumeAttachWindow(window.controller)
                                ? contour::remote::WindowAdoption::Adopted
                                : contour::remote::WindowAdoption::Declined);
    }
    void upgradeDaemon(contour::remote::DaemonLink& /*link*/) override { ++upgrades; }
    void restartDaemon(contour::remote::DaemonLink& /*link*/) override { ++restarts; }
    void linkEnded() override
    {
        ++endings;
        // As the application does.
        contour::remote::passOnLaunchResume(links());
    }
    [[nodiscard]] contour::remote::SwitchGrants& switchGrants() override { return grants; }
    /// Never fires: no question of these tests expires.
    [[nodiscard]] contour::remote::SwitchDeadline const& switchDeadline() override { return _deadline; }

    /// @return The launch's link, then those this host started, in the order they were made.
    [[nodiscard]] std::vector<contour::remote::DaemonLink*> links() const
    {
        auto all = std::vector<contour::remote::DaemonLink*> {};
        if (launchLink != nullptr)
            all.push_back(launchLink);
        for (auto const& link: handOverLinks)
            all.push_back(link.get());
        return all;
    }

    contour::remote::SwitchGrants grants; ///< The answers that stand for the switches programs ask for.
    std::vector<std::unique_ptr<contour::test::ScopedController>> opened; ///< Every window opened, in order.
    std::vector<contour::remote::WindowAdoption> adoptions;               ///< What the binder said to each.
    /// The links started for hand-overs, in order.
    std::vector<std::unique_ptr<contour::remote::DaemonLink>>& handOverLinks;
    std::vector<contour::remote::HandOverOutcome> outcomes; ///< How each reported hand-over went.
    /// The link the test built itself, told how its hand-overs went; null for none.
    contour::remote::DaemonLink* launchLink = nullptr;
    /// Changes what a hand-over link is built with, before it is; empty for nothing.
    std::function<void(contour::remote::DaemonLinkConfig&)> adjustHandOver;
    int upgrades = 0;
    int restarts = 0;
    int endings = 0; ///< How often linkEnded() was told.

  private:
    contour::session::TerminalSessionManager& _manager;
    uint32_t _nextId = 100;
    contour::remote::SwitchDeadline _deadline {
        .timeout = contour::remote::DefaultSwitchTimeout,
        .timer = [](std::chrono::milliseconds /*delay*/, std::function<void()> const& /*fire*/) {},
        .clock = core::platform::defaultSteadyClock(),
    };
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
/// @param intent What the link's first connection follows.
/// @param address The daemon as a switch names it.
[[nodiscard]] contour::remote::DaemonLinkConfig linkConfig(
    vthost::TcpEndpoint endpoint,
    TestTimeline& timeline,
    std::string hostLabel = {},
    contour::remote::NativeController::SessionSettingsSource sessionSettings = {},
    vthost::proto::AttachIntent intent = { .kind = vthost::proto::AttachIntentKind::AllWorkspaces,
                                           .workspaces = {} },
    contour::remote::DaemonAddress address = {})
{
    // A link with a source states what it says from the first hello on, as the application's does.
    auto attachSettings = std::optional<vtbackend::Settings> {};
    if (sessionSettings)
        if (auto resolved = sessionSettings())
            attachSettings = *std::move(resolved);
    return contour::remote::DaemonLinkConfig {
        .controller = controllerConfig(std::move(endpoint),
                                       timeline,
                                       ControllerOptions { .sessionSettings = std::move(sessionSettings),
                                                           .attachSettings = std::move(attachSettings),
                                                           .intent = std::move(intent) }),
        .hostLabel = std::move(hostLabel),
        .locality = contour::window::DaemonLocality::Remote,
        .clientRelease = "test",
        .handOverFrom = std::nullopt,
        .address = std::move(address),
        .arrival = std::nullopt,
    };
}

/// A test app and a link on it: the windows the link adopts make their sessions through its controller, the
/// others through the app's mock factory.
struct LinkRig
{
    TestTimeline timeline;
    /// Declared before the app: the panes the manager holds unregister from the link's controller as they go.
    std::unique_ptr<contour::remote::DaemonLink> link;
    /// The links the host starts for hand-overs; declared before the app, as @c link is.
    std::vector<std::unique_ptr<contour::remote::DaemonLink>> handOverLinks;
    contour::test::TestApp app { std::make_unique<contour::test::MockPtySessionFactory>() };
    FakeLinkHost host { app.manager(), handOverLinks };

    /// Builds the link to @p endpoint.
    /// @param endpoint The daemon.
    /// @param hostLabel The host label the link is configured with.
    /// @param sessionSettings Asked for the link's session settings; empty for none.
    /// @param intent What the link's first connection follows.
    /// @param address The daemon as a switch names it.
    explicit LinkRig(
        vthost::TcpEndpoint endpoint,
        std::string hostLabel = {},
        contour::remote::NativeController::SessionSettingsSource sessionSettings = {},
        vthost::proto::AttachIntent intent = { .kind = vthost::proto::AttachIntentKind::AllWorkspaces,
                                               .workspaces = {} },
        contour::remote::DaemonAddress address = {})
    {
        // In the body: the link is declared before the app and host it is built on.
        link = std::make_unique<contour::remote::DaemonLink>(contour::remote::DaemonLink::Id { 1 },
                                                             linkConfig(std::move(endpoint),
                                                                        timeline,
                                                                        std::move(hostLabel),
                                                                        std::move(sessionSettings),
                                                                        std::move(intent),
                                                                        std::move(address)),
                                                             host,
                                                             app.manager());
        host.launchLink = link.get();
    }

    LinkRig(LinkRig const&) = delete;
    LinkRig& operator=(LinkRig const&) = delete;
    LinkRig(LinkRig&&) = delete;
    LinkRig& operator=(LinkRig&&) = delete;

    ~LinkRig()
    {
        if (link)
            link->stop();
        for (auto const& handedTo: host.handOverLinks)
            handedTo->stop();
        host.opened.clear();
        drainGuiEvents();
    }
};

/// @return The sessions @p window shows, in tab order.
[[nodiscard]] std::vector<contour::session::TerminalSession*> sessionsOf(
    contour::session::TerminalSessionManager& manager, vtworkspace::WorkspaceId window)
{
    return manager.sessionsOfWindow(window);
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

/// @return What a plain launch's first connection follows.
[[nodiscard]] vthost::proto::AttachIntent launch()
{
    return vthost::proto::AttachIntent { .kind = vthost::proto::AttachIntentKind::Launch, .workspaces = {} };
}

/// Has the daemon remember @p workspace as switched away from, by a client that showed it.
void hide(DaemonFixture& daemon, uint64_t workspace)
{
    daemon.onDaemon([&daemon, workspace] {
        daemon.host->noteWorkspaceReleased(vtworkspace::WorkspaceId { workspace },
                                           vthost::proto::WorkspaceRelease::SwitchedAway);
        return 0;
    });
}

/// @return How many of the daemon's connections show @p workspace. Daemon thread.
[[nodiscard]] std::size_t showing(DaemonFixture& daemon, uint64_t workspace)
{
    return daemon.host->clientsShowing(vtworkspace::WorkspaceId { workspace }).size();
}

/// @return Whether @p workspace is shown, left or hidden on the daemon.
[[nodiscard]] vthost::WorkspaceVisibility visibilityOn(DaemonFixture& daemon, uint64_t workspace)
{
    return daemon.onDaemon(
        [&daemon, workspace] { return daemon.host->visibility(vtworkspace::WorkspaceId { workspace }); });
}

/// What a hand-over link that cannot reach its daemon is built with: nothing listens on port 1.
void unreachable(contour::remote::DaemonLinkConfig& config)
{
    config.controller.transport = std::make_shared<vthost::TlsTransportFactory>(
        vthost::TcpEndpoint { .host = "127.0.0.1", .port = 1, .token = {}, .caPem = {} });
}

/// A daemon that cannot change what a connection shows: one that predates workspace-follow.
[[nodiscard]] vthost::NativeSessionServices withoutWorkspaceFollow()
{
    auto services = vthost::NativeSessionServices {};
    std::erase(services.handshake.capabilities.idents,
               std::to_underlying(vthost::proto::Capability::WorkspaceFollow));
    return services;
}

/// Serves the first connection it is given, through @p serve, and closes every later one.
/// @param serve What serves a connection.
/// @param seen How many connections came so far.
core::async::Task<void> serveFirstOnly(vthost::ConnectionHandler serve,
                                       std::shared_ptr<int> seen,
                                       vthost::ConnectionId id,
                                       std::unique_ptr<core::net::ISocket> socket)
{
    if ((*seen)++ >= 1)
    {
        socket->close();
        co_return;
    }
    co_await serve(std::move(id), std::move(socket));
}

} // namespace

TEST_CASE("DaemonLink: a launch brings back each left workspace in a window on a link of its own",
          "[attach][link][handover]")
{
    using vthost::WorkspaceVisibility;
    auto daemon = DaemonFixture {};
    auto const ids = seedWorkspaces(daemon, 3);
    // The third was switched away from: a launch does not bring it back.
    hide(daemon, ids[2]);
    auto rig = LinkRig { daemon.endpoint(), "home", {}, launch() };
    auto& link = *rig.link;
    auto& manager = rig.app.manager();
    auto const boot = contour::test::ScopedController { manager };
    REQUIRE(link.awaitAttached(10s).has_value());

    // The first left workspace adopts the boot window; the second is handed to a link of its own, which
    // opens its window and then has the first link let the workspace go.
    REQUIRE(waitUntil([&] { return rig.host.outcomes.size() == 1; }));
    CHECK(rig.host.outcomes == std::vector { contour::remote::HandOverOutcome::Settled });
    REQUIRE(rig.host.handOverLinks.size() == 1);
    auto& handedTo = *rig.host.handOverLinks.front();
    REQUIRE(rig.host.opened.size() == 1);
    CHECK(rig.host.adoptions == std::vector { contour::remote::WindowAdoption::Adopted });
    CHECK(std::ranges::to<std::vector>(manager.windowsBoundTo(link)) == std::vector { boot.id });
    CHECK(std::ranges::to<std::vector>(manager.windowsBoundTo(handedTo))
          == std::vector { rig.host.opened.front()->id });
    CHECK(manager.hostLabelOf(rig.host.opened.front()->id) == "home");

    // One connection per window, each following its window's workspace only; the hidden one stays hidden.
    CHECK(daemon.waitUntilOnDaemon([&] {
        return showing(daemon, ids[0]) == 1 && showing(daemon, ids[1]) == 1 && showing(daemon, ids[2]) == 0;
    }));
    CHECK(waitUntil([&] { return link.liveController()->workspaceIds() == std::vector { ids[0] }; }));
    CHECK(handedTo.liveController()->workspaceIds() == std::vector { ids[1] });
    CHECK(visibilityOn(daemon, ids[1]) == WorkspaceVisibility::Shown);
    CHECK(visibilityOn(daemon, ids[2]) == WorkspaceVisibility::Hidden);
    CHECK(daemon.onDaemon([&daemon] { return daemon.live.size(); }) == 2);
}

TEST_CASE("DaemonLink: a launch while a workspace is handed over does not take it",
          "[attach][link][handover]")
{
    auto daemon = DaemonFixture {};
    auto const ids = seedWorkspaces(daemon, 2);
    auto rig = LinkRig { daemon.endpoint(), {}, {}, launch() };
    auto& link = *rig.link;
    auto const boot = contour::test::ScopedController { rig.app.manager() };
    REQUIRE(link.awaitAttached(10s).has_value());
    // The hand-over link's connection is accepted, and its first layout held back.
    daemon.holdNewConnectionsAtLayout();
    REQUIRE(waitUntil([&] { return rig.host.handOverLinks.size() == 1; }));
    REQUIRE(daemon.waitUntilOnDaemon([&] { return showing(daemon, ids[1]) == 2; }));
    CHECK(rig.host.outcomes.empty());

    // Another launch meanwhile finds every workspace shown, and gets one of its own.
    auto secondTimeline = TestTimeline {};
    auto const second =
        makeNativeController(daemon.endpoint(), secondTimeline, ControllerOptions { .intent = launch() });
    second->startConnecting();
    REQUIRE(daemon.waitUntilOnDaemon([&daemon] { return daemon.host->model().workspaceCount() == 3; }));
    CHECK(daemon.onDaemon([&] { return showing(daemon, ids[0]) == 1 && showing(daemon, ids[1]) == 2; }));

    // Only once the hand-over link has the layout does the first link let the workspace go.
    daemon.releaseHeld();
    REQUIRE(waitUntil([&] { return rig.host.outcomes.size() == 1; }));
    CHECK(rig.host.outcomes == std::vector { contour::remote::HandOverOutcome::Settled });
    CHECK(daemon.waitUntilOnDaemon([&] { return showing(daemon, ids[1]) == 1; }));
    second->stop();
}

TEST_CASE("DaemonLink: a hand-over that fails leaves the workspace on the first link, in a window of its own",
          "[attach][link][handover]")
{
    using vthost::WorkspaceVisibility;
    auto daemon = DaemonFixture {};
    auto const ids = seedWorkspaces(daemon, 2);
    auto rig = LinkRig { daemon.endpoint(), {}, {}, launch() };
    rig.host.adjustHandOver = unreachable;
    auto& link = *rig.link;
    auto& manager = rig.app.manager();
    auto const boot = contour::test::ScopedController { manager };
    REQUIRE(link.awaitAttached(10s).has_value());

    REQUIRE(waitUntil([&] { return rig.host.outcomes.size() == 1; }, 2000));
    CHECK(rig.host.outcomes == std::vector { contour::remote::HandOverOutcome::Failed });
    // The first link shows it in a window of its own, and still follows it.
    REQUIRE(waitUntil([&] { return rig.host.opened.size() == 1; }));
    CHECK(std::ranges::to<std::vector>(manager.windowsBoundTo(link))
          == std::vector { boot.id, rig.host.opened.front()->id });
    CHECK(daemon.onDaemon([&] { return showing(daemon, ids[1]); }) == 1);
    CHECK(visibilityOn(daemon, ids[1]) == WorkspaceVisibility::Shown);
    // The link that failed serves nothing, and may go.
    CHECK(waitUntil([&] {
        drainGuiEvents();
        return rig.host.handOverLinks.front()->isFinished();
    }));
    CHECK(rig.host.endings >= 1);
}

// Review (PR 3, item 7): a window closed while it switched to a new workspace failed the switch, but the
// workspace the daemon made for it arrived later all the same and got a window of its own.
TEST_CASE("DaemonLink: a window closed while it switches to a new workspace leaves that workspace hidden",
          "[attach][link][switch]")
{
    using vthost::WorkspaceVisibility;
    auto daemon = DaemonFixture {};
    std::ignore = seedWorkspaces(daemon, 2);
    auto rig = LinkRig { daemon.endpoint(), {}, {}, launch() };
    // The hand-over fails: the link serves both windows, and the boot window keeps it alive.
    rig.host.adjustHandOver = unreachable;
    auto& link = *rig.link;
    auto& manager = rig.app.manager();
    auto const boot = contour::test::ScopedController { manager };
    REQUIRE(link.awaitAttached(10s).has_value());
    REQUIRE(waitUntil([&] { return rig.host.opened.size() == 1; }, 2000));
    auto& second = *rig.host.opened.front();
    REQUIRE(waitUntil([&] { return link.workspaceOf(second.id).has_value(); }));
    REQUIRE(waitUntil([&] { return !sessionsOf(manager, second.id).empty(); }));

    // The next layout -- the new workspace's -- is held on its way.
    daemon.onDaemon([&daemon] {
        for (auto* const socket: daemon.gated)
            socket->holdAtNextLayout();
        return 0;
    });
    auto told = std::optional<std::expected<uint64_t, std::string>> {};
    REQUIRE(link.switchWindow(second.id,
                              0,
                              vthost::proto::WorkspaceRelease::Closed,
                              [&](auto landed) { told = std::move(landed); })
                .has_value());
    REQUIRE(daemon.waitUntilOnDaemon([&daemon] { return daemon.host->model().workspaceCount() == 3; }));
    auto const made = daemon.onDaemon([&daemon] { return daemon.host->model().workspaceAt(2)->id().value; });

    auto const panes = watchedSessionsOf(manager, second.id);
    second.controller->closeWindow();
    endClosedSessions(panes);
    REQUIRE(told.has_value());
    CHECK_FALSE(told->has_value());

    // The workspace arrives after the window went: let go of as hidden, and shown in no window.
    daemon.releaseHeld();
    CHECK(waitUntil([&] { return visibilityOn(daemon, made) == WorkspaceVisibility::Hidden; }, 2000));
    drainGuiEvents();
    CHECK(rig.host.opened.size() == 1);
    CHECK_FALSE(link.windowShowing(made).has_value());
}

TEST_CASE("DaemonLink: a closed window's workspace is left for a later launch", "[attach][link][handover]")
{
    using vthost::WorkspaceVisibility;
    auto daemon = DaemonFixture {};
    auto const ids = seedWorkspaces(daemon, 1);
    auto rig = LinkRig { daemon.endpoint(), {}, {}, launch() };
    auto& link = *rig.link;
    // A local window stays open: the link's last window is not the process's.
    auto const local = contour::test::ScopedController { rig.app.manager() };
    auto const boot = contour::test::ScopedController { rig.app.manager() };
    REQUIRE(link.awaitAttached(10s).has_value());
    REQUIRE(link.bindPendingWindow(*boot.controller) == contour::remote::WindowAdoption::Adopted);
    REQUIRE(waitUntil([&] { return !sessionsOf(rig.app.manager(), boot.id).empty(); }));
    auto const panes = watchedSessionsOf(rig.app.manager(), boot.id);

    boot.controller->closeWindow();

    // Told it was closed, not merely that the connection went.
    CHECK(daemon.waitUntilOnDaemon([&] {
        return daemon.host->lastRelease(vtworkspace::WorkspaceId { ids[0] })
               == vthost::proto::WorkspaceRelease::Closed;
    }));
    drainGuiEvents();
    CHECK(daemon.waitUntilOnDaemon([&] { return daemon.live.empty(); }));
    CHECK(visibilityOn(daemon, ids[0]) == WorkspaceVisibility::Left);
    endClosedSessions(panes);
    drainGuiEvents();
}

// Review (PR 3, item 4): with the launch's own window closed, the windows left are on links that resume
// exactly their workspaces. A daemon restarting with a restore numbers its workspaces anew, so those ids name
// other workspaces of the new run, or none: a window then followed an unrelated workspace, or closed with
// nothing in its place, and the last one closing quit the application.
TEST_CASE("DaemonLink: a daemon restarted with a restore keeps a window for every workspace it brought back",
          "[attach][link][handover][reconnect]")
{
    using vthost::WorkspaceVisibility;
    auto daemon = DaemonFixture {};
    auto const ids = seedWorkspaces(daemon, 3);
    auto rig = LinkRig { daemon.endpoint(), {}, {}, launch() };
    auto& link = *rig.link;
    auto& manager = rig.app.manager();
    auto boot = std::make_unique<contour::test::ScopedController>(manager);
    REQUIRE(link.awaitAttached(10s).has_value());
    REQUIRE(waitUntil([&] { return rig.host.outcomes.size() == 2; }));
    REQUIRE(rig.host.opened.size() == 2);
    REQUIRE(daemon.waitUntilOnDaemon([&] {
        return showing(daemon, ids[0]) == 1 && showing(daemon, ids[1]) == 1 && showing(daemon, ids[2]) == 1;
    }));

    // The launch's window closes: its workspace is left, and the other two windows go on.
    auto const bootPanes = watchedSessionsOf(manager, boot->id);
    boot->controller->closeWindow();
    endClosedSessions(bootPanes);
    REQUIRE(daemon.waitUntilOnDaemon([&] { return showing(daemon, ids[0]) == 0; }));
    drainGuiEvents();

    // The daemon restarts and restores the two workspaces still shown, numbered anew: the first of them is 1
    // now, which no window showed, and the second is 2, which the first window did.
    auto snapshot = daemon.onDaemon([&daemon] {
        return vthost::captureWorkspace(
            *daemon.host, { .scrollback = vthost::ScrollbackPersistence::Off }, std::chrono::sys_seconds {});
    });
    REQUIRE(snapshot.workspaces.size() == 3);
    snapshot.workspaces.erase(snapshot.workspaces.begin());
    daemon.restore = vthost::WorkspaceRestore { .snapshot = std::move(snapshot) };
    daemon.restartHost();

    // The application judges the roles at every link's start and end, so judge them again and again while
    // the links reconnect one after another -- the moment they disagree on the run is one of them.
    static_cast<void>(waitUntil(
        [&] {
            contour::remote::passOnLaunchResume(rig.host.links());
            return false;
        },
        100));

    // Each restored workspace gets a window again, and the ones the lost windows showed are closed.
    auto const restored = daemon.onDaemon([&daemon] {
        auto workspaces = std::vector<uint64_t> {};
        for (auto const index: std::views::iota(0, daemon.host->model().workspaceCount()))
            workspaces.push_back(daemon.host->model().workspaceAt(index)->id().value);
        return workspaces;
    });
    REQUIRE(restored.size() == 2);
    CHECK(daemon.waitUntilOnDaemon([&] {
        return std::ranges::all_of(restored,
                                   [&](uint64_t workspace) { return showing(daemon, workspace) == 1; });
    }));
    auto const openWindows = [&] {
        drainGuiEvents();
        return std::ranges::count_if(rig.host.opened, [&](auto const& window) {
            return manager.controllerFor(window->id) != nullptr;
        });
    };
    CHECK(waitUntil([&] { return openWindows() == 2; }, 3000));
    for (auto const workspace: restored)
        CHECK(visibilityOn(daemon, workspace) == WorkspaceVisibility::Shown);

    // Review (PR 3 re-review, N1/T1): the links reconnect one after another, so for a moment they have met
    // different runs of the one daemon; none but the first may then take the launch role, which would create
    // a workspace and a window of its own. Let every pending event drain, then count again.
    auto const workspacesOnDaemon = [&daemon] {
        return daemon.onDaemon([&daemon] { return daemon.host->model().workspaceCount(); });
    };
    CHECK(workspacesOnDaemon() == 2);
    CHECK(openWindows() == 2);
}

// Review (PR 3 re-review, N1): the links of one `--connect-tcp` daemon reconnect one after another after a
// restart, so for a moment one of them has met the new run while the rest still hold the old one. They are
// still one daemon: only one of them may take the launch role, else a second creates a workspace and a
// window of its own.
TEST_CASE("DaemonLink: links of one daemon keep one launch role while they reconnect to a restarted one",
          "[attach][link][handover][reconnect]")
{
    using Fallback = contour::remote::NativeController::ResumeFallback;
    // The daemon lets one connection in after its restart. A link that holds no launch role dies of the
    // lost workspace it meets then, which leaves nothing to compare; so the run goes again, with a fresh
    // daemon, until the one let in is the launch's holder -- a chance of one in three each.
    auto compared = false;
    for ([[maybe_unused]] auto const attempt: std::views::iota(0, 25))
    {
        auto daemon = DaemonFixture {};
        std::ignore = seedWorkspaces(daemon, 3);
        auto rig = LinkRig { daemon.endpoint(), {}, {}, launch() };
        auto const boot = contour::test::ScopedController { rig.app.manager() };
        REQUIRE(rig.link->awaitAttached(10s).has_value());
        REQUIRE(waitUntil([&] { return rig.host.outcomes.size() == 2; }));
        REQUIRE(daemon.waitUntilOnDaemon([&] { return daemon.live.size() == 3; }));
        auto const holders = [&] {
            return std::ranges::count_if(
                rig.host.links(), [](auto* link) { return link->resumeFallback() == Fallback::Launch; });
        };
        auto const links = rig.host.links();
        auto const holder = std::ranges::find_if(
            links, [](auto* link) { return link->resumeFallback() == Fallback::Launch; });
        REQUIRE(holder != links.end());
        REQUIRE(holders() == 1);

        daemon.refuseConnections();
        auto const letIn = std::make_shared<int>(0);
        daemon.onDaemon([&daemon, letIn] {
            ++daemon.instance;
            daemon.host = daemon.makeHost();
            auto serve = vthost::makeNativeHandler(daemon.loop, *daemon.host, daemon.services);
            daemon.native = [serve = std::move(serve), letIn](vthost::ConnectionId id,
                                                              std::unique_ptr<core::net::ISocket> socket) {
                return serveFirstOnly(serve, letIn, std::move(id), std::move(socket));
            };
            return 0;
        });
        REQUIRE(daemon.waitUntilOnDaemon([&] { return daemon.live.size() == 1; }));
        auto const newRun = [&](contour::remote::DaemonLink* link) {
            return link->liveController() != nullptr
                   && link->liveController()->daemonInstance() == std::optional<uint64_t> { daemon.instance };
        };
        REQUIRE(waitUntil([&] { return std::ranges::any_of(rig.host.links(), newRun); }));
        if (!newRun(*holder))
            continue;
        compared = true;

        // The application judges the roles as a link starts or ends.
        contour::remote::passOnLaunchResume(rig.host.links());
        CHECK(holders() == 1);
        break;
    }
    CHECK(compared);
}

TEST_CASE("DaemonLink: --workspace shows that workspace only", "[attach][link][handover]")
{
    auto daemon = DaemonFixture {};
    auto const ids = seedWorkspaces(daemon, 3);
    auto rig = LinkRig { daemon.endpoint(),
                         "home",
                         {},
                         { .kind = vthost::proto::AttachIntentKind::Workspaces, .workspaces = { ids[1] } } };
    auto& link = *rig.link;
    auto const boot = contour::test::ScopedController { rig.app.manager() };
    REQUIRE(link.awaitAttached(10s).has_value());
    REQUIRE(waitUntil([&] { return link.liveController()->workspaceIds() == std::vector { ids[1] }; }));
    drainGuiEvents();
    CHECK(rig.host.handOverLinks.empty());
    CHECK(rig.host.opened.empty());
    CHECK(daemon.onDaemon([&] { return showing(daemon, ids[0]) + showing(daemon, ids[2]); }) == 0);
}

TEST_CASE("DaemonLink: a second machine attaching gets the left workspaces, not every one",
          "[attach][link][handover]")
{
    auto daemon = DaemonFixture {};
    auto const ids = seedWorkspaces(daemon, 3);
    // Another client shows the first; the third was switched away from.
    auto otherTimeline = TestTimeline {};
    auto const other = makeNativeController(
        daemon.endpoint(),
        otherTimeline,
        ControllerOptions {
            .intent = { .kind = vthost::proto::AttachIntentKind::Workspaces, .workspaces = { ids[0] } } });
    REQUIRE(other->connectAndWait(10s).has_value());
    hide(daemon, ids[2]);

    // As a saved host attaches: a launch, not every workspace.
    auto rig = LinkRig { daemon.endpoint(), "home", {}, launch() };
    auto& link = *rig.link;
    auto const boot = contour::test::ScopedController { rig.app.manager() };
    REQUIRE(link.awaitAttached(10s).has_value());
    REQUIRE(waitUntil([&] { return link.liveController()->workspaceIds() == std::vector { ids[1] }; }));
    drainGuiEvents();
    CHECK(rig.host.handOverLinks.empty());
    CHECK(daemon.onDaemon([&] { return showing(daemon, ids[0]); }) == 1);
    CHECK(daemon.onDaemon([&] { return showing(daemon, ids[2]); }) == 0);
    other->stop();
}

TEST_CASE("DaemonLink: the chrome says which daemon a window is on, and the default says nothing",
          "[attach][link][chrome]")
{
    using contour::session::SwitchTarget;
    using vthost::proto::SwitchDaemonKind;
    auto daemon = DaemonFixture {};
    std::ignore = seedWorkspaces(daemon, 1);

    SECTION("a non-default local label")
    {
        auto rig =
            LinkRig { daemon.endpoint(), {}, {}, {}, { .kind = SwitchDaemonKind::Label, .name = "admin" } };
        CHECK(rig.link->chromeLabel().empty()); // not until the attach settled
        REQUIRE(rig.link->awaitAttached(10s).has_value());
        CHECK(rig.link->chromeLabel() == "admin");
        CHECK(rig.link->hostLabel().empty()); // still not a remote host: sessions keep running locally
        CHECK(rig.link->daemonTarget()
              == SwitchTarget { .kind = SwitchDaemonKind::Label, .daemon = "admin", .workspace = {} });
    }

    SECTION("the default local label")
    {
        auto rig =
            LinkRig { daemon.endpoint(), {}, {}, {}, { .kind = SwitchDaemonKind::Label, .name = "default" } };
        REQUIRE(rig.link->awaitAttached(10s).has_value());
        CHECK(rig.link->chromeLabel().empty());
        CHECK(rig.link->daemonTarget()->daemon == "default");
    }

    SECTION("a saved host")
    {
        auto rig =
            LinkRig { daemon.endpoint(), "home", {}, {}, { .kind = SwitchDaemonKind::Host, .name = "home" } };
        REQUIRE(rig.link->awaitAttached(10s).has_value());
        CHECK(rig.link->chromeLabel() == "home");
        CHECK(rig.link->daemonTarget()->kind == SwitchDaemonKind::Host);
    }
}

TEST_CASE("DaemonLink: a link connects as it is built, and reconcile asks the host for one window per "
          "further workspace on a daemon without workspace-follow",
          "[attach][link]")
{
    auto daemon = DaemonFixture { {}, withoutWorkspaceFollow() };
    std::ignore = seedWorkspaces(daemon, 3);
    auto rig = LinkRig { daemon.endpoint(), "home" };
    auto& link = *rig.link;
    auto const boot = contour::test::ScopedController { rig.app.manager() };

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
    auto const windows = std::ranges::to<std::vector>(rig.app.manager().windowsBoundTo(link));
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
    REQUIRE(link.awaitAttached(10s).has_value());
    REQUIRE(waitUntil([&] { return rig.host.opened.size() == 1; }));
    auto const lostWindow = rig.host.opened.front()->id;

    // Closed on the daemon while this client was away: its only session ended.
    daemon.whileDisconnected([&] {
        auto* workspace = daemon.host->model().workspace(vtworkspace::WorkspaceId { workspaces[1] });
        daemon.host->handleSessionExit(workspace->tabAt(0)->rootPane()->session());
    });
    // The window of a link of its own: that link's exact resume follows the emptied workspace, and loses it.
    REQUIRE(waitUntil([&] { return rig.app.manager().controllerFor(lostWindow) == nullptr; }, 1000));
    CHECK(rig.app.manager().controllerFor(boot.id) != nullptr);
}

TEST_CASE("DaemonLink: stop is idempotent and stops the controller", "[attach][link]")
{
    auto daemon = DaemonFixture {};
    std::ignore = seedWorkspaces(daemon, 1);
    auto rig = LinkRig { daemon.endpoint() };
    auto& link = *rig.link;
    REQUIRE(link.awaitAttached(10s).has_value());
    REQUIRE(daemon.waitUntilOnDaemon([&] { return daemon.live.size() == 1; }));

    link.stop();
    link.stop();
    // The daemon saw this client go.
    CHECK(daemon.waitUntilOnDaemon([&] { return daemon.live.empty(); }));
}

TEST_CASE("DaemonLink: a further link to the daemon states no working directory", "[attach][link][cwd]")
{
    // The directory a launch states opens a tab in the launch's own attach; a window a switch or a hand-over
    // opens later on another link of that daemon has nothing to open there.
    auto daemon = DaemonFixture {};
    auto timeline = TestTimeline {};
    auto config = linkConfig(daemon.endpoint(), timeline);
    config.controller.workingDirectory = std::filesystem::temp_directory_path();
    auto app = contour::test::TestApp { std::make_unique<contour::test::MockPtySessionFactory>() };
    auto handOverLinks = std::vector<std::unique_ptr<contour::remote::DaemonLink>> {};
    auto host = FakeLinkHost { app.manager(), handOverLinks };
    auto link = contour::remote::DaemonLink {
        contour::remote::DaemonLink::Id { 1 }, std::move(config), host, app.manager()
    };

    CHECK(link.siblingConfig().controller.workingDirectory.empty());
    link.stop();
}

TEST_CASE("DaemonLink: a further link to the daemon is built to ask for the color preference shown then",
          "[attach][link][colors]")
{
    // The launch's config is made before the OS's scheme is read; a link cloned from it later must not state
    // the preference of that moment, or the daemon builds every new session under it.
    auto daemon = DaemonFixture {};
    auto timeline = TestTimeline {};
    auto shown = std::atomic<std::optional<vtbackend::ColorPreference>> {};
    auto config = linkConfig(daemon.endpoint(), timeline);
    config.controller.colorPreference = [&shown] {
        return shown.load();
    };
    auto app = contour::test::TestApp { std::make_unique<contour::test::MockPtySessionFactory>() };
    auto handOverLinks = std::vector<std::unique_ptr<contour::remote::DaemonLink>> {};
    auto host = FakeLinkHost { app.manager(), handOverLinks };
    auto link = contour::remote::DaemonLink {
        contour::remote::DaemonLink::Id { 1 }, std::move(config), host, app.manager()
    };

    shown = vtbackend::ColorPreference::Light;
    auto const sibling = link.siblingConfig();
    REQUIRE(sibling.controller.colorPreference);
    CHECK(sibling.controller.colorPreference() == vtbackend::ColorPreference::Light);
    link.stop();
}

TEST_CASE("DaemonLink: a stopped link no longer owns the attach-window binder", "[attach][link]")
{
    auto daemon = DaemonFixture {};
    std::ignore = seedWorkspaces(daemon, 1);
    auto rig = LinkRig { daemon.endpoint() };
    auto& link = *rig.link;
    auto& manager = rig.app.manager();
    auto const boot = contour::test::ScopedController { manager };
    CHECK(manager.attachWindowBinderOwner() == &link);
    REQUIRE(link.awaitAttached(10s).has_value());
    CHECK(manager.attachWindowBinderOwner() == &link);

    link.stop();
    CHECK(manager.attachWindowBinderOwner() == nullptr);
    // A window spawned now is nobody's: it opens a first tab of its own, and stays local.
    auto const spawned = contour::test::ScopedController { manager };
    CHECK_FALSE(manager.consumeAttachWindow(spawned.controller));
    CHECK(manager.bindingOf(spawned.id) == nullptr);
}

TEST_CASE("DaemonLink: a connect that fails is given up, and adopts no window", "[attach][link]")
{
    auto daemon = DaemonFixture {};
    daemon.refuseConnections();
    auto rig = LinkRig { daemon.endpoint(), "home" };
    auto& link = *rig.link;
    auto const window = contour::test::ScopedController { rig.app.manager() };

    CHECK(rig.app.manager().attachWindowBinderOwner() == &link);
    CHECK_FALSE(link.awaitAttached(100ms).has_value());
    // Given up with the controller: a window spawned from now on opens a tab of its own.
    CHECK(rig.app.manager().attachWindowBinderOwner() == nullptr);
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
    REQUIRE(link.awaitAttached(10s).has_value());
    REQUIRE(link.bindPendingWindow(*boot.controller) == contour::remote::WindowAdoption::Adopted);
    REQUIRE(waitUntil([&] { return !sessionsOf(rig.app.manager(), boot.id).empty(); }));
    REQUIRE(daemon.waitUntilOnDaemon([&] { return daemon.live.size() == 1; }));
    auto const panes = watchedSessionsOf(rig.app.manager(), boot.id);
    auto const* const controller = link.liveController();
    REQUIRE(controller != nullptr);

    boot.controller->closeWindow();

    // It stopped at once: it serves nothing and shows no host.
    CHECK(rig.host.endings == 1);
    CHECK(rig.app.manager().windowsBoundTo(link).empty());
    CHECK_FALSE(link.isAttached());
    CHECK(link.hostLabel().empty());
    CHECK(rig.app.manager().bindingOf(boot.id) == nullptr);
    // Its controller's stop, which may wait for the daemon, is made off the GUI thread; the daemon sees
    // this client go.
    CHECK(waitUntil([&] {
        drainGuiEvents();
        return controller->connectionState() != vthost::client::ConnectionState::Connected;
    }));
    CHECK(daemon.waitUntilOnDaemon([&] { return daemon.live.empty(); }));
    // Not destroyed yet: the closed window's pane still holds its pty.
    drainGuiEvents();
    CHECK_FALSE(link.isFinished());
    CHECK(rig.host.endings == 1);
    // It may be destroyed once the closed window's panes let its controller go, and asks again then.
    endClosedSessions(panes);
    CHECK(waitUntil([&] {
        drainGuiEvents();
        return link.isFinished();
    }));
    CHECK(rig.host.endings >= 2);
}

// Review (PR 4, M4): closing the last window hands its controller's stop to the background; once the
// closed window's panes let go, the link ends and the controller has stopped. (That the close itself returns
// before the stop finishes is pinned deterministically by RemoteController_test's stalled controller.)
TEST_CASE("DaemonLink: closing its last window ends the link and stops its controller",
          "[attach][link][threading]")
{
    auto daemon = DaemonFixture {};
    std::ignore = seedWorkspaces(daemon, 1);
    auto rig = LinkRig { daemon.endpoint(), "home" };
    auto& link = *rig.link;
    auto const local = contour::test::ScopedController { rig.app.manager() };
    auto const boot = contour::test::ScopedController { rig.app.manager() };
    REQUIRE(link.awaitAttached(10s).has_value());
    REQUIRE(link.bindPendingWindow(*boot.controller) == contour::remote::WindowAdoption::Adopted);
    REQUIRE(waitUntil([&] { return !sessionsOf(rig.app.manager(), boot.id).empty(); }));
    auto const panes = watchedSessionsOf(rig.app.manager(), boot.id);
    auto const* const controller = link.liveController();
    REQUIRE(controller != nullptr);

    boot.controller->closeWindow();

    CHECK(waitUntil([&] {
        drainGuiEvents();
        return !controller->isWindingDown();
    }));
    endClosedSessions(panes);
    CHECK(waitUntil([&] {
        drainGuiEvents();
        return link.isFinished();
    }));
}

// What the application's teardown relies on: a link whose last window just went has only posted its
// controller's stop, and the teardown stops every link before that event may run.
TEST_CASE("DaemonLink: stop() stops the controller of a link that just ended", "[attach][link]")
{
    auto daemon = DaemonFixture {};
    std::ignore = seedWorkspaces(daemon, 1);
    auto rig = LinkRig { daemon.endpoint(), "home" };
    auto& link = *rig.link;
    auto const local = contour::test::ScopedController { rig.app.manager() };
    auto const boot = contour::test::ScopedController { rig.app.manager() };
    REQUIRE(link.awaitAttached(10s).has_value());
    REQUIRE(link.bindPendingWindow(*boot.controller) == contour::remote::WindowAdoption::Adopted);
    auto const* const controller = link.liveController();
    REQUIRE(controller != nullptr);
    boot.controller->closeWindow();
    REQUIRE(controller->connectionState() == vthost::client::ConnectionState::Connected);

    link.stop();

    CHECK(controller->connectionState() != vthost::client::ConnectionState::Connected);
    CHECK(daemon.waitUntilOnDaemon([&] { return daemon.live.empty(); }));
}

TEST_CASE("DaemonLink: closing one of two windows keeps the link serving the other", "[attach][link]")
{
    // One link serves two windows only where the daemon cannot let a connection stop showing a workspace.
    auto daemon = DaemonFixture { {}, withoutWorkspaceFollow() };
    std::ignore = seedWorkspaces(daemon, 2);
    auto rig = LinkRig { daemon.endpoint(), "home" };
    auto& link = *rig.link;
    auto const boot = contour::test::ScopedController { rig.app.manager() };
    REQUIRE(link.awaitAttached(10s).has_value());
    REQUIRE(link.bindPendingWindow(*boot.controller) == contour::remote::WindowAdoption::Adopted);
    REQUIRE(waitUntil([&] { return rig.host.opened.size() == 1; }));
    auto const second = rig.host.opened.front()->id;
    REQUIRE(rig.app.manager().windowsBoundTo(link).size() == 2);

    rig.host.opened.front()->controller->closeWindow();
    drainGuiEvents();

    CHECK(rig.host.endings == 0);
    CHECK(std::ranges::to<std::vector>(rig.app.manager().windowsBoundTo(link)) == std::vector { boot.id });
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

// Review (PR 2, item 2): a restarted daemon's reconnect detaches every binding while the panes' ptys live on,
// capturing the link's controller. The lost window's close ends the link, which must then wait for those
// ptys: destroyed at once, the pane's later destruction called into a freed controller.
TEST_CASE("ContourGuiApp: a link ended by a restarted daemon outlives the ptys it detached",
          "[contour][app][attach][link][reconnect]")
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
    auto const panes = watchedSessionsOf(app.manager(), window.id);

    // The daemon comes back as a new run: the window's workspace is gone, and the link closes the window.
    daemon.restartHost();
    REQUIRE(waitUntil([&] { return app.manager().controllerFor(window.id) == nullptr; }, 3000));
    drainGuiEvents();

    // Ended, and kept: the closed window's pane still holds a pty of its controller.
    CHECK(app.app().daemonLinks().size() == 1);
    REQUIRE(std::ranges::all_of(panes, [](auto const& pane) { return !pane.isNull(); }));

    // The pane ends and is deleted later, as a closed session is; only then may the link go.
    endClosedSessions(panes);
    CHECK(waitUntil([&] {
        drainGuiEvents();
        return std::ranges::none_of(panes, [](auto const& pane) { return !pane.isNull(); });
    }));
    CHECK(waitUntil([&] {
        drainGuiEvents();
        return app.app().daemonLinks().empty();
    }));
}

// Review (PR 3, item 9): a label from a program or another daemon was joined into a socket path unchecked.
TEST_CASE("ContourGuiApp: a switch to a label no daemon could have is refused before any path is made",
          "[contour][app][switch]")
{
    auto app = contour::test::TestApp { std::make_unique<contour::test::MockPtySessionFactory>() };
    using vthost::proto::SwitchDaemonKind;
    for (auto const* const label: { "../../etc/x", "a/b", ".hidden", "", "admin-native" })
    {
        auto const config = app.app().linkConfigFor(
            contour::remote::DaemonAddress { .kind = SwitchDaemonKind::Label, .name = label });
        CHECK_FALSE(config.has_value());
        if (!config)
            CHECK(config.error().contains("names no daemon"));
    }
    CHECK(app.app()
              .linkConfigFor(
                  contour::remote::DaemonAddress { .kind = SwitchDaemonKind::Label, .name = "admin" })
              .has_value());
}

// Review (PR 2, item 9): "New Terminal" from the menu bar while the link boots -- its first layout pending,
// no window showing a session -- has the daemon author the workspace, as before per-window links. A local
// window opened instead would be claimed as one more boot window, and stay empty.
TEST_CASE("ContourGuiApp: New Terminal while a link boots is its daemon's to author",
          "[contour][app][attach][link]")
{
    auto daemon = DaemonFixture {};
    std::ignore = seedWorkspaces(daemon, 1);
    auto timeline = TestTimeline {};
    auto app = contour::test::TestApp { std::make_unique<contour::test::MockPtySessionFactory>() };
    auto& link = app.app().startDaemonLink(linkConfig(daemon.endpoint(), timeline, "home"));
    REQUIRE(link.awaitAttached(10s).has_value());
    auto const workspaces = [&daemon] {
        return daemon.onDaemon([&daemon] { return daemon.host->model().workspaceCount(); });
    };
    REQUIRE(workspaces() == 1);

    app.app().runMenuBarAction(contour::actions::NewTerminal { .profileName = std::nullopt },
                               contour::command::ActionScope::Application);

    CHECK(daemon.waitUntilOnDaemon([&daemon] { return daemon.host->model().workspaceCount() == 2; }));
    CHECK(workspaces() == 2);
}

TEST_CASE("Two links in one process: windows on two daemons stay apart", "[attach][link][integration]")
{
    using vthost::client::ConnectionState;
    auto daemonA = DaemonFixture {};
    auto daemonB = DaemonFixture {};
    std::ignore = seedWorkspaces(daemonA, 1);
    std::ignore = seedWorkspaces(daemonB, 1);
    auto timelineA = TestTimeline {};
    auto timelineB = TestTimeline {};
    auto app = contour::test::TestApp { std::make_unique<contour::test::MockPtySessionFactory>() };
    auto& manager = app.manager();
    auto& linkA = app.app().startDaemonLink(linkConfig(daemonA.endpoint(), timelineA, "alpha"));
    auto& linkB = app.app().startDaemonLink(linkConfig(daemonB.endpoint(), timelineB, "beta"));
    auto const windowA = contour::test::ScopedController { manager };
    auto const windowB = contour::test::ScopedController { manager };
    REQUIRE(linkA.awaitAttached(10s).has_value());
    REQUIRE(linkB.awaitAttached(10s).has_value());
    REQUIRE(linkA.bindPendingWindow(*windowA.controller) == contour::remote::WindowAdoption::Adopted);
    REQUIRE(linkB.bindPendingWindow(*windowB.controller) == contour::remote::WindowAdoption::Adopted);
    REQUIRE(waitUntil([&] {
        return sessionsOf(manager, windowA.id).size() == 1 && sessionsOf(manager, windowB.id).size() == 1;
    }));
    CHECK(manager.hostLabelOf(windowA.id) == "alpha");
    CHECK(manager.hostLabelOf(windowB.id) == "beta");
    // Each window's drift badge shows its own daemon's drift; a local window has none.
    CHECK(windowA.controller->daemonDrift() == &linkA.drift());
    CHECK(windowB.controller->daemonDrift() == &linkB.drift());
    auto const local = contour::test::ScopedController { manager };
    CHECK(local.controller->daemonDrift() == nullptr);

    // What is typed into a window's pane reaches its own daemon only.
    auto* const paneA = sessionsOf(manager, windowA.id).front();
    auto* const paneB = sessionsOf(manager, windowB.id).front();
    auto const remoteA = daemonA.onDaemon([&] { return daemonA.host->sessionIds().front(); });
    auto const remoteB = daemonB.onDaemon([&] { return daemonB.host->sessionIds().front(); });
    std::ignore = paneA->terminal().device().write("typed-in-a");
    std::ignore = paneB->terminal().device().write("typed-in-b");
    REQUIRE(waitUntil([&] { return daemonA.ptyInput(remoteA).contains("typed-in-a"); }));
    REQUIRE(waitUntil([&] { return daemonB.ptyInput(remoteB).contains("typed-in-b"); }));
    CHECK_FALSE(daemonA.ptyInput(remoteA).contains("typed-in-b"));
    CHECK_FALSE(daemonB.ptyInput(remoteB).contains("typed-in-a"));

    // A split in window B makes a pane on daemon B, and in window B only.
    manager.splitActivePane(/*vertical*/ false, paneB);
    REQUIRE(daemonB.waitUntilOnDaemon([&] { return daemonB.host->sessionIds().size() == 2; }));
    REQUIRE(waitUntil([&] { return sessionsOf(manager, windowB.id).size() == 2; }));
    CHECK(daemonA.onDaemon([&] { return daemonA.host->sessionIds().size(); }) == 1);
    CHECK(sessionsOf(manager, windowA.id).size() == 1);

    // Daemon A goes away: window A seeks it again, window B stays connected and keeps typing.
    daemonA.refuseConnections();
    REQUIRE(waitUntil([&] { return manager.hostConnection(windowA.id) != ConnectionState::Connected; }));
    CHECK(manager.hostConnection(windowB.id) == ConnectionState::Connected);
    CHECK(linkB.liveController()->connectionState() == ConnectionState::Connected);
    std::ignore = paneB->terminal().device().write("still-b");
    CHECK(waitUntil([&] { return daemonB.ptyInput(remoteB).contains("still-b"); }));
    daemonA.serveAgain();
}

TEST_CASE("the attach boot window adopts the primary daemon workspace", "[contour][app][attach]")
{
    using contour::remote::primaryDaemonWorkspaceToAdopt;

    // The boot window adopts the primary — lowest-id — daemon workspace, so Main.qml adopts it instead of
    // authoring a spurious fresh tab on the daemon. (A later OS window takes the staged pending-workspace
    // path instead, and never asks.)
    CHECK(primaryDaemonWorkspaceToAdopt({ 7, 12, 30 }) == 7);

    // No daemon workspace reported yet: nothing to bind now — the caller still claims the boot window
    // and the link's reconcile binds it once the first layout arrives.
    CHECK_FALSE(primaryDaemonWorkspaceToAdopt({}).has_value());
}

namespace
{

/// @return The session of @p window that shows the daemon's session @p remote; null for none.
[[nodiscard]] contour::session::TerminalSession* showingIn(contour::session::TerminalSessionManager& manager,
                                                           vtworkspace::WorkspaceId window,
                                                           vtworkspace::SessionId remote)
{
    auto const sessions = sessionsOf(manager, window);
    auto const found = std::ranges::find_if(sessions, [&](contour::session::TerminalSession const* session) {
        return manager.hostSessionIdOf(*session) == remote.value;
    });
    return found != sessions.end() ? *found : nullptr;
}

/// @return Whether @p session's screen shows @p needle. Under the terminal lock: the link's controller
///         mirrors into the grid from its reactor thread.
[[nodiscard]] bool screenShows(contour::session::TerminalSession& session, std::string_view needle)
{
    auto const guard = std::lock_guard { session.terminal() };
    return session.terminal().primaryScreen().grid().renderMainPageText().contains(needle);
}

/// One daemon whose two workspaces a launch shows in two windows, each on a link of its own: window A shows
/// a tab split in two, [left | right], and a tab of one pane beside it; window B shows one tab of one pane.
struct OneDaemonTwoWindows
{
    DaemonFixture daemon;
    std::vector<uint64_t> workspaces = seedWorkspaces(daemon, 2);
    vtworkspace::SessionId left = daemon.onDaemon([this] {
        return daemon.host->model()
            .workspace(vtworkspace::WorkspaceId { workspaces[0] })
            ->tabAt(0)
            ->rootPane()
            ->session();
    });
    vtworkspace::SessionId right = daemon.onDaemon([this] {
        auto* tab = daemon.host->model().workspace(vtworkspace::WorkspaceId { workspaces[0] })->tabAt(0);
        daemon.host->splitActivePane(tab->id(), vtworkspace::SplitState::Vertical, 0.5);
        auto const id = tab->activePane()->session();
        daemon.host->terminal(id)->writeToScreen("right-text");
        return id;
    });
    vtworkspace::SessionId single = daemon.onDaemon([this] {
        auto* tab = daemon.host->createTab({}, left);
        return tab->rootPane()->session();
    });
    vtworkspace::SessionId other = daemon.onDaemon([this] {
        auto const id = daemon.host->model()
                            .workspace(vtworkspace::WorkspaceId { workspaces[1] })
                            ->tabAt(0)
                            ->rootPane()
                            ->session();
        daemon.host->terminal(id)->writeToScreen("other-text");
        return id;
    });
    std::size_t daemonSessions = daemon.onDaemon([this] { return daemon.host->sessionIds().size(); });
    LinkRig rig { daemon.endpoint(), "home", {}, launch() };
    contour::test::ScopedController boot { rig.app.manager() };

    OneDaemonTwoWindows() = default;
    OneDaemonTwoWindows(OneDaemonTwoWindows const&) = delete;
    OneDaemonTwoWindows& operator=(OneDaemonTwoWindows const&) = delete;
    OneDaemonTwoWindows(OneDaemonTwoWindows&&) = delete;
    OneDaemonTwoWindows& operator=(OneDaemonTwoWindows&&) = delete;
    ~OneDaemonTwoWindows() = default;

    /// @return The manager both windows live in.
    [[nodiscard]] contour::session::TerminalSessionManager& manager() noexcept { return rig.app.manager(); }
    /// @return Window A, the launch link's.
    [[nodiscard]] vtworkspace::WorkspaceId windowA() const noexcept { return boot.id; }
    /// @return Window B, the hand-over link's.
    [[nodiscard]] vtworkspace::WorkspaceId windowB() const { return rig.host.opened.front()->id; }

    /// Waits until both windows show their workspaces, each on its own link.
    void awaitShown()
    {
        REQUIRE(rig.link->awaitAttached(10s).has_value());
        REQUIRE(waitUntil([&] { return rig.host.outcomes.size() == 1; }));
        REQUIRE(rig.host.outcomes == std::vector { contour::remote::HandOverOutcome::Settled });
        REQUIRE(rig.host.opened.size() == 1);
        REQUIRE(waitUntil([&] {
            return sessionsOf(manager(), windowA()).size() == 3
                   && sessionsOf(manager(), windowB()).size() == 1;
        }));
        REQUIRE(manager().bindingOf(windowA()) == rig.link.get());
        REQUIRE(manager().bindingOf(windowB()) == rig.host.handOverLinks.front().get());
    }

    /// Waits until the move asked last has landed in window B and nothing waits for it any more.
    /// @param moved The daemon sessions it moved.
    void awaitLandedInB(std::span<vtworkspace::SessionId const> moved)
    {
        REQUIRE(waitUntil([&] {
            return manager().livePendingRemoteMove() == nullptr
                   && std::ranges::all_of(moved, [&](vtworkspace::SessionId id) {
                          return showingIn(manager(), windowB(), id) != nullptr
                                 && showingIn(manager(), windowA(), id) == nullptr;
                      });
        }));
        drainGuiEvents();
        // The same shells, still on the daemon: none was spawned or closed for it.
        CHECK(daemon.onDaemon([this] { return daemon.host->sessionIds().size(); }) == daemonSessions);
        for (auto const id: moved)
            CHECK(daemon.onDaemon([&] { return daemon.host->terminal(id) != nullptr; }));
        // Where the user looks now.
        CHECK(manager().focusedWindow() == windowB());
    }
};

} // namespace

TEST_CASE("DaemonLink: a move between two windows of one daemon lands in the other window's link",
          "[attach][link][handover][move]")
{
    using contour::window::MoveRefusal;
    OneDaemonTwoWindows rig;
    rig.awaitShown();
    auto& manager = rig.manager();
    auto* const onto = showingIn(manager, rig.windowB(), rig.other);
    REQUIRE(onto != nullptr);

    SECTION("a pane dropped beside the other window's pane")
    {
        auto* const dragged = showingIn(manager, rig.windowA(), rig.right);
        REQUIRE(dragged != nullptr);
        auto const drag = manager.paneDragOf(*dragged);
        REQUIRE(drag.has_value());
        auto const resolved = manager.resolveNodeDrag(*drag);
        CHECK(resolved.tab.has_value());
        CHECK(manager.moveRefusalIn(resolved, rig.windowB(), contour::window::MovePlace::EdgeOtherWindow)
              == MoveRefusal::None);

        REQUIRE(manager.moveNode(
            *drag,
            vtworkspace::EdgeTarget { .pane = onto->modelSessionId(), .side = vtworkspace::SplitSide::Right },
            nullptr,
            {}));
        rig.awaitLandedInB(std::array { rig.right });
        CHECK(sessionsOf(manager, rig.windowA()).size() == 2);
        CHECK(sessionsOf(manager, rig.windowB()).size() == 2);
        auto* const landed = showingIn(manager, rig.windowB(), rig.right);
        REQUIRE(landed != nullptr);
        CHECK(std::get<1>(manager.model().findSessionLeaf(landed->modelSessionId()))
              == std::get<1>(manager.model().findSessionLeaf(onto->modelSessionId())));
        // The moved shell's screen, mirrored through the window's own link.
        CHECK(waitUntil([&] { return screenShows(*landed, "right-text"); }));
    }

    SECTION("a tab dropped into the other window's tab strip")
    {
        auto const drag = manager.tabDragOf(rig.windowA(), 0);
        REQUIRE(drag.has_value());
        REQUIRE(manager.moveNode(
            *drag, vtworkspace::TabWellTarget { .workspace = rig.windowB(), .index = 1 }, nullptr, {}));
        rig.awaitLandedInB(std::array { rig.left, rig.right });
        CHECK(sessionsOf(manager, rig.windowA()).size() == 1);
        CHECK(showingIn(manager, rig.windowA(), rig.single) != nullptr);
        CHECK(manager.model().workspace(rig.windowB())->tabCount() == 2);
        CHECK(waitUntil([&] {
            auto* const landed = showingIn(manager, rig.windowB(), rig.right);
            return landed != nullptr && screenShows(*landed, "right-text");
        }));
    }

    SECTION("a tab merged into a split of the other window's pane")
    {
        auto const drag = manager.tabDragOf(rig.windowA(), 0);
        REQUIRE(drag.has_value());
        REQUIRE(manager.moveNode(*drag,
                                 vtworkspace::EdgeTarget { .pane = onto->modelSessionId(),
                                                           .side = vtworkspace::SplitSide::Bottom },
                                 nullptr,
                                 {}));
        rig.awaitLandedInB(std::array { rig.left, rig.right });
        CHECK(sessionsOf(manager, rig.windowA()).size() == 1);
        CHECK(manager.model().workspace(rig.windowB())->tabCount() == 1);
        CHECK(manager.model().workspace(rig.windowB())->tabAt(0)->paneCount() == 3);
    }
}

TEST_CASE("DaemonLink: a move between windows of two daemons is refused", "[attach][link][move]")
{
    using contour::window::MoveRefusal;
    // Two runs of a daemon each draw an instance of their own.
    auto daemonA = DaemonFixture { {}, {}, 3 };
    auto daemonB = DaemonFixture { {}, {}, 5 };
    std::ignore = seedWorkspaces(daemonA, 1);
    std::ignore = seedWorkspaces(daemonB, 1);
    auto timelineA = TestTimeline {};
    auto timelineB = TestTimeline {};
    auto app = contour::test::TestApp { std::make_unique<contour::test::MockPtySessionFactory>() };
    auto& manager = app.manager();
    auto& linkA = app.app().startDaemonLink(linkConfig(daemonA.endpoint(), timelineA, "alpha"));
    auto& linkB = app.app().startDaemonLink(linkConfig(daemonB.endpoint(), timelineB, "beta"));
    auto const windowA = contour::test::ScopedController { manager };
    auto const windowB = contour::test::ScopedController { manager };
    REQUIRE(linkA.awaitAttached(10s).has_value());
    REQUIRE(linkB.awaitAttached(10s).has_value());
    REQUIRE(linkA.bindPendingWindow(*windowA.controller) == contour::remote::WindowAdoption::Adopted);
    REQUIRE(linkB.bindPendingWindow(*windowB.controller) == contour::remote::WindowAdoption::Adopted);
    REQUIRE(waitUntil([&] {
        return sessionsOf(manager, windowA.id).size() == 1 && sessionsOf(manager, windowB.id).size() == 1;
    }));
    auto* const paneA = sessionsOf(manager, windowA.id).front();
    auto* const paneB = sessionsOf(manager, windowB.id).front();

    auto const drag = manager.paneDragOf(*paneA);
    REQUIRE(drag.has_value());
    auto const resolved = manager.resolveNodeDrag(*drag);
    CHECK(manager.moveRefusalIn(resolved, windowB.id, contour::window::MovePlace::EdgeOtherWindow)
          == MoveRefusal::OtherHost);
    auto const moved = manager.moveNode(
        *drag,
        vtworkspace::EdgeTarget { .pane = paneB->modelSessionId(), .side = vtworkspace::SplitSide::Right },
        nullptr,
        {});
    REQUIRE_FALSE(moved.has_value());
    CHECK(moved.error() == contour::session::MoveFailure { MoveRefusal::OtherHost });
    CHECK(manager.tabMergeSupport(windowA.id, windowB.id) == contour::session::TabMergeSupport::Unsupported);
    drainGuiEvents();
    CHECK(sessionsOf(manager, windowA.id) == std::vector { paneA });
    CHECK(sessionsOf(manager, windowB.id) == std::vector { paneB });
}

TEST_CASE("DaemonLink: only the daemon this client launched on offers a restart and an upgrade",
          "[attach][link][drift]")
{
    using contour::window::DaemonUpkeep;
    // The launch's local daemon, or a local one a switch reached by its label: both local, one this client's.
    auto const upkeep = GENERATE(DaemonUpkeep::ThisClient, DaemonUpkeep::None);
    auto daemon = DaemonFixture {};
    std::ignore = seedWorkspaces(daemon, 1);
    auto timeline = TestTimeline {};
    // Declared before the app, as LinkRig's: the panes unregister from the link's controller as they go.
    auto link = std::unique_ptr<contour::remote::DaemonLink> {};
    auto handOverLinks = std::vector<std::unique_ptr<contour::remote::DaemonLink>> {};
    auto app = contour::test::TestApp { std::make_unique<contour::test::MockPtySessionFactory>() };
    auto host = FakeLinkHost { app.manager(), handOverLinks };
    auto config = linkConfig(daemon.endpoint(),
                             timeline,
                             {},
                             {},
                             { .kind = vthost::proto::AttachIntentKind::AllWorkspaces, .workspaces = {} },
                             { .kind = vthost::proto::SwitchDaemonKind::Label, .name = "admin" });
    config.locality = contour::window::DaemonLocality::Local;
    config.clientRelease = "0.0.1"; // drifted from any daemon this suite runs
    config.upkeep = upkeep;
    link = std::make_unique<contour::remote::DaemonLink>(
        contour::remote::DaemonLink::Id { 1 }, std::move(config), host, app.manager());
    REQUIRE(link->awaitAttached(10s).has_value());

    link->offerDaemonRestartIfDrifted();
    link->drift().requestRestart();
    drainGuiEvents();

    if (upkeep == DaemonUpkeep::ThisClient)
    {
        CHECK(link->drift().isVisible());
        CHECK(host.restarts == 1);
    }
    else
    {
        // Not this client's: a restart would relaunch it onto the daemon it launched on, not this one.
        CHECK_FALSE(link->drift().isVisible());
        CHECK(host.restarts == 0);
    }
    link->stop();
    drainGuiEvents();
}
