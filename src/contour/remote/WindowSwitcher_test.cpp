// SPDX-License-Identifier: Apache-2.0
#include <contour/config/Actions.hpp>
#include <contour/display/TerminalDisplay.hpp>
#include <contour/platform/Notifier.hpp>
#include <contour/remote/DaemonLink.hpp>
#include <contour/remote/SwitchGrants.hpp>
#include <contour/remote/SwitchGuard.hpp>
#include <contour/remote/WindowSwitcher.hpp>
#include <contour/remote/testing/DaemonFixture.hpp>
#include <contour/session/TerminalSession.hpp>
#include <contour/session/TerminalSessionManager.hpp>
#include <contour/test/AppFixture.hpp>
#include <contour/test/DisplayPixels.hpp>
#include <contour/test/GuiTestFixtures.hpp>
#include <contour/test/PermissionFixtures.hpp>
#include <contour/window/SwitcherController.hpp>
#include <contour/window/WindowController.hpp>

#include <vtbackend/render/RenderBuffer.hpp>
#include <vtbackend/screen/Terminal.hpp>

#include <QtCore/QCoreApplication>
#include <QtCore/QPointer>
#include <QtGui/QImage>
#include <QtQuick/QQuickWindow>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <expected>
#include <format>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <ranges>
#include <set>
#include <span>
#include <string>
#include <string_view>
#include <tuple>
#include <utility>
#include <vector>

#include <vtcontrol/testing/FakeControlApi.hpp>
#include <vthost/Transport.hpp>
#include <vthost/ViewRouting.hpp>
#include <vthost/WorkspaceVisibility.hpp>
#include <vthost/client/ConnectionState.hpp>
#include <vthost/control/ControlService.hpp>
#include <vtworkspace/SessionModel.hpp>

using namespace std::chrono_literals;
using namespace contour::remote::testing;
using contour::remote::DaemonAddress;
using contour::remote::DaemonLink;
using contour::remote::DaemonLinkConfig;
using contour::session::SwitchMode;
using contour::session::SwitchTarget;
using vthost::WorkspaceVisibility;
using vthost::proto::SwitchDaemonKind;
using vthost::proto::SwitchOutcome;

namespace
{

/// Daemon A, which the window starts on, by label.
[[nodiscard]] DaemonAddress daemonA()
{
    return DaemonAddress { .kind = SwitchDaemonKind::Label, .name = "a" };
}
/// Daemon B, which a cross-daemon switch goes to, by label.
[[nodiscard]] DaemonAddress daemonB()
{
    return DaemonAddress { .kind = SwitchDaemonKind::Label, .name = "b" };
}
/// A daemon whose socket nobody serves.
[[nodiscard]] DaemonAddress unreachable()
{
    return DaemonAddress { .kind = SwitchDaemonKind::Label, .name = "down" };
}

/// @return A target on daemon @p daemon, naming @p workspace (empty to decide).
[[nodiscard]] SwitchTarget to(DaemonAddress const& daemon, std::string_view workspace = {})
{
    return SwitchTarget { .kind = daemon.kind,
                          .daemon = daemon.name,
                          .workspace = contour::session::workspaceTargetOf(workspace) };
}

/// What a test link to @p endpoint is built with: launched, and named @p address.
[[nodiscard]] DaemonLinkConfig linkConfig(
    vthost::TcpEndpoint endpoint,
    TestTimeline& timeline,
    DaemonAddress address,
    vthost::proto::AttachIntent intent = { .kind = vthost::proto::AttachIntentKind::Default, .workspace = 0 },
    contour::remote::NativeController::SwitchRequests switchRequests =
        contour::remote::NativeController::SwitchRequests::Answered)
{
    return DaemonLinkConfig {
        .controller =
            controllerConfig(std::move(endpoint),
                             timeline,
                             ControllerOptions { .intent = intent, .switchRequests = switchRequests }),
        .hostLabel = {},
        .locality = contour::window::DaemonLocality::Remote,
        .clientRelease = "test",
        .opening = std::nullopt,
        .address = std::move(address),
        .arrival = std::nullopt,
    };
}

/// Lists a fixture's workspaces as its control API would: native id, position, uuid and visibility, read on
/// the daemon's thread and told on the GUI thread later, as the production lister does.
class FixtureLister final: public contour::remote::IWorkspaceLister
{
  public:
    explicit FixtureLister(std::map<std::string, DaemonFixture*> daemons): _daemons(std::move(daemons)) {}

    void list(contour::remote::ListTarget target, contour::remote::WorkspacesListed done) override
    {
        ++listings;
        auto const found = _daemons.find(target.address.name);
        auto listed = contour::remote::WorkspaceListing {};
        if (declined.contains(target.address.name))
            listed = std::unexpected(
                contour::remote::ListFailure { .kind = contour::remote::ListFailureKind::Declined,
                                               .reason = "the daemon at 127.0.0.1:1 was not trusted" });
        else if (found == _daemons.end())
            listed =
                std::unexpected(contour::remote::ListFailure { .reason = "cannot connect to the daemon" });
        else
            listed = describe(*found->second);
        QMetaObject::invokeMethod(
            &_receiver,
            [done = std::move(done), listed = std::move(listed)]() mutable { done(std::move(listed)); },
            Qt::QueuedConnection);
    }

    /// Creates a workspace labelled @p label on the fixture's daemon, as `workspace.create` does.
    void create(contour::remote::ListTarget target,
                std::string label,
                contour::remote::WorkspaceCreated done) override
    {
        created.push_back(label);
        auto const found = _daemons.find(target.address.name);
        auto made = std::expected<vtcontrol::ObjectRef, std::string> {};
        if (found == _daemons.end())
            made = std::unexpected(std::string { "cannot connect to the daemon" });
        else
            made = found->second->onDaemon([&daemon = *found->second, &label] {
                auto* const workspace = daemon.host->createWorkspace();
                auto const ref = vtcontrol::ObjectRef {
                    .kind = vtcontrol::ObjectKind::Workspace,
                    .uuid = daemon.host->objects().uuidOf(workspace->id()).value_or(vtcontrol::Uuid {})
                };
                daemon.host->objects().setLabel(ref, label);
                return ref;
            });
        QMetaObject::invokeMethod(
            &_receiver,
            [done = std::move(done), made = std::move(made)]() mutable { done(std::move(made)); },
            Qt::QueuedConnection);
    }

    int listings = 0; ///< How often a daemon was listed.
    /// The daemons whose listing the user declines to trust, by label.
    std::set<std::string> declined;
    std::vector<std::string> created; ///< The labels a workspace was created with, in order.

  private:
    [[nodiscard]] static std::vector<vtcontrol::WorkspaceInfo> describe(DaemonFixture& daemon)
    {
        return daemon.onDaemon([&daemon] {
            auto infos = std::vector<vtcontrol::WorkspaceInfo> {};
            auto const& model = daemon.host->model();
            for (auto const index: std::views::iota(0, model.workspaceCount()))
            {
                auto const id = model.workspaceAt(index)->id();
                auto info = vtcontrol::WorkspaceInfo {};
                info.id = vtcontrol::toString(vtcontrol::ObjectRef {
                    .kind = vtcontrol::ObjectKind::Workspace,
                    .uuid = daemon.host->objects().uuidOf(id).value_or(vtcontrol::Uuid {}) });
                info.position = static_cast<uint32_t>(index + 1);
                info.native = std::to_string(id.value);
                info.visibility = std::string { vthost::nameOf(daemon.host->visibility(id.value)) };
                info.tabs.resize(static_cast<std::size_t>(model.workspaceAt(index)->tabCount()));
                infos.push_back(std::move(info));
            }
            return infos;
        });
    }

    std::map<std::string, DaemonFixture*> _daemons;
    QObject _receiver;
};

/// Raises nothing.
class SilentNotifier final: public contour::platform::Notifier
{
  public:
    void notify(vtbackend::DesktopNotification const& /*notification*/) override {}
    void close(std::string const& /*identifier*/) override {}
};

/// The application a switch runs in: it opens windows as a spawned window's Main.qml would, owns every link
/// it starts, and resolves the daemons a test names to their fixtures.
class FakeApp final: public contour::remote::IDaemonLinkHost, public contour::remote::ISwitchHost
{
  public:
    FakeApp(contour::session::TerminalSessionManager& manager,
            std::vector<std::unique_ptr<DaemonLink>>& links,
            TestTimeline& timeline,
            std::map<std::string, vthost::TcpEndpoint> endpoints):
        links(links), _manager(manager), _timeline(timeline), _endpoints(std::move(endpoints))
    {
    }

    FakeApp(FakeApp const&) = delete;
    FakeApp& operator=(FakeApp const&) = delete;
    FakeApp(FakeApp&&) = delete;
    FakeApp& operator=(FakeApp&&) = delete;
    ~FakeApp() override = default;

    // IDaemonLinkHost
    void openWindowFor(QScreen* /*screen*/) override
    {
        auto& window = *opened.emplace_back(std::make_unique<contour::test::ScopedController>(_manager));
        std::ignore = _manager.consumeAttachWindow(window.controller);
    }
    contour::remote::UpgradeStart upgradeDaemon(DaemonLink& /*link*/) override
    {
        return contour::remote::UpgradeStart::Started;
    }
    void restartDaemon(DaemonLink& /*link*/) override {}
    void linkEnded() override
    {
        ++endings;
        // As the application does.
        contour::remote::passOnLaunchResume(switchableLinks());
    }
    void startWindowLink(DaemonLinkConfig config) override
    {
        std::ignore = startSwitchLink(std::move(config));
    }
    [[nodiscard]] contour::remote::ElsewhereNotices elsewhereNotices() override
    {
        return { .policy = {}, .bell = {}, .homeDirectory = {}, .notifier = notifier };
    }
    [[nodiscard]] contour::remote::SwitchGrants& switchGrants() override { return grants; }
    [[nodiscard]] contour::remote::SwitchDeadline const& switchDeadline() override
    {
        return questionDeadline;
    }

    // ISwitchHost
    [[nodiscard]] std::vector<DaemonLink*> switchableLinks() override
    {
        return std::ranges::to<std::vector>(
            links | std::views::transform([](auto const& link) { return link.get(); }));
    }
    [[nodiscard]] std::expected<DaemonLinkConfig, std::string> linkConfigFor(
        DaemonAddress const& daemon, vtworkspace::WorkspaceId /*window*/) override
    {
        if (daemon == unreachable())
            // Nothing listens on port 1.
            return linkConfig(
                vthost::TcpEndpoint { .host = "127.0.0.1", .port = 1, .token = {}, .caPem = {} },
                _timeline,
                daemon);
        auto const found = _endpoints.find(daemon.name);
        if (found == _endpoints.end())
            return std::unexpected(std::string { "no such daemon" });
        return linkConfig(found->second, _timeline, daemon);
    }
    DaemonLink& startSwitchLink(DaemonLinkConfig config) override
    {
        ++started;
        auto& link = *links.emplace_back(
            std::make_unique<DaemonLink>(DaemonLink::Id { _nextId++ }, std::move(config), *this, _manager));
        contour::remote::passOnLaunchResume(switchableLinks());
        return link;
    }

