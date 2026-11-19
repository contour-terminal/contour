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
#include <vthost/testing/ServedTesting.hpp>

#ifdef VTHOST_TEST_ORCHESTRATOR_WASM
    #include <vthost/testing/OrchestratorWorld.hpp>
#endif

using namespace vthost;
using namespace vthost::handover;
using namespace std::chrono_literals;
using vthost::testing::LiveHost;
using vthost::testing::openServedTab;
using vthost::testing::releaseForAdoption;
using vthost::testing::ServedText;
using vtworkspace::SessionId;

namespace
{

/// Thrown from the fake replacer where the exec succeeds: it ends run() as the exec ends the old image.
struct ImageReplaced: std::exception
{
};

/// @return The echoing shell on a PTY of its own.
[[nodiscard]] std::unique_ptr<vtpty::Pty> catDevice(vtpty::PageSize size)
{
    return vthost::testing::shellProcess(size);
}

/// Records why each session ended.
class EndRecorder final: public SessionStreamEvents
{
  public:
    [[nodiscard]] bool mirrorsScreens() const noexcept override { return false; }
    void sessionEnded(SessionId session, SessionEnd cause) override { ends.emplace_back(session, cause); }

    std::vector<std::pair<SessionId, SessionEnd>> ends;
};

/// A daemon about to be upgraded on POSIX, its exec a fake's.
struct ServedUpgrade: vthost::testing::UpgradeBench
{
    /// @param config How the daemon's host is set up.
    explicit ServedUpgrade(SessionHostConfig config = LiveHost::sequentialIds()):
        UpgradeBench { catDevice, {}, std::move(config) }
    {
    }

