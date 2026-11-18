// SPDX-License-Identifier: Apache-2.0
//
// The production daemon world behind vthost's interface: what it shows of an extension that could not run, on
// the fake WebAssembly backend and fakes of the files, the locks, the processes and the directories.
#include <vtbackend/screen/LocalTime.hpp>
#include <vtbackend/screen/Settings.hpp>

#include <vtpty/MockPty.hpp>

#include <core/Environment.hpp>
#include <core/net/EventLoop.hpp>
#include <core/net/testing/ScriptedBackend.hpp>
#include <core/platform/Clock.hpp>
#include <core/platform/testing/InMemoryFileSystem.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <format>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <vtextension/DaemonExtensionRuntime.hpp>
#include <vtextension/testing/FakeDirectoryOpener.hpp>
#include <vtextension/testing/FakeFileLocker.hpp>
#include <vtextension/testing/FakeProcessRunner.hpp>
#include <vtextension/testing/FakeWasmBackend.hpp>
#include <vtextension/testing/TestModules.hpp>
#include <vthost/DaemonWorldExtensions.hpp>
#include <vthost/ExtensionActions.hpp>
#include <vthost/ExtensionHosting.hpp>
#include <vthost/SessionHost.hpp>

using namespace std::string_view_literals;

namespace
{

/// A daemon module by its manifest, which the runtime refuses to load: it has a start function.
constexpr auto BrokenManifest =
    "name = broken\nversion = 2\ntitle = Agents\nplacement daemon\nroot work = config:work\n"
    "command run short @work = true\naction open = Open Agents\n"
    "action refresh = Refresh Agents\n"sv;

/// @return @p bytes as file content.
[[nodiscard]] std::string textOf(std::vector<std::byte> const& bytes)
{
    auto text = std::string(bytes.size(), '\0');
    std::ranges::transform(bytes, text.begin(), [](std::byte b) { return static_cast<char>(b); });
    return text;
}

/// Served tabs that record what is asked of them.
class RecordingTabs final: public vthost::IServedTabs
{
  public:
    [[nodiscard]] std::expected<std::string, vthost::ServedTabError> open(std::string_view extension,
                                                                          std::string_view title,
                                                                          std::string_view /*moduleDigest*/,
                                                                          std::string_view scheme,
                                                                          vthost::Origin /*origin*/) override
    {
        calls.push_back(std::format("open {} {} {}", extension, title, scheme));
        return std::string { "tab-1" };
    }

    [[nodiscard]] std::expected<void, vthost::ServedTabError> close(std::string_view extension,
                                                                    std::string_view tab) override
    {
        calls.push_back(std::format("close {} {}", extension, tab));
        return std::unexpected(vthost::ServedTabError::NotOwned);
    }

    [[nodiscard]] std::expected<void, vthost::ServedTabError> reveal(std::string_view extension,
                                                                     std::string_view tab,
                                                                     vthost::Origin /*origin*/) override
    {
        calls.push_back(std::format("reveal {} {}", extension, tab));
        return std::unexpected(vthost::ServedTabError::Gone);
    }

    [[nodiscard]] std::vector<vthost::ServedTabInfo> list(std::string_view /*extension*/) const override
    {
        return {};
    }

    [[nodiscard]] std::expected<std::string, vthost::ServedTabError> read(std::string_view /*extension*/,
                                                                          std::string_view /*tab*/) override
    {
        return std::unexpected(vthost::ServedTabError::Limit);
    }

    [[nodiscard]] std::expected<void, vthost::ServedTabError> write(std::string_view /*extension*/,
                                                                    std::string_view /*tab*/,
                                                                    std::string_view /*bytes*/) override
    {
        return std::unexpected(vthost::ServedTabError::BadArgument);
    }

    void closeAll(std::string_view extension) override
    {
        calls.push_back(std::format("closeAll {}", extension));
    }

    std::vector<std::string> calls; ///< What was asked, in order.
};

/// Whether a Fixture makes its daemon world as it is constructed.
enum class WorldMade : uint8_t
{
    Later = 0, ///< A host makes it, through makeWorld().
    Now,       ///< The fixture does, with its own events and wiring.
};

/// The daemon world of one on-demand extension, `broken`, whose module does not load.
struct Fixture
{
    explicit Fixture(WorldMade made = WorldMade::Now)
    {
        directories.add("/w");
        fileSystem.addFile(
            "/ext/broken.wasm",
            textOf(vtextension::testing::withManifest(
                vtextension::testing::toModule(vtextension::testing::StartFunctionModule), BrokenManifest)));
        if (made == WorldMade::Now)
            world = makeWorld(vthost::DaemonWorldEvents { .requestsChanged = {},
                                                          .summaryChanged = [this] { ++summaryChanges; },
                                                          .presenceChanged = [this] { ++presenceChanges; } },
                              vthost::DaemonWorldWiring { .tabs = tabs, .actions = actions });
    }

