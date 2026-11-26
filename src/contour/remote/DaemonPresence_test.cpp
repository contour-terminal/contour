// SPDX-License-Identifier: Apache-2.0
#include <contour/platform/Notifier.hpp>
#include <contour/remote/DaemonPresence.hpp>
#include <contour/remote/NativeController.hpp>
#include <contour/remote/PresenceTransport.hpp>
#include <contour/remote/testing/DaemonFixture.hpp>
#include <contour/test/RecordingNotifier.hpp>

#include <core/net/testing/InMemoryTransport.hpp>
#include <core/testing/ScopedTempDir.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <functional>
#include <iterator>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <vthost/Transport.hpp>
#include <vthost/testing/FakeDaemonControl.hpp>

using contour::remote::AppVisibility;
using contour::remote::DaemonPresence;
using contour::remote::DialRefusal;
using contour::remote::ListingSettings;
using contour::remote::PresenceDeps;
using contour::remote::WindowLinkView;
using contour::remote::testing::controllerConfig;
using contour::remote::testing::ControllerOptions;
using contour::remote::testing::DaemonFixture;
using contour::remote::testing::makeNativeController;
using contour::remote::testing::makeTestTlsTransport;
using contour::remote::testing::seedWorkspaces;
using contour::remote::testing::TestTimeline;
using contour::remote::testing::waitUntil;
using contour::window::DaemonLinkState;
using contour::window::DaemonRef;
using contour::window::SectionFeed;
using vthost::client::ConnectionState;
using vtworkspace::WorkspaceId;
using namespace std::chrono_literals;

namespace
{
using contour::test::RecordingNotifier;

using Dialled = std::expected<std::shared_ptr<vthost::ITransportFactory>, DialRefusal>;

/// A transport whose every dial fails as @c kind says.
class FailingTransport final: public vthost::ITransportFactory
{
  public:
    explicit FailingTransport(vthost::TransportErrorKind kind): _kind(kind) {}

    [[nodiscard]] core::async::Task<
        std::expected<std::unique_ptr<vthost::Connection>, vthost::TransportError>>
    connect(core::net::EventLoop& /*loop*/) override
    {
        return fail(_kind);
    }

    [[nodiscard]] std::string describe() const override { return "failing"; }

  private:
    [[nodiscard]] static core::async::Task<
        std::expected<std::unique_ptr<vthost::Connection>, vthost::TransportError>>
    fail(vthost::TransportErrorKind kind)
    {
        co_return std::unexpected { vthost::TransportError { .kind = kind, .message = "no luck" } };
    }

    vthost::TransportErrorKind _kind;
};

/// @return A dial that failed as @p kind says.
[[nodiscard]] core::async::Task<std::expected<std::unique_ptr<vthost::Connection>, vthost::TransportError>>
failedWith(vthost::TransportErrorKind kind)
{
    co_return std::unexpected { vthost::TransportError { .kind = kind, .message = "no luck" } };
}

/// A transport whose connection is made and hung up on at once: the daemon never answers the hello.
class HangingUpTransport final: public vthost::ITransportFactory
{
  public:
    [[nodiscard]] core::async::Task<
        std::expected<std::unique_ptr<vthost::Connection>, vthost::TransportError>>
    connect(core::net::EventLoop& loop) override
    {
        auto pair = core::net::testing::makeSocketPair(loop);
        pair->first.reset();
        return connected(std::make_unique<vthost::Connection>(
            vthost::Connection { .lifetime = {}, .socket = std::move(pair->second), .token = {} }));
    }

    [[nodiscard]] std::string describe() const override { return "hanging up"; }

  private:
    [[nodiscard]] static core::async::Task<
        std::expected<std::unique_ptr<vthost::Connection>, vthost::TransportError>>
    connected(std::unique_ptr<vthost::Connection> connection)
    {
        co_return std::move(connection);
    }
};

/// Whether the daemon a StoppableTransport reaches answers its dials.
enum class DaemonServing : uint8_t
{
    Stopped = 0, ///< Every dial finds it absent.
    Serving,     ///< Every dial reaches it.
};

/// Whether the daemon a StoppableTransport reaches serves, and how often it was dialled.
struct StoppableDaemon
{
    std::atomic<DaemonServing> serving = DaemonServing::Serving; ///< Set by the test; read by each dial.
    std::atomic<int> dials = 0;                                  ///< Every dial, served or not.
};

/// Reaches a daemon while it serves; finds it absent once stopped.
class StoppableTransport final: public vthost::ITransportFactory
{
  public:
    StoppableTransport(std::shared_ptr<vthost::ITransportFactory> live,
                       std::shared_ptr<StoppableDaemon> daemon):
        _live(std::move(live)), _daemon(std::move(daemon))
    {
    }

    [[nodiscard]] core::async::Task<
        std::expected<std::unique_ptr<vthost::Connection>, vthost::TransportError>>
    connect(core::net::EventLoop& loop) override
    {
        ++_daemon->dials;
        if (_daemon->serving == DaemonServing::Serving)
            return _live->connect(loop);
        return failedWith(vthost::TransportErrorKind::DaemonAbsent);
    }

    [[nodiscard]] std::string describe() const override { return _live->describe(); }

  private:
    std::shared_ptr<vthost::ITransportFactory> _live;
    std::shared_ptr<StoppableDaemon> _daemon;
};

/// A discovery that answers at once with the labels a test set.
class FakeDiscovery final: public contour::cli::IDaemonDiscovery
{
  public:
    std::vector<contour::cli::DiscoveredDaemon> discover() override
    {
        if (discovering)
            discovering();
        auto found = std::vector<contour::cli::DiscoveredDaemon> {};
        for (auto const& label: running)
            found.push_back({ .label = label, .socket = {}, .workspaces = std::nullopt });
        return found;
    }

    std::vector<std::string> running; ///< Set before a refresh, never during one.
    /// Run first by each discovery, off the GUI thread: what happens just before it looks.
    std::function<void()> discovering;
};

DaemonRef const Starfive = contour::window::hostDaemonRef("starfive");
DaemonRef const Default = contour::window::localDaemonRef("default");

/// A window link of this process on @p daemon.
[[nodiscard]] WindowLinkView windowOn(DaemonRef daemon,
                                      contour::remote::NativeController* controller = nullptr)
{
    return WindowLinkView { .link = {},
                            .daemon = std::move(daemon),
                            .instance = {},
                            .name = {},
                            .locality = contour::window::DaemonLocality::Local,
                            .directory = {},
                            .state = DaemonLinkState::Connected,
                            .shownWorkspaces = {},
                            .controller = controller };
}

/// A presence over a fixture daemon: the saved host `starfive` is the fixture, listed by its configuration,
/// and a window of this process is on `local:default`.
struct PresenceRig
{
    DaemonFixture& daemon;
    TestTimeline timeline {};
    std::shared_ptr<FakeDiscovery> discovery = std::make_shared<FakeDiscovery>();
    ListingSettings settings { .hosts = { { "starfive", contour::window::SidebarListingChoice::Listed } },
                               .shownByUser = {},
                               .hiddenByUser = {} };
    std::vector<WindowLinkView> links { windowOn(Default) };
    vthost::client::BackoffSchedule backoff { .first = 10ms, .ceiling = 50ms };
    int observersMade = 0;
    int observerDials = 0;
    std::function<Dialled(DaemonRef const&)> observerDial = [this](DaemonRef const&) -> Dialled {
        return makeTestTlsTransport(daemon.endpoint());
    };
    std::vector<WorkspaceId> interactiveAsked;
    std::vector<std::pair<std::chrono::milliseconds, std::function<void()>>> timers;
    AppVisibility visibility = AppVisibility::Visible;
    contour::config::SidebarDaemons stored;                        ///< The GUI state's listing choices.
    std::vector<contour::config::SidebarDaemons> persisted;        ///< Each listing written, as written.
    int settingsReads = 0;                                         ///< How often the settings were read.
    std::vector<vthost::client::BackoffSchedule> observerBackoffs; ///< Each observer's, as made.
    std::vector<DaemonRef> started;
    RecordingNotifier notifier;
    int rings = 0;                         ///< How often the window focused last was asked to ring.
    std::vector<unsigned> observedWaiting; ///< Each count of the observed daemons' waiting tabs, as told.
    std::vector<contour::remote::NativeController*> observers; ///< Every observer made, oldest first.
    std::unique_ptr<DaemonPresence> presence = std::make_unique<DaemonPresence>(deps());

