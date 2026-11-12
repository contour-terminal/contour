// SPDX-License-Identifier: Apache-2.0
#include <contour/command/ContextMenu.hpp>
#include <contour/config/Actions.hpp>
#include <contour/config/FinishNotificationConfig.hpp>
#include <contour/platform/Notifier.hpp>
#include <contour/remote/DaemonLink.hpp>
#include <contour/remote/SwitchGrants.hpp>
#include <contour/remote/testing/DaemonFixture.hpp>
#include <contour/remote/testing/NativeControllerTestAccess.hpp>
#include <contour/session/TerminalSessionManager.hpp>
#include <contour/test/AppFixture.hpp>
#include <contour/test/GuiTestFixtures.hpp>
#include <contour/window/PromptHost.hpp>
#include <contour/window/WindowController.hpp>

#include <core/Utils.hpp>
#include <core/platform/FileUri.hpp>
#include <core/platform/PathUtils.hpp>

#include <QtCore/QPointer>
#include <QtGui/QGuiApplication>
#include <QtQuick/QQuickWindow>

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

#include <QtTest/QTest>
#include <vtcontrol/Api.hpp>
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

/// Raises nothing and records every notification: the recorded vector is the assertion.
class RecordingNotifier final: public contour::platform::Notifier
{
  public:
    void notify(vtbackend::DesktopNotification const& notification) override
    {
        raised.push_back(notification);
    }
    void close(std::string const& identifier) override { closed.push_back(identifier); }

    std::vector<vtbackend::DesktopNotification> raised; ///< What was raised, in order.
    std::vector<std::string> closed;                    ///< What was withdrawn, in order.
};

/// An application that records what a link asks of it. A window it opens is a manager-minted controller that
/// asks the attach-window binder at once, as a spawned window's Main.qml does while it loads. A link it is
/// asked to start for a window of its own it owns, as the application does.
class FakeLinkHost final: public contour::remote::IDaemonLinkHost
{
  public:
    /// @param manager The manager the windows live in.
    /// @param windowLinks Where the links it starts are kept: declared before the manager's app, as their
    ///        windows' panes unregister from them as they go.
    FakeLinkHost(contour::session::TerminalSessionManager& manager,
                 std::vector<std::unique_ptr<contour::remote::DaemonLink>>& windowLinks):
        windowLinks(windowLinks), _manager(manager)
    {
    }

    FakeLinkHost(FakeLinkHost const&) = delete;
    FakeLinkHost& operator=(FakeLinkHost const&) = delete;
    FakeLinkHost(FakeLinkHost&&) = delete;
    FakeLinkHost& operator=(FakeLinkHost&&) = delete;

    ~FakeLinkHost() override
    {
        // While the manager the links serve windows of exists.
        for (auto const& link: windowLinks)
            link->stop();
    }

    void startWindowLink(contour::remote::DaemonLinkConfig config) override
    {
        if (adjustWindowLink)
            adjustWindowLink(config);
        windowLinks.push_back(std::make_unique<contour::remote::DaemonLink>(
            contour::remote::DaemonLink::Id { _nextId++ }, std::move(config), *this, _manager));
        contour::remote::passOnLaunchResume(links());
    }

    [[nodiscard]] contour::remote::ElsewhereNotices elsewhereNotices() override
    {
        return { .policy = finishPolicy, .bell = {}, .homeDirectory = "/home/test", .notifier = notifier };
    }

