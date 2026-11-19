// SPDX-License-Identifier: Apache-2.0
#include <vtpty/ChannelPty.hpp>
#include <vtpty/MockPty.hpp>

#include <crispy/BufferObject.hpp>

#include <core/Environment.hpp>
#include <core/net/EventLoop.hpp>
#include <core/net/testing/CoroTestSupport.hpp>
#include <core/net/testing/ScriptedBackend.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <format>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <ranges>
#include <string>
#include <string_view>
#include <thread>
#include <tuple>
#include <utility>
#include <vector>

#include <vtcontrol/Uuid.hpp>
#include <vtcontrol/testing/SequentialUuidSource.hpp>
#include <vthost/ExtensionHosting.hpp>
#include <vthost/LastSessionWatcher.hpp>
#include <vthost/ServedSessions.hpp>
#include <vthost/SessionHost.hpp>
#include <vthost/testing/FakeDaemonWorldHosting.hpp>
#include <vthost/testing/FakeProfileCatalog.hpp>
#include <vthost/testing/HandoverTesting.hpp>
#include <vthost/testing/ServedTesting.hpp>

using namespace std::chrono_literals;
using namespace vthost;
using vthost::testing::expectedStoppedBox;
using vthost::testing::FakeWindow;
using vthost::testing::LiveHost;
using vthost::testing::openServedTab;
using vthost::testing::pageLines;
using vthost::testing::ServedUuids;
using vthost::testing::showsBox;
using vtworkspace::SessionId;

namespace
{

constexpr auto Size = vtbackend::PageSize { vtbackend::LineCount(24), vtbackend::ColumnCount(80) };

/// A host over MockPty sessions with no pump threads, that ends with its last session, as an auto-spawned
/// daemon does; a counter stands in for the shutdown.
struct WatchedHost
{
    core::net::testing::ScriptedBackend source;
    core::net::EventLoop loop { source };
    SessionHost host { loop,
                       [](vtbackend::PageSize size, std::optional<vtpty::Process::ExecInfo> const&) {
                           return std::make_unique<vtpty::MockPty>(size);
                       },
                       vtbackend::Settings {},
                       core::defaultEnvironment(),
                       SessionHostConfig { .pumping = SessionPumping::Manual } };
    int shutdowns = 0;
    LastSessionWatcher watcher { host, loop, [this] { ++shutdowns; } };

    /// Turns the loop once.
    void turn() { loop.blockOn(core::net::testing::sleepFor(&loop, 0ms)); }
};

} // namespace

TEST_CASE("a served device takes output and buffers input", "[vthost][served]")
{
    auto h = WatchedHost {};
    auto& served = h.host.served();
    auto const device = served.makeDevice(Size);
    REQUIRE(device != nullptr);

    // What the extension draws reaches the terminal reading the device.
    device->feed("\x1b[Hdrawn");
    auto pool = crispy::BufferObjectPool<char> { 4096 };
    auto storage = pool.allocateBufferObject();
    auto const read = device->read(*storage, 1s, 4096);
    REQUIRE(read.has_value());
    CHECK(read->data == "\x1b[Hdrawn");

    // What the terminal writes is the extension's to read.
    CHECK(device->writeInput("q", vtpty::InputKind::Keyboard) == 1);
    CHECK(served.inputOf(*device) == "q");

    // A device it did not make has nothing buffered here.
    auto const stranger = vtpty::ChannelPty { Size };
    CHECK(served.inputOf(stranger).empty());
}

TEST_CASE("a chunk that does not fit whole is dropped whole", "[vthost][served]")
{
    auto h = WatchedHost {};
    auto& served = h.host.served();
    auto const device = served.makeDevice(Size);
    auto const filler = std::string(vthost::MaxServedTransferBytes - 2, 'a');
    REQUIRE(device->writeInput(filler, vtpty::InputKind::Keyboard) == static_cast<int>(filler.size()));
    REQUIRE(served.inputOf(*device) == filler);

    // Three bytes into two of room: not cut, so the reader never sees half a key.
    std::ignore = device->writeInput("\x1b[A", vtpty::InputKind::Keyboard);
    CHECK(served.inputOf(*device) == filler);

    // What fits is kept again.
    std::ignore = device->writeInput("x", vtpty::InputKind::Keyboard);
    CHECK(served.inputOf(*device) == filler + "x");
}

TEST_CASE("a chunk bigger than the bound is dropped, however empty the buffer", "[vthost][served]")
{
    auto h = WatchedHost {};
    auto& served = h.host.served();
    auto const device = served.makeDevice(Size);
    std::ignore =
        device->writeInput(std::string(vthost::MaxServedTransferBytes + 1, 'p'), vtpty::InputKind::Keyboard);
    CHECK(served.inputOf(*device).empty());
    std::ignore = device->writeInput("q", vtpty::InputKind::Keyboard);
    CHECK(served.inputOf(*device) == "q");
}

TEST_CASE("(c) served tab as the last session", "[daemon][served][lifecycle]")
{
    SECTION("an upgrade's close of it keeps the daemon")
    {
        auto h = WatchedHost {};
        REQUIRE(openServedTab(h.host).has_value());
        h.host.closeServedSessions();
        h.turn();
        h.turn();
        CHECK(h.host.sessionCount() == 0);
        CHECK(h.shutdowns == 0);
    }
    SECTION("the user's close of it ends the daemon")
    {
        auto h = WatchedHost {};
        auto const served = openServedTab(h.host);
        REQUIRE(served.has_value());
        h.host.closeSession(*served);
        h.turn();
        h.turn();
        CHECK(h.shutdowns == 1);
    }
}

TEST_CASE("the upgrade latch refuses served spawns until the rollback", "[daemon][served][upgrade]")
{
    auto h = WatchedHost {};
    h.host.closeServedSessions();
    CHECK(h.host.servedAdmission() == ServedAdmission::Closed);
    CHECK_FALSE(openServedTab(h.host).has_value());
    CHECK(h.host.sessionCount() == 0);

    h.host.admitServedSessions();
    CHECK(h.host.servedAdmission() == ServedAdmission::Open);
    auto const served = openServedTab(h.host);
    REQUIRE(served.has_value());
    CHECK(h.host.servedBy(*served) != nullptr);
    CHECK(*h.host.servedBy(*served)
          == ServedBy { .extension = "stub", .scheme = "stub", .moduleDigest = "" });
}

// ---------------------------------------------------------------------------------------------------------
// Served tabs per workspace, as daemon-scoped extensions reach them (IServedTabs).