    explicit PresenceRig(DaemonFixture& fixture): daemon(fixture) {}
    PresenceRig(PresenceRig const&) = delete;
    PresenceRig& operator=(PresenceRig const&) = delete;
    PresenceRig(PresenceRig&&) = delete;
    PresenceRig& operator=(PresenceRig&&) = delete;
    ~PresenceRig() { presence.reset(); }

    /// Makes the presence afresh, from the rig as it is now (its backoff among it).
    void rebuild()
    {
        presence.reset();
        presence = std::make_unique<DaemonPresence>(deps());
    }

    [[nodiscard]] PresenceDeps deps()
    {
        return PresenceDeps {
            .discovery = discovery,
            .settings =
                [this] {
                    ++settingsReads;
                    return settings;
                },
            .observerTransport =
                [this](DaemonRef const& ref) {
                    ++observerDials;
                    return observerDial(ref);
                },
            .interactiveTransport = [this](DaemonRef const&, WorkspaceId window) -> Dialled {
                interactiveAsked.push_back(window);
                return makeTestTlsTransport(daemon.endpoint());
            },
            .makeObserver =
                [this](DaemonRef const&,
                       std::shared_ptr<vthost::ITransportFactory> transport,
                       vthost::client::BackoffSchedule schedule) {
                    ++observersMade;
                    observerBackoffs.push_back(schedule);
                    auto made = makeNativeController(
                        daemon.endpoint(),
                        timeline,
                        ControllerOptions { .backoff = schedule,
                                            .role = vthost::proto::ConnectionRole::Observe,
                                            .transport = std::move(transport) });
                    observers.push_back(made.get());
                    return made;
                },
            .windowLinks = [this] { return links; },
            .refreshTimer =
                [this](std::chrono::milliseconds delay, std::function<void()> fire) {
                    timers.emplace_back(delay, std::move(fire));
                },
            .visibility = [this] { return visibility; },
            .startDaemon = [this](DaemonRef const& ref) { started.push_back(ref); },
            .updateListing =
                [this](std::function<void(contour::config::SidebarDaemons&)> const& change) {
                    change(stored);
                    persisted.push_back(stored);
                    settings.shownByUser = stored.shown;
                    settings.hiddenByUser = stored.hidden;
                },
            .elsewhereNotices =
                [this] {
                    return contour::remote::ElsewhereNotices {
                        .policy = {}, .bell = {}, .homeDirectory = "/home/test", .notifier = notifier
                    };
                },
            .ringLastFocusedWindow = [this](contour::config::Bell const&) { ++rings; },
            .observedWaitingChanged = [this](unsigned waiting) { observedWaiting.push_back(waiting); },
            .redialBackoff = backoff,
        };
    }

    /// Fires every timer armed so far.
    void fireTimers()
    {
        for (auto& timer: std::exchange(timers, {}))
            timer.second();
    }

    /// @return The redials armed so far: every timer but the periodic refresh's.
    [[nodiscard]] std::size_t redialsArmed() const
    {
        return static_cast<std::size_t>(std::ranges::count_if(timers, [](auto const& timer) {
            return timer.first != contour::remote::PresenceRefreshInterval;
        }));
    }

    /// @return @p ref's section; none while it is not listed.
    [[nodiscard]] std::optional<contour::window::DaemonSection> section(DaemonRef const& ref) const
    {
        auto const sections = presence->sections();
        auto const found = std::ranges::find(sections, ref, &contour::window::DaemonSection::daemon);
        return found != sections.end() ? std::optional { *found } : std::nullopt;
    }

    /// @return Whether @p ref's section is in @p state.
    [[nodiscard]] bool isIn(DaemonRef const& ref, DaemonLinkState state) const
    {
        auto const found = section(ref);
        return found && found->state == state;
    }

    /// @return How many connections the daemon serves now.
    [[nodiscard]] std::size_t connections() const
    {
        return daemon.onDaemon([this] { return daemon.live.size(); });
    }
};

/// @return A workspace of @p id, holding nothing.
[[nodiscard]] contour::window::DirectoryWorkspace workspaceOf(uint64_t id)
{
    auto workspace = contour::window::DirectoryWorkspace {};
    workspace.id = id;
    return workspace;
}

/// @return The pane @p session of @p directory; none when it lists none.
[[nodiscard]] std::optional<contour::window::DirectoryPane> paneOf(
    contour::window::WorkspaceDirectory const& directory, uint64_t session)
{
    for (auto const& workspace: directory.workspaces)
        for (auto const& tab: workspace.tabs)
            for (auto const& pane: tab.panes)
                if (pane.key.value == session)
                    return pane;
    return std::nullopt;
}

} // namespace

TEST_CASE("DaemonPresence: a listed daemon no window is on gets one observer", "[presence]")
{
    auto daemon = DaemonFixture {};
    auto rig = PresenceRig { daemon };
    rig.presence->refresh();

    REQUIRE(waitUntil([&] { return rig.isIn(Starfive, DaemonLinkState::Connected); }));
    CHECK(rig.section(Starfive)->feed == SectionFeed::Observer);
    CHECK(rig.observersMade == 1);
    auto const hellos = daemon.clientFrames.hellos();
    REQUIRE(hellos.size() == 1);
    CHECK(hellos.front().role == vthost::proto::ConnectionRole::Observe);

    // Another look keeps the one it has.
    rig.presence->refresh();
    rig.presence->linkSetChanged();
    CHECK_FALSE(waitUntil([&] { return daemon.clientFrames.hellos().size() > 1; }, 40));
    CHECK(rig.observersMade == 1);
}

TEST_CASE("DaemonPresence: a daemon a window is on gets no observer", "[presence]")
{
    auto daemon = DaemonFixture {};
    auto rig = PresenceRig { daemon };
    rig.links.push_back(windowOn(Starfive));
    rig.links.back().directory.workspaces.push_back(workspaceOf(3));
    rig.presence->refresh();

    CHECK_FALSE(waitUntil([&] { return !daemon.clientFrames.hellos().empty(); }, 40));
    CHECK(rig.observersMade == 0);
    REQUIRE(rig.section(Starfive).has_value());
    CHECK(rig.section(Starfive)->feed == SectionFeed::WindowLink);
    CHECK(rig.section(Starfive)->directory.workspaces.size() == 1);
}

TEST_CASE("DaemonPresence: a window landing on an observed daemon adopts it, no new dial", "[presence]")
{
    auto daemon = DaemonFixture {};
    auto rig = PresenceRig { daemon };
    rig.presence->refresh();
    REQUIRE(waitUntil([&] { return rig.isIn(Starfive, DaemonLinkState::Connected); }));

    auto adoption = rig.presence->adoptObserver(Starfive, {});
    auto& adopted = adoption.controller;
    REQUIRE(adopted != nullptr);
    CHECK(rig.presence->adoptObserver(Starfive, {}).controller == nullptr); // handed out once
    rig.links.push_back(windowOn(Starfive, adopted.get()));
    rig.presence->linkSetChanged();
    rig.presence->refresh();

    CHECK_FALSE(waitUntil([&] { return daemon.clientFrames.hellos().size() > 1; }, 40));
    CHECK(rig.observersMade == 1);
    CHECK(rig.section(Starfive)->feed == SectionFeed::WindowLink);
    CHECK(adopted->connectionState() == ConnectionState::Connected);
    CHECK(rig.connections() == 1);
}