    std::vector<std::unique_ptr<contour::test::ScopedController>> opened; ///< Every window opened, in order.
    std::vector<std::unique_ptr<DaemonLink>>& links;                      ///< Every link, the launch's first.
    int endings = 0;                                                      ///< How often a link said it ended.
    int started = 0;                      ///< How many links a switch started.
    contour::remote::SwitchGrants grants; ///< The answers that stand for the switches programs ask for.
    /// Where the finishes of other workspaces would be raised; none is in these tests.
    SilentNotifier notifier;
    /// What every switch's time is read from; moves only when a test says so.
    core::platform::ManualClock clock { core::platform::SteadyTimePoint {} + std::chrono::hours { 1 } };
    /// What expires the switch questions; never fires unless a test says otherwise.
    contour::remote::SwitchDeadline questionDeadline {
        .timeout = contour::remote::DefaultSwitchTimeout,
        .timer = [](std::chrono::milliseconds /*delay*/, std::function<void()> const& /*fire*/) {},
        .clock = clock,
    };

  private:
    contour::session::TerminalSessionManager& _manager;
    TestTimeline& _timeline;
    std::map<std::string, vthost::TcpEndpoint> _endpoints;
    uint32_t _nextId = 1;
};

/// A switch timer a test fires by hand: what is armed waits until fireAll().
struct ManualSwitchTimer
{
    struct Armed
    {
        std::chrono::milliseconds delay;
        std::function<void()> fire;
    };
    std::vector<Armed> armed; ///< What is armed and not fired, in arming order.

    /// @param clock What the switch's time is read from.
    [[nodiscard]] contour::remote::SwitchDeadline deadline(core::platform::IClock const& clock)
    {
        return contour::remote::SwitchDeadline {
            .timeout = contour::remote::DefaultSwitchTimeout,
            .timer = [this](std::chrono::milliseconds delay,
                            std::function<void()> fire) { armed.push_back({ delay, std::move(fire) }); },
            .clock = clock,
        };
    }

    /// Fires everything armed, as the time being up would.
    void fireAll()
    {
        for (auto& each: std::exchange(armed, {}))
            each.fire();
    }
};

/// What a switch request was told.
struct Told
{
    std::optional<std::expected<SwitchOutcome, std::string>> outcome;

    [[nodiscard]] contour::session::SwitchCompletion completion()
    {
        return [this](std::expected<SwitchOutcome, std::string> result) {
            outcome = std::move(result);
        };
    }
};

/// A workspace no connection selects is hidden: the daemon keeps nothing more to remember, so there is
/// nothing to do.
void hide(DaemonFixture& /*daemon*/, uint64_t /*workspace*/)
{
}

/// @return Whether @p workspace is shown or hidden on the daemon.
[[nodiscard]] WorkspaceVisibility visibilityOn(DaemonFixture& daemon, uint64_t workspace)
{
    return daemon.onDaemon([&daemon, workspace] { return daemon.host->visibility(workspace); });
}

/// @return How many connections of the daemon show @p workspace.
[[nodiscard]] std::size_t showing(DaemonFixture& daemon, uint64_t workspace)
{
    return daemon.onDaemon([&daemon, workspace] { return daemon.host->clientsShowing(workspace).size(); });
}

/// @return How many connections the daemon has.
[[nodiscard]] std::size_t connections(DaemonFixture& daemon)
{
    return daemon.onDaemon([&daemon] { return daemon.live.size(); });
}

/// How daemon B of a SwitchRig starts.
enum class DaemonBStart : uint8_t
{
    Seeded, ///< With two workspaces of one tab each.
    Fresh,  ///< As one no client attached to yet: its own workspace, holding no tab.
};

/// Two daemons of distinct runs, A of two workspaces (the second hidden, so a launch brings back only the
/// first) and B of two (or of none but its own empty one), a window on A's first, and the switcher.
struct SwitchRig
{
    DaemonFixture a { {}, {}, 3 };
    DaemonFixture b { {}, {}, 5 };
    std::vector<uint64_t> aWorkspaces = seedWorkspaces(a, 2);
    std::vector<uint64_t> bWorkspaces; ///< None for a fresh B.
    TestTimeline timeline;
    /// Declared before the app: the panes the manager holds unregister from the links' controllers as they
    /// go.
    std::vector<std::unique_ptr<DaemonLink>> links;
    contour::test::TestApp app { std::make_unique<contour::test::MockPtySessionFactory>() };
    FakeApp host { app.manager(), links, timeline, { { "a", a.endpoint() }, { "b", b.endpoint() } } };
    FixtureLister* lister = nullptr;                       ///< The switcher's.
    std::vector<contour::remote::SwitchPickRequest> picks; ///< What the switcher popup was asked to show.
    ManualSwitchTimer timer;                               ///< The switcher's deadlines.
    std::unique_ptr<contour::remote::WindowSwitcher> switcher;
    std::unique_ptr<contour::test::ScopedController> window;

    SwitchRig(): SwitchRig(contour::remote::NativeController::SwitchRequests::Answered, DaemonBStart::Seeded)
    {
    }

    /// @param switchRequests Whether the window's client answers the daemon's switch requests.
    explicit SwitchRig(contour::remote::NativeController::SwitchRequests switchRequests):
        SwitchRig(switchRequests, DaemonBStart::Seeded)
    {
    }

    /// @param bStart How daemon B starts.
    explicit SwitchRig(DaemonBStart bStart):
        SwitchRig(contour::remote::NativeController::SwitchRequests::Answered, bStart)
    {
    }

    /// @param switchRequests Whether the window's client answers the daemon's switch requests.
    /// @param bStart How daemon B starts.
    SwitchRig(contour::remote::NativeController::SwitchRequests switchRequests, DaemonBStart bStart):
        bWorkspaces(bStart == DaemonBStart::Seeded ? seedWorkspaces(b, 2) : std::vector<uint64_t> {})
    {
        hide(a, aWorkspaces[1]);
        auto made = std::make_unique<FixtureLister>(
            std::map<std::string, DaemonFixture*> { { "a", &a }, { "b", &b }, { "down", &b } });
        lister = made.get();
        switcher = std::make_unique<contour::remote::WindowSwitcher>(
            app.manager(),
            host,
            std::move(made),
            [this](contour::remote::SwitchPickRequest pick) {
                picks.push_back(std::move(pick));
                return std::expected<void, std::string> {};
            },
            timer.deadline(host.clock));
        // A switch a program asks for reaches the manager's switching, which hands it to this switcher.
        app.switching().forwardTo = switcher.get();
        links.push_back(std::make_unique<DaemonLink>(
            DaemonLink::Id { 1000 },
            linkConfig(a.endpoint(),
                       timeline,
                       daemonA(),
                       { .kind = vthost::proto::AttachIntentKind::Default, .workspace = 0 },
                       switchRequests),
            host,
            app.manager()));
        window = std::make_unique<contour::test::ScopedController>(app.manager());
        REQUIRE(pollUntilAttached(launchLink(), 10s).has_value());
        REQUIRE(launchLink().bindPendingWindow(*window->controller)
                == contour::remote::WindowAdoption::Adopted);
        REQUIRE(waitUntil([&] { return launchLink().workspaceOf(shown()) == aWorkspaces[0]; }));
        REQUIRE(waitUntil([&] { return !app.manager().sessionsOfWindow(shown()).empty(); }));
        REQUIRE(visibilityOn(a, aWorkspaces[0]) == WorkspaceVisibility::Shown);
    }

    SwitchRig(SwitchRig const&) = delete;
    SwitchRig& operator=(SwitchRig const&) = delete;
    SwitchRig(SwitchRig&&) = delete;
    SwitchRig& operator=(SwitchRig&&) = delete;

    ~SwitchRig()
    {
        app.switching().forwardTo = nullptr;
        for (auto const& link: links)
            link->stop();
        host.opened.clear();
        window.reset();
        drainGuiEvents();
    }

    [[nodiscard]] DaemonLink& launchLink() const { return *links.front(); }

    /// @return The local workspace the window displays now: a switch within the daemon realizes the new one
    ///         in a local workspace of its own.
    [[nodiscard]] vtworkspace::WorkspaceId shown() const { return window->controller->displayedWorkspace(); }

    /// @return The link the window is bound to now; null for none.
    [[nodiscard]] DaemonLink* windowLink()
    {
        auto const* const binding = app.manager().bindingOf(shown());
        auto const found = std::ranges::find_if(links, [binding](auto const& link) {
            return static_cast<contour::session::WindowBinding const*>(link.get()) == binding;
        });
        return found != links.end() ? found->get() : nullptr;
    }

    /// Asks the switcher, and waits for it to tell.
    [[nodiscard]] std::expected<SwitchOutcome, std::string> request(
        SwitchTarget target, SwitchMode mode = SwitchMode::Switch) const
    {
        auto told = Told {};
        switcher->request(
            shown(), std::move(target), mode, contour::session::NoLandingDeadline, told.completion());
        REQUIRE(waitUntil([&] { return told.outcome.has_value(); }, 2000));
        return *told.outcome;
    }

    /// @return The window's sessions, watched for their destruction.
    [[nodiscard]] std::vector<QPointer<contour::session::TerminalSession>> windowSessions()
    {
        auto watched = std::vector<QPointer<contour::session::TerminalSession>> {};
        for (auto* const session: app.manager().sessionsOfWindow(shown()))
            watched.emplace_back(session);
        return watched;
    }

    /// @return Whether none of @p before is among the window's sessions now.
    [[nodiscard]] bool showsNoneOf(std::span<QPointer<contour::session::TerminalSession> const> before)
    {
        auto const now = app.manager().sessionsOfWindow(shown());
        return std::ranges::none_of(
            before, [&](auto const& old) { return old && std::ranges::contains(now, old.data()); });
    }
};

/// What a `workspace.switch` call answered, once it has.
using ControlAnswer = std::optional<std::expected<vtcontrol::SwitchWorkspaceReply, vtcontrol::ControlError>>;

/// A daemon's control API, as `contour switch` reaches it: the service runs on the daemon's thread, and a
/// call is made as from a pane of the daemon.
class ControlApi
{
  public:
    explicit ControlApi(DaemonFixture& daemon): _daemon(daemon)
    {
        _daemon.onDaemon([this] {
            _service = std::make_unique<vthost::control::ControlService>(
                *_daemon.host,
                vthost::control::ControlServiceConfig {
                    .version = "test", .socketPath = "/run/test.sock", .clock = _wall });
            return 0;
        });
    }
    ~ControlApi()
    {
        _daemon.onDaemon([this] {
            _service.reset();
            return 0;
        });
    }
    ControlApi(ControlApi const&) = delete;
    ControlApi& operator=(ControlApi const&) = delete;
    ControlApi(ControlApi&&) = delete;
    ControlApi& operator=(ControlApi&&) = delete;