namespace
{

/// The extension the tests serve tabs for, and its scheme.
constexpr auto Orchestrator = std::string_view { "orchestrator" };
constexpr auto AgentsScheme = std::string_view { "agents" };

/// A host whose shell sessions pump on devices that never end by themselves, with a fake daemon world that
/// journals the hints it is raised, and windows that attach to it.
struct ServedBench
{
    ServedBench():
        live { [](vtpty::PageSize size) -> std::unique_ptr<vtpty::Pty> {
                  // A shell that never ends by itself: its pane lives as long as the test.
                  return std::make_unique<vtpty::ChannelPty>(size);
              },
               SessionHostConfig {
                   .daemonWorld =
                       [this](DaemonWorldEvents events, OwnerPost post, DaemonWorldWiring wiring) {
                           tabs = &wiring.tabs;
                           world = std::make_shared<vthost::testing::FakeDaemonWorldHosting>(
                               journal, std::move(post), std::move(events));
                           world->rows = { ExtensionActionRows { .extension = std::string(Orchestrator),
                                                                 .title = "Agents",
                                                                 .state = DaemonExtensionState::Running,
                                                                 .rows = { { "open", "Open Agents" } } },
                                           ExtensionActionRows { .extension = "other",
                                                                 .title = "Other",
                                                                 .state = DaemonExtensionState::Running,
                                                                 .rows = { { "open", "Open Other" } } } };
                           return world;
                       },
                   .uuids = std::make_shared<vtcontrol::testing::SequentialUuidSource>(),
                   .servedIds = std::make_shared<ServedUuids>() } }
    {
        auto* const tab = live.host.createTab();
        REQUIRE(tab != nullptr);
        shell = tab->rootPane()->session();
        first = live.host.workspaceId();
    }

    /// Attaches @p window, selecting @p workspace, and mints its origin as invoking an action does.
    /// @return Its origin.
    [[nodiscard]] Origin attach(FakeWindow& window,
                                vtworkspace::WorkspaceId workspace,
                                std::string_view extension = Orchestrator)
    {
        window.selected = workspace.value;
        live.host.subscribeStream(&window);
        live.host.noteClientAttached(&window);
        REQUIRE(live.host.invokeExtensionAction(window.connectionId(), extension, "open")
                == InvocationEffect::Deliver);
        auto const taken = live.host.extensionActions().take(extension);
        REQUIRE(taken.size() == 1);
        return taken.front().second;
    }

    /// Lets @p window go, as its connection ending does.
    void detach(FakeWindow& window)
    {
        live.host.noteClientDetached(&window, DetachKind::Closed);
        live.host.forgetActionOrigin(window.connectionId());
        live.host.unsubscribeStream(&window);
    }

    /// Selects @p workspace in @p window, as its SelectWorkspace does on the loop.
    void select(FakeWindow& window, vtworkspace::WorkspaceId workspace)
    {
        window.selected = workspace.value;
        live.host.noteSelected(window);
    }

    /// @return A new workspace, with a shell tab in it.
    [[nodiscard]] vtworkspace::WorkspaceId newWorkspace()
    {
        auto* const workspace = live.host.createWorkspace();
        REQUIRE(workspace != nullptr);
        return workspace->id();
    }

    /// @return The served sessions, in id order.
    [[nodiscard]] std::vector<SessionId> served() const
    {
        return std::ranges::to<std::vector>(live.host.sessionIds() | std::views::filter([this](SessionId id) {
                                                return live.host.servedBy(id) != nullptr;
                                            }));
    }

    /// @return The session of the served tab @p id; turns the loop until it exists.
    [[nodiscard]] SessionId sessionOf(std::string const& id)
    {
        auto found = std::optional<SessionId> {};
        REQUIRE(live.eventually([&] {
            for (auto const session: served())
                if (auto const uuid = live.host.objects().uuidOf(session);
                    uuid && vtcontrol::toString(*uuid) == id)
                    found = session;
            return found.has_value();
        }));
        return *found;
    }

    /// @return The workspace @p session's tab is in.
    [[nodiscard]] vtworkspace::WorkspaceId workspaceOf(SessionId session) const
    {
        auto const [workspace, tab, leaf] = live.host.model().findSessionLeaf(session);
        REQUIRE(workspace != nullptr);
        return workspace->id();
    }

    /// @return The session of @p workspace's active tab.
    [[nodiscard]] SessionId activeIn(vtworkspace::WorkspaceId workspace) const
    {
        auto const* const found = live.host.model().workspace(workspace);
        REQUIRE(found != nullptr);
        REQUIRE(found->activeTab() != nullptr);
        return found->activeTab()->rootPane()->session();
    }

    /// @return How many times @p extension was raised ServedHint::Tabs.
    [[nodiscard]] std::size_t tabHints(std::string_view extension = Orchestrator) const
    {
        return static_cast<std::size_t>(std::ranges::count(*journal, std::format("hint {} tabs", extension)));
    }

    /// Turns the loop once.
    void turn() { std::ignore = live.loop.runOnce(0ms); }

    std::shared_ptr<std::vector<std::string>> journal = std::make_shared<std::vector<std::string>>();
    std::shared_ptr<vthost::testing::FakeDaemonWorldHosting> world;
    IServedTabs* tabs = nullptr;
    LiveHost live;
    SessionId shell {};
    vtworkspace::WorkspaceId first {};
};

/// @return Whether @p result failed with @p error.
template <typename T>
[[nodiscard]] bool failedWith(std::expected<T, ServedTabError> const& result, ServedTabError error)
{
    return !result && result.error() == error;
}

} // namespace

TEST_CASE("bytes the guest writes reach the terminal", "[daemon][served]")
{
    auto bench = ServedBench {};
    auto window = FakeWindow { 1 };
    auto const origin = bench.attach(window, bench.first);
    auto const opened = bench.tabs->open(Orchestrator, "Agents", "3f9a", AgentsScheme, origin);
    REQUIRE(opened.has_value());
    auto const session = bench.sessionOf(*opened);
    REQUIRE(bench.tabs->write(Orchestrator, *opened, "\x1b[2J\x1b[Hdashboard").has_value());
    CHECK(bench.live.eventually([&] { return bench.live.pageText(session).contains("dashboard"); }));
    // Named after the extension's title, and marked as served by it.
    CHECK(*bench.live.host.servedBy(session)
          == ServedBy {
              .extension = "orchestrator", .scheme = "agents", .moduleDigest = "3f9a", .title = "Agents" });
}

TEST_CASE("client input reaches tab-read in order", "[daemon][served]")
{
    auto bench = ServedBench {};
    auto window = FakeWindow { 1 };
    auto const origin = bench.attach(window, bench.first);
    auto const opened = bench.tabs->open(Orchestrator, "Agents", "", AgentsScheme, origin);
    REQUIRE(opened.has_value());
    auto const session = bench.sessionOf(*opened);
    auto const hintsBefore = bench.tabHints();

    bench.live.host.writeInputFrom(session, "j", window.connectionId(), proto::InputKind::Keyboard);
    bench.live.host.writeInputFrom(session, "\x1b[B", window.connectionId(), proto::InputKind::Keyboard);
    auto read = std::string {};
    REQUIRE(bench.live.eventually([&] {
        auto const chunk = bench.tabs->read(Orchestrator, *opened);
        REQUIRE(chunk.has_value());
        read += *chunk;
        return read.size() >= 4;
    }));
    CHECK(read == "j\x1b[B");
    // The first chunk into an empty buffer told the extension.
    CHECK(bench.live.eventually([&] { return bench.tabHints() > hintsBefore; }));
}