TEST_CASE("DaemonPresence: giveBack makes it the daemon's observer once more", "[presence]")
{
    auto daemon = DaemonFixture {};
    auto const workspaces = seedWorkspaces(daemon, 1);
    auto rig = PresenceRig { daemon };
    rig.presence->refresh();
    REQUIRE(waitUntil([&] { return rig.isIn(Starfive, DaemonLinkState::Connected); }));

    // A window takes it, shows a workspace, and leaves the daemon again.
    auto adoption = rig.presence->adoptObserver(Starfive, {});
    auto& adopted = adoption.controller;
    REQUIRE(adopted != nullptr);
    auto const landed = std::make_shared<std::optional<std::expected<uint64_t, std::string>>>();
    REQUIRE(
        adopted
            ->promote(daemon.promotionOf(workspaces.front(),
                                         vtpty::PageSize { vtpty::LineCount(24), vtpty::ColumnCount(80) },
                                         std::nullopt),
                      makeTestTlsTransport(daemon.endpoint()),
                      [landed](std::expected<uint64_t, std::string> result) { *landed = std::move(result); })
            .has_value());
    REQUIRE(waitUntil([&] { return landed->has_value(); }));
    REQUIRE(adopted->demote(makeTestTlsTransport(daemon.endpoint())).has_value());
    REQUIRE(adopted->role() == vthost::proto::ConnectionRole::Observe);

    rig.presence->giveBack(Starfive, adoption.handOff, std::move(adopted));
    rig.presence->refresh();

    CHECK_FALSE(waitUntil([&] { return daemon.clientFrames.hellos().size() > 1; }, 40));
    CHECK(rig.observersMade == 1);
    REQUIRE(rig.section(Starfive).has_value());
    CHECK(rig.section(Starfive)->feed == SectionFeed::Observer);
    CHECK(rig.section(Starfive)->state == DaemonLinkState::Connected);
}

TEST_CASE("DaemonPresence: giveBack while a window is on the daemon stops it", "[presence]")
{
    auto daemon = DaemonFixture {};
    std::ignore = seedWorkspaces(daemon, 1);
    auto rig = PresenceRig { daemon };
    rig.presence->refresh();
    REQUIRE(waitUntil([&] { return rig.isIn(Starfive, DaemonLinkState::Connected); }));
    auto adoption = rig.presence->adoptObserver(Starfive, {});
    auto& adopted = adoption.controller;
    REQUIRE(adopted != nullptr);

    // Meanwhile another window's own link is on the daemon.
    auto const window = makeNativeController(daemon.endpoint(), rig.timeline);
    REQUIRE(window->connectAndWait(10s).has_value());
    rig.links.push_back(windowOn(Starfive, window.get()));
    REQUIRE(daemon.waitUntilOnDaemon([&] { return daemon.live.size() == 2; }));

    rig.presence->giveBack(Starfive, adoption.handOff, std::move(adopted));
    CHECK(daemon.waitUntilOnDaemon([&] { return daemon.live.size() == 1; }));
    CHECK(window->connectionState() == ConnectionState::Connected);
    CHECK(rig.section(Starfive)->feed == SectionFeed::WindowLink);
}

TEST_CASE("DaemonPresence: a change in another window's link reaches this window's sidebar at once",
          "[presence]")
{
    auto daemon = DaemonFixture {};
    auto rig = PresenceRig { daemon };
    rig.settings.hosts.clear();
    rig.presence->refresh();
    auto told = 0;
    auto const subscription = rig.presence->onChanged([&] { ++told; });

    rig.links.front().directory.workspaces.push_back(workspaceOf(9));
    rig.presence->linkChanged();

    // Synchronously: no event was pumped.
    CHECK(told == 1);
    auto const section = rig.section(Default);
    REQUIRE(section.has_value());
    REQUIRE(section->directory.workspaces.size() == 1);
    CHECK(section->directory.workspaces.front().id == 9);
}

TEST_CASE("DaemonPresence: a password host is not asked until connect()", "[presence]")
{
    auto daemon = DaemonFixture {};
    auto rig = PresenceRig { daemon };
    rig.observerDial = [](DaemonRef const&) -> Dialled {
        return std::unexpected { DialRefusal { .state = DaemonLinkState::NeedsInput,
                                               .reason = "needs a password" } };
    };
    rig.presence->refresh();

    REQUIRE(rig.isIn(Starfive, DaemonLinkState::NeedsInput));
    CHECK(rig.section(Starfive)->reason == "needs a password");
    CHECK(rig.observersMade == 0);
    CHECK(rig.interactiveAsked.empty());

    rig.presence->connect(Starfive, WorkspaceId { 3 });
    CHECK(rig.interactiveAsked == std::vector { WorkspaceId { 3 } });
    CHECK(rig.observersMade == 1);
    REQUIRE(waitUntil([&] { return rig.isIn(Starfive, DaemonLinkState::Connected); }));
    CHECK(daemon.clientFrames.hellos().size() == 1);
}

TEST_CASE("DaemonPresence: NeedsInput is not redialed in the background", "[presence]")
{
    auto daemon = DaemonFixture {};
    auto rig = PresenceRig { daemon };

    SECTION("a dial refused before it was made")
    {
        rig.observerDial = [](DaemonRef const&) -> Dialled {
            return std::unexpected { DialRefusal { .state = DaemonLinkState::NeedsInput,
                                                   .reason = "pin it" } };
        };
        rig.presence->refresh();
        REQUIRE(rig.isIn(Starfive, DaemonLinkState::NeedsInput));
    }
    SECTION("a dial that met a password prompt")
    {
        rig.observerDial = [](DaemonRef const&) -> Dialled {
            return std::make_shared<FailingTransport>(vthost::TransportErrorKind::AuthenticationRequired);
        };
        rig.presence->refresh();
        REQUIRE(waitUntil([&] { return rig.isIn(Starfive, DaemonLinkState::NeedsInput); }));
    }

    auto const made = rig.observersMade;
    auto const dials = rig.observerDials;
    rig.presence->refresh();
    rig.fireTimers();
    rig.fireTimers();
    CHECK_FALSE(waitUntil([&] { return rig.observerDials != dials; }, 40));
    CHECK(rig.observersMade == made);
    CHECK(rig.isIn(Starfive, DaemonLinkState::NeedsInput));
}

TEST_CASE("DaemonPresence: an unreachable daemon is redialed with backoff", "[presence]")
{
    auto daemon = DaemonFixture {};
    auto rig = PresenceRig { daemon };
    rig.observerDial = [](DaemonRef const&) -> Dialled {
        return std::make_shared<FailingTransport>(vthost::TransportErrorKind::Unreachable);
    };
    rig.presence->refresh();

    REQUIRE(waitUntil([&] { return rig.redialsArmed() == 1; }));
    CHECK_FALSE(waitUntil([&] { return rig.redialsArmed() > 1; }, 40)); // one redial, however often told
    auto const redial = std::ranges::find_if(rig.timers, [](auto const& timer) {
        return timer.first != contour::remote::PresenceRefreshInterval;
    });
    CHECK(redial->first == vthost::client::backoffDelay(rig.backoff, 1));
    // Tried again: the row does not say it failed.
    CHECK(rig.isIn(Starfive, DaemonLinkState::Reconnecting));

    rig.observerDial = [&](DaemonRef const&) -> Dialled {
        return makeTestTlsTransport(daemon.endpoint());
    };
    rig.fireTimers();
    CHECK(rig.observersMade == 2);
    CHECK(waitUntil([&] { return rig.isIn(Starfive, DaemonLinkState::Connected); }));
}

TEST_CASE("DaemonPresence: disconnect stops a connect still connecting", "[presence]")
{
    auto daemon = DaemonFixture {};
    auto rig = PresenceRig { daemon };
    rig.observerDial = [](DaemonRef const&) -> Dialled {
        return std::unexpected { DialRefusal { .state = DaemonLinkState::NeedsInput,
                                               .reason = "needs a password" } };
    };
    rig.presence->refresh();
    REQUIRE(rig.isIn(Starfive, DaemonLinkState::NeedsInput));

    rig.presence->connect(Starfive, WorkspaceId { 3 });
    REQUIRE(rig.isIn(Starfive, DaemonLinkState::Connecting));
    rig.presence->disconnect(Starfive);

    CHECK(rig.isIn(Starfive, DaemonLinkState::NotConnected));
    // Nothing dials it again by itself: not the connect that was cancelled, nor a redial, nor a refresh.
    CHECK_FALSE(waitUntil([&] { return !rig.isIn(Starfive, DaemonLinkState::NotConnected); }, 200));
    rig.presence->refresh();
    rig.fireTimers();
    CHECK(rig.isIn(Starfive, DaemonLinkState::NotConnected));
    CHECK(rig.observersMade == 1);

    // Disconnecting what does not connect does nothing.
    rig.presence->connect(Starfive, WorkspaceId { 3 });
    REQUIRE(waitUntil([&] { return rig.isIn(Starfive, DaemonLinkState::Connected); }));
    rig.presence->disconnect(Starfive);
    CHECK(rig.isIn(Starfive, DaemonLinkState::Connected));
}

