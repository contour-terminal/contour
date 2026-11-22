// SPDX-License-Identifier: Apache-2.0
#include <vtpty/DirectoryHandle.hpp>

#include <core/Environment.hpp>
#include <core/async/Task.hpp>
#include <core/net/EventLoop.hpp>
#include <core/net/testing/CoroTestSupport.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <expected>
#include <filesystem>
#include <format>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <ranges>
#include <string>
#include <string_view>
#include <tuple>
#include <utility>
#include <variant>
#include <vector>

#include <vthost/ExtensionHosting.hpp>
#include <vthost/LastSessionWatcher.hpp>
#include <vthost/OwnedPaneStore.hpp>
#include <vthost/OwnedPanes.hpp>
#include <vthost/SessionHost.hpp>
#include <vthost/SessionSnapshot.hpp>
#include <vthost/handover/HandoverCapture.hpp>
#include <vthost/handover/HandoverCodec.hpp>
#include <vthost/handover/HandoverRestore.hpp>
#include <vthost/handover/HandoverToken.hpp>
#include <vthost/handover/ImageHandoff.hpp>
#include <vthost/handover/LiveUpgrade.hpp>
#include <vthost/keeper/Messages.hpp>
#include <vthost/keeper/UniqueFd.hpp>
#include <vthost/keeperlink/Recovery.hpp>
#include <vthost/keeperlink/Structure.hpp>
#include <vthost/testing/FakeDaemonWorldHosting.hpp>
#include <vthost/testing/FakeHandoverMemory.hpp>
#include <vthost/testing/FakeProcessImageReplacer.hpp>
#include <vthost/testing/HandoverTesting.hpp>
#include <vthost/testing/LiveUpgradeTesting.hpp>
#include <vthost/testing/ServedEndToEnd.hpp>
#include <vthost/testing/ServedTesting.hpp>

#ifdef VTHOST_TEST_ORCHESTRATOR_WASM
    #include <vthost/testing/OrchestratorDashboard.hpp>
#endif

using namespace vthost;
using namespace vthost::handover;
using namespace std::chrono_literals;
using vthost::testing::EndRecorder;
using vthost::testing::holdsExactly;
using vthost::testing::idsOf;
using vthost::testing::ImageReplaced;
using vthost::testing::LiveHost;
using vthost::testing::openServedTab;
using vthost::testing::recoverFromKeeper;
using vthost::testing::releaseForAdoption;
using vthost::testing::ServedText;
using vthost::testing::ServedUpgrade;
using vtworkspace::SessionId;

namespace
{

/// @return The echoing shell on a PTY of its own.
[[nodiscard]] std::unique_ptr<vtpty::Pty> catDevice(vtpty::PageSize size)
{
    return vthost::testing::shellProcess(size);
}

/// The sessions of a host with two shells side by side in one tab, and a served tab beside it.
struct ServedLayout
{
    std::vector<SessionId> shells;
    SessionId served {};
};

/// Spawns two shells side by side in one tab of @p live, and opens a served tab beside it.
/// @return The sessions, once every screen is drawn.
[[nodiscard]] ServedLayout spawnServedLayout(LiveHost& live)
{
    live.spawnTwo();
    auto layout = ServedLayout { .shells = live.host.sessionIds() };
    auto const opened = openServedTab(live.host);
    REQUIRE(opened.has_value());
    layout.served = *opened;
    REQUIRE(live.eventually([&] {
        return std::ranges::all_of(layout.shells,
                                   [&](SessionId id) { return live.pageText(id).contains("ready"); })
               && live.pageText(layout.served).contains(ServedText);
    }));
    return layout;
}

/// Opens a served tab in @p live, as the extension reopening it would, and waits for its screen.
void reopenServed(LiveHost& live)
{
    auto const reopened = openServedTab(live.host);
    REQUIRE(reopened.has_value());
    CHECK(live.host.servedBy(*reopened) != nullptr);
    REQUIRE(live.eventually([&] { return live.pageText(*reopened).contains(ServedText); }));
}

} // namespace