TEST_CASE("input beyond the bound is dropped whole, the rest in order", "[daemon][served]")
{
    auto bench = ServedBench {};
    auto window = FakeWindow { 1 };
    auto const origin = bench.attach(window, bench.first);
    auto const opened = bench.tabs->open(Orchestrator, "Agents", "", AgentsScheme, origin);
    REQUIRE(opened.has_value());
    auto const session = bench.sessionOf(*opened);

    // 64 chunks of 1020 bytes fit (65 280 of 65 536); one of 600 more does not, and is dropped whole; one
    // byte still fits. Typed at once: each input of a served session reaches its device as a write of its
    // own.
    auto expected = std::string {};
    for (auto const index: std::views::iota(0, 64))
    {
        auto const chunk = std::string(1020, static_cast<char>('a' + (index % 26)));
        bench.live.host.writeInput(session, chunk);
        expected += chunk;
    }
    bench.live.host.writeInput(session, std::string(600, '!'));
    bench.live.host.writeInput(session, "z");
    expected += "z";
    REQUIRE(bench.live.eventually([&] { return bench.live.host.inputBacklog(session) == 0; }));
    auto read = std::string {};
    auto const complete = bench.live.eventually([&] {
        read += bench.tabs->read(Orchestrator, *opened).value_or(std::string {});
        return read.size() >= expected.size();
    });
    INFO(std::format(
        "read {} bytes, ending {}", read.size(), read.substr(read.size() > 8 ? read.size() - 8 : 0)));
    REQUIRE(complete);
    CHECK(read.size() == expected.size());
    CHECK(read == expected);

    // Read, it takes whole chunks again.
    bench.live.host.writeInput(session, "\x1b[A");
    auto again = std::string {};
    REQUIRE(bench.live.eventually([&] {
        again += bench.tabs->read(Orchestrator, *opened).value_or(std::string {});
        return again.size() >= 3;
    }));
    CHECK(again == "\x1b[A");
}

TEST_CASE("a burst beyond the bound keeps whole inputs up to it and drops the rest whole", "[daemon][served]")
{
    auto bench = ServedBench {};
    auto window = FakeWindow { 1 };
    auto const origin = bench.attach(window, bench.first);
    auto const opened = bench.tabs->open(Orchestrator, "Agents", "", AgentsScheme, origin);
    REQUIRE(opened.has_value());
    auto const session = bench.sessionOf(*opened);

    // 100 KiB in 100 inputs of a client, none waiting for the one before to be delivered.
    auto kept = std::string {};
    for (auto const index: std::views::iota(0, 100))
    {
        auto const input = std::string(1024, static_cast<char>('A' + (index % 26)));
        bench.live.host.writeInputFrom(session, input, window.connectionId(), proto::InputKind::Keyboard);
        if (index < 64)
            kept += input;
    }
    REQUIRE(bench.live.eventually([&] { return bench.live.host.inputBacklog(session) == 0; }));
    auto read = std::string {};
    REQUIRE(bench.live.eventually([&] {
        read += bench.tabs->read(Orchestrator, *opened).value_or(std::string {});
        return read.size() >= kept.size();
    }));
    // Exactly the first 64 inputs, whole; none of the other 36, not even in part.
    CHECK(read.size() == kept.size());
    CHECK(read == kept);
    bench.turn();
    CHECK(bench.tabs->read(Orchestrator, *opened) == std::expected<std::string, ServedTabError> { "" });
}

TEST_CASE("a resize appears in tabs()", "[daemon][served]")
{
    auto bench = ServedBench {};
    auto window = FakeWindow { 1 };
    auto const origin = bench.attach(window, bench.first);
    auto const opened = bench.tabs->open(Orchestrator, "Agents", "", AgentsScheme, origin);
    REQUIRE(opened.has_value());
    std::ignore = bench.sessionOf(*opened);
    auto const hintsBefore = bench.tabHints();

    auto const size = vtpty::PageSize { vtbackend::LineCount(30), vtbackend::ColumnCount(100) };
    std::ignore = bench.live.host.applyClientSize(&window, size, std::nullopt);
    CHECK(bench.live.eventually([&] {
        auto const listed = bench.tabs->list(Orchestrator);
        return listed.size() == 1 && listed.front().cols == 100 && listed.front().rows == 30;
    }));
    CHECK(bench.live.eventually([&] { return bench.tabHints() > hintsBefore; }));
}

TEST_CASE("write before the tab is created reaches the terminal", "[daemon][served]")
{
    auto bench = ServedBench {};
    auto window = FakeWindow { 1 };
    auto const origin = bench.attach(window, bench.first);
    auto const opened = bench.tabs->open(Orchestrator, "Agents", "", AgentsScheme, origin);
    REQUIRE(opened.has_value());
    // No loop turn yet: the tab is pending, and what is drawn waits in its device.
    REQUIRE(bench.tabs->write(Orchestrator, *opened, "\x1b[Hearly").has_value());
    CHECK(bench.tabs->read(Orchestrator, *opened) == std::expected<std::string, ServedTabError> { "" });
    auto const session = bench.sessionOf(*opened);
    CHECK(bench.live.eventually([&] { return bench.live.pageText(session).contains("early"); }));
}

TEST_CASE("close before the tab is created creates nothing", "[daemon][served]")
{
    auto bench = ServedBench {};
    auto window = FakeWindow { 1 };
    auto const origin = bench.attach(window, bench.first);
    auto const sessionsBefore = bench.live.host.sessionCount();
    auto const opened = bench.tabs->open(Orchestrator, "Agents", "", AgentsScheme, origin);
    REQUIRE(opened.has_value());
    REQUIRE(bench.tabs->close(Orchestrator, *opened).has_value());
    CHECK(bench.tabs->list(Orchestrator).empty());
    bench.turn();
    bench.turn();
    CHECK(bench.live.host.sessionCount() == sessionsBefore);
    CHECK(bench.served().empty());
    CHECK(bench.tabs->list(Orchestrator).empty());
    // Gone from the mirror: nothing more reaches it.
    CHECK(failedWith(bench.tabs->write(Orchestrator, *opened, "x"), ServedTabError::NotOwned));
}