TEST_CASE("DaemonPresence: an observer reconnects after a live upgrade with flags intact", "[presence]")
{
    auto control = vthost::testing::FakeDaemonControl {};
    auto services = vthost::NativeSessionServices {};
    services.handshake.capabilities.idents.push_back(
        std::to_underlying(vthost::proto::Capability::LiveUpgrade));
    services.daemon = control;
    auto daemon = DaemonFixture { {}, std::move(services) };
    auto const session = daemon.seedSession("shell");
    auto rig = PresenceRig { daemon };
    rig.presence->refresh();
    REQUIRE(waitUntil([&] { return rig.isIn(Starfive, DaemonLinkState::Connected); }));

    daemon.onDaemon([&daemon, session] {
        daemon.host->terminal(session)->writeToScreen("\a");
        return 0;
    });
    auto const rang = [&] {
        auto const pane = paneOf(rig.section(Starfive)->directory, session.value);
        return pane && pane->bell == vthost::proto::PaneBell::Rang;
    };
    REQUIRE(waitUntil(rang));

    daemon.onDaemon([&control] {
        control.announceUpgrade("9.9.10");
        control.closeObservers();
        return 0;
    });
    REQUIRE(waitUntil([&] { return daemon.clientFrames.hellos().size() == 2; }, 2000));
    REQUIRE(waitUntil([&] { return rig.isIn(Starfive, DaemonLinkState::Connected); }, 2000));
    CHECK(waitUntil(rang));
    CHECK(rig.observersMade == 1);
}

TEST_CASE("DaemonPresence: a lost observer backs off", "[presence]")
{
    // The presence's schedule is each observer's: an hour, so that no reconnect can come within the test.
    auto daemon = DaemonFixture {};
    auto rig = PresenceRig { daemon };
    rig.backoff = { .first = 1h, .ceiling = 1h };
    rig.rebuild();
    rig.presence->refresh();
    REQUIRE(waitUntil([&] { return rig.isIn(Starfive, DaemonLinkState::Connected); }));
    REQUIRE(rig.observerBackoffs.size() == 1);
    CHECK(rig.observerBackoffs.front().first == 1h);
    CHECK(rig.observerBackoffs.front().ceiling == 1h);

    daemon.dropConnections();
    REQUIRE(waitUntil([&] { return rig.isIn(Starfive, DaemonLinkState::Reconnecting); }));
    // The controller's own backoff holds the next hello back.
    CHECK_FALSE(waitUntil([&] { return daemon.clientFrames.hellos().size() > 1; }, 100));
    CHECK(rig.observersMade == 1);
}

TEST_CASE("DaemonPresence: an observer redials no more often than the sidebar looks again", "[presence]")
{
    static_assert(contour::remote::ObserverRedialBackoff.ceiling == contour::remote::PresenceRefreshInterval);
    // What an application's presence schedules by, unless told otherwise.
    CHECK(PresenceDeps {}.redialBackoff.ceiling == contour::remote::PresenceRefreshInterval);
}

TEST_CASE("DaemonPresence: a redial waits for a window to be visible", "[presence]")
{
    auto daemon = DaemonFixture {};
    auto rig = PresenceRig { daemon };
    rig.visibility = AppVisibility::Hidden;
    rig.observerDial = [](DaemonRef const&) -> Dialled {
        return std::make_shared<FailingTransport>(vthost::TransportErrorKind::Unreachable);
    };
    rig.presence->refresh();
    REQUIRE(waitUntil([&] { return rig.isIn(Starfive, DaemonLinkState::Reconnecting); }));
    CHECK_FALSE(waitUntil([&] { return rig.redialsArmed() != 0; }, 40));

    // A window shows again: the application refreshes the presence, which redials what waited.
    rig.visibility = AppVisibility::Visible;
    rig.presence->refresh();
    CHECK(rig.redialsArmed() == 1);
}

TEST_CASE("DaemonPresence: a daemon that never answered the hello is redialed", "[presence]")
{
    auto daemon = DaemonFixture {};
    auto rig = PresenceRig { daemon };
    rig.observerDial = [](DaemonRef const&) -> Dialled {
        return std::make_shared<HangingUpTransport>();
    };
    rig.presence->refresh();

    // The transport connected; the daemon took nothing: a dial that may pass, not a failure to show.
    REQUIRE(waitUntil([&] {
        return !rig.observers.empty() && rig.observers.front()->connectionState() == ConnectionState::Failed;
    }));
    REQUIRE(waitUntil([&] { return rig.redialsArmed() == 1; }));
    CHECK(rig.isIn(Starfive, DaemonLinkState::Reconnecting));
}

TEST_CASE("DaemonPresence: an observer never starts a daemon", "[presence]")
{
    auto daemon = DaemonFixture {};
    auto const tmp = core::testing::ScopedTempDir { "contour-presence" };
    auto const socket = tmp.path() / "work";
    auto rig = PresenceRig { daemon };
    rig.settings.hosts.clear();
    // Discovery still says it runs; the dial finds it gone.
    rig.discovery->running = { "default", "work" };
    auto const dialing = contour::remote::PresenceTransportDeps {
        .savedHost = [](std::string const&) -> contour::config::HostEntry const* { return nullptr; },
        .openHost = {},
        .localSocketOf = [socket](std::string const&) { return socket; },
        .knownDaemons = nullptr,
        .routeOf = {},
    };
    rig.observerDial = [&dialing](DaemonRef const& ref) {
        return contour::remote::observerTransport(ref, dialing);
    };
    auto const work = contour::window::localDaemonRef("work");
    rig.presence->refresh();

    REQUIRE(waitUntil([&] { return rig.isIn(work, DaemonLinkState::NotRunning); }, 2000));
    CHECK(rig.observersMade == 1);
    CHECK_FALSE(std::filesystem::exists(socket));
    rig.presence->refresh();
    rig.fireTimers();
    CHECK_FALSE(waitUntil([&] { return rig.observersMade != 1; }, 40));
}

TEST_CASE("DaemonPresence: an observer whose daemon stopped reads Not running and dials it no more",
          "[presence]")
{
    auto daemon = DaemonFixture {};
    auto rig = PresenceRig { daemon };
    auto const starfive = std::make_shared<StoppableDaemon>();
    rig.observerDial = [&](DaemonRef const&) -> Dialled {
        return std::make_shared<StoppableTransport>(makeTestTlsTransport(daemon.endpoint()), starfive);
    };
    rig.presence->refresh();
    REQUIRE(waitUntil([&] { return rig.isIn(Starfive, DaemonLinkState::Connected); }));

    // The daemon on the host stops: its relay says none is running (--no-start) on every login since.
    starfive->serving = DaemonServing::Stopped;
    daemon.dropConnections();

    REQUIRE(waitUntil([&] { return rig.isIn(Starfive, DaemonLinkState::NotRunning); }, 2000));
    auto const dials = starfive->dials.load();
    // No ssh login every few seconds: the 10 ms backoff would have dialled many times by now.
    CHECK_FALSE(waitUntil([&] { return starfive->dials.load() != dials; }, 200));
    CHECK(rig.redialsArmed() == 0);
    CHECK(rig.isIn(Starfive, DaemonLinkState::NotRunning));
}