    vthost::testing::FakeProcessImageReplacer replacer;
    vthost::testing::FakeHandoverMemory memory;
    ExecHandoff handoff { replacer, memory };
};

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

/// @return Whether @p host holds exactly the sessions @p ids, none of them served.
[[nodiscard]] bool holdsExactly(SessionHost const& host, std::vector<SessionId> const& ids)
{
    return host.sessionIds() == ids
           && std::ranges::none_of(ids, [&](SessionId id) { return host.servedBy(id) != nullptr; });
}

/// Opens a served tab in @p live, as the extension reopening it would, and waits for its screen.
void reopenServed(LiveHost& live)
{
    auto const reopened = openServedTab(live.host);
    REQUIRE(reopened.has_value());
    CHECK(live.host.servedBy(*reopened) != nullptr);
    REQUIRE(live.eventually([&] { return live.pageText(*reopened).contains(ServedText); }));
}

/// @return The ids of @p sessions.
[[nodiscard]] std::vector<SessionId> idsOf(std::vector<HandoverSession> const& sessions)
{
    return std::ranges::to<std::vector<SessionId>>(sessions | std::views::transform(&HandoverSession::id));
}

/// Plays a keeper recovery of @p live's daemon, whose keeper held @p shells while it served @p served itself:
/// the daemon dies, and what the keeper kept becomes the image its successor takes over.
/// @param live The daemon, gone on return.
/// @param shells Its sessions on the keeper, in order.
/// @param served The served session it hosted.
/// @return The image the keeper's inventory recovers.
[[nodiscard]] HandoverImage recoverFromKeeper(std::unique_ptr<LiveHost>& live,
                                              std::vector<SessionId> const& shells,
                                              SessionId served)
{
    // The structure the dead daemon pushed to its keeper: the served session runs on no keeper child.
    auto structure = keeperlink::captureStructure(
        live->host,
        keeperlink::StructureExtras {
            .daemonInstance = 0xC0FFEE, .release = "0.9.0", .upgradeGeneration = 0, .listeners = {} });
    auto const servedEntry = std::ranges::find(structure.sessions, served, &HandoverSession::id);
    REQUIRE(servedEntry != structure.sessions.end());
    CHECK(servedEntry->device == SessionDevice::PtyOnly);

    // The keeper held the shells: the test plays it, naming each by a keeper session of its own and
    // handing its PTY over as the inventory does.
    auto kept = std::vector<keeper::InventorySession> {};
    auto masters = std::vector<int> {};
    for (auto const id: shells)
    {
        auto const entry = std::ranges::find(structure.sessions, id, &HandoverSession::id);
        REQUIRE(entry != structure.sessions.end());
        auto const& hosted = *live->host.hostedSession(id);
        auto const pty = hosted.pty().handover();
        REQUIRE(pty.has_value());
        entry->device = SessionDevice::KeeperChild;
        entry->keeperSession = kept.size() + 1;
        kept.push_back(keeper::InventorySession { .session = entry->keeperSession,
                                                  .pid = hosted.process()->crossing().child->pid,
                                                  .exited = std::nullopt,
                                                  .placed = keeper::Placement::Placed,
                                                  .descriptors = { keeper::KeptDescriptor::Master } });
        masters.push_back(pty->masterFd);
    }
    auto memory = vthost::testing::FakeHandoverMemory {};
    auto const handle = memory.write(keeperlink::encodeStructure(structure));
    REQUIRE(handle.has_value());
    auto inventory = std::vector<keeperlink::ReceivedMessage> {};
    inventory.push_back(
        { .message = keeper::InventoryBegin { .daemonImagePath = {},
                                              .sessionCount = static_cast<uint32_t>(shells.size()),
                                              .listenerCount = 0 },
          .fds = {} });
    inventory.back().fds.emplace_back(static_cast<int>(handle->value));
    for (auto const& [session, master]: std::views::zip(kept, masters))
    {
        inventory.push_back({ .message = session, .fds = {} });
        inventory.back().fds.emplace_back(master);
    }
    // The dead daemon let go of its PTYs and programs without closing either.
    releaseForAdoption(live->host);
    live.reset();

    auto recovery = keeperlink::imageFromInventory(inventory, memory);
    CHECK(recovery.problems.empty());
    return std::move(recovery.image);
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

using core::async::Task;
using vthost::testing::ClientKind;
using vthost::testing::Connection;
using vthost::testing::OrchestratorName;
using vthost::testing::OrchestratorWorld;

/// The longest a window waits, in 1 ms loop ticks, for what it counts on.
constexpr auto WaitTicks = 60'000;

/// The pull requests github/prs-open.json lists, as the dashboard numbers them.
constexpr auto FixturePullRequests = std::array { std::string_view { "#2375" },
                                                  std::string_view { "#2369" },
                                                  std::string_view { "#2346" },
                                                  std::string_view { "#2114" },
                                                  std::string_view { "#1847" } };

/// @return A configuration naming objects with sequential UUIDs, whose daemon world is @p world's.
[[nodiscard]] SessionHostConfig servingFrom(OrchestratorWorld& world)
{
    auto config = LiveHost::sequentialIds();
    config.daemonWorld = world.factory();
    return config;
}

/// @return A host that takes over @p image, its daemon world @p world's.
[[nodiscard]] std::unique_ptr<LiveHost> takeOver(HandoverImage image, OrchestratorWorld& world)
{
    auto config = servingFrom(world);
    config.handover = std::move(image);
    return std::make_unique<LiveHost>(catDevice, std::move(config));
}

/// @return Whether @p page lists every fixture pull request beneath MY PULL REQUESTS, each on a row of the
///         rounded box that holds the heading.
[[nodiscard]] bool showsFixtureLists(std::string_view page)
{
    auto const lines = vthost::testing::pageLines(page);
    auto const top = std::ranges::find_if(
        lines, [](std::string const& line) { return line.starts_with("╭") && line.ends_with("╮"); });
    auto const bottom = std::ranges::find_if(top, lines.end(), [](std::string const& line) {
        return line.starts_with("╰") && line.ends_with("╯");
    });
    auto const heading = std::ranges::find_if(
        top, bottom, [](std::string const& line) { return line.contains("MY PULL REQUESTS"); });
    if (bottom == lines.end() || heading == bottom)
        return false;
    return std::ranges::all_of(FixturePullRequests, [&](std::string_view number) {
        return std::ranges::any_of(heading, bottom, [&](std::string const& line) {
            return line.starts_with("│") && line.ends_with("│") && line.contains(number);
        });
    });
}

/// @return The session of the served tab in @p host's workspace @p workspace, if there is one.
[[nodiscard]] std::optional<SessionId> servedIn(SessionHost const& host, uint64_t workspace)
{
    for (auto const id: host.sessionIds())
    {
        auto const* const holder = std::get<0>(host.model().findSessionLeaf(id));
        if (host.servedBy(id) != nullptr && holder != nullptr && holder->id().value == workspace)
            return id;
    }
    return std::nullopt;
}

/// @return How many times @p log was told to reveal @p session.
[[nodiscard]] std::size_t revealsOf(vthost::testing::ClientLog const& log, SessionId session)
{
    return static_cast<std::size_t>(std::ranges::count_if(log.extensionEvents, [&](auto const& event) {
        auto const* const reveal = std::get_if<proto::RevealPane>(&event);
        return reveal != nullptr && reveal->session == session.value;
    }));
}

/// The orchestrator's dashboard as a window opened it.
struct OpenedDashboard
{
    std::optional<SessionId> session; ///< Its tab's session, once it showed the lists.
    uint64_t workspace = 0;           ///< The window's workspace, which holds it.
    std::size_t questions = 0;        ///< How many grant questions the window was asked before it opened it.
};

/// What a window does once it opened the dashboard.
enum class ThenOpen : uint8_t
{
    Leave = 0, ///< It leaves.
    Again,     ///< It invokes "open" again, waits until it is told to reveal the dashboard, and leaves.
};

/// What a window that opened the dashboard twice saw of the second "open".
struct SecondOpen
{
    std::size_t sessionsBefore = 0; ///< The daemon's sessions, as it invoked it.
    std::size_t sessionsAfter = 0;  ///< The daemon's sessions, once it was told to reveal the dashboard.
    std::size_t reveals = 0;        ///< How many reveals of the dashboard the second "open" brought.
};

/// The window's part: grants the orchestrator if it is asked, waits until its palette offers "open" with the
/// extension running, invokes it, and waits until its workspace's served tab shows the lists -- answering
/// every command the orchestrator runs meanwhile; then does what @p then says.
Task<void> openAndLeave(LiveHost* live,
                        OrchestratorWorld* world,
                        Connection* window,
                        OpenedDashboard* opened,
                        ThenOpen then,
                        SecondOpen* second)
{
    auto const answering = [world] {
        std::ignore = world->processRunner.answerWaiting();
    };
    auto const offered = [&] {
        answering();
        return offersRunning(window->log, OrchestratorName, "open");
    };
    std::ignore = co_await core::net::testing::waitUntil(
        &live->loop, [&] { return offered() || !window->log.requests.empty(); }, WaitTicks);
    if (!window->log.requests.empty())
    {
        auto const& request = window->log.requests.front();
        window->client->answerDaemonExtension(request.name, request.grantDigest, GrantState::Granted);
        std::ignore = co_await core::net::testing::waitUntil(&live->loop, offered, WaitTicks);
    }
    opened->questions = window->log.requests.size();
    opened->workspace = window->client->selectedWorkspace();

    window->client->invokeExtensionAction(std::string(OrchestratorName), "open");
    std::ignore = co_await core::net::testing::waitUntil(
        &live->loop,
        [&] {
            answering();
            auto const session = servedIn(live->host, opened->workspace);
            if (session && showsFixtureLists(live->pageText(*session)))
                opened->session = session;
            return opened->session.has_value();
        },
        WaitTicks);

    if (then == ThenOpen::Again && opened->session)
    {
        auto const revealed = revealsOf(window->log, *opened->session);
        second->sessionsBefore = live->host.sessionCount();
        window->client->invokeExtensionAction(std::string(OrchestratorName), "open");
        std::ignore = co_await core::net::testing::waitUntil(
            &live->loop,
            [&] {
                answering();
                return revealsOf(window->log, *opened->session) > revealed;
            },
            WaitTicks);
        second->reveals = revealsOf(window->log, *opened->session) - revealed;
        second->sessionsAfter = live->host.sessionCount();
    }
    window->client->detach();
}

/// Opens the orchestrator's dashboard from a new window on @p live, as "Open Agents" in its palette does.
/// @param intent Which workspace the window asks to be shown.
/// @param then What the window does once the dashboard shows the lists.
/// @param second Where what it saw of a second "open" is written, for ThenOpen::Again.
/// @return The dashboard, once it shows the fixture lists.
[[nodiscard]] OpenedDashboard openDashboard(LiveHost& live,
                                            OrchestratorWorld& world,
                                            proto::AttachIntent intent = {},
                                            ThenOpen then = ThenOpen::Leave,
                                            SecondOpen* second = nullptr)
{
    auto window = world.connect(live.loop, live.host, ClientKind::Window, intent);
    auto opened = OpenedDashboard {};
    live.loop.blockOn(
        core::net::testing::allOf(window->server->run(),
                                  window->client->run(),
                                  openAndLeave(&live, &world, window.get(), &opened, then, second)));
    REQUIRE(opened.session.has_value());
    CHECK(live.host.servedBy(*opened.session) != nullptr);
    return opened;
}

/// @return Whether @p before, a daemon world @p world made, was replaced by another instance, its own
/// guest's.
[[nodiscard]] bool reinstantiated(std::weak_ptr<vthost::DaemonWorldExtensions> const& before,
                                  OrchestratorWorld const& world)
{
    // Owner order compares the control blocks, which @p before keeps from being reused.
    return before.expired() && !world.current.expired()
           && (before.owner_before(world.current) || world.current.owner_before(before));
}

/// Spawns two shells side by side in one tab of @p live.
/// @return Their sessions, once both are drawn.
[[nodiscard]] std::vector<SessionId> spawnShells(LiveHost& live)
{
    live.spawnTwo();
    auto shells = live.host.sessionIds();
    REQUIRE(live.eventually([&] {
        return std::ranges::all_of(shells, [&](SessionId id) { return live.pageText(id).contains("ready"); });
    }));
    return shells;
}

/// Checks that @p live's @p shells take input and draw it.
void checkShellsAnswer(LiveHost& live, std::vector<SessionId> const& shells)
{
    for (auto const id: shells)
        live.host.writeInput(id, "after-the-dashboard");
    REQUIRE(live.eventually([&] {
        return std::ranges::all_of(
            shells, [&](SessionId id) { return live.pageText(id).contains("after-the-dashboard"); });
    }));
}

/// Upgrades @p u's daemon, its dashboard @p dashboard open, up to the exec; then the old image goes, its
/// guest with it, as the exec ends them.
/// @return The image the new daemon takes over.
[[nodiscard]] HandoverImage upgradeWithDashboard(ServedUpgrade& u, SessionId dashboard)
{
    auto recorder = EndRecorder {};
    auto subscription = std::optional<ScopedStreamSubscription> {};
    subscription.emplace(
        u.live->host, recorder, &SessionHost::subscribeStream, &SessionHost::unsubscribeStream);
    auto image = std::optional<HandoverImage> {};
    u.replacer.onReplace = [&] {
        auto const& [program, argv] = u.replacer.replacements.back();
        auto const fd = handoverTokenOf(argv.back()).and_then(parseHandoverDescriptor);
        REQUIRE(fd.has_value());
        auto decoded = readHandover(u.memory, *fd);
        REQUIRE(decoded.has_value());
        image = *std::move(decoded);
        releaseForAdoption(u.live->host);
        throw ImageReplaced {};
    };
    auto upgrade = u.orchestrator(u.handoff);
    REQUIRE_THROWS_AS(std::ignore = u.run(upgrade, UpgradeTarget { .imageId = u.imageId }), ImageReplaced);

    // The upgrade closed the dashboard, with a close of its own kind, and captured everything else.
    CHECK(std::ranges::contains(recorder.ends, std::pair { dashboard, SessionEnd::Upgrade }));
    REQUIRE(image.has_value());
    CHECK_FALSE(std::ranges::contains(image->sessions, dashboard, &HandoverSession::id));
    u.replacer.onReplace = {};
    return *std::move(image);
}

} // namespace

TEST_CASE("open shows the fixture lists", "[daemon][e2e][served]")
{
    auto world = OrchestratorWorld { VTHOST_TEST_ORCHESTRATOR_WASM, VTHOST_TEST_ORCHESTRATOR_TESTDATA };
    auto const live = std::make_unique<LiveHost>(catDevice, servingFrom(world));
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
    auto u = ServedUpgrade { servingFrom(world) };
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
    auto u = ServedUpgrade { servingFrom(world) };
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
    auto live = std::make_unique<LiveHost>(catDevice, servingFrom(world));
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