TEST_CASE(
    "(a) a live upgrade with a served tab open completes; the tab is gone, every other session survives",
    "[daemon][served][upgrade]")
{
    auto u = ServedUpgrade {};
    auto recorder = EndRecorder {};
    auto subscription = std::optional<ScopedStreamSubscription> {};
    subscription.emplace(
        u.live->host, recorder, &SessionHost::subscribeStream, &SessionHost::unsubscribeStream);
    auto const old = spawnServedLayout(*u.live);

    // Admission stays closed for the whole run: while the clients go, and up to the exec.
    auto refusedWhileClosing = std::optional<bool> {};
    u.hooks.onClose = [&] {
        refusedWhileClosing = !openServedTab(u.live->host).has_value();
    };
    auto next = std::unique_ptr<LiveHost> {};
    auto image = std::optional<HandoverImage> {};
    u.replacer.onReplace = [&] {
        CHECK(u.live->host.servedAdmission() == ServedAdmission::Closed);
        CHECK(u.hooks.served == std::vector<std::string> { "close" });
        auto const& [program, argv] = u.replacer.replacements.back();
        auto const fd = handoverTokenOf(argv.back()).and_then(parseHandoverDescriptor);
        REQUIRE(fd.has_value());
        auto decoded = readHandover(u.memory, *fd);
        REQUIRE(decoded.has_value());
        image = *decoded;
        releaseForAdoption(u.live->host);
        auto config = LiveHost::sequentialIds();
        config.handover = *std::move(decoded);
        next = std::make_unique<LiveHost>(catDevice, std::move(config));
        throw ImageReplaced {};
    };
    auto upgrade = u.orchestrator(u.handoff);
    REQUIRE_THROWS_AS(std::ignore = u.run(upgrade, UpgradeTarget { .imageId = u.imageId }), ImageReplaced);

    // The served tab was closed by the upgrade, before anything was captured; the image holds the shells.
    REQUIRE(image.has_value());
    CHECK(idsOf(image->sessions) == old.shells);
    CHECK(std::ranges::find(recorder.ends, std::pair { old.served, SessionEnd::Upgrade })
          != recorder.ends.end());
    REQUIRE_FALSE(u.hooks.served.empty());
    CHECK(u.hooks.served.front() == "close");
    CHECK(refusedWhileClosing == std::optional { true });

    subscription.reset();
    u.acceptor.reset();
    u.live.reset();

    // The new image runs the same shells, and the extension opens its tab again there.
    REQUIRE(next != nullptr);
    CHECK(next->host.handoverOutcome().has_value());
    CHECK(holdsExactly(next->host, old.shells));
    for (auto const id: old.shells)
        next->host.writeInput(id, "after-upgrade");
    REQUIRE(next->eventually([&] {
        return std::ranges::all_of(
            old.shells, [&](SessionId id) { return next->pageText(id).contains("after-upgrade"); });
    }));
    reopenServed(*next);
}

TEST_CASE("(b) a keeper recovery drops a served tab, keeps every other session, and it can be reopened",
          "[daemon][served][keeper]")
{
    auto live = std::make_unique<LiveHost>(catDevice);
    auto const old = spawnServedLayout(*live);

    auto image = recoverFromKeeper(live, old.shells, old.served);
    CHECK(idsOf(image.sessions) == old.shells);

    auto config = LiveHost::sequentialIds();
    config.handover = std::move(image);
    auto const next = std::make_unique<LiveHost>(catDevice, std::move(config));
    CHECK(holdsExactly(next->host, old.shells));
    next->host.writeInput(old.shells.front(), "recovered");
    REQUIRE(next->eventually([&] { return next->pageText(old.shells.front()).contains("recovered"); }));
    reopenServed(*next);
}

