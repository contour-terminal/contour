// SPDX-License-Identifier: Apache-2.0
#include <vtbackend/state/TerminalStateCodec.hpp>

#include <vtpty/MockPty.hpp>

#include <catch2/catch_test_macros.hpp>

#ifndef _WIN32
    #include <vtpty/Process.hpp>
    #include <vtpty/ProcessPlacement.hpp>
    #include <vtpty/Pty.hpp>
    #include <vtpty/UnixPty.hpp>
    #include <vtpty/test/HandleTesting.hpp>
    #include <vtpty/test/ProcessTesting.hpp>

    #include <csignal>

    #include <fcntl.h>
#endif

#include <core/Environment.hpp>
#include <core/net/PlatformLoop.hpp>

#include <algorithm>
#include <chrono>
#include <functional>
#include <memory>
#include <mutex>
#include <ranges>
#include <string>
#include <thread>
#include <vector>

#include <vtcontrol/testing/SequentialUuidSource.hpp>
#include <vthost/LoopDrain.hpp>
#include <vthost/ObjectRegistry.hpp>
#include <vthost/SessionHost.hpp>
#include <vthost/handover/HandoverCapture.hpp>
#include <vthost/handover/HandoverCodec.hpp>
#include <vthost/testing/HandoverTesting.hpp>
#include <vthost/testing/TestHelpers.hpp>

using namespace vthost;
using namespace vthost::handover;

TEST_CASE("an object registry snapshot restores every binding and label", "[vthost][handover]")
{
    auto source = ObjectRegistry { std::make_shared<vtcontrol::testing::SequentialUuidSource>() };
    auto const workspace = source.mint();
    auto const tab = source.mint();
    auto const pane = source.mint();
    auto const other = source.mint();
    source.bind(vtworkspace::WorkspaceId { 3 }, workspace);
    source.bind(vtworkspace::TabId { 5 }, tab);
    source.bind(vtworkspace::SessionId { 9 }, pane);
    source.bind(vtworkspace::SessionId { 2 }, other);
    auto const paneRef = vtcontrol::ObjectRef { .kind = vtcontrol::ObjectKind::Pane, .uuid = pane };
    auto const tabRef = vtcontrol::ObjectRef { .kind = vtcontrol::ObjectKind::Tab, .uuid = tab };
    source.setLabel(paneRef, "build");
    source.setLabel(tabRef, "work");

    auto const snapshot = source.snapshot();
    REQUIRE(snapshot.panes.size() == 2);
    CHECK(snapshot.panes[0].id == 2); // by id ascending
    CHECK(snapshot.panes[1].id == 9);
    CHECK(snapshot.labels.size() == 2);

    // Restored into a registry that held something else: exactly the snapshot, nothing of what was there.
    auto target = ObjectRegistry { std::make_shared<vtcontrol::testing::SequentialUuidSource>() };
    target.bind(vtworkspace::SessionId { 7 }, target.mint());
    target.restore(snapshot);
    CHECK(target.uuidOf(vtworkspace::WorkspaceId { 3 }) == workspace);
    CHECK(target.uuidOf(vtworkspace::TabId { 5 }) == tab);
    CHECK(target.uuidOf(vtworkspace::SessionId { 9 }) == pane);
    CHECK(target.uuidOf(vtworkspace::SessionId { 2 }) == other);
    CHECK_FALSE(target.uuidOf(vtworkspace::SessionId { 7 }).has_value());
    CHECK(target.labelOf(paneRef) == "build");
    CHECK(target.labelOf(tabRef) == "work");
    CHECK(target.snapshot().panes.size() == snapshot.panes.size());
    CHECK(target.snapshot().labels.size() == snapshot.labels.size());
}

#ifndef _WIN32
namespace
{

using namespace std::chrono_literals;

using vthost::testing::shellProcess;

/// @return A child running CatScript on a PTY of its own.
/// @param size The PTY's size.
/// @param placement Where the child is placed.
[[nodiscard]] std::unique_ptr<vtpty::Pty> catProcess(
    vtpty::PageSize size,
    std::shared_ptr<vtpty::ProcessPlacement> placement = std::make_shared<vtpty::NoPlacement>())
{
    return shellProcess(size, vthost::testing::CatScript, std::move(placement));
}

/// A live host whose capture names one listener and a lease, as the tests check them.
struct CaptureHarness: vthost::testing::LiveHost
{
    using LiveHost::LiveHost;