    /// @return The daemon world, on this fixture's fakes.
    [[nodiscard]] std::unique_ptr<vthost::DaemonWorldExtensions> makeWorld(vthost::DaemonWorldEvents events,
                                                                           vthost::DaemonWorldWiring wiring)
    {
        return std::make_unique<vthost::DaemonWorldExtensions>(
            vtextension::DaemonExtensionRuntimeConfig {
                .declarations = { vtextension::DaemonExtensionDeclaration {
                    .name = "broken",
                    .modulePath = "/ext/broken.wasm",
                    .autostart = vtextension::ExtensionAutostart::OnDemand,
                    .configJson = R"({"work":"/w"})",
                    .storeDirectory = "/state/extensions/default/broken/store" } },
                .grantsFile = "/config/extension-grants.daemon-scoped.txt",
                .home = "/home/me",
                .inheritedEnvironment = {},
                .bundledDirectories = {} },
            vthost::DaemonWorldServices { .backend = backend,
                                          .runner = runner,
                                          .directories = directories,
                                          .fileSystem = fileSystem,
                                          .locker = locker,
                                          .wallClock = wallClock,
                                          .localTime = localTime },
            std::move(events),
            wiring);
    }

    Fixture(Fixture const&) = delete;
    Fixture(Fixture&&) = delete;
    Fixture& operator=(Fixture const&) = delete;
    Fixture& operator=(Fixture&&) = delete;
    ~Fixture() = default;

    vtextension::testing::FakeWasmBackend backend;
    core::platform::testing::InMemoryFileSystem fileSystem;
    vtextension::testing::FakeFileLocker locker;
    vtextension::testing::FakeProcessRunner runner;
    vtextension::testing::FakeDirectoryOpener directories;
    core::platform::ManualWallClock wallClock;
    vtbackend::LocalTimeConverter localTime { &vtbackend::systemLocalTime };
    int summaryChanges = 0;
    int presenceChanges = 0;
    RecordingTabs tabs;
    vthost::ExtensionActions actions { [](std::string_view /*extension*/, vthost::ServedHint /*hint*/) {} };
    /// Made once its module is on disk.
    std::unique_ptr<vthost::DaemonWorldExtensions> world;
};

} // namespace

TEST_CASE("DaemonWorldExtensions: a module that fails after consent shows that it failed and why",
          "[daemon][summary][consent]")
{
    auto fixture = Fixture {};
    auto& world = *fixture.world;
    world.startOnDemand("broken");
    auto const pending = world.pendingRequests();
    REQUIRE(pending.size() == 1);
    CHECK(world.status("broken")->state == vthost::DaemonExtensionState::Pending);
    CHECK(world.summaries().summaryOf("broken").empty());

    auto const changesBefore = fixture.summaryChanges;
    world.answer("broken", pending[0].grantDigest, vthost::GrantState::Granted);

    // Not "waiting for consent": failed, the reason at hand for `contour extension status`.
    auto const status = world.status("broken");
    REQUIRE(status.has_value());
    CHECK(status->state == vthost::DaemonExtensionState::Failed);
    CHECK(status->problem.contains("daemon.extensions.broken: its module does not load"));
    CHECK(world.list().at(0).state == vthost::DaemonExtensionState::Failed);
    // The status line says that it failed, and its rows are told to repaint.
    CHECK(world.summaries().summaryOf("broken") == "broken failed");
    CHECK(fixture.summaryChanges > changesBefore);

    // Revoked, the failure goes with the grant, and its tabs with it.
    world.revoke("broken");
    CHECK(fixture.tabs.calls == std::vector<std::string> { "closeAll broken" });
    CHECK(world.status("broken")->state == vthost::DaemonExtensionState::Stopped);
    CHECK(world.status("broken")->problem.empty());
    CHECK(world.summaries().summaryOf("broken").empty());
    CHECK_FALSE(world.status("nope").has_value());
    world.stop();
}