TEST_CASE("two opens in one batch make one tab", "[daemon][served]")
{
    auto bench = ServedBench {};
    auto window = FakeWindow { 1 };
    auto const origin = bench.attach(window, bench.first);
    auto const first = bench.tabs->open(Orchestrator, "Agents", "", AgentsScheme, origin);
    auto const second = bench.tabs->open(Orchestrator, "Agents", "", AgentsScheme, origin);
    REQUIRE(first.has_value());
    CHECK(second == first);
    std::ignore = bench.sessionOf(*first);
    bench.turn();
    CHECK(bench.served().size() == 1);
    CHECK(bench.tabs->list(Orchestrator).size() == 1);

    // Opened once more, the open tab is the answer, revealed where the window is.
    CHECK(bench.tabs->open(Orchestrator, "Agents", "", AgentsScheme, origin) == first);
    CHECK(bench.live.eventually([&] { return !window.reveals.empty(); }));
    CHECK(bench.served().size() == 1);
}

TEST_CASE("open from a second workspace opens a second dashboard", "[daemon][served]")
{
    auto bench = ServedBench {};
    auto const second = bench.newWorkspace();
    auto one = FakeWindow { 1 };
    auto two = FakeWindow { 2 };
    auto const originOne = bench.attach(one, bench.first);
    auto const originTwo = bench.attach(two, second);
    auto const a = bench.tabs->open(Orchestrator, "Agents", "", AgentsScheme, originOne);
    auto const b = bench.tabs->open(Orchestrator, "Agents", "", AgentsScheme, originTwo);
    REQUIRE(a.has_value());
    REQUIRE(b.has_value());
    CHECK(*a != *b);
    CHECK(bench.workspaceOf(bench.sessionOf(*a)) == bench.first);
    CHECK(bench.workspaceOf(bench.sessionOf(*b)) == second);
    // Each activated in its workspace, as a new tab is.
    CHECK(bench.activeIn(bench.first) == bench.sessionOf(*a));
    CHECK(bench.activeIn(second) == bench.sessionOf(*b));
    CHECK(bench.tabs->list(Orchestrator).size() == 2);
}

TEST_CASE("reveal into another workspace asks first", "[daemon][served]")
{
    auto bench = ServedBench {};
    auto const second = bench.newWorkspace();
    auto showingFirst = FakeWindow { 1 };
    auto asking = FakeWindow { 2 };
    auto const originFirst = bench.attach(showingFirst, bench.first);
    auto const originAsking = bench.attach(asking, second);
    auto const opened = bench.tabs->open(Orchestrator, "Agents", "", AgentsScheme, originFirst);
    REQUIRE(opened.has_value());
    auto const session = bench.sessionOf(*opened);
    // Another tab is active in the first workspace now.
    REQUIRE(bench.live.host.createTabIn(bench.first) != nullptr);
    auto const otherActive = bench.activeIn(bench.first);
    REQUIRE(otherActive != session);
    showingFirst.reveals.clear();

    REQUIRE(bench.tabs->reveal(Orchestrator, *opened, originAsking).has_value());
    REQUIRE(bench.live.eventually([&] { return asking.switches.size() == 1; }));
    auto const& request = asking.switches.front();
    // The workspace being left is the asking window's own; the target names the tab's workspace.
    CHECK(request.workspace == second.value);
    CHECK(request.target.kind == proto::SwitchDaemonKind::Same);
    CHECK(request.target.workspace == vtcontrol::toString(*bench.live.host.objects().uuidOf(bench.first)));
    CHECK(bench.activeIn(bench.first) == otherActive);

    auto const answer = [&](proto::SwitchOutcome outcome) {
        bench.live.host.completeSwitch(asking.connectionId(),
                                       proto::SwitchReply { .request = request.request, .outcome = outcome });
        bench.turn();
    };
    SECTION("refused: nothing is activated, and no window is told")
    {
        answer(proto::SwitchOutcome::Refused);
        CHECK(bench.activeIn(bench.first) == otherActive);
        CHECK(asking.reveals.empty());
        CHECK(showingFirst.reveals.empty());
    }
    SECTION("the picker shown: nothing more")
    {
        answer(proto::SwitchOutcome::PickerShown);
        CHECK(bench.activeIn(bench.first) == otherActive);
        CHECK(asking.reveals.empty());
        CHECK(showingFirst.reveals.empty());
    }
    SECTION("switched: activated, and the asking window alone is told")
    {
        answer(proto::SwitchOutcome::Switched);
        CHECK(bench.activeIn(bench.first) == session);
        CHECK(asking.reveals == std::vector { session });
        CHECK(showingFirst.reveals.empty());
    }
    SECTION("focused elsewhere: activated, and the window showing it is told")
    {
        answer(proto::SwitchOutcome::FocusedExisting);
        CHECK(bench.activeIn(bench.first) == session);
        CHECK(showingFirst.reveals == std::vector { session });
        CHECK(asking.reveals.empty());
    }
}

TEST_CASE("a reveal in the window's own workspace activates and tells it", "[daemon][served]")
{
    auto bench = ServedBench {};
    auto window = FakeWindow { 1 };
    auto const origin = bench.attach(window, bench.first);
    auto const opened = bench.tabs->open(Orchestrator, "Agents", "", AgentsScheme, origin);
    REQUIRE(opened.has_value());
    // Pending, a reveal answers ok and does nothing more: the creation activates the tab.
    CHECK(bench.tabs->reveal(Orchestrator, *opened, origin).has_value());
    auto const session = bench.sessionOf(*opened);
    REQUIRE(bench.live.host.createTabIn(bench.first) != nullptr);
    bench.turn();
    CHECK(window.reveals.empty());

    REQUIRE(bench.tabs->reveal(Orchestrator, *opened, origin).has_value());
    CHECK(bench.live.eventually([&] { return window.reveals == std::vector { session }; }));
    CHECK(bench.activeIn(bench.first) == session);
    CHECK(window.switches.empty());
}

TEST_CASE("tab-open returns at once and the tab follows on the loop", "[daemon][served]")
{
    auto bench = ServedBench {};
    auto window = FakeWindow { 1 };
    auto const origin = bench.attach(window, bench.first);
    auto const sessionsBefore = bench.live.host.sessionCount();
    auto const opened = bench.tabs->open(Orchestrator, "Agents", "", AgentsScheme, origin);
    REQUIRE(opened.has_value());
    // Listed at once, pending, at the size the workspace has; nothing created yet.
    auto const pending = bench.tabs->list(Orchestrator);
    REQUIRE(pending.size() == 1);
    auto const area = bench.live.host.pageSize();
    CHECK(pending.front()
          == ServedTabInfo { .tab = *opened,
                             .scheme = "agents",
                             .cols = unbox<uint32_t>(area.columns),
                             .rows = unbox<uint32_t>(area.lines),
                             .state = ServedTabState::Pending });

    CHECK(bench.live.host.sessionCount() == sessionsBefore);
    auto const hintsBefore = bench.tabHints();

    std::ignore = bench.sessionOf(*opened);
    CHECK(bench.live.host.sessionCount() == sessionsBefore + 1);
    CHECK(bench.tabs->list(Orchestrator).front().state == ServedTabState::Open);
    CHECK(bench.live.eventually([&] { return bench.tabHints() > hintsBefore; }));
}