    /// @return The capture, with the caller's part of it as the tests name it.
    [[nodiscard]] std::expected<HandoverImage, HandoverError> capture()
    {
        return LiveHost::capture({ { .name = "native", .handle = vtpty::CrossingHandle { 17 } } },
                                 vtpty::CrossingHandle { 23 });
    }
};

/// @return The session ids of @p tree's leaves, in tree order.
[[nodiscard]] std::vector<uint64_t> leafSessions(vtworkspace::WorkspaceTreeState const& tree)
{
    auto sessions = std::vector<uint64_t> {};
    auto const visit = [&](this auto const& self, vtworkspace::PaneTreeState const& node) -> void {
        if (node.children.empty())
            sessions.push_back(node.session.value);
        for (auto const& child: node.children)
            self(child);
    };
    for (auto const& workspace: tree.workspaces)
        for (auto const& tab: workspace.tabs)
            visit(tab.root);
    return sessions;
}

} // namespace

TEST_CASE("capture holds every session with its pty descriptors and ids", "[vthost][handover][threading]")
{
    auto h = CaptureHarness { [](vtpty::PageSize size) { return catProcess(size); } };
    h.spawnTwo();
    auto const ids = h.host.sessionIds();
    REQUIRE(h.eventually(
        [&] { return h.pageText(ids[0]).contains("ready") && h.pageText(ids[1]).contains("ready"); }));
    auto const paneUuid = h.host.objects().uuidOf(ids[1]);
    REQUIRE(paneUuid.has_value());
    h.host.objects().setLabel(vtcontrol::ObjectRef { .kind = vtcontrol::ObjectKind::Pane, .uuid = *paneUuid },
                              "logs");

    h.settle();
    auto const image = h.capture();
    REQUIRE(image.has_value());

    CHECK(image->daemonInstance == 0xC0FFEE);
    CHECK(image->fromRelease == "0.7.1");
    CHECK(image->lease == vtpty::CrossingHandle { 23 });
    REQUIRE(image->listeners.size() == 1);
    CHECK(image->listeners[0].name == "native");
    CHECK(image->listeners[0].handle == vtpty::CrossingHandle { 17 });

    // The model as it is, with the ids it mints next.
    auto const live = h.host.model().idState();
    CHECK(image->modelIds.nextWorkspace == live.nextWorkspace);
    CHECK(image->modelIds.nextTab == live.nextTab);
    CHECK(image->modelIds.nextPane == live.nextPane);
    auto treeSessions = leafSessions(image->tree);
    std::ranges::sort(treeSessions);
    CHECK(treeSessions == std::vector<uint64_t> { ids[0].value, ids[1].value });
    CHECK(image->nextSessionId.value > ids[1].value);
    CHECK(image->objects.panes.size() == 2);
    CHECK(image->objects.tabs.size() == 1);
    CHECK(image->objects.workspaces.size() == 1);

    REQUIRE(image->sessions.size() == 2);
    for (auto const& [captured, id]: std::views::zip(image->sessions, ids))
    {
        INFO("session " << id.value);
        CHECK(captured.id == id);
        CHECK(captured.uuid == h.host.objects().uuidOf(id));
        auto const& process = *h.host.hostedSession(id)->process();
        // Read, not released: the host still owns them, and they are open.
        auto const handedOver = process.handover();
        REQUIRE(handedOver.has_value());
        CHECK(captured.pty.masterFd == handedOver->masterFd);
        CHECK(vtpty::testing::isOpen(captured.pty.masterFd));
        CHECK(captured.child.pid == process.handoverChild()->pid);
        CHECK(::kill(static_cast<pid_t>(captured.child.pid), 0) == 0);
        CHECK(captured.child.executable == "/bin/sh");
        CHECK_FALSE(captured.terminalState.empty());
    }
    CHECK(image->sessions[0].label.empty());
    CHECK(image->sessions[1].label == "logs");

    // The image survives its own encoding.
    auto const decoded = decodeHandover(encodeHandover(*image));
    REQUIRE(decoded.has_value());
    CHECK(decoded->sessions.size() == 2);

    rollbackCapture(h.host);
}