TEST_CASE("DaemonWorldExtensions: the status line shows what each phase says", "[daemon][summary]")
{
    using vtextension::DaemonExtensionPhase;
    struct Row
    {
        DaemonExtensionPhase phase;
        std::string_view shown;
    };
    auto const rows = std::array {
        Row { .phase = DaemonExtensionPhase::Running, .shown = "agents 1/4" },
        Row { .phase = DaemonExtensionPhase::Disabled, .shown = "broken disabled" },
        Row { .phase = DaemonExtensionPhase::Failed, .shown = "broken failed" },
        Row { .phase = DaemonExtensionPhase::Pending, .shown = "" },
        Row { .phase = DaemonExtensionPhase::Denied, .shown = "" },
        Row { .phase = DaemonExtensionPhase::Stopped, .shown = "" },
    };
    auto fixture = Fixture {};
    auto& world = *fixture.world;
    // As a run's lane does: it published a summary, then its phase changes.
    world.presence().publishSummary("broken", 0, "agents 1/4", "#1 running");
    for (auto const& row: rows)
    {
        INFO(static_cast<int>(row.phase));
        auto const changesBefore = fixture.summaryChanges;
        world.presence().phaseChanged("broken", 0, row.phase);
        CHECK(world.summaries().summaryOf("broken") == row.shown);
        // Every phase change repaints.
        CHECK(fixture.summaryChanges == changesBefore + 1);
    }
    world.stop();
}

TEST_CASE("DaemonWorldExtensions: what a revoked run still publishes is dropped",
          "[daemon][summary][consent]")
{
    auto fixture = Fixture {};
    auto& world = *fixture.world;
    // Run 0 is up and published.
    world.presence().phaseChanged("broken", 0, vtextension::DaemonExtensionPhase::Running);
    world.presence().publishSummary("broken", 0, "agents 1/4", "#1 running");
    REQUIRE(world.summaries().summaryOf("broken") == "agents 1/4");

    world.revoke("broken");
    CHECK(world.summaries().summaryOf("broken").empty());

    // Its lane still had a summary-set in flight: it lands after the revoke, and is not shown.
    world.presence().publishSummary("broken", 0, "late", "late detail");
    world.presence().phaseChanged("broken", 0, vtextension::DaemonExtensionPhase::Running);
    CHECK(world.summaries().summaryOf("broken").empty());
    CHECK(world.status("broken")->summary.empty());
    CHECK(world.status("broken")->detail.empty());

    // The next run's word is shown again.
    world.presence().phaseChanged("broken", 1, vtextension::DaemonExtensionPhase::Running);
    world.presence().publishSummary("broken", 1, "agents 2/4", "");
    CHECK(world.summaries().summaryOf("broken") == "agents 2/4");
    world.stop();
}

TEST_CASE("DaemonWorldExtensions: the palette rows are the declared actions, with where it stands",
          "[daemon][presence]")
{
    auto fixture = Fixture {};
    auto& world = *fixture.world;
    using Rows = std::vector<std::pair<std::string, std::string>>;
    auto const declared = Rows { { "open", "Open Agents" }, { "refresh", "Refresh Agents" } };
    CHECK(world.actionRows()
          == std::vector { vthost::ExtensionActionRows { .extension = "broken",
                                                         .title = "Agents",
                                                         .state = vthost::DaemonExtensionState::Stopped,
                                                         .rows = declared } });

    // Waiting for a grant is a state the palette shows.
    world.startOnDemand("broken");
    auto const afterStart = fixture.presenceChanges;
    CHECK(afterStart >= 1);
    CHECK(world.actionRows().at(0).state == vthost::DaemonExtensionState::Pending);

    // So is every phase a run reports.
    world.presence().phaseChanged("broken", 0, vtextension::DaemonExtensionPhase::Disabled);
    CHECK(fixture.presenceChanges == afterStart + 1);

    // Neither a hint nor a restart reaches an extension that does not run.
    world.hint("broken", vthost::ServedHint::Actions);
    world.hint("unknown", vthost::ServedHint::Tabs);
    world.restart("broken");
    CHECK(world.actionRows().at(0).state == vthost::DaemonExtensionState::Pending);
    world.stop();
    CHECK(world.actionRows().empty());
}

TEST_CASE("DaemonWorldExtensions: a guest fault or a disable closes no tab", "[daemon][served]")
{
    auto fixture = Fixture {};
    auto& world = *fixture.world;
    world.presence().phaseChanged("broken", 0, vtextension::DaemonExtensionPhase::Running);
    world.presence().phaseChanged("broken", 0, vtextension::DaemonExtensionPhase::Disabled);
    world.restart("broken");
    world.presence().phaseChanged("broken", 0, vtextension::DaemonExtensionPhase::Running);
    // Its tabs stay open, as they were drawn, for the restarted guest to draw again.
    CHECK(fixture.tabs.calls.empty());
    world.stop();
}

