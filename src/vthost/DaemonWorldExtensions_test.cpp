// SPDX-License-Identifier: Apache-2.0
//
// The production daemon world behind vthost's interface: what it shows of an extension that could not run, on
// the fake WebAssembly backend and fakes of the files, the locks, the processes and the directories.
#include <vtbackend/screen/LocalTime.hpp>
#include <vtbackend/screen/Settings.hpp>

#include <vtpty/BeneathRead.hpp>
#include <vtpty/ChannelPty.hpp>
#include <vtpty/DirectoryHandle.hpp>
#include <vtpty/MockPty.hpp>

#include <core/Environment.hpp>
#include <core/net/EventLoop.hpp>
#include <core/net/testing/ScriptedBackend.hpp>
#include <core/platform/Clock.hpp>
#include <core/platform/testing/InMemoryFileSystem.hpp>
#include <core/testing/ScopedTempDir.hpp>

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <format>
#include <fstream>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include <vtcontrol/testing/SequentialUuidSource.hpp>
#include <vtextension/CanonicalAbi.hpp>
#include <vtextension/DaemonExtensionRuntime.hpp>
#include <vtextension/DaemonImports.hpp>
#include <vtextension/DaemonWorld.hpp>
#include <vtextension/testing/FakeBeneathReader.hpp>
#include <vtextension/testing/FakeDirectoryOpener.hpp>
#include <vtextension/testing/FakeFileLocker.hpp>
#include <vtextension/testing/FakeProcessRunner.hpp>
#include <vtextension/testing/FakeWasmBackend.hpp>
#include <vtextension/testing/TestModules.hpp>
#include <vthost/DaemonWorldExtensions.hpp>
#include <vthost/ExtensionActions.hpp>
#include <vthost/ExtensionHosting.hpp>
#include <vthost/SessionHost.hpp>
#include <vthost/testing/HandoverTesting.hpp>
#include <vthost/testing/ServedTesting.hpp>

using namespace std::chrono_literals;
using namespace std::string_view_literals;
using vthost::testing::FakeWindow;

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

/// Owned panes that record what is asked of them, and answer what the test set.
class RecordingOwnedPanes final: public vthost::IOwnedPanes
{
  public:
    [[nodiscard]] std::expected<std::string, vthost::OwnedPaneError> open(
        vthost::OwnedPaneSpawn spawn) override
    {
        opens.push_back(std::move(spawn));
        return openAnswer;
    }

    [[nodiscard]] std::expected<void, vthost::OwnedPaneError> close(std::string_view extension,
                                                                    std::string_view pane) override
    {
        closes.push_back(std::format("{} {}", extension, pane));
        return {};
    }

    [[nodiscard]] std::expected<std::vector<vthost::OwnedPaneInfo>, vthost::OwnedPaneError> list(
        std::string_view /*extension*/) const override
    {
        return listAnswer;
    }

    std::vector<vthost::OwnedPaneSpawn> opens;
    std::vector<std::string> closes;
    std::expected<std::string, vthost::OwnedPaneError> openAnswer = std::string { "pane-1" };
    std::expected<std::vector<vthost::OwnedPaneInfo>, vthost::OwnedPaneError> listAnswer {
        std::unexpect, vthost::OwnedPaneError::NotReconciled
    };
};

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

    void drawStopped(std::string_view extension, std::string_view title, std::string_view lastFault) override
    {
        drawn.push_back(std::format("stopped {} {} {}", extension, title, lastFault));
    }

    void resumeDrawing(std::string_view extension) override
    {
        drawn.push_back(std::format("resume {}", extension));
    }

    std::vector<std::string> calls; ///< What was asked of its tabs, in order.
    std::vector<std::string> drawn; ///< Who was to draw them, in order.
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
            world = makeWorld(
                vthost::DaemonWorldEvents { .requestsChanged = {},
                                            .summaryChanged = [this] { ++summaryChanges; },
                                            .presenceChanged = [this] { ++presenceChanges; } },
                [](std::function<void()> const& job) { job(); },
                vthost::DaemonWorldWiring { .tabs = tabs, .actions = actions, .panes = panes });
    }

    /// @return The daemon world, on this fixture's fakes.
    [[nodiscard]] std::unique_ptr<vthost::DaemonWorldExtensions> makeWorld(vthost::DaemonWorldEvents events,
                                                                           vthost::OwnerPost post,
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
                                          .files = files,
                                          .fileSystem = fileSystem,
                                          .locker = locker,
                                          .wallClock = wallClock,
                                          .localTime = localTime },
            std::move(events),
            std::move(post),
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
    vtextension::testing::FakeBeneathReader files;
    core::platform::ManualWallClock wallClock;
    vtbackend::LocalTimeConverter localTime { &vtbackend::systemLocalTime };
    int summaryChanges = 0;
    int presenceChanges = 0;
    RecordingTabs tabs;
    RecordingOwnedPanes panes;
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
    // A run that is not disabled any more by the time the host asks gets no frame; a running one draws
    // itself.
    CHECK(fixture.tabs.drawn == std::vector<std::string> { "resume broken", "resume broken" });
    world.stop();
}