TEST_CASE("rollback after capture resumes all sessions with no lost input", "[vthost][handover][threading]")
{
    auto h = CaptureHarness { [](vtpty::PageSize size) { return catProcess(size); } };
    h.spawnTwo();
    auto const ids = h.host.sessionIds();
    REQUIRE(h.eventually(
        [&] { return h.pageText(ids[0]).contains("ready") && h.pageText(ids[1]).contains("ready"); }));

    // Input handed over after the pumps stopped is what the final quiesce takes in.
    h.settle();
    h.host.writeInput(ids[0], "alpha");
    h.host.writeInput(ids[1], "beta");
    auto const image = h.capture();
    REQUIRE(image.has_value());
    REQUIRE(image->sessions.size() == 2);
    CHECK(image->sessions[0].carried
          == std::vector { CarriedDelivery { .kind = DeliveryKind::Pty, .bytes = "alpha" } });
    CHECK(image->sessions[1].carried
          == std::vector { CarriedDelivery { .kind = DeliveryKind::Pty, .bytes = "beta" } });
    CHECK_FALSE(h.pageText(ids[0]).contains("alpha"));

    // Rolled back: what was carried goes out once, and what follows after it.
    rollbackCapture(h.host);
    h.host.writeInput(ids[0], "-gamma");
    h.host.writeInput(ids[1], "-delta");
    REQUIRE(h.eventually([&] { return h.pageText(ids[0]).contains("alpha-gamma"); }));
    REQUIRE(h.eventually([&] { return h.pageText(ids[1]).contains("beta-delta"); }));
    CHECK(h.host.inputBacklog(ids[0]) == 0);
    CHECK(h.host.inputBacklog(ids[1]) == 0);
}

TEST_CASE("a capture refuses a session that runs on no child process, and every session runs on",
          "[vthost][handover][threading]")
{
    // The second session's device is no PTY of this process, as an SSH or channel session's is: nothing could
    // adopt it after the exec.
    auto spawned = 0;
    auto h = CaptureHarness { [&spawned](vtpty::PageSize size) -> std::unique_ptr<vtpty::Pty> {
        if (spawned++ == 0)
            return catProcess(size);
        return std::make_unique<vthost::testing::StallingPty>(size);
    } };
    h.spawnTwo();
    auto const ids = h.host.sessionIds();
    REQUIRE(h.eventually([&] { return h.pageText(ids[0]).contains("ready"); }));
    REQUIRE(h.eventually([&] { return h.host.pumpEventsInFlight() == 0; }));

    auto const refused = h.capture();
    REQUIRE_FALSE(refused.has_value());
    CHECK(refused.error() == HandoverError::UnsupportedSession);

    // Nothing was stopped: no rollback is needed for input to flow.
    h.host.writeInput(ids[0], "still-here");
    REQUIRE(h.eventually([&] { return h.pageText(ids[0]).contains("still-here"); }));
    CHECK(h.host.sessionCount() == 2);
}

TEST_CASE("a capture refused for a session still spawning resumes every session",
          "[vthost][handover][threading]")
{
    // The first session runs; the second's child is parked before its terminal is set up.
    auto placement = std::make_shared<vtpty::testing::HoldingPlacement>();
    auto spawned = 0;
    auto h = CaptureHarness { [&](vtpty::PageSize size) {
        return spawned++ == 0 ? catProcess(size) : catProcess(size, placement);
    } };
    h.spawnTwo();
    auto const ids = h.host.sessionIds();
    REQUIRE(h.eventually([&] { return h.pageText(ids[0]).contains("ready"); }));
    REQUIRE(h.eventually([&] { return h.host.pumpEventsInFlight() == 0; }));

    // The first is quiesced before the second refuses: both run on, the first resumed.
    auto const refused = h.capture();
    REQUIRE_FALSE(refused.has_value());
    CHECK(refused.error() == HandoverError::SessionStarting);
    h.host.writeInput(ids[0], "resumed");
    REQUIRE(h.eventually([&] { return h.pageText(ids[0]).contains("resumed"); }));

    // Retried once the child has started, the capture succeeds.
    placement->releaseAll();
    REQUIRE(h.eventually([&] { return h.pageText(ids[1]).contains("ready"); }));
    h.settle();
    auto const image = h.capture();
    REQUIRE(image.has_value());
    CHECK(image->sessions.size() == 2);
    rollbackCapture(h.host);
}