TEST_CASE("an origin that left fails the open as gone", "[daemon][served]")
{
    auto bench = ServedBench {};
    auto window = FakeWindow { 1 };
    auto const origin = bench.attach(window, bench.first);
    bench.detach(window);
    CHECK(
        failedWith(bench.tabs->open(Orchestrator, "Agents", "", AgentsScheme, origin), ServedTabError::Gone));
    // An origin never handed out is gone as well.
    CHECK(failedWith(bench.tabs->open(Orchestrator, "Agents", "", AgentsScheme, 4711), ServedTabError::Gone));
    bench.turn();
    CHECK(bench.served().empty());
}

TEST_CASE("an open uses the workspace the mirror holds", "[daemon][served]")
{
    auto bench = ServedBench {};
    auto const second = bench.newWorkspace();
    auto const third = bench.newWorkspace();
    auto window = FakeWindow { 1 };
    auto const origin = bench.attach(window, bench.first);
    bench.select(window, second);
    auto const opened = bench.tabs->open(Orchestrator, "Agents", "", AgentsScheme, origin);
    REQUIRE(opened.has_value());
    CHECK(bench.workspaceOf(bench.sessionOf(*opened)) == second);

    // The window selects the third workspace; before the loop has taken it in, the guest opens again for it.
    window.selected = third.value;
    auto const raced = bench.tabs->open(Orchestrator, "Agents", "", AgentsScheme, origin);
    CHECK(raced == opened);
    bench.turn();
    CHECK(bench.workspaceOf(bench.sessionOf(*opened)) == second);
    CHECK(bench.served().size() == 1);

    // Once it has, a close and a new open land in the third.
    bench.select(window, third);
    REQUIRE(bench.tabs->close(Orchestrator, *opened).has_value());
    REQUIRE(bench.live.eventually([&] { return bench.served().empty(); }));
    auto const reopened = bench.tabs->open(Orchestrator, "Agents", "", AgentsScheme, origin);
    REQUIRE(reopened.has_value());
    CHECK(*reopened != *opened);
    CHECK(bench.workspaceOf(bench.sessionOf(*reopened)) == third);
}

TEST_CASE("an open during an upgrade is refused as limit", "[daemon][served]")
{
    auto bench = ServedBench {};
    auto window = FakeWindow { 1 };
    auto const origin = bench.attach(window, bench.first);
    bench.live.host.closeServedSessions();
    CHECK(failedWith(bench.tabs->open(Orchestrator, "Agents", "", AgentsScheme, origin),
                     ServedTabError::Limit));
    bench.turn();
    CHECK(bench.served().empty());

    bench.live.host.admitServedSessions();
    auto const opened = bench.tabs->open(Orchestrator, "Agents", "", AgentsScheme, origin);
    REQUIRE(opened.has_value());
    std::ignore = bench.sessionOf(*opened);
}

TEST_CASE("a pending tab whose creation meets the upgrade latch is dropped", "[daemon][served]")
{
    auto bench = ServedBench {};
    auto window = FakeWindow { 1 };
    auto const origin = bench.attach(window, bench.first);
    auto const opened = bench.tabs->open(Orchestrator, "Agents", "", AgentsScheme, origin);
    REQUIRE(opened.has_value());
    bench.live.host.closeServedSessions();
    auto const hintsBefore = bench.tabHints();
    bench.turn();
    bench.turn();
    CHECK(bench.served().empty());
    CHECK(bench.tabs->list(Orchestrator).empty());
    CHECK(bench.live.eventually([&] { return bench.tabHints() > hintsBefore; }));
    bench.live.host.admitServedSessions();
}

TEST_CASE("a shell pane is not a tab", "[daemon][served]")
{
    auto bench = ServedBench {};
    auto window = FakeWindow { 1 };
    auto const origin = bench.attach(window, bench.first);
    auto const shellUuid = bench.live.host.objects().uuidOf(bench.shell);
    REQUIRE(shellUuid.has_value());
    auto const shellId = vtcontrol::toString(*shellUuid);
    auto const sessionsBefore = bench.live.host.sessionCount();
    auto const pageBefore = bench.live.pageText(bench.shell);

    CHECK(failedWith(bench.tabs->write(Orchestrator, shellId, "\x1b[2Jowned"), ServedTabError::NotOwned));
    CHECK(failedWith(bench.tabs->read(Orchestrator, shellId), ServedTabError::NotOwned));
    CHECK(failedWith(bench.tabs->close(Orchestrator, shellId), ServedTabError::NotOwned));
    CHECK(failedWith(bench.tabs->reveal(Orchestrator, shellId, origin), ServedTabError::NotOwned));
    CHECK(failedWith(bench.tabs->write(Orchestrator, "garbage", "x"), ServedTabError::NotOwned));

    // The shell's screen, input and life are untouched.
    bench.live.host.writeInput(bench.shell, "typed");
    bench.turn();
    bench.turn();
    CHECK(bench.live.host.sessionCount() == sessionsBefore);
    CHECK(bench.live.host.hostedSession(bench.shell) != nullptr);
    CHECK(bench.live.pageText(bench.shell) == pageBefore);
    CHECK(window.reveals.empty());
    CHECK(window.switches.empty());
}

TEST_CASE("another extension's tab is not owned", "[daemon][served]")
{
    auto bench = ServedBench {};
    auto window = FakeWindow { 1 };
    auto const origin = bench.attach(window, bench.first);
    auto const opened = bench.tabs->open(Orchestrator, "Agents", "", AgentsScheme, origin);
    REQUIRE(opened.has_value());
    auto const session = bench.sessionOf(*opened);

    CHECK(failedWith(bench.tabs->write("other", *opened, "\x1b[2Jmine"), ServedTabError::NotOwned));
    CHECK(failedWith(bench.tabs->read("other", *opened), ServedTabError::NotOwned));
    CHECK(failedWith(bench.tabs->close("other", *opened), ServedTabError::NotOwned));
    CHECK(failedWith(bench.tabs->reveal("other", *opened, origin), ServedTabError::NotOwned));
    CHECK(bench.tabs->list("other").empty());
    bench.turn();
    CHECK(bench.live.host.hostedSession(session) != nullptr);
    CHECK_FALSE(bench.live.pageText(session).contains("mine"));
}