    void openWindowFor(QScreen* /*screen*/) override
    {
        ++windowsAsked;
        if (windowLoad == WindowLoad::Fails)
            return;
        auto& window = *opened.emplace_back(std::make_unique<contour::test::ScopedController>(_manager));
        adoptions.push_back(_manager.consumeAttachWindow(window.controller)
                                ? contour::remote::WindowAdoption::Adopted
                                : contour::remote::WindowAdoption::Declined);
    }
    contour::remote::UpgradeStart upgradeDaemon(contour::remote::DaemonLink& /*link*/) override
    {
        ++upgrades;
        return upgradeAnswer;
    }
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
        for (auto const& link: windowLinks)
            all.push_back(link.get());
        return all;
    }

    contour::remote::SwitchGrants grants; ///< The answers that stand for the switches programs ask for.
    std::vector<std::unique_ptr<contour::test::ScopedController>> opened; ///< Every window opened, in order.
    std::vector<contour::remote::WindowAdoption> adoptions;               ///< What the binder said to each.
    /// The links started for windows of their own, in order.
    std::vector<std::unique_ptr<contour::remote::DaemonLink>>& windowLinks;
    /// The link the test built itself; null for none.
    contour::remote::DaemonLink* launchLink = nullptr;
    /// Changes what a window link is built with, before it is; empty for nothing.
    std::function<void(contour::remote::DaemonLinkConfig&)> adjustWindowLink;
    /// What a finish elsewhere is decided by: every command, however short.
    contour::config::FinishNotificationConfig finishPolicy { .when = contour::config::NotifyWhen::Unfocused,
                                                             .minDuration = std::chrono::seconds { 0 } };
    /// Where the finishes elsewhere are raised.
    RecordingNotifier notifier;
    int upgrades = 0;
    /// What upgradeDaemon() answers.
    contour::remote::UpgradeStart upgradeAnswer = contour::remote::UpgradeStart::Started;
    int restarts = 0;
    int endings = 0; ///< How often linkEnded() was told.
    /// Whether a window openWindowFor() opens loads and asks the binder, or never comes.
    enum class WindowLoad : uint8_t
    {
        Loads = 0,
        Fails,
    };
    WindowLoad windowLoad = WindowLoad::Loads;
    int windowsAsked = 0; ///< How often openWindowFor() was asked.

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
    vthost::proto::AttachIntent intent = { .kind = vthost::proto::AttachIntentKind::Default, .workspace = 0 },
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
                                                           .intent = intent }),
        .hostLabel = std::move(hostLabel),
        .locality = contour::window::DaemonLocality::Remote,
        .clientRelease = "test",
        .opening = std::nullopt,
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
    /// The links the host starts for windows of their own; declared before the app, as @c link is.
    std::vector<std::unique_ptr<contour::remote::DaemonLink>> windowLinks;
    contour::test::TestApp app { std::make_unique<contour::test::MockPtySessionFactory>() };
    FakeLinkHost host { app.manager(), windowLinks };

    /// Builds the link to @p endpoint.
    /// @param endpoint The daemon.
    /// @param hostLabel The host label the link is configured with.
    /// @param sessionSettings Asked for the link's session settings; empty for none.
    /// @param intent What the link's first connection follows.
    /// @param address The daemon as a switch names it.
    explicit LinkRig(vthost::TcpEndpoint endpoint,
                     std::string hostLabel = {},
                     contour::remote::NativeController::SessionSettingsSource sessionSettings = {},
                     vthost::proto::AttachIntent intent = { .kind = vthost::proto::AttachIntentKind::Default,
                                                            .workspace = 0 },
                     contour::remote::DaemonAddress address = {})
    {
        // In the body: the link is declared before the app and host it is built on.
        link = std::make_unique<contour::remote::DaemonLink>(contour::remote::DaemonLink::Id { 1 },
                                                             linkConfig(std::move(endpoint),
                                                                        timeline,
                                                                        std::move(hostLabel),
                                                                        std::move(sessionSettings),
                                                                        intent,
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
        for (auto const& windowLink: host.windowLinks)
            windowLink->stop();
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

/// Lets the GUI thread run what was queued for a while: long enough for what the daemon would send to land.
void settleGui()
{
    for ([[maybe_unused]] auto const turn: std::views::iota(0, 10))
    {
        drainGuiEvents();
        QTest::qWait(10);
    }
}

/// @return The daemon workspace @p window displays through @p link, once the GUI ran what was queued; none
///         when the window is gone or displays none of the link's.
[[nodiscard]] std::optional<uint64_t> displayedBy(contour::session::TerminalSessionManager const& manager,
                                                  contour::remote::DaemonLink const& link,
                                                  contour::window::WindowId window)
{
    drainGuiEvents();
    auto const* const controller = manager.controllerOf(window);
    return controller != nullptr ? link.workspaceOf(controller->displayedWorkspace()) : std::nullopt;
}

/// @return What a plain launch's first connection follows.
[[nodiscard]] vthost::proto::AttachIntent launch()
{
    return vthost::proto::AttachIntent { .kind = vthost::proto::AttachIntentKind::Default, .workspace = 0 };
}

/// @return How many of the daemon's connections show @p workspace. Daemon thread.
[[nodiscard]] std::size_t showing(DaemonFixture& daemon, uint64_t workspace)
{
    return daemon.host->clientsShowing(workspace).size();
}

/// @return Whether @p workspace is shown, left or hidden on the daemon.
[[nodiscard]] vthost::WorkspaceVisibility visibilityOn(DaemonFixture& daemon, uint64_t workspace)
{
    return daemon.onDaemon([&daemon, workspace] { return daemon.host->visibility(workspace); });
}

/// What a link that cannot reach its daemon is built with: nothing listens on port 1.
void unreachable(contour::remote::DaemonLinkConfig& config)
{
    config.controller.transport = contour::remote::testing::makeTestTlsTransport(
        vthost::TcpEndpoint { .host = "127.0.0.1", .port = 1, .token = {}, .caPem = {} });
}

/// A daemon that offers in-place upgrades.
[[nodiscard]] vthost::NativeSessionServices withLiveUpgrade()
{
    auto services = vthost::NativeSessionServices {};
    services.handshake.capabilities.idents.push_back(
        std::to_underlying(vthost::proto::Capability::LiveUpgrade));
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

TEST_CASE("DaemonLink: a launch opens one window selecting the most recent workspace", "[attach][link]")
{
    using vthost::WorkspaceVisibility;
    auto daemon = DaemonFixture {};
    auto const ids = seedWorkspaces(daemon, 3);
    // Another connection selected the second last: that is the daemon's most recent selection.
    {
        auto otherTimeline = TestTimeline {};
        auto const other = makeNativeController(
            daemon.endpoint(),
            otherTimeline,
            ControllerOptions {
                .intent = { .kind = vthost::proto::AttachIntentKind::Select, .workspace = ids[1] } });
        REQUIRE(other->connectAndWait(10s).has_value());
        other->stop();
    }
    REQUIRE(daemon.waitUntilOnDaemon([&daemon] { return daemon.live.empty(); }));

    auto rig = LinkRig { daemon.endpoint(), "home", {}, launch() };
    auto& link = *rig.link;
    auto& manager = rig.app.manager();
    auto const boot = contour::test::ScopedController { manager };
    REQUIRE(pollUntilAttached(link, 10s).has_value());

    // The boot window displays it; no other window, and no other connection, comes.
    REQUIRE(waitUntil([&] { return link.workspaceOf(boot.id) == ids[1]; }));
    REQUIRE(waitUntil([&] { return !sessionsOf(manager, boot.id).empty(); }));
    drainGuiEvents();
    CHECK(rig.host.opened.empty());
    CHECK(rig.host.windowLinks.empty());
    CHECK(std::ranges::to<std::vector>(manager.windowsBoundTo(link)) == std::vector { boot.id });
    CHECK(daemon.onDaemon([&daemon] { return daemon.live.size(); }) == 1);
    CHECK(visibilityOn(daemon, ids[1]) == WorkspaceVisibility::Shown);
    CHECK(visibilityOn(daemon, ids[0]) == WorkspaceVisibility::Hidden);
    CHECK(visibilityOn(daemon, ids[2]) == WorkspaceVisibility::Hidden);
    // The others are entries of the window's directory, every one of them.
    auto const directory = link.liveController()->directory();
    CHECK(directory.workspaces.size() == 3);
    CHECK(directory.selected == ids[1]);
}

// Review (PR 3, item 7): a window closed while it switched to a new workspace failed the switch, but the
// workspace the daemon made for it arrived later all the same and got a window of its own.
TEST_CASE("DaemonLink: a window closed while it switches to a new workspace leaves that workspace hidden",
          "[attach][link][switch]")
{
    using vthost::WorkspaceVisibility;
    auto daemon = DaemonFixture {};
    std::ignore = seedWorkspaces(daemon, 1);
    auto rig = LinkRig { daemon.endpoint(), {}, {}, launch() };
    auto& link = *rig.link;
    auto& manager = rig.app.manager();
    // A local window stays open: the link's last window is not the process's.
    auto const local = contour::test::ScopedController { manager };
    auto const boot = contour::test::ScopedController { manager };
    REQUIRE(pollUntilAttached(link, 10s).has_value());
    REQUIRE(link.bindPendingWindow(*boot.controller) == contour::remote::WindowAdoption::Adopted);
    REQUIRE(waitUntil([&] { return !sessionsOf(manager, boot.id).empty(); }));

    // The next layout -- the new workspace's -- is held on its way.
    daemon.onDaemon([&daemon] {
        for (auto* const socket: daemon.gated)
            socket->holdAtNextLayout();
        return 0;
    });
    auto told = std::optional<std::expected<uint64_t, std::string>> {};
    REQUIRE(link.selectWorkspace(0, [&](auto landed) { told = std::move(landed); }).has_value());
    REQUIRE(daemon.waitUntilOnDaemon([&daemon] { return daemon.host->model().workspaceCount() == 2; }));
    auto const made = daemon.onDaemon([&daemon] { return daemon.host->model().workspaceAt(1)->id().value; });

    auto const panes = watchedSessionsOf(manager, boot.id);
    boot.controller->closeWindow();
    endClosedSessions(panes);
    REQUIRE(told.has_value());
    CHECK_FALSE(told->has_value());

    // The workspace arrives after the window went: no connection selects it, and no window shows it.
    daemon.releaseHeld();
    CHECK(waitUntil([&] { return daemon.onDaemon([&daemon] { return daemon.live.empty(); }); }, 2000));
    CHECK(visibilityOn(daemon, made) == WorkspaceVisibility::Hidden);
    drainGuiEvents();
    CHECK(rig.host.opened.empty());
    CHECK_FALSE(link.windowShowing(made).has_value());
}

// What NewWorkspace sends: the pane the window has active, so the daemon starts the new workspace's shell in
// that pane's folder and names the workspace after it. A link that sent no session would leave both empty.
TEST_CASE("DaemonLink: a new workspace starts in the folder of the window's active pane",
          "[attach][link][switch]")
{
    auto const root = std::filesystem::temp_directory_path() / "contour-link-new-workspace";
    std::filesystem::remove_all(root);
    std::filesystem::create_directories(root / "repo" / ".git");
    std::filesystem::create_directories(root / "repo" / "src");
    std::ofstream { root / "repo" / ".git" / "HEAD" } << "ref: refs/heads/main\n";
    auto daemon = DaemonFixture {};
    auto const ids = seedWorkspaces(daemon, 1);
    auto rig = LinkRig { daemon.endpoint(), {}, {}, launch() };
    auto& link = *rig.link;
    auto& manager = rig.app.manager();
    auto const local = contour::test::ScopedController { manager };
    auto const boot = contour::test::ScopedController { manager };
    REQUIRE(pollUntilAttached(link, 10s).has_value());
    REQUIRE(link.bindPendingWindow(*boot.controller) == contour::remote::WindowAdoption::Adopted);
    REQUIRE(waitUntil([&] { return !sessionsOf(manager, boot.id).empty(); }));
    // The shell of the pane the window shows is in the repository's src.
    daemon.onDaemon([&] {
        auto const pane =
            daemon.host->model().workspace(vtworkspace::WorkspaceId { ids[0] })->tabAt(0)->rootPane();
        daemon.host->terminal(pane->session())
            ->writeToScreen(std::format(
                "\033]7;{}\033\\",
                core::platform::fileUri(core::platform::normalizePath(root / "repo" / "src"), "localhost")));
        return 0;
    });

    REQUIRE(link.selectWorkspace(0).has_value());
    REQUIRE(daemon.waitUntilOnDaemon([&daemon] { return daemon.host->model().workspaceCount() == 2; }));

    auto const label = daemon.onDaemon([&daemon] {
        auto const made = daemon.host->model().workspaceAt(1)->id();
        auto const uuid = daemon.host->objects().uuidOf(made);
        return std::string { daemon.host->objects().labelOf(
            vtcontrol::ObjectRef { vtcontrol::ObjectKind::Workspace, uuid.value() }) };
    });
    CHECK(label == "repo");
    std::filesystem::remove_all(root);
}

TEST_CASE("DaemonLink: a closed window's workspace stays on the daemon, selected by no connection",
          "[attach][link]")
{
    using vthost::WorkspaceVisibility;
    auto daemon = DaemonFixture {};
    auto const ids = seedWorkspaces(daemon, 1);
    auto rig = LinkRig { daemon.endpoint(), {}, {}, launch() };
    auto& link = *rig.link;
    // A local window stays open: the link's last window is not the process's.
    auto const local = contour::test::ScopedController { rig.app.manager() };
    auto const boot = contour::test::ScopedController { rig.app.manager() };
    REQUIRE(pollUntilAttached(link, 10s).has_value());
    REQUIRE(link.bindPendingWindow(*boot.controller) == contour::remote::WindowAdoption::Adopted);
    REQUIRE(waitUntil([&] { return !sessionsOf(rig.app.manager(), boot.id).empty(); }));
    auto const panes = watchedSessionsOf(rig.app.manager(), boot.id);

    boot.controller->closeWindow();

    drainGuiEvents();
    CHECK(daemon.waitUntilOnDaemon([&] { return daemon.live.empty(); }));
    CHECK(visibilityOn(daemon, ids[0]) == WorkspaceVisibility::Hidden);
    endClosedSessions(panes);
    drainGuiEvents();
}

// Review (PR 3, item 4): a daemon restarting with a restore numbers its workspaces anew, so the id a window
// selected names another workspace of the new run, or none: the window must display what the new run selects
// for it, and the application keep its window.
TEST_CASE(
    "DaemonLink: a daemon restarted with a restore keeps the window, showing a workspace it brought back",
    "[attach][link][reconnect]")
{
    auto daemon = DaemonFixture {};
    auto const ids = seedWorkspaces(daemon, 3);
    auto rig = LinkRig {
        daemon.endpoint(), {}, {}, { .kind = vthost::proto::AttachIntentKind::Select, .workspace = ids[2] }
    };
    auto& link = *rig.link;
    auto& manager = rig.app.manager();
    auto const boot = contour::test::ScopedController { manager };
    REQUIRE(pollUntilAttached(link, 10s).has_value());
    REQUIRE(waitUntil([&] { return link.workspaceOf(boot.id) == ids[2]; }));
    auto const window = boot.windowId;

    // The daemon restarts and restores the first two workspaces only, numbered anew.
    auto snapshot = daemon.onDaemon([&daemon] {
        return vthost::captureWorkspace(
            *daemon.host, { .scrollback = vthost::ScrollbackPersistence::Off }, std::chrono::sys_seconds {});
    });
    REQUIRE(snapshot.workspaces.size() == 3);
    snapshot.workspaces.pop_back();
    daemon.restore = vthost::WorkspaceRestore { .snapshot = std::move(snapshot) };
    daemon.restartHost();

    // The window stays, displaying a workspace of the new run, and the daemon gained none for it.
    auto const restored = daemon.onDaemon([&daemon] {
        auto workspaces = std::vector<uint64_t> {};
        for (auto const index: std::views::iota(0, daemon.host->model().workspaceCount()))
            workspaces.push_back(daemon.host->model().workspaceAt(index)->id().value);
        return workspaces;
    });
    REQUIRE(restored.size() == 2);
    auto const displayed = [&] {
        return displayedBy(manager, link, window);
    };
    CHECK(waitUntil([&] { return displayed() && std::ranges::contains(restored, *displayed()); }, 3000));
    CHECK(manager.controllerOf(window) != nullptr);
    CHECK(daemon.onDaemon([&daemon] { return daemon.host->model().workspaceCount(); }) == 2);
}

// Review (PR 3 re-review, N1): the links of one `--connect-tcp` daemon reconnect one after another after a
// restart, so for a moment one of them has met the new run while the rest still hold the old one. They are
// still one daemon: only one of them may take the launch role.
TEST_CASE("DaemonLink: links of one daemon keep one launch role while they reconnect to a restarted one",
          "[attach][link][reconnect]")
{
    using Fallback = contour::remote::NativeController::ResumeFallback;
    auto daemon = DaemonFixture {};
    std::ignore = seedWorkspaces(daemon, 3);
    auto rig = LinkRig { daemon.endpoint(), {}, {}, launch() };
    // The daemon lets one connection in after its restart, and the one to compare is the launch's holder.
    // Every link loses its connection at the same moment and retries on the same schedule, so which of them
    // reaches the new run first is decided by which heard of its loss first -- most often not the holder. So
    // the window links wait out a reconnect far longer than the test runs, and the holder's is let in.
    rig.host.adjustWindowLink = [](contour::remote::DaemonLinkConfig& config) {
        config.controller.backoff = { .first = 60s, .ceiling = 60s };
    };
    auto const boot = contour::test::ScopedController { rig.app.manager() };
    REQUIRE(pollUntilAttached(*rig.link, 10s).has_value());
    REQUIRE(waitUntil([&] { return rig.link->workspaceOf(boot.id).has_value(); }));
    // Two New Windows, each a link of its own to the daemon.
    REQUIRE(rig.link->openWindowLike(boot.id, std::nullopt, nullptr).has_value());
    REQUIRE(rig.link->openWindowLike(boot.id, std::nullopt, nullptr).has_value());
    REQUIRE(daemon.waitUntilOnDaemon([&] { return daemon.live.size() == 3; }));
    auto const holders = [&] {
        return std::ranges::count_if(rig.host.links(),
                                     [](auto* link) { return link->resumeFallback() == Fallback::Launch; });
    };
    // The launch's own link: the only one not slowed above.
    REQUIRE(rig.link->resumeFallback() == Fallback::Launch);
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
    REQUIRE(waitUntil([&] { return newRun(rig.link.get()); }));
    // The rest still hold the old run.
    REQUIRE(std::ranges::count_if(rig.host.links(), newRun) == 1);

    // The application judges the roles as a link starts or ends.
    contour::remote::passOnLaunchResume(rig.host.links());
    CHECK(holders() == 1);
}

TEST_CASE("DaemonLink: --workspace shows that workspace only", "[attach][link]")
{
    using vthost::WorkspaceVisibility;
    auto daemon = DaemonFixture {};
    auto const ids = seedWorkspaces(daemon, 3);
    auto rig = LinkRig { daemon.endpoint(),
                         "home",
                         {},
                         { .kind = vthost::proto::AttachIntentKind::Select, .workspace = ids[1] } };
    auto& link = *rig.link;
    auto const boot = contour::test::ScopedController { rig.app.manager() };
    REQUIRE(pollUntilAttached(link, 10s).has_value());
    REQUIRE(waitUntil([&] { return link.workspaceOf(boot.id) == ids[1]; }));
    drainGuiEvents();
    CHECK(rig.host.windowLinks.empty());
    CHECK(rig.host.opened.empty());
    CHECK(visibilityOn(daemon, ids[0]) == WorkspaceVisibility::Hidden);
    CHECK(visibilityOn(daemon, ids[2]) == WorkspaceVisibility::Hidden);
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
        REQUIRE(pollUntilAttached(*rig.link, 10s).has_value());
        CHECK(rig.link->chromeLabel() == "admin");
        CHECK(rig.link->hostLabel().empty()); // still not a remote host: sessions keep running locally
        CHECK(rig.link->daemonTarget()
              == SwitchTarget { .kind = SwitchDaemonKind::Label, .daemon = "admin", .workspace = {} });
    }

    SECTION("the default local label")
    {
        auto rig =
            LinkRig { daemon.endpoint(), {}, {}, {}, { .kind = SwitchDaemonKind::Label, .name = "default" } };
        REQUIRE(pollUntilAttached(*rig.link, 10s).has_value());
        CHECK(rig.link->chromeLabel().empty());
        CHECK(rig.link->daemonTarget()->daemon == "default");
    }

    SECTION("a saved host")
    {
        auto rig =
            LinkRig { daemon.endpoint(), "home", {}, {}, { .kind = SwitchDaemonKind::Host, .name = "home" } };
        REQUIRE(pollUntilAttached(*rig.link, 10s).has_value());
        CHECK(rig.link->chromeLabel() == "home");
        CHECK(rig.link->daemonTarget()->kind == SwitchDaemonKind::Host);
    }
}

TEST_CASE("DaemonLink: a link connects as it is built, and serves the one window it adopted",
          "[attach][link]")
{
    auto daemon = DaemonFixture {};
    std::ignore = seedWorkspaces(daemon, 3);
    auto rig = LinkRig { daemon.endpoint(), "home" };
    auto& link = *rig.link;
    auto const boot = contour::test::ScopedController { rig.app.manager() };

    // The label is the attach's only once it settled.
    CHECK(link.hostLabel().empty());
    REQUIRE(pollUntilAttached(link, 10s).has_value());
    CHECK(link.isAttached());
    CHECK(link.hostLabel() == "home");

    // The layouts arrive: the boot window displays the selected workspace, and no other window opens.
    REQUIRE(waitUntil([&] { return link.workspaceOf(boot.id).has_value(); }));
    link.reconcile(); // idempotent: nothing more is opened
    CHECK(rig.host.opened.empty());
    auto const windows = std::ranges::to<std::vector>(rig.app.manager().windowsBoundTo(link));
    CHECK(windows == std::vector { boot.id });
    // The window it serves is bound to it, until it stops.
    CHECK(rig.app.manager().bindingOf(boot.id) == &link);
    link.stop();
    CHECK(rig.app.manager().bindingOf(boot.id) == nullptr);
}

TEST_CASE("DaemonLink: a connection whose selected workspace ends selects another",
          "[attach][link][reconnect]")
{
    auto daemon = DaemonFixture {};
    auto const workspaces = seedWorkspaces(daemon, 2);
    auto rig = LinkRig { daemon.endpoint(),
                         {},
                         {},
                         { .kind = vthost::proto::AttachIntentKind::Select, .workspace = workspaces[1] } };
    auto& link = *rig.link;
    auto& manager = rig.app.manager();
    auto const boot = contour::test::ScopedController { manager };
    REQUIRE(pollUntilAttached(link, 10s).has_value());
    REQUIRE(waitUntil([&] { return link.workspaceOf(boot.id) == workspaces[1]; }));
    REQUIRE(waitUntil([&] { return !sessionsOf(manager, boot.id).empty(); }));
    auto const window = boot.windowId;

    // Its last tab closes elsewhere: the daemon selects the other workspace for this connection.
    daemon.onDaemon([&] {
        auto* workspace = daemon.host->model().workspace(vtworkspace::WorkspaceId { workspaces[1] });
        daemon.host->handleSessionExit(workspace->tabAt(0)->rootPane()->session());
        return 0;
    });

    // The window stays, displaying the other one in place of the one that ended.
    auto const displayed = [&] {
        return displayedBy(manager, link, window);
    };
    REQUIRE(waitUntil([&] { return displayed() == workspaces[0]; }, 3000));
    auto const* const controller = manager.controllerOf(window);
    REQUIRE(controller != nullptr);
    CHECK(sessionsOf(manager, controller->displayedWorkspace()).size() == 1);
    // The workspace it displayed before left the model, and with it its binding.
    CHECK(manager.model().workspace(boot.id) == nullptr);
    CHECK(manager.bindingOf(boot.id) == nullptr);
    CHECK(controller->ownedWorkspaces().size() == 1);
    CHECK(std::ranges::to<std::vector>(manager.windowsBoundTo(link))
          == std::vector { controller->displayedWorkspace() });
}

TEST_CASE("DaemonLink: stop is idempotent and stops the controller", "[attach][link]")
{
    auto daemon = DaemonFixture {};
    std::ignore = seedWorkspaces(daemon, 1);
    auto rig = LinkRig { daemon.endpoint() };
    auto& link = *rig.link;
    REQUIRE(pollUntilAttached(link, 10s).has_value());
    REQUIRE(daemon.waitUntilOnDaemon([&] { return daemon.live.size() == 1; }));

    link.stop();
    link.stop();
    // The daemon saw this client go.
    CHECK(daemon.waitUntilOnDaemon([&] { return daemon.live.empty(); }));
}

TEST_CASE("DaemonLink: a further link to the daemon states no working directory", "[attach][link][cwd]")
{
    // The directory a launch states opens a tab in the launch's own attach; a window a switch or New Window
    // opens later on another link of that daemon has nothing to open there.
    auto daemon = DaemonFixture {};
    auto timeline = TestTimeline {};
    auto config = linkConfig(daemon.endpoint(), timeline);
    config.controller.workingDirectory = std::filesystem::temp_directory_path();
    auto app = contour::test::TestApp { std::make_unique<contour::test::MockPtySessionFactory>() };
    auto windowLinks = std::vector<std::unique_ptr<contour::remote::DaemonLink>> {};
    auto host = FakeLinkHost { app.manager(), windowLinks };
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
    auto windowLinks = std::vector<std::unique_ptr<contour::remote::DaemonLink>> {};
    auto host = FakeLinkHost { app.manager(), windowLinks };
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
    REQUIRE(pollUntilAttached(link, 10s).has_value());
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
    CHECK_FALSE(pollUntilAttached(link, 100ms).has_value());
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
    REQUIRE(pollUntilAttached(link, 10s).has_value());
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
    REQUIRE(pollUntilAttached(link, 10s).has_value());
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

// What the application's teardown relies on: a link whose last window just went has only begun its
// controller's stop, on a thread of its own, and the teardown stops every link before that stop may end.
// Whether the background stop has ended by then is a race, which is why only the outcome is asserted.
TEST_CASE("DaemonLink: stop() stops the controller of a link that just ended", "[attach][link]")
{
    auto daemon = DaemonFixture {};
    std::ignore = seedWorkspaces(daemon, 1);
    auto rig = LinkRig { daemon.endpoint(), "home" };
    auto& link = *rig.link;
    auto const local = contour::test::ScopedController { rig.app.manager() };
    auto const boot = contour::test::ScopedController { rig.app.manager() };
    REQUIRE(pollUntilAttached(link, 10s).has_value());
    REQUIRE(link.bindPendingWindow(*boot.controller) == contour::remote::WindowAdoption::Adopted);
    auto const* const controller = link.liveController();
    REQUIRE(controller != nullptr);
    boot.controller->closeWindow();

    link.stop();

    CHECK_FALSE(controller->isWindingDown());
    CHECK(controller->connectionState() == vthost::client::ConnectionState::Stopped);
    CHECK(daemon.waitUntilOnDaemon([&] { return daemon.live.empty(); }));
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
    REQUIRE(pollUntilAttached(link, 10s).has_value());
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
    REQUIRE(pollUntilAttached(link, 10s).has_value());

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
    REQUIRE(pollUntilAttached(linkA, 10s).has_value());
    REQUIRE(pollUntilAttached(linkB, 10s).has_value());

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
    REQUIRE(pollUntilAttached(link, 10s).has_value());
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
// capturing the link's controller. The window's close ends the link, which must then wait for those ptys:
// destroyed at once, the pane's later destruction called into a freed controller.
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
    REQUIRE(pollUntilAttached(link, 10s).has_value());
    REQUIRE(link.bindPendingWindow(*window.controller) == contour::remote::WindowAdoption::Adopted);
    REQUIRE(waitUntil([&] { return !sessionsOf(app.manager(), window.id).empty(); }));
    auto const panes = watchedSessionsOf(app.manager(), window.id);

    // The daemon comes back as a new run: the window's workspace is gone, and the window displays the one the
    // new run selects for it, its old panes detached.
    daemon.restartHost();
    auto const displayed = [&] {
        return window.controller->displayedWorkspace();
    };
    REQUIRE(waitUntil(
        [&] { return displayed() != window.id && !sessionsOf(app.manager(), displayed()).empty(); }, 3000));
    drainGuiEvents();
    auto const current = watchedSessionsOf(app.manager(), displayed());

    // The window closes: the link ends, and is kept while a pane still holds a pty of its controller -- the
    // detached ones among them.
    window.controller->closeWindow();
    drainGuiEvents();
    CHECK(app.app().daemonLinks().size() == 1);
    REQUIRE(std::ranges::any_of(panes, [](auto const& pane) { return !pane.isNull(); }));

    // The panes end and are deleted later, as a closed session is; only then may the link go.
    endClosedSessions(panes);
    endClosedSessions(current);
    CHECK(waitUntil([&] {
        drainGuiEvents();
        return std::ranges::none_of(panes, [](auto const& pane) { return !pane.isNull(); });
    }));
    CHECK(waitUntil([&] {
        drainGuiEvents();
        return app.app().daemonLinks().empty();
    }));
}

// A prompt waits for the window that would show it: per window, not for any.
TEST_CASE("ContourGuiApp: an ssh prompt is held for its own window until that window is on screen",
          "[contour][app][askpass]")
{
    auto app = contour::test::TestApp { std::make_unique<contour::test::MockPtySessionFactory>() };
    auto& manager = app.app().sessionsManager();
    auto const& router = app.app().askpassRouter();

    auto* const first = manager.createWindowController();
    auto* const second = manager.createWindowController();
    REQUIRE(first != nullptr);
    REQUIRE(second != nullptr);
    auto const firstRoute = first->windowId().value;
    auto const secondRoute = second->windowId().value;
    auto const pendingOf = [](QObject* host) {
        return host->property("pendingPrompt").toMap();
    };
    auto replies = std::vector<contour::askpass::AskpassReply> {};
    auto const ask = [&](contour::askpass::AskpassRouteId route, contour::askpass::AskpassPromptId id) {
        router->ask(
            route,
            id,
            contour::askpass::AskpassPromptKind::Secret,
            QStringLiteral("ssh://me@host"),
            QStringLiteral("Password: "),
            [&replies](contour::askpass::AskpassReply reply) { replies.push_back(std::move(reply)); });
    };

    // Neither window is on screen: a prompt for either is held, and nothing is shown.
    ask(firstRoute, 1);
    ask(secondRoute, 2);
    CHECK(router->isHolding(firstRoute));
    CHECK(router->isHolding(secondRoute));
    CHECK(pendingOf(first->promptHost()).isEmpty());
    CHECK(pendingOf(second->promptHost()).isEmpty());

    // A window routes by its own id, which it keeps as it shows one workspace after another: the second
    // window now shows a workspace made after its prompt was asked for.
    std::ignore = manager.createWorkspaceIn(second->windowId());
    manager.selectWorkspace(second->windowId(), 1);

    // Showing the second window shows its prompt, and the first window's goes on waiting.
    auto osWindow = QQuickWindow {};
    second->bindWindow(&osWindow);
    osWindow.show();
    REQUIRE(QTest::qWaitForWindowExposed(&osWindow));
    app.app().refreshWindowAttention();
    CHECK_FALSE(router->isHolding(secondRoute));
    CHECK(router->isHolding(firstRoute));
    CHECK(pendingOf(second->promptHost()).value("promptId").toInt() == 2);
    CHECK(pendingOf(first->promptHost()).isEmpty());

    // A prompt of the window that is on screen is shown at once; one that has no window of its own goes to
    // the first window, which is not.
    ask(secondRoute, 3);
    CHECK_FALSE(router->isHolding(secondRoute));
    ask(contour::askpass::AnyWindow, 4);
    CHECK(router->isHolding(contour::askpass::AnyWindow));

    // Minimised is not on screen either, where the platform has the state: what comes next is held again.
    osWindow.showMinimized();
    QTest::qWait(50);
    if (osWindow.windowStates().testFlag(Qt::WindowMinimized))
    {
        app.app().refreshWindowAttention();
        ask(secondRoute, 5);
        CHECK(router->isHolding(secondRoute));
        osWindow.showNormal();
        QTest::qWait(50);
        app.app().refreshWindowAttention();
        CHECK_FALSE(router->isHolding(secondRoute));
    }

    // Closing the window cancels what it still holds, rather than showing it elsewhere.
    osWindow.hide();
    app.app().refreshWindowAttention();
    ask(secondRoute, 6);
    CHECK(router->isHolding(secondRoute));
    router->shutdown();
    CHECK_FALSE(router->isHolding(secondRoute));
    CHECK_FALSE(replies.empty());
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
            contour::remote::DaemonAddress { .kind = SwitchDaemonKind::Label, .name = label },
            vtworkspace::WorkspaceId { 1 });
        CHECK_FALSE(config.has_value());
        if (!config)
            CHECK(config.error().contains("names no daemon"));
    }
    CHECK(app.app()
              .linkConfigFor(
                  contour::remote::DaemonAddress { .kind = SwitchDaemonKind::Label, .name = "admin" },
                  vtworkspace::WorkspaceId { 1 })
              .has_value());
}

// Review (PR 2, item 9): "New Terminal" from the menu bar while the link boots -- its first layout pending,
// no window showing a session -- opens the window the link adopts as its boot window, and authors nothing on
// the daemon: a workspace made for it would show in no window.
TEST_CASE("ContourGuiApp: New Terminal while a link boots authors no workspace on its daemon",
          "[contour][app][attach][link]")
{
    auto daemon = DaemonFixture {};
    std::ignore = seedWorkspaces(daemon, 1);
    auto timeline = TestTimeline {};
    auto app = contour::test::TestApp { std::make_unique<contour::test::MockPtySessionFactory>() };
    auto& link = app.app().startDaemonLink(linkConfig(daemon.endpoint(), timeline, "home"));
    REQUIRE(pollUntilAttached(link, 10s).has_value());
    auto const workspaces = [&daemon] {
        return daemon.onDaemon([&daemon] { return daemon.host->model().workspaceCount(); });
    };
    REQUIRE(workspaces() == 1);

    app.app().runMenuBarAction(contour::actions::NewTerminal { .profileName = std::nullopt },
                               contour::command::ActionScope::Application);

    drainGuiEvents();
    CHECK(workspaces() == 1);
    CHECK(app.app().daemonLinks().size() == 1);
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
    REQUIRE(pollUntilAttached(linkA, 10s).has_value());
    REQUIRE(pollUntilAttached(linkB, 10s).has_value());
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

namespace
{

/// One daemon of @p count workspaces, a launch's window on its first, and a New Window of it: a second window
/// on a link of its own, selecting the same workspace.
struct TwoWindowsOneWorkspace
{
    DaemonFixture daemon;
    std::vector<uint64_t> workspaces;
    LinkRig rig;
    contour::test::ScopedController boot { rig.app.manager() };

    explicit TwoWindowsOneWorkspace(int count = 1):
        workspaces(seedWorkspaces(daemon, count)), rig(daemon.endpoint(), "home", {}, launch())
    {
        REQUIRE(pollUntilAttached(*rig.link, 10s).has_value());
        REQUIRE(waitUntil([&] { return rig.link->workspaceOf(boot.id).has_value(); }));
        REQUIRE(waitUntil([&] { return !sessionsOf(manager(), boot.id).empty(); }));
        REQUIRE(rig.link->openWindowLike(boot.id, std::nullopt, nullptr).has_value());
        REQUIRE(waitUntil(
            [&] { return rig.host.opened.size() == 1 && !sessionsOf(manager(), secondWindow()).empty(); }));
        REQUIRE(rig.host.windowLinks.size() == 1);
    }

    TwoWindowsOneWorkspace(TwoWindowsOneWorkspace const&) = delete;
    TwoWindowsOneWorkspace& operator=(TwoWindowsOneWorkspace const&) = delete;
    TwoWindowsOneWorkspace(TwoWindowsOneWorkspace&&) = delete;
    TwoWindowsOneWorkspace& operator=(TwoWindowsOneWorkspace&&) = delete;
    ~TwoWindowsOneWorkspace() = default;

    [[nodiscard]] contour::session::TerminalSessionManager& manager() noexcept { return rig.app.manager(); }
    /// @return The New Window's link.
    [[nodiscard]] contour::remote::DaemonLink& secondLink() const { return *rig.host.windowLinks.front(); }
    /// @return The local workspace the New Window displays.
    [[nodiscard]] vtworkspace::WorkspaceId secondWindow() const
    {
        return rig.host.opened.front()->controller->displayedWorkspace();
    }
    /// @return The daemon's session of its first workspace's first tab.
    [[nodiscard]] uint64_t firstPane()
    {
        return daemon.onDaemon([this] {
            return daemon.host->model()
                .workspace(vtworkspace::WorkspaceId { workspaces[0] })
                ->tabAt(0)
                ->rootPane()
                ->session()
                .value;
        });
    }
};

/// @return The session of @p window that shows the daemon's session @p remote; null for none.
[[nodiscard]] contour::session::TerminalSession* showing(contour::session::TerminalSessionManager& manager,
                                                         vtworkspace::WorkspaceId window,
                                                         uint64_t remote)
{
    auto const sessions = sessionsOf(manager, window);
    auto const found = std::ranges::find_if(sessions, [&](contour::session::TerminalSession const* session) {
        return manager.hostSessionIdOf(*session) == remote;
    });
    return found != sessions.end() ? *found : nullptr;
}

} // namespace

TEST_CASE("DaemonLink: two windows selecting one workspace", "[attach][link]")
{
    using vthost::proto::RaiseRole;
    auto rig = TwoWindowsOneWorkspace {};
    auto& manager = rig.manager();
    auto const pane = rig.firstPane();

    // Each window has local sessions of its own for the one pane.
    auto* const first = showing(manager, rig.boot.id, pane);
    auto* const second = showing(manager, rig.secondWindow(), pane);
    REQUIRE(first != nullptr);
    REQUIRE(second != nullptr);
    CHECK(first != second);
    CHECK(rig.daemon.onDaemon([&] { return showing(rig.daemon, rig.workspaces[0]); }) == 2);

    // What is typed into one reaches the daemon's one pane, whose output both show.
    std::ignore = first->terminal().device().write("typed-in-first");
    REQUIRE(waitUntil(
        [&] { return rig.daemon.ptyInput(vtworkspace::SessionId { pane }).contains("typed-in-first"); }));
    rig.daemon.onDaemon([&] {
        rig.daemon.host->terminal(vtworkspace::SessionId { pane })->writeToScreen("both-see-this");
        return 0;
    });
    auto const shows = [](contour::session::TerminalSession& session, std::string_view text) {
        auto const guard = std::lock_guard { session.terminal() };
        return session.terminal().primaryScreen().grid().renderMainPageText().contains(text);
    };
    CHECK(waitUntil([&] { return shows(*first, "both-see-this") && shows(*second, "both-see-this"); }));

    // One raises: the window focused last.
    manager.setFocusedWindow(rig.secondWindow());
    CHECK(waitUntil([&] {
        return rig.secondLink().raiseRole() == RaiseRole::Raise
               && rig.rig.link->raiseRole() == RaiseRole::Quiet;
    }));
    // Reported again on every poll: a focus report of the second window still in flight may land after the
    // first's, and the latest report decides.
    CHECK(waitUntil([&] {
        manager.setFocusedWindow(rig.boot.id);
        return rig.rig.link->raiseRole() == RaiseRole::Raise
               && rig.secondLink().raiseRole() == RaiseRole::Quiet;
    }));

    // Closing one leaves the other, and its link, as they were.
    auto const panes = watchedSessionsOf(manager, rig.secondWindow());
    rig.rig.host.opened.front()->controller->closeWindow();
    endClosedSessions(panes);
    drainGuiEvents();
    CHECK(manager.bindingOf(rig.boot.id) == rig.rig.link.get());
    CHECK(showing(manager, rig.boot.id, pane) == first);
    CHECK(rig.rig.link->isAttached());
    CHECK(rig.daemon.waitUntilOnDaemon([&] { return showing(rig.daemon, rig.workspaces[0]) == 1; }));
    CHECK(rig.daemon.onDaemon(
        [&] { return rig.daemon.host->terminal(vtworkspace::SessionId { pane }) != nullptr; }));
}

TEST_CASE("DaemonLink: the window keeps its own active tab when another window activates a tab",
          "[attach][link]")
{
    auto rig = TwoWindowsOneWorkspace {};
    auto& manager = rig.manager();
    // A second tab, made on the daemon: both windows show it.
    auto const pane = rig.firstPane();
    rig.daemon.onDaemon([&] {
        std::ignore = rig.daemon.host->createTab({}, vtworkspace::SessionId { pane });
        return 0;
    });
    auto const tabsOf = [&](vtworkspace::WorkspaceId window) {
        return manager.model().workspace(window)->tabCount();
    };
    REQUIRE(waitUntil([&] { return tabsOf(rig.boot.id) == 2 && tabsOf(rig.secondWindow()) == 2; }));
    auto const activeRow = [&](vtworkspace::WorkspaceId window) {
        auto const* const workspace = manager.model().workspace(window);
        return workspace->indexOf(workspace->activeTab()->id());
    };

    // The second window activates its first tab; the first window activates its second.
    manager.activateTab(rig.secondWindow(), 0);
    manager.activateTab(rig.boot.id, 1);
    REQUIRE(activeRow(rig.secondWindow()) == 0);
    REQUIRE(activeRow(rig.boot.id) == 1);

    // The daemon hears of both, and pushes them to both windows: neither moves the other's.
    rig.daemon.onDaemon([&] { return 0; });
    for ([[maybe_unused]] auto const turn: std::views::iota(0, 20))
    {
        drainGuiEvents();
        QTest::qWait(10);
    }
    CHECK(activeRow(rig.secondWindow()) == 0);
    CHECK(activeRow(rig.boot.id) == 1);
}

TEST_CASE("DaemonLink: a quiet window does not ring the bell", "[attach][link][bell]")
{
    using vthost::proto::RaiseRole;
    auto rig = TwoWindowsOneWorkspace {};
    auto& manager = rig.manager();
    auto const pane = rig.firstPane();
    // Each window's focus reaches the daemon over its own connection: the second's first, then the first's,
    // reported on every poll so that it is the latest whatever order the two connections are read in.
    manager.setFocusedWindow(rig.secondWindow());
    REQUIRE(waitUntil([&] { return rig.secondLink().raiseRole() == RaiseRole::Raise; }));
    REQUIRE(waitUntil([&] {
        manager.setFocusedWindow(rig.boot.id);
        return rig.rig.link->raiseRole() == RaiseRole::Raise
               && rig.secondLink().raiseRole() == RaiseRole::Quiet;
    }));
    // What each window's session rings, as SessionChrome.qml hears it.
    auto rings = std::array { 0, 0 };
    auto* const loud = showing(manager, rig.boot.id, pane);
    auto* const quiet = showing(manager, rig.secondWindow(), pane);
    REQUIRE(loud != nullptr);
    REQUIRE(quiet != nullptr);
    QObject::connect(loud, &contour::session::TerminalSession::onBell, loud, [&rings](float) { ++rings[0]; });
    QObject::connect(
        quiet, &contour::session::TerminalSession::onBell, quiet, [&rings](float) { ++rings[1]; });
    auto alerts = 0;
    QObject::connect(quiet, &contour::session::TerminalSession::onAlert, quiet, [&alerts] { ++alerts; });

    rig.daemon.onDaemon([&] {
        rig.daemon.host->terminal(vtworkspace::SessionId { pane })->writeToScreen("\a");
        return 0;
    });

    REQUIRE(waitUntil([&] { return rings[0] == 1; }));
    settleGui();
    CHECK(rings[1] == 0);
    CHECK(alerts == 0);
}

TEST_CASE("DaemonLink: the raise holder notifies a finished command in an unselected workspace",
          "[attach][link][notify]")
{
    using vthost::proto::RaiseRole;
    auto daemon = DaemonFixture {};
    auto const ids = seedWorkspaces(daemon, 2);
    auto rig = LinkRig {
        daemon.endpoint(), {}, {}, { .kind = vthost::proto::AttachIntentKind::Select, .workspace = ids[0] }
    };
    auto& link = *rig.link;
    auto const boot = contour::test::ScopedController { rig.app.manager() };
    REQUIRE(pollUntilAttached(link, 10s).has_value());
    REQUIRE(waitUntil([&] { return link.workspaceOf(boot.id) == ids[0]; }));
    // A lone window raises.
    REQUIRE(waitUntil([&] { return link.raiseRole() == RaiseRole::Raise; }));

    // A command finishes in the workspace the window does not select.
    daemon.onDaemon([&] {
        auto const pane = daemon.host->model()
                              .workspace(vtworkspace::WorkspaceId { ids[1] })
                              ->tabAt(0)
                              ->rootPane()
                              ->session();
        daemon.host->terminal(pane)->writeToScreen(
            "\033]133;A\007$ \033]133;B\007make\r\n\033]133;C\007out\r\n\033]133;D;3\007");
        return 0;
    });

    // Told once, as a hidden pane's finish is.
    REQUIRE(waitUntil([&] { return !rig.host.notifier.raised.empty(); }, 3000));
    CHECK(rig.host.notifier.raised.size() == 1);
    CHECK(rig.host.notifier.raised.front().title.contains("make"));
    // A later directory change tells it no more.
    daemon.onDaemon([&] {
        auto const pane = daemon.host->model()
                              .workspace(vtworkspace::WorkspaceId { ids[1] })
                              ->tabAt(0)
                              ->rootPane()
                              ->session();
        daemon.host->terminal(pane)->writeToScreen("\033]2;renamed\007");
        return 0;
    });
    settleGui();
    CHECK(rig.host.notifier.raised.size() == 1);

    // Nor does the window selecting that workspace and then the first again: the finish is still in the
    // directory, and still old news.
    auto& manager = rig.app.manager();
    auto const window = boot.windowId;
    auto const displayed = [&] {
        return displayedBy(manager, link, window);
    };
    REQUIRE(link.selectWorkspace(ids[1]).has_value());
    REQUIRE(waitUntil([&] { return displayed() == ids[1]; }, 2000));
    REQUIRE(link.selectWorkspace(ids[0]).has_value());
    REQUIRE(waitUntil([&] { return displayed() == ids[0]; }, 2000));
    settleGui();
    CHECK(rig.host.notifier.raised.size() == 1);
}

TEST_CASE("DaemonLink: a jump to a bell in an unselected workspace shows its pane once the switch lands",
          "[attach][link][attention]")
{
    auto daemon = DaemonFixture {};
    auto const ids = seedWorkspaces(daemon, 2);
    auto rig = LinkRig {
        daemon.endpoint(), {}, {}, { .kind = vthost::proto::AttachIntentKind::Select, .workspace = ids[0] }
    };
    auto& link = *rig.link;
    auto& manager = rig.app.manager();
    auto const boot = contour::test::ScopedController { manager };
    REQUIRE(pollUntilAttached(link, 10s).has_value());
    REQUIRE(waitUntil([&] { return link.workspaceOf(boot.id) == ids[0]; }));
    auto* const controller = manager.controllerOf(boot.windowId);
    REQUIRE(controller != nullptr);

    // The bell of a pane in the workspace the window does not show.
    daemon.onDaemon([&] {
        auto const pane = daemon.host->model()
                              .workspace(vtworkspace::WorkspaceId { ids[1] })
                              ->tabAt(0)
                              ->rootPane()
                              ->session();
        daemon.host->terminal(pane)->writeToScreen("\a");
        return 0;
    });
    auto belled = std::optional<contour::window::DirectoryPane> {};
    REQUIRE(waitUntil(
        [&] {
            for (auto const& workspace: link.directory().workspaces)
                if (workspace.id == ids[1])
                    for (auto const& tab: workspace.tabs)
                        for (auto const& pane: tab.panes)
                            if (pane.bellSerial > 0)
                                belled = pane;
            return belled.has_value();
        },
        3000));

    // The pane is not shown, and not seen, until the switch lands.
    controller->jumpToNextAttention();
    CHECK(controller->jumpPending());
    CHECK(controller->tabAttention().of(belled->key).seenBellSerial == 0);
    REQUIRE(waitUntil([&] { return displayedBy(manager, link, boot.windowId) == ids[1]; }, 3000));
    REQUIRE(waitUntil([&] { return !controller->jumpPending(); }, 3000));
    CHECK(controller->tabAttention().of(belled->key).seenBellSerial == belled->bellSerial);
    auto const* const active = controller->activeSession();
    REQUIRE(active != nullptr);
    CHECK(manager.paneKeyOf(active->modelSessionId()) == belled->key);

    // The bell is seen in this window now: nothing more to jump to.
    controller->jumpToNextAttention();
    CHECK_FALSE(controller->jumpPending());
}

TEST_CASE("DaemonLink: a switch back to the shown workspace while another is under way stays",
          "[attach][link][switch]")
{
    auto daemon = DaemonFixture {};
    auto const ids = seedWorkspaces(daemon, 2);
    auto rig = LinkRig {
        daemon.endpoint(), {}, {}, { .kind = vthost::proto::AttachIntentKind::Select, .workspace = ids[0] }
    };
    auto& link = *rig.link;
    auto& manager = rig.app.manager();
    auto const boot = contour::test::ScopedController { manager };
    REQUIRE(pollUntilAttached(link, 10s).has_value());
    REQUIRE(waitUntil([&] { return link.workspaceOf(boot.id) == ids[0]; }));
    REQUIRE(waitUntil([&] { return !sessionsOf(manager, boot.id).empty(); }));

    // To the second, then at once back to the first, the one shown.
    auto toSecond = std::optional<std::expected<uint64_t, std::string>> {};
    auto toFirst = std::optional<std::expected<uint64_t, std::string>> {};
    REQUIRE(link.selectWorkspace(ids[1], [&](auto landed) { toSecond = std::move(landed); }).has_value());
    REQUIRE(link.selectWorkspace(ids[0], [&](auto landed) { toFirst = std::move(landed); }).has_value());
    REQUIRE(toSecond.has_value());
    CHECK_FALSE(toSecond->has_value());
    REQUIRE(toFirst.has_value());
    CHECK(*toFirst == ids[0]);

    // Whatever the daemon states on its way, the window ends where it was.
    REQUIRE(waitUntil([&] { return link.liveController()->selectedWorkspace() == ids[0]; }, 2000));
    for ([[maybe_unused]] auto const turn: std::views::iota(0, 20))
    {
        drainGuiEvents();
        QTest::qWait(10);
    }
    CHECK(link.workspaceOf(manager.controllerOf(boot.windowId)->displayedWorkspace()) == ids[0]);
    CHECK(daemon.onDaemon([&] { return showing(daemon, ids[0]); }) == 1);
}

namespace
{
/// @return The session of the first pane @p window displays, if it displays any.
[[nodiscard]] contour::session::TerminalSession* firstDisplayedSession(
    contour::session::TerminalSessionManager& manager, contour::window::WindowId window)
{
    drainGuiEvents();
    auto const* const controller = manager.controllerOf(window);
    if (controller == nullptr)
        return nullptr;
    auto const sessions = sessionsOf(manager, controller->displayedWorkspace());
    return sessions.empty() ? nullptr : sessions.front();
}

/// Checks that @p window displays the daemon's @p pane as a live pane: what its program prints lands in the
/// window, and what is typed into the window reaches the program.
/// @param tag Makes the printed and typed text unique to the caller.
void requireDrivesPane(DaemonFixture& daemon,
                       contour::session::TerminalSessionManager& manager,
                       contour::window::WindowId window,
                       vtworkspace::SessionId pane,
                       std::string_view tag)
{
    REQUIRE(waitUntil([&] { return firstDisplayedSession(manager, window) != nullptr; }, 3000));
    auto const printed = std::format("printed-{}", tag);
    daemon.onDaemon([&] {
        daemon.host->terminal(pane)->writeToScreen(printed);
        return 0;
    });
    CHECK(waitUntil(
        [&] {
            auto* const session = firstDisplayedSession(manager, window);
            if (session == nullptr)
                return false;
            auto const guard = std::lock_guard { session->terminal() };
            return session->terminal().primaryScreen().grid().renderMainPageText().contains(printed);
        },
        3000));
    auto* const session = firstDisplayedSession(manager, window);
    REQUIRE(session != nullptr);
    auto const typed = std::format("typed-{}", tag);
    std::ignore = session->terminal().device().write(typed);
    CHECK(waitUntil([&] { return daemon.ptyInput(pane).contains(typed); }, 3000));
}

/// Has @p link's window show @p workspace, and waits until it does.
void switchShownWorkspace(contour::remote::DaemonLink& link,
                          contour::session::TerminalSessionManager& manager,
                          contour::window::WindowId window,
                          uint64_t workspace)
{
    auto landed = std::optional<std::expected<uint64_t, std::string>> {};
    REQUIRE(link.selectWorkspace(workspace, [&](auto outcome) { landed = std::move(outcome); }).has_value());
    REQUIRE(waitUntil([&] { return landed.has_value(); }, 3000));
    REQUIRE(*landed == workspace);
    REQUIRE(waitUntil([&] { return displayedBy(manager, link, window) == workspace; }, 3000));
}

/// @return The one pane of @p daemon's @p workspace. Not on the daemon thread.
[[nodiscard]] vtworkspace::SessionId onlyPaneOf(DaemonFixture& daemon, uint64_t workspace)
{
    return daemon.onDaemon([&] {
        return daemon.host->model()
            .workspace(vtworkspace::WorkspaceId { workspace })
            ->tabAt(0)
            ->rootPane()
            ->session();
    });
}

/// @return What a link's first connection follows to select @p workspace.
[[nodiscard]] vthost::proto::AttachIntent selecting(uint64_t workspace)
{
    return vthost::proto::AttachIntent { .kind = vthost::proto::AttachIntentKind::Select,
                                         .workspace = workspace };
}

/// Has the window @p boot of @p rig attach to the first of @p workspaces, show the second, then the first
/// again, with the panes the first switch let go of ended as their exit watcher ends them.
void switchAwayAndBack(LinkRig& rig,
                       contour::test::ScopedController const& boot,
                       std::span<uint64_t const> workspaces)
{
    auto& link = *rig.link;
    auto& manager = rig.app.manager();
    REQUIRE(pollUntilAttached(link, 10s).has_value());
    REQUIRE(waitUntil([&] { return link.workspaceOf(boot.id) == workspaces[0]; }));
    REQUIRE(waitUntil([&] { return !sessionsOf(manager, boot.id).empty(); }));
    auto const firstPanes = watchedSessionsOf(manager, boot.id);
    switchShownWorkspace(link, manager, boot.windowId, workspaces[1]);
    endClosedSessions(firstPanes);
    REQUIRE(waitUntil([&] {
        drainGuiEvents();
        return std::ranges::all_of(firstPanes, [](auto const& pane) { return pane.isNull(); });
    }));
    switchShownWorkspace(link, manager, boot.windowId, workspaces[0]);
}
} // namespace

TEST_CASE("DaemonLink: a window switched to another workspace and back shows and drives its pane again",
          "[attach][link][switch]")
{
    auto daemon = DaemonFixture {};
    auto const ids = seedWorkspaces(daemon, 2);
    auto const pane = onlyPaneOf(daemon, ids[0]);
    auto rig = LinkRig { daemon.endpoint(), {}, {}, selecting(ids[0]) };
    auto const boot = contour::test::ScopedController { rig.app.manager() };
    switchAwayAndBack(rig, boot, ids);
    requireDrivesPane(daemon, rig.app.manager(), boot.windowId, pane, "back");
}

TEST_CASE("DaemonLink: a window switched back before the panes it left ended shows and drives its pane again",
          "[attach][link][switch]")
{
    auto daemon = DaemonFixture {};
    auto const ids = seedWorkspaces(daemon, 2);
    auto const pane = onlyPaneOf(daemon, ids[0]);
    auto rig = LinkRig { daemon.endpoint(), {}, {}, selecting(ids[0]) };
    auto& link = *rig.link;
    auto& manager = rig.app.manager();
    auto const boot = contour::test::ScopedController { manager };
    REQUIRE(pollUntilAttached(link, 10s).has_value());
    REQUIRE(waitUntil([&] { return link.workspaceOf(boot.id) == ids[0]; }));
    REQUIRE(waitUntil([&] { return !sessionsOf(manager, boot.id).empty(); }));
    auto const firstPanes = watchedSessionsOf(manager, boot.id);

    // Back again while the pane the first switch detached is still alive: it holds the session no more.
    switchShownWorkspace(link, manager, boot.windowId, ids[1]);
    switchShownWorkspace(link, manager, boot.windowId, ids[0]);
    requireDrivesPane(daemon, manager, boot.windowId, pane, "quick");
    endClosedSessions(firstPanes);
    drainGuiEvents();
}

TEST_CASE("DaemonLink: a client started after one switched away and back shows and drives the pane",
          "[attach][link][switch]")
{
    auto daemon = DaemonFixture {};
    auto const ids = seedWorkspaces(daemon, 2);
    auto const pane = onlyPaneOf(daemon, ids[0]);
    {
        auto rig = LinkRig { daemon.endpoint(), {}, {}, selecting(ids[0]) };
        auto const boot = contour::test::ScopedController { rig.app.manager() };
        switchAwayAndBack(rig, boot, ids);
    }
    REQUIRE(daemon.waitUntilOnDaemon([&daemon] { return daemon.live.empty(); }));

    // The client restarts: a new one attaches as a launch does, and shows the workspace selected last.
    auto rig = LinkRig { daemon.endpoint(), {}, {}, launch() };
    auto const boot = contour::test::ScopedController { rig.app.manager() };
    REQUIRE(pollUntilAttached(*rig.link, 10s).has_value());
    REQUIRE(waitUntil([&] { return rig.link->workspaceOf(boot.id) == ids[0]; }));
    requireDrivesPane(daemon, rig.app.manager(), boot.windowId, pane, "restarted");
}

TEST_CASE("DaemonLink: seen marks survive a reconnect", "[attach][link][reconnect]")
{
    using vthost::client::ConnectionState;
    auto daemon = DaemonFixture {};
    std::ignore = seedWorkspaces(daemon, 1);
    auto rig = LinkRig { daemon.endpoint(), {}, {}, launch() };
    auto& link = *rig.link;
    auto& manager = rig.app.manager();
    auto const boot = contour::test::ScopedController { manager };
    REQUIRE(pollUntilAttached(link, 10s).has_value());
    REQUIRE(waitUntil([&] { return !sessionsOf(manager, boot.id).empty(); }));
    // A second tab, in front: the first one is out of sight.
    auto const background = daemon.onDaemon([&daemon] {
        auto* const workspace = daemon.host->model().workspaceAt(0);
        auto const first = workspace->tabAt(0)->rootPane()->session();
        std::ignore = daemon.host->createTab({}, first);
        return first.value;
    });
    REQUIRE(waitUntil([&] { return manager.model().workspace(boot.id)->tabCount() == 2; }));
    manager.activateTab(boot.id, 1);
    auto& ledger = boot.controller->tabAttention();
    auto const key = contour::window::PaneKey { .value = background };

    // Output in the hidden tab marks it, once the connection's first moments -- which repaint every pane --
    // are over (window::ResyncGracePeriod).
    rig.app.steadyClock().advance(std::chrono::seconds { 1 });
    daemon.onDaemon([&] {
        daemon.host->terminal(vtworkspace::SessionId { background })->writeToScreen("unseen output");
        return 0;
    });
    REQUIRE(waitUntil([&] { return ledger.of(key).activity != contour::window::TabActivity::None; }));

    // The connection goes and comes back: the mark is kept under the daemon's session id.
    auto const* const controller = link.liveController();
    daemon.dropConnections();
    REQUIRE(waitUntil([&] { return controller->connectionState() != ConnectionState::Connected; }));
    REQUIRE(waitUntil([&] { return controller->connectionState() == ConnectionState::Connected; }, 3000));
    drainGuiEvents();
    CHECK(ledger.of(key).activity != contour::window::TabActivity::None);
    CHECK(link.workspaceOf(boot.id).has_value());
}

TEST_CASE("DaemonLink: tear-off in daemon mode opens a window on the same workspace and moves nothing",
          "[attach][link][move]")
{
    auto daemon = DaemonFixture {};
    std::ignore = seedWorkspaces(daemon, 1);
    auto rig = LinkRig { daemon.endpoint(), {}, {}, launch() };
    auto& link = *rig.link;
    auto& manager = rig.app.manager();
    auto const boot = contour::test::ScopedController { manager };
    REQUIRE(pollUntilAttached(link, 10s).has_value());
    REQUIRE(waitUntil([&] { return !sessionsOf(manager, boot.id).empty(); }));
    auto const second = daemon.onDaemon([&daemon] {
        auto const first = daemon.host->model().workspaceAt(0)->tabAt(0)->rootPane()->session();
        return daemon.host->createTab({}, first)->rootPane()->session().value;
    });
    REQUIRE(waitUntil([&] { return manager.model().workspace(boot.id)->tabCount() == 2; }));
    manager.activateTab(boot.id, 0);
    auto const before = daemon.onDaemon([&daemon] { return daemon.host->sessionIds(); });

    // The second tab is torn off.
    manager.tearOffTabToNewWindow(boot.id, 1, nullptr);

    // A window of a link of its own shows the same workspace, the torn tab in front.
    REQUIRE(waitUntil([&] { return rig.host.opened.size() == 1; }, 3000));
    auto const torn = [&] {
        return rig.host.opened.front()->controller->displayedWorkspace();
    };
    REQUIRE(waitUntil([&] { return manager.model().workspace(torn())->tabCount() == 2; }));
    REQUIRE(rig.host.windowLinks.size() == 1);
    CHECK(rig.host.windowLinks.front()->workspaceOf(torn()) == link.workspaceOf(boot.id));
    auto const* const front = manager.model().workspace(torn())->activeTab();
    REQUIRE(front != nullptr);
    auto const* const frontSession = manager.sessionForId(front->rootPane()->firstLeaf()->session());
    REQUIRE(frontSession != nullptr);
    CHECK(manager.hostSessionIdOf(*frontSession) == second);
    // Nothing moved: the first window keeps both tabs, and the daemon its sessions.
    CHECK(manager.model().workspace(boot.id)->tabCount() == 2);
    CHECK(daemon.onDaemon([&daemon] { return daemon.host->sessionIds(); }) == before);
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

/// One daemon whose two workspaces two windows show, each on a link of its own: window A shows
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
    /// @return Window B, the window link's.
    [[nodiscard]] vtworkspace::WorkspaceId windowB() const { return rig.host.opened.front()->id; }

    /// Waits until both windows show their workspaces, each on its own link.
    void awaitShown()
    {
        REQUIRE(pollUntilAttached(*rig.link, 10s).has_value());
        REQUIRE(waitUntil([&] { return rig.link->workspaceOf(windowA()) == workspaces[0]; }));
        // Window B: a window of a link of its own, selecting the second workspace.
        auto config = rig.link->siblingConfig();
        config.controller.intent = { .kind = vthost::proto::AttachIntentKind::Select,
                                     .workspace = workspaces[1] };
        config.opening = contour::remote::WindowOpening {};
        rig.host.startWindowLink(std::move(config));
        REQUIRE(waitUntil([&] { return rig.host.opened.size() == 1; }));
        REQUIRE(waitUntil([&] {
            return sessionsOf(manager(), windowA()).size() == 3
                   && sessionsOf(manager(), windowB()).size() == 1;
        }));
        REQUIRE(manager().bindingOf(windowA()) == rig.link.get());
        REQUIRE(manager().bindingOf(windowB()) == rig.host.windowLinks.front().get());
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
          "[attach][link][move]")
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
    REQUIRE(pollUntilAttached(linkA, 10s).has_value());
    REQUIRE(pollUntilAttached(linkB, 10s).has_value());
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
    auto windowLinks = std::vector<std::unique_ptr<contour::remote::DaemonLink>> {};
    auto app = contour::test::TestApp { std::make_unique<contour::test::MockPtySessionFactory>() };
    auto host = FakeLinkHost { app.manager(), windowLinks };
    auto config = linkConfig(daemon.endpoint(),
                             timeline,
                             {},
                             {},
                             { .kind = vthost::proto::AttachIntentKind::Default, .workspace = 0 },
                             { .kind = vthost::proto::SwitchDaemonKind::Label, .name = "admin" });
    config.locality = contour::window::DaemonLocality::Local;
    config.clientRelease = "0.0.1"; // drifted from any daemon this suite runs
    config.upkeep = upkeep;
    link = std::make_unique<contour::remote::DaemonLink>(
        contour::remote::DaemonLink::Id { 1 }, std::move(config), host, app.manager());
    REQUIRE(pollUntilAttached(*link, 10s).has_value());

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

namespace
{
/// Whether an UpgradeRig's link can connect to its daemon.
enum class Reachability : uint8_t
{
    Unreachable = 0,
    Reachable,
};

/// A link to a local daemon on a test app, with a window bound to it for the notices.
struct UpgradeRig
{
    DaemonFixture daemon { {}, withLiveUpgrade() };
    TestTimeline timeline;
    /// Declared before the app, as LinkRig's: the panes unregister from the link's controller as they go.
    std::unique_ptr<contour::remote::DaemonLink> link;
    std::vector<std::unique_ptr<contour::remote::DaemonLink>> windowLinks;
    contour::test::TestApp app { std::make_unique<contour::test::MockPtySessionFactory>() };
    FakeLinkHost host { app.manager(), windowLinks };
    std::unique_ptr<contour::test::ScopedController> window;

    /// @param upkeep Whose daemon the link says it is.
    /// @param reachability Whether the link's daemon can be reached.
    explicit UpgradeRig(contour::window::DaemonUpkeep upkeep,
                        Reachability reachability = Reachability::Reachable)
    {
        std::ignore = seedWorkspaces(daemon, 1);
        window = std::make_unique<contour::test::ScopedController>(app.manager());
        auto config = linkConfig(daemon.endpoint(), timeline, "home");
        config.locality = contour::window::DaemonLocality::Local;
        config.upkeep = upkeep;
        if (reachability == Reachability::Unreachable)
            unreachable(config);
        link = std::make_unique<contour::remote::DaemonLink>(
            contour::remote::DaemonLink::Id { 1 }, std::move(config), host, app.manager());
        if (reachability == Reachability::Reachable)
        {
            REQUIRE(pollUntilAttached(*link, 10s).has_value());
            REQUIRE(link->bindPendingWindow(*window->controller) == contour::remote::WindowAdoption::Adopted);
        }
        else
        {
            app.manager().bindWindow(window->id, *link);
        }
        link->offerDaemonRestartIfDrifted();
    }

    UpgradeRig(UpgradeRig const&) = delete;
    UpgradeRig& operator=(UpgradeRig const&) = delete;
    UpgradeRig(UpgradeRig&&) = delete;
    UpgradeRig& operator=(UpgradeRig&&) = delete;

    ~UpgradeRig()
    {
        link->stop();
        window.reset();
        drainGuiEvents();
    }
};
} // namespace

TEST_CASE("DaemonLink: an upgrade by command reaches the host once for a connected daemon of this client",
          "[attach][link][drift]")
{
    auto rig = UpgradeRig { contour::window::DaemonUpkeep::ThisClient };
    auto const started = rig.link->upgradeDaemonOnCommand();
    CHECK(started.has_value());
    CHECK(rig.host.upgrades == 1);
    CHECK(rig.link->drift().isUpgrading());
}

TEST_CASE("DaemonLink: an upgrade by command is refused, with the reason, for a daemon not this client's",
          "[attach][link][drift]")
{
    auto rig = UpgradeRig { contour::window::DaemonUpkeep::None };
    auto const started = rig.link->upgradeDaemonOnCommand();
    REQUIRE_FALSE(started.has_value());
    CHECK(started.error() == contour::window::describe(contour::window::UpgradeVerdict::NotThisClients));
    CHECK(rig.host.upgrades == 0);
}

TEST_CASE("DaemonLink: a second upgrade by command while one is under way is refused",
          "[attach][link][drift]")
{
    auto rig = UpgradeRig { contour::window::DaemonUpkeep::ThisClient };
    REQUIRE(rig.link->upgradeDaemonOnCommand().has_value());
    auto const second = rig.link->upgradeDaemonOnCommand();
    REQUIRE_FALSE(second.has_value());
    CHECK(second.error() == contour::window::UpgradeAlreadyUnderWay);
    CHECK(rig.host.upgrades == 1);
}

TEST_CASE("DaemonLink: an upgrade by command is refused for a link that is not connected",
          "[attach][link][drift]")
{
    // Live, with a controller that never connects: that is not a connection to judge a daemon by.
    auto rig = UpgradeRig { contour::window::DaemonUpkeep::ThisClient, Reachability::Unreachable };
    REQUIRE(rig.link->isLive());
    auto const started = rig.link->upgradeDaemonOnCommand();
    REQUIRE_FALSE(started.has_value());
    CHECK(started.error() == contour::window::describe(contour::window::UpgradeVerdict::NotConnected));
    CHECK(rig.host.upgrades == 0);
}

TEST_CASE("DaemonLink: an upgrade the application declines leaves the model settled and tells the user",
          "[attach][link][drift]")
{
    auto rig = UpgradeRig { contour::window::DaemonUpkeep::ThisClient };
    rig.host.upgradeAnswer = contour::remote::UpgradeStart::AlreadyUnderWay;
    CHECK(rig.link->upgradeDaemonOnCommand().has_value());
    CHECK(rig.host.upgrades == 1);
    CHECK_FALSE(rig.link->drift().isUpgrading());
    CHECK(rig.window->controller->notices().toastText().contains("already being upgraded"));
    // Settled: the next request goes through to the host again.
    rig.host.upgradeAnswer = contour::remote::UpgradeStart::Started;
    CHECK(rig.link->upgradeDaemonOnCommand().has_value());
    CHECK(rig.host.upgrades == 2);
}

TEST_CASE("DaemonLink: a failed upgrade by command is a toast only when no badge shows it",
          "[attach][link][drift]")
{
    auto rig = UpgradeRig { contour::window::DaemonUpkeep::ThisClient };
    auto& notices = rig.window->controller->notices();

    SECTION("no drift: the badge is invisible")
    {
        rig.link->drift().show({}, contour::window::DriftOffer::None);
        REQUIRE(rig.link->upgradeDaemonOnCommand().has_value());
        rig.link->drift().noteUpgradeFailed(QStringLiteral("no room"));
        CHECK(notices.toastText().contains("no room"));
    }

    SECTION("a drift on show: the badge carries the failure")
    {
        rig.link->drift().show(vthost::client::DriftNotice { .kind = vthost::client::DriftKind::DaemonOlder,
                                                             .text = "older",
                                                             .confirmation = {} },
                               contour::window::DriftOffer::UpgradeAndRestart);
        REQUIRE(rig.link->upgradeDaemonOnCommand().has_value());
        REQUIRE(rig.link->drift().isVisible());
        rig.link->drift().noteUpgradeFailed(QStringLiteral("no room"));
        CHECK_FALSE(notices.toastShown());
    }
}

namespace
{

/// How often a window asked to ring and to alert, as Main.qml hears it; deaf once destroyed.
struct WindowRings
{
    QObject receiver; ///< What the counts are connected through: disconnected as this goes.
    int bells = 0;
    int alerts = 0;

    explicit WindowRings(contour::window::WindowController& controller)
    {
        QObject::connect(&controller,
                         &contour::window::WindowController::bellRequested,
                         &receiver,
                         [this](QString const& /*source*/, double /*volume*/) { ++bells; });
        QObject::connect(
            &controller, &contour::window::WindowController::alertRequested, &receiver, [this] { ++alerts; });
    }
    WindowRings(WindowRings const&) = delete;
    WindowRings& operator=(WindowRings const&) = delete;
    WindowRings(WindowRings&&) = delete;
    WindowRings& operator=(WindowRings&&) = delete;
    ~WindowRings() = default;
};

/// @return Whether @p notification says @p text, in its title or its body.
[[nodiscard]] bool says(vtbackend::DesktopNotification const& notification, std::string_view text)
{
    return notification.title.contains(text) || notification.body.contains(text);
}

/// What a shell's prompt and one command that exits with 3 print, with OSC 133 marks.
constexpr auto FinishedMake =
    std::string_view { "\033]133;A\007$ \033]133;B\007make\r\n\033]133;C\007out\r\n\033]133;D;3\007" };

} // namespace

TEST_CASE("DaemonLink: the raise holder rings and notifies for a pane another window shows, once",
          "[attach][link][bell][notify]")
{
    using vthost::proto::RaiseRole;
    OneDaemonTwoWindows rig;
    rig.awaitShown();
    auto& manager = rig.manager();
    auto& linkA = *rig.rig.link;
    auto& linkB = *rig.rig.host.windowLinks.front();
    // Window B, opened last, was focused as it opened, and reported so as it bound its workspace: that report
    // lands first, or it would take the role back from A in the middle of what follows.
    REQUIRE(waitUntil([&] { return linkB.raiseRole() == RaiseRole::Raise; }, 3000));
    // Then window A, showing the first workspace, is focused last: it raises.
    manager.setFocusedWindow(rig.windowA());
    REQUIRE(waitUntil(
        [&] { return linkA.raiseRole() == RaiseRole::Raise && linkB.raiseRole() == RaiseRole::Quiet; },
        3000));
    auto ringsA = WindowRings { *rig.boot.controller };
    auto ringsB = WindowRings { *rig.rig.host.opened.front()->controller };
    auto* const paneB = showingIn(manager, rig.windowB(), rig.other);
    REQUIRE(paneB != nullptr);
    auto paneRings = 0;
    QObject::connect(
        paneB, &contour::session::TerminalSession::onBell, paneB, [&paneRings](float) { ++paneRings; });

    // An agent in the second workspace, which only window B shows, rings and asks for the user.
    rig.daemon.onDaemon([&] {
        rig.daemon.host->terminal(rig.other)->writeToScreen("\a\033]9;needs input\033\\");
        return 0;
    });

    auto const& raised = rig.rig.host.notifier.raised;
    REQUIRE(waitUntil([&] { return ringsA.bells == 1 && !raised.empty(); }, 3000));
    settleGui();
    CHECK(ringsA.bells == 1);
    CHECK(ringsA.alerts == 1);
    REQUIRE(raised.size() == 1);
    CHECK(says(raised.front(), "needs input"));
    // The window that shows the pane marks it and stays quiet.
    CHECK(ringsB.bells == 0);
    CHECK(ringsB.alerts == 0);
    CHECK(paneRings == 0);

    // A switch to that workspace and back raises none of it again.
    auto const displayed = [&] {
        return displayedBy(manager, linkA, rig.boot.windowId);
    };
    REQUIRE(linkA.selectWorkspace(rig.workspaces[1]).has_value());
    REQUIRE(waitUntil([&] { return displayed() == rig.workspaces[1]; }, 2000));
    REQUIRE(linkA.selectWorkspace(rig.workspaces[0]).has_value());
    REQUIRE(waitUntil([&] { return displayed() == rig.workspaces[0]; }, 2000));
    settleGui();
    CHECK(ringsA.bells == 1);
    CHECK(raised.size() == 1);
}

TEST_CASE("DaemonLink: a mirrored pane's bell is judged by the raise role it arrived under",
          "[attach][link][bell][raise]")
{
    using vthost::proto::RaiseRole;
    OneDaemonTwoWindows rig;
    rig.awaitShown();
    auto& manager = rig.manager();
    auto& linkA = *rig.rig.link;
    auto& linkB = *rig.rig.host.windowLinks.front();
    // Settled as the test above settles them: B's own report first, then A focused last.
    REQUIRE(waitUntil([&] { return linkB.raiseRole() == RaiseRole::Raise; }, 3000));
    manager.setFocusedWindow(rig.windowA());
    REQUIRE(waitUntil(
        [&] { return linkA.raiseRole() == RaiseRole::Raise && linkB.raiseRole() == RaiseRole::Quiet; },
        3000));
    settleGui();
    auto* const paneB = showingIn(manager, rig.windowB(), rig.other);
    REQUIRE(paneB != nullptr);
    auto rings = 0;
    QObject::connect(paneB, &contour::session::TerminalSession::onBell, paneB, [&rings](float) { ++rings; });
    auto& controllerB = *linkB.liveController();

    SECTION("a bell that arrived while quiet stays quiet, whatever role follows it")
    {
        contour::remote::NativeControllerTestAccess::bellThenRole(
            controllerB, rig.other.value, RaiseRole::Raise);
        settleGui();
        CHECK(linkB.raiseRole() == RaiseRole::Raise);
        CHECK(rings == 0);
    }
    SECTION("a bell that arrived while raising rings, whatever role follows it")
    {
        contour::remote::NativeControllerTestAccess::bellThenRole(
            controllerB, rig.other.value, RaiseRole::Raise);
        settleGui();
        REQUIRE(linkB.raiseRole() == RaiseRole::Raise);
        rings = 0;
        contour::remote::NativeControllerTestAccess::bellThenRole(
            controllerB, rig.other.value, RaiseRole::Quiet);
        settleGui();
        CHECK(linkB.raiseRole() == RaiseRole::Quiet);
        CHECK(rings == 1);
    }
}

TEST_CASE("DaemonLink: a lone window rings and notifies for a workspace it does not select",
          "[attach][link][bell][notify]")
{
    using vthost::proto::RaiseRole;
    auto daemon = DaemonFixture {};
    auto const ids = seedWorkspaces(daemon, 2);
    auto rig = LinkRig {
        daemon.endpoint(), {}, {}, { .kind = vthost::proto::AttachIntentKind::Select, .workspace = ids[0] }
    };
    auto& link = *rig.link;
    auto const boot = contour::test::ScopedController { rig.app.manager() };
    REQUIRE(pollUntilAttached(link, 10s).has_value());
    REQUIRE(waitUntil([&] { return link.workspaceOf(boot.id) == ids[0]; }));
    REQUIRE(waitUntil([&] { return link.raiseRole() == RaiseRole::Raise; }));
    auto rings = WindowRings { *boot.controller };
    auto const elsewhere = daemon.onDaemon([&] {
        return daemon.host->model()
            .workspace(vtworkspace::WorkspaceId { ids[1] })
            ->tabAt(0)
            ->rootPane()
            ->session();
    });
    auto const& raised = rig.host.notifier.raised;

    SECTION("a bell and a desktop notification")
    {
        daemon.onDaemon([&] {
            daemon.host->terminal(elsewhere)->writeToScreen("\a\033]9;needs input\033\\");
            return 0;
        });
        REQUIRE(waitUntil([&] { return rings.bells == 1 && !raised.empty(); }, 3000));
        settleGui();
        CHECK(rings.bells == 1);
        REQUIRE(raised.size() == 1);
        CHECK(says(raised.front(), "needs input"));
        // Marked in the directory until the window shows the pane.
        auto const directory = link.liveController()->directory();
        auto const marked = std::ranges::any_of(directory.workspaces, [&](auto const& workspace) {
            return std::ranges::any_of(workspace.tabs, [&](auto const& tab) {
                return std::ranges::any_of(tab.panes, [&](auto const& pane) {
                    return pane.key.value == elsewhere.value && pane.bell == vthost::proto::PaneBell::Rang;
                });
            });
        });
        CHECK(marked);
    }
    SECTION("a Bell-only finish policy rings for a finish there, and notifies nothing")
    {
        rig.host.finishPolicy.action = contour::config::NotifyAction::Bell;
        daemon.onDaemon([&] {
            daemon.host->terminal(elsewhere)->writeToScreen(FinishedMake);
            return 0;
        });
        REQUIRE(waitUntil([&] { return rings.bells == 1; }, 3000));
        settleGui();
        CHECK(rings.bells == 1);
        CHECK(raised.empty());
    }
}

TEST_CASE("DaemonLink: a window whose given-up switch reverts to a workspace that ended follows the daemon",
          "[attach][link][switch]")
{
    // The window shows the second workspace, switches to the first, and gives that up after the second's last
    // session ended. The daemon keeps an ended workspace in its model, empty; it is no workspace to select,
    // so no batch states it again.
    auto daemon = DaemonFixture {};
    auto const ids = seedWorkspaces(daemon, 2);
    auto const shown = ids[1];
    auto const other = ids[0];
    auto rig = LinkRig {
        daemon.endpoint(), {}, {}, { .kind = vthost::proto::AttachIntentKind::Select, .workspace = shown }
    };
    auto& link = *rig.link;
    auto& manager = rig.app.manager();
    auto const boot = contour::test::ScopedController { manager };
    REQUIRE(pollUntilAttached(link, 10s).has_value());
    REQUIRE(waitUntil([&] { return link.workspaceOf(boot.id) == shown; }));
    REQUIRE(waitUntil([&] { return !sessionsOf(manager, boot.id).empty(); }));
    auto const window = boot.windowId;

    // Nothing the daemon says reaches the window until the end: the switch to the other workspace is under
    // way when the shown one ends.
    daemon.holdEverything();
    REQUIRE(link.selectWorkspace(other).has_value());
    REQUIRE(daemon.waitUntilOnDaemon([&] { return showing(daemon, other) == 1; }));
    daemon.onDaemon([&] {
        auto* workspace = daemon.host->model().workspace(vtworkspace::WorkspaceId { shown });
        daemon.host->handleSessionExit(workspace->tabAt(0)->rootPane()->session());
        return 0;
    });
    REQUIRE(daemon.waitUntilOnDaemon([&] {
        auto const* const ended = daemon.host->model().workspace(vtworkspace::WorkspaceId { shown });
        return ended == nullptr || ended->tabCount() == 0;
    }));
    // Given up for the one the window shows, which no batch will state again: it holds no tab any more.
    REQUIRE(link.selectWorkspace(shown).has_value());
    daemon.releaseHeld();

    // The window stays, and shows the workspace the daemon selects in place of the one that ended.
    auto const displayed = [&] {
        return displayedBy(manager, link, window);
    };
    CHECK(waitUntil([&] { return displayed() == other; }, 3000));
    settleGui();
    CHECK(manager.controllerOf(window) != nullptr);
    CHECK(displayed() == other);
}

TEST_CASE("DaemonLink: a switch to a workspace the daemon does not have fails at once",
          "[attach][link][switch]")
{
    auto daemon = DaemonFixture {};
    auto const ids = seedWorkspaces(daemon, 2);
    auto rig = LinkRig {
        daemon.endpoint(), {}, {}, { .kind = vthost::proto::AttachIntentKind::Select, .workspace = ids[0] }
    };
    auto& link = *rig.link;
    auto const boot = contour::test::ScopedController { rig.app.manager() };
    REQUIRE(pollUntilAttached(link, 10s).has_value());
    REQUIRE(waitUntil([&] { return link.workspaceOf(boot.id) == ids[0]; }));

    auto landed = std::optional<std::expected<uint64_t, std::string>> {};
    REQUIRE(
        link.selectWorkspace(
                999, [&landed](std::expected<uint64_t, std::string> outcome) { landed = std::move(outcome); })
            .has_value());
    // Answered by the daemon, well within the switch's own timeout.
    REQUIRE(waitUntil([&] { return landed.has_value(); }, 1000));
    REQUIRE_FALSE(landed->has_value());
    CHECK(landed->error().contains("no such workspace"));
    CHECK(link.workspaceOf(boot.id) == ids[0]);

    // The next switch is not held up by it.
    landed.reset();
    REQUIRE(link.selectWorkspace(
                    ids[1],
                    [&landed](std::expected<uint64_t, std::string> outcome) { landed = std::move(outcome); })
                .has_value());
    REQUIRE(waitUntil([&] { return landed.has_value(); }, 3000));
    CHECK(*landed == std::expected<uint64_t, std::string> { ids[1] });
}

TEST_CASE("DaemonLink: a New Window whose connection fails tells the window it was asked from, and ends",
          "[attach][link][window]")
{
    auto live = DaemonFixture {};
    std::ignore = seedWorkspaces(live, 1);
    // Where a daemon listened, and listens no more: a connect there is refused.
    auto const dead = [] {
        auto const gone = DaemonFixture {};
        return gone.endpoint();
    }();
    auto rig = LinkRig { live.endpoint() };
    auto const boot = contour::test::ScopedController { rig.app.manager() };
    REQUIRE(pollUntilAttached(*rig.link, 10s).has_value());

    auto config = linkConfig(dead, rig.timeline);
    config.opening = contour::remote::WindowOpening { .activeTab = std::nullopt,
                                                      .screen = nullptr,
                                                      .askedBy = boot.windowId };
    auto const endingsBefore = rig.host.endings;
    rig.host.startWindowLink(std::move(config));

    REQUIRE(waitUntil(
        [&] { return boot.controller->notices().toastText().contains("Could not open a window"); }, 3000));
    CHECK(rig.host.opened.empty());
    CHECK(rig.host.endings > endingsBefore);
}

TEST_CASE("DaemonLink: a New Window that never loads tells the window it was asked from, and ends",
          "[attach][link][window]")
{
    auto daemon = DaemonFixture {};
    std::ignore = seedWorkspaces(daemon, 1);
    auto rig = LinkRig { daemon.endpoint() };
    auto const boot = contour::test::ScopedController { rig.app.manager() };
    REQUIRE(pollUntilAttached(*rig.link, 10s).has_value());
    REQUIRE(waitUntil([&] { return rig.link->workspaceOf(boot.id).has_value(); }));

    rig.host.windowLoad = FakeLinkHost::WindowLoad::Fails;
    auto const endingsBefore = rig.host.endings;
    REQUIRE(rig.link->openWindowLike(boot.id, std::nullopt, nullptr).has_value());

    REQUIRE(waitUntil([&] { return rig.host.windowsAsked == 1; }, 3000));
    REQUIRE(waitUntil(
        [&] { return boot.controller->notices().toastText().contains("its window did not open"); }));
    CHECK(rig.host.opened.empty());
    CHECK(rig.host.endings > endingsBefore);
    // The window it was asked from still shows its workspace on its own link.
    CHECK(rig.app.manager().bindingOf(boot.id) == rig.link.get());
}

TEST_CASE(
    "ContourGuiApp: a launch that names a workspace lists the daemon's, attaches to that one and opens its "
    "first window",
    "[contour][app][attach][link][startup]")
{
    using vthost::WorkspaceVisibility;
    auto daemon = DaemonFixture {};
    auto const ids = seedWorkspaces(daemon, 2);
    auto app = contour::test::TestApp { std::make_unique<contour::test::MockPtySessionFactory>() };
    // The wait turns it off until the first window; the rest of the suite expects the default.
    auto const restoreQuit = core::Finally { [] { QGuiApplication::setQuitOnLastWindowClosed(true); } };
    auto const transport = contour::remote::testing::makeTestTlsTransport(daemon.endpoint());

    REQUIRE(app.app().prepareLaunchAttach(contour::ContourGuiApp::LaunchAttach {
        .transport = transport,
        .budget = vthost::AttachBudget { 10s },
        .hostLabel = "home",
        .locality = contour::window::DaemonLocality::Remote,
        .address = {},
        .workspace = "work",
    }));
    app.app().startAttachWait();
    REQUIRE(app.app().isStartingUp());

    // The listing goes to the daemon the launch attaches to, and nothing is attached before it answers.
    REQUIRE(app.startupLister().requests.size() == 1);
    CHECK(app.startupLister().requests.front().target.transport == transport);
    CHECK(daemon.onDaemon([&daemon] { return daemon.live.size(); }) == 0);
    app.startupLister().answer(std::vector {
        vtcontrol::WorkspaceInfo { .id = "workspace:018f3c4e-9a1b-7c2d-8e3f-0123456789aa",
                                   .label = {},
                                   .position = 1,
                                   .native = std::to_string(ids[0]) },
        vtcontrol::WorkspaceInfo { .id = "workspace:018f3c4e-9a1b-7c2d-8e3f-0123456789ab",
                                   .label = "work",
                                   .position = 2,
                                   .native = std::to_string(ids[1]) },
    });

    // The attach follows the workspace it named, and the first window opens: the launch waits no more.
    REQUIRE(QTest::qWaitFor([&app] { return !app.app().isStartingUp(); }, 15000));
    CHECK(app.exitCodes().empty());
    REQUIRE(waitUntil([&] { return visibilityOn(daemon, ids[1]) == WorkspaceVisibility::Shown; }));
    CHECK(visibilityOn(daemon, ids[0]) == WorkspaceVisibility::Hidden);
    CHECK(daemon.onDaemon([&daemon] { return daemon.live.size(); }) == 1);
}
