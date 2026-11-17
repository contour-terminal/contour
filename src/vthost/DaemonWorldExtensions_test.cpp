// SPDX-License-Identifier: Apache-2.0
//
// The production daemon world behind vthost's interface: what it shows of an extension that could not run, on
// the fake WebAssembly backend and fakes of the files, the locks, the processes and the directories.
#include <vtbackend/screen/LocalTime.hpp>

#include <core/platform/Clock.hpp>
#include <core/platform/testing/InMemoryFileSystem.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include <vtextension/DaemonExtensionRuntime.hpp>
#include <vtextension/testing/FakeDirectoryOpener.hpp>
#include <vtextension/testing/FakeFileLocker.hpp>
#include <vtextension/testing/FakeProcessRunner.hpp>
#include <vtextension/testing/FakeWasmBackend.hpp>
#include <vtextension/testing/TestModules.hpp>
#include <vthost/DaemonWorldExtensions.hpp>

using namespace std::string_view_literals;

namespace
{

/// A daemon module by its manifest, which the runtime refuses to load: it has a start function.
constexpr auto BrokenManifest =
    "name = broken\nversion = 2\nplacement daemon\nroot work = config:work\ncommand run short @work = true\n"sv;

/// @return @p bytes as file content.
[[nodiscard]] std::string textOf(std::vector<std::byte> const& bytes)
{
    auto text = std::string(bytes.size(), '\0');
    std::ranges::transform(bytes, text.begin(), [](std::byte b) { return static_cast<char>(b); });
    return text;
}

/// The daemon world of one on-demand extension, `broken`, whose module does not load.
struct Fixture
{
    Fixture()
    {
        directories.add("/w");
        fileSystem.addFile(
            "/ext/broken.wasm",
            textOf(vtextension::testing::withManifest(
                vtextension::testing::toModule(vtextension::testing::StartFunctionModule), BrokenManifest)));
        world = std::make_unique<vthost::DaemonWorldExtensions>(
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
            vthost::DaemonWorldEvents { .requestsChanged = {},
                                        .summaryChanged = [this] { ++summaryChanges; } });
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

    // Revoked, the failure goes with the grant.
    world.revoke("broken");
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