TEST_CASE("DaemonPresence: a local daemon that stopped and started again is observed again", "[presence]")
{
    auto daemon = DaemonFixture {};
    auto rig = PresenceRig { daemon };
    rig.settings.hosts.clear();
    auto const work = contour::window::localDaemonRef("work");
    auto const served = std::make_shared<StoppableDaemon>();
    rig.observerDial = [&](DaemonRef const&) -> Dialled {
        return std::make_shared<StoppableTransport>(makeTestTlsTransport(daemon.endpoint()), served);
    };
    rig.discovery->running = { "default", "work" };
    rig.presence->refresh();
    REQUIRE(waitUntil([&] { return rig.isIn(work, DaemonLinkState::Connected); }));

    served->serving = DaemonServing::Stopped;
    rig.discovery->running = { "default" };
    daemon.dropConnections();
    REQUIRE(waitUntil([&] { return rig.isIn(work, DaemonLinkState::NotRunning); }, 2000));
    // The observer that gave up asked for a discovery, which no longer finds it.
    REQUIRE(waitUntil([&] { return rig.presence->localDaemons().size() == 1; }));
    auto const dials = served->dials.load();
    CHECK_FALSE(waitUntil([&] { return served->dials.load() != dials; }, 200));

    // Started again from the command line: the next look finds it, and observes it without asking.
    served->serving = DaemonServing::Serving;
    rig.discovery->running = { "default", "work" };
    rig.presence->refresh();
    REQUIRE(waitUntil([&] { return rig.isIn(work, DaemonLinkState::Connected); }, 2000));
    CHECK(rig.observersMade == 2);
    CHECK(rig.interactiveAsked.empty());
    CHECK(rig.started.empty());
}

TEST_CASE("DaemonPresence: a local daemon restarted before the next discovery is observed again",
          "[presence]")
{
    auto daemon = DaemonFixture {};
    auto rig = PresenceRig { daemon };
    rig.settings.hosts.clear();
    auto const work = contour::window::localDaemonRef("work");
    auto const served = std::make_shared<StoppableDaemon>();
    rig.observerDial = [&](DaemonRef const&) -> Dialled {
        return std::make_shared<StoppableTransport>(makeTestTlsTransport(daemon.endpoint()), served);
    };
    rig.discovery->running = { "default", "work" };
    rig.presence->refresh();
    REQUIRE(waitUntil([&] { return rig.isIn(work, DaemonLinkState::Connected); }));

    // Stopped, and started again from the command line before the discovery its observer's giving up asks
    // for looks: no discovery ever misses it.
    served->serving = DaemonServing::Stopped;
    rig.discovery->discovering = [served] {
        served->serving = DaemonServing::Serving;
    };
    daemon.dropConnections();

    REQUIRE(waitUntil([&] { return rig.observersMade == 2; }, 2000));
    REQUIRE(waitUntil([&] { return rig.isIn(work, DaemonLinkState::Connected); }, 2000));
    CHECK(rig.interactiveAsked.empty());
    CHECK(rig.started.empty());
}

TEST_CASE("DaemonPresence: a local daemon started outside Contour is observed once discovery sees it",
          "[presence]")
{
    auto daemon = DaemonFixture {};
    auto rig = PresenceRig { daemon };
    rig.settings.hosts.clear();
    auto const work = contour::window::localDaemonRef("work");
    auto serving = DaemonServing::Stopped;
    rig.observerDial = [&](DaemonRef const& ref) -> Dialled {
        if (ref == work && serving == DaemonServing::Stopped)
            return std::unexpected { DialRefusal { .state = DaemonLinkState::NotRunning,
                                                   .reason = "not running" } };
        return makeTestTlsTransport(daemon.endpoint());
    };
    // Found, but gone by the time it is dialled.
    rig.discovery->running = { "default", "work" };
    rig.presence->refresh();
    REQUIRE(waitUntil([&] { return rig.isIn(work, DaemonLinkState::NotRunning); }));
    CHECK(rig.observerDials == 1);

    // Stopped: nothing is dialled while it is not found.
    rig.discovery->running = { "default" };
    rig.presence->refresh();
    REQUIRE(waitUntil([&] { return rig.presence->localDaemons().size() == 1; }));
    CHECK(rig.isIn(work, DaemonLinkState::NotRunning));
    CHECK(rig.observerDials == 1);

    // Started from the command line: the next discovery finds it, and it is observed without anyone asking.
    serving = DaemonServing::Serving;
    rig.discovery->running = { "default", "work" };
    rig.presence->refresh();
    REQUIRE(waitUntil([&] { return rig.isIn(work, DaemonLinkState::Connected); }));
    CHECK(rig.observerDials == 2);
    CHECK(rig.interactiveAsked.empty());
    CHECK(rig.started.empty());
}

TEST_CASE("DaemonPresence: nothing is observed while no window is on a daemon", "[presence]")
{
    auto daemon = DaemonFixture {};
    auto rig = PresenceRig { daemon };
    rig.links.clear();
    rig.presence->refresh();
    CHECK(rig.presence->sections().empty());
    CHECK(rig.observersMade == 0);

    rig.links.push_back(windowOn(Default));
    rig.presence->refresh();
    REQUIRE(waitUntil([&] { return rig.isIn(Starfive, DaemonLinkState::Connected); }));

    rig.links.clear();
    rig.presence->linkSetChanged();
    CHECK(rig.presence->sections().empty());
    CHECK(daemon.waitUntilOnDaemon([&] { return daemon.live.empty(); }));
}

TEST_CASE("DaemonPresence: a closed window's subscription is gone", "[presence]")
{
    auto daemon = DaemonFixture {};
    auto rig = PresenceRig { daemon };
    rig.settings.hosts.clear();
    auto told = 0;
    {
        auto const subscription = rig.presence->onChanged([&] { ++told; });
        rig.presence->linkChanged();
        REQUIRE(told == 1);
    }
    rig.presence->linkChanged();
    rig.presence->linkSetChanged();
    CHECK(told == 1);
}

TEST_CASE("DaemonPresence: a verb for a restarted daemon's old instance does nothing", "[presence]")
{
    auto daemon = DaemonFixture {};
    std::ignore = seedWorkspaces(daemon, 1);
    auto rig = PresenceRig { daemon };
    rig.presence->refresh();
    REQUIRE(waitUntil([&] {
        auto const section = rig.section(Starfive);
        return section && section->state == DaemonLinkState::Connected && section->instance.value != 0;
    }));
    auto const old = rig.section(Starfive)->instance;

    daemon.restartHost();
    REQUIRE(waitUntil(
        [&] {
            auto const section = rig.section(Starfive);
            return section && section->state == DaemonLinkState::Connected && section->instance != old;
        },
        2000));
    auto const added = contour::remote::testing::addWorkspace(daemon);
    REQUIRE(waitUntil([&] { return !rig.section(Starfive)->directory.workspaces.empty(); }));
    auto const colour = std::optional { vtbackend::RGBColor { 0x11, 0x22, 0x33 } };

    rig.presence->setWorkspaceColor(Starfive, old, added.id, colour);
    CHECK_FALSE(waitUntil(
        [&] { return daemon.clientFrames.count(vthost::proto::PduType::SetWorkspaceColor) > 0; }, 40));

    rig.presence->setWorkspaceColor(Starfive, rig.section(Starfive)->instance, added.id, colour);
    CHECK(
        waitUntil([&] { return daemon.clientFrames.count(vthost::proto::PduType::SetWorkspaceColor) == 1; }));
}

TEST_CASE("DaemonPresence: Hide persists through updateListing and drops the observer", "[presence]")
{
    auto daemon = DaemonFixture {};
    auto rig = PresenceRig { daemon };
    rig.presence->refresh();
    REQUIRE(waitUntil([&] { return rig.isIn(Starfive, DaemonLinkState::Connected); }));

    rig.presence->setListed(Starfive, contour::window::SidebarListingChoice::Unlisted);
    REQUIRE(rig.persisted.size() == 1);
    CHECK(rig.persisted.front().hidden.contains(Starfive.value));
    CHECK_FALSE(rig.section(Starfive).has_value());
    CHECK(daemon.waitUntilOnDaemon([&] { return daemon.live.empty(); }));

    rig.presence->setListed(Starfive, contour::window::SidebarListingChoice::Listed);
    CHECK(rig.persisted.back().shown.contains(Starfive.value));
    CHECK_FALSE(rig.persisted.back().hidden.contains(Starfive.value));
    CHECK(waitUntil([&] { return rig.isIn(Starfive, DaemonLinkState::Connected); }));
}