TEST_CASE("revoking closes the extension's tabs", "[daemon][served]")
{
    auto bench = ServedBench {};
    auto window = FakeWindow { 1 };
    auto const origin = bench.attach(window, bench.first);
    auto const opened = bench.tabs->open(Orchestrator, "Agents", "", AgentsScheme, origin);
    REQUIRE(opened.has_value());
    std::ignore = bench.sessionOf(*opened);
    // A second one, still pending, in another workspace.
    auto other = FakeWindow { 2 };
    auto const second = bench.newWorkspace();
    auto const pending =
        bench.tabs->open(Orchestrator, "Agents", "", AgentsScheme, bench.attach(other, second));
    REQUIRE(pending.has_value());

    bench.tabs->closeAll(Orchestrator);
    CHECK(bench.tabs->list(Orchestrator).empty());
    CHECK(bench.live.eventually([&] { return bench.served().empty(); }));
    bench.turn();
    CHECK(bench.served().empty());
    // The shell is not the extension's.
    CHECK(bench.live.host.hostedSession(bench.shell) != nullptr);
}

TEST_CASE("the user's close of a served tab tells its extension", "[daemon][served]")
{
    auto bench = ServedBench {};
    auto window = FakeWindow { 1 };
    auto const origin = bench.attach(window, bench.first);
    auto const opened = bench.tabs->open(Orchestrator, "Agents", "", AgentsScheme, origin);
    REQUIRE(opened.has_value());
    auto const session = bench.sessionOf(*opened);
    REQUIRE(bench.live.eventually([&] { return bench.tabHints() > 0; }));
    auto const hintsBefore = bench.tabHints();

    bench.live.host.closeSession(session);
    CHECK(bench.live.eventually([&] { return bench.tabs->list(Orchestrator).empty(); }));
    CHECK(bench.live.eventually([&] { return bench.tabHints() > hintsBefore; }));
    CHECK(failedWith(bench.tabs->write(Orchestrator, *opened, "x"), ServedTabError::NotOwned));
}

TEST_CASE("a guest thread opens, draws and reads while the loop turns", "[daemon][served][threading]")
{
    auto bench = ServedBench {};
    auto window = FakeWindow { 1 };
    auto const origin = bench.attach(window, bench.first);
    auto const second = bench.newWorkspace();
    auto done = std::atomic<bool> { false };
    auto opened = std::string {};
    {
        auto guest = std::jthread { [&] {
            // No assertion on this thread: Catch2's are the test thread's.
            auto const id = bench.tabs->open(Orchestrator, "Agents", "", AgentsScheme, origin);
            if (!id)
            {
                done = true;
                return;
            }
            for ([[maybe_unused]] auto const round: std::views::iota(0, 200))
            {
                std::ignore = bench.tabs->write(Orchestrator, *id, "\x1b[Hframe");
                std::ignore = bench.tabs->read(Orchestrator, *id);
                std::ignore = bench.tabs->list(Orchestrator);
                std::ignore = bench.tabs->open(Orchestrator, "Agents", "", AgentsScheme, origin);
            }
            opened = *id;
            done = true;
        } };
        // Meanwhile the loop creates the tab, and the window switches back and forth.
        auto flip = false;
        while (!done)
        {
            bench.select(window, flip ? bench.first : second);
            flip = !flip;
            bench.turn();
        }
    }
    bench.select(window, bench.first);
    REQUIRE_FALSE(opened.empty());
    auto const session = bench.sessionOf(opened);
    CHECK(bench.live.eventually([&] { return bench.live.pageText(session).contains("frame"); }));
}

TEST_CASE("a dashboard moved to another workspace is that workspace's", "[daemon][served]")
{
    auto bench = ServedBench {};
    auto const second = bench.newWorkspace();
    auto one = FakeWindow { 1 };
    auto two = FakeWindow { 2 };
    auto const originOne = bench.attach(one, bench.first);
    auto const originTwo = bench.attach(two, second);
    auto const opened = bench.tabs->open(Orchestrator, "Agents", "", AgentsScheme, originOne);
    REQUIRE(opened.has_value());
    auto const session = bench.sessionOf(*opened);
    auto const [workspace, tab, leaf] = bench.live.host.model().findSessionLeaf(session);
    REQUIRE(tab != nullptr);
    bench.live.host.model().moveTabToWorkspace(bench.first, tab->id(), second, 0);
    REQUIRE(bench.workspaceOf(session) == second);

    // Opened from the second workspace, it is the one there: revealed, not made twice.
    CHECK(bench.tabs->open(Orchestrator, "Agents", "", AgentsScheme, originTwo) == opened);
    CHECK(bench.live.eventually([&] { return two.reveals == std::vector { session }; }));
    CHECK(bench.served().size() == 1);

    // The first workspace has none any more: its own is made.
    auto const own = bench.tabs->open(Orchestrator, "Agents", "", AgentsScheme, originOne);
    REQUIRE(own.has_value());
    CHECK(*own != *opened);
    CHECK(bench.workspaceOf(bench.sessionOf(*own)) == bench.first);
    CHECK(bench.served().size() == 2);
}

TEST_CASE("a dashboard's one pane dragged to another workspace moves as its tab", "[daemon][served]")
{
    auto bench = ServedBench {};
    auto const second = bench.newWorkspace();
    auto one = FakeWindow { 1 };
    auto two = FakeWindow { 2 };
    auto const originOne = bench.attach(one, bench.first);
    auto const originTwo = bench.attach(two, second);
    auto const opened = bench.tabs->open(Orchestrator, "Agents", "", AgentsScheme, originOne);
    REQUIRE(opened.has_value());
    auto const session = bench.sessionOf(*opened);

    // Dragged as a pane, as a window drags a tab's only pane: a served tab stays whole, so it moves as the
    // tab.
    auto& host = bench.live.host;
    auto const moved =
        host.moveNode(vtworkspace::PaneSource { session },
                      vtworkspace::TabWellTarget { .workspace = second, .index = 0 },
                      vthost::MoveWorkspaceHooks {
                          .create = [&] { return host.createEmptyWorkspace().id(); },
                          .discard = [&](vtworkspace::WorkspaceId id) { host.discardWorkspace(id); } });
    REQUIRE(moved.has_value());
    REQUIRE(bench.workspaceOf(session) == second);
    CHECK(host.servedBy(session) != nullptr);

    // Tracked: opened from the second workspace, it is the one there, and revealed there.
    CHECK(bench.tabs->open(Orchestrator, "Agents", "", AgentsScheme, originTwo) == opened);
    CHECK(bench.live.eventually([&] { return two.reveals == std::vector { session }; }));
    CHECK(bench.served().size() == 1);
}