TEST_CASE("DaemonWorldExtensions: a served-tab call reaches the host in the manifest's words",
          "[daemon][served]")
{
    auto tabs = RecordingTabs {};
    auto panes = RecordingOwnedPanes {};
    auto actions = vthost::ExtensionActions { [](std::string_view, vthost::ServedHint) {} };
    auto const scheme = std::string_view { "agents" };
    auto service = vthost::servedTabServiceOf(
        vtextension::ServedTabOwner {
            .extension = "orchestrator", .title = "Agents", .scheme = scheme, .moduleDigest = "3f9a" },
        vthost::DaemonWorldWiring { .tabs = tabs, .actions = actions, .panes = panes });

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
        vthost::DaemonWorldWiring { .tabs = tabs, .actions = actions, .panes = panes });
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
                                                     vthost::OwnerPost post,
                                                     vthost::DaemonWorldWiring wiring) {
                                      auto made =
                                          fixture.makeWorld(std::move(events), std::move(post), wiring);
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

namespace
{

/// The orchestrator as a daemon module: it serves `agents` under the title Agents, and declares `open`.
constexpr auto ServingManifest =
    "name = orchestrator\nversion = 2\ntitle = Agents\nplacement daemon\nroot work = config:work\n"
    "command run short @work = true\nserve agents\naction open = Open Agents\n"sv;

constexpr auto Orchestrator = std::string_view { "orchestrator" };

/// What the stopped frame says of the orchestrator.
constexpr auto StoppedTitle = std::string_view { "Agents stopped after repeated errors" };
constexpr auto StoppedRestart =
    std::string_view { "Restart it from the command palette: \u201CAgents: restart extension\u201D." };

/// What the scripted guest's `update` does.
enum class UpdateOutcome : uint8_t
{
    Returns = 0, ///< It returns.
    Traps,       ///< It traps: the instance faults.
};

/// Whether a new instance of the module can be made.
enum class InstanceOutcome : uint8_t
{
    Created = 0, ///< It can.
    Refused,     ///< It fails to instantiate: no guest runs at all.
};

/// What the orchestrator's daemon world runs on: fakes of the backend, files, locks, processes and
/// directories, the module on disk, and a scripted guest whose `update` and instantiation can be made to
/// fail.
struct ServingFakes
{
    /// Where the guest's allocator starts, past what the host writes.
    static constexpr auto AllocatorStart = uint32_t { 32 * 1024 };

    ServingFakes()
    {
        directories.add("/w");
        fileSystem.addFile(
            "/ext/orchestrator.wasm",
            textOf(vtextension::testing::withManifest(
                vtextension::testing::toModule(vtextension::testing::DaemonProbeModule), ServingManifest)));
        backend.guest.exports["cabi_realloc"] =
            [this](std::span<uint32_t> argv) -> std::expected<void, vtextension::wasm::CallFailure> {
            next = vtextension::abi::alignTo(next, argv[2]);
            argv[0] = next;
            next += argv[3];
            return {};
        };
        backend.guest.exports["load"] =
            [this](std::span<uint32_t> /*argv*/) -> std::expected<void, vtextension::wasm::CallFailure> {
            next = AllocatorStart;
            ++loads;
            return {};
        };
        backend.guest.exports["update"] =
            [this](std::span<uint32_t> /*argv*/) -> std::expected<void, vtextension::wasm::CallFailure> {
            next = AllocatorStart;
            ++updates;
            if (updateOutcome.load() == UpdateOutcome::Traps)
                return std::unexpected(vtextension::wasm::CallFailure::Trap);
            return {};
        };
        backend.onInstantiate = [this]() -> std::expected<void, vtextension::ExtensionError> {
            if (instanceOutcome.load() == InstanceOutcome::Refused)
                return std::unexpected(vtextension::ExtensionError::InvalidComponent);
            return {};
        };
    }

    ServingFakes(ServingFakes const&) = delete;
    ServingFakes(ServingFakes&&) = delete;
    ServingFakes& operator=(ServingFakes const&) = delete;
    ServingFakes& operator=(ServingFakes&&) = delete;
    ~ServingFakes() = default;

    /// @return The daemon world on these fakes, its faults recovered from at once.
    [[nodiscard]] std::unique_ptr<vthost::DaemonWorldExtensions> makeWorld(vthost::DaemonWorldEvents events,
                                                                           vthost::OwnerPost post,
                                                                           vthost::DaemonWorldWiring wiring)
    {
        auto limits = vtextension::daemonWorldLimits();
        limits.initialBackoff = 1ms;
        return std::make_unique<vthost::DaemonWorldExtensions>(
            vtextension::DaemonExtensionRuntimeConfig {
                .declarations = { vtextension::DaemonExtensionDeclaration {
                    .name = std::string(Orchestrator),
                    .modulePath = "/ext/orchestrator.wasm",
                    .autostart = vtextension::ExtensionAutostart::OnDemand,
                    .configJson = R"({"work":"/w"})",
                    .storeDirectory = "/state/extensions/default/orchestrator/store" } },
                .grantsFile = "/config/extension-grants.daemon-scoped.txt",
                .home = "/home/me",
                .inheritedEnvironment = {},
                .bundledDirectories = {},
                .limits = limits },
            vthost::DaemonWorldServices { .backend = backend,
                                          .runner = runner,
                                          .directories = directories,
                                          .files = files,
                                          .fileSystem = fileSystem,
                                          .locker = locker,
                                          .wallClock = wallClock,
                                          .localTime = localTime },
            std::move(events),
            std::move(post),
            wiring);
    }

    vtextension::testing::FakeWasmBackend backend;
    core::platform::testing::InMemoryFileSystem fileSystem;
    vtextension::testing::FakeFileLocker locker;
    vtextension::testing::FakeProcessRunner runner;
    vtextension::testing::FakeDirectoryOpener directories;
    vtextension::testing::FakeBeneathReader files;
    core::platform::ManualWallClock wallClock;
    vtbackend::LocalTimeConverter localTime { &vtbackend::systemLocalTime };
    std::atomic<int> loads = 0;   ///< How many `load` calls returned.
    std::atomic<int> updates = 0; ///< How many `update` calls were made.
    std::atomic<UpdateOutcome> updateOutcome = UpdateOutcome::Returns;
    std::atomic<InstanceOutcome> instanceOutcome = InstanceOutcome::Created;
    uint32_t next = AllocatorStart; ///< The guest's allocator; on the lane only.
};

/// The orchestrator's daemon world on a live host, granted and running, its faults recovered from at once,
/// and a window that opened its dashboard.
struct ServingWorld: ServingFakes
{
    ServingWorld()
    {
        REQUIRE(world != nullptr);
        auto* const shellTab = live.host.createTab();
        REQUIRE(shellTab != nullptr);
        workspace = live.host.workspaceId();

        // Granted, it runs.
        world->startOnDemand(Orchestrator);
        auto const pending = world->pendingRequests();
        REQUIRE(pending.size() == 1);
        world->answer(Orchestrator, pending.front().grantDigest, vthost::GrantState::Granted);
        REQUIRE(live.eventually([&] { return loads.load() == 1 && isRunning(); }));

        // A window invoked "Open Agents"; the guest opens its dashboard for it.
        window.selected = workspace.value;
        live.host.subscribeStream(&window);
        live.host.noteClientAttached(&window);
        REQUIRE(live.host.invokeExtensionAction(window.connectionId(), Orchestrator, "open")
                == vthost::InvocationEffect::Deliver);
        auto const taken = live.host.extensionActions().take(Orchestrator);
        REQUIRE(taken.size() == 1);
        auto const opened = tabs->open(Orchestrator, "Agents", "3f9a", "agents", taken.front().second);
        REQUIRE(opened.has_value());
        tab = *opened;
        REQUIRE(live.eventually([&] {
            for (auto const session: live.host.sessionIds())
                if (auto const uuid = live.host.objects().uuidOf(session);
                    live.host.servedBy(session) != nullptr && uuid && vtcontrol::toString(*uuid) == *opened)
                    dashboard = session;
            return dashboard.has_value();
        }));
        REQUIRE(tabs->write(Orchestrator, *opened, "\x1b[2J\x1b[Hdashboard").has_value());
        REQUIRE(live.eventually([&] { return page().contains("dashboard"); }));
    }

    ServingWorld(ServingWorld const&) = delete;
    ServingWorld(ServingWorld&&) = delete;
    ServingWorld& operator=(ServingWorld const&) = delete;
    ServingWorld& operator=(ServingWorld&&) = delete;
    ~ServingWorld() { live.host.unsubscribeStream(&window); }

    /// @return Where the orchestrator stands.
    [[nodiscard]] vthost::DaemonExtensionState state() const
    {
        auto const status = world->status(Orchestrator);
        return status ? status->state : vthost::DaemonExtensionState::Stopped;
    }

    [[nodiscard]] bool isRunning() const { return state() == vthost::DaemonExtensionState::Running; }
    [[nodiscard]] bool isDisabled() const { return state() == vthost::DaemonExtensionState::Disabled; }

    /// Has the guest called, as a tab of its changed, and waits until it was -- or until it is disabled.
    void callGuest()
    {
        auto const before = updates.load();
        world->hint(Orchestrator, vthost::ServedHint::Tabs);
        REQUIRE(live.eventually([&] { return updates.load() > before || isDisabled(); }));
    }

    /// Calls the guest, whose `update` traps, until it is disabled.
    /// @return How many of its `update` calls faulted.
    [[nodiscard]] int faultUntilDisabled()
    {
        updateOutcome = UpdateOutcome::Traps;
        auto const before = updates.load();
        for ([[maybe_unused]] auto const attempt: std::views::iota(0, 10))
        {
            if (isDisabled())
                break;
            callGuest();
        }
        REQUIRE(live.eventually([&] { return isDisabled(); }));
        return updates.load() - before;
    }

    /// @return The dashboard's main page, as text.
    [[nodiscard]] std::string page() { return live.pageText(*dashboard); }

    /// @return The dashboard's page size.
    [[nodiscard]] vtbackend::PageSize pageSize()
    {
        auto& terminal = live.host.hostedSession(*dashboard)->terminal();
        auto const lock = std::lock_guard { terminal };
        return terminal.pageSize();
    }

    /// Checks that the dashboard shows the stopped frame, with @p fault and the restart line cut to @p
    /// restart inside a box @p inner columns wide, waiting until it does.
    void expectFrame(std::string_view fault,
                     std::string_view restart = StoppedRestart,
                     std::size_t inner = 65)
    {
        auto const box = vthost::testing::expectedStoppedBox(
            pageSize(),
            { { StoppedTitle, StoppedTitle.size() }, { fault, fault.size() }, { restart, inner } },
            inner);
        auto const shown = live.eventually([&] { return vthost::testing::showsBox(page(), box); });
        INFO(page());
        CHECK(shown);
    }

    /// Types @p keys into the dashboard, then a click, as a window does; waits until both reached its device.
    void typeAndClick(std::string_view keys)
    {
        live.host.writeInputFrom(*dashboard, keys, window.connectionId(), vthost::proto::InputKind::Keyboard);
        live.host.writeInputFrom(
            *dashboard, "\x1b[<0;5;5M", window.connectionId(), vthost::proto::InputKind::Other);
        REQUIRE(live.eventually([&] { return live.host.inputBacklog(*dashboard) == 0; }));
    }

    /// @return Whether the dashboard's terminal has @p mode on.
    [[nodiscard]] bool modeOn(vtbackend::DECMode mode)
    {
        auto& terminal = live.host.hostedSession(*dashboard)->terminal();
        auto const lock = std::lock_guard { terminal };
        return terminal.isModeEnabled(mode);
    }

    /// Turns the loop a few times: long enough for what a hint would set off to have started.
    void settle()
    {
        auto turns = 0;
        std::ignore = live.eventually([&] { return ++turns > 5; });
    }

    vthost::IServedTabs* tabs = nullptr;
    vthost::DaemonWorldExtensions* world = nullptr;
    vthost::testing::LiveHost live {
        [](vtpty::PageSize size) -> std::unique_ptr<vtpty::Pty> {
            // A shell that never ends by itself.
            return std::make_unique<vtpty::ChannelPty>(size);
        },
        vthost::SessionHostConfig { .daemonWorld =
                                        [this](vthost::DaemonWorldEvents events,
                                               vthost::OwnerPost post,
                                               vthost::DaemonWorldWiring wiring) {
                                            tabs = &wiring.tabs;
                                            auto made = makeWorld(std::move(events), std::move(post), wiring);
                                            world = made.get();
                                            return std::shared_ptr<vthost::IDaemonWorldHosting> { std::move(
                                                made) };
                                        },
                                    .uuids = std::make_shared<vtcontrol::testing::SequentialUuidSource>(),
                                    .servedIds = std::make_shared<vthost::testing::ServedUuids>() }
    };
    vtworkspace::WorkspaceId workspace {};
    FakeWindow window { 1 };
    std::optional<vtworkspace::SessionId> dashboard;
    std::string tab; ///< The dashboard's id, as the guest names it.
};

} // namespace

TEST_CASE("DaemonWorldExtensions: three faults draw the stopped frame", "[daemon][served]")
{
    auto bench = ServingWorld {};
    CHECK(bench.faultUntilDisabled() == 3);
    CHECK(bench.world->status(Orchestrator)->lastFault == "extension trapped");

    // The host's frame replaced the dashboard, fault included.
    bench.expectFrame("extension trapped");
    CHECK_FALSE(bench.page().contains("dashboard"));

    // No guest call happened after the third fault, whatever it is hinted.
    auto const loads = bench.loads.load();
    auto const updates = bench.updates.load();
    bench.world->hint(Orchestrator, vthost::ServedHint::Tabs);
    bench.world->hint(Orchestrator, vthost::ServedHint::Actions);
    bench.settle();
    CHECK(bench.loads.load() == loads);
    CHECK(bench.updates.load() == updates);
    CHECK(bench.isDisabled());
}

TEST_CASE("DaemonWorldExtensions: the stopped frame is redrawn on resize", "[daemon][served]")
{
    auto bench = ServingWorld {};
    std::ignore = bench.faultUntilDisabled();
    bench.expectFrame("extension trapped");

    // 50 x 20: the box fills the width at line 7, 46 columns inside; the restart line is cut with an
    // ellipsis.
    std::ignore = bench.live.host.applyClientSize(
        &bench.window,
        vtpty::PageSize { vtbackend::LineCount(20), vtbackend::ColumnCount(50) },
        std::nullopt);
    REQUIRE(bench.live.eventually([&] { return bench.pageSize().columns == vtbackend::ColumnCount(50); }));
    bench.expectFrame("extension trapped", "Restart it from the command palette: \u201CAgents:\u2026", 46);
}

TEST_CASE("DaemonWorldExtensions: the frame is drawn without any guest", "[daemon][served]")
{
    auto bench = ServingWorld {};
    // The module fails to instantiate from now on: after the first fault no guest runs again.
    bench.instanceOutcome = InstanceOutcome::Refused;
    auto const loads = bench.loads.load();
    CHECK(bench.faultUntilDisabled() == 1);
    CHECK(bench.loads.load() == loads);
    CHECK(bench.world->status(Orchestrator)->lastFault == "invalid WebAssembly module");
    bench.expectFrame("invalid WebAssembly module");
}

TEST_CASE("DaemonWorldExtensions: the palette row becomes restart, and restart re-enables once",
          "[daemon][served][presence]")
{
    auto bench = ServingWorld {};
    std::ignore = bench.faultUntilDisabled();
    bench.expectFrame("extension trapped");
    auto const rows = bench.world->actionRows();
    REQUIRE(rows.size() == 1);
    CHECK(rows.front().state == vthost::DaemonExtensionState::Disabled);
    CHECK(rows.front().title == "Agents");

    // The palette's restart row names no action; invoking it restarts the extension, which loads again. A
    // hint raised while it was disabled is still pending, and reaches it then -- a hint, never input: what is
    // typed into the host's frame is dropped. The guest takes that hint before it faults again.
    bench.updateOutcome = UpdateOutcome::Returns;
    auto const loads = bench.loads.load();
    CHECK(bench.live.host.invokeExtensionAction(bench.window.connectionId(), Orchestrator, "")
          == vthost::InvocationEffect::Restart);
    REQUIRE(bench.live.eventually([&] { return bench.loads.load() == loads + 1 && bench.isRunning(); }));
    // Until the restarted guest draws, the tab says that it restarts.
    CHECK(bench.live.eventually([&] { return bench.page().contains("Agents restarting\u2026"); }));
    bench.callGuest();
    bench.updateOutcome = UpdateOutcome::Traps;

    // A fresh fault window: two faults leave it running, each followed by a new instance.
    for (auto const fault: std::views::iota(1, 3))
    {
        bench.callGuest();
        REQUIRE(bench.live.eventually([&] { return bench.loads.load() == loads + 1 + fault; }));
        CHECK(bench.isRunning());
    }
    // The third disables it again, and the frame is back.
    bench.callGuest();
    CHECK(bench.live.eventually([&] { return bench.isDisabled(); }));
    bench.expectFrame("extension trapped");
}

TEST_CASE("DaemonWorldExtensions: what is typed into the host's frame never reaches the restarted extension",
          "[daemon][served]")
{
    auto bench = ServingWorld {};
    // The dashboard asked for mouse and focus reports, and something was typed it never read.
    REQUIRE(bench.tabs->write(Orchestrator, bench.tab, "\x1b[?1000h\x1b[?1006h\x1b[?1004h").has_value());
    REQUIRE(
        bench.live.eventually([&] { return bench.modeOn(vtbackend::DECMode::MouseProtocolNormalTracking); }));
    bench.typeAndClick("q");

    std::ignore = bench.faultUntilDisabled();
    bench.expectFrame("extension trapped");
    // The frame turned the reports off: the terminal sends none for it.
    CHECK_FALSE(bench.modeOn(vtbackend::DECMode::MouseProtocolNormalTracking));
    CHECK_FALSE(bench.modeOn(vtbackend::DECMode::FocusTracking));
    bench.typeAndClick("jk\r");

    // Restarted, and typed into while it says so.
    bench.updateOutcome = UpdateOutcome::Returns;
    auto const loads = bench.loads.load();
    REQUIRE(bench.live.host.invokeExtensionAction(bench.window.connectionId(), Orchestrator, "")
            == vthost::InvocationEffect::Restart);
    REQUIRE(bench.live.eventually([&] { return bench.loads.load() == loads + 1 && bench.isRunning(); }));
    REQUIRE(bench.live.eventually([&] { return bench.page().contains("Agents restarting\u2026"); }));
    bench.typeAndClick("x");
    CHECK(bench.tabs->read(Orchestrator, bench.tab)
          == std::expected<std::string, vthost::ServedTabError> { "" });

    // Once it draws again, what is typed is its own.
    REQUIRE(bench.tabs->write(Orchestrator, bench.tab, "\x1b[2J\x1b[Hdashboard").has_value());
    bench.live.host.writeInputFrom(
        *bench.dashboard, "k", bench.window.connectionId(), vthost::proto::InputKind::Keyboard);
    auto read = std::string {};
    CHECK(bench.live.eventually([&] {
        read += bench.tabs->read(Orchestrator, bench.tab).value_or(std::string {});
        return !read.empty();
    }));
    CHECK(read == "k");
}

#ifndef _WIN32 // The native reader reads beneath a directory handle, which Windows has none of.

namespace
{

/// A daemon module that may read files beneath its one root.
constexpr auto ReadingManifest =
    "name = reader\nversion = 2\ntitle = Reader\nplacement daemon\nroot work = config:work\n"
    "read work\ncommand run short @work = true\n"sv;

} // namespace

TEST_CASE("DaemonWorldExtensions: the daemon world reads files through the native reader",
          "[daemon][hosting][file-read]")
{
    // A real directory, its file read by the production reader beneath the production opener's handle.
    auto const temp = core::testing::ScopedTempDir { "contour-daemon-file-read" };
    auto const work = std::filesystem::canonical(temp.path());
    std::filesystem::create_directory(work / "run-1");
    {
        auto out = std::ofstream { work / "run-1" / ".git", std::ios::binary };
        out << "gitdir: /src/repo/.git/worktrees/run-1\n";
    }
    std::filesystem::create_symlink("/etc/passwd", work / "run-1" / "escape");

    using FileResult = std::expected<vtextension::FileContent, vtextension::FileError>;
    constexpr auto RootAddress = uint32_t { 100 };
    constexpr auto PathAddress = uint32_t { 200 };
    constexpr auto ResultAddress = uint32_t { 2048 };
    auto backend = vtextension::testing::FakeWasmBackend {};
    auto results = std::vector<FileResult> {};
    auto loaded = std::atomic<bool> { false };
    auto next = uint32_t { 4096 };
    backend.guest.exports["cabi_realloc"] =
        [&next](std::span<uint32_t> argv) -> std::expected<void, vtextension::wasm::CallFailure> {
        next = vtextension::abi::alignTo(next, argv[2]);
        argv[0] = next;
        next += argv[3];
        return {};
    };
    // Its load reads one file, and one link, as the guest would.
    backend.guest.exports["load"] =
        [&](std::span<uint32_t> /*argv*/) -> std::expected<void, vtextension::wasm::CallFailure> {
        auto& instance = *backend.lastInstance;
        auto const put = [&instance](uint32_t address, std::string_view text) {
            std::ranges::transform(text, instance.linearMemory.begin() + address, [](char c) {
                return static_cast<std::byte>(c);
            });
        };
        auto memory = vtextension::testing::FakeInstanceMemory { instance };
        put(RootAddress, "work");
        for (auto const path: { "run-1/.git"sv, "run-1/escape"sv })
        {
            put(PathAddress, path);
            auto const arguments = std::array {
                RootAddress, uint32_t { 4 }, PathAddress, static_cast<uint32_t>(path.size()), ResultAddress
            };
            if (!instance.callHost(std::to_underlying(vtextension::DaemonImport::FileRead), arguments))
                return std::unexpected(vtextension::wasm::CallFailure::Trap);
            results.push_back(*vtextension::abi::Codec<FileResult>::load(memory, ResultAddress));
        }
        loaded = true;
        return {};
    };

    auto fileSystem = core::platform::testing::InMemoryFileSystem {};
    fileSystem.addDirectory(work);
    fileSystem.addFile(
        "/ext/reader.wasm",
        textOf(vtextension::testing::withManifest(
            vtextension::testing::toModule(vtextension::testing::DaemonProbeModule), ReadingManifest)));
    auto locker = vtextension::testing::FakeFileLocker {};
    auto runner = vtextension::testing::FakeProcessRunner {};
    auto wallClock = core::platform::ManualWallClock {};
    auto localTime = vtbackend::LocalTimeConverter { &vtbackend::systemLocalTime };
    auto tabs = RecordingTabs {};
    auto panes = RecordingOwnedPanes {};
    auto actions =
        vthost::ExtensionActions { [](std::string_view /*extension*/, vthost::ServedHint /*hint*/) {} };
    auto world = vthost::DaemonWorldExtensions(
        vtextension::DaemonExtensionRuntimeConfig {
            .declarations = { vtextension::DaemonExtensionDeclaration {
                .name = "reader",
                .modulePath = "/ext/reader.wasm",
                .autostart = vtextension::ExtensionAutostart::OnDemand,
                .configJson = std::format(R"({{"work":"{}"}})", work.string()),
                .storeDirectory = "/state/extensions/default/reader/store" } },
            .grantsFile = "/config/extension-grants.daemon-scoped.txt",
            .home = "/home/me",
            .inheritedEnvironment = {},
            .bundledDirectories = {} },
        vthost::DaemonWorldServices { .backend = backend,
                                      .runner = runner,
                                      .directories = vtpty::nativeDirectoryOpener(),
                                      .files = vtpty::nativeBeneathReader(),
                                      .fileSystem = fileSystem,
                                      .locker = locker,
                                      .wallClock = wallClock,
                                      .localTime = localTime },
        vthost::DaemonWorldEvents {},
        [](std::function<void()> const& job) { job(); },
        vthost::DaemonWorldWiring { .tabs = tabs, .actions = actions, .panes = panes });

    world.startOnDemand("reader");
    auto const pending = world.pendingRequests();
    REQUIRE(pending.size() == 1);
    world.answer("reader", pending.front().grantDigest, vthost::GrantState::Granted);
    auto const deadline = std::chrono::steady_clock::now() + 10s;
    while (!loaded.load() && std::chrono::steady_clock::now() < deadline)
        std::this_thread::sleep_for(5ms);
    REQUIRE(loaded.load());
    world.stop();

    REQUIRE(results.size() == 2);
    REQUIRE(results[0].has_value());
    auto const text = "gitdir: /src/repo/.git/worktrees/run-1\n"sv;
    CHECK(results[0]->bytes == vtextension::ByteList(text.begin(), text.end()));
    CHECK(results[0]->modifiedUnixMs != 0);
    CHECK(results[1] == FileResult { std::unexpect, vtextension::FileError::Escapes });
}

#endif

TEST_CASE("DaemonWorldExtensions: the pane calls reach the host's owned panes in the world's words",
          "[daemon][panes]")
{
    auto tabs = RecordingTabs {};
    auto panes = RecordingOwnedPanes {};
    auto actions = vthost::ExtensionActions { [](std::string_view, vthost::ServedHint) {} };
    auto host = vthost::paneHostOf(
        vtextension::ServedTabOwner {
            .extension = "orchestrator", .title = "Agents", .scheme = std::nullopt, .moduleDigest = "" },
        vthost::DaemonWorldWiring { .tabs = tabs, .actions = actions, .panes = panes });

    // The spawn reaches the host whole, under the extension's name.
    auto const directory =
        std::make_shared<vtpty::DirectoryHandle const>(vtpty::DirectoryHandle::unbacked("/w/run-1"));
    CHECK(host->open(vtextension::PaneSpawn { .argv = { "claude" },
                                              .environment = { { "GIT_CONFIG_COUNT", "2" } },
                                              .removedEnvironment = { "GIT_DIR" },
                                              .directory = directory,
                                              .cwd = "/w/run-1",
                                              .title = "run-1",
                                              .tag = "run-1",
                                              .near = "tab-7" })
          == std::expected<std::string, vtextension::PaneError> { "pane-1" });
    REQUIRE(panes.opens.size() == 1);
    CHECK(panes.opens.front().extension == "orchestrator");
    CHECK(panes.opens.front().argv == std::vector<std::string> { "claude" });
    CHECK(panes.opens.front().removedEnvironment == std::vector<std::string> { "GIT_DIR" });
    CHECK(panes.opens.front().directory == directory);
    CHECK(panes.opens.front().near == "tab-7");

    CHECK(host->close("pane-1").has_value());
    CHECK(panes.closes == std::vector<std::string> { "orchestrator pane-1" });

    // Not reconciled yet: the guest is told to retry (limit), never that its panes are gone.
    CHECK(host->list()
          == std::expected<std::vector<vtextension::PaneInfo>, vtextension::PaneError> {
              std::unexpect, vtextension::PaneError::NotReconciled });
    CHECK(vtextension::hostErrorOf(vtextension::PaneError::NotReconciled) == vtextension::HostError::Limit);

    // A pane's status and program, as the world spells them.
    struct Row
    {
        std::optional<vtbackend::ProgramState> status;
        std::optional<vtextension::AgentStatus> agentStatus;
    };
    auto const row = GENERATE(Row { std::nullopt, std::nullopt },
                              Row { vtbackend::ProgramState::Idle, vtextension::AgentStatus::Idle },
                              Row { vtbackend::ProgramState::Working, vtextension::AgentStatus::Working },
                              Row { vtbackend::ProgramState::Done, vtextension::AgentStatus::Done },
                              Row { vtbackend::ProgramState::Blocked, vtextension::AgentStatus::Blocked },
                              Row { vtbackend::ProgramState::Error, vtextension::AgentStatus::Error });
    panes.listAnswer = std::vector { vthost::OwnedPaneInfo { .pane = "pane-1",
                                                             .tag = "run-1",
                                                             .status = row.status,
                                                             .program = vthost::PaneLiveness::Gone,
                                                             .state = vthost::OwnedPaneState::Open } };
    CHECK(host->list()
          == std::expected<std::vector<vtextension::PaneInfo>, vtextension::PaneError> {
              std::vector { vtextension::PaneInfo { .pane = "pane-1",
                                                    .tag = "run-1",
                                                    .status = row.agentStatus,
                                                    .program = vtextension::PaneProgram::Gone } } });

    // The host refusals, as the policy words them; reveal waits for a later phase.
    struct Refusal
    {
        vthost::OwnedPaneError host;
        vtextension::PaneError policy;
    };
    for (auto const& [hostError, policyError]: std::array {
             Refusal { .host = vthost::OwnedPaneError::TooMany, .policy = vtextension::PaneError::TooMany },
             Refusal { .host = vthost::OwnedPaneError::Upgrading,
                       .policy = vtextension::PaneError::HostRefused },
             Refusal { .host = vthost::OwnedPaneError::ProgramMissing,
                       .policy = vtextension::PaneError::ProgramMissing },
             Refusal { .host = vthost::OwnedPaneError::NotSupported,
                       .policy = vtextension::PaneError::NotSupportedYet },
             Refusal { .host = vthost::OwnedPaneError::DirectoryGone,
                       .policy = vtextension::PaneError::CwdMissing },
         })
    {
        panes.openAnswer = std::unexpected(hostError);
        CHECK(host->open(vtextension::PaneSpawn { .argv = { "claude" } })
              == std::expected<std::string, vtextension::PaneError> { std::unexpect, policyError });
    }
    CHECK(host->reveal("pane-1", 1)
          == std::expected<void, vtextension::PaneError> { std::unexpect,
                                                           vtextension::PaneError::NotSupportedYet });
}