TEST_CASE("(d) a capture and restore round trip with a served session present", "[daemon][served][handover]")
{
    auto live = std::make_unique<LiveHost>(catDevice);
    auto const old = spawnServedLayout(*live);
    auto const uuids = std::ranges::to<std::vector<std::optional<vtcontrol::Uuid>>>(
        old.shells | std::views::transform([&](SessionId id) { return live->host.objects().uuidOf(id); }));

    // Without the close the capture refuses it, as it refuses every device no new image could adopt.
    auto const refused = checkCapturable(live->host);
    REQUIRE_FALSE(refused.has_value());
    CHECK(refused.error() == HandoverError::UnsupportedSession);

    live->host.closeServedSessions();
    REQUIRE(checkCapturable(live->host).has_value());
    live->settle();
    auto const captured = live->capture();
    REQUIRE(captured.has_value());
    auto decoded = decodeHandover(encodeHandover(*captured));
    REQUIRE(decoded.has_value());
    CHECK(idsOf(decoded->sessions) == old.shells);

    releaseForAdoption(live->host);
    live.reset();
    auto config = LiveHost::sequentialIds();
    config.handover = *std::move(decoded);
    auto const next = std::make_unique<LiveHost>(catDevice, std::move(config));
    CHECK(next->host.handoverOutcome().has_value());
    CHECK(holdsExactly(next->host, old.shells));
    for (auto const [id, uuid]: std::views::zip(old.shells, uuids))
        CHECK(next->host.objects().uuidOf(id) == uuid);
}

// ---------------------------------------------------------------------------------------------------------
// Owned panes, in the host's real construction order: the registry is read before any restore, and nothing
// lists it before the constructor's posted reconciliation ran -- a boot extension asking at once is told to
// retry (WIT `limit`), never that its agent's pane is gone.

namespace
{

constexpr auto PanesOwner = std::string_view { "orchestrator" };

/// A daemon world whose boot extension lists its panes as it is made, during the host's construction; it
/// journals the hints it is raised afterwards.
struct BootLister
{
    std::shared_ptr<std::vector<std::string>> journal = std::make_shared<std::vector<std::string>>();
    std::optional<std::expected<std::vector<OwnedPaneInfo>, OwnedPaneError>> listedAtBoot;

    [[nodiscard]] DaemonWorldFactory factory()
    {
        return [this](DaemonWorldEvents events, OwnerPost post, DaemonWorldWiring wiring) {
            listedAtBoot = wiring.panes.list(PanesOwner);
            return std::make_shared<vthost::testing::FakeDaemonWorldHosting>(
                journal, std::move(post), std::move(events));
        };
    }

    /// @return How many times the extension was raised ServedHint::Panes.
    [[nodiscard]] std::size_t paneHints() const
    {
        return static_cast<std::size_t>(
            std::ranges::count(*journal, std::format("hint {} panes", PanesOwner)));
    }
};

/// @return A registry holding @p pane as the extension's agent pane `run-1`.
[[nodiscard]] std::shared_ptr<InMemoryOwnedPaneStore> registryOf(vtcontrol::Uuid pane)
{
    return std::make_shared<InMemoryOwnedPaneStore>(
        std::map<std::string, std::vector<OwnedPaneRecord>, std::less<>> {
            { std::string(PanesOwner), { OwnedPaneRecord { .pane = pane, .tag = "run-1" } } } });
}

/// @return A configuration naming objects with sequential UUIDs, its registry @p registry, its daemon world
///         @p lister's.
[[nodiscard]] SessionHostConfig withRegistry(std::shared_ptr<IOwnedPaneStore> registry, BootLister& lister)
{
    auto config = LiveHost::sequentialIds();
    config.ownedPaneStore = std::move(registry);
    config.daemonWorld = lister.factory();
    return config;
}

/// Checks what @p next lists once reconciled: the agent's pane @p pane, alive, raised once.
void checkReconciledAgent(LiveHost& next, BootLister const& lister, vtcontrol::Uuid pane)
{
    // Asked during the construction, before the restore: retry later, nothing concluded.
    REQUIRE(lister.listedAtBoot.has_value());
    CHECK(*lister.listedAtBoot
          == std::expected<std::vector<OwnedPaneInfo>, OwnedPaneError> { std::unexpect,
                                                                         OwnedPaneError::NotReconciled });
    REQUIRE(next.eventually([&] { return next.host.ownedPanes().list(PanesOwner).has_value(); }));
    CHECK(next.host.ownedPanes().list(PanesOwner)
          == std::expected<std::vector<OwnedPaneInfo>, OwnedPaneError> {
              std::vector { OwnedPaneInfo { .pane = vtcontrol::toString(pane),
                                            .tag = "run-1",
                                            .status = std::nullopt,
                                            .program = PaneLiveness::Alive,
                                            .state = OwnedPaneState::Open } } });
    REQUIRE(next.eventually([&] { return lister.paneHints() == 1; }));
}

} // namespace