TEST_CASE("DaemonWorldExtensions: a served-tab call reaches the host in the manifest's words",
          "[daemon][served]")
{
    auto tabs = RecordingTabs {};
    auto actions = vthost::ExtensionActions { [](std::string_view, vthost::ServedHint) {} };
    auto const scheme = std::string_view { "agents" };
    auto service = vthost::servedTabServiceOf(
        vtextension::ServedTabOwner {
            .extension = "orchestrator", .title = "Agents", .scheme = scheme, .moduleDigest = "3f9a" },
        vthost::DaemonWorldWiring { .tabs = tabs, .actions = actions });

    // Only the declared scheme is served, under the manifest's title.
    CHECK(service->tabOpen("shell", 1)
          == std::expected<std::string, vtextension::HostError> { std::unexpect,
                                                                  vtextension::HostError::BadArgument });
    CHECK(service->tabOpen("agents", 1) == std::expected<std::string, vtextension::HostError> { "tab-1" });
    CHECK(tabs.calls == std::vector<std::string> { "open orchestrator Agents agents" });

    // Each of the host's refusals, as the WIT words it.
    using Unit = std::expected<void, vtextension::HostError>;
    CHECK(service->tabClose("tab-1") == Unit { std::unexpect, vtextension::HostError::NotOwned });
    CHECK(service->tabReveal("tab-1", 1) == Unit { std::unexpect, vtextension::HostError::Gone });
    CHECK(service->tabRead("tab-1")
          == std::expected<vtextension::ByteList, vtextension::HostError> { std::unexpect,
                                                                            vtextension::HostError::Limit });
    auto const bytes = std::array<uint8_t, 2> { 'h', 'i' };
    CHECK(service->tabWrite("tab-1", bytes) == Unit { std::unexpect, vtextension::HostError::BadArgument });
    CHECK(service->tabs() == std::expected<std::vector<vtextension::TabInfo>, vtextension::HostError> {});

    // Actions are this extension's only, with their origin.
    auto const origin =
        actions.originOf(vthost::ConnectionId { .endpoint = "native", .index = 1, .peer = {} });
    actions.deliver("orchestrator", "open", origin);
    actions.deliver("other", "open", origin);
    CHECK(service->actionsTake()
          == std::expected<std::vector<vtextension::ActionInfo>, vtextension::HostError> {
              std::vector { vtextension::ActionInfo { .id = "open", .origin = origin } } });

    // An extension whose manifest serves no tab opens none.
    auto unserving = vthost::servedTabServiceOf(
        vtextension::ServedTabOwner {
            .extension = "plain", .title = "plain", .scheme = std::nullopt, .moduleDigest = "" },
        vthost::DaemonWorldWiring { .tabs = tabs, .actions = actions });
    CHECK(unserving->tabOpen("agents", 1)
          == std::expected<std::string, vtextension::HostError> { std::unexpect,
                                                                  vtextension::HostError::BadArgument });
}

TEST_CASE("DaemonWorldExtensions: the first invocation starts an on-demand extension", "[daemon][served]")
{
    auto fixture = Fixture { WorldMade::Later };
    auto* world = static_cast<vthost::DaemonWorldExtensions*>(nullptr);
    core::net::testing::ScriptedBackend source;
    core::net::EventLoop loop { source };
    auto host =
        vthost::SessionHost { loop,
                              [](vtbackend::PageSize size, std::optional<vtpty::Process::ExecInfo> const&) {
                                  return std::make_unique<vtpty::MockPty>(size);
                              },
                              vtbackend::Settings {},
                              core::defaultEnvironment(),
                              vthost::SessionHostConfig {
                                  .pumping = vthost::SessionPumping::Manual,
                                  .daemonWorld = [&](vthost::DaemonWorldEvents events,
                                                     vthost::OwnerPost const& /*post*/,
                                                     vthost::DaemonWorldWiring wiring) {
                                      auto made = fixture.makeWorld(std::move(events), wiring);
                                      world = made.get();
                                      return std::shared_ptr<vthost::IDaemonWorldHosting> { std::move(made) };
                                  } } };
    REQUIRE(world != nullptr);
    REQUIRE(world->status("broken")->state == vthost::DaemonExtensionState::Stopped);

    auto const connection = vthost::ConnectionId { .endpoint = "native", .index = 1, .peer = {} };
    CHECK(host.invokeExtensionAction(connection, "broken", "open")
          == vthost::InvocationEffect::StartThenDeliver);
    // Started on demand: not granted yet, it asks its user first; the action waits for it.
    CHECK(world->status("broken")->state == vthost::DaemonExtensionState::Pending);
    auto const taken = host.extensionActions().take("broken");
    REQUIRE(taken.size() == 1);
    CHECK(taken.front().first == "open");
    CHECK(host.extensionActions().connectionOf("broken", taken.front().second) == connection);
}