TEST_CASE("a session that ended is dropped from the capture, and ended first",
          "[vthost][handover][threading]")
{
    auto spawned = 0;
    auto h = CaptureHarness { [&spawned](vtpty::PageSize size) -> std::unique_ptr<vtpty::Pty> {
        if (spawned++ == 0)
            return catProcess(size);
        // Ends once it is sent a byte.
        return std::make_unique<vtpty::Process>(
            vtpty::testing::execInfo("/bin/sh",
                                     { "-c", "stty raw -echo; printf ready; head -c 1 >/dev/null; exit 0" }),
            vtpty::createPty(size, std::nullopt),
            /*escapeSandbox=*/false,
            std::make_shared<vtpty::NoPlacement>());
    } };
    h.spawnTwo();
    auto const ids = h.host.sessionIds();
    REQUIRE(h.eventually(
        [&] { return h.pageText(ids[0]).contains("ready") && h.pageText(ids[1]).contains("ready"); }));

    // Ended, its close posted but not handled: the loop is not turned until the capture.
    h.host.writeInput(ids[1], "x");
    auto const& ended = h.host.hostedSession(ids[1])->terminal().device();
    auto const deadline = std::chrono::steady_clock::now() + 10s;
    while (!ended.isClosed() && std::chrono::steady_clock::now() < deadline)
        std::this_thread::sleep_for(10ms);
    REQUIRE(ended.isClosed());
    REQUIRE(h.host.sessionCount() == 2);

    auto const quiesced = h.host.quiesceAll();
    REQUIRE(quiesced.has_value());
    REQUIRE(quiesced->sessions.size() == 1);
    CHECK(quiesced->sessions[0].id == ids[0]);
    CHECK(leafSessions(quiesced->tree) == std::vector<uint64_t> { ids[0].value });
    CHECK(h.host.sessionCount() == 1);
    CHECK_FALSE(h.host.objects().uuidOf(ids[1]).has_value());

    // The close it posted finds it gone; the capture holds the one left.
    REQUIRE(h.eventually([&] { return h.host.pumpEventsInFlight() == 0; }));
    CHECK(h.host.sessionCount() == 1);
    auto const image = h.capture();
    REQUIRE(image.has_value());
    REQUIRE(image->sessions.size() == 1);
    CHECK(image->sessions[0].id == ids[0]);
    CHECK(image->objects.panes.size() == 1);
    rollbackCapture(h.host);
}

TEST_CASE("a captured terminal state decodes to the screen the session shows",
          "[vthost][handover][threading]")
{
    auto h = CaptureHarness { [](vtpty::PageSize size) { return catProcess(size); } };
    h.spawnTwo();
    auto const ids = h.host.sessionIds();
    REQUIRE(h.eventually(
        [&] { return h.pageText(ids[0]).contains("ready") && h.pageText(ids[1]).contains("ready"); }));
    h.host.writeInput(ids[0], "\r\nfirst pane");
    h.host.writeInput(ids[1], "\r\nsecond pane");
    REQUIRE(h.eventually([&] { return h.pageText(ids[0]).contains("first pane"); }));
    REQUIRE(h.eventually([&] { return h.pageText(ids[1]).contains("second pane"); }));

    h.settle();
    auto const image = h.capture();
    REQUIRE(image.has_value());
    REQUIRE(image->sessions.size() == 2);
    for (auto const& captured: image->sessions)
    {
        INFO("session " << captured.id.value);
        auto const state = vtbackend::decodeTerminalState(captured.terminalState);
        REQUIRE(state.has_value());
        auto settings = vtbackend::settingsFor(
            *state, fromWireSessionSettings(captured.statedSettings, defaultSessionSettings()));
        REQUIRE(settings.has_value());
        // A terminal built as the new image builds it, restored from what crossed.
        auto restored = HostedSession { captured.id,
                                        core::defaultEnvironment(),
                                        std::make_unique<vtpty::MockPty>(captured.pty.pageSize),
                                        *std::move(settings),
                                        vtbackend::HostIdentity {},
                                        vtbackend::TerminalClocks::system(),
                                        core::platform::NativeFileSystem::instance(),
                                        std::function<void()> {},
                                        std::function<void()> {},
                                        std::function<void()> {},
                                        SessionNotifyCallback {},
                                        vthost::SessionClipboardCallback {},
                                        std::function<void()> {},
                                        SessionSignalCallback {} };
        auto& terminal = restored.terminal();
        auto const _ = std::lock_guard { terminal };
        REQUIRE(terminal.restoreState(*state).has_value());
        CHECK(terminal.primaryScreen().renderMainPageText() == h.pageText(captured.id));
    }
    rollbackCapture(h.host);
}
#endif