    /// Calls `workspace.switch` from the active pane of the daemon's @p workspace, as `contour switch
    /// --to-label …` run in it, with no workspace named: the caller's own.
    void callFromPaneOf(uint64_t workspace, vtcontrol::SwitchWorkspaceRequest request)
    {
        _daemon.onDaemon([this, workspace, request = std::move(request)] {
            auto const pane = _daemon.host->model()
                                  .workspace(vtworkspace::WorkspaceId { workspace })
                                  ->activeTab()
                                  ->rootPane()
                                  ->session();
            _context.callerPane = vtcontrol::toString(vtcontrol::ObjectRef {
                .kind = vtcontrol::ObjectKind::Pane, .uuid = *_daemon.host->objects().uuidOf(pane) });
            _answer.reset();
            _service->handle(_context, request, [this](auto reply) { _answer = std::move(reply); });
            return 0;
        });
    }

    /// @return What the call answered so far.
    [[nodiscard]] ControlAnswer answer()
    {
        return _daemon.onDaemon([this] { return _answer; });
    }

  private:
    DaemonFixture& _daemon;
    core::platform::ManualWallClock _wall { std::chrono::system_clock::time_point {} };
    std::unique_ptr<vthost::control::ControlService> _service; ///< Daemon thread only.
    vtcontrol::testing::FakeCallContext _context;              ///< Daemon thread only.
    ControlAnswer _answer;                                     ///< Daemon thread only.
};

/// @return The control call's answer once it came while the GUI thread runs; fails the case otherwise.
[[nodiscard]] vtcontrol::SwitchWorkspaceReply awaitAnswer(ControlApi& api)
{
    REQUIRE(waitUntil([&] { return api.answer().has_value(); }, 2000));
    auto answer = *api.answer();
    REQUIRE(answer.has_value());
    return *answer;
}

/// @return The session the window asks a switch's question in, once it does; fails the case otherwise.
[[nodiscard]] contour::session::TerminalSession& awaitSwitchQuestion(SwitchRig& rig)
{
    auto const asked = [&] {
        auto* const session = rig.app.manager().actingSessionOfWindow(rig.shown());
        return session != nullptr
               && session->pendingPermissionAsk()
                      == contour::session::PermissionAsk { contour::session::GuardedRole::SwitchWindow };
    };
    REQUIRE(waitUntil(asked, 2000));
    return *rig.app.manager().actingSessionOfWindow(rig.shown());
}

/// @return The first line of the question @p session shows.
[[nodiscard]] QString questionLine(contour::session::TerminalSession const& session)
{
    return session.pendingPermission()
        .value(QStringLiteral("lines"))
        .toList()
        .value(0)
        .toMap()
        .value(QStringLiteral("text"))
        .toString();
}

/// Has the daemon's pane of @p workspace print @p text, as a program in it would.
void printInPaneOf(DaemonFixture& daemon, uint64_t workspace, std::string text)
{
    daemon.onDaemon([&daemon, workspace, text = std::move(text)] {
        auto const pane =
            daemon.host->model().workspace(vtworkspace::WorkspaceId { workspace })->activeTab()->rootPane();
        daemon.host->terminal(pane->session())->writeToScreen(text);
        return 0;
    });
}

/// @return Whether @p buffer, the grid the display drew the frame from, holds @p text.
[[nodiscard]] bool drawsText(vtbackend::RenderBuffer const& buffer, std::string_view text)
{
    auto cells = std::vector<vtbackend::RenderCell const*> {};
    for (auto const& cell: buffer.cells)
        cells.push_back(&cell);
    std::ranges::sort(cells, [](auto const* a, auto const* b) {
        return std::tie(a->position.line, a->position.column)
               < std::tie(b->position.line, b->position.column);
    });
    auto drawn = std::u32string {};
    for (auto const* cell: cells)
        drawn += cell->codepoints;
    auto const wanted = std::u32string(text.begin(), text.end());
    return drawn.contains(wanted);
}

} // namespace

TEST_CASE("WindowSwitcher: a switch to another daemon shows its workspace, and hides the old one",
          "[switch][link]")
{
    auto rig = SwitchRig {};
    auto const before = rig.windowSessions();
    auto const profile = before.front()->profileName();
    auto const* const oldLink = &rig.launchLink();

    auto const outcome = rig.request(to(daemonB(), "2"));

    REQUIRE(outcome == SwitchOutcome::Switched);
    auto* const now = rig.windowLink();
    REQUIRE(now != nullptr);
    CHECK(now != oldLink);
    CHECK(now->address() == daemonB());
    CHECK(now->workspaceOf(rig.shown()) == rig.bWorkspaces[1]);
    CHECK(rig.b.waitUntilOnDaemon([&] {
        return rig.b.host->visibility(vtworkspace::WorkspaceId { rig.bWorkspaces[1] })
               == WorkspaceVisibility::Shown;
    }));
    // Detached, not killed: A's workspace stays in its daemon as hidden.
    CHECK(rig.a.waitUntilOnDaemon([&] {
        return rig.a.host->visibility(vtworkspace::WorkspaceId { rig.aWorkspaces[0] })
               == WorkspaceVisibility::Hidden;
    }));
    CHECK(rig.a.onDaemon([&] {
        return rig.a.host->model().workspace(vtworkspace::WorkspaceId { rig.aWorkspaces[0] }) != nullptr;
    }));
    // The window shows B's panes in place of A's, under its own render profile.
    REQUIRE(waitUntil([&] { return !rig.app.manager().sessionsOfWindow(rig.shown()).empty(); }));
    CHECK(rig.showsNoneOf(before));
    for (auto* const session: rig.app.manager().sessionsOfWindow(rig.shown()))
        CHECK(session->profileName() == profile);
    // Rebinding the old link's last window ended it.
    CHECK(waitUntil([&] { return rig.host.endings > 0; }));
}

// A daemon no client attached to yet holds one workspace, its own, and no tab. The switch followed that
// workspace, handed the window its empty layout and detached the window's panes: the last pane's end found
// the window empty, TerminalPane closed it, and with the last window gone the application quit.
TEST_CASE("WindowSwitcher: a switch to a daemon no client attached to yet keeps the window, in a new "
          "workspace there",
          "[switch][link]")
{
    auto rig = SwitchRig { DaemonBStart::Fresh };
    auto const before = rig.windowSessions();
    auto const bOwn = rig.b.onDaemon([&] { return rig.b.host->workspaceId().value; });
    // Each pane's display, as the window's QML has one: TerminalPane.qml answers its end by closing the
    // window when the window's controller says no pane of it is left.
    auto displays = std::vector<std::unique_ptr<contour::display::TerminalDisplay>> {};
    auto closesWindow = false;
    for (auto const& session: before)
    {
        auto& display = *displays.emplace_back(std::make_unique<contour::display::TerminalDisplay>());
        display.setSession(session.data());
        QObject::connect(&display, &contour::display::TerminalDisplay::terminated, [&] {
            closesWindow = closesWindow || rig.window->controller->canCloseWindow();
        });
    }

    REQUIRE(rig.request(to(daemonB())) == SwitchOutcome::Switched);

    CHECK_FALSE(closesWindow);
    auto* const now = rig.windowLink();
    REQUIRE(now != nullptr);
    CHECK(now->address() == daemonB());
    auto const shown = now->workspaceOf(rig.shown());
    REQUIRE(shown.has_value());
    CHECK(*shown != bOwn);
    REQUIRE(waitUntil([&] { return !rig.app.manager().sessionsOfWindow(rig.shown()).empty(); }));
    CHECK(rig.showsNoneOf(before));
    CHECK_FALSE(rig.window->controller->canCloseWindow());
    // Detached, not killed: A's workspace stays in its daemon as hidden.
    CHECK(rig.a.waitUntilOnDaemon([&] {
        return rig.a.host->visibility(vtworkspace::WorkspaceId { rig.aWorkspaces[0] })
               == WorkspaceVisibility::Hidden;
    }));
}

// The window is handed the workspace a link a switch started shows only once it holds a tab: until then the
// window would be emptied, and close.
TEST_CASE("WindowSwitcher: a link a switch started lands once its workspace holds a tab", "[switch][link]")
{
    auto rig = SwitchRig { DaemonBStart::Fresh };
    auto const bOwn = rig.b.onDaemon([&] { return rig.b.host->workspaceId().value; });
    auto landed = std::optional<std::expected<uint64_t, std::string>> {};
    auto config = linkConfig(rig.b.endpoint(),
                             rig.timeline,
                             daemonB(),
                             { .kind = vthost::proto::AttachIntentKind::Select, .workspace = bOwn });
    config.arrival = contour::remote::SwitchArrival {
        .mode = SwitchMode::Switch,
        .landed = [&](std::expected<uint64_t, std::string> outcome) { landed = std::move(outcome); },
    };
    auto& link = rig.host.startSwitchLink(std::move(config));

    // Its layout is here, holding no tab: nothing to show, so nothing is told.
    REQUIRE(waitUntil([&] {
        auto const adoptable = link.canAdoptWindow(rig.shown());
        return !adoptable && adoptable.error() == "the daemon's workspace holds no tab";
    }));
    drainGuiEvents();
    CHECK_FALSE(landed.has_value());

    std::ignore = rig.b.seedSession("first");

    REQUIRE(waitUntil([&] { return landed.has_value(); }));
    CHECK(*landed == bOwn);
    CHECK(link.canAdoptWindow(rig.shown()).has_value());
}