TEST_CASE("DaemonPresence: the refresh timer re-arms only while a window is visible, and a fire after "
          "destruction is harmless",
          "[presence]")
{
    auto daemon = DaemonFixture {};
    auto rig = PresenceRig { daemon };
    rig.links.clear(); // nothing listed: the only timer is the refresh's

    rig.presence->refresh();
    REQUIRE(rig.timers.size() == 1);
    CHECK(rig.timers.front().first == contour::remote::PresenceRefreshInterval);
    rig.presence->refresh(); // armed already
    CHECK(rig.timers.size() == 1);

    rig.fireTimers();
    CHECK(rig.timers.size() == 1); // re-armed by the refresh it ran

    rig.visibility = AppVisibility::Hidden;
    rig.fireTimers();
    CHECK(rig.timers.empty());
    rig.presence->refresh();
    CHECK(rig.timers.empty());

    rig.visibility = AppVisibility::Visible;
    rig.presence->refresh();
    REQUIRE(rig.timers.size() == 1);
    rig.presence.reset();
    rig.fireTimers(); // finds the presence gone
}

namespace
{

/// What a pane rings and notifies with.
constexpr auto BellAndNotice = std::string_view { "\a\033]9;needs input\033\\" };

/// Rings the bell, and notifies, in @p daemon's session @p session.
void ringIn(DaemonFixture& daemon, vtworkspace::SessionId session)
{
    daemon.onDaemon([&] {
        daemon.host->terminal(session)->writeToScreen(BellAndNotice);
        return 0;
    });
}

/// @return Whether @p observer holds its daemon's raise role.
[[nodiscard]] bool raises(contour::remote::NativeController const* observer)
{
    return observer->raiseRole() == vthost::proto::RaiseRole::Raise;
}

} // namespace

TEST_CASE("DaemonPresence: a bell on an observed-only daemon alerts once from the last focused window",
          "[presence][raise]")
{
    auto daemon = DaemonFixture {};
    auto const session = daemon.seedSession({});
    auto rig = PresenceRig { daemon };
    rig.presence->processFocusChanged(contour::window::WindowFocusState::Focused);
    rig.presence->refresh();
    REQUIRE(waitUntil([&] { return rig.isIn(Starfive, DaemonLinkState::Connected); }));
    REQUIRE(rig.observers.size() == 1);
    REQUIRE(waitUntil([&] { return raises(rig.observers.front()); }));

    ringIn(daemon, session);

    REQUIRE(waitUntil([&] { return rig.rings == 1 && !rig.notifier.raised.empty(); }, 1000));
    CHECK_FALSE(waitUntil([&] { return rig.rings > 1 || rig.notifier.raised.size() > 1; }, 40));
    CHECK(rig.rings == 1);
    REQUIRE(rig.notifier.raised.size() == 1);
    CHECK(rig.notifier.raised.front().title.contains("starfive"));
    // The tab that rang waits for the user, counted once for the observed daemon.
    REQUIRE(waitUntil([&] { return !rig.observedWaiting.empty() && rig.observedWaiting.back() == 1; }));
}

TEST_CASE("DaemonPresence: two processes observing the same daemon alert once", "[presence][raise]")
{
    auto daemon = DaemonFixture {};
    auto const session = daemon.seedSession({});
    auto first = PresenceRig { daemon };
    auto second = PresenceRig { daemon };
    first.presence->processFocusChanged(contour::window::WindowFocusState::Focused);
    first.presence->refresh();
    REQUIRE(waitUntil([&] { return first.observers.size() == 1 && raises(first.observers.front()); }));
    second.presence->refresh();
    REQUIRE(waitUntil([&] { return second.isIn(Starfive, DaemonLinkState::Connected); }));
    REQUIRE(second.observers.size() == 1);

    // The second process is focused later: it raises from now on.
    first.presence->processFocusChanged(contour::window::WindowFocusState::Unfocused);
    second.presence->processFocusChanged(contour::window::WindowFocusState::Focused);
    REQUIRE(waitUntil([&] { return raises(second.observers.front()) && !raises(first.observers.front()); }));

    ringIn(daemon, session);

    REQUIRE(waitUntil([&] { return second.rings == 1; }, 1000));
    CHECK_FALSE(waitUntil([&] { return first.rings != 0 || second.rings > 1; }, 40));
    CHECK(first.rings == 0);
    CHECK(first.notifier.raised.empty());
    CHECK(second.notifier.raised.size() == 1);
    // The tab that rang waits for the user: on the taskbar of the process that raises for the daemon only.
    REQUIRE(waitUntil([&] { return !second.observedWaiting.empty() && second.observedWaiting.back() == 1; }));
    CHECK_FALSE(
        waitUntil([&] { return !first.observedWaiting.empty() && first.observedWaiting.back() != 0; }, 100));
}

TEST_CASE("DaemonPresence: a daemon a window is on is not alerted twice", "[presence][raise]")
{
    using vthost::proto::AttachIntentKind;
    auto daemon = DaemonFixture {};
    auto observed = DaemonFixture {};
    auto const ids = seedWorkspaces(daemon, 2);
    auto const observedSession = observed.seedSession({});
    auto rig = PresenceRig { daemon };
    auto const riscv = contour::window::hostDaemonRef("riscv");
    rig.settings.hosts.emplace_back("riscv", contour::window::SidebarListingChoice::Listed);
    auto observerDialled = std::vector<DaemonRef> {};
    rig.observerDial = [&](DaemonRef const& ref) -> Dialled {
        observerDialled.push_back(ref);
        return makeTestTlsTransport(observed.endpoint());
    };
    rig.presence->processFocusChanged(contour::window::WindowFocusState::Focused);
    auto const window = makeNativeController(
        daemon.endpoint(),
        rig.timeline,
        ControllerOptions { .intent = { .kind = AttachIntentKind::Select, .workspace = ids[0] } });
    REQUIRE(window->connectAndWait(10s).has_value());
    rig.links.push_back(windowOn(Starfive, window.get()));
    rig.presence->refresh();
    REQUIRE(waitUntil([&] { return window->raiseRole() == vthost::proto::RaiseRole::Raise; }));
    // The daemon no window is on is observed, and raises: the alert path this test relies on is live.
    REQUIRE(waitUntil([&] { return rig.isIn(riscv, DaemonLinkState::Connected); }));
    REQUIRE(rig.observers.size() == 1);
    REQUIRE(waitUntil([&] { return raises(rig.observers.front()); }));
    CHECK(std::ranges::find(observerDialled, Starfive) == observerDialled.end());
    auto const elsewhere = daemon.onDaemon([&] {
        return daemon.host->model().workspace(WorkspaceId { ids[1] })->tabAt(0)->rootPane()->session();
    });

    ringIn(daemon, elsewhere);

    // The window's own link raises it (DaemonLink::raiseElsewhere); the presence has no observer to.
    auto raised = std::vector<vthost::proto::PaneSummary> {};
    REQUIRE(waitUntil([&] {
        std::ranges::move(window->takeRaised(), std::back_inserter(raised));
        return !raised.empty();
    }));

    // The observed daemon rings after it: exactly its alert arrives, none for the window's daemon.
    ringIn(observed, observedSession);

    REQUIRE(waitUntil([&] { return rig.rings == 1 && !rig.notifier.raised.empty(); }, 1000));
    CHECK_FALSE(waitUntil([&] { return rig.rings > 1 || rig.notifier.raised.size() > 1; }, 40));
    CHECK(rig.rings == 1);
    REQUIRE(rig.notifier.raised.size() == 1);
    CHECK(rig.notifier.raised.front().title.contains("riscv"));
    CHECK_FALSE(rig.notifier.raised.front().title.contains("starfive"));
}