TEST_CASE("an extension cannot act through another one's origin", "[daemon][served]")
{
    auto bench = ServedBench {};
    auto window = FakeWindow { 1 };
    // The window invoked the orchestrator's action: its origin was handed to the orchestrator only.
    auto const origin = bench.attach(window, bench.first);
    CHECK(failedWith(bench.tabs->open("other", "Other", "", AgentsScheme, origin), ServedTabError::Gone));
    bench.turn();
    CHECK(bench.served().empty());
    CHECK(bench.tabs->list("other").empty());

    // Nor reveal its own tab to that window.
    auto otherWindow = FakeWindow { 2 };
    auto const otherOrigin = bench.attach(otherWindow, bench.first, "other");
    auto const opened = bench.tabs->open("other", "Other", "", AgentsScheme, otherOrigin);
    REQUIRE(opened.has_value());
    std::ignore = bench.sessionOf(*opened);
    otherWindow.reveals.clear();
    CHECK(failedWith(bench.tabs->reveal("other", *opened, origin), ServedTabError::Gone));
    bench.turn();
    CHECK(window.reveals.empty());
}

namespace
{
/// What the stopped frame of the orchestrator says, its fault a trap.
constexpr auto StoppedTitle = std::string_view { "Agents stopped after repeated errors" };
constexpr auto StoppedFault = std::string_view { "extension trapped" };
constexpr auto StoppedRestart =
    std::string_view { "Restart it from the command palette: \u201CAgents: restart extension\u201D." };

/// @return The page size of @p session's terminal.
[[nodiscard]] vtbackend::PageSize pageSizeOf(LiveHost& live, SessionId session)
{
    auto& terminal = live.host.hostedSession(session)->terminal();
    auto const lock = std::lock_guard { terminal };
    return terminal.pageSize();
}
} // namespace

TEST_CASE("the stopped frame is a box centred on the page, naming the fault and the restart row",
          "[daemon][served]")
{
    auto bench = ServedBench {};
    auto window = FakeWindow { 1 };
    auto const origin = bench.attach(window, bench.first);
    auto const opened = bench.tabs->open(Orchestrator, "Agents", "", AgentsScheme, origin);
    REQUIRE(opened.has_value());
    auto const session = bench.sessionOf(*opened);
    REQUIRE(bench.tabs->write(Orchestrator, *opened, "\x1b[2J\x1b[H\x1b[31mdashboard").has_value());
    REQUIRE(bench.live.eventually([&] { return bench.live.pageText(session).contains("dashboard"); }));

    bench.tabs->drawStopped(Orchestrator, "Agents", StoppedFault);
    // 80 wide: the restart line is the widest, 65 columns; the box is 69 wide and 5 high, centred.
    auto const inner = std::size_t { 65 };
    auto const box = expectedStoppedBox(pageSizeOf(bench.live, session),
                                        { { StoppedTitle, StoppedTitle.size() },
                                          { StoppedFault, StoppedFault.size() },
                                          { StoppedRestart, inner } },
                                        inner);
    auto const drawn = bench.live.eventually([&] { return showsBox(bench.live.pageText(session), box); });
    INFO(bench.live.pageText(session));
    REQUIRE(drawn);
    auto const lines = pageLines(bench.live.pageText(session));
    // Nothing else is left of what the extension drew, and the cursor is hidden: every sequence of the frame
    // passed the served session's allowlist.
    CHECK(std::ranges::all_of(lines | std::views::take(box.top),
                              [](std::string const& line) { return line.empty(); }));
    CHECK(std::ranges::all_of(lines | std::views::drop(box.top + box.lines.size()),
                              [](std::string const& line) { return line.empty(); }));
    auto& terminal = bench.live.host.hostedSession(session)->terminal();
    auto const lock = std::lock_guard { terminal };
    CHECK_FALSE(terminal.isModeEnabled(vtbackend::DECMode::VisibleCursor));
}

TEST_CASE("the host's frame follows a resize until the extension draws again", "[daemon][served]")
{
    auto bench = ServedBench {};
    auto window = FakeWindow { 1 };
    auto const origin = bench.attach(window, bench.first);
    auto const opened = bench.tabs->open(Orchestrator, "Agents", "", AgentsScheme, origin);
    REQUIRE(opened.has_value());
    auto const session = bench.sessionOf(*opened);
    bench.tabs->drawStopped(Orchestrator, "Agents", StoppedFault);
    REQUIRE(bench.live.eventually([&] { return bench.live.pageText(session).contains(StoppedTitle); }));

    // 50 wide: the box fills the width, 46 columns inside; the restart line is cut with an ellipsis.
    std::ignore = bench.live.host.applyClientSize(
        &window, vtpty::PageSize { vtbackend::LineCount(20), vtbackend::ColumnCount(50) }, std::nullopt);
    REQUIRE(bench.live.eventually(
        [&] { return pageSizeOf(bench.live, session).columns == vtbackend::ColumnCount(50); }));
    auto const inner = std::size_t { 46 };
    auto const cut = std::string_view { "Restart it from the command palette: \u201CAgents:\u2026" };
    auto const box = expectedStoppedBox(
        pageSizeOf(bench.live, session),
        { { StoppedTitle, StoppedTitle.size() }, { StoppedFault, StoppedFault.size() }, { cut, inner } },
        inner);
    auto const redrawn = bench.live.eventually([&] { return showsBox(bench.live.pageText(session), box); });
    INFO(bench.live.pageText(session));
    CHECK(redrawn);

    // Restarting: one line says so, drawn again at every new size, until the extension draws.
    bench.tabs->resumeDrawing(Orchestrator);
    auto const restarting = std::string_view { "Agents restarting\u2026" };
    auto const restartingBox = [&] {
        return expectedStoppedBox(pageSizeOf(bench.live, session), { { restarting, 18 } }, 18);
    };
    REQUIRE(bench.live.eventually([&] { return showsBox(bench.live.pageText(session), restartingBox()); }));
    CHECK_FALSE(bench.live.pageText(session).contains(StoppedTitle));
    std::ignore = bench.live.host.applyClientSize(
        &window, vtpty::PageSize { vtbackend::LineCount(20), vtbackend::ColumnCount(56) }, std::nullopt);
    REQUIRE(bench.live.eventually(
        [&] { return pageSizeOf(bench.live, session).columns == vtbackend::ColumnCount(56); }));
    CHECK(bench.live.eventually([&] { return showsBox(bench.live.pageText(session), restartingBox()); }));

    // The extension draws, and a resize leaves what it drew.
    REQUIRE(bench.tabs->write(Orchestrator, *opened, "\x1b[2J\x1b[Hdashboard").has_value());
    REQUIRE(bench.live.eventually([&] { return bench.live.pageText(session).contains("dashboard"); }));
    std::ignore = bench.live.host.applyClientSize(
        &window, vtpty::PageSize { vtbackend::LineCount(22), vtbackend::ColumnCount(60) }, std::nullopt);
    REQUIRE(bench.live.eventually([&] {
        auto const listed = bench.tabs->list(Orchestrator);
        return listed.size() == 1 && listed.front().cols == 60;
    }));
    // A mark written after the resize: what the resize would have drawn reached the terminal before it.
    REQUIRE(bench.tabs->write(Orchestrator, *opened, "\x1b[22;1Hend").has_value());
    REQUIRE(bench.live.eventually([&] { return bench.live.pageText(session).contains("end"); }));
    CHECK(bench.live.pageText(session).contains("dashboard"));
    CHECK_FALSE(bench.live.pageText(session).contains(StoppedTitle));
    CHECK_FALSE(bench.live.pageText(session).contains(restarting));
}