// Review (PR 3, item 15): a switch a program asked for is answered on the old link's connection, which the
// rebinding ends; the answer only got out because it was queued on that link's reactor ahead of the stop.
TEST_CASE(
    "WindowSwitcher: a switch to another daemon is told it switched while the old link still serves the "
    "window",
    "[switch][link]")
{
    auto rig = SwitchRig {};
    auto& oldLink = rig.launchLink();
    auto toldWhileOld = std::optional<bool> {};
    auto told = Told {};
    auto completion = told.completion();
    rig.switcher->request(rig.shown(),
                          to(daemonB(), "2"),
                          SwitchMode::Switch,
                          contour::session::NoLandingDeadline,
                          [&](std::expected<SwitchOutcome, std::string> outcome) {
                              toldWhileOld = rig.windowLink() == &oldLink && oldLink.isLive();
                              completion(std::move(outcome));
                          });
    REQUIRE(waitUntil([&] { return told.outcome.has_value(); }, 2000));
    REQUIRE(*told.outcome == SwitchOutcome::Switched);
    CHECK(toldWhileOld == true);
    // And the window is rebound all the same.
    REQUIRE(rig.windowLink() != nullptr);
    CHECK(rig.windowLink() != &oldLink);
    CHECK(rig.windowLink()->workspaceOf(rig.shown()) == rig.bWorkspaces[1]);
}

TEST_CASE("WindowSwitcher: a switch within the daemon reuses the window's connection", "[switch][link]")
{
    auto rig = SwitchRig {};
    auto const before = rig.windowSessions();
    auto const connectionsBefore = connections(rig.a);

    auto const outcome = rig.request(to(daemonA(), "2"));

    REQUIRE(outcome == SwitchOutcome::Switched);
    CHECK(rig.windowLink() == &rig.launchLink());
    CHECK(rig.launchLink().workspaceOf(rig.shown()) == rig.aWorkspaces[1]);
    CHECK(rig.host.started == 0);
    CHECK(connections(rig.a) == connectionsBefore);
    CHECK(rig.a.waitUntilOnDaemon([&] {
        return rig.a.host->visibility(vtworkspace::WorkspaceId { rig.aWorkspaces[0] })
               == WorkspaceVisibility::Hidden;
    }));
    CHECK(visibilityOn(rig.a, rig.aWorkspaces[1]) == WorkspaceVisibility::Shown);
    REQUIRE(waitUntil(
        [&] { return !rig.app.manager().sessionsOfWindow(rig.shown()).empty() && rig.showsNoneOf(before); }));
    // The daemon's own name for itself is the window's daemon too.
    CHECK(rig.request(SwitchTarget { .kind = SwitchDaemonKind::Same,
                                     .daemon = {},
                                     .workspace = contour::session::WorkspaceAt { .position = 2 } })
          == SwitchOutcome::FocusedExisting);
}

TEST_CASE("WindowSwitcher: a same-daemon switch naming a control id resolves from the directory without a "
          "listing",
          "[switch][link]")
{
    auto rig = SwitchRig {};
    auto const target = rig.a.onDaemon([&rig] {
        return vtcontrol::toString(vtcontrol::ObjectRef {
            .kind = vtcontrol::ObjectKind::Workspace,
            .uuid = *rig.a.host->objects().uuidOf(vtworkspace::WorkspaceId { rig.aWorkspaces[1] }) });
    });
    // The directory carries the control ids once the daemon stated them.
    auto* const controller = rig.launchLink().liveController();
    REQUIRE(controller != nullptr);
    REQUIRE(waitUntil([&] {
        auto const directory = controller->directory();
        return std::ranges::all_of(directory.workspaces, [](auto const& workspace) {
            return workspace.controlId != std::array<uint8_t, 16> {};
        });
    }));

    auto const outcome = rig.request(to(daemonA(), target));

    REQUIRE(outcome == SwitchOutcome::Switched);
    CHECK(rig.lister->listings == 0);
    CHECK(rig.windowLink() == &rig.launchLink());
    CHECK(rig.launchLink().workspaceOf(rig.shown()) == rig.aWorkspaces[1]);
}

TEST_CASE("WindowSwitcher: a same-daemon switch naming a control id the directory lacks asks the listing",
          "[switch][link]")
{
    auto rig = SwitchRig {};
    auto* const controller = rig.launchLink().liveController();
    REQUIRE(controller != nullptr);
    REQUIRE(waitUntil([&] { return controller->directory().workspaces.size() == rig.aWorkspaces.size(); }));
    // An id no workspace the window knows of carries: the daemon's listing is the one to say.
    auto absent = vtcontrol::Uuid {};
    absent.bytes.fill(0xEE);

    auto const outcome = rig.request(to(daemonA(),
                                        vtcontrol::toString(vtcontrol::ObjectRef {
                                            .kind = vtcontrol::ObjectKind::Workspace, .uuid = absent })));

    CHECK(rig.lister->listings == 1);
    CHECK(outcome != SwitchOutcome::Switched);
    CHECK(rig.launchLink().workspaceOf(rig.shown()) == rig.aWorkspaces[0]);
}

TEST_CASE("WindowSwitcher: SwitchToWorkspace selects another workspace in the same window", "[switch][link]")
{
    auto rig = SwitchRig {};
    auto const before = rig.windowSessions();
    auto const window = rig.window->windowId;
    auto* const acting = rig.app.manager().actingSessionOfWindow(rig.shown());
    REQUIRE(acting != nullptr);

    // As the action does: by position, on the window's own daemon.
    REQUIRE(acting->operator()(contour::actions::SwitchToWorkspace { .index = 2 }));

    // The same window, the same link and connection, nothing listed: the window displays the second one.
    REQUIRE(waitUntil([&] { return rig.launchLink().workspaceOf(rig.shown()) == rig.aWorkspaces[1]; }));
    REQUIRE(waitUntil([&] { return !rig.app.manager().sessionsOfWindow(rig.shown()).empty(); }));
    CHECK(rig.window->controller->windowId() == window);
    CHECK(rig.windowLink() == &rig.launchLink());
    CHECK(rig.lister->listings == 0);
    CHECK(rig.host.started == 0);
    CHECK(connections(rig.a) == 1);
    CHECK(waitUntil([&] { return rig.showsNoneOf(before); }));
    CHECK(rig.window->controller->ownedWorkspaces().size() == 1);
}

TEST_CASE("WindowSwitcher: an unreachable daemon leaves the window as it was", "[switch][link]")
{
    auto rig = SwitchRig {};
    auto const before = rig.windowSessions();

    SECTION("the daemon cannot be listed")
    {
        auto const outcome =
            rig.request(to(DaemonAddress { .kind = SwitchDaemonKind::Host, .name = "nowhere" }));
        REQUIRE_FALSE(outcome.has_value());
        CHECK(outcome.error() == "no such daemon");
        CHECK(rig.window->controller->notices().toastText().contains("Could not switch"));
    }
    SECTION("the daemon is listed, but its link does not connect")
    {
        // The listing comes from B; the connect goes where nothing listens.
        auto const outcome = rig.request(to(unreachable(), "1"));
        REQUIRE_FALSE(outcome.has_value());
        CHECK(rig.host.started == 1);
        CHECK(rig.window->controller->notices().toastText().contains("Could not switch"));
    }
    SECTION("the user declines to trust the daemon: said without alarm, nothing is started")
    {
        rig.lister->declined.insert("b");
        auto const outcome = rig.request(to(daemonB(), "1"));
        REQUIRE_FALSE(outcome.has_value());
        CHECK(outcome.error() == "the daemon at 127.0.0.1:1 was not trusted");
        CHECK(rig.host.started == 0);
        auto const toast = rig.window->controller->notices().toastText();
        CHECK(toast.contains("Did not switch"));
        CHECK_FALSE(toast.contains("Could not switch"));
    }

    CHECK(rig.windowLink() == &rig.launchLink());
    CHECK(rig.launchLink().workspaceOf(rig.shown()) == rig.aWorkspaces[0]);
    CHECK(visibilityOn(rig.a, rig.aWorkspaces[0]) == WorkspaceVisibility::Shown);
    CHECK(rig.app.manager().sessionsOfWindow(rig.shown()).size() == before.size());
    CHECK_FALSE(rig.showsNoneOf(before));
}

TEST_CASE("WindowSwitcher: a daemon lost after the connect and before the first frame changes nothing",
          "[switch][link]")
{
    auto rig = SwitchRig {};
    auto const before = rig.windowSessions();
    // B's target was switched away from before: a switch that never showed it leaves it so.
    hide(rig.b, rig.bWorkspaces[1]);
    rig.b.holdNewConnectionsAtLayout();
    auto told = Told {};
    rig.switcher->request(rig.shown(),
                          to(daemonB(), "2"),
                          SwitchMode::Switch,
                          contour::session::NoLandingDeadline,
                          told.completion());
    // The new link's connection selects B's workspace, and is held before its first layout.
    REQUIRE(waitUntil([&] { return showing(rig.b, rig.bWorkspaces[1]) == 1; }));
    CHECK_FALSE(told.outcome.has_value());

    rig.b.refuseConnections();

    REQUIRE(waitUntil([&] { return told.outcome.has_value(); }, 2000));
    CHECK_FALSE(told.outcome->has_value());
    CHECK(rig.windowLink() == &rig.launchLink());
    CHECK(rig.launchLink().workspaceOf(rig.shown()) == rig.aWorkspaces[0]);
    CHECK(visibilityOn(rig.a, rig.aWorkspaces[0]) == WorkspaceVisibility::Shown);
    CHECK_FALSE(rig.showsNoneOf(before));
    // The link it started gave up: served again, B's workspace is selected by no one.
    rig.b.serveAgain();
    CHECK_FALSE(waitUntil([&] { return showing(rig.b, rig.bWorkspaces[1]) > 0; }, 100));
    // The connection went before it confirmed the layout: B gives the workspace back its record.
    CHECK(visibilityOn(rig.b, rig.bWorkspaces[1]) == WorkspaceVisibility::Hidden);
}

TEST_CASE(
    "WindowSwitcher: a switch within the daemon lost before the first frame leaves the window as it was",
    "[switch][link]")
{
    auto rig = SwitchRig {};
    auto const before = rig.windowSessions();
    // The window's connection is up; the next layout it is sent -- the new workspace's -- is held, and with
    // it the batch end stating the selection.
    rig.a.onDaemon([&rig] {
        for (auto* const socket: rig.a.gated)
            socket->holdAtNextLayout();
        return 0;
    });
    auto told = Told {};
    rig.switcher->request(rig.shown(),
                          to(daemonA(), std::string { contour::remote::NewWorkspaceSpec }),
                          SwitchMode::Switch,
                          contour::session::NoLandingDeadline,
                          told.completion());
    REQUIRE(waitUntil(
        [&] { return rig.a.onDaemon([&rig] { return rig.a.host->model().workspaceCount(); }) == 3; }));
    auto const made = rig.a.onDaemon([&rig] { return rig.a.host->model().workspaceAt(2)->id().value; });
    CHECK_FALSE(told.outcome.has_value());

    rig.a.dropConnections();

    REQUIRE(waitUntil([&] { return told.outcome.has_value(); }, 2000));
    CHECK_FALSE(told.outcome->has_value());
    // The reconnect resumes what the window shows, not the workspace it was switching to.
    REQUIRE(waitUntil([&] { return showing(rig.a, rig.aWorkspaces[0]) == 1; }, 2000));
    CHECK(waitUntil([&] { return visibilityOn(rig.a, made) == WorkspaceVisibility::Hidden; }, 2000));
    drainGuiEvents();
    CHECK(rig.launchLink().workspaceOf(rig.shown()) == rig.aWorkspaces[0]);
    CHECK(rig.windowLink() == &rig.launchLink());
    CHECK_FALSE(rig.showsNoneOf(before));
}