TEST_CASE("DaemonPresence: a hide keeps what another process chose meanwhile", "[presence]")
{
    auto daemon = DaemonFixture {};
    auto rig = PresenceRig { daemon };
    rig.presence->refresh();
    // Written by another process after this one read the settings.
    rig.stored.hidden.insert(contour::window::hostDaemonRef("elsewhere").value);

    rig.presence->setListed(Starfive, contour::window::SidebarListingChoice::Unlisted);
    REQUIRE(rig.persisted.size() == 1);
    CHECK(rig.persisted.front().hidden.contains("host:elsewhere"));
    CHECK(rig.persisted.front().hidden.contains(Starfive.value));
}

TEST_CASE("DaemonPresence: no listing choice is kept for a link: daemon", "[presence]")
{
    auto daemon = DaemonFixture {};
    auto rig = PresenceRig { daemon };
    rig.presence->refresh();

    rig.presence->setListed(contour::window::linkDaemonRef(4),
                            contour::window::SidebarListingChoice::Unlisted);
    CHECK(rig.persisted.empty());
}

TEST_CASE("DaemonPresence: the listing settings are read once, and again when they may have changed",
          "[presence]")
{
    auto daemon = DaemonFixture {};
    auto rig = PresenceRig { daemon };
    rig.presence->refresh();
    REQUIRE(rig.settingsReads == 1);

    // As a CycleDaemon key press refreshes, every time.
    rig.presence->refresh();
    rig.presence->refresh();
    rig.presence->linkSetChanged();
    CHECK(rig.settingsReads == 1);

    rig.presence->settingsChanged();
    CHECK(rig.settingsReads == 2);
    rig.presence->setListed(Starfive, contour::window::SidebarListingChoice::Unlisted);
    CHECK(rig.settingsReads == 3);
    // The periodic refresh reads them again: another process may have changed them.
    rig.fireTimers();
    CHECK(rig.settingsReads == 4);
}

TEST_CASE("DaemonPresence: two windows on one daemon reached by its own address are one section",
          "[presence]")
{
    auto daemon = DaemonFixture {};
    auto rig = PresenceRig { daemon };
    rig.settings.hosts.clear();
    auto const instance = contour::window::DaemonInstance { 77 };
    rig.links.clear();
    for (auto const id: { 3, 2 })
    {
        rig.links.push_back(windowOn(contour::window::linkDaemonRef(static_cast<uint64_t>(id))));
        rig.links.back().instance = instance;
        rig.links.back().shownWorkspaces = { static_cast<uint64_t>(id) };
    }
    rig.presence->refresh();

    auto const sections = rig.presence->sections();
    REQUIRE(sections.size() == 1);
    CHECK(sections.front().daemon == contour::window::linkDaemonRef(2));
    CHECK(sections.front().shownElsewhere == std::set<uint64_t> { 2, 3 });
}

TEST_CASE("DaemonPresence: links are named alike only for one run of one daemon", "[presence]")
{
    using contour::window::DaemonInstance;
    auto const link = [](DaemonRef ref, uint64_t instance) {
        auto view = windowOn(std::move(ref));
        view.instance = DaemonInstance { instance };
        return view;
    };
    auto const merged = contour::remote::mergedLinks({ link(contour::window::linkDaemonRef(5), 9),
                                                       link(Default, 9),
                                                       link(contour::window::linkDaemonRef(6), 8),
                                                       link(contour::window::linkDaemonRef(7), 0),
                                                       link(contour::window::linkDaemonRef(8), 0) });
    REQUIRE(merged.size() == 5);
    CHECK(merged[0].daemon == Default); // a local daemon's name wins over the link's own
    CHECK(merged[1].daemon == Default);
    CHECK(merged[2].daemon == contour::window::linkDaemonRef(6)); // another run
    CHECK(merged[3].daemon == contour::window::linkDaemonRef(7)); // runs not known yet stay apart
    CHECK(merged[4].daemon == contour::window::linkDaemonRef(8));
}

TEST_CASE("DaemonPresence: a sidebar is told nothing once the presence shut down", "[presence]")
{
    auto daemon = DaemonFixture {};
    auto rig = PresenceRig { daemon };
    rig.presence->refresh();
    auto told = 0;
    auto const subscription = rig.presence->onChanged([&told] { ++told; });

    // The application is going: its windows' sidebars must not queue a rebuild that outlives it.
    rig.presence->shutdown();
    rig.presence->refresh();
    rig.presence->linkSetChanged();
    CHECK(told == 0);
}

TEST_CASE("DaemonPresence: shutdown stops every observer, and starts none after", "[presence]")
{
    auto daemon = DaemonFixture {};
    auto rig = PresenceRig { daemon };
    rig.presence->refresh();
    REQUIRE(waitUntil([&] { return rig.isIn(Starfive, DaemonLinkState::Connected); }));

    rig.presence->shutdown();
    // Waited for: by the time it returns, the daemon was told.
    CHECK(daemon.waitUntilOnDaemon([&] { return daemon.live.empty(); }));
    CHECK(rig.presence->sections().empty());

    rig.presence->refresh();
    rig.presence->linkSetChanged();
    rig.presence->connect(Starfive, WorkspaceId { 1 });
    CHECK(rig.observersMade == 1);
    CHECK(rig.presence->sections().empty());
}

TEST_CASE("DaemonPresence: a recompute while an observer is handed over dials no second one",
          "[presence][adopt]")
{
    auto daemon = DaemonFixture {};
    auto rig = PresenceRig { daemon };
    rig.presence->refresh();
    REQUIRE(waitUntil([&] { return rig.isIn(Starfive, DaemonLinkState::Connected); }));

    // A window takes it; its link is not attached yet, so no window link is on the daemon.
    auto adoption = rig.presence->adoptObserver(Starfive, {});
    auto& adopted = adoption.controller;
    REQUIRE(adopted != nullptr);
    rig.presence->linkSetChanged();
    rig.presence->linkSetChanged();

    CHECK_FALSE(waitUntil([&] { return daemon.clientFrames.hellos().size() > 1; }, 40));
    CHECK(rig.observersMade == 1);
    // Once the adopting link is attached, the hand-over is done: nothing is dialled either.
    rig.links.push_back(windowOn(Starfive, adopted.get()));
    rig.presence->linkChanged();
    rig.links.pop_back();
    rig.presence->giveBack(Starfive, adoption.handOff, std::move(adopted));
    CHECK(rig.observersMade == 1);
}

TEST_CASE("DaemonPresence: an observer of an earlier run is not handed over", "[presence][adopt]")
{
    auto daemon = DaemonFixture {};
    auto rig = PresenceRig { daemon };
    rig.presence->refresh();
    REQUIRE(waitUntil([&] { return rig.isIn(Starfive, DaemonLinkState::Connected); }));
    REQUIRE(!rig.observers.empty());
    auto const running = rig.observers.back()->daemonInstance().value_or(0);
    REQUIRE(running != 0);

    CHECK(rig.presence->adoptObserver(Starfive, contour::window::DaemonInstance { running + 1 }).controller
          == nullptr);

    // Still the daemon's observer, and handed to a window that names its run.
    CHECK(rig.section(Starfive)->feed == SectionFeed::Observer);
    auto adoption = rig.presence->adoptObserver(Starfive, contour::window::DaemonInstance { running });
    auto& adopted = adoption.controller;
    CHECK(adopted != nullptr);
    rig.presence->giveBack(Starfive, adoption.handOff, std::move(adopted));
}

TEST_CASE("DaemonPresence: a connection coming back keeps the daemon from being dialled", "[presence][adopt]")
{
    auto daemon = DaemonFixture {};
    auto rig = PresenceRig { daemon };
    // A window is on the daemon, and leaves it: its connection comes back once its panes let it go.
    auto leaving = makeNativeController(daemon.endpoint(),
                                        rig.timeline,
                                        ControllerOptions { .role = vthost::proto::ConnectionRole::Observe });
    leaving->startConnecting();
    REQUIRE(waitUntil([&] { return leaving->connectionState() == ConnectionState::Connected; }));
    rig.links.push_back(windowOn(Starfive, leaving.get()));
    rig.presence->refresh();
    REQUIRE(rig.observersMade == 0);
    auto const handOff = rig.presence->expectGiveBack(Starfive, contour::remote::DaemonLinkId { 3 });
    rig.links.pop_back();

    rig.presence->linkSetChanged();
    rig.presence->refresh();

    CHECK(rig.observersMade == 0);
    rig.presence->giveBack(Starfive, handOff, std::move(leaving));
    CHECK(rig.observersMade == 0);
    REQUIRE(rig.section(Starfive).has_value());
    CHECK(rig.section(Starfive)->feed == SectionFeed::Observer);
    CHECK(rig.section(Starfive)->state == DaemonLinkState::Connected);
}