TEST_CASE("the stopped frame measures an emoji title as the screen lays it out", "[daemon][served]")
{
    auto bench = ServedBench {};
    auto window = FakeWindow { 1 };
    auto const origin = bench.attach(window, bench.first);
    auto const opened = bench.tabs->open(Orchestrator, "Agents", "", AgentsScheme, origin);
    REQUIRE(opened.has_value());
    auto const session = bench.sessionOf(*opened);
    // 50 wide: the box takes the whole width, so a column measured short would wrap its right border.
    std::ignore = bench.live.host.applyClientSize(
        &window, vtpty::PageSize { vtbackend::LineCount(20), vtbackend::ColumnCount(50) }, std::nullopt);
    REQUIRE(bench.live.eventually(
        [&] { return pageSizeOf(bench.live, session).columns == vtbackend::ColumnCount(50); }));

    // U+2764 U+FE0F: one cluster, two columns where its first codepoint alone takes one.
    bench.tabs->drawStopped(Orchestrator, "Agents \u2764\uFE0F", StoppedFault);
    auto const title = std::string_view { "Agents \u2764\uFE0F stopped after repeated errors" };
    auto const cut = std::string_view { "Restart it from the command palette: \u201CAgents \u2026" };
    auto const box = expectedStoppedBox(pageSizeOf(bench.live, session),
                                        { { title, 39 }, { StoppedFault, StoppedFault.size() }, { cut, 46 } },
                                        46);
    auto const drawn = bench.live.eventually([&] { return showsBox(bench.live.pageText(session), box); });
    INFO(bench.live.pageText(session));
    CHECK(drawn);
    // Nothing spilled onto the line below the box.
    auto const lines = pageLines(bench.live.pageText(session));
    REQUIRE(lines.size() > box.top + box.lines.size());
    CHECK(lines[box.top + box.lines.size()].empty());
}

TEST_CASE("the host's frames leave the scrollback as it was", "[daemon][served]")
{
    auto bench = ServedBench {};
    auto window = FakeWindow { 1 };
    auto const origin = bench.attach(window, bench.first);
    auto const opened = bench.tabs->open(Orchestrator, "Agents", "", AgentsScheme, origin);
    REQUIRE(opened.has_value());
    auto const session = bench.sessionOf(*opened);
    auto const historyOf = [&] {
        auto& terminal = bench.live.host.hostedSession(session)->terminal();
        auto const lock = std::lock_guard { terminal };
        return terminal.primaryScreen().historyLineCount();
    };
    bench.tabs->drawStopped(Orchestrator, "Agents", StoppedFault);
    REQUIRE(bench.live.eventually([&] { return bench.live.pageText(session).contains(StoppedTitle); }));
    auto const history = historyOf();

    // Each wider page draws the frame again -- and an erase of the whole page would push a copy of it into
    // the scrollback every time. Widening alone rewraps nothing.
    auto const lines = pageSizeOf(bench.live, session).lines;
    for (auto const columns: { 90, 100, 110 })
    {
        std::ignore = bench.live.host.applyClientSize(
            &window, vtpty::PageSize { lines, vtbackend::ColumnCount(columns) }, std::nullopt);
        REQUIRE(bench.live.eventually([&] {
            auto const page = pageLines(bench.live.pageText(session));
            return pageSizeOf(bench.live, session).columns == vtbackend::ColumnCount(columns)
                   && std::ranges::any_of(page, [&](std::string const& line) {
                          // Centred anew: its left border moved with the width.
                          return line.contains(StoppedTitle)
                                 && line.starts_with(
                                     std::string(static_cast<std::size_t>((columns - 69) / 2), ' ')
                                     + "\u2502");
                      });
        }));
    }
    bench.tabs->resumeDrawing(Orchestrator);
    REQUIRE(bench.live.eventually([&] { return bench.live.pageText(session).contains("restarting"); }));
    CHECK(historyOf() == history);
}

TEST_CASE("a served tab keeps no history whatever the profiles say", "[daemon][served]")
{
    auto bench = ServedBench {};
    auto window = FakeWindow { 1 };
    auto const origin = bench.attach(window, bench.first);
    auto const opened = bench.tabs->open(Orchestrator, "Agents", "", AgentsScheme, origin);
    REQUIRE(opened.has_value());
    auto const session = bench.sessionOf(*opened);
    auto const historyOf = [&] {
        auto& terminal = bench.live.host.hostedSession(session)->terminal();
        auto const lock = std::lock_guard { terminal };
        return std::pair { terminal.settings().historyLimits, terminal.primaryScreen().historyLineCount() };
    };
    auto const none = vtbackend::HistoryLimits::plain(vtbackend::LineCount(0));
    CHECK(historyOf() == std::pair { none, vtbackend::LineCount(0) });

    // Twice the page scrolled through: nothing of it is kept above the page.
    auto const lines = unbox(pageSizeOf(bench.live, session).lines);
    auto text = std::string {};
    for (auto const line: std::views::iota(0, 2 * lines))
        text += std::format("line {}\r\n", line);
    text += "the end";
    REQUIRE(bench.tabs->write(Orchestrator, *opened, text).has_value());
    REQUIRE(bench.live.eventually([&] { return bench.live.pageText(session).contains("the end"); }));
    CHECK(historyOf() == std::pair { none, vtbackend::LineCount(0) });

    // A reload of the profiles hands the shells their scrollback, and the served tab still none.
    bench.live.host.replaceProfiles(std::make_shared<vthost::testing::FakeProfileCatalog>(
        std::map<std::string, vthost::ResolvedProfile> {
            { "main", vthost::testing::fakeProfile("/bin/zsh", vtbackend::LineCount(1000)) } },
        "main"));
    CHECK(historyOf().first == none);
}

TEST_CASE("a page too small for the stopped box shows its lines alone, cut", "[daemon][served]")
{
    auto const frame = stoppedFrame(
        vtbackend::PageSize { vtbackend::LineCount(2), vtbackend::ColumnCount(8) }, "Agents", "trap");
    CHECK_FALSE(frame.contains("\u256D"));
    CHECK(frame.contains("\x1b[1;1HAgents \u2026"));
    CHECK(frame.contains("\x1b[2;1Htrap"));
    // Two lines fit: the third is not drawn.
    CHECK_FALSE(frame.contains("Restart"));
    CHECK_FALSE(frame.contains("\x1b[3;1H"));
    // Drawn in one synchronized update, from a cleared screen.
    CHECK(frame.starts_with("\x1b[?2026h"));
    CHECK(frame.ends_with("\x1b[?2026l"));
}