TEST_CASE("WindowSwitcher: a switch within the daemon whose target never shows times out and selects back",
          "[switch][link][timeout]")
{
    auto rig = SwitchRig {};
    auto const before = rig.windowSessions();
    // The window's connection is up; the next layout it is sent -- the new workspace's -- is held.
    rig.a.onDaemon([&rig] {
        for (auto* const socket: rig.a.gated)
            socket->holdAtNextLayout();
        return 0;
    });
    auto told = Told {};
    rig.switcher->request(rig.shown(),
                          to(daemonA(), std::string { contour::remote::NewWorkspaceSpec }),
                          SwitchMode::Switch,
                          contour::session::NoLandingDeadline,
                          told.completion());
    REQUIRE(waitUntil(
        [&] { return rig.a.onDaemon([&rig] { return rig.a.host->model().workspaceCount(); }) == 3; }));
    REQUIRE(rig.timer.armed.size() == 1);
    CHECK(rig.timer.armed.front().delay == contour::remote::DefaultSwitchTimeout);
    CHECK_FALSE(told.outcome.has_value());

    rig.timer.fireAll();

    REQUIRE(told.outcome.has_value());
    REQUIRE_FALSE(told.outcome->has_value());
    CHECK(told.outcome->error().contains("in time"));
    CHECK(rig.window->controller->notices().toastText().contains("Could not switch"));
    // The window's own workspace is selected again, and the layouts held arrive to no switch.
    rig.a.releaseHeld();
    CHECK(waitUntil([&] { return showing(rig.a, rig.aWorkspaces[0]) == 1; }, 2000));
    drainGuiEvents();
    CHECK_FALSE(
        waitUntil([&] { return rig.launchLink().workspaceOf(rig.shown()) != rig.aWorkspaces[0]; }, 50));
    CHECK(visibilityOn(rig.a, rig.aWorkspaces[0]) == WorkspaceVisibility::Shown);
    CHECK_FALSE(rig.showsNoneOf(before));
    // The window is free for the next switch, which lands.
    CHECK(rig.request(to(daemonA(), "2")) == SwitchOutcome::Switched);
    CHECK(rig.launchLink().workspaceOf(rig.shown()) == rig.aWorkspaces[1]);
}

TEST_CASE("WindowSwitcher: a timed-out switch to a new workspace hides the one the daemon made late",
          "[switch][link][timeout]")
{
    auto rig = SwitchRig {};
    rig.a.onDaemon([&rig] {
        for (auto* const socket: rig.a.gated)
            socket->holdAtNextLayout();
        return 0;
    });
    auto told = Told {};
    rig.switcher->request(rig.shown(),
                          to(daemonA(), std::string { contour::remote::NewWorkspaceSpec }),
                          SwitchMode::Switch,
                          contour::session::NoLandingDeadline,
                          told.completion());
    // The daemon makes the workspace; its layout is held on the way to the window's connection.
    REQUIRE(waitUntil(
        [&] { return rig.a.onDaemon([&rig] { return rig.a.host->model().workspaceCount(); }) == 3; }));
    auto const made = rig.a.onDaemon([&rig] { return rig.a.host->model().workspaceAt(2)->id().value; });
    REQUIRE(rig.timer.armed.size() == 1);

    rig.timer.fireAll();
    REQUIRE(told.outcome.has_value());
    REQUIRE_FALSE(told.outcome->has_value());

    // The layout arrives after the switch was given up: the workspace is let go of as hidden, and no window
    // opens for it.
    rig.a.releaseHeld();
    CHECK(waitUntil([&] { return visibilityOn(rig.a, made) == WorkspaceVisibility::Hidden; }, 2000));
    CHECK(showing(rig.a, made) == 0);
    drainGuiEvents();
    CHECK(rig.host.opened.empty());
    CHECK(rig.host.started == 0);
    CHECK(rig.launchLink().workspaceOf(rig.shown()) == rig.aWorkspaces[0]);
    CHECK_FALSE(rig.launchLink().windowShowing(made).has_value());
}

TEST_CASE("WindowSwitcher: a switch to another daemon whose target never shows times out and drops its link",
          "[switch][link][timeout]")
{
    auto rig = SwitchRig {};
    auto const before = rig.windowSessions();
    // B's target was switched away from before: the abandoned switch leaves it so.
    hide(rig.b, rig.bWorkspaces[1]);
    rig.b.holdNewConnectionsAtLayout();
    auto told = Told {};
    rig.switcher->request(rig.shown(),
                          to(daemonB(), "2"),
                          SwitchMode::Switch,
                          contour::session::NoLandingDeadline,
                          told.completion());
    // The new link's connection selects B's workspace, and is held before its first layout.
    REQUIRE(waitUntil([&] { return showing(rig.b, rig.bWorkspaces[1]) == 1; }));
    REQUIRE(rig.timer.armed.size() == 1);
    CHECK_FALSE(told.outcome.has_value());

    rig.timer.fireAll();

    REQUIRE(told.outcome.has_value());
    REQUIRE_FALSE(told.outcome->has_value());
    CHECK(told.outcome->error().contains("in time"));
    // The link it started is gone, and with it B's selection; the window never left A.
    CHECK(waitUntil([&] { return showing(rig.b, rig.bWorkspaces[1]) == 0; }, 2000));
    rig.b.releaseHeld();
    drainGuiEvents();
    CHECK(rig.windowLink() == &rig.launchLink());
    CHECK(rig.launchLink().workspaceOf(rig.shown()) == rig.aWorkspaces[0]);
    CHECK(visibilityOn(rig.a, rig.aWorkspaces[0]) == WorkspaceVisibility::Shown);
    CHECK(rig.b.waitUntilOnDaemon([&] {
        return rig.b.host->visibility(vtworkspace::WorkspaceId { rig.bWorkspaces[1] })
               == WorkspaceVisibility::Hidden;
    }));
    CHECK_FALSE(rig.showsNoneOf(before));
    CHECK(rig.host.opened.empty());
    // The window is free for the next switch, which lands.
    CHECK(rig.request(to(daemonB(), "2")) == SwitchOutcome::Switched);
    CHECK(rig.windowLink()->address() == daemonB());
}

TEST_CASE(
    "WindowSwitcher: a timed-out switch to a new workspace on another daemon leaves the one it made hidden",
    "[switch][link][timeout]")
{
    auto rig = SwitchRig {};
    auto const workspacesOnB = rig.b.onDaemon([&rig] { return rig.b.host->model().workspaceCount(); });
    rig.b.holdNewConnectionsAtLayout();
    auto told = Told {};
    rig.switcher->request(rig.shown(),
                          to(daemonB(), std::string { contour::remote::NewWorkspaceSpec }),
                          SwitchMode::Switch,
                          contour::session::NoLandingDeadline,
                          told.completion());
    // B makes the workspace for the new link's connection, whose first layout is held.
    REQUIRE(waitUntil([&] {
        return rig.b.onDaemon([&rig] { return rig.b.host->model().workspaceCount(); }) == workspacesOnB + 1;
    }));
    auto const made = rig.b.onDaemon(
        [&rig, workspacesOnB] { return rig.b.host->model().workspaceAt(workspacesOnB)->id().value; });
    REQUIRE(rig.timer.armed.size() == 1);

    rig.timer.fireAll();
    REQUIRE(told.outcome.has_value());
    REQUIRE_FALSE(told.outcome->has_value());

    // The link it started is gone before the window saw the workspace: put away, as the same daemon's
    // given-up switch leaves it, not left for the next launch to bring back.
    CHECK(waitUntil([&] { return showing(rig.b, made) == 0; }, 2000));
    rig.b.releaseHeld();
    drainGuiEvents();
    CHECK(rig.b.waitUntilOnDaemon([&] {
        return rig.b.host->visibility(vtworkspace::WorkspaceId { made }) == WorkspaceVisibility::Hidden;
    }));
    CHECK(rig.windowLink() == &rig.launchLink());
    CHECK(rig.launchLink().workspaceOf(rig.shown()) == rig.aWorkspaces[0]);
    CHECK(rig.host.opened.empty());
}

TEST_CASE("WindowSwitcher: a deadline of a switch that landed gives nothing up", "[switch][link][timeout]")
{
    auto rig = SwitchRig {};
    REQUIRE(rig.request(to(daemonA(), "2")) == SwitchOutcome::Switched);
    REQUIRE(rig.timer.armed.size() == 1);

    rig.timer.fireAll();
    drainGuiEvents();

    CHECK(rig.launchLink().workspaceOf(rig.shown()) == rig.aWorkspaces[1]);
    CHECK(visibilityOn(rig.a, rig.aWorkspaces[1]) == WorkspaceVisibility::Shown);
    CHECK_FALSE(rig.window->controller->notices().toastText().contains("Could not switch"));
}

TEST_CASE("WindowSwitcher: a daemon of several workspaces asks the user, and a cancel changes nothing",
          "[switch][link]")
{
    auto rig = SwitchRig {};
    auto const before = rig.windowSessions();

    auto const outcome = rig.request(to(daemonB()));

    REQUIRE(outcome == SwitchOutcome::PickerShown);
    REQUIRE(rig.picks.size() == 1);
    CHECK(rig.picks.front().window == rig.shown());
    CHECK(rig.picks.front().target == to(daemonB()));
    CHECK(rig.picks.front().workspaces.size() == rig.bWorkspaces.size());
    // Esc: the popup re-enters nothing.
    drainGuiEvents();
    CHECK(rig.host.started == 0);
    CHECK(rig.windowLink() == &rig.launchLink());
    CHECK(rig.launchLink().workspaceOf(rig.shown()) == rig.aWorkspaces[0]);
    CHECK(visibilityOn(rig.a, rig.aWorkspaces[0]) == WorkspaceVisibility::Shown);
    CHECK_FALSE(rig.showsNoneOf(before));

    // The pick re-enters the switch with the workspace picked.
    auto const picked =
        rig.request(to(daemonB(), vtcontrol::toString(rig.picks.front().workspaces.back().ref)));
    CHECK(picked == SwitchOutcome::Switched);
    CHECK(rig.windowLink() != &rig.launchLink());
}