TEST_CASE("DaemonPresence: a give-back of nothing clears the mark and dials again", "[presence][adopt]")
{
    auto daemon = DaemonFixture {};
    auto rig = PresenceRig { daemon };
    rig.links.push_back(windowOn(Starfive));
    rig.presence->refresh();
    auto const handOff = rig.presence->expectGiveBack(Starfive, contour::remote::DaemonLinkId { 3 });
    rig.links.pop_back();
    rig.presence->linkSetChanged();
    REQUIRE(rig.observersMade == 0);

    rig.presence->giveBack(Starfive, handOff, nullptr);

    CHECK(rig.observersMade == 1);
    CHECK(waitUntil([&] { return rig.isIn(Starfive, DaemonLinkState::Connected); }));
}

TEST_CASE("DaemonPresence: while a window holds a daemon's connection, its section keeps what it showed",
          "[presence][adopt]")
{
    // What the sidebar lists of the daemon -- its rows, and the run a pending jump's marks are matched by --
    // stays through the hand-over, which neither a window link nor an observer feeds.
    auto daemon = DaemonFixture {};
    std::ignore = seedWorkspaces(daemon, 2);
    auto rig = PresenceRig { daemon };

    SECTION("handed to a window landing on it: Connecting, and no second dial")
    {
        rig.presence->refresh();
        REQUIRE(waitUntil([&] {
            auto const section = rig.section(Starfive);
            return section && section->state == DaemonLinkState::Connected
                   && section->directory.workspaces.size() >= 2;
        }));
        auto const before = *rig.section(Starfive);

        auto adoption = rig.presence->adoptObserver(Starfive, {});
        REQUIRE(adoption.controller != nullptr);
        // As the application does once the adopting link is made, before it is attached.
        rig.presence->linkSetChanged();

        auto const during = rig.section(Starfive);
        REQUIRE(during.has_value());
        CHECK(during->state == DaemonLinkState::Connecting);
        CHECK(during->instance == before.instance);
        CHECK(during->directory == before.directory);
        // Its row offers no Connect meanwhile; one asked for all the same dials nothing.
        rig.presence->connect(Starfive, WorkspaceId { 1 });
        CHECK(rig.observersMade == 1);
        CHECK(rig.interactiveAsked.empty());

        rig.links.push_back(windowOn(Starfive, adoption.controller.get()));
        rig.presence->linkChanged();
        CHECK(rig.section(Starfive)->feed == SectionFeed::WindowLink);
        rig.links.pop_back();
        rig.presence->giveBack(Starfive, adoption.handOff, std::move(adoption.controller));
    }
    SECTION("coming back from a window leaving it: what the link last read")
    {
        auto leaving =
            makeNativeController(daemon.endpoint(),
                                 rig.timeline,
                                 ControllerOptions { .role = vthost::proto::ConnectionRole::Observe });
        leaving->startConnecting();
        REQUIRE(waitUntil([&] {
            return leaving->connectionState() == ConnectionState::Connected
                   && leaving->directory().workspaces.size() >= 2;
        }));
        auto view = windowOn(Starfive, leaving.get());
        view.instance = contour::window::DaemonInstance { leaving->daemonInstance().value_or(0) };
        view.directory = leaving->directory();
        rig.links.push_back(view);
        rig.presence->refresh();
        REQUIRE(rig.observersMade == 0);

        auto const handOff = rig.presence->expectGiveBack(Starfive, contour::remote::DaemonLinkId { 3 });
        rig.links.pop_back();
        rig.presence->linkSetChanged();

        auto const during = rig.section(Starfive);
        REQUIRE(during.has_value());
        CHECK(during->state == DaemonLinkState::Connected);
        CHECK(during->instance == view.instance);
        CHECK(during->directory == view.directory);
        CHECK(during->feed == SectionFeed::Observer);
        rig.presence->connect(Starfive, WorkspaceId { 1 });
        CHECK(rig.observersMade == 0);

        rig.presence->giveBack(Starfive, handOff, std::move(leaving));
        CHECK(rig.observersMade == 0);
    }
}

TEST_CASE("DaemonPresence: a give-back of nothing clears its own hand-over only", "[presence][adopt]")
{
    // A daemon left, rejoined and left again before the first connection came back: the first give-back
    // leaves the second hand-over standing.
    auto daemon = DaemonFixture {};
    auto rig = PresenceRig { daemon };
    rig.links.push_back(windowOn(Starfive));
    rig.presence->refresh();
    auto const first = rig.presence->expectGiveBack(Starfive, contour::remote::DaemonLinkId { 3 });
    auto const second = rig.presence->expectGiveBack(Starfive, contour::remote::DaemonLinkId { 4 });
    rig.links.pop_back();
    rig.presence->linkSetChanged();

    rig.presence->giveBack(Starfive, first, nullptr);
    CHECK(rig.observersMade == 0);

    rig.presence->giveBack(Starfive, second, nullptr);
    CHECK(rig.observersMade == 1);
}

TEST_CASE("DaemonPresence: a link stopping after its hand-over was done dials nothing", "[presence][adopt]")
{
    // As the application quits: the window that jumped to the daemon has its adopted link attached, and its
    // stop tells the give-back nothing. Nothing is left to observe the daemon for: the quit dials nothing.
    auto daemon = DaemonFixture {};
    auto rig = PresenceRig { daemon };
    rig.presence->refresh();
    REQUIRE(waitUntil([&] { return rig.isIn(Starfive, DaemonLinkState::Connected); }));
    auto adoption = rig.presence->adoptObserver(Starfive, {});
    REQUIRE(adoption.controller != nullptr);
    rig.links.push_back(windowOn(Starfive, adoption.controller.get()));
    rig.presence->linkChanged();
    REQUIRE(rig.observersMade == 1);

    adoption.controller->stop();
    rig.links.pop_back();
    rig.presence->giveBack(Starfive, adoption.handOff, nullptr);

    CHECK(rig.observersMade == 1);
}

TEST_CASE("DaemonPresence: once shut down, a give-back dials nothing", "[presence][adopt]")
{
    auto daemon = DaemonFixture {};
    auto rig = PresenceRig { daemon };
    rig.links.push_back(windowOn(Starfive));
    rig.presence->refresh();
    auto const handOff = rig.presence->expectGiveBack(Starfive, contour::remote::DaemonLinkId { 3 });
    rig.links.pop_back();

    rig.presence->shutdown();
    rig.presence->giveBack(Starfive, handOff, nullptr);

    CHECK(rig.observersMade == 0);
}

TEST_CASE("DaemonPresence: observers are built to answer switch requests", "[presence][adopt]")
{
    auto daemon = DaemonFixture {};
    auto timeline = TestTimeline {};
    auto config = controllerConfig(daemon.endpoint(), timeline, ControllerOptions {});
    config.switchRequests = contour::remote::NativeController::SwitchRequests::Declined;
    auto const backoff = vthost::client::BackoffSchedule { .first = 10ms, .ceiling = 50ms };

    auto const observer = contour::remote::observerConfigOf(std::move(config), backoff);

    // A daemon never asks an observer to switch; a window that adopts it must answer.
    CHECK(observer.switchRequests == contour::remote::NativeController::SwitchRequests::Answered);
    CHECK(observer.role == vthost::proto::ConnectionRole::Observe);
    CHECK(observer.resumeFallback == contour::remote::NativeController::ResumeFallback::Nothing);
    CHECK(observer.backoff.first == backoff.first);
}