TEST_CASE("owned panes are reconciled after a live upgrade's restore", "[daemon][panes][handover]")
{
    // The old host owns the agent's pane itself, so that a capture which dropped it would fail this.
    auto const registry = std::make_shared<InMemoryOwnedPaneStore>();
    auto oldLister = BootLister {};
    auto live = std::make_unique<LiveHost>(catDevice, withRegistry(registry, oldLister));
    live->spawnTwo();
    REQUIRE(live->eventually([&] { return live->host.ownedPanes().list(PanesOwner).has_value(); }));
    auto const directory = std::make_shared<vtpty::DirectoryHandle const>(
        vtpty::DirectoryHandle::unbacked(std::filesystem::temp_directory_path().string()));
    auto const opened = live->host.ownedPanes().open(OwnedPaneSpawn { .extension = std::string(PanesOwner),
                                                                      .argv = { "claude" },
                                                                      .directory = directory,
                                                                      .cwd = directory->resolvedPath(),
                                                                      .title = "run-1",
                                                                      .tag = "run-1" });
    REQUIRE(opened.has_value());
    auto const agent = vtcontrol::parseUuid(*opened);
    REQUIRE(agent.has_value());
    REQUIRE(live->eventually([&] {
        auto const listed = live->host.ownedPanes().list(PanesOwner);
        return listed && listed->size() == 1 && listed->front().state == OwnedPaneState::Open;
    }));
    for (auto const session: live->host.sessionIds())
        REQUIRE(live->eventually([&] { return live->pageText(session).contains("ready"); }));
    live->settle();
    auto captured = live->capture();
    REQUIRE(captured.has_value());
    auto decoded = decodeHandover(encodeHandover(*captured));
    REQUIRE(decoded.has_value());
    // Read before the old host goes, whose destruction would write it: the exec runs no destructor, so the
    // registry is what LiveUpgrade flushed before it (LiveUpgrade_test pins that it does).
    live->host.flushOwnedPanes();
    CHECK(registry->loadAll()
          == std::vector<std::pair<std::string, std::vector<OwnedPaneRecord>>> {
              { std::string(PanesOwner), { OwnedPaneRecord { .pane = *agent, .tag = "run-1" } } } });
    releaseForAdoption(live->host);
    live.reset();

    auto lister = BootLister {};
    auto config = withRegistry(registry, lister);
    config.handover = *std::move(decoded);
    auto const next = std::make_unique<LiveHost>(catDevice, std::move(config));
    CHECK(next->host.handoverOutcome().has_value());
    checkReconciledAgent(*next, lister, *agent);
}

TEST_CASE("owned panes are reconciled after a keeper recovery", "[daemon][panes][keeper]")
{
    auto live = std::make_unique<LiveHost>(catDevice);
    auto const old = spawnServedLayout(*live);
    auto const agent = live->host.objects().uuidOf(old.shells.back());
    REQUIRE(agent.has_value());
    auto image = recoverFromKeeper(live, old.shells, old.served);

    auto lister = BootLister {};
    auto config = withRegistry(registryOf(*agent), lister);
    config.handover = std::move(image);
    auto const next = std::make_unique<LiveHost>(catDevice, std::move(config));
    CHECK(holdsExactly(next->host, old.shells));
    checkReconciledAgent(*next, lister, *agent);
}

TEST_CASE("owned panes are reconciled on the normal constructor exit", "[daemon][panes][restore]")
{
    // A pane the registry names, which a cold start does not bring back.
    auto const gone = vtcontrol::Uuid { std::array<uint8_t, 16> { 0xAB, 0xCD, 0, 0, 0, 0, 0x40, 0, 0x80 } };
    auto const registry = registryOf(gone);
    auto lister = BootLister {};
    auto config = withRegistry(registry, lister);
    SECTION("a fresh start")
    {
    }
    SECTION("a snapshot restore")
    {
        auto workspace = WorkspaceLayoutSnapshot {};
        workspace.tabs.push_back(TabSnapshot { .root = PaneNode { .pane = PaneSnapshot {} } });
        config.restore =
            WorkspaceRestore { .snapshot = WorkspaceSnapshot { .workspaces = { std::move(workspace) } } };
    }
    auto const next = std::make_unique<LiveHost>(catDevice, std::move(config));

    REQUIRE(lister.listedAtBoot.has_value());
    CHECK(*lister.listedAtBoot
          == std::expected<std::vector<OwnedPaneInfo>, OwnedPaneError> { std::unexpect,
                                                                         OwnedPaneError::NotReconciled });
    REQUIRE(next->eventually([&] { return next->host.ownedPanes().list(PanesOwner).has_value(); }));
    CHECK(next->host.ownedPanes().list(PanesOwner)->empty());
    REQUIRE(next->eventually([&] { return lister.paneHints() == 1; }));
}