TEST_CASE("WindowSwitcher: Open shows the target in a new window, and leaves this one alone",
          "[switch][link]")
{
    auto rig = SwitchRig {};
    auto const before = rig.windowSessions();

    auto const outcome = rig.request(to(daemonB(), "1"), SwitchMode::Open);

    REQUIRE(outcome == SwitchOutcome::Switched);
    REQUIRE(rig.host.opened.size() == 1);
    auto const opened = rig.host.opened.front()->id;
    auto const* const binding = rig.app.manager().bindingOf(opened);
    REQUIRE(rig.links.size() == 2);
    CHECK(binding == rig.links.back().get());
    CHECK(rig.links.back()->workspaceOf(opened) == rig.bWorkspaces[0]);
    CHECK(rig.windowLink() == &rig.launchLink());
    CHECK(rig.launchLink().workspaceOf(rig.shown()) == rig.aWorkspaces[0]);
    CHECK(visibilityOn(rig.a, rig.aWorkspaces[0]) == WorkspaceVisibility::Shown);
    CHECK_FALSE(rig.showsNoneOf(before));

    // Open again: shown in this process already, so that window is focused instead.
    CHECK(rig.request(to(daemonB(), "1"), SwitchMode::Open) == SwitchOutcome::FocusedExisting);
    CHECK(rig.host.opened.size() == 1);
}

TEST_CASE("WindowSwitcher: of two clients showing a workspace, only the one asked switches", "[switch][link]")
{
    auto rig = SwitchRig {};
    // Another client -- another process, another machine -- shows A's first workspace too.
    auto otherTimeline = TestTimeline {};
    auto other =
        makeNativeController(rig.a.endpoint(),
                             otherTimeline,
                             ControllerOptions { .intent = { .kind = vthost::proto::AttachIntentKind::Select,
                                                             .workspace = rig.aWorkspaces[0] } });
    REQUIRE(other->connectAndWait(10s).has_value());
    REQUIRE(waitUntil([&] { return showing(rig.a, rig.aWorkspaces[0]) == 2; }));

    REQUIRE(rig.request(to(daemonA(), "2")) == SwitchOutcome::Switched);

    CHECK(rig.launchLink().workspaceOf(rig.shown()) == rig.aWorkspaces[1]);
    CHECK(
        rig.a.waitUntilOnDaemon([&] { return rig.a.host->clientsShowing(rig.aWorkspaces[0]).size() == 1; }));
    CHECK(visibilityOn(rig.a, rig.aWorkspaces[0]) == WorkspaceVisibility::Shown);
    CHECK(other->settledLayout(rig.aWorkspaces[0]) != nullptr);
    other->stop();
}

TEST_CASE("WindowSwitcher: a window with no daemon behind it switches nowhere", "[switch][link]")
{
    auto app = contour::test::TestApp { std::make_unique<contour::test::MockPtySessionFactory>() };
    auto links = std::vector<std::unique_ptr<DaemonLink>> {};
    auto timeline = TestTimeline {};
    auto host = FakeApp { app.manager(), links, timeline, {} };
    auto switcher = contour::remote::WindowSwitcher {
        app.manager(),
        host,
        std::make_unique<FixtureLister>(std::map<std::string, DaemonFixture*> {}),
        [](contour::remote::SwitchPickRequest const&) { return std::expected<void, std::string> {}; },
        contour::remote::SwitchDeadline {
            .timeout = contour::remote::DefaultSwitchTimeout,
            .timer = [](std::chrono::milliseconds, std::function<void()> const&) {},
            .clock = host.clock }
    };
    auto const local = contour::test::ScopedController { app.manager() };
    auto told = Told {};

    switcher.request(
        local.id, to(daemonB()), SwitchMode::Switch, contour::session::NoLandingDeadline, told.completion());

    REQUIRE(told.outcome.has_value());
    CHECK_FALSE(told.outcome->has_value());
    CHECK(host.started == 0);
}

namespace
{
/// A switcher over an app with no link at all, and a window of it that runs in this process, owning two
/// workspaces (each with a tab) and showing the first.
struct InProcessRig
{
    contour::test::MockPtySessionFactory* factory = nullptr;
    contour::test::TestApp app { [this] {
        auto made = std::make_unique<contour::test::MockPtySessionFactory>();
        factory = made.get();
        return made;
    }() };
    std::vector<std::unique_ptr<DaemonLink>> links;
    TestTimeline timeline;
    FakeApp host { app.manager(), links, timeline, {} };
    contour::remote::WindowSwitcher switcher {
        app.manager(),
        host,
        std::make_unique<FixtureLister>(std::map<std::string, DaemonFixture*> {}),
        [this](contour::remote::SwitchPickRequest pick) {
            picks.push_back(std::move(pick));
            return std::expected<void, std::string> {};
        },
        contour::remote::SwitchDeadline {
            .timeout = contour::remote::DefaultSwitchTimeout,
            .timer = [](std::chrono::milliseconds, std::function<void()> const&) {},
            .clock = host.clock }
    };
    std::vector<contour::remote::SwitchPickRequest> picks; ///< What the switcher popup was asked to show.
    contour::test::ScopedControllerWithOsWindow window { app.manager() };
    vtworkspace::WorkspaceId first;
    vtworkspace::WorkspaceId second;

    InProcessRig()
    {
        window->bindWindow(window.osWindow.get());
        window->createNewTab();
        first = window->displayedWorkspace();
        second = app.manager().createWorkspaceIn(window->windowId());
        app.manager().selectWorkspace(window->windowId(), 1);
        window->createNewTab();
        app.manager().selectWorkspace(window->windowId(), 0);
    }

    /// @return The ref the switcher's rows name workspace @p workspace by.
    [[nodiscard]] vtcontrol::ObjectRef refOf(vtworkspace::WorkspaceId workspace)
    {
        auto const rows = switcher.windowWorkspaceRows(first);
        auto const found = std::ranges::find(rows, workspace.value, &contour::remote::WorkspaceRow::id);
        REQUIRE(found != rows.end());
        return found->ref;
    }

    /// Asks for @p target in @p mode, and returns how it went.
    [[nodiscard]] Told ask(contour::session::SwitchTarget target, SwitchMode mode = SwitchMode::Switch)
    {
        auto told = Told {};
        switcher.request(window->displayedWorkspace(),
                         std::move(target),
                         mode,
                         contour::session::NoLandingDeadline,
                         told.completion());
        return told;
    }
};
} // namespace

TEST_CASE("WindowSwitcher: a window that runs in-process lists and shows the workspaces it owns",
          "[switch][inprocess]")
{
    auto rig = InProcessRig {};
    REQUIRE(rig.switcher.windowWorkspaceRows(rig.first).size() == 2);

    auto const told =
        rig.ask({ .kind = SwitchDaemonKind::Same, .daemon = {}, .workspace = rig.refOf(rig.second) });
    REQUIRE(told.outcome.has_value());
    CHECK(told.outcome->has_value());
    CHECK(rig.window->displayedWorkspace() == rig.second);
}

TEST_CASE("WindowSwitcher: a window that runs in-process makes the workspace it is asked for, named",
          "[switch][inprocess]")
{
    auto rig = InProcessRig {};
    auto told = Told {};
    rig.switcher.createWorkspace(rig.window->displayedWorkspace(),
                                 { .kind = SwitchDaemonKind::Same, .daemon = {}, .workspace = {} },
                                 "billing",
                                 SwitchMode::Switch,
                                 told.completion());
    REQUIRE(told.outcome.has_value());
    CHECK(told.outcome->has_value());
    REQUIRE(rig.window->ownedWorkspaces().size() == 3);
    auto const made = rig.window->ownedWorkspaces().back();
    CHECK(rig.window->displayedWorkspace() == made);
    auto const rows = rig.switcher.windowWorkspaceRows(made);
    auto const named = std::ranges::find(rows, made.value, &contour::remote::WorkspaceRow::id);
    REQUIRE(named != rows.end());
    CHECK(named->label == "billing");
}

TEST_CASE("WindowSwitcher: a window that runs in-process refuses another daemon and a new window",
          "[switch][inprocess]")
{
    auto rig = InProcessRig {};
    auto const elsewhere = rig.ask(to(daemonB()));
    REQUIRE(elsewhere.outcome.has_value());
    CHECK_FALSE(elsewhere.outcome->has_value());

    auto const opened =
        rig.ask({ .kind = SwitchDaemonKind::Same, .daemon = {}, .workspace = rig.refOf(rig.second) },
                SwitchMode::Open);
    REQUIRE(opened.outcome.has_value());
    CHECK_FALSE(opened.outcome->has_value());
    CHECK(rig.window->displayedWorkspace() == rig.first);
}

TEST_CASE("WindowSwitcher: a window that runs in-process asks which of its workspaces, among several",
          "[switch][inprocess]")
{
    auto rig = InProcessRig {};
    auto const told = rig.ask(
        { .kind = SwitchDaemonKind::Same, .daemon = {}, .workspace = contour::session::AnyWorkspace {} });
    REQUIRE(told.outcome.has_value());
    REQUIRE(told.outcome->has_value());
    CHECK(**told.outcome == SwitchOutcome::PickerShown);
    REQUIRE(rig.picks.size() == 1);
    CHECK(rig.picks.front().workspaces.size() == 2);
    CHECK(rig.picks.front().window == rig.first);
    CHECK(rig.window->displayedWorkspace() == rig.first);
}

TEST_CASE("WindowSwitcher: a window that runs in-process says why it made no workspace",
          "[switch][inprocess]")
{
    auto rig = InProcessRig {};
    SECTION("its first shell could not start")
    {
        rig.factory->creation = contour::test::MockPtySessionFactory::SessionCreation::Refused;
        auto const told = rig.ask(
            { .kind = SwitchDaemonKind::Same, .daemon = {}, .workspace = contour::session::NewWorkspace {} });
        REQUIRE(told.outcome.has_value());
        REQUIRE_FALSE(told.outcome->has_value());
        CHECK(told.outcome->error()
              == contour::session::describe(contour::session::NewWorkspaceError::FirstShellFailed));
        CHECK(rig.window->ownedWorkspaces().size() == 2);
    }
    SECTION("the window has no pane to start it beside")
    {
        auto const made = rig.app.manager().newWorkspaceFor(vtworkspace::WorkspaceId { 987654 }, "x");
        REQUIRE_FALSE(made.has_value());
        CHECK(made.error() == contour::session::NewWorkspaceError::NoActiveSession);
        CHECK(contour::session::describe(contour::session::NewWorkspaceError::NoActiveSession)
              != contour::session::describe(contour::session::NewWorkspaceError::FirstShellFailed));
    }
}

