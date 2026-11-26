// SPDX-License-Identifier: Apache-2.0
#include <contour/config/Actions.hpp>
#include <contour/display/TerminalDisplay.hpp>
#include <contour/platform/Notifier.hpp>
#include <contour/remote/DaemonLink.hpp>
#include <contour/remote/DaemonPresence.hpp>
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
#include <contour/test/RecordingAnnouncer.hpp>
#include <contour/test/SwitcherFakes.hpp>
#include <contour/window/SidebarJumps.hpp>
#include <contour/window/SwitcherController.hpp>
#include <contour/window/WindowController.hpp>

#include <vtbackend/render/RenderBuffer.hpp>
#include <vtbackend/screen/Terminal.hpp>

#include <core/Random.hpp>

#include <QtCore/QCoreApplication>
#include <QtCore/QPointF>
#include <QtCore/QPointer>
#include <QtCore/QVariantList>
#include <QtCore/QVariantMap>
#include <QtGui/QImage>
#include <QtQuick/QQuickWindow>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <atomic>
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

#include <QtTest/QSignalSpy>
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
    return DaemonAddress { .kind = SwitchDaemonKind::Name, .name = "a" };
}
/// Daemon B, which a cross-daemon switch goes to, by label.
[[nodiscard]] DaemonAddress daemonB()
{
    return DaemonAddress { .kind = SwitchDaemonKind::Name, .name = "b" };
}
/// A daemon whose socket nobody serves.
[[nodiscard]] DaemonAddress unreachable()
{
    return DaemonAddress { .kind = SwitchDaemonKind::Name, .name = "down" };
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
        if (holding)
        {
            held.emplace_back(std::move(target), std::move(done));
            return;
        }
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
            listed = describe(*found->second, staleHoldingTab);
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
                daemon.host->objects().setName(ref, label);
                return ref;
            });
        QMetaObject::invokeMethod(
            &_receiver,
            [done = std::move(done), made = std::move(made)]() mutable { done(std::move(made)); },
            Qt::QueuedConnection);
    }

    int listings = 0; ///< How often a daemon was listed.
    /// Whether a listing waits for release() rather than being answered.
    bool holding = false;
    /// The listings held, in order.
    std::vector<std::pair<contour::remote::ListTarget, contour::remote::WorkspacesListed>> held;

    /// Answers every held listing, as the daemon would have, and holds nothing more.
    void release()
    {
        holding = false;
        for (auto& [target, done]: std::exchange(held, {}))
            list(std::move(target), std::move(done));
    }
    /// The daemons whose listing the user declines to trust, by label.
    std::set<std::string> declined;
    std::vector<std::string> created; ///< The labels a workspace was created with, in order.
    /// Daemon workspaces the listing still shows holding a tab, though they hold none by the time a switch
    /// lands: the last tab closed since the listing was taken.
    std::set<uint64_t> staleHoldingTab;

  private:
    [[nodiscard]] static std::vector<vtcontrol::WorkspaceInfo> describe(
        DaemonFixture& daemon, std::set<uint64_t> const& staleHoldingTab)
    {
        return daemon.onDaemon([&daemon, &staleHoldingTab] {
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
                info.tabs.resize(staleHoldingTab.contains(id.value)
                                     ? 1
                                     : static_cast<std::size_t>(model.workspaceAt(index)->tabCount()));
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

/// A transport that counts the dials made through it, each made by the transport it wraps.
class CountingTransport final: public vthost::ITransportFactory
{
  public:
    explicit CountingTransport(std::shared_ptr<vthost::ITransportFactory> inner): _inner(std::move(inner)) {}

    [[nodiscard]] core::async::Task<
        std::expected<std::unique_ptr<vthost::Connection>, vthost::TransportError>>
    connect(core::net::EventLoop& loop) override
    {
        _dials.fetch_add(1, std::memory_order_relaxed);
        return _inner->connect(loop);
    }

    [[nodiscard]] std::string describe() const override { return _inner->describe(); }

    void holdDaemonStart(core::platform::SteadyTimePoint until) override { _inner->holdDaemonStart(until); }

    /// @return How many dials were made through it so far. Any thread.
    [[nodiscard]] int dials() const noexcept { return _dials.load(std::memory_order_relaxed); }

  private:
    std::shared_ptr<vthost::ITransportFactory> _inner;
    std::atomic<int> _dials = 0;
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
    void linkChanged(DaemonLink& /*link*/) override {}
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
        DaemonAddress const& daemon, vtworkspace::WorkspaceId window) override
    {
        askingWindows.push_back(window);
        if (daemon == unreachable())
            // Nothing listens on port 1.
            return linkConfig(
                vthost::TcpEndpoint { .host = "127.0.0.1", .port = 1, .token = {}, .caPem = {} },
                _timeline,
                daemon);
        auto const found = _endpoints.find(daemon.name);
        if (found == _endpoints.end())
            return std::unexpected(std::string { "no such daemon" });
        // As the application's: a transport of the window's own, which every dial of the link goes through.
        auto config = linkConfig(found->second, _timeline, daemon);
        auto counted = std::make_shared<CountingTransport>(std::move(config.controller.transport));
        windowTransports.insert_or_assign(daemon.name, counted);
        config.controller.transport = std::move(counted);
        return config;
    }
    DaemonLink& startSwitchLink(DaemonLinkConfig config) override
    {
        ++started;
        auto& link = *links.emplace_back(
            std::make_unique<DaemonLink>(DaemonLink::Id { _nextId++ }, std::move(config), *this, _manager));
        contour::remote::passOnLaunchResume(switchableLinks());
        return link;
    }
    DaemonLink& adoptSwitchLink(DaemonLinkConfig config,
                                std::unique_ptr<contour::remote::NativeController> observing,
                                contour::remote::PromotionRequest request,
                                std::shared_ptr<vthost::ITransportFactory> interactive,
                                contour::remote::GiveBack giveBack) override
    {
        ++started;
        ++adopted;
        auto& link = *links.emplace_back(DaemonLink::adopt(DaemonLink::Id { _nextId++ },
                                                           std::move(config),
                                                           std::move(observing),
                                                           std::move(request),
                                                           std::move(interactive),
                                                           std::move(giveBack),
                                                           *this,
                                                           _manager));
        contour::remote::passOnLaunchResume(switchableLinks());
        return link;
    }

    std::vector<std::unique_ptr<contour::test::ScopedController>> opened; ///< Every window opened, in order.
    std::vector<std::unique_ptr<DaemonLink>>& links;                      ///< Every link, the launch's first.
    int endings = 0;                                                      ///< How often a link said it ended.
    int started = 0; ///< How many links a switch started.
    int adopted = 0; ///< How many of them took a presence's observer over.
    /// The windows linkConfigFor() was asked for, where a new link's prompts are shown, in order.
    std::vector<vtworkspace::WorkspaceId> askingWindows;
    /// The transport linkConfigFor() last made for each daemon, by name.
    std::map<std::string, std::shared_ptr<CountingTransport>> windowTransports;
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

/// The presence as a switch sees it: the sections it lists and the observers it holds, set by the test, and
/// what was handed back to it.
class FakeObserverPool final: public contour::remote::IObserverPool
{
  public:
    [[nodiscard]] contour::remote::Adoption adoptObserver(contour::window::DaemonRef const& daemon,
                                                          contour::window::DaemonInstance instance) override
    {
        auto const found = observers.find(daemon);
        if (found == observers.end() || found->second == nullptr)
            return {};
        auto const run = found->second->daemonInstance();
        if (instance != contour::window::DaemonInstance {} && run && *run != instance.value)
            return {};
        ++adoptions;
        return contour::remote::Adoption { .controller = std::exchange(found->second, nullptr),
                                           .handOff = contour::remote::HandOffId { nextHandOff++ } };
    }

    void giveBack(contour::window::DaemonRef const& daemon,
                  contour::remote::HandOffId handOff,
                  std::unique_ptr<contour::remote::NativeController> demoted) override
    {
        handOffsEnded.push_back(handOff);
        givenBack.emplace_back(daemon, std::move(demoted));
    }

    [[nodiscard]] contour::remote::HandOffId expectGiveBack(contour::window::DaemonRef const& daemon,
                                                            contour::remote::DaemonLinkId from) override
    {
        expected.emplace_back(daemon, from);
        return contour::remote::HandOffId { nextHandOff++ };
    }

    [[nodiscard]] contour::remote::ObserverWanted wantsObserver(
        contour::window::DaemonRef const& /*daemon*/,
        contour::remote::DaemonLinkId /*leaving*/) const override
    {
        return wanted;
    }

    [[nodiscard]] std::optional<contour::window::DaemonSection> listedSection(
        contour::window::DaemonRef const& daemon) const override
    {
        auto const found = sections.find(daemon);
        return found != sections.end() ? std::optional { found->second } : std::nullopt;
    }

    /// Stops what was handed back, while its daemon is still there.
    void stopGivenBack()
    {
        for (auto& [daemon, controller]: givenBack)
            if (controller != nullptr)
                controller->stop();
        for (auto& [daemon, controller]: observers)
            if (controller != nullptr)
                controller->stop();
    }

    /// @return How many connections were handed back to @p daemon, the stopped ones (null) apart.
    [[nodiscard]] std::size_t heldFor(contour::window::DaemonRef const& daemon) const
    {
        return static_cast<std::size_t>(std::ranges::count_if(
            givenBack, [&](auto const& entry) { return entry.first == daemon && entry.second != nullptr; }));
    }

    /// @return How often @p daemon was told a connection ended instead (null).
    [[nodiscard]] std::size_t stoppedFor(contour::window::DaemonRef const& daemon) const
    {
        return static_cast<std::size_t>(std::ranges::count_if(
            givenBack, [&](auto const& entry) { return entry.first == daemon && entry.second == nullptr; }));
    }

    std::map<contour::window::DaemonRef, contour::window::DaemonSection> sections; ///< What it lists.
    /// The observers it hands over, by daemon; null once handed over.
    std::map<contour::window::DaemonRef, std::unique_ptr<contour::remote::NativeController>> observers;
    contour::remote::ObserverWanted wanted =
        contour::remote::ObserverWanted::No; ///< wantsObserver()'s answer.
    int adoptions = 0;                       ///< How many observers it handed over.
    /// What came back, in order: a connection, or null for one stopped.
    std::vector<std::pair<contour::window::DaemonRef, std::unique_ptr<contour::remote::NativeController>>>
        givenBack;
    /// The give-backs announced, in order.
    std::vector<std::pair<contour::window::DaemonRef, contour::remote::DaemonLinkId>> expected;
    /// The hand-over each give-back named, in order.
    std::vector<contour::remote::HandOffId> handOffsEnded;
    uint64_t nextHandOff = 1; ///< What the next hand-over is named.
};

/// Random bytes that are always the same: a creation token a test can name.
class FixedRandom final: public core::IRandom
{
  public:
    void fill(std::span<std::byte> out) override { std::ranges::fill(out, std::byte { 0x5a }); }
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
    /// When a deadline fires: when the test says (Manual), or the moment it is armed (AtOnce), before the
    /// link it bounds read anything from its daemon.
    enum class Firing : uint8_t
    {
        Manual = 0,
        AtOnce,
    };
    Firing firing = Firing::Manual;

    /// @param clock What the switch's time is read from.
    [[nodiscard]] contour::remote::SwitchDeadline deadline(core::platform::IClock const& clock)
    {
        return contour::remote::SwitchDeadline {
            .timeout = contour::remote::DefaultSwitchTimeout,
            .timer =
                [this](std::chrono::milliseconds delay, std::function<void()> fire) {
                    if (firing == Firing::AtOnce)
                        fire();
                    else
                        armed.push_back({ delay, std::move(fire) });
                },
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

/// What the application's presence of the listed daemons lists in a SwitchRig's window.
enum class RigListing : uint8_t
{
    OwnOnly = 0, ///< Nothing but the window's own daemon: the production presence finds no daemon of a test.
    WithB,       ///< Daemon B too, as the saved host `b`, observed while no window is on it.
    BothListed, ///< B as WithB, and A too, as the local daemon `a` running now: a window can come back to it.
};

/// What a SwitchRig's switcher reaches as the presence of the listed daemons.
enum class SwitcherPool : uint8_t
{
    Fake = 0, ///< The rig's FakeObserverPool: a jump dials its own connection, and a left link is stopped.
    Presence, ///< The application's presence itself, as production wires it: a jump adopts its observer.
};

/// Finds no local daemon: a test's presence never reaches this machine's.
[[nodiscard]] std::shared_ptr<contour::cli::IDaemonDiscovery> noLocalDaemons()
{
    return std::make_shared<contour::test::FixedDiscovery>(std::vector<contour::cli::DiscoveredDaemon> {});
}

/// Finds the local daemon `a` running, as a presence that lists every local daemon running would.
[[nodiscard]] std::shared_ptr<contour::cli::IDaemonDiscovery> localDaemonA()
{
    return std::make_shared<contour::test::FixedDiscovery>(std::vector {
        contour::cli::DiscoveredDaemon { .label = "a", .socket = {}, .workspaces = std::nullopt } });
}

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
    FakeObserverPool
        pool;           ///< The switcher's presence (SwitcherPool::Fake); lists nothing unless a test says.
    FixedRandom random; ///< Draws the creation tokens.
    FixtureLister* lister = nullptr;                       ///< The switcher's.
    std::vector<contour::remote::SwitchPickRequest> picks; ///< What the switcher popup was asked to show.
    ManualSwitchTimer timer;                               ///< The switcher's deadlines.
    std::unique_ptr<contour::remote::WindowSwitcher> switcher;
    contour::test::RecordingAnnouncer* said = nullptr; ///< What the window's screen reader was told.
    std::unique_ptr<contour::test::ScopedController> window;

    SwitchRig(): SwitchRig(contour::remote::NativeController::SwitchRequests::Answered, DaemonBStart::Seeded)
    {
    }

    /// @param listing What the window's sidebar lists besides its own daemon.
    explicit SwitchRig(RigListing listing):
        SwitchRig(contour::remote::NativeController::SwitchRequests::Answered, DaemonBStart::Seeded, listing)
    {
    }

    /// @param listing What the window's sidebar lists besides its own daemon.
    /// @param switcherPool What the switcher reaches as the presence.
    SwitchRig(RigListing listing, SwitcherPool switcherPool):
        SwitchRig(contour::remote::NativeController::SwitchRequests::Answered,
                  DaemonBStart::Seeded,
                  listing,
                  switcherPool)
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
    /// @param listing What the window's sidebar lists besides its own daemon.
    /// @param switcherPool What the switcher reaches as the presence.
    SwitchRig(contour::remote::NativeController::SwitchRequests switchRequests,
              DaemonBStart bStart,
              RigListing listing = RigListing::OwnOnly,
              SwitcherPool switcherPool = SwitcherPool::Fake):
        bWorkspaces(bStart == DaemonBStart::Seeded ? seedWorkspaces(b, 2) : std::vector<uint64_t> {}),
        app { std::make_unique<contour::test::MockPtySessionFactory>(),
              nullptr,
              nullptr,
              nullptr,
              contour::test::TestApp::testHostIdentity(),
              nullptr,
              {},
              nullptr,
              contour::platform::nullAppearanceServices(),
              nullptr,
              contour::config::HostPlatform::Other,
              nullptr,
              presenceOf(listing) }
    {
        hide(a, aWorkspaces[1]);
        auto made = std::make_unique<FixtureLister>(
            std::map<std::string, DaemonFixture*> { { "a", &a }, { "b", &b }, { "down", &b } });
        lister = made.get();
        auto& reached = switcherPool == SwitcherPool::Presence
                            ? static_cast<contour::remote::IObserverPool&>(presence())
                            : static_cast<contour::remote::IObserverPool&>(pool);
        switcher = std::make_unique<contour::remote::WindowSwitcher>(
            app.manager(),
            host,
            reached,
            std::move(made),
            [this](contour::remote::SwitchPickRequest pick) {
                picks.push_back(std::move(pick));
                return std::expected<void, std::string> {};
            },
            timer.deadline(host.clock),
            random);
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
        // Its menus in the scene, as a test picks their rows there; what it says recorded.
        window = std::make_unique<contour::test::ScopedController>(
            app.manager(),
            [this](std::function<QObject*()> const& /*anchor*/)
                -> std::unique_ptr<contour::platform::Announcer> {
                auto announcer = std::make_unique<contour::test::RecordingAnnouncer>();
                said = announcer.get();
                return announcer;
            });
        REQUIRE(pollUntilAttached(launchLink(), 10s).has_value());
        REQUIRE(launchLink().bindPendingWindow(*window->controller)
                == contour::remote::WindowAdoption::Adopted);
        REQUIRE(waitUntil([&] { return launchLink().workspaceOf(shown()) == aWorkspaces[0]; }));
        REQUIRE(waitUntil([&] { return !app.manager().sessionsOfWindow(shown()).empty(); }));
        REQUIRE(a.visibilityOf(aWorkspaces[0]) == WorkspaceVisibility::Shown);
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
        pool.stopGivenBack();
        host.opened.clear();
        window.reset();
        drainGuiEvents();
    }

    [[nodiscard]] DaemonLink& launchLink() const { return *links.front(); }

    /// @return The application's presence of the listed daemons, which the window's sidebar reads.
    [[nodiscard]] contour::remote::DaemonPresence& presence()
    {
        return dynamic_cast<contour::remote::DaemonPresence&>(app.app().daemonSections());
    }

    /// What the application's presence is given: production's, or one listing B and fed by this rig's links.
    [[nodiscard]] contour::remote::PresenceOverrides presenceOf(RigListing listing)
    {
        if (listing == RigListing::OwnOnly)
            return {};
        // Each daemon's observer reaches that daemon: A by its local label, every other one B.
        auto const endpointOf = [this](contour::window::DaemonRef const& daemon) {
            return daemon == contour::window::localDaemonRef("a") ? a.endpoint() : b.endpoint();
        };
        return contour::remote::PresenceOverrides {
            .makeObserver =
                [this, endpointOf](contour::window::DaemonRef const& daemon,
                                   std::shared_ptr<vthost::ITransportFactory> transport,
                                   vthost::client::BackoffSchedule backoff) {
                    return makeNativeController(
                        endpointOf(daemon),
                        timeline,
                        ControllerOptions { .backoff = backoff,
                                            .role = vthost::proto::ConnectionRole::Observe,
                                            .transport = std::move(transport) });
                },
            .refreshTimer = [](std::chrono::milliseconds, std::function<void()> const&) {},
            .discovery = listing == RigListing::BothListed ? localDaemonA() : noLocalDaemons(),
            // B's configuration lists it; what the user chose is the app's GUI state, as production reads it.
            .settings =
                [this] {
                    auto settings = contour::remote::ListingSettings {
                        .hosts = { { "b", contour::window::SidebarListingChoice::Listed } },
                        .shownByUser = {},
                        .hiddenByUser = {},
                    };
                    if (auto const state = app.guiState().load())
                    {
                        settings.shownByUser = state->sidebarDaemons.shown;
                        settings.hiddenByUser = state->sidebarDaemons.hidden;
                    }
                    return settings;
                },
            .observerTransport = [endpointOf](contour::window::DaemonRef const& daemon)
                -> std::expected<std::shared_ptr<vthost::ITransportFactory>, contour::remote::DialRefusal> {
                return makeTestTlsTransport(endpointOf(daemon));
            },
            .windowLinks = [this] { return linkViews(); },
        };
    }

    /// @return This rig's attached links, as the application's own would be told to its presence.
    [[nodiscard]] std::vector<contour::remote::WindowLinkView> linkViews()
    {
        auto views = std::vector<contour::remote::WindowLinkView> {};
        for (auto const& link: links)
        {
            auto* const controller = link->liveController();
            if (!link->isAttached() || controller == nullptr)
                continue;
            auto view = contour::remote::WindowLinkView {
                .link = link->id(),
                .daemon = contour::remote::daemonRefOf(link->address(), link->id()),
                .instance = contour::window::DaemonInstance { controller->daemonInstance().value_or(0) },
                .name = std::string { link->hostLabel() },
                .locality = link->daemonLocality(),
                .directory = link->directory(),
                .state = contour::remote::linkStateOf(
                    controller->connectionState(), std::nullopt, controller->observeSupport()),
                .shownWorkspaces = {},
                .controller = controller,
            };
            for (auto const window: app.manager().windowsBoundTo(*link))
                if (auto const workspace = link->workspaceOf(window))
                    view.shownWorkspaces.insert(*workspace);
            views.push_back(std::move(view));
        }
        return views;
    }

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
        switcher->request(shown(), std::move(target), mode, contour::session::UserSwitch, told.completion());
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
TEST_CASE("WindowSwitcher: a program's switch to another daemon is told it switched while the old link still "
          "serves "
          "the window",
          "[switch][link]")
{
    using namespace std::chrono_literals;
    auto rig = SwitchRig {};
    auto& oldLink = rig.launchLink();
    auto toldWhileOld = std::optional<bool> {};
    auto told = Told {};
    auto completion = told.completion();
    // A program's switch has a time to land by (proto::SwitchRequest).
    rig.switcher->request(
        rig.shown(),
        to(daemonB(), "2"),
        SwitchMode::Switch,
        contour::session::SwitchWaiter { .landBy = rig.host.clock.now() + 20s,
                                         .told = contour::session::SwitchTold::BeforeLeaving },
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

// Review (Phase 6, I3): the user's switch is told once the window is the other daemon's, so what its
// completion runs on Switched -- a sidebar menu's verb after its jump -- runs in the window as it landed,
// at once, with no turn of the event loop in between.
TEST_CASE(
    "WindowSwitcher: the user's switch to another daemon is told it switched once the window is rebound",
    "[switch][link]")
{
    auto rig = SwitchRig {};
    auto& oldLink = rig.launchLink();
    auto const before = rig.app.manager().daemonRefOf(rig.shown());
    REQUIRE(before.has_value());
    auto toldOn = std::optional<DaemonLink const*> {};
    auto daemonWhenTold = std::optional<contour::window::DaemonRef> {};
    auto told = Told {};
    auto completion = told.completion();
    rig.switcher->request(rig.shown(),
                          to(daemonB(), "2"),
                          SwitchMode::Switch,
                          contour::session::UserSwitch,
                          [&](std::expected<SwitchOutcome, std::string> outcome) {
                              toldOn = rig.windowLink();
                              daemonWhenTold = rig.app.manager().daemonRefOf(rig.shown());
                              completion(std::move(outcome));
                          });
    REQUIRE(waitUntil([&] { return told.outcome.has_value(); }, 2000));
    REQUIRE(*told.outcome == SwitchOutcome::Switched);
    REQUIRE(toldOn.has_value());
    CHECK(*toldOn != &oldLink);
    CHECK(*toldOn == rig.windowLink());
    CHECK(daemonWhenTold == rig.app.manager().daemonRefOf(rig.shown()));
    CHECK(daemonWhenTold != before);
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
    CHECK(rig.a.visibilityOf(rig.aWorkspaces[1]) == WorkspaceVisibility::Shown);
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
    CHECK(rig.a.visibilityOf(rig.aWorkspaces[0]) == WorkspaceVisibility::Shown);
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
    rig.switcher->request(
        rig.shown(), to(daemonB(), "2"), SwitchMode::Switch, contour::session::UserSwitch, told.completion());
    // The new link's connection selects B's workspace, and is held before its first layout.
    REQUIRE(waitUntil([&] { return showing(rig.b, rig.bWorkspaces[1]) == 1; }));
    CHECK_FALSE(told.outcome.has_value());

    rig.b.refuseConnections();

    REQUIRE(waitUntil([&] { return told.outcome.has_value(); }, 2000));
    CHECK_FALSE(told.outcome->has_value());
    CHECK(rig.windowLink() == &rig.launchLink());
    CHECK(rig.launchLink().workspaceOf(rig.shown()) == rig.aWorkspaces[0]);
    CHECK(rig.a.visibilityOf(rig.aWorkspaces[0]) == WorkspaceVisibility::Shown);
    CHECK_FALSE(rig.showsNoneOf(before));
    // The link it started gave up: served again, B's workspace is selected by no one.
    rig.b.serveAgain();
    CHECK_FALSE(waitUntil([&] { return showing(rig.b, rig.bWorkspaces[1]) > 0; }, 100));
    // The connection went before it confirmed the layout: B gives the workspace back its record.
    CHECK(rig.b.visibilityOf(rig.bWorkspaces[1]) == WorkspaceVisibility::Hidden);
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
                          contour::session::UserSwitch,
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
    CHECK(waitUntil([&] { return rig.a.visibilityOf(made) == WorkspaceVisibility::Hidden; }, 2000));
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
                          contour::session::UserSwitch,
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
    CHECK(rig.a.visibilityOf(rig.aWorkspaces[0]) == WorkspaceVisibility::Shown);
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
                          contour::session::UserSwitch,
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
    CHECK(waitUntil([&] { return rig.a.visibilityOf(made) == WorkspaceVisibility::Hidden; }, 2000));
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
    rig.switcher->request(
        rig.shown(), to(daemonB(), "2"), SwitchMode::Switch, contour::session::UserSwitch, told.completion());
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
    CHECK(rig.a.visibilityOf(rig.aWorkspaces[0]) == WorkspaceVisibility::Shown);
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
                          contour::session::UserSwitch,
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
    CHECK(rig.a.visibilityOf(rig.aWorkspaces[1]) == WorkspaceVisibility::Shown);
    CHECK_FALSE(rig.window->controller->notices().toastText().contains("Could not switch"));
}

// A workspace holding no tab is no failure to show: the switch keeps waiting for the tab the user opens
// there, however long it takes, and only a lost connection ends it. (A daemon no client attached to yet
// holds one, its own; the listing the switch was resolved from may also be stale, the last tab closed
// since.)
TEST_CASE("WindowSwitcher: a switch to a workspace holding no tab waits for one, with no deadline",
          "[switch][link][timeout]")
{
    auto rig = SwitchRig { DaemonBStart::Fresh };
    auto const before = rig.windowSessions();
    auto const bOwn = rig.b.onDaemon([&] { return rig.b.host->workspaceId().value; });
    rig.lister->staleHoldingTab.insert(bOwn);
    auto told = Told {};
    rig.switcher->request(
        rig.shown(), to(daemonB(), "1"), SwitchMode::Switch, contour::session::UserSwitch, told.completion());
    REQUIRE(waitUntil([&] { return showing(rig.b, bOwn) == 1; }, 2000));
    REQUIRE(rig.timer.armed.size() == 1);
    auto const& link = *rig.links.back();
    REQUIRE(waitUntil([&] { return link.switchWait() == contour::remote::SwitchWait::ForFirstTab; }));

    // The deadline passes, which would fail a switch whose target never shows: nothing fails here.
    rig.timer.fireAll();
    drainGuiEvents();
    CHECK_FALSE(told.outcome.has_value());
    CHECK_FALSE(rig.window->controller->notices().toastText().contains("Could not switch"));
    CHECK(showing(rig.b, bOwn) == 1); // the link it started still holds the workspace

    SECTION("a tab opened there completes the switch, and the window shows it")
    {
        std::ignore = rig.b.seedSession("first");

        REQUIRE(waitUntil([&] { return told.outcome.has_value(); }, 5000));
        CHECK(*told.outcome == SwitchOutcome::Switched);
        REQUIRE(rig.windowLink() != nullptr);
        CHECK(rig.windowLink()->address() == daemonB());
        CHECK(rig.windowLink()->workspaceOf(rig.shown()) == bOwn);
        REQUIRE(waitUntil([&] { return !rig.app.manager().sessionsOfWindow(rig.shown()).empty(); }));
        CHECK(rig.showsNoneOf(before));
    }
    SECTION("a connection lost meanwhile still fails the switch")
    {
        rig.b.dropConnections();

        REQUIRE(waitUntil([&] { return told.outcome.has_value(); }, 5000));
        CHECK_FALSE(told.outcome->has_value());
        CHECK(rig.windowLink() == &rig.launchLink());
        CHECK_FALSE(rig.showsNoneOf(before));
    }
}

// The wait is decided as the target's layout arrives, not when the deadline passes: a deadline that passes
// before any settled layout arrived still fails the switch, on the bound an unsettled target always had.
TEST_CASE("WindowSwitcher: a deadline passing before the target's layout arrived still fails the switch",
          "[switch][link][timeout]")
{
    auto rig = SwitchRig { DaemonBStart::Fresh };
    auto const before = rig.windowSessions();
    auto const bOwn = rig.b.onDaemon([&] { return rig.b.host->workspaceId().value; });
    rig.lister->staleHoldingTab.insert(bOwn);
    auto told = Told {};
    rig.timer.firing = ManualSwitchTimer::Firing::AtOnce;
    rig.switcher->request(
        rig.shown(), to(daemonB(), "1"), SwitchMode::Switch, contour::session::UserSwitch, told.completion());
    drainGuiEvents();

    REQUIRE(told.outcome.has_value());
    CHECK_FALSE(told.outcome->has_value());
    CHECK(told.outcome->error().contains("in time"));
    CHECK(rig.window->controller->notices().toastText().contains("Could not switch"));
    CHECK_FALSE(rig.showsNoneOf(before));
}

// A switch waiting for a tab ends for a newer switch of its window, quietly: the window is not locked by it.
TEST_CASE("WindowSwitcher: a switch waiting for a tab ends for a newer switch of the window",
          "[switch][link][timeout]")
{
    auto rig = SwitchRig { DaemonBStart::Fresh };
    auto const bOwn = rig.b.onDaemon([&] { return rig.b.host->workspaceId().value; });
    rig.lister->staleHoldingTab.insert(bOwn);
    auto waiting = Told {};
    rig.switcher->request(rig.shown(),
                          to(daemonB(), "1"),
                          SwitchMode::Switch,
                          contour::session::UserSwitch,
                          waiting.completion());
    REQUIRE(waitUntil([&] { return showing(rig.b, bOwn) == 1; }, 2000));
    REQUIRE(waitUntil(
        [&] { return rig.links.back()->switchWait() == contour::remote::SwitchWait::ForFirstTab; }));
    rig.timer.fireAll();
    drainGuiEvents();
    REQUIRE_FALSE(waiting.outcome.has_value());

    CHECK(rig.request(to(daemonA(), "2")) == SwitchOutcome::Switched);

    REQUIRE(waiting.outcome.has_value());
    CHECK_FALSE(waiting.outcome->has_value());
    CHECK(waiting.outcome->error().contains("newer switch"));
    CHECK_FALSE(rig.window->controller->notices().toastText().contains("Could not switch"));
    CHECK(rig.windowLink() == &rig.launchLink());
}

// A window that closes while its switch waits for a tab ends that switch: no deadline bounds it any more.
TEST_CASE("WindowSwitcher: the close of a window ends the switch it waits in", "[switch][link][timeout]")
{
    auto rig = SwitchRig { DaemonBStart::Fresh };
    auto const bOwn = rig.b.onDaemon([&] { return rig.b.host->workspaceId().value; });
    rig.lister->staleHoldingTab.insert(bOwn);
    auto told = Told {};
    rig.switcher->request(
        rig.shown(), to(daemonB(), "1"), SwitchMode::Switch, contour::session::UserSwitch, told.completion());
    REQUIRE(waitUntil([&] { return showing(rig.b, bOwn) == 1; }, 2000));
    REQUIRE(waitUntil(
        [&] { return rig.links.back()->switchWait() == contour::remote::SwitchWait::ForFirstTab; }));
    rig.timer.fireAll();
    drainGuiEvents();
    REQUIRE_FALSE(told.outcome.has_value());

    rig.switcher->windowClosed(rig.shown());

    REQUIRE(told.outcome.has_value());
    CHECK_FALSE(told.outcome->has_value());
    CHECK_FALSE(rig.window->controller->notices().toastText().contains("Could not switch"));
    // The window's id is free of it: a closed window's switch does not linger.
    rig.switcher->windowClosed(rig.shown());
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
    CHECK(rig.a.visibilityOf(rig.aWorkspaces[0]) == WorkspaceVisibility::Shown);
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
    CHECK(rig.a.visibilityOf(rig.aWorkspaces[0]) == WorkspaceVisibility::Shown);
    CHECK_FALSE(rig.showsNoneOf(before));

    // Open again: shown in this process already, so that window is focused instead.
    CHECK(rig.request(to(daemonB(), "1"), SwitchMode::Open) == SwitchOutcome::FocusedExisting);
    CHECK(rig.host.opened.size() == 1);
}

namespace
{
/// The first tab of one of a daemon's workspaces, by the daemon's ids.
struct FirstTab
{
    uint64_t tab = 0;     ///< The tab's id on the daemon.
    uint64_t session = 0; ///< The session of its active pane.
};

/// Gives @p daemon's @p workspace a second tab, which the daemon has active from then on.
/// @return Its first tab, which a landing must activate for the check to mean anything.
[[nodiscard]] FirstTab addSecondTab(DaemonFixture& daemon, uint64_t workspace)
{
    auto const id = vtworkspace::WorkspaceId { workspace };
    auto const first = daemon.onDaemon([&] {
        auto const* const tab = daemon.host->model().workspace(id)->tabAt(0);
        return FirstTab { .tab = tab->id().value, .session = tab->activePane()->session().value };
    });
    daemon.onDaemon([&] {
        std::ignore = daemon.host->createTab({}, vtworkspace::SessionId { first.session });
        return 0;
    });
    REQUIRE(daemon.onDaemon(
        [&] { return daemon.host->model().workspace(id)->activeTab()->id().value != first.tab; }));
    return first;
}

/// Waits until @p window shows two tabs.
/// @return Whether it does, with its first tab active and @p first's pane its active one.
[[nodiscard]] bool showsFirstTab(contour::session::TerminalSessionManager& manager,
                                 contour::test::ScopedController const& window,
                                 FirstTab first)
{
    auto const twoTabs = waitUntil([&] {
        auto const* const shown = manager.model().workspace(window.id);
        return shown != nullptr && shown->tabCount() == 2;
    });
    drainGuiEvents();
    auto const* const shown = manager.model().workspace(window.id);
    auto const* const active = window->activeSession();
    return twoTabs && shown != nullptr && shown->indexOf(shown->activeTab()->id()) == 0 && active != nullptr
           && manager.paneKeyOf(active->modelSessionId()) == contour::window::PaneKey { first.session };
}
} // namespace

TEST_CASE("WindowSwitcher: Open lands on the row's pane in the new window", "[switch][link][jump]")
{
    auto rig = SwitchRig {};
    auto const first = addSecondTab(rig.b, rig.bWorkspaces[0]);
    auto target = to(daemonB(), "1");
    target.focus = contour::session::LandingFocus { .tab = first.tab,
                                                    .pane = contour::window::PaneKey { first.session } };

    REQUIRE(rig.request(target, SwitchMode::Open) == SwitchOutcome::Switched);

    REQUIRE(rig.host.opened.size() == 1);
    CHECK(showsFirstTab(rig.app.manager(), *rig.host.opened.front(), first));
}

TEST_CASE("WindowSwitcher: Open of a workspace another window shows raises it and activates the tab",
          "[switch][link][jump]")
{
    auto rig = SwitchRig {};
    auto const first = addSecondTab(rig.b, rig.bWorkspaces[0]);
    REQUIRE(rig.request(to(daemonB(), "1"), SwitchMode::Open) == SwitchOutcome::Switched);
    REQUIRE(rig.host.opened.size() == 1);
    auto& manager = rig.app.manager();
    auto const& other = *rig.host.opened.front();
    // The daemon's active tab, the second, is what the new window activated, as nothing named another.
    REQUIRE(waitUntil([&] {
        auto const* const shown = manager.model().workspace(other.id);
        return shown != nullptr && shown->tabCount() == 2 && shown->indexOf(shown->activeTab()->id()) == 1;
    }));

    auto target = to(daemonB(), "1");
    target.focus = contour::session::LandingFocus { .tab = first.tab,
                                                    .pane = contour::window::PaneKey { first.session } };
    CHECK(rig.request(target, SwitchMode::Open) == SwitchOutcome::FocusedExisting);

    CHECK(rig.host.opened.size() == 1);
    CHECK(showsFirstTab(manager, other, first));
    // The window that asked stays where it was.
    CHECK(rig.launchLink().workspaceOf(rig.shown()) == rig.aWorkspaces[0]);
}

TEST_CASE("WindowSwitcher: a switch within the daemon lands on the row's tab", "[switch][link][jump]")
{
    // The window shows the selected workspace in a local workspace of its own: the landing finds it there.
    auto rig = SwitchRig {};
    auto const first = addSecondTab(rig.a, rig.aWorkspaces[1]);
    auto target = to(daemonA(), "2");
    target.focus = contour::session::LandingFocus { .tab = first.tab, .pane = std::nullopt };

    REQUIRE(rig.request(target) == SwitchOutcome::Switched);

    drainGuiEvents();
    auto const* const shown = rig.app.manager().model().workspace(rig.shown());
    REQUIRE(shown != nullptr);
    REQUIRE(shown->tabCount() == 2);
    CHECK(shown->indexOf(shown->activeTab()->id()) == 0);
}

namespace
{
/// A split tab of a daemon's, by the daemon's ids.
struct SplitTab
{
    uint64_t tab = 0;    ///< The tab's id on the daemon.
    uint64_t first = 0;  ///< The session of the pane listed first.
    uint64_t active = 0; ///< The session of its active pane, listed last.
};

/// Splits the tab of A's second workspace, its active pane made the one listed last: not the one listed
/// first, which a landing on the tab must not show.
[[nodiscard]] SplitTab splitSecondWorkspaceOfA(SwitchRig& rig)
{
    auto const id = vtworkspace::WorkspaceId { rig.aWorkspaces[1] };
    auto const split = rig.a.onDaemon([&] {
        auto* const tab = rig.a.host->model().workspace(id)->tabAt(0);
        rig.a.host->splitActivePane(tab->id(), vtworkspace::SplitState::Vertical, 0.5);
        // Its active pane made the one listed last, whichever the split made active.
        auto leaves = std::vector<vtworkspace::Pane*> {};
        tab->rootPane()->walkTree([&](vtworkspace::Pane& pane) {
            if (pane.isLeaf())
                leaves.push_back(&pane);
        });
        rig.a.host->model().setActivePane(tab->id(), leaves.back()->id());
        return SplitTab { .tab = tab->id().value,
                          .first = leaves.front()->session().value,
                          .active = tab->activePane()->session().value };
    });
    REQUIRE(split.active != split.first);
    return split;
}
} // namespace

TEST_CASE("WindowSwitcher: a landing naming a tab alone shows that tab's active pane, not its first",
          "[switch][link][jump]")
{
    auto rig = SwitchRig {};
    // A's second workspace's tab, split, its active pane not the one listed first.
    auto const split = splitSecondWorkspaceOfA(rig);
    auto target = to(daemonA(), "2");
    target.focus = contour::session::LandingFocus { .tab = split.tab, .pane = std::nullopt };

    REQUIRE(rig.request(target) == SwitchOutcome::Switched);

    auto& manager = rig.app.manager();
    auto const showsActive = [&] {
        drainGuiEvents();
        auto const* const active = rig.window->controller->activeSession();
        return active != nullptr
               && manager.paneKeyOf(active->modelSessionId()) == contour::window::PaneKey { split.active };
    };
    // At once: the landing is what the switch was told after.
    {
        auto const directory = rig.window->controller->directorySource()->directory();
        auto const listed = contour::window::findTab(directory, rig.aWorkspaces[1], split.tab);
        CHECK(directory.selected == rig.aWorkspaces[1]);
        REQUIRE(listed.has_value());
        CHECK(listed->tab->panes.size() == 2);
        CHECK(listed->tab->panes.front().key.value == split.first);
        auto const* const active = rig.window->controller->activeSession();
        REQUIRE(active != nullptr);
        CHECK(manager.paneKeyOf(active->modelSessionId()).value == split.active);
    }
    CHECK(showsActive());
}

namespace
{
/// @return The model row of the tab @p tab of the daemon's @p workspace in @p window's sidebar, its workspace
///         unfolded first.
[[nodiscard]] int sidebarTabRow(contour::window::WindowController& window, uint64_t workspace, uint64_t tab)
{
    auto& sidebar = window.sidebar();
    sidebar.refreshNow();
    auto const rowOf = [&](contour::window::SidebarRowKind kind) {
        auto const& rows = sidebar.rows();
        auto const at = std::ranges::find_if(rows, [&](contour::window::SidebarRow const& row) {
            return row.key.kind == kind && row.key.workspace == workspace
                   && (kind != contour::window::SidebarRowKind::Tab || row.key.tab == tab);
        });
        return at != rows.end() ? static_cast<int>(std::distance(rows.begin(), at)) : -1;
    };
    if (rowOf(contour::window::SidebarRowKind::Tab) < 0)
    {
        auto const holder = rowOf(contour::window::SidebarRowKind::Workspace);
        REQUIRE(holder >= 0);
        sidebar.toggle(holder);
        sidebar.refreshNow();
    }
    return rowOf(contour::window::SidebarRowKind::Tab);
}

/// @return The action id of the in-scene menu row titled @p title; none for none.
[[nodiscard]] std::optional<int> menuActionIdOf(QVariantList const& rows, QString const& title)
{
    for (auto const& value: rows)
    {
        auto const row = value.toMap();
        if (row["kind"].toString() == "command" && row["title"].toString() == title)
            return row["actionId"].toInt();
    }
    return std::nullopt;
}
} // namespace

TEST_CASE("WindowSwitcher: Open from a tab row activates that tab in the new window",
          "[switch][sidebar][jump]")
{
    auto rig = SwitchRig {};
    auto const first = addSecondTab(rig.a, rig.aWorkspaces[1]);
    auto& window = *rig.window->controller;
    auto& manager = rig.app.manager();
    REQUIRE(waitUntil([&] {
        auto const* const source = window.directorySource();
        return source != nullptr && source->directory().workspaces.size() == 2
               && source->directory().workspaces[1].tabs.size() == 2;
    }));
    auto const row = sidebarTabRow(window, rig.aWorkspaces[1], first.tab);
    REQUIRE(row >= 0);
    auto const before = rig.windowSessions();

    window.sidebar().openContextMenu(row, QPointF {});
    auto const open = menuActionIdOf(window.sidebarContextMenuModel(), QStringLiteral("Open in New Window"));
    REQUIRE(open.has_value());
    window.triggerSidebarContextMenuAction(*open);

    // Asked as a new window, for the row's tab.
    auto const& requests = rig.app.switching().requests;
    REQUIRE(requests.size() == 1);
    CHECK(requests.front().mode == SwitchMode::Open);
    CHECK(requests.front().target.focus.tab == first.tab);
    REQUIRE(waitUntil([&] { return rig.host.opened.size() == 1; }));
    CHECK(showsFirstTab(manager, *rig.host.opened.front(), first));

    // Open from the menu never shows the row first: this window stays where it was.
    CHECK(rig.launchLink().workspaceOf(rig.shown()) == rig.aWorkspaces[0]);
    CHECK_FALSE(window.rowActionPending());
    CHECK_FALSE(rig.showsNoneOf(before));
}

namespace
{
/// @return The model row of the first row of @p kind in @p window's sidebar that @p matches; -1 for none.
[[nodiscard]] int sidebarRowWhere(contour::window::WindowController& window,
                                  std::function<bool(contour::window::SidebarRowKey const&)> const& matches)
{
    auto& sidebar = window.sidebar();
    sidebar.refreshNow();
    auto const& rows = sidebar.rows();
    auto const at = std::ranges::find_if(rows, [&](auto const& row) { return matches(row.key); });
    return at != rows.end() ? static_cast<int>(std::distance(rows.begin(), at)) : -1;
}

/// @return Whether @p said said @p message last.
[[nodiscard]] bool saidLast(contour::test::RecordingAnnouncer const* said, QString const& message)
{
    return said != nullptr && !said->said.empty() && said->said.back().message == message;
}
} // namespace

TEST_CASE("WindowSwitcher: a real window's Open in New Window is ended by its daemon row's menu, not by Esc",
          "[switch][sidebar][jump]")
{
    // The window's routing, end to end: the window id its cancel names, the mode of the jump it ends, and
    // what its sidebar says. Esc and the location chip speak for a jump in this window alone.
    auto rig = SwitchRig {};
    auto& window = *rig.window->controller;
    auto const ownDaemon = rig.app.manager().daemonRefOf(rig.shown());
    REQUIRE(ownDaemon.has_value());
    REQUIRE(waitUntil([&] {
        auto const* const source = window.directorySource();
        return source != nullptr && source->directory().workspaces.size() == 2;
    }));
    // Open in New Window from A's second workspace's row, its new link held before it shows anything.
    rig.a.holdNewConnectionsAtLayout();
    auto const row = sidebarRowWhere(window, [&](auto const& key) {
        return key.kind == contour::window::SidebarRowKind::Workspace && key.workspace == rig.aWorkspaces[1];
    });
    REQUIRE(row >= 0);
    window.sidebar().openContextMenu(row, QPointF {});
    auto const open = menuActionIdOf(window.sidebarContextMenuModel(), QStringLiteral("Open in New Window"));
    REQUIRE(open.has_value());
    window.triggerSidebarContextMenuAction(*open);
    REQUIRE(waitUntil([&] { return rig.host.started == 1; }, 2000));
    CHECK_FALSE(window.sidebar().isJumpPending());

    window.cancelSidebarJump();
    CHECK(rig.app.switching().cancels.empty());

    auto const daemonRow = sidebarRowWhere(window, [&](auto const& key) {
        return key.kind == contour::window::SidebarRowKind::Daemon && key.daemon == *ownDaemon;
    });
    REQUIRE(daemonRow >= 0);
    window.sidebar().openContextMenu(daemonRow, QPointF {});
    auto const cancel = menuActionIdOf(window.sidebarContextMenuModel(), QStringLiteral("Cancel Jump"));
    REQUIRE(cancel.has_value());
    window.triggerSidebarContextMenuAction(*cancel);

    CHECK(rig.app.switching().cancels == std::vector { rig.shown() });
    CHECK(saidLast(rig.said, QStringLiteral("Jump cancelled")));
    // The Open it ended opens nothing once its daemon answers.
    rig.a.releaseHeld();
    drainGuiEvents();
    CHECK(rig.host.opened.empty());
}

TEST_CASE("WindowSwitcher: a real window's row of its own daemon's ended run asks no switch",
          "[switch][sidebar][jump]")
{
    auto rig = SwitchRig {};
    auto& window = *rig.window->controller;
    auto& manager = rig.app.manager();
    REQUIRE(waitUntil([&] {
        auto const* const source = window.directorySource();
        return source != nullptr && source->directory().workspaces.size() == 2;
    }));
    auto const row = sidebarRowWhere(window, [&](auto const& key) {
        return key.kind == contour::window::SidebarRowKind::Workspace && key.workspace == rig.aWorkspaces[1];
    });
    REQUIRE(row >= 0);
    window.sidebar().openContextMenu(row, QPointF {});
    auto const open = menuActionIdOf(window.sidebarContextMenuModel(), QStringLiteral("Open in New Window"));
    REQUIRE(open.has_value());
    auto const before = manager.daemonInstanceOf(rig.shown());
    REQUIRE(before.has_value());

    // The daemon restarts while the menu is open: the row names a run that has ended.
    rig.a.restartHost();
    REQUIRE(waitUntil(
        [&] {
            auto const now = manager.daemonInstanceOf(rig.shown());
            return now.has_value() && *now != *before;
        },
        2000));
    window.triggerSidebarContextMenuAction(*open);

    CHECK(rig.host.started == 0);
    CHECK_FALSE(window.sidebar().isJumpPending());
    REQUIRE(rig.said != nullptr);
    CHECK(std::ranges::any_of(rig.said->said, [](auto const& said) {
        return said.message.contains(QStringLiteral("the daemon restarted"));
    }));
}

// {{{ Other daemons' rows, through a real window's sidebar
namespace
{
/// B, as the window's sidebar lists it: the saved host `b`.
[[nodiscard]] contour::window::DaemonRef sidebarB()
{
    return contour::window::hostDaemonRef("b");
}

/// B as the saved host the sidebar lists, for a switch to land on the daemon its rows name.
[[nodiscard]] DaemonAddress hostB()
{
    return DaemonAddress { .kind = SwitchDaemonKind::Host, .name = "b" };
}

/// @return B's section as the window's sidebar reads it, once it lists @p workspace; fails the case
/// otherwise.
[[nodiscard]] contour::window::DaemonSection awaitSectionB(SwitchRig& rig, uint64_t workspace)
{
    auto const listed = [&] {
        auto const sections = rig.presence().sections();
        auto const found = std::ranges::find(sections, sidebarB(), &contour::window::DaemonSection::daemon);
        return found != sections.end() && found->state == contour::window::DaemonLinkState::Connected
                       && contour::window::findWorkspace(found->directory, workspace) != nullptr
                   ? std::optional { *found }
                   : std::nullopt;
    };
    REQUIRE(waitUntil([&] { return listed().has_value(); }, 3000));
    return *listed();
}

/// @return The model row of B's row of @p kind naming @p workspace (and @p tab; a pane row, the tab's first),
///         its parents unfolded.
[[nodiscard]] int rowOfB(contour::window::WindowController& window,
                         contour::window::SidebarRowKind kind,
                         uint64_t workspace,
                         uint64_t tab = 0)
{
    using contour::window::SidebarRowKind;
    auto& sidebar = window.sidebar();
    auto const find = [&](SidebarRowKind wanted) {
        sidebar.refreshNow();
        auto const& rows = sidebar.rows();
        auto const at = std::ranges::find_if(rows, [&](contour::window::SidebarRow const& row) {
            return row.key.daemon == sidebarB() && row.key.kind == wanted && row.key.workspace == workspace
                   && (wanted == SidebarRowKind::Workspace || row.key.tab == tab);
        });
        return at != rows.end() ? static_cast<int>(std::distance(rows.begin(), at)) : -1;
    };
    if (find(SidebarRowKind::Workspace) < 0)
    {
        auto const& rows = sidebar.rows();
        auto const daemonAt = std::ranges::find_if(rows, [&](contour::window::SidebarRow const& row) {
            return row.key.daemon == sidebarB() && row.key.kind == SidebarRowKind::Daemon;
        });
        REQUIRE(daemonAt != rows.end());
        sidebar.expand(static_cast<int>(std::distance(rows.begin(), daemonAt)));
    }
    if (kind != SidebarRowKind::Workspace && find(SidebarRowKind::Tab) < 0)
    {
        auto const holder = find(SidebarRowKind::Workspace);
        REQUIRE(holder >= 0);
        sidebar.expand(holder);
    }
    if (kind == SidebarRowKind::Pane && find(SidebarRowKind::Pane) < 0)
    {
        auto const holder = find(SidebarRowKind::Tab);
        REQUIRE(holder >= 0);
        sidebar.expand(holder);
    }
    return find(kind);
}

/// @return The action id of the command row titled @p title in the submenu titled @p submenu; none for none.
[[nodiscard]] std::optional<int> submenuActionIdOf(QVariantList const& rows,
                                                   QString const& submenu,
                                                   QString const& title)
{
    for (auto const& value: rows)
    {
        auto const row = value.toMap();
        if (row["kind"].toString() == "submenu" && row["title"].toString() == submenu)
            return menuActionIdOf(row["children"].toList(), title);
    }
    return std::nullopt;
}

/// @return The close node a close confirmation request carries (its first argument).
[[nodiscard]] contour::window::CloseNode closeNodeOf(QList<QVariant> const& request)
{
    auto const node = request.at(0);
    REQUIRE(node.metaType() == QMetaType::fromType<contour::window::CloseNode>());
    return node.value<contour::window::CloseNode>();
}

/// @return The row key a colour pick request carries.
[[nodiscard]] contour::window::SidebarRowKey rowKeyOf(QVariant const& key)
{
    REQUIRE(key.metaType() == QMetaType::fromType<contour::window::SidebarRowKey>());
    return key.value<contour::window::SidebarRowKey>();
}

/// Opens @p window's sidebar menu over @p row and picks its row titled @p title.
void pickFromMenu(contour::window::WindowController& window, int row, QString const& title)
{
    REQUIRE(row >= 0);
    window.sidebar().openContextMenu(row, QPointF {});
    auto const id = menuActionIdOf(window.sidebarContextMenuModel(), title);
    REQUIRE(id.has_value());
    window.triggerSidebarContextMenuAction(*id);
}
} // namespace

namespace
{
/// @return The model row of @p window's sidebar row of @p kind naming @p daemon; -1 for none.
[[nodiscard]] int rowOfKind(contour::window::WindowController& window,
                            contour::window::SidebarRowKind kind,
                            contour::window::DaemonRef const& daemon = {})
{
    window.sidebar().refreshNow();
    auto const& rows = window.sidebar().rows();
    auto const at = std::ranges::find_if(rows, [&](contour::window::SidebarRow const& row) {
        return row.key.kind == kind
               && (kind == contour::window::SidebarRowKind::Hidden || row.key.daemon == daemon);
    });
    return at != rows.end() ? static_cast<int>(std::distance(rows.begin(), at)) : -1;
}
} // namespace

TEST_CASE(
    "WindowSwitcher: Hide shows a toast whose Undo restores the daemon, and the hidden row shows it again",
    "[switch][sidebar][presence]")
{
    using contour::window::SidebarRowKind;
    auto rig = SwitchRig { RigListing::WithB };
    rig.presence().linkSetChanged();
    std::ignore = awaitSectionB(rig, rig.bWorkspaces[1]);
    auto& window = *rig.window->controller;
    auto const hiddenByUser = [&] {
        auto const state = rig.app.guiState().load();
        return state ? state->sidebarDaemons.hidden : std::set<std::string> {};
    };
    auto const before = rig.app.guiState().load()->sidebarDaemons;

    pickFromMenu(
        window, rowOfKind(window, SidebarRowKind::Daemon, sidebarB()), QStringLiteral("Hide from Sidebar"));

    CHECK(hiddenByUser() == std::set<std::string> { sidebarB().value });
    CHECK(rowOfKind(window, SidebarRowKind::Daemon, sidebarB()) < 0);
    REQUIRE(rowOfKind(window, SidebarRowKind::Hidden) >= 0);
    CHECK(window.sidebar()
              .rows()[static_cast<std::size_t>(rowOfKind(window, SidebarRowKind::Hidden))]
              .hidden.size()
          == 1);
    auto& notices = window.notices();
    CHECK(notices.toastText() == QStringLiteral("b hidden from the sidebar"));
    CHECK(notices.toastActionText() == QStringLiteral("Undo"));

    SECTION("the toast's Undo")
    {
        notices.triggerToastAction();
    }
    SECTION("Show from the hidden row's menu")
    {
        pickFromMenu(window, rowOfKind(window, SidebarRowKind::Hidden), QStringLiteral("Show b"));
    }

    CHECK(rig.app.guiState().load()->sidebarDaemons == before);
    CHECK(rowOfKind(window, SidebarRowKind::Hidden) < 0);
    CHECK(waitUntil([&] { return rowOfKind(window, SidebarRowKind::Daemon, sidebarB()) >= 0; }, 3000));
}

TEST_CASE("WindowSwitcher: a drag from another daemon's row does not start", "[switch][sidebar][jump]")
{
    auto rig = SwitchRig { RigListing::WithB };
    rig.presence().linkSetChanged();
    std::ignore = awaitSectionB(rig, rig.bWorkspaces[1]);
    auto& window = *rig.window->controller;
    using contour::window::SidebarRowKind;
    CHECK(window.beginSidebarDrag(rowOfB(window, SidebarRowKind::Workspace, rig.bWorkspaces[1])).isEmpty());
    auto const section = awaitSectionB(rig, rig.bWorkspaces[1]);
    auto const tab = contour::window::findWorkspace(section.directory, rig.bWorkspaces[1])->tabs.front().id;
    CHECK(window.beginSidebarDrag(rowOfB(window, SidebarRowKind::Tab, rig.bWorkspaces[1], tab)).isEmpty());
}

TEST_CASE("WindowSwitcher: closing another daemon's tab names the daemon in the confirmation",
          "[switch][sidebar][jump]")
{
    auto rig = SwitchRig { RigListing::WithB };
    rig.presence().linkSetChanged();
    auto& window = *rig.window->controller;
    auto const section = awaitSectionB(rig, rig.bWorkspaces[1]);
    auto const tab = contour::window::findWorkspace(section.directory, rig.bWorkspaces[1])->tabs.front().id;
    auto const sessionsBefore = rig.app.manager().sessionsOfWindow(rig.shown()).size();
    auto asked =
        QSignalSpy { &window, &contour::window::WindowController::sidebarCloseConfirmationRequested };

    pickFromMenu(window,
                 rowOfB(window, contour::window::SidebarRowKind::Tab, rig.bWorkspaces[1], tab),
                 QStringLiteral("Close Tab…"));

    REQUIRE(asked.count() == 1);
    auto const node = closeNodeOf(asked.front());
    auto const workspace = node.row.workspace;
    auto const askedTab = node.row.tab;
    CHECK(node.kind == contour::window::CloseKind::Tab);
    CHECK(node.row.daemon == sidebarB());
    CHECK(workspace == rig.bWorkspaces[1]);
    CHECK(askedTab == tab);
    auto const text = window.closeConfirmation(asked.front()[0], QString {}, QString {});
    CHECK(text["message"].toString().endsWith(QStringLiteral("It runs on b.")));

    window.confirmClose(asked.front()[0]);
    CHECK(waitUntil([&] {
        auto const sections = rig.presence().sections();
        auto const found = std::ranges::find(sections, sidebarB(), &contour::window::DaemonSection::daemon);
        return found != sections.end() && !contour::window::findTab(found->directory, workspace, askedTab);
    }));
    // The window's own daemon's tabs are as they were, and the window still shows what it showed.
    drainGuiEvents();
    CHECK(rig.app.manager().sessionsOfWindow(rig.shown()).size() == sessionsBefore);
    CHECK(rig.launchLink().workspaceOf(rig.shown()) == rig.aWorkspaces[0]);
    CHECK(rig.app.switching().requests.empty());
}

TEST_CASE("WindowSwitcher: Split on another daemon's tab jumps there, then splits that tab",
          "[switch][sidebar][jump]")
{
    auto rig = SwitchRig { RigListing::WithB };
    rig.presence().linkSetChanged();
    auto& window = *rig.window->controller;
    auto const section = awaitSectionB(rig, rig.bWorkspaces[1]);
    auto const tab = contour::window::findWorkspace(section.directory, rig.bWorkspaces[1])->tabs.front().id;

    pickFromMenu(window,
                 rowOfB(window, contour::window::SidebarRowKind::Tab, rig.bWorkspaces[1], tab),
                 QStringLiteral("Split Vertically"));

    // A jump in place first: the window is on B's workspace, and then that tab is split there.
    auto const& requests = rig.app.switching().requests;
    REQUIRE(requests.size() == 1);
    CHECK(requests.front().mode == SwitchMode::Switch);
    REQUIRE(waitUntil([&] { return rig.app.manager().daemonRefOf(rig.shown()) == sidebarB(); }, 3000));
    CHECK(waitUntil(
        [&] {
            auto const* const source = window.directorySource();
            if (source == nullptr)
                return false;
            auto const directory = source->directory();
            auto const listed = contour::window::findTab(directory, rig.bWorkspaces[1], tab);
            return directory.selected == rig.bWorkspaces[1] && listed && listed->tab->panes.size() == 2;
        },
        3000));
}

namespace
{
/// A program status OSC that marks its pane Blocked: it asks the user.
constexpr auto BlockedStatus = std::string_view { "\033]7501;state=blocked:id=agent\033\\" };

/// @return Whether the window of @p rig is on B.
[[nodiscard]] bool onB(SwitchRig& rig)
{
    return rig.app.manager().daemonRefOf(rig.shown()) == sidebarB();
}

/// @return The panes of @p directory's workspace @p workspace it lists Blocked, in its order.
[[nodiscard]] std::vector<contour::window::PaneKey> blockedPanesIn(
    contour::window::WorkspaceDirectory const& directory, uint64_t workspace)
{
    auto keys = std::vector<contour::window::PaneKey> {};
    if (auto const* const listed = contour::window::findWorkspace(directory, workspace))
        for (auto const& tab: listed->tabs)
            for (auto const& pane: tab.panes)
                if (std::ranges::any_of(pane.status, [](auto const& record) {
                        return record.state == vtbackend::ProgramState::Blocked;
                    }))
                    keys.push_back(pane.key);
    return keys;
}

/// @return The panes of B's workspace @p workspace the window's sidebar lists Blocked, in its order.
[[nodiscard]] std::vector<contour::window::PaneKey> blockedPanesOfB(SwitchRig& rig, uint64_t workspace)
{
    return blockedPanesIn(awaitSectionB(rig, workspace).directory, workspace);
}

/// @return The pane @p window has active; none for none.
[[nodiscard]] std::optional<contour::window::PaneKey> activePaneOf(SwitchRig& rig,
                                                                   contour::window::WindowController& window)
{
    auto const* const active = window.activeSession();
    return active != nullptr ? std::optional { rig.app.manager().paneKeyOf(active->modelSessionId()) }
                             : std::nullopt;
}

/// @return Whether @p window is on B with @p pane active.
[[nodiscard]] bool showsPaneOfB(SwitchRig& rig,
                                contour::window::WindowController& window,
                                contour::window::PaneKey pane)
{
    return onB(rig) && activePaneOf(rig, window) == pane;
}

/// Has B's pane of @p workspace go Blocked, and waits for the window's sidebar to list it so.
/// @return Its pane key.
[[nodiscard]] contour::window::PaneKey blockPaneOfB(SwitchRig& rig, uint64_t workspace)
{
    printInPaneOf(rig.b, workspace, std::string { BlockedStatus });
    REQUIRE(waitUntil([&] { return !blockedPanesOfB(rig, workspace).empty(); }, 3000));
    return blockedPanesOfB(rig, workspace).front();
}

/// @return The row of A's workspace @p workspace in @p window's sidebar; negative for none.
[[nodiscard]] int ownWorkspaceRow(contour::window::WindowController& window, uint64_t workspace)
{
    return sidebarRowWhere(window, [&](auto const& key) {
        return key.kind == contour::window::SidebarRowKind::Workspace && key.workspace == workspace
               && key.daemon != sidebarB();
    });
}
} // namespace

TEST_CASE("JumpToNextAttention lands on another daemon's blocked pane", "[switch][sidebar][attention][jump]")
{
    auto rig = SwitchRig { RigListing::WithB };
    rig.presence().linkSetChanged();
    auto& window = *rig.window->controller;
    auto const pane = blockPaneOfB(rig, rig.bWorkspaces[1]);

    window.jumpToNextAttention();

    auto const& requests = rig.app.switching().requests;
    REQUIRE(requests.size() == 1);
    CHECK(requests.front().mode == SwitchMode::Switch);
    REQUIRE(waitUntil([&] { return onB(rig); }, 3000));
    CHECK(waitUntil(
        [&] {
            auto const* const source = window.directorySource();
            auto const* const active = window.activeSession();
            return source != nullptr && source->directory().selected == rig.bWorkspaces[1]
                   && active != nullptr && rig.app.manager().paneKeyOf(active->modelSessionId()) == pane;
        },
        3000));
}

TEST_CASE("an own-row activation while a jump connects ends that jump", "[switch][sidebar][attention][jump]")
{
    auto rig = SwitchRig { RigListing::WithB };
    rig.presence().linkSetChanged();
    auto& window = *rig.window->controller;
    std::ignore = blockPaneOfB(rig, rig.bWorkspaces[1]);
    // B's new link is held before it shows anything: the jump stays connecting.
    rig.b.holdNewConnectionsAtLayout();
    window.jumpToNextAttention();
    REQUIRE(waitUntil([&] { return rig.host.started == 1; }, 2000));
    REQUIRE(window.sidebar().isJumpPending());
    auto const saidBefore = rig.said != nullptr ? rig.said->said.size() : std::size_t { 0 };

    auto const own = ownWorkspaceRow(window, rig.aWorkspaces[1]);
    REQUIRE(own >= 0);
    window.sidebar().activate(own);

    CHECK(rig.app.switching().cancels == std::vector { rig.shown() });
    CHECK_FALSE(window.sidebar().isJumpPending());
    rig.b.releaseHeld();
    drainGuiEvents();
    // The window stays on A, on the workspace the newer navigation chose, and the ended jump said nothing.
    CHECK(rig.windowLink() == &rig.launchLink());
    CHECK(waitUntil([&] { return rig.launchLink().workspaceOf(rig.shown()) == rig.aWorkspaces[1]; }, 3000));
    CHECK((rig.said == nullptr || rig.said->said.size() == saidBefore));
}

// Carry-over (Task 26, correction 22): a second press while a jump connects goes on from where that jump
// goes.
TEST_CASE("JumpToNextAttention while a jump connects goes on from that jump's pane",
          "[switch][sidebar][attention][jump]")
{
    auto rig = SwitchRig { RigListing::WithB };
    rig.presence().linkSetChanged();
    auto& window = *rig.window->controller;
    auto const first = blockPaneOfB(rig, rig.bWorkspaces[0]);
    auto const second = blockPaneOfB(rig, rig.bWorkspaces[1]);
    rig.b.holdNewConnectionsAtLayout();

    window.jumpToNextAttention();
    auto const& requests = rig.app.switching().requests;
    REQUIRE(requests.size() == 1);
    REQUIRE(window.sidebar().isJumpPending());
    auto const firstAsked = requests.front().target.focus.pane;
    REQUIRE(firstAsked.has_value());
    REQUIRE((*firstAsked == first || *firstAsked == second));

    // The window still shows A's pane: from it, the first would be asked again.
    window.jumpToNextAttention();
    REQUIRE(requests.size() == 2);
    CHECK(requests.back().target.focus.pane.has_value());
    CHECK(requests.back().target.focus.pane != firstAsked);
    rig.b.releaseHeld();
}

// Phase 7 review I3: once the window is on B, a press goes on to a pane of B's other workspace along the
// window's own link, with no new switch -- the link a jump lands on carries every pane's program status.
namespace
{
/// @return The panes of workspace @p workspace that @p window's own directory -- not the presence's section
///         of its daemon -- lists Blocked.
[[nodiscard]] std::vector<contour::window::PaneKey> ownBlockedPanes(contour::window::WindowController& window,
                                                                    uint64_t workspace)
{
    auto const* const source = window.directorySource();
    return source != nullptr ? blockedPanesIn(source->directory(), workspace)
                             : std::vector<contour::window::PaneKey> {};
}

/// Presses JumpToNextAttention until @p window shows one of @p candidates on B.
/// @return The pane it landed on.
[[nodiscard]] contour::window::PaneKey landOnB(SwitchRig& rig,
                                               contour::window::WindowController& window,
                                               std::span<contour::window::PaneKey const> candidates)
{
    window.jumpToNextAttention();
    REQUIRE(waitUntil(
        [&] {
            return std::ranges::any_of(candidates,
                                       [&](auto const pane) { return showsPaneOfB(rig, window, pane); });
        },
        3000));
    return *activePaneOf(rig, window);
}

/// Presses JumpToNextAttention twice on B: on to @p other, of B's workspace @p workspace, then back to
/// @p landed. Neither press asks for a switch.
void triageOnB(SwitchRig& rig,
               contour::window::WindowController& window,
               contour::window::PaneKey landed,
               contour::window::PaneKey other,
               uint64_t workspace)
{
    auto const requests = rig.app.switching().requests.size();
    window.jumpToNextAttention();
    CHECK(waitUntil([&] { return showsPaneOfB(rig, window, other); }, 3000));
    CHECK(window.directorySource()->directory().selected == workspace);
    window.jumpToNextAttention();
    CHECK(waitUntil([&] { return showsPaneOfB(rig, window, landed); }, 3000));
    CHECK(rig.app.switching().requests.size() == requests);
}
} // namespace

TEST_CASE("JumpToNextAttention on B goes on to B's other workspace", "[switch][sidebar][attention][jump]")
{
    auto rig = SwitchRig { RigListing::WithB };
    rig.presence().linkSetChanged();
    auto& window = *rig.window->controller;
    auto const blocked =
        std::array { blockPaneOfB(rig, rig.bWorkspaces[0]), blockPaneOfB(rig, rig.bWorkspaces[1]) };
    auto const landed = landOnB(rig, window, blocked);
    auto const at = landed == blocked[0] ? std::size_t { 1 } : std::size_t { 0 };
    // This rig's pool has no observer to hand over: the jump dialled B afresh, and the status of the panes
    // the window does not show comes with the connection's first coalesced flush, after the layout it landed
    // on.
    REQUIRE(waitUntil(
        [&] { return std::ranges::contains(ownBlockedPanes(window, rig.bWorkspaces[at]), blocked[at]); },
        3000));
    triageOnB(rig, window, landed, blocked[at], rig.bWorkspaces[at]);
}

TEST_CASE("JumpToNextAttention on B goes on to a pane of B's other workspace blocked after the landing",
          "[switch][sidebar][attention][jump]")
{
    auto rig = SwitchRig { RigListing::WithB };
    rig.presence().linkSetChanged();
    auto& window = *rig.window->controller;
    auto const first = blockPaneOfB(rig, rig.bWorkspaces[0]);
    auto const landed = landOnB(rig, window, std::span { &first, 1 });

    auto const later = blockPaneOfB(rig, rig.bWorkspaces[1]);
    REQUIRE(waitUntil(
        [&] { return std::ranges::contains(ownBlockedPanes(window, rig.bWorkspaces[1]), later); }, 3000));
    triageOnB(rig, window, landed, later, rig.bWorkspaces[1]);
}

TEST_CASE("JumpToNextAttention on B goes on to B's other workspace, on the observer the jump adopted",
          "[switch][sidebar][attention][jump]")
{
    auto rig = SwitchRig { RigListing::WithB, SwitcherPool::Presence };
    rig.presence().linkSetChanged();
    auto& window = *rig.window->controller;
    auto const blocked =
        std::array { blockPaneOfB(rig, rig.bWorkspaces[0]), blockPaneOfB(rig, rig.bWorkspaces[1]) };
    auto const landed = landOnB(rig, window, blocked);
    // As production's: the jump took the presence's observer of B over, which held every pane's status.
    CHECK(rig.host.adopted == 1);
    auto const at = landed == blocked[0] ? std::size_t { 1 } : std::size_t { 0 };
    // So the other workspace's pane is listed Blocked the moment the window lands, with nothing to wait for.
    CHECK(std::ranges::contains(ownBlockedPanes(window, rig.bWorkspaces[at]), blocked[at]));
    triageOnB(rig, window, landed, blocked[at], rig.bWorkspaces[at]);
}

TEST_CASE("Open in New Window on an observed daemon adopts its observer, and the new window sees B's other "
          "workspace at once",
          "[switch][sidebar][attention][jump][presence]")
{
    auto rig = SwitchRig { RigListing::WithB, SwitcherPool::Presence };
    rig.presence().linkSetChanged();
    auto const blocked =
        std::array { blockPaneOfB(rig, rig.bWorkspaces[0]), blockPaneOfB(rig, rig.bWorkspaces[1]) };
    std::ignore = awaitSectionB(rig, rig.bWorkspaces[1]);
    auto const connectionsBefore = connections(rig.b);

    REQUIRE(rig.request(to(hostB(), "1"), SwitchMode::Open) == SwitchOutcome::Switched);

    // As a switch in place does: the presence's observer of B is the new window's connection, no second one.
    CHECK(rig.host.adopted == 1);
    CHECK(connections(rig.b) == connectionsBefore);
    REQUIRE(rig.host.opened.size() == 1);
    auto& other = *rig.host.opened.front()->controller;
    REQUIRE(waitUntil([&] {
        auto const* const source = other.directorySource();
        return source != nullptr && source->directory().selected == rig.bWorkspaces[0];
    }));
    // The observer held every workspace's status: the new window lists B's other workspace's blocked pane the
    // moment it shows B, with no flush to wait for, so Next Needing Me goes on to it.
    CHECK(std::ranges::contains(ownBlockedPanes(other, rig.bWorkspaces[1]), blocked[1]));
    // The asking window stayed where it was.
    CHECK(rig.launchLink().workspaceOf(rig.shown()) == rig.aWorkspaces[0]);
}

// {{{ JumpBack
namespace
{
/// @return The workspace @p window shows, and the tab its active pane is in; none for none.
[[nodiscard]] std::optional<std::pair<uint64_t, uint64_t>> shownTab(SwitchRig& rig,
                                                                    contour::window::WindowController& window)
{
    auto const* const source = window.directorySource();
    auto const* const active = window.activeSession();
    if (source == nullptr || active == nullptr)
        return std::nullopt;
    auto const directory = source->directory();
    auto const listed =
        contour::window::tabHoldingPane(directory, rig.app.manager().paneKeyOf(active->modelSessionId()));
    if (!listed || directory.selected != listed->workspace->id)
        return std::nullopt;
    return std::pair { listed->workspace->id, listed->tab->id };
}

/// @return Whether @p window is on A, showing @p workspace (and tab @p tab, when named).
[[nodiscard]] bool onA(SwitchRig& rig,
                       contour::window::WindowController& window,
                       uint64_t workspace,
                       std::optional<uint64_t> tab = std::nullopt)
{
    if (onB(rig))
        return false;
    auto const shown = shownTab(rig, window);
    return shown && shown->first == workspace && (!tab || shown->second == *tab);
}

/// Waits for the presence to observe A, the daemon the window left, under its run: a jump back names it.
void awaitAObserved(SwitchRig& rig)
{
    auto const run = rig.a.onDaemon([&rig] { return rig.a.host->instance(); });
    auto state = std::string {};
    auto const observed = waitUntil(
        [&] {
            // As the application tells its presence whenever a link ends or attaches.
            rig.presence().linkSetChanged();
            auto const sections = rig.presence().sections();
            auto const found = std::ranges::find(
                sections, contour::window::localDaemonRef("a"), &contour::window::DaemonSection::daemon);
            state = found == sections.end() ? std::string { "unlisted" }
                                            : std::format("state {} run {} feed {} reason {}",
                                                          static_cast<int>(found->state),
                                                          found->instance.value,
                                                          static_cast<int>(found->feed),
                                                          found->reason);
            return found != sections.end() && found->state == contour::window::DaemonLinkState::Connected
                   && found->instance == contour::window::DaemonInstance { run };
        },
        3000);
    INFO(state);
    REQUIRE(observed);
}

/// Activates B's workspace row @p workspace in @p window's sidebar, and waits for the window to show it.
void jumpToB(SwitchRig& rig, contour::window::WindowController& window, uint64_t workspace)
{
    std::ignore = awaitSectionB(rig, workspace);
    auto const row = rowOfB(window, contour::window::SidebarRowKind::Workspace, workspace);
    REQUIRE(row >= 0);
    window.sidebar().activate(row);
    REQUIRE(waitUntil(
        [&] {
            auto const shown = shownTab(rig, window);
            return onB(rig) && shown && shown->first == workspace;
        },
        3000));
}
} // namespace

TEST_CASE("JumpBack returns to the previous daemon's tab", "[switch][sidebar][jump][history]")
{
    auto rig = SwitchRig { RigListing::BothListed };
    rig.presence().linkSetChanged();
    auto& window = *rig.window->controller;
    // A second tab on A's workspace, the active one.
    auto const first = addSecondTab(rig.a, rig.aWorkspaces[0]);
    REQUIRE(waitUntil(
        [&] {
            auto const shown = shownTab(rig, window);
            return shown && shown->first == rig.aWorkspaces[0] && shown->second != first.tab;
        },
        3000));
    auto const tab = shownTab(rig, window)->second;

    jumpToB(rig, window, rig.bWorkspaces[1]);
    awaitAObserved(rig);
    window.jumpBack();

    CHECK(waitUntil([&] { return onA(rig, window, rig.aWorkspaces[0], tab); }, 3000));
    // Its own landing is not a place to go back to.
    window.jumpBack();
    drainGuiEvents();
    CHECK(rig.app.switching().requests.size() == 2);
}

TEST_CASE("JumpBack while a jump connects supersedes it", "[switch][sidebar][jump][history]")
{
    auto rig = SwitchRig { RigListing::WithB };
    rig.presence().linkSetChanged();
    auto& window = *rig.window->controller;
    // A's second workspace, shown from the sidebar: A's first is the place to go back to.
    auto const own = ownWorkspaceRow(window, rig.aWorkspaces[1]);
    REQUIRE(own >= 0);
    window.sidebar().activate(own);
    REQUIRE(waitUntil([&] { return onA(rig, window, rig.aWorkspaces[1]); }, 3000));
    // B's new link is held before it shows anything: the jump stays connecting.
    std::ignore = awaitSectionB(rig, rig.bWorkspaces[1]);
    rig.b.holdNewConnectionsAtLayout();
    window.sidebar().activate(rowOfB(window, contour::window::SidebarRowKind::Workspace, rig.bWorkspaces[1]));
    REQUIRE(waitUntil([&] { return rig.host.started == 1; }, 2000));
    REQUIRE(window.sidebar().isJumpPending());

    window.jumpBack();

    CHECK(rig.app.switching().cancels == std::vector { rig.shown() });
    CHECK_FALSE(window.sidebar().isJumpPending());
    rig.b.releaseHeld();
    drainGuiEvents();
    CHECK(rig.windowLink() == &rig.launchLink());
    CHECK(waitUntil([&] { return onA(rig, window, rig.aWorkspaces[0]); }, 3000));
    // B's connection went back to observing it: its section stays listed and connected.
    CHECK(waitUntil(
        [&] {
            auto const sections = rig.presence().sections();
            auto const found =
                std::ranges::find(sections, sidebarB(), &contour::window::DaemonSection::daemon);
            return found != sections.end() && found->state == contour::window::DaemonLinkState::Connected;
        },
        3000));
}

TEST_CASE("JumpBack after a triage run returns to where it started",
          "[switch][sidebar][attention][jump][history]")
{
    auto rig = SwitchRig { RigListing::BothListed };
    rig.presence().linkSetChanged();
    auto& window = *rig.window->controller;
    // Two tabs of B's first workspace whose panes ask the user: the run goes onto B, then between them.
    auto const workspace = rig.bWorkspaces[0];
    printInPaneOf(rig.b, workspace, std::string { BlockedStatus });
    std::ignore = addSecondTab(rig.b, workspace);
    printInPaneOf(rig.b, workspace, std::string { BlockedStatus });
    REQUIRE(waitUntil([&] { return blockedPanesOfB(rig, workspace).size() == 2; }, 3000));
    auto const blocked = blockedPanesOfB(rig, workspace);
    auto const shows = [&](contour::window::PaneKey pane) {
        return showsPaneOfB(rig, window, pane);
    };

    // Three attention jumps: onto B, then between its two panes, the window's own by then.
    window.jumpToNextAttention();
    REQUIRE(waitUntil([&] { return shows(blocked[0]) || shows(blocked[1]); }, 3000));
    auto const landed = *activePaneOf(rig, window);
    auto const other = landed == blocked[0] ? blocked[1] : blocked[0];
    window.jumpToNextAttention();
    REQUIRE(waitUntil([&] { return shows(other); }, 3000));
    window.jumpToNextAttention();
    REQUIRE(waitUntil([&] { return shows(landed); }, 3000));

    // One press: where the run started, not a pane along it.
    awaitAObserved(rig);
    window.jumpBack();

    CHECK(waitUntil([&] { return onA(rig, window, rig.aWorkspaces[0]); }, 3000));
}

TEST_CASE("JumpBack: a switcher pick and a workspace switch are recorded, a tab-strip switch is not",
          "[switch][jump][history]")
{
    auto rig = SwitchRig {};
    auto& window = *rig.window->controller;
    auto* const acting = rig.app.manager().actingSessionOfWindow(rig.shown());
    REQUIRE(acting != nullptr);
    // A's first workspace holds two tabs; the first is active.
    auto const first = addSecondTab(rig.a, rig.aWorkspaces[0]);
    auto second = std::optional<uint64_t> {};
    REQUIRE(waitUntil(
        [&] {
            auto const shown = shownTab(rig, window);
            if (shown && shown->second != first.tab)
                second = shown->second;
            return second.has_value();
        },
        3000));
    REQUIRE(acting->operator()(contour::actions::SwitchToTab { .position = 1 }));
    REQUIRE(waitUntil([&] { return onA(rig, window, rig.aWorkspaces[0], first.tab); }, 3000));

    // A workspace switch (SwitchToWorkspace), from A's first workspace's first tab.
    REQUIRE(acting->operator()(contour::actions::SwitchToWorkspace { .index = 2 }));
    REQUIRE(waitUntil([&] { return onA(rig, window, rig.aWorkspaces[1]); }, 3000));
    // A switcher pick back to the first workspace, made as the switcher's request service makes it.
    auto told = Told {};
    rig.switcher->request(
        rig.shown(),
        to(daemonA(), "1"),
        SwitchMode::Switch,
        contour::session::UserSwitch,
        rig.app.manager().recordingSwitchCompletion(rig.shown(), SwitchMode::Switch, told.completion()));
    REQUIRE(waitUntil([&] { return told.outcome.has_value(); }, 2000));
    REQUIRE(waitUntil([&] { return onA(rig, window, rig.aWorkspaces[0]); }, 3000));
    // A tab-strip switch there: no place of its own.
    auto* const here = rig.app.manager().actingSessionOfWindow(rig.shown());
    REQUIRE(here != nullptr);
    REQUIRE(here->operator()(contour::actions::SwitchToTab { .position = 2 }));
    REQUIRE(waitUntil([&] { return onA(rig, window, rig.aWorkspaces[0], *second); }, 3000));

    // Back: the pick's origin, A's second workspace -- not the first tab the strip left.
    window.jumpBack();
    CHECK(waitUntil([&] { return onA(rig, window, rig.aWorkspaces[1]); }, 3000));
    // Back again: the switch's origin, the first workspace's first tab.
    window.jumpBack();
    CHECK(waitUntil([&] { return onA(rig, window, rig.aWorkspaces[0], first.tab); }, 3000));
}

// Review Focus 3, as Task 21's A→B→A case: JumpBack's landing on A before the panes it left there unbound.
// Phase 7 review I2: on production's wiring -- the presence is the switcher's pool, so A's link is demoted as
// the window leaves, and is handed back to the presence only once its panes are gone, which here is after
// JumpBack landed on A again.
TEST_CASE("A→B→JumpBack before A's panes unbound leaves exactly one connection to A",
          "[switch][sidebar][jump][history][threading]")
{
    auto rig = SwitchRig { RigListing::BothListed, SwitcherPool::Presence };
    rig.presence().linkSetChanged();
    auto& window = *rig.window->controller;
    auto const leftOnA = rig.windowSessions();
    std::ignore = awaitSectionB(rig, rig.bWorkspaces[1]);
    window.sidebar().activate(rowOfB(window, contour::window::SidebarRowKind::Workspace, rig.bWorkspaces[1]));
    REQUIRE(waitUntil([&] { return onB(rig); }, 3000));
    rig.presence().linkSetChanged();
    // A's old panes are closed but not gone: no exit watcher runs in this rig, so each goes only once the
    // test says its exit was seen, below. Until then A's link holds them, and its connection.
    REQUIRE(std::ranges::any_of(leftOnA, [](auto const& pane) { return static_cast<bool>(pane); }));

    window.jumpBack();

    REQUIRE(waitUntil([&] { return onA(rig, window, rig.aWorkspaces[0]); }, 3000));
    // B's jump adopted the presence's observer; the jump back found none of A's to adopt, and dialled.
    CHECK(rig.host.adopted == 1);
    // A's old panes go now, as each one's watcher would see its closed device: A's old link hands its
    // connection back to the presence, which a window is on A again by then.
    for (auto const& session: leftOnA)
        if (session && session->terminal().device().isClosed())
            session->onClosed();
    rig.presence().linkSetChanged();
    CHECK(waitUntil(
        [&] {
            drainGuiEvents();
            return connections(rig.a) == 1;
        },
        3000));
    settleGui();
    rig.presence().linkSetChanged();
    settleGui();
    CHECK(connections(rig.a) == 1);
    CHECK(rig.windowLink() != &rig.launchLink());
}

namespace
{
/// @return The row of A's pane @p pane of workspace @p workspace in @p window's sidebar, once it lists it.
[[nodiscard]] int ownPaneRow(contour::window::WindowController& window, uint64_t workspace, uint64_t pane)
{
    auto row = -1;
    REQUIRE(waitUntil(
        [&] {
            row = sidebarRowWhere(window, [&](auto const& key) {
                return key.kind == contour::window::SidebarRowKind::Pane && key.workspace == workspace
                       && key.pane == contour::window::PaneKey { pane } && key.daemon != sidebarB();
            });
            return row >= 0;
        },
        3000));
    return row;
}

/// @return How many things the window of @p rig said so far.
[[nodiscard]] std::size_t saidCount(SwitchRig const& rig)
{
    return rig.said != nullptr ? rig.said->said.size() : std::size_t { 0 };
}
} // namespace

// Phase 7 review I1: an own-row menu action is a navigation of the window's own, as an activation is.
TEST_CASE(
    "Go to Pane on an own pane row while a jump connects ends that jump, and is a place to go back from",
    "[switch][sidebar][jump][history]")
{
    auto rig = SwitchRig { RigListing::WithB };
    rig.presence().linkSetChanged();
    auto& window = *rig.window->controller;
    auto const split = splitSecondWorkspaceOfA(rig);
    std::ignore = ownPaneRow(window, rig.aWorkspaces[1], split.first);
    // B's new link is held before it shows anything: the jump stays connecting.
    std::ignore = awaitSectionB(rig, rig.bWorkspaces[1]);
    rig.b.holdNewConnectionsAtLayout();
    window.sidebar().activate(rowOfB(window, contour::window::SidebarRowKind::Workspace, rig.bWorkspaces[1]));
    REQUIRE(waitUntil([&] { return rig.host.started == 1; }, 2000));
    REQUIRE(window.sidebar().isJumpPending());
    auto const saidBefore = saidCount(rig);
    auto const shownBefore = rig.shown();

    pickFromMenu(window, ownPaneRow(window, rig.aWorkspaces[1], split.first), QStringLiteral("Go to Pane"));

    CHECK(rig.app.switching().cancels == std::vector { shownBefore });
    CHECK_FALSE(window.sidebar().isJumpPending());
    rig.b.releaseHeld();
    drainGuiEvents();
    // The window stays on A, on the pane the newer navigation chose, and the ended jump said nothing.
    CHECK(rig.windowLink() == &rig.launchLink());
    REQUIRE(waitUntil(
        [&] {
            return onA(rig, window, rig.aWorkspaces[1])
                   && activePaneOf(rig, window) == contour::window::PaneKey { split.first };
        },
        3000));
    CHECK(saidCount(rig) == saidBefore);
    // Where Go to Pane left from is a place to go back to.
    window.jumpBack();
    CHECK(waitUntil([&] { return onA(rig, window, rig.aWorkspaces[0]); }, 3000));
}

// Phase 7 review M1: the tab comes back as it is, its own active pane focused, not its first.
TEST_CASE("JumpBack to a tab of another own workspace shows its active pane",
          "[switch][sidebar][jump][history]")
{
    auto rig = SwitchRig {};
    auto& window = *rig.window->controller;
    auto const split = splitSecondWorkspaceOfA(rig);
    auto const showsActive = [&] {
        return onA(rig, window, rig.aWorkspaces[1], split.tab)
               && activePaneOf(rig, window) == contour::window::PaneKey { split.active };
    };
    window.sidebar().activate(ownWorkspaceRow(window, rig.aWorkspaces[1]));
    REQUIRE(waitUntil(showsActive, 3000));
    window.sidebar().activate(ownWorkspaceRow(window, rig.aWorkspaces[0]));
    REQUIRE(waitUntil([&] { return onA(rig, window, rig.aWorkspaces[0]); }, 3000));

    window.jumpBack();

    CHECK(waitUntil(showsActive, 3000));
    drainGuiEvents();
    // Nor was the tab's active pane changed on the daemon.
    CHECK(rig.a.onDaemon([&] {
        return rig.a.host->model()
            .workspace(vtworkspace::WorkspaceId { rig.aWorkspaces[1] })
            ->tabAt(0)
            ->activePane()
            ->session()
            .value;
    }) == split.active);
}

// Phase 7 review M2: a navigation that never happened is no place to go back from, nor a run's start.
TEST_CASE("An attention jump whose workspace was never selected records nothing",
          "[switch][sidebar][attention][jump][history]")
{
    auto rig = SwitchRig {};
    auto& window = *rig.window->controller;
    // A's second workspace's pane asks the user; A's first holds two tabs, the second active.
    printInPaneOf(rig.a, rig.aWorkspaces[1], std::string { BlockedStatus });
    REQUIRE(waitUntil([&] { return !ownBlockedPanes(window, rig.aWorkspaces[1]).empty(); }, 3000));
    auto const first = addSecondTab(rig.a, rig.aWorkspaces[0]);
    auto second = std::optional<uint64_t> {};
    REQUIRE(waitUntil(
        [&] {
            auto const shown = shownTab(rig, window);
            if (shown && shown->first == rig.aWorkspaces[0] && shown->second != first.tab)
                second = shown->second;
            return second.has_value();
        },
        3000));
    auto const connected = [&] {
        auto const* const controller = rig.launchLink().liveController();
        return controller != nullptr
               && controller->connectionState() == vthost::client::ConnectionState::Connected;
    };

    // While A is away, the press selects nothing.
    rig.a.refuseConnections();
    REQUIRE(waitUntil([&] { return !connected(); }, 2000));
    window.jumpToNextAttention();
    rig.a.serveAgain();
    REQUIRE(waitUntil(connected, 3000));
    REQUIRE(waitUntil([&] { return showing(rig.a, rig.aWorkspaces[0]) == 1; }, 3000));
    // A tab-strip click, unrecorded, then a press that lands.
    auto* const acting = rig.app.manager().actingSessionOfWindow(rig.shown());
    REQUIRE(acting != nullptr);
    REQUIRE(acting->operator()(contour::actions::SwitchToTab { .position = 1 }));
    REQUIRE(waitUntil([&] { return onA(rig, window, rig.aWorkspaces[0], first.tab); }, 3000));
    window.jumpToNextAttention();
    REQUIRE(waitUntil([&] { return onA(rig, window, rig.aWorkspaces[1]); }, 3000));

    // The run began where the press that landed was made, not where the one that went nowhere was.
    window.jumpBack();
    CHECK(waitUntil([&] { return onA(rig, window, rig.aWorkspaces[0], first.tab); }, 3000));
}

// Phase 7 review M4: a second press while the window's own workspace is being selected goes on from there.
TEST_CASE("JumpToNextAttention while the own workspace is being selected goes on from that pane",
          "[switch][sidebar][attention][jump]")
{
    auto rig = SwitchRig {};
    auto& window = *rig.window->controller;
    // Two blocked panes of A's second workspace, in two tabs, the first tab active: what the workspace shows
    // when no jump lands in it is the first press's pane.
    printInPaneOf(rig.a, rig.aWorkspaces[1], std::string { BlockedStatus });
    auto const first = addSecondTab(rig.a, rig.aWorkspaces[1]);
    printInPaneOf(rig.a, rig.aWorkspaces[1], std::string { BlockedStatus });
    rig.a.onDaemon([&rig, &first] {
        rig.a.host->model().activateTab(vtworkspace::WorkspaceId { rig.aWorkspaces[1] },
                                        vtworkspace::TabId { first.tab });
        return 0;
    });
    REQUIRE(waitUntil([&] { return ownBlockedPanes(window, rig.aWorkspaces[1]).size() == 2; }, 3000));
    auto const blocked = ownBlockedPanes(window, rig.aWorkspaces[1]);
    // A's answer to the selection is held: the first press's pane waits for its workspace.
    rig.a.holdEverything();

    window.jumpToNextAttention();
    window.jumpToNextAttention();
    rig.a.releaseHeld();

    // The second press went on to the other pane, not to the first one again.
    CHECK(waitUntil(
        [&] { return onA(rig, window, rig.aWorkspaces[1]) && activePaneOf(rig, window) == blocked[1]; },
        3000));
}

// Phase 7 review M8: an empty back stack says so.
TEST_CASE("JumpBack with nowhere to go back to says so", "[switch][jump][history]")
{
    auto rig = SwitchRig {};
    auto& window = *rig.window->controller;
    CHECK_FALSE(window.canJumpBack());
    CHECK(window.menuBarState().jumpBack == contour::command::JumpAvailability::Unavailable);
    CHECK(window.menuBarState().nextAttention == contour::command::JumpAvailability::Unavailable);

    window.jumpBack();

    CHECK(saidLast(rig.said, QStringLiteral("Nowhere to go back to")));
    // Once a place is recorded, the row can act.
    window.sidebar().activate(ownWorkspaceRow(window, rig.aWorkspaces[1]));
    REQUIRE(waitUntil([&] { return onA(rig, window, rig.aWorkspaces[1]); }, 3000));
    CHECK(window.canJumpBack());
    CHECK(window.menuBarState().jumpBack == contour::command::JumpAvailability::Available);
}

// Phase 7 review M10: a switch the sidebar did not ask for replaces its jump, which says nothing.
TEST_CASE("A workspace switch while a sidebar jump connects ends that jump without a word",
          "[switch][sidebar][jump]")
{
    auto rig = SwitchRig { RigListing::WithB };
    rig.presence().linkSetChanged();
    auto& window = *rig.window->controller;
    std::ignore = awaitSectionB(rig, rig.bWorkspaces[1]);
    rig.b.holdNewConnectionsAtLayout();
    window.sidebar().activate(rowOfB(window, contour::window::SidebarRowKind::Workspace, rig.bWorkspaces[1]));
    REQUIRE(waitUntil([&] { return rig.host.started == 1; }, 2000));
    REQUIRE(window.sidebar().isJumpPending());
    auto const saidBefore = saidCount(rig);

    auto* const acting = rig.app.manager().actingSessionOfWindow(rig.shown());
    REQUIRE(acting != nullptr);
    REQUIRE(acting->operator()(contour::actions::SwitchToWorkspace { .index = 2 }));

    CHECK_FALSE(window.sidebar().isJumpPending());
    REQUIRE(waitUntil([&] { return onA(rig, window, rig.aWorkspaces[1]); }, 3000));
    rig.b.releaseHeld();
    drainGuiEvents();
    CHECK(rig.windowLink() == &rig.launchLink());
    CHECK(saidCount(rig) == saidBefore);
}
// }}}

TEST_CASE("WindowSwitcher: New Tab Here on a workspace another window shows runs in that window",
          "[switch][sidebar][jump]")
{
    auto rig = SwitchRig { RigListing::WithB };
    rig.presence().linkSetChanged();
    auto& window = *rig.window->controller;
    // Another window shows B's second workspace, reached as the sidebar names B.
    REQUIRE(rig.request(to(hostB(), "2"), SwitchMode::Open) == SwitchOutcome::Switched);
    REQUIRE(rig.host.opened.size() == 1);
    auto& other = *rig.host.opened.front()->controller;
    REQUIRE(waitUntil([&] {
        auto const* const source = other.directorySource();
        return source != nullptr && source->directory().selected == rig.bWorkspaces[1];
    }));
    auto const tabsOf = [](contour::window::WindowController const& shown, uint64_t workspace) {
        auto const* const source = shown.directorySource();
        if (source == nullptr)
            return std::size_t { 0 };
        auto const directory = source->directory();
        auto const* const listed = contour::window::findWorkspace(directory, workspace);
        return listed != nullptr ? listed->tabs.size() : std::size_t { 0 };
    };
    // Its listing as observed is stale once another window is on it: read the live one.
    rig.presence().linkSetChanged();
    std::ignore = awaitSectionB(rig, rig.bWorkspaces[1]);
    auto const before = tabsOf(other, rig.bWorkspaces[1]);
    auto const sessionsHere = rig.app.manager().sessionsOfWindow(rig.shown()).size();
    rig.app.switching().requests.clear();

    pickFromMenu(window,
                 rowOfB(window, contour::window::SidebarRowKind::Workspace, rig.bWorkspaces[1]),
                 QStringLiteral("New Tab Here"));

    REQUIRE(waitUntil([&] { return tabsOf(other, rig.bWorkspaces[1]) == before + 1; }, 3000));
    // This window did not move, and holds what it held.
    CHECK(rig.launchLink().workspaceOf(rig.shown()) == rig.aWorkspaces[0]);
    CHECK(rig.app.manager().sessionsOfWindow(rig.shown()).size() == sessionsHere);
    CHECK(rig.host.opened.size() == 1);
}

TEST_CASE("WindowSwitcher: a verb for a restarted daemon's old instance does nothing",
          "[switch][sidebar][jump]")
{
    // How long a frame is given to reach B, in waitUntil()'s 5 ms attempts: the stale verbs' must not within
    // it, the control's must.
    constexpr auto StaleVerbAttempts = 200;
    auto rig = SwitchRig { RigListing::WithB };
    rig.presence().linkSetChanged();
    auto& window = *rig.window->controller;
    auto const section = awaitSectionB(rig, rig.bWorkspaces[1]);
    auto const old = section.instance;
    auto const tab = contour::window::findWorkspace(section.directory, rig.bWorkspaces[1])->tabs.front().id;
    auto asked =
        QSignalSpy { &window, &contour::window::WindowController::sidebarCloseConfirmationRequested };

    // The menu is pinned to the row as it was; B restarts while it is open.
    auto const row = rowOfB(window, contour::window::SidebarRowKind::Tab, rig.bWorkspaces[1], tab);
    REQUIRE(row >= 0);
    window.sidebar().openContextMenu(row, QPointF {});
    auto const close = menuActionIdOf(window.sidebarContextMenuModel(), QStringLiteral("Close Tab…"));
    auto const rename = menuActionIdOf(window.sidebarContextMenuModel(), QStringLiteral("Rename…"));
    REQUIRE(close.has_value());
    REQUIRE(rename.has_value());
    rig.b.restartHost();
    REQUIRE(waitUntil(
        [&] {
            auto const sections = rig.presence().sections();
            auto const found =
                std::ranges::find(sections, sidebarB(), &contour::window::DaemonSection::daemon);
            return found != sections.end() && found->state == contour::window::DaemonLinkState::Connected
                   && found->instance != old;
        },
        3000));
    // B's new run is seeded as the old one was: its ids are the old run's again, so a stale verb that slipped
    // through would land on a node of the same ids.
    auto const seeded = seedWorkspaces(rig.b, 2);
    REQUIRE(seeded == rig.bWorkspaces);
    auto const fresh = awaitSectionB(rig, rig.bWorkspaces[1]);
    REQUIRE(fresh.instance != old);
    auto const freshTab =
        contour::window::findWorkspace(fresh.directory, rig.bWorkspaces[1])->tabs.front().id;
    REQUIRE(freshTab == tab);
    auto const freshListed = [&] {
        auto const sections = rig.presence().sections();
        auto const found = std::ranges::find(sections, sidebarB(), &contour::window::DaemonSection::daemon);
        return found != sections.end() ? contour::window::findTab(found->directory, rig.bWorkspaces[1], tab)
                                       : std::nullopt;
    };

    window.triggerSidebarContextMenuAction(*close);
    REQUIRE(asked.count() == 1);
    window.confirmClose(asked.front()[0]);
    window.triggerSidebarContextMenuAction(*rename);

    // Nothing reaches B's new run -- its tab of the same ids lives on -- and the window neither jumps nor
    // shows anything first.
    CHECK_FALSE(waitUntil([&] { return !freshListed().has_value(); }, StaleVerbAttempts));
    CHECK(rig.app.switching().requests.empty());
    CHECK_FALSE(window.rowActionPending());
    CHECK(rig.launchLink().workspaceOf(rig.shown()) == rig.aWorkspaces[0]);

    // The control: the same close asked on that row as B's new run lists it does reach it within the time
    // the stale one was given -- so the quiet above is the stale run's, not a verb arriving slowly.
    pickFromMenu(window,
                 rowOfB(window, contour::window::SidebarRowKind::Tab, rig.bWorkspaces[1], tab),
                 QStringLiteral("Close Tab…"));
    REQUIRE(asked.count() == 2);
    CHECK(closeNodeOf(asked.back()).row.instance == fresh.instance);
    window.confirmClose(asked.back()[0]);
    CHECK(waitUntil([&] { return !freshListed().has_value(); }, StaleVerbAttempts));
}

TEST_CASE("WindowSwitcher: Custom colour on another daemon's workspace colours it there",
          "[switch][sidebar][jump]")
{
    auto rig = SwitchRig { RigListing::WithB };
    rig.presence().linkSetChanged();
    auto& window = *rig.window->controller;
    std::ignore = awaitSectionB(rig, rig.bWorkspaces[1]);
    auto picked = QSignalSpy { &window, &contour::window::WindowController::workspaceColorPickRequested };

    auto const row = rowOfB(window, contour::window::SidebarRowKind::Workspace, rig.bWorkspaces[1]);
    REQUIRE(row >= 0);
    window.sidebar().openContextMenu(row, QPointF {});
    auto const custom = submenuActionIdOf(
        window.sidebarContextMenuModel(), QStringLiteral("Workspace Color"), QStringLiteral("Custom…"));
    REQUIRE(custom.has_value());
    window.triggerSidebarContextMenuAction(*custom);
    REQUIRE(picked.count() == 1);
    // It hangs from B's row, by its index: not from an own row of the same id (workspace ids start at 1 on
    // every daemon).
    CHECK(picked.front()[0].toInt()
          == rowOfB(window, contour::window::SidebarRowKind::Workspace, rig.bWorkspaces[1]));
    CHECK(rowKeyOf(picked.front()[1]).daemon == sidebarB());
    CHECK(rowKeyOf(picked.front()[1]).workspace == rig.bWorkspaces[1]);

    // The picker's pick, as Sidebar.qml commits it: with the row it was opened with.
    auto const colour = vtbackend::RGBColor { 0x12, 0x34, 0x56 };
    window.setRowColor(picked.front()[1], QColor { 0x12, 0x34, 0x56 });
    CHECK(waitUntil([&] {
        auto const sections = rig.presence().sections();
        auto const found = std::ranges::find(sections, sidebarB(), &contour::window::DaemonSection::daemon);
        auto const* const listed = found != sections.end()
                                       ? contour::window::findWorkspace(found->directory, rig.bWorkspaces[1])
                                       : nullptr;
        return listed != nullptr && listed->color == colour;
    }));
    // A workspace of the window's own daemon that happens to share the id is left as it was.
    auto const* const source = window.directorySource();
    REQUIRE(source != nullptr);
    auto const ownDirectory = source->directory();
    if (auto const* const own = contour::window::findWorkspace(ownDirectory, rig.bWorkspaces[1]))
        CHECK_FALSE(own->color.has_value());
}

namespace
{
/// @return Whether @p daemon's model holds workspace @p workspace.
[[nodiscard]] bool holdsWorkspace(DaemonFixture& daemon, uint64_t workspace)
{
    return daemon.onDaemon(
        [&] { return daemon.host->model().workspace(vtworkspace::WorkspaceId { workspace }) != nullptr; });
}
} // namespace

// Review (Phase 6, C1): a cancelled close of another daemon's workspace pinned its row on the window, and the
// CloseWorkspace action on the window's own workspace of the same id then closed the other daemon's.
TEST_CASE("WindowSwitcher: a cancelled close of another daemon's workspace leaves the own close the own",
          "[switch][sidebar][jump]")
{
    auto rig = SwitchRig { RigListing::WithB };
    rig.presence().linkSetChanged();
    auto& window = *rig.window->controller;
    // Workspace ids start at 1 on every daemon: A's second workspace and B's share theirs.
    auto const shared = rig.bWorkspaces[1];
    REQUIRE(rig.aWorkspaces[1] == shared);
    std::ignore = awaitSectionB(rig, shared);
    auto sidebarAsked =
        QSignalSpy { &window, &contour::window::WindowController::sidebarCloseConfirmationRequested };
    auto ownAsked =
        QSignalSpy { &window, &contour::window::WindowController::closeWorkspaceConfirmationRequested };

    // Close Workspace… on B's, cancelled: the dialog is dismissed, and nothing is answered.
    pickFromMenu(window,
                 rowOfB(window, contour::window::SidebarRowKind::Workspace, shared),
                 QStringLiteral("Close Workspace…"));
    REQUIRE(sidebarAsked.count() == 1);

    // Later, the CloseWorkspace action on the window's own workspace of that id: a daemon window always asks.
    rig.app.manager().closeWorkspace(window, shared);
    REQUIRE(ownAsked.count() == 1);
    auto const node = closeNodeOf(ownAsked.front());
    CHECK(node.row.daemon != sidebarB());
    auto const text = window.closeConfirmation(ownAsked.front()[0], QString {}, QString {});
    CHECK_FALSE(text["message"].toString().contains(QStringLiteral("It runs on")));
    window.confirmClose(ownAsked.front()[0]);

    // The window's own closes; B's is untouched.
    CHECK(rig.a.waitUntilOnDaemon(
        [&] { return rig.a.host->model().workspace(vtworkspace::WorkspaceId { shared }) == nullptr; }));
    drainGuiEvents();
    CHECK(holdsWorkspace(rig.b, shared));
    CHECK(rig.b.clientFrames.count(vthost::proto::PduType::CloseWorkspace) == 0);
}

TEST_CASE("WindowSwitcher: Close Workspace… on another daemon's workspace closes it there, not the own",
          "[switch][sidebar][jump]")
{
    auto rig = SwitchRig { RigListing::WithB };
    rig.presence().linkSetChanged();
    auto& window = *rig.window->controller;
    auto const shared = rig.bWorkspaces[1];
    REQUIRE(rig.aWorkspaces[1] == shared);
    std::ignore = awaitSectionB(rig, shared);
    auto asked =
        QSignalSpy { &window, &contour::window::WindowController::sidebarCloseConfirmationRequested };

    pickFromMenu(window,
                 rowOfB(window, contour::window::SidebarRowKind::Workspace, shared),
                 QStringLiteral("Close Workspace…"));
    REQUIRE(asked.count() == 1);
    auto const node = closeNodeOf(asked.front());
    CHECK(node.kind == contour::window::CloseKind::Workspace);
    CHECK(node.row.daemon == sidebarB());
    auto const text = window.closeConfirmation(asked.front()[0], QString {}, QString {});
    CHECK(text["message"].toString().endsWith(QStringLiteral("It runs on b.")));
    window.confirmClose(asked.front()[0]);

    CHECK(rig.b.waitUntilOnDaemon(
        [&] { return rig.b.host->model().workspace(vtworkspace::WorkspaceId { shared }) == nullptr; }));
    drainGuiEvents();
    CHECK(holdsWorkspace(rig.a, shared));
    CHECK(rig.launchLink().workspaceOf(rig.shown()) == rig.aWorkspaces[0]);
    CHECK(rig.app.switching().requests.empty());
}

TEST_CASE("WindowSwitcher: Close Pane… on another daemon's pane closes that pane there",
          "[switch][sidebar][jump]")
{
    auto rig = SwitchRig { RigListing::WithB };
    rig.presence().linkSetChanged();
    auto& window = *rig.window->controller;
    auto const workspace = rig.bWorkspaces[1];
    // B's tab, split: a tab of one pane lists no pane rows.
    auto const tab = rig.b.onDaemon([&] {
        auto* const held = rig.b.host->model().workspace(vtworkspace::WorkspaceId { workspace })->tabAt(0);
        rig.b.host->splitActivePane(held->id(), vtworkspace::SplitState::Vertical, 0.5);
        return held->id().value;
    });
    REQUIRE(waitUntil([&] {
        auto const sections = rig.presence().sections();
        auto const found = std::ranges::find(sections, sidebarB(), &contour::window::DaemonSection::daemon);
        auto const listed = found != sections.end()
                                ? contour::window::findTab(found->directory, workspace, tab)
                                : std::nullopt;
        return listed && listed->tab->panes.size() == 2;
    }));
    auto const sessionsBefore = rig.app.manager().sessionsOfWindow(rig.shown()).size();
    auto asked =
        QSignalSpy { &window, &contour::window::WindowController::sidebarCloseConfirmationRequested };

    pickFromMenu(window,
                 rowOfB(window, contour::window::SidebarRowKind::Pane, workspace, tab),
                 QStringLiteral("Close Pane…"));
    REQUIRE(asked.count() == 1);
    auto const node = closeNodeOf(asked.front());
    CHECK(node.kind == contour::window::CloseKind::Pane);
    CHECK(node.row.daemon == sidebarB());
    window.confirmClose(asked.front()[0]);

    CHECK(waitUntil([&] {
        auto const sections = rig.presence().sections();
        auto const found = std::ranges::find(sections, sidebarB(), &contour::window::DaemonSection::daemon);
        auto const listed = found != sections.end()
                                ? contour::window::findTab(found->directory, workspace, tab)
                                : std::nullopt;
        return listed && listed->tab->panes.size() == 1
               && std::ranges::none_of(listed->tab->panes,
                                       [&](auto const& pane) { return pane.key == node.row.pane; });
    }));
    drainGuiEvents();
    CHECK(rig.app.manager().sessionsOfWindow(rig.shown()).size() == sessionsBefore);
    CHECK(rig.launchLink().workspaceOf(rig.shown()) == rig.aWorkspaces[0]);
}

// Review (Phase 6, I2): Custom… on another daemon's tab pinned its row on the window, and a later pick of the
// window's own tab of the same id -- the menu bar's Tab Color › Custom… -- coloured the other daemon's.
TEST_CASE("WindowSwitcher: after a pick on another daemon's tab, the own tab's pick colours the own tab",
          "[switch][sidebar][jump][tabcolor]")
{
    auto rig = SwitchRig { RigListing::WithB };
    rig.presence().linkSetChanged();
    auto& window = *rig.window->controller;
    // No tab strip: the active tab's pick hangs from its sidebar row, as Custom… on a row's does.
    window.setTabBarVisibility(contour::config::TabBarVisibility::Never);
    auto const* const source = window.directorySource();
    REQUIRE(source != nullptr);
    auto const ownTab = [&] {
        auto const directory = source->directory();
        auto const* const listed = contour::window::findWorkspace(directory, rig.aWorkspaces[0]);
        REQUIRE(listed != nullptr);
        REQUIRE_FALSE(listed->tabs.empty());
        return listed->tabs.front();
    };
    // B's tab with the id of the window's own: tab ids start at 1 on every daemon, too.
    auto const section = awaitSectionB(rig, rig.bWorkspaces[0]);
    auto const* const theirs = contour::window::findWorkspace(section.directory, rig.bWorkspaces[0]);
    REQUIRE(theirs != nullptr);
    REQUIRE_FALSE(theirs->tabs.empty());
    auto const shared = theirs->tabs.front().id;
    REQUIRE(ownTab().id == shared);
    auto const colourOfB = [&]() -> std::optional<vtbackend::RGBColor> {
        auto const sections = rig.presence().sections();
        auto const found = std::ranges::find(sections, sidebarB(), &contour::window::DaemonSection::daemon);
        auto const listed = found != sections.end()
                                ? contour::window::findTab(found->directory, rig.bWorkspaces[0], shared)
                                : std::nullopt;
        return listed ? listed->tab->color : std::nullopt;
    };
    auto picked = QSignalSpy { &window, &contour::window::WindowController::sidebarTabColorPickRequested };

    // Custom… on B's tab, committed.
    auto const row = rowOfB(window, contour::window::SidebarRowKind::Tab, rig.bWorkspaces[0], shared);
    REQUIRE(row >= 0);
    window.sidebar().openContextMenu(row, QPointF {});
    auto const custom = submenuActionIdOf(
        window.sidebarContextMenuModel(), QStringLiteral("Tab Color"), QStringLiteral("Custom…"));
    REQUIRE(custom.has_value());
    window.triggerSidebarContextMenuAction(*custom);
    REQUIRE(picked.count() == 1);
    CHECK(rowKeyOf(picked.at(0)[1]).daemon == sidebarB());
    auto const plum = vtbackend::RGBColor { 0x8E, 0x45, 0x85 };
    window.setRowColor(picked.at(0)[1], QColor { plum.red, plum.green, plum.blue });
    REQUIRE(waitUntil([&] { return colourOfB() == plum; }));

    // Later, the window's own active tab's pick (the menu bar's Tab Color › Custom…), committed.
    window.beginActiveTabColorPick();
    REQUIRE(picked.count() == 2);
    CHECK(rowKeyOf(picked.at(1)[1]).daemon != sidebarB());
    CHECK(picked.at(1)[0].toInt() >= 0);
    auto const teal = vtbackend::RGBColor { 0x00, 0x80, 0x80 };
    window.setRowColor(picked.at(1)[1], QColor { teal.red, teal.green, teal.blue });

    // The own tab takes it; B's keeps its own.
    CHECK(waitUntil([&] { return ownTab().color == teal; }));
    drainGuiEvents();
    CHECK(colourOfB() == plum);
}

// Review (Phase 6 round 2, item 1): the own row keys came from the rows' cached own daemon, which a switch
// updates only by the rebuild it queues; until then CloseWorkspace named the daemon just left, and closed
// its workspace of the same id there.
TEST_CASE(
    "WindowSwitcher: CloseWorkspace right after a switch closes the workspace of the daemon switched to",
    "[switch][sidebar][jump]")
{
    auto rig = SwitchRig {};
    auto& window = *rig.window->controller;
    // Workspace ids start at 1 on every daemon: A's second workspace and B's share theirs.
    auto const shared = rig.aWorkspaces[1];
    window.sidebar().refreshNow();
    auto asked =
        QSignalSpy { &window, &contour::window::WindowController::closeWorkspaceConfirmationRequested };
    auto told = Told {};
    auto completion = told.completion();
    // Dispatched in the switch's completion: the window is B's, and the rebuild it queued has not run.
    rig.switcher->request(rig.shown(),
                          to(daemonB(), "2"),
                          SwitchMode::Switch,
                          contour::session::UserSwitch,
                          [&](std::expected<SwitchOutcome, std::string> outcome) {
                              rig.app.manager().closeWorkspace(window, shared);
                              completion(std::move(outcome));
                          });
    REQUIRE(waitUntil([&] { return told.outcome.has_value(); }, 2000));
    REQUIRE(*told.outcome == SwitchOutcome::Switched);
    REQUIRE(rig.bWorkspaces[1] == shared);
    REQUIRE(asked.count() == 1);
    // The node names the daemon the window is on now, not the one it left.
    auto const onB = rig.app.manager().daemonRefOf(rig.shown());
    REQUIRE(onB.has_value());
    CHECK(closeNodeOf(asked.front()).row.daemon == *onB);
    window.confirmClose(asked.front()[0]);

    CHECK(rig.b.waitUntilOnDaemon(
        [&] { return rig.b.host->model().workspace(vtworkspace::WorkspaceId { shared }) == nullptr; }));
    drainGuiEvents();
    CHECK(holdsWorkspace(rig.a, shared));
    CHECK(rig.a.clientFrames.count(vthost::proto::PduType::CloseWorkspace) == 0);
}

TEST_CASE("WindowSwitcher: the active tab's colour pick right after a switch names the daemon switched to",
          "[switch][sidebar][jump][tabcolor]")
{
    auto rig = SwitchRig {};
    auto& window = *rig.window->controller;
    window.setTabBarVisibility(contour::config::TabBarVisibility::Never);
    window.sidebar().refreshNow();
    auto picked = QSignalSpy { &window, &contour::window::WindowController::sidebarTabColorPickRequested };
    auto told = Told {};
    auto completion = told.completion();
    auto const left = rig.app.manager().daemonRefOf(rig.shown());
    rig.switcher->request(rig.shown(),
                          to(daemonB(), "2"),
                          SwitchMode::Switch,
                          contour::session::UserSwitch,
                          [&](std::expected<SwitchOutcome, std::string> outcome) {
                              window.beginActiveTabColorPick();
                              completion(std::move(outcome));
                          });
    REQUIRE(waitUntil([&] { return told.outcome.has_value(); }, 2000));
    REQUIRE(*told.outcome == SwitchOutcome::Switched);
    REQUIRE(picked.count() == 1);
    auto const key = rowKeyOf(picked.front()[1]);
    auto const onB = rig.app.manager().daemonRefOf(rig.shown());
    REQUIRE(onB.has_value());
    CHECK(onB != left);
    CHECK(key.daemon == *onB);
    CHECK(key.workspace == rig.bWorkspaces[1]);
    CHECK(key.instance
          == rig.app.manager().daemonInstanceOf(rig.shown()).value_or(contour::window::DaemonInstance {}));

    // The pick, committed with the key it was opened with, colours B's tab; A's of the same ids keeps none.
    auto const teal = vtbackend::RGBColor { 0x00, 0x80, 0x80 };
    window.setRowColor(picked.front()[1], QColor { teal.red, teal.green, teal.blue });
    auto const colourOn = [&](DaemonFixture& daemon, uint64_t workspace) {
        return daemon.onDaemon([&]() -> std::optional<vtbackend::RGBColor> {
            auto* const held = daemon.host->model().workspace(vtworkspace::WorkspaceId { workspace });
            return held != nullptr && held->tabAt(0) != nullptr ? held->tabAt(0)->color() : std::nullopt;
        });
    };
    CHECK(waitUntil([&] { return colourOn(rig.b, rig.bWorkspaces[1]) == teal; }, 2000));
    drainGuiEvents();
    CHECK_FALSE(colourOn(rig.a, rig.aWorkspaces[1]).has_value());
}

// Review (Phase 6 round 2, item 2): a close asked of the window's own daemon that restarted while the dialog
// was open names the run that ended; the new run's node of the same ids is not what the user was asked.
TEST_CASE("WindowSwitcher: a close of the own daemon's ended run closes nothing", "[switch][sidebar]")
{
    auto rig = SwitchRig {};
    auto& window = *rig.window->controller;
    auto const workspace = rig.aWorkspaces[1];
    auto asked =
        QSignalSpy { &window, &contour::window::WindowController::closeWorkspaceConfirmationRequested };
    rig.app.manager().closeWorkspace(window, workspace);
    REQUIRE(asked.count() == 1);
    auto stale = closeNodeOf(asked.front());
    REQUIRE(stale.row.instance
            == rig.app.manager().daemonInstanceOf(rig.shown()).value_or(contour::window::DaemonInstance {}));
    stale.row.instance = contour::window::DaemonInstance { stale.row.instance.value + 1 };

    // How long a close is given to reach A, in waitUntil()'s 5 ms attempts: the stale one's must not within
    // it, the control's must.
    constexpr auto CloseAttempts = 200;
    auto const closed = [&] {
        return !holdsWorkspace(rig.a, workspace);
    };
    window.confirmClose(QVariant::fromValue(stale));
    CHECK_FALSE(waitUntil(closed, CloseAttempts));

    // The control: the node as asked, of the run the window is on, closes it within the same time.
    window.confirmClose(asked.front()[0]);
    CHECK(waitUntil(closed, CloseAttempts));
}

// Review (pass 2, M1): the colour picker stays open as long as the close dialog does, and commits to the row
// key it was opened with: one of the own daemon's run that ended colours nothing of the run serving now.
TEST_CASE("WindowSwitcher: a colour pick of the own daemon's ended run colours nothing", "[switch][sidebar]")
{
    auto rig = SwitchRig {};
    auto& window = *rig.window->controller;
    // A sidebar placement draws no tab strip: the pick opens over the tab's sidebar row.
    window.setTabBarVisibility(contour::config::TabBarVisibility::Never);
    window.sidebar().refreshNow();
    auto picked = QSignalSpy { &window, &contour::window::WindowController::sidebarTabColorPickRequested };
    window.beginActiveTabColorPick();
    REQUIRE(picked.count() == 1);
    auto stale = rowKeyOf(picked.front()[1]);
    REQUIRE(stale.instance
            == rig.app.manager().daemonInstanceOf(rig.shown()).value_or(contour::window::DaemonInstance {}));
    stale.instance = contour::window::DaemonInstance { stale.instance.value + 1 };
    auto staleWorkspace = stale;
    staleWorkspace.kind = contour::window::SidebarRowKind::Workspace;

    // How long a colour is given to reach A, in waitUntil()'s 5 ms attempts: the stale pick's must not within
    // it, the control's must.
    constexpr auto ColourAttempts = 200;
    auto const teal = vtbackend::RGBColor { 0x00, 0x80, 0x80 };
    auto const tealColour = QColor { teal.red, teal.green, teal.blue };
    auto const colouredOnA = [&] {
        return rig.a.onDaemon([&] {
            auto const* const held =
                rig.a.host->model().workspace(vtworkspace::WorkspaceId { stale.workspace });
            auto const* const tab = held != nullptr ? held->tabAt(0) : nullptr;
            return (tab != nullptr && tab->color() == teal) || (held != nullptr && held->color() == teal);
        });
    };
    window.setRowColor(QVariant::fromValue(stale), tealColour);
    window.setRowColor(QVariant::fromValue(staleWorkspace), tealColour);
    CHECK_FALSE(waitUntil(colouredOnA, ColourAttempts));

    // The control: the pick as opened, of the run the window is on, colours it within the same time.
    window.setRowColor(picked.front()[1], tealColour);
    CHECK(waitUntil(colouredOnA, ColourAttempts));
}
// }}}

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
    CHECK(rig.a.visibilityOf(rig.aWorkspaces[0]) == WorkspaceVisibility::Shown);
    CHECK(other->settledLayout(rig.aWorkspaces[0]) != nullptr);
    other->stop();
}

TEST_CASE("WindowSwitcher: a window with no daemon behind it switches nowhere", "[switch][link]")
{
    auto app = contour::test::TestApp { std::make_unique<contour::test::MockPtySessionFactory>() };
    auto links = std::vector<std::unique_ptr<DaemonLink>> {};
    auto timeline = TestTimeline {};
    auto host = FakeApp { app.manager(), links, timeline, {} };
    auto pool = FakeObserverPool {};
    auto random = FixedRandom {};
    auto switcher = contour::remote::WindowSwitcher {
        app.manager(),
        host,
        pool,
        std::make_unique<FixtureLister>(std::map<std::string, DaemonFixture*> {}),
        [](contour::remote::SwitchPickRequest const&) { return std::expected<void, std::string> {}; },
        contour::remote::SwitchDeadline {
            .timeout = contour::remote::DefaultSwitchTimeout,
            .timer = [](std::chrono::milliseconds, std::function<void()> const&) {},
            .clock = host.clock },
        random
    };
    auto const local = contour::test::ScopedController { app.manager() };
    auto told = Told {};

    switcher.request(
        local.id, to(daemonB()), SwitchMode::Switch, contour::session::UserSwitch, told.completion());

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
    FakeObserverPool pool;
    FixedRandom random;
    contour::remote::WindowSwitcher switcher {
        app.manager(),
        host,
        pool,
        std::make_unique<FixtureLister>(std::map<std::string, DaemonFixture*> {}),
        [this](contour::remote::SwitchPickRequest pick) {
            picks.push_back(std::move(pick));
            return std::expected<void, std::string> {};
        },
        contour::remote::SwitchDeadline {
            .timeout = contour::remote::DefaultSwitchTimeout,
            .timer = [](std::chrono::milliseconds, std::function<void()> const&) {},
            .clock = host.clock },
        random
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
                         contour::session::UserSwitch,
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
    CHECK(named->name == "billing");
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
                          contour::session::UserSwitch,
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
    CHECK(rig.a.visibilityOf(*shown) == WorkspaceVisibility::Shown);
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

TEST_CASE(
    "WindowSwitcher: a cancel while the workspace is being created ends the jump, the workspace left listed",
    "[switch][link][switcher][jump]")
{
    // New Workspace on daemon B from the sidebar: the user cancels before B answered the creation. The jump
    // ends there, said as cancelled; what B made all the same stays, for the user to show or close.
    auto rig = SwitchRig {};
    auto const workspacesOnB = rig.b.onDaemon([&rig] { return rig.b.host->model().workspaceCount(); });
    auto const before = rig.windowSessions();
    auto said = std::vector<contour::window::JumpEvent> {};
    auto jumps = contour::window::SidebarJumps { *rig.switcher,
                                                 [&said](contour::window::JumpEvent event,
                                                         contour::window::SidebarTarget const&,
                                                         std::string const&) { said.push_back(event); },
                                                 [] {},
                                                 {} };
    jumps.createAndJump(rig.shown(),
                        contour::window::SidebarTarget { .daemon = contour::window::DaemonRef { "local:b" },
                                                         .instance = {},
                                                         .workspace = 0,
                                                         .tab = std::nullopt,
                                                         .pane = std::nullopt },
                        std::nullopt);
    // The daemon's answer is queued: the cancel comes first.
    REQUIRE(rig.lister->created.size() == 1);

    jumps.cancel(rig.shown(), contour::window::JumpMode::Here);

    REQUIRE_FALSE(said.empty());
    CHECK(said.back() == contour::window::JumpEvent::Cancelled);
    CHECK(jumps.pending() == contour::window::PendingJumps {});
    drainGuiEvents();
    CHECK(said.back() == contour::window::JumpEvent::Cancelled);
    CHECK(rig.windowLink() == &rig.launchLink());
    CHECK(rig.windowSessions() == before);
    CHECK(rig.b.onDaemon([&rig] { return rig.b.host->model().workspaceCount(); }) == workspacesOnB + 1);
}

TEST_CASE("WindowSwitcher: a workspace that cannot be created is said in the window, and nothing switches",
          "[switch][link][switcher]")
{
    // Asked for from the switcher, which has closed by the time the daemon answers: the window says why.
    auto rig = SwitchRig {};
    auto const before = rig.windowSessions();
    auto told = Told {};
    // A daemon the fixture's lister knows nothing of: `workspace.create` cannot reach it.
    auto const nowhere = DaemonAddress { .kind = SwitchDaemonKind::Name, .name = "nowhere" };
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
                    window, std::move(target), mode, contour::session::UserSwitch, told.completion());
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

    api.callFromPaneOf(rig.aWorkspaces[0], { .toName = "b", .toWorkspace = "2" });

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

    api.callFromPaneOf(rig.aWorkspaces[0], { .toName = "b", .toWorkspace = "2" });
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

    api.callFromPaneOf(rig.aWorkspaces[0], { .toName = "b", .toWorkspace = "2" });
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

    api.callFromPaneOf(rig.aWorkspaces[0], { .toName = "b", .toWorkspace = "2" });
    contour::test::answerShown(awaitSwitchQuestion(rig), PermissionDecision::Deny, PermissionMemory::Session);
    auto const first = awaitAnswer(api);
    CHECK(first.outcome == "refused");
    CHECK(first.detail == contour::remote::SwitchRefusedDetail);

    // Another workspace of the same daemon is the same pair: refused at once, nothing asked.
    api.callFromPaneOf(rig.aWorkspaces[0], { .toName = "b", .toWorkspace = "1" });
    auto const second = awaitAnswer(api);
    CHECK(second.outcome == "refused");
    auto const* const session = rig.app.manager().actingSessionOfWindow(rig.shown());
    REQUIRE(session != nullptr);
    CHECK_FALSE(session->pendingPermissionAsk().has_value());
    CHECK(rig.windowLink() == &rig.launchLink());
    CHECK(rig.host.started == 0);
    CHECK_FALSE(rig.showsNoneOf(before));
    CHECK(rig.a.visibilityOf(rig.aWorkspaces[0]) == WorkspaceVisibility::Shown);
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
                .kind = SwitchDaemonKind::Name, .daemon = "b", .workspace = "2" },
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

    api.callFromPaneOf(rig.aWorkspaces[0], { .toName = "b", .toWorkspace = "2" });

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

// {{{ Jumps through the presence (spec §5): a daemon the sidebar lists is resolved from its observer's
// directory, and its observer taken over; the daemon left keeps the window's connection as its observer.
namespace
{

/// Daemon B as the presence names it.
[[nodiscard]] contour::window::DaemonRef refB()
{
    return contour::window::localDaemonRef("b");
}

/// Has the rig's presence list B, connected, through an observer of it it hands over to a switch.
/// @return B's directory, as the observer holds it.
contour::window::WorkspaceDirectory observeB(SwitchRig& rig)
{
    auto observer =
        makeNativeController(rig.b.endpoint(),
                             rig.timeline,
                             ControllerOptions { .backoff = { .first = 10ms, .ceiling = 50ms },
                                                 .role = vthost::proto::ConnectionRole::Observe });
    observer->startConnecting();
    REQUIRE(waitUntil([&] {
        return observer->connectionState() == vthost::client::ConnectionState::Connected
               && observer->directory().workspaces.size() >= rig.bWorkspaces.size();
    }));
    auto section = contour::window::DaemonSection {};
    section.daemon = refB();
    section.instance = contour::window::DaemonInstance { observer->daemonInstance().value_or(0) };
    section.name = "b";
    section.state = contour::window::DaemonLinkState::Connected;
    section.directory = observer->directory();
    section.feed = contour::window::SectionFeed::Observer;
    auto directory = section.directory;
    rig.pool.sections.insert_or_assign(refB(), std::move(section));
    rig.pool.observers.insert_or_assign(refB(), std::move(observer));
    return directory;
}

/// @return A jump's target on @p daemon: the workspace of @p id in @p directory, by its control id, named in
///         run @p instance.
[[nodiscard]] SwitchTarget jumpTarget(DaemonAddress const& daemon,
                                      contour::window::WorkspaceDirectory const& directory,
                                      uint64_t id,
                                      contour::window::DaemonInstance instance)
{
    auto const* const workspace = contour::window::findWorkspace(directory, id);
    REQUIRE(workspace != nullptr);
    return SwitchTarget { .kind = daemon.kind,
                          .daemon = daemon.name,
                          .workspace = vtcontrol::ObjectRef { .kind = vtcontrol::ObjectKind::Workspace,
                                                              .uuid = { .bytes = workspace->controlId } },
                          .focus = {},
                          .instance = instance };
}

/// @return The run of B the presence lists.
[[nodiscard]] contour::window::DaemonInstance listedRunOfB(SwitchRig const& rig)
{
    return rig.pool.sections.at(refB()).instance;
}

/// Asks the switcher without waiting.
void ask(SwitchRig const& rig,
         SwitchTarget target,
         Told& told,
         contour::session::SwitchWaiter waiter = contour::session::UserSwitch)
{
    rig.switcher->request(rig.shown(), std::move(target), SwitchMode::Switch, waiter, told.completion());
}

} // namespace

TEST_CASE("WindowSwitcher: a jump to an observed daemon adopts its connection without a listing dial",
          "[switch][link][presence]")
{
    auto rig = SwitchRig {};
    auto const directory = observeB(rig);
    auto const connectionsBefore = connections(rig.b);

    auto const outcome = rig.request(jumpTarget(daemonB(), directory, rig.bWorkspaces[1], listedRunOfB(rig)));

    REQUIRE(outcome == SwitchOutcome::Switched);
    CHECK(rig.lister->listings == 0);
    CHECK(rig.pool.adoptions == 1);
    CHECK(rig.host.adopted == 1);
    CHECK(rig.host.started == 1);
    auto* const now = rig.windowLink();
    REQUIRE(now != nullptr);
    CHECK(now->address() == daemonB());
    CHECK(now->workspaceOf(rig.shown()) == rig.bWorkspaces[1]);
    // The observer's own connection shows the workspace now: no second one was dialled.
    CHECK(connections(rig.b) == connectionsBefore);
}

TEST_CASE("WindowSwitcher: a jump to a daemon another window is on opens a new link, as New Window",
          "[switch][link][presence]")
{
    // The presence lists B but holds no observer of it (a window link feeds its section): the switch is
    // resolved from the section, and dials a link of its own.
    auto rig = SwitchRig {};
    auto const directory = observeB(rig);
    rig.pool.observers.at(refB())->stop();
    rig.pool.observers.erase(refB());

    auto const outcome = rig.request(jumpTarget(daemonB(), directory, rig.bWorkspaces[1], listedRunOfB(rig)));

    REQUIRE(outcome == SwitchOutcome::Switched);
    CHECK(rig.lister->listings == 0);
    CHECK(rig.host.adopted == 0);
    CHECK(rig.host.started == 1);
    REQUIRE(rig.windowLink() != nullptr);
    CHECK(rig.windowLink()->workspaceOf(rig.shown()) == rig.bWorkspaces[1]);
}

TEST_CASE("WindowSwitcher: the left daemon becomes observed only when no other window is on it",
          "[switch][link][presence]")
{
    auto const refA = contour::window::localDaemonRef("a");
    for (auto const wanted: { contour::remote::ObserverWanted::Yes, contour::remote::ObserverWanted::No })
    {
        INFO("wanted " << static_cast<int>(wanted));
        auto rig = SwitchRig {};
        auto const directory = observeB(rig);
        rig.pool.wanted = wanted;
        auto const leftLink = rig.launchLink().id();
        auto const before = rig.windowSessions();

        REQUIRE(rig.request(jumpTarget(daemonB(), directory, rig.bWorkspaces[1], listedRunOfB(rig)))
                == SwitchOutcome::Switched);

        // A's connection comes back once the window's old panes are gone -- their exit seen, as each one's
        // watcher sees its closed device -- or is stopped.
        for (auto const& session: before)
            if (session && session->terminal().device().isClosed())
                session->onClosed();
        REQUIRE(waitUntil([&] {
            drainGuiEvents();
            return rig.pool.heldFor(refA) + rig.pool.stoppedFor(refA) == 1;
        }));
        if (wanted == contour::remote::ObserverWanted::Yes)
        {
            CHECK(rig.pool.heldFor(refA) == 1);
            REQUIRE(rig.pool.expected.size() == 1);
            CHECK(rig.pool.expected.front().first == refA);
            CHECK(rig.pool.expected.front().second == leftLink);
        }
        else
        {
            CHECK(rig.pool.stoppedFor(refA) == 1);
            CHECK(rig.pool.expected.empty());
        }
    }
}

TEST_CASE("WindowSwitcher: a second click while connecting lands on the second target",
          "[switch][link][presence]")
{
    // Review Focus 5: the jump to B waits for its promotion; the user then clicks A's second workspace.
    auto rig = SwitchRig {};
    auto const directory = observeB(rig);
    rig.b.holdEverything();
    auto first = Told {};
    ask(rig, jumpTarget(daemonB(), directory, rig.bWorkspaces[1], listedRunOfB(rig)), first);
    REQUIRE(rig.host.adopted == 1);
    CHECK_FALSE(first.outcome.has_value());

    auto const second = rig.request(to(daemonA(), "2"));

    REQUIRE(second == SwitchOutcome::Switched);
    REQUIRE(first.outcome.has_value());
    CHECK_FALSE(first.outcome->has_value());
    CHECK(rig.windowLink() == &rig.launchLink());
    CHECK(rig.launchLink().workspaceOf(rig.shown()) == rig.aWorkspaces[1]);
    // B's link was abandoned, and its observer went back to the presence.
    CHECK(waitUntil([&] { return rig.pool.heldFor(refB()) + rig.pool.stoppedFor(refB()) == 1; }));
    CHECK(rig.pool.heldFor(refB()) == 1);
    rig.b.releaseHeld();
}

TEST_CASE("WindowSwitcher: a second click while the listing runs supersedes it", "[switch][link][presence]")
{
    auto rig = SwitchRig {};
    rig.lister->holding = true;
    auto first = Told {};
    ask(rig, to(daemonB(), "2"), first);
    REQUIRE(rig.lister->held.size() == 1);

    auto const second = rig.request(to(daemonA(), "2"));

    REQUIRE(second == SwitchOutcome::Switched);
    REQUIRE(first.outcome.has_value());
    CHECK_FALSE(first.outcome->has_value());
    // The listing's late answer is nobody's: nothing is dialled, and the window stays where it went.
    rig.lister->release();
    drainGuiEvents();
    CHECK(rig.host.started == 0);
    CHECK(rig.windowLink() == &rig.launchLink());
    CHECK(rig.launchLink().workspaceOf(rig.shown()) == rig.aWorkspaces[1]);
}

TEST_CASE("WindowSwitcher: an Open in New Window and a switch in place never end each other",
          "[switch][link][presence]")
{
    // Each lands: the window opening elsewhere is not what a click in this one replaces, nor the other way.
    SECTION("Open first, then a switch in place")
    {
        auto rig = SwitchRig {};
        rig.b.holdNewConnectionsAtLayout();
        auto opening = Told {};
        rig.switcher->request(rig.shown(),
                              to(daemonB(), "2"),
                              SwitchMode::Open,
                              contour::session::UserSwitch,
                              opening.completion());
        REQUIRE(waitUntil([&] { return rig.host.started == 1; }, 2000));

        REQUIRE(rig.request(to(daemonA(), "2")) == SwitchOutcome::Switched);
        CHECK(rig.launchLink().workspaceOf(rig.shown()) == rig.aWorkspaces[1]);
        CHECK_FALSE(opening.outcome.has_value());

        rig.b.releaseHeld();
        REQUIRE(waitUntil([&] { return opening.outcome.has_value(); }, 2000));
        CHECK(*opening.outcome == SwitchOutcome::Switched);
        CHECK(waitUntil([&] { return rig.host.opened.size() == 1; }));
    }
    SECTION("a switch in place first, then Open")
    {
        auto rig = SwitchRig {};
        rig.b.holdNewConnectionsAtLayout();
        auto switching = Told {};
        ask(rig, to(daemonB(), "2"), switching);
        REQUIRE(waitUntil([&] { return rig.host.started == 1; }, 2000));

        REQUIRE(rig.request(to(daemonA(), "2"), SwitchMode::Open) == SwitchOutcome::Switched);
        CHECK(waitUntil([&] { return rig.host.opened.size() == 1; }));
        CHECK_FALSE(switching.outcome.has_value());

        rig.b.releaseHeld();
        REQUIRE(waitUntil([&] { return switching.outcome.has_value(); }, 2000));
        CHECK(*switching.outcome == SwitchOutcome::Switched);
        REQUIRE(rig.windowLink() != nullptr);
        CHECK(rig.windowLink()->address() == daemonB());
    }
}

TEST_CASE("WindowSwitcher: a cancel ends the switch of its own mode alone", "[switch][link][presence]")
{
    auto rig = SwitchRig {};
    rig.b.holdNewConnectionsAtLayout();
    auto opening = Told {};
    rig.switcher->request(rig.shown(),
                          to(daemonB(), "2"),
                          SwitchMode::Open,
                          contour::session::UserSwitch,
                          opening.completion());
    REQUIRE(waitUntil([&] { return rig.host.started == 1; }, 2000));

    rig.switcher->cancel(rig.shown(), SwitchMode::Switch);
    CHECK_FALSE(opening.outcome.has_value());

    rig.switcher->cancel(rig.shown(), SwitchMode::Open);
    REQUIRE(opening.outcome.has_value());
    CHECK_FALSE(opening.outcome->has_value());
    rig.b.releaseHeld();
}

TEST_CASE("WindowSwitcher: a program's switch does not end a user's jump", "[switch][link][presence]")
{
    auto rig = SwitchRig {};
    rig.lister->holding = true;
    auto first = Told {};
    ask(rig, to(daemonB(), "2"), first);
    REQUIRE(rig.lister->held.size() == 1);

    auto program = Told {};
    ask(rig,
        to(daemonA(), "2"),
        program,
        contour::session::SwitchWaiter { .landBy = rig.host.clock.now() + 10s,
                                         .told = contour::session::SwitchTold::BeforeLeaving });

    REQUIRE(program.outcome.has_value());
    CHECK_FALSE(program.outcome->has_value());
    CHECK_FALSE(first.outcome.has_value());
    rig.lister->release();
    REQUIRE(waitUntil([&] { return first.outcome.has_value(); }, 2000));
    CHECK(*first.outcome == SwitchOutcome::Switched);
    REQUIRE(rig.windowLink() != nullptr);
    CHECK(rig.windowLink()->address() == daemonB());
}

TEST_CASE("WindowSwitcher: Cancel leaves the window as it was and gives the observer back",
          "[switch][link][presence]")
{
    auto rig = SwitchRig {};
    auto const directory = observeB(rig);
    auto const before = rig.windowSessions();
    rig.b.holdEverything();
    auto told = Told {};
    ask(rig, jumpTarget(daemonB(), directory, rig.bWorkspaces[1], listedRunOfB(rig)), told);
    REQUIRE(rig.host.adopted == 1);

    rig.switcher->cancel(rig.shown(), SwitchMode::Switch);

    REQUIRE(told.outcome.has_value());
    CHECK_FALSE(told.outcome->has_value());
    CHECK(rig.windowLink() == &rig.launchLink());
    CHECK(rig.launchLink().workspaceOf(rig.shown()) == rig.aWorkspaces[0]);
    CHECK_FALSE(rig.showsNoneOf(before));
    CHECK(waitUntil([&] { return rig.pool.heldFor(refB()) + rig.pool.stoppedFor(refB()) == 1; }));
    CHECK(rig.pool.heldFor(refB()) == 1);
    rig.b.releaseHeld();
}

TEST_CASE("WindowSwitcher: a timed-out jump gives the observer back", "[switch][link][presence][timeout]")
{
    auto rig = SwitchRig {};
    auto const directory = observeB(rig);
    rig.b.holdEverything();
    auto told = Told {};
    ask(rig, jumpTarget(daemonB(), directory, rig.bWorkspaces[1], listedRunOfB(rig)), told);
    REQUIRE(rig.host.adopted == 1);

    rig.timer.fireAll();

    REQUIRE(told.outcome.has_value());
    CHECK_FALSE(told.outcome->has_value());
    CHECK(rig.windowLink() == &rig.launchLink());
    CHECK(waitUntil([&] { return rig.pool.heldFor(refB()) + rig.pool.stoppedFor(refB()) == 1; }));
    CHECK(rig.pool.heldFor(refB()) == 1);
    rig.b.releaseHeld();
}

TEST_CASE("WindowSwitcher: a password host asks in the jumping window", "[switch][link][presence]")
{
    SECTION("unlisted: the link's configuration is resolved for the window")
    {
        auto rig = SwitchRig {};
        REQUIRE(rig.request(to(daemonB(), "2")) == SwitchOutcome::Switched);
        REQUIRE_FALSE(rig.host.askingWindows.empty());
        CHECK(std::ranges::all_of(rig.host.askingWindows, [&](auto window) {
            return window == rig.window->controller->displayedWorkspace();
        }));
    }
    SECTION("listed, waiting for the user: the link's configuration is resolved for the window all the same")
    {
        auto rig = SwitchRig {};
        auto section = contour::window::DaemonSection {};
        section.daemon = refB();
        section.name = "b";
        section.state = contour::window::DaemonLinkState::NeedsInput;
        rig.pool.sections.insert_or_assign(refB(), std::move(section));
        auto const window = rig.shown();

        REQUIRE(rig.request(to(daemonB(), "2")) == SwitchOutcome::Switched);
        REQUIRE_FALSE(rig.host.askingWindows.empty());
        CHECK(rig.host.askingWindows.back() == window);
    }
}

/// Drops @p rig's B connections and waits for the window's link to dial B again.
/// @return Whether the redial went through the transport the window's link was configured with.
[[nodiscard]] bool redialsThroughWindowTransport(SwitchRig& rig)
{
    REQUIRE(rig.host.windowTransports.contains("b"));
    auto const& transport = *rig.host.windowTransports.at("b");
    auto const before = transport.dials();
    rig.b.dropConnections();
    return waitUntil([&] { return transport.dials() > before; }, 5000);
}

TEST_CASE("WindowSwitcher: a window's link to a listed daemon dials every time as the window's own",
          "[switch][link][presence]")
{
    // A saved host that asks for a password is listed: each reconnect of the window's link -- the footer's
    // Reconnect among them -- asks in the window again, and may start the remote daemon, as for any host.
    SECTION("dialled for the jump")
    {
        auto rig = SwitchRig {};
        auto section = contour::window::DaemonSection {};
        section.daemon = refB();
        section.name = "b";
        section.state = contour::window::DaemonLinkState::NeedsInput;
        rig.pool.sections.insert_or_assign(refB(), std::move(section));

        REQUIRE(rig.request(to(daemonB(), "2")) == SwitchOutcome::Switched);
        REQUIRE(rig.windowLink() != nullptr);
        REQUIRE(rig.windowLink()->address() == daemonB());

        CHECK(redialsThroughWindowTransport(rig));
    }
    SECTION("adopted from the presence's observer")
    {
        auto rig = SwitchRig {};
        auto const directory = observeB(rig);

        REQUIRE(rig.request(jumpTarget(daemonB(), directory, rig.bWorkspaces[1], listedRunOfB(rig)))
                == SwitchOutcome::Switched);
        REQUIRE(rig.host.adopted == 1);

        CHECK(redialsThroughWindowTransport(rig));
    }
}

TEST_CASE("WindowSwitcher: a jump naming an earlier daemon run fails and switches nothing",
          "[switch][link][presence]")
{
    auto rig = SwitchRig {};
    auto const directory = observeB(rig);
    auto const earlier = contour::window::DaemonInstance { listedRunOfB(rig).value + 1 };

    auto const outcome = rig.request(jumpTarget(daemonB(), directory, rig.bWorkspaces[1], earlier));

    REQUIRE_FALSE(outcome.has_value());
    CHECK(outcome.error() == "the daemon restarted");
    CHECK(rig.lister->listings == 0);
    CHECK(rig.host.started == 0);
    CHECK(rig.pool.adoptions == 0);
    CHECK(rig.windowLink() == &rig.launchLink());
}
// }}}

// {{{ The application's quit, as the GUI verb orders it
TEST_CASE("ContourGuiApp: a quit while a link waits to give its daemon back dials no observer",
          "[switch][presence][adopt]")
{
    // The window jumped from A to B: A's link waits to give its connection back to the presence. The GUI
    // verb's event loop returns, and the hosting is released as it orders it: no observer is dialled for A.
    auto a = DaemonFixture {};
    auto b = DaemonFixture {};
    auto timeline = TestTimeline {};
    auto observersMade = 0;
    auto const hostA = DaemonAddress { .kind = SwitchDaemonKind::Host, .name = "a" };
    auto const hostB = DaemonAddress { .kind = SwitchDaemonKind::Host, .name = "b" };
    auto const endpointOf = [&](contour::window::DaemonRef const& daemon) {
        return daemon.value == "host:a" ? a.endpoint() : b.endpoint();
    };
    auto app = contour::test::TestApp {
        std::make_unique<contour::test::MockPtySessionFactory>(),
        nullptr,
        nullptr,
        nullptr,
        contour::test::TestApp::testHostIdentity(),
        nullptr,
        {},
        nullptr,
        contour::platform::nullAppearanceServices(),
        nullptr,
        contour::config::HostPlatform::Other,
        nullptr,
        contour::remote::PresenceOverrides {
            .makeObserver =
                [&](contour::window::DaemonRef const& daemon,
                    std::shared_ptr<vthost::ITransportFactory> transport,
                    vthost::client::BackoffSchedule backoff) {
                    ++observersMade;
                    return makeNativeController(
                        endpointOf(daemon),
                        timeline,
                        ControllerOptions { .backoff = backoff,
                                            .role = vthost::proto::ConnectionRole::Observe,
                                            .transport = std::move(transport) });
                },
            .refreshTimer = [](std::chrono::milliseconds, std::function<void()> const&) {},
            .discovery = noLocalDaemons(),
            .settings =
                [] {
                    return contour::remote::ListingSettings {
                        .hosts = { { "a", contour::window::SidebarListingChoice::Listed },
                                   { "b", contour::window::SidebarListingChoice::Listed } },
                        .shownByUser = {},
                        .hiddenByUser = {},
                    };
                },
            .observerTransport = [&](contour::window::DaemonRef const& daemon)
                -> std::expected<std::shared_ptr<vthost::ITransportFactory>, contour::remote::DialRefusal> {
                return makeTestTlsTransport(endpointOf(daemon));
            },
            .windowLinks = {},
        }
    };
    auto& onA = app.app().startDaemonLink(linkConfig(a.endpoint(), timeline, hostA));
    auto& onB = app.app().startDaemonLink(linkConfig(b.endpoint(), timeline, hostB));
    REQUIRE(pollUntilAttached(onA, 10s).has_value());
    REQUIRE(pollUntilAttached(onB, 10s).has_value());
    drainGuiEvents();
    REQUIRE(observersMade == 0);
    auto& pool = app.app().observerPool();
    auto const daemonOfA = contour::window::DaemonRef { "host:a" };
    auto const handOff = pool.expectGiveBack(daemonOfA, onA.id());
    onA.leaveDaemon(contour::remote::LeaveKind::KeepAsObserver,
                    [&pool, daemonOfA, handOff](std::unique_ptr<contour::remote::NativeController> returned) {
                        pool.giveBack(daemonOfA, handOff, std::move(returned));
                    });

    app.app().endGuiRun();
    app.app().releaseHosting();
    drainGuiEvents();

    CHECK(observersMade == 0);
}
// }}}