TEST_CASE("an extension told to retry hears Panes though it owns no pane", "[daemon][panes][restore]")
{
    auto lister = BootLister {};
    auto const next = std::make_unique<LiveHost>(
        catDevice, withRegistry(std::make_shared<InMemoryOwnedPaneStore>(), lister));
    REQUIRE(lister.listedAtBoot.has_value());
    CHECK(*lister.listedAtBoot
          == std::expected<std::vector<OwnedPaneInfo>, OwnedPaneError> { std::unexpect,
                                                                         OwnedPaneError::NotReconciled });
    REQUIRE(next->eventually([&] { return lister.paneHints() == 1; }));
    CHECK(next->host.ownedPanes().list(PanesOwner)->empty());
}

TEST_CASE("a cold restart empties the registry", "[daemon][panes][restore]")
{
    auto const gone = vtcontrol::Uuid { std::array<uint8_t, 16> { 0xAB, 0xCD, 0, 0, 0, 0, 0x40, 0, 0x80 } };
    auto const registry = registryOf(gone);
    auto lister = BootLister {};
    auto config = withRegistry(registry, lister);
    auto workspace = WorkspaceLayoutSnapshot {};
    workspace.tabs.push_back(TabSnapshot { .root = PaneNode { .pane = PaneSnapshot {} } });
    config.restore =
        WorkspaceRestore { .snapshot = WorkspaceSnapshot { .workspaces = { std::move(workspace) } } };
    auto const next = std::make_unique<LiveHost>(catDevice, std::move(config));

    REQUIRE(next->eventually([&] { return lister.paneHints() == 1; }));
    CHECK(next->host.ownedPanes().list(PanesOwner)->empty());
    // The file is written anew, empty.
    next->host.flushOwnedPanes();
    auto const kept = registry->loadAll();
    REQUIRE(kept.size() == 1);
    CHECK(kept.front().first == PanesOwner);
    CHECK(kept.front().second.empty());
}

#ifdef VTHOST_TEST_ORCHESTRATOR_WASM

// ---------------------------------------------------------------------------------------------------------
// The orchestrator's dashboard end to end: its real module serves it on the real DaemonWorldExtensions, its
// commands answered from the recorded fixtures, granted and opened by a window -- across a live upgrade and a
// keeper recovery, each of which closes it, and after which a second "open" draws it again.

namespace
{

using vthost::testing::checkShellsAnswer;
using vthost::testing::openDashboard;
using vthost::testing::OrchestratorName;
using vthost::testing::OrchestratorWorld;
using vthost::testing::reinstantiated;
using vthost::testing::SecondOpen;
using vthost::testing::spawnShells;
using vthost::testing::takeOver;
using vthost::testing::ThenOpen;
using vthost::testing::upgradeWithDashboard;

} // namespace

TEST_CASE("open shows the fixture lists", "[daemon][e2e][served]")
{
    auto world = OrchestratorWorld { VTHOST_TEST_ORCHESTRATOR_WASM, VTHOST_TEST_ORCHESTRATOR_TESTDATA };
    auto const live = std::make_unique<LiveHost>(catDevice, world.hostConfig());
    auto const shells = spawnShells(*live);

    // The window is asked for the grant, grants it, and opens the dashboard in its workspace; its second
    // "open" reveals that dashboard rather than opening another (one per extension and workspace).
    auto second = SecondOpen {};
    auto const opened = openDashboard(*live, world, {}, ThenOpen::Again, &second);
    CHECK(opened.questions == 1);
    CHECK(live->host.servedBy(*opened.session)->extension == OrchestratorName);
    CHECK(second.sessionsBefore == shells.size() + 1);
    CHECK(second.sessionsAfter == second.sessionsBefore);
    CHECK(second.reveals >= 1);

    // A window in another workspace gets a dashboard of its own there.
    auto const elsewhere =
        openDashboard(*live, world, proto::AttachIntent { .kind = proto::AttachIntentKind::NewWorkspace });
    CHECK(elsewhere.questions == 0);
    CHECK(elsewhere.workspace != opened.workspace);
    CHECK(elsewhere.session != opened.session);
    CHECK(live->host.servedBy(*elsewhere.session)->extension == OrchestratorName);
}