TEST_CASE("WindowSwitcher: a window that shows no workspace it was asked for says so, and takes no focus",
          "[switch][inprocess]")
{
    auto rig = InProcessRig {};
    // The window's directory lists no such workspace: nothing is asked, so nothing is shown.
    CHECK(rig.window->showWorkspace(987654) == contour::window::SelectResult::Refused);
    CHECK(rig.window->showWorkspace(rig.first.value) == contour::window::SelectResult::Requested);
    CHECK(rig.window->displayedWorkspace() == rig.first);
}

TEST_CASE("WindowSwitcher: SwitchToWorkspace(N) in a window that runs in-process counts its directory, a "
          "tab-less workspace included",
          "[switch][inprocess]")
{
    auto rig = InProcessRig {};
    // first, second, a workspace holding no tab, and a fourth that does.
    auto const bare = rig.app.manager().createWorkspaceIn(rig.window->windowId());
    auto const fourth = rig.app.manager().createWorkspaceIn(rig.window->windowId());
    rig.app.manager().selectWorkspace(rig.window->windowId(), 3);
    rig.window->createNewTab();
    rig.app.manager().selectWorkspace(rig.window->windowId(), 0);
    REQUIRE(rig.window->ownedWorkspaces().size() == 4);
    REQUIRE(rig.window->displayedWorkspace() == rig.first);

    auto const at = [&](uint32_t position) {
        return rig.ask({ .kind = SwitchDaemonKind::Same,
                         .daemon = {},
                         .workspace = contour::session::WorkspaceAt { position } });
    };
    SECTION("the second is the second")
    {
        auto const told = at(2);
        REQUIRE(told.outcome.has_value());
        CHECK(told.outcome->has_value());
        CHECK(rig.window->displayedWorkspace() == rig.second);
    }
    SECTION("the tab-less one takes its number, and is no place to switch to")
    {
        auto const told = at(3);
        REQUIRE(told.outcome.has_value());
        CHECK_FALSE(told.outcome->has_value());
        CHECK(rig.window->displayedWorkspace() == rig.first);
        // The picker and the sidebar number the rest by the same list.
        auto const rows = rig.switcher.windowWorkspaceRows(rig.first);
        REQUIRE(rows.size() == 3);
        CHECK(rows[2].id == fourth.value);
        CHECK(rows[2].position == 4);
    }
    SECTION("the one after it is the fourth")
    {
        auto const told = at(4);
        REQUIRE(told.outcome.has_value());
        CHECK(told.outcome->has_value());
        CHECK(rig.window->displayedWorkspace() == fourth);
    }
    std::ignore = bare;
}

TEST_CASE("WindowSwitcher: a reconnect forgets the workspace a timed-out switch asked for",
          "[switch][link][timeout][reconnect]")
{
    auto rig = SwitchRig {};
    rig.a.onDaemon([&rig] {
        for (auto* const socket: rig.a.gated)
            socket->holdAtNextLayout();
        return 0;
    });
    auto told = Told {};
    rig.switcher->request(rig.shown(),
                          to(daemonA(), std::string { contour::remote::NewWorkspaceSpec }),
                          SwitchMode::Switch,
                          contour::session::NoLandingDeadline,
                          told.completion());
    REQUIRE(waitUntil(
        [&] { return rig.a.onDaemon([&rig] { return rig.a.host->model().workspaceCount(); }) == 3; }));
    REQUIRE(rig.timer.armed.size() == 1);
    rig.timer.fireAll();
    REQUIRE(told.outcome.has_value());
    REQUIRE_FALSE(told.outcome->has_value());

    // The connection goes before the layout of the workspace it was given up for ever arrived: the one sought
    // next resumes only what the window shows.
    auto const connectionIs = [&](vthost::client::ConnectionState state) {
        auto const* const controller = rig.launchLink().liveController();
        return controller != nullptr && controller->connectionState() == state;
    };
    rig.a.dropConnections();
    REQUIRE(waitUntil([&] { return !connectionIs(vthost::client::ConnectionState::Connected); }, 2000));
    REQUIRE(waitUntil([&] { return connectionIs(vthost::client::ConnectionState::Connected); }, 2000));
    REQUIRE(waitUntil([&] { return showing(rig.a, rig.aWorkspaces[0]) == 1; }, 2000));
    drainGuiEvents();

    // A new workspace a later switch asks for is that switch's, not mistaken for the given-up one and hidden.
    auto const again = rig.request(to(daemonA(), std::string { contour::remote::NewWorkspaceSpec }));
    INFO((again ? std::string { "switched" } : again.error()));
    CHECK(again == SwitchOutcome::Switched);
    auto const shown = rig.launchLink().workspaceOf(rig.shown());
    REQUIRE(shown.has_value());
    CHECK(*shown != rig.aWorkspaces[0]);
    CHECK(visibilityOn(rig.a, *shown) == WorkspaceVisibility::Shown);
}

TEST_CASE("WindowSwitcher: a labelled workspace is created on the daemon, then switched to",
          "[switch][link][switcher]")
{
    auto rig = SwitchRig {};
    auto const workspacesOnB = rig.b.onDaemon([&rig] { return rig.b.host->model().workspaceCount(); });
    auto told = Told {};
    rig.switcher->createWorkspace(rig.shown(), to(daemonB()), "q", SwitchMode::Switch, told.completion());
    REQUIRE(waitUntil([&] { return told.outcome.has_value(); }, 2000));
    INFO((*told.outcome ? std::string { "switched" } : told.outcome->error()));
    CHECK(*told.outcome == SwitchOutcome::Switched);
    CHECK(rig.lister->created == std::vector<std::string> { "q" });
    CHECK(rig.b.onDaemon([&rig] { return rig.b.host->model().workspaceCount(); }) == workspacesOnB + 1);
    REQUIRE(rig.windowLink() != nullptr);
    CHECK(rig.windowLink()->address() == daemonB());
    auto const made = rig.b.onDaemon(
        [&rig, workspacesOnB] { return rig.b.host->model().workspaceAt(workspacesOnB)->id().value; });
    CHECK(rig.windowLink()->workspaceOf(rig.shown()) == made);

    SECTION("the window's own daemon lists its workspaces from the directory, and nothing is listed")
    {
        auto const listings = rig.lister->listings;
        CHECK_FALSE(rig.switcher->windowWorkspaceRows(rig.shown()).empty());
        CHECK(rig.lister->listings == listings);
    }
}

TEST_CASE("WindowSwitcher: a workspace that cannot be created is said in the window, and nothing switches",
          "[switch][link][switcher]")
{
    // Asked for from the switcher, which has closed by the time the daemon answers: the window says why.
    auto rig = SwitchRig {};
    auto const before = rig.windowSessions();
    auto told = Told {};
    // A daemon the fixture's lister knows nothing of: `workspace.create` cannot reach it.
    auto const nowhere = DaemonAddress { .kind = SwitchDaemonKind::Label, .name = "nowhere" };
    rig.switcher->createWorkspace(rig.shown(), to(nowhere), "q", SwitchMode::Switch, told.completion());
    REQUIRE(waitUntil([&] { return told.outcome.has_value(); }, 2000));
    CHECK_FALSE(told.outcome->has_value());
    CHECK(rig.window->controller->notices().toastText().contains("Could not create a workspace on"));
    CHECK(rig.windowSessions() == before);
}

TEST_CASE("WindowSwitcher: the switcher popup's pick re-enters the switch, and Esc changes nothing",
          "[switch][link][switcher]")
{
    auto rig = SwitchRig {};
    auto const before = rig.windowSessions();
    REQUIRE(rig.request(to(daemonB())) == SwitchOutcome::PickerShown);
    REQUIRE(rig.picks.size() == 1);
    auto const& pick = rig.picks.front();

    // The popup, as the application builds it for a pick: the one daemon, its rows from the switch's listing.
    auto told = Told {};
    auto popup = contour::window::SwitcherController { contour::window::SwitcherServices {
        .discovery = nullptr,
        .savedHosts = {},
        .list = {},
        .request =
            [&](vtworkspace::WorkspaceId window, SwitchTarget target, SwitchMode mode) {
                rig.switcher->request(
                    window, std::move(target), mode, contour::session::NoLandingDeadline, told.completion());
            },
        .startDaemon = {},
    } };
    popup.open(contour::window::SwitcherOpening {
        .window = pick.window,
        .windowDaemon = to(daemonA()),
        .mode = pick.mode,
        .prompt = contour::window::SwitcherPrompt::Browse,
        .pick =
            contour::window::SwitcherDaemon {
                .address = pick.target,
                .workspaceCount = pick.workspaces.size(),
                .tag = {},
                .listing = contour::window::SwitcherListing::Listed,
                .failure = {},
                .workspaces = contour::remote::switcherWorkspacesOf(pick.workspaces, pick.window),
            },
    });
    // B's two workspaces, and a new one.
    REQUIRE(popup.rowCount() == 3);

    SECTION("Esc")
    {
        popup.cancel();
        drainGuiEvents();
        CHECK_FALSE(told.outcome.has_value());
        CHECK(rig.host.started == 0);
        CHECK(rig.windowLink() == &rig.launchLink());
        CHECK(rig.launchLink().workspaceOf(rig.shown()) == rig.aWorkspaces[0]);
        CHECK_FALSE(rig.showsNoneOf(before));
    }
    SECTION("Return on B's second workspace")
    {
        popup.setCurrentIndex(1);
        popup.activate(1);
        REQUIRE(waitUntil([&] { return told.outcome.has_value(); }, 2000));
        CHECK(*told.outcome == SwitchOutcome::Switched);
        REQUIRE(rig.windowLink() != nullptr);
        CHECK(rig.windowLink()->address() == daemonB());
        CHECK(rig.windowLink()->workspaceOf(rig.shown()) == rig.bWorkspaces[1]);
    }
}

TEST_CASE("WindowSwitcher: a switch a pane asks for is asked about, and switches once allowed",
          "[switch][link][guard]")
{
    using contour::session::PermissionDecision;
    using contour::session::PermissionMemory;
    auto rig = SwitchRig {};
    auto api = ControlApi { rig.a };
    auto const* const oldLink = &rig.launchLink();

    api.callFromPaneOf(rig.aWorkspaces[0], { .toLabel = "b", .toWorkspace = "2" });

    auto& asking = awaitSwitchQuestion(rig);
    CHECK(questionLine(asking).contains(QStringLiteral("the workspace “2” of the daemon “b”")));
    CHECK(asking.pendingPermission()
              .value(QStringLiteral("rememberText"))
              .toString()
              .contains(QStringLiteral("from the daemon “a” to the daemon “b”")));
    // Nothing moves while the question is up.
    CHECK_FALSE(api.answer().has_value());
    CHECK(rig.windowLink() == oldLink);

    contour::test::answerShown(asking, PermissionDecision::Allow, PermissionMemory::Once);

    auto const reply = awaitAnswer(api);
    CHECK(reply.outcome == "switched");
    CHECK(reply.detail.empty());
    auto* const now = rig.windowLink();
    REQUIRE(now != nullptr);
    CHECK(now->address() == daemonB());
    CHECK(now->workspaceOf(rig.shown()) == rig.bWorkspaces[1]);
    CHECK(rig.a.waitUntilOnDaemon([&] {
        return rig.a.host->visibility(vtworkspace::WorkspaceId { rig.aWorkspaces[0] })
               == WorkspaceVisibility::Hidden;
    }));
    // Allowed once: the next one asks again.
    CHECK_FALSE(rig.host.grants.remembered(daemonA(), to(daemonB())).has_value());
}

// Review (PR 3, item 8): the question outlived the daemon's wait for its answer, and an Allow after that
// switched the window while `contour switch` had already given up.
TEST_CASE("WindowSwitcher: a switch question unanswered until shortly before the daemon gives up is refused",
          "[switch][link][guard][timeout]")
{
    using contour::session::PermissionDecision;
    using contour::session::PermissionMemory;
    auto rig = SwitchRig {};
    auto expiries = ManualSwitchTimer {};
    rig.host.questionDeadline.timer = expiries.deadline(rig.host.clock).timer;
    auto api = ControlApi { rig.a };
    auto const* const oldLink = &rig.launchLink();

    api.callFromPaneOf(rig.aWorkspaces[0], { .toLabel = "b", .toWorkspace = "2" });
    auto& asking = awaitSwitchQuestion(rig);
    REQUIRE(expiries.armed.size() == 1);
    CHECK(expiries.armed.front().delay
          == contour::remote::switchQuestionExpiry(std::chrono::milliseconds { vthost::SwitchAnswerTimeout },
                                                   contour::remote::DefaultSwitchTimeout));
    CHECK(expiries.armed.front().delay < std::chrono::milliseconds { vthost::SwitchAnswerTimeout });

    expiries.fireAll();

    auto const reply = awaitAnswer(api);
    CHECK(reply.outcome == "refused");
    // The question is gone, and the window stays where it was.
    drainGuiEvents();
    CHECK_FALSE(asking.pendingPermissionAsk()
                == contour::session::PermissionAsk { contour::session::GuardedRole::SwitchWindow });
    CHECK(rig.windowLink() == oldLink);
    CHECK(rig.host.started == 0);
}

// Review (PR 4, I1): the question's expiry left the switch its own deadline, but the switch lists the target
// daemon's workspaces before it follows anything, so an Allow near the expiry could land after the daemon
// replied Failed. The switch now carries when it must land by, counted from the request's arrival.
TEST_CASE("WindowSwitcher: an allowed switch lands before the daemon gives up, whatever the listing took",
          "[switch][link][guard][timeout]")
{
    using contour::session::PermissionDecision;
    using contour::session::PermissionMemory;
    auto rig = SwitchRig {};
    auto api = ControlApi { rig.a };
    auto const* const oldLink = &rig.launchLink();
    // When the daemon stops waiting, less the reply's trip back.
    auto const landingWindow =
        std::chrono::milliseconds { vthost::SwitchAnswerTimeout } - contour::remote::SwitchReplyMargin;

    api.callFromPaneOf(rig.aWorkspaces[0], { .toLabel = "b", .toWorkspace = "2" });
    auto& asking = awaitSwitchQuestion(rig);
    contour::test::answerShown(asking, PermissionDecision::Allow, PermissionMemory::Once);

    SECTION("a listing done after that time starts nothing")
    {
        // The listing is told after the time the switch had to land by.
        rig.host.clock.advance(landingWindow);

        auto const reply = awaitAnswer(api);
        CHECK(reply.outcome == "failed");
        CHECK(reply.detail.contains("stopped waiting"));
        CHECK(rig.host.started == 0);
        CHECK(rig.timer.armed.empty());
        CHECK(rig.windowLink() == oldLink);
        CHECK(showing(rig.b, rig.bWorkspaces[1]) == 0);
    }
    SECTION("a listing done shortly before it gives the switch only what is left")
    {
        rig.host.clock.advance(landingWindow - 3s);

        REQUIRE(waitUntil([&] { return rig.host.started == 1; }));
        REQUIRE(rig.timer.armed.size() == 1);
        CHECK(rig.timer.armed.front().delay == 3s);
    }
}

TEST_CASE("WindowSwitcher: a switch the user refused to have asked again is refused without a prompt",
          "[switch][link][guard]")
{
    using contour::session::PermissionDecision;
    using contour::session::PermissionMemory;
    auto rig = SwitchRig {};
    auto api = ControlApi { rig.a };
    auto const before = rig.windowSessions();

    api.callFromPaneOf(rig.aWorkspaces[0], { .toLabel = "b", .toWorkspace = "2" });
    contour::test::answerShown(awaitSwitchQuestion(rig), PermissionDecision::Deny, PermissionMemory::Session);
    auto const first = awaitAnswer(api);
    CHECK(first.outcome == "refused");
    CHECK(first.detail == contour::remote::SwitchRefusedDetail);

    // Another workspace of the same daemon is the same pair: refused at once, nothing asked.
    api.callFromPaneOf(rig.aWorkspaces[0], { .toLabel = "b", .toWorkspace = "1" });
    auto const second = awaitAnswer(api);
    CHECK(second.outcome == "refused");
    auto const* const session = rig.app.manager().actingSessionOfWindow(rig.shown());
    REQUIRE(session != nullptr);
    CHECK_FALSE(session->pendingPermissionAsk().has_value());
    CHECK(rig.windowLink() == &rig.launchLink());
    CHECK(rig.host.started == 0);
    CHECK_FALSE(rig.showsNoneOf(before));
    CHECK(visibilityOn(rig.a, rig.aWorkspaces[0]) == WorkspaceVisibility::Shown);
}

TEST_CASE("WindowSwitcher: a switch for a workspace no window here shows fails, saying so",
          "[switch][link][guard]")
{
    auto rig = SwitchRig {};
    auto replies = std::make_shared<std::vector<vthost::proto::SwitchReply>>();

    // The window's connection is asked for its hidden workspace, which none of its windows shows.
    auto const clients = rig.a.onDaemon([&rig] { return rig.a.host->clientsShowing(rig.aWorkspaces[0]); });
    REQUIRE(clients.size() == 1);
    rig.a.onDaemon([&rig, &clients, replies] {
        rig.a.host->requestSwitch(
            clients.front(),
            vtworkspace::WorkspaceId { rig.aWorkspaces[1] },
            vthost::proto::SwitchTargetWire {
                .kind = SwitchDaemonKind::Label, .daemon = "b", .workspace = "2" },
            [replies](vthost::proto::SwitchReply const& reply) { replies->push_back(reply); });
        return 0;
    });

    REQUIRE(waitUntil([&] { return rig.a.onDaemon([&] { return !replies->empty(); }); }, 2000));
    auto const reply = rig.a.onDaemon([&] { return replies->front(); });
    CHECK(reply.outcome == SwitchOutcome::Failed);
    CHECK(reply.detail == std::format("no window here shows workspace {}", rig.aWorkspaces[1]));
    CHECK(rig.windowLink() == &rig.launchLink());
}

TEST_CASE("WindowSwitcher: a client that does not answer switch requests is still too old for one",
          "[switch][link][guard]")
{
    auto rig = SwitchRig { contour::remote::NativeController::SwitchRequests::Declined };
    auto api = ControlApi { rig.a };

    api.callFromPaneOf(rig.aWorkspaces[0], { .toLabel = "b", .toWorkspace = "2" });

    auto const reply = awaitAnswer(api);
    CHECK(reply.outcome == "failed");
    CHECK(reply.detail == "the window cannot switch (client too old)");
    CHECK(rig.windowLink() == &rig.launchLink());
}

TEST_CASE("WindowSwitcher: a real window shows each daemon's pane as it switches between them",
          "[display][switch][link]")
{
    REQUIRE_DISPLAY_OR_SKIP();
    auto rig = SwitchRig {};
    printInPaneOf(rig.a, rig.aWorkspaces[0], "\r\nMARKER-FROM-ALPHA");
    printInPaneOf(rig.b, rig.bWorkspaces[0], "\r\nMARKER-FROM-BRAVO");

    // The window's display, made for the pane the window shows as the QML's Loader makes one per pane.
    auto window = QQuickWindow {};
    window.resize(800, 600);
    window.show();
    auto display = std::unique_ptr<contour::display::TerminalDisplay> {};
    auto const showFirstPane = [&] {
        REQUIRE(waitUntil([&] { return !rig.app.manager().sessionsOfWindow(rig.shown()).empty(); }));
        display = std::make_unique<contour::display::TerminalDisplay>();
        display->setParentItem(window.contentItem());
        display->setSize(QSizeF(800, 600));
        display->setSession(rig.app.manager().sessionsOfWindow(rig.shown()).front());
    };
    // Grabs frames until the display has drawn @p text, or the wait ends.
    auto const drawn = [&](std::string_view text) {
        return waitUntil([&] {
            QCoreApplication::processEvents();
            std::ignore = window.grabWindow();
            return drawsText(display->session().terminal().renderBuffer().get(), text);
        });
    };
    auto const drawsNow = [&](std::string_view text) {
        return drawsText(display->session().terminal().renderBuffer().get(), text);
    };

    showFirstPane();
    REQUIRE(drawn("MARKER-FROM-ALPHA"));
    CHECK_FALSE(drawsNow("MARKER-FROM-BRAVO"));

    // Switch in place to B: the display goes before the pane it shows does.
    display.reset();
    REQUIRE(rig.request(to(daemonB(), "1")) == SwitchOutcome::Switched);
    showFirstPane();
    REQUIRE(drawn("MARKER-FROM-BRAVO"));
    CHECK_FALSE(drawsNow("MARKER-FROM-ALPHA"));

    // And back: A's workspace kept running in its daemon, with what it showed.
    display.reset();
    REQUIRE(rig.request(to(daemonA(), "1")) == SwitchOutcome::Switched);
    showFirstPane();
    REQUIRE(drawn("MARKER-FROM-ALPHA"));
    CHECK_FALSE(drawsNow("MARKER-FROM-BRAVO"));
    display.reset();
}