TEST_CASE("a live upgrade with the dashboard open", "[daemon][e2e][served][upgrade]")
{
    auto world = OrchestratorWorld { VTHOST_TEST_ORCHESTRATOR_WASM, VTHOST_TEST_ORCHESTRATOR_TESTDATA };
    auto u = ServedUpgrade { world.hostConfig() };
    auto const shells = spawnShells(*u.live);
    auto const before = openDashboard(*u.live, world);
    CHECK(before.questions == 1);

    auto const oldWorld = world.current;
    auto image = upgradeWithDashboard(u, *before.session);
    CHECK(idsOf(image.sessions) == shells);
    u.acceptor.reset();
    u.live.reset();

    // The new image runs the same shells, and a guest instance of its own -- granted already, the grant being
    // on file -- draws the lists again on a second "open".
    auto const next = takeOver(std::move(image), world);
    CHECK(holdsExactly(next->host, shells));
    checkShellsAnswer(*next, shells);
    auto const after = openDashboard(*next, world);
    CHECK(after.questions == 0);
    CHECK(reinstantiated(oldWorld, world));
    checkShellsAnswer(*next, shells);
}

TEST_CASE("a live upgrade with the dashboard open as the last session", "[daemon][e2e][served][upgrade]")
{
    auto world = OrchestratorWorld { VTHOST_TEST_ORCHESTRATOR_WASM, VTHOST_TEST_ORCHESTRATOR_TESTDATA };
    auto u = ServedUpgrade { world.hostConfig() };
    auto shutdowns = 0;
    auto watcher = std::optional<LastSessionWatcher> {};
    watcher.emplace(u.live->host, u.live->loop, [&] { ++shutdowns; });

    // The user closes every shell: the dashboard is the last session, and the daemon serves on.
    auto const shells = spawnShells(*u.live);
    auto const before = openDashboard(*u.live, world);
    for (auto const id: shells)
        u.live->host.closeSession(id);
    REQUIRE(u.live->eventually([&] { return u.live->host.sessionIds() == std::vector { *before.session }; }));

    // The upgrade closes it: no session is left, and none of the daemon ends with it.
    auto const oldWorld = world.current;
    auto image = upgradeWithDashboard(u, *before.session);
    CHECK(image.sessions.empty());
    std::ignore = u.live->loop.runOnce(10ms);
    std::ignore = u.live->loop.runOnce(10ms);
    CHECK(shutdowns == 0);
    watcher.reset();
    u.acceptor.reset();
    u.live.reset();

    auto const next = takeOver(std::move(image), world);
    CHECK(next->host.handoverOutcome().has_value());
    auto const after = openDashboard(*next, world);
    CHECK(after.questions == 0);
    CHECK(reinstantiated(oldWorld, world));
}

TEST_CASE("a keeper recovery with the dashboard open", "[daemon][e2e][served][keeper]")
{
    auto world = OrchestratorWorld { VTHOST_TEST_ORCHESTRATOR_WASM, VTHOST_TEST_ORCHESTRATOR_TESTDATA };
    auto live = std::make_unique<LiveHost>(catDevice, world.hostConfig());
    auto const shells = spawnShells(*live);
    auto const before = openDashboard(*live, world);

    // The daemon dies; its keeper's inventory holds the shells, never the dashboard.
    auto const oldWorld = world.current;
    auto image = recoverFromKeeper(live, shells, *before.session);
    CHECK(idsOf(image.sessions) == shells);

    auto const next = takeOver(std::move(image), world);
    CHECK(holdsExactly(next->host, shells));
    checkShellsAnswer(*next, shells);
    auto const after = openDashboard(*next, world);
    CHECK(after.questions == 0);
    CHECK(reinstantiated(oldWorld, world));
}

#endif
