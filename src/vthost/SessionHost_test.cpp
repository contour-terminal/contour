// SPDX-License-Identifier: Apache-2.0
#include <vtbackend/core/TerminalContext.hpp>
#include <vtbackend/screen/LocalTime.hpp>
#include <vtbackend/screen/TerminalClocks.hpp>
#include <vtbackend/shell/CommandBlock.hpp>
#include <vtbackend/testing/InMemoryGitFileSystem.hpp>
#include <vtbackend/testing/MockTerm.hpp>
#include <vtbackend/testing/TestHelpers.hpp>

#include <vtpty/MockPty.hpp>
#ifndef _WIN32
    #include <vtpty/Process.hpp>
    #include <vtpty/ProcessPlacement.hpp>
    #include <vtpty/Pty.hpp>
    #include <vtpty/UnixPty.hpp>
    #include <vtpty/test/PtyReading.hpp>
#endif

#include <crispy/BufferObject.hpp>

#include <core/async/Task.hpp>
#include <core/async/testing/ManualExecutor.hpp>
#include <core/log/LogSink.hpp>
#include <core/net/EventLoop.hpp>
#include <core/net/IoBackend.hpp>
#include <core/net/testing/CoroTestSupport.hpp>
#include <core/net/testing/ScriptedBackend.hpp>
#include <core/platform/Clock.hpp>
#include <core/platform/NativeFileSystem.hpp>
#include <core/platform/testing/InMemoryFileSystem.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <filesystem>
#include <format>
#include <fstream>
#include <functional>
#include <future>
#include <iterator>
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

#include <vtcontrol/ObjectRef.hpp>
#include <vtcontrol/testing/SequentialUuidSource.hpp>

#ifdef CONTOUR_EXTENSIONS
    #include <vtextension/testing/TestModules.hpp>
    #include <vtextension/wasm/DefaultBackend.hpp>
    #include <vtextension/wasm/WasmRuntime.hpp>
    #include <vthost/DaemonExtensions.hpp>
    #include <vthost/control/ControlService.hpp>
    #include <vthost/testing/GatedFileSystem.hpp>
#endif
#include <vthost/ConfigReload.hpp>
#include <vthost/HostedShell.hpp>
#include <vthost/ObjectRegistry.hpp>
#include <vthost/SessionHost.hpp>
#include <vthost/TappingPty.hpp>
#include <vthost/testing/FakeExtensionHosting.hpp>
#include <vthost/testing/FakeProfileCatalog.hpp>
#include <vthost/testing/TestHelpers.hpp>
#include <vtworkspace/LayoutTree.hpp>
#include <vtworkspace/Pane.hpp>
#include <vtworkspace/Tab.hpp>

using vthost::SessionHost;
using vtworkspace::SplitState;

namespace
{

/// Records every fanned-out model event so tests can assert what subscribers saw.
struct RecordingEvents final: vtworkspace::ModelEvents
{
    std::vector<std::string> log;

    void tabAdded(vtworkspace::WindowId, vtworkspace::TabId, int index) override
    {
        log.push_back(std::format("tabAdded:{}", index));
    }
    void tabClosed(vtworkspace::WindowId, vtworkspace::TabId, int index) override
    {
        log.push_back(std::format("tabClosed:{}", index));
    }
    void tabMoved(vtworkspace::WindowId, vtworkspace::TabId, int from, int to) override
    {
        log.push_back(std::format("tabMoved:{}->{}", from, to));
    }
    void activeTabChanged(vtworkspace::WindowId, vtworkspace::TabId, int index) override
    {
        log.push_back(std::format("activeTabChanged:{}", index));
    }
    void paneSplit(vtworkspace::TabId, vtworkspace::PaneId, vtworkspace::PaneId) override
    {
        log.emplace_back("paneSplit");
    }
    void paneClosed(vtworkspace::TabId, vtworkspace::PaneId, vtworkspace::PaneId) override
    {
        log.emplace_back("paneClosed");
    }
    void activePaneChanged(vtworkspace::TabId, vtworkspace::PaneId) override
    {
        log.emplace_back("activePaneChanged");
    }
    void paneRatioChanged(vtworkspace::TabId, vtworkspace::PaneId, double) override
    {
        log.emplace_back("paneRatioChanged");
    }
    void tabTitleChanged(vtworkspace::TabId) override { log.emplace_back("tabTitleChanged"); }
    void tabColorChanged(vtworkspace::TabId) override { log.emplace_back("tabColorChanged"); }

    [[nodiscard]] bool saw(std::string_view needle) const
    {
        return std::ranges::any_of(log, [&](auto const& entry) { return entry.starts_with(needle); });
    }
};

/// A PTY whose read BLOCKS, the way a real one does — MockPty answers every read immediately, so it
/// cannot exercise a teardown that has to interrupt a blocked reader.
///
/// It reproduces the two behaviours of `vtpty::UnixPty` that matter here: a read with no timeout
/// parks until `wakeupReader()`, and a read on a CLOSED device reports `EAGAIN` rather than EOF (the
/// selector is empty, so it parks on the break pipe). `EAGAIN` is what `Terminal::processInputOnce`
/// answers with "retry", which is how a pump with no timeout can park forever.
class BlockingPty final: public vtpty::Pty
{
  public:
    explicit BlockingPty(vtbackend::PageSize size) noexcept: _size(size) {}

    [[nodiscard]] vtpty::StartResult start() override { return vtpty::StartOutcome {}; }
    [[nodiscard]] vtpty::PtySlave& slave() noexcept override { return _slave; }
    void close() override
    {
        {
            auto const lock = std::lock_guard { _mutex };
            _closed = true;
        }
        wakeupReader();
    }
    [[nodiscard]] bool isClosed() const noexcept override
    {
        auto const lock = std::lock_guard { _mutex };
        return _closed;
    }
    void wakeupReader() override
    {
        {
            auto const lock = std::lock_guard { _mutex };
            _wakeups += 1;
        }
        _wake.notify_all();
    }

    [[nodiscard]] std::optional<ReadResult> read(crispy::BufferObject<char>& /*storage*/,
                                                 std::optional<std::chrono::milliseconds> timeout,
                                                 size_t /*size*/) override
    {
        auto lock = std::unique_lock { _mutex };
        _reads += 1;
        auto const woken = [this] {
            return _wakeups > 0;
        };
        if (timeout)
            std::ignore = _wake.wait_for(lock, *timeout, woken);
        else
            _wake.wait(lock, woken); // no timeout: parks until a wakeup, exactly like the real thing
        if (_wakeups > 0)
            _wakeups -= 1;
        errno = EAGAIN; // never EOF — a closed device still reports "retry"
        return std::nullopt;
    }

    [[nodiscard]] int write(std::string_view data) override { return static_cast<int>(data.size()); }
    [[nodiscard]] vtpty::PageSize pageSize() const noexcept override { return _size; }
    void resizeScreen(vtpty::PageSize size, std::optional<vtpty::ImageSize>) override { _size = size; }
    void waitForClosed() override
    {
        auto lock = std::unique_lock { _mutex };
        _wake.wait(lock, [this] { return _closed; });
    }

    /// How many reads the pump issued — proof it really parked rather than spinning.
    [[nodiscard]] int reads() const
    {
        auto const lock = std::lock_guard { _mutex };
        return _reads;
    }

  private:
    mutable std::mutex _mutex;
    std::condition_variable _wake;
    vtpty::PtySlaveDummy _slave;
    vtpty::PageSize _size;
    bool _closed = false;
    int _wakeups = 0;
    int _reads = 0;
};

/// A PTY whose program prints @p output, then waits until it exits (EOF) or the host closes the device.
///
/// What tells a process that exited by itself from a close the host asked for: the first reads EOF, the
/// second reads `EAGAIN` on a closed device, as `BlockingPty` documents of the real thing.
class ExitingPty final: public vtpty::Pty
{
  public:
    ExitingPty(vtbackend::PageSize size, std::string output): _size(size), _output(std::move(output)) {}

    [[nodiscard]] vtpty::StartResult start() override { return vtpty::StartOutcome {}; }
    [[nodiscard]] vtpty::PtySlave& slave() noexcept override { return _slave; }
    void close() override
    {
        {
            auto const lock = std::lock_guard { _mutex };
            _closed = true;
        }
        _wake.notify_all();
    }
    [[nodiscard]] bool isClosed() const noexcept override
    {
        auto const lock = std::lock_guard { _mutex };
        return _closed;
    }
    void wakeupReader() override { _wake.notify_all(); }

    /// The program exits: the next read reports EOF.
    void exitProcess()
    {
        {
            auto const lock = std::lock_guard { _mutex };
            _exited = true;
        }
        _wake.notify_all();
    }

    [[nodiscard]] std::optional<ReadResult> read(crispy::BufferObject<char>& storage,
                                                 std::optional<std::chrono::milliseconds> timeout,
                                                 size_t /*size*/) override
    {
        auto lock = std::unique_lock { _mutex };
        if (!_output.empty())
        {
            auto const pooled = storage.writeAtEnd(_output);
            _output.clear();
            return ReadResult { .data = std::string_view(pooled.data(), pooled.size()),
                                .fromStdoutFastPipe = false };
        }
        // The timeout is honoured: the parser reads ahead with a zero one while another thread holds
        // the terminal, and a read that blocked there would park the pump before it parsed anything.
        auto const ended = [this] {
            return _exited || _closed;
        };
        if (timeout)
            std::ignore = _wake.wait_for(lock, *timeout, ended);
        else
            _wake.wait(lock, ended);
        errno = _exited ? EIO : EAGAIN;
        return std::nullopt;
    }

    [[nodiscard]] int write(std::string_view data) override { return static_cast<int>(data.size()); }
    [[nodiscard]] vtpty::PageSize pageSize() const noexcept override { return _size; }
    void resizeScreen(vtpty::PageSize size, std::optional<vtpty::ImageSize>) override { _size = size; }
    void waitForClosed() override
    {
        auto lock = std::unique_lock { _mutex };
        _wake.wait(lock, [this] { return _closed; });
    }

  private:
    mutable std::mutex _mutex;
    std::condition_variable _wake;
    vtpty::PtySlaveDummy _slave;
    vtpty::PageSize _size;
    std::string _output;
    bool _closed = false;
    bool _exited = false;
};

/// Stands in for one attached client. The client-area registry is keyed by stream observer, so a
/// test that reports an area has to be somebody — and these tests are about what the host does with
/// the report, not about who made it.
struct StubClient final: vthost::SessionStreamEvents
{
};

/// A single-pane tab whose leaf runs @p command — the shape most of the new tests build on.
[[nodiscard]] vtworkspace::LayoutTab leafTab(std::string command)
{
    auto tab = vtworkspace::LayoutTab {};
    tab.root.command = std::move(command);
    return tab;
}

/// A host over MockPty sessions with pump threads disabled (tests drive the
/// model on the calling thread, which stands in for the loop thread).
struct HostHarness
{
    /// @param sizePolicy How the host resolves one client area from several reports.
    /// @param hostIdentity The machine the host says it runs on.
    explicit HostHarness(vthost::ClientSizePolicy sizePolicy = vthost::ClientSizePolicy::Latest,
                         vtbackend::HostIdentity hostIdentity = {}):
        policy { sizePolicy }, identity { std::move(hostIdentity) }
    {
        host.subscribe(&recorder);
    }

    core::net::testing::ScriptedBackend source;
    core::net::EventLoop loop { source };
    RecordingEvents recorder;
    StubClient client;
    StubClient otherClient;           ///< For the multi-client policy cases.
    vthost::ClientSizePolicy policy;  // set by the constructor, before `host` reads it
    vtbackend::HostIdentity identity; // set by the constructor, before `host` reads it
    /// The override every spawn was given, in spawn order (an absent one as an empty shell).
    std::vector<vtpty::Process::ExecInfo> seen;
    SessionHost host {
        loop,
        [this](vtbackend::PageSize size, std::optional<vtpty::Process::ExecInfo> const& spawn) {
            seen.push_back(spawn.value_or(vtpty::Process::ExecInfo {}));
            return std::make_unique<vtpty::MockPty>(size);
        },
        vtbackend::Settings {},
        core::defaultEnvironment(),
        vthost::SessionHostConfig { .pumping = vthost::SessionPumping::Manual,
                                    .sizePolicy = policy,
                                    .startupLayout = vtworkspace::Layout {},
                                    .identity = identity,
                                    .uuids = std::make_shared<vtcontrol::testing::SequentialUuidSource>() }
    };
};

/// @param count Which identifier of a `SequentialUuidSource` (1-based).
/// @return That identifier.
[[nodiscard]] vtcontrol::Uuid seq(unsigned count)
{
    return *vtcontrol::parseUuid(std::format("00000000-0000-7000-8000-{:012x}", count));
}

/// @param kind What the id names.
/// @param count Which identifier of a `SequentialUuidSource`.
/// @return Its reference as the environment spells it, e.g. `pane:00000000-...-000000000002`.
[[nodiscard]] std::string ref(vtcontrol::ObjectKind kind, unsigned count)
{
    return vtcontrol::toString(vtcontrol::ObjectRef { kind, seq(count) });
}

} // namespace

TEST_CASE("createTab seeds a backing session handed back by the allocator", "[vthost][host]")
{
    auto h = HostHarness {};

    auto* tab = h.host.createTab();
    REQUIRE(tab != nullptr);
    REQUIRE(h.host.sessionCount() == 1);

    // The pane's session id is the pre-minted one, and it maps to a live terminal.
    auto const session = tab->rootPane()->session();
    REQUIRE(h.host.terminal(session) != nullptr);
    CHECK(h.recorder.saw("tabAdded"));
}

TEST_CASE("splitActivePane backs the new leaf with a fresh session", "[vthost][host]")
{
    auto h = HostHarness {};
    auto* tab = h.host.createTab();
    REQUIRE(tab != nullptr);

    h.host.splitActivePane(tab->id(), SplitState::Vertical, 0.5);

    REQUIRE(tab->paneCount() == 2);
    REQUIRE(h.host.sessionCount() == 2);
    CHECK(h.recorder.saw("paneSplit"));

    // Both leaves resolve to distinct live terminals.
    auto* first = h.host.terminal(tab->rootPane()->first()->session());
    auto* second = h.host.terminal(tab->rootPane()->second()->session());
    REQUIRE(first != nullptr);
    REQUIRE(second != nullptr);
    CHECK(first != second);
}

TEST_CASE("a refused split reaps the orphaned backing session", "[vthost][host]")
{
    auto h = HostHarness {};
    auto* tab = h.host.createTab();
    REQUIRE(tab != nullptr);
    REQUIRE(h.host.sessionCount() == 1);

    h.host.splitActivePane(vtworkspace::TabId { 4711 }, SplitState::Vertical, 0.5);

    // The unknown tab refused the split; the pre-spawned session must not leak.
    CHECK(h.host.sessionCount() == 1);
    CHECK(tab->paneCount() == 1);
}

TEST_CASE("a session exit prunes its pane and keeps the sibling", "[vthost][host]")
{
    auto h = HostHarness {};
    auto* tab = h.host.createTab();
    REQUIRE(tab != nullptr);
    h.host.splitActivePane(tab->id(), SplitState::Horizontal, 0.5);
    REQUIRE(h.host.sessionCount() == 2);

    auto const exited = tab->rootPane()->first()->session();
    auto const surviving = tab->rootPane()->second()->session();

    h.host.handleSessionExit(exited);

    CHECK(h.host.sessionCount() == 1);
    CHECK(h.host.terminal(exited) == nullptr);
    CHECK(h.host.terminal(surviving) != nullptr);
    CHECK(tab->paneCount() == 1);
    CHECK(h.recorder.saw("paneClosed"));
}

TEST_CASE("the last pane's session exit closes the whole tab", "[vthost][host]")
{
    auto h = HostHarness {};
    auto* tab = h.host.createTab();
    REQUIRE(tab != nullptr);
    auto const session = tab->rootPane()->session();

    h.host.handleSessionExit(session);

    CHECK(h.host.sessionCount() == 0);
    CHECK(h.host.model().window(h.host.windowId())->tabCount() == 0);
    CHECK(h.recorder.saw("tabClosed"));
}

TEST_CASE("writeInput stamps the session's last input on the loop clock", "[vthost][host][flush]")
{
    auto clock =
        core::platform::ManualClock { core::platform::SteadyTimePoint {} + std::chrono::seconds { 5 } };
    auto source = core::net::testing::ScriptedBackend {};
    auto loop = core::net::EventLoop { source, clock };
    auto host = SessionHost { loop,
                              [](vtbackend::PageSize size, std::optional<vtpty::Process::ExecInfo> const&) {
                                  return std::make_unique<vtpty::MockPty>(size);
                              },
                              vtbackend::Settings {},
                              core::defaultEnvironment(),
                              vthost::SessionHostConfig { .pumping = vthost::SessionPumping::Manual } };
    auto const a = vthost::testing::newSession(host);
    auto const b = vthost::testing::newSession(host);

    CHECK_FALSE(host.lastInputAt(a).has_value());
    host.writeInput(a, "x");
    CHECK(host.lastInputAt(a) == clock.now());
    CHECK_FALSE(host.lastInputAt(b).has_value());
    CHECK_FALSE(host.lastInputAt(vtworkspace::SessionId { 999 }).has_value());

    // A later input moves the stamp; input for an unknown session stamps nothing.
    clock.advance(std::chrono::milliseconds { 7 });
    host.writeInput(a, "y");
    host.writeInput(vtworkspace::SessionId { 999 }, "z");
    CHECK(host.lastInputAt(a) == clock.now());
    CHECK_FALSE(host.lastInputAt(vtworkspace::SessionId { 999 }).has_value());

    host.handleSessionExit(a);
    CHECK_FALSE(host.lastInputAt(a).has_value());
}

TEST_CASE("an unsubscribed observer stops receiving events", "[vthost][host]")
{
    auto h = HostHarness {};
    h.host.unsubscribe(&h.recorder);

    REQUIRE(h.host.createTab() != nullptr);
    CHECK(h.recorder.log.empty());
}

TEST_CASE("applyClientSize reprojects the leaves onto the new client area", "[vthost][host]")
{
    auto h = HostHarness {};
    auto* tab = h.host.createTab();
    REQUIRE(tab != nullptr);

    h.host.applyClientSize(
        &h.client, vtpty::PageSize { .lines = vtpty::LineCount(40), .columns = vtpty::ColumnCount(100) });

    // The sole full-area leaf now spans the whole client width (columns are
    // unaffected by the status-line height, so this is exact).
    auto* terminal = h.host.terminal(tab->rootPane()->session());
    REQUIRE(terminal != nullptr);
    CHECK(terminal->totalPageSize().columns.value == 100);
}

TEST_CASE("applyPaneSize resizes one pane without disturbing the rest", "[vthost][host]")
{
    auto h = HostHarness {};
    auto* tab = h.host.createTab();
    REQUIRE(tab != nullptr);
    h.host.applyClientSize(
        &h.client, vtpty::PageSize { .lines = vtpty::LineCount(30), .columns = vtpty::ColumnCount(100) });
    h.host.splitActivePane(tab->id(), SplitState::Vertical, 0.5);
    REQUIRE(h.host.sessionCount() == 2);

    auto const firstSession = tab->rootPane()->first()->session();
    auto const secondSession = tab->rootPane()->second()->session();
    // The projection of a 100-column area: 50 + one divider cell + 49.
    REQUIRE(h.host.terminal(firstSession)->totalPageSize().columns.value == 50);
    REQUIRE(h.host.terminal(secondSession)->totalPageSize().columns.value == 49);

    // A client whose divider sits elsewhere than the 0.5 ratio reports what it renders.
    h.host.applyPaneSize(
        firstSession, vtpty::PageSize { .lines = vtpty::LineCount(30), .columns = vtpty::ColumnCount(70) });

    CHECK(h.host.terminal(firstSession)->totalPageSize().columns.value == 70);
    CHECK(h.host.terminal(secondSession)->totalPageSize().columns.value == 49); // untouched
    CHECK(h.host.pageSize().columns.value == 100);                              // the client area stands

    // An unknown session is ignored rather than fatal (a race with a session exit).
    h.host.applyPaneSize(vtworkspace::SessionId { 987654 },
                         vtpty::PageSize { .lines = vtpty::LineCount(1), .columns = vtpty::ColumnCount(1) });
    CHECK(h.host.sessionCount() == 2);
}

TEST_CASE("a re-projection discards per-pane refinements", "[vthost][host]")
{
    // The refinement is valid only for the tree it was measured on: any structural change
    // re-projects, and the client re-reports against the new shape.
    auto h = HostHarness {};
    auto* tab = h.host.createTab();
    REQUIRE(tab != nullptr);
    h.host.applyClientSize(
        &h.client, vtpty::PageSize { .lines = vtpty::LineCount(30), .columns = vtpty::ColumnCount(100) });
    h.host.splitActivePane(tab->id(), SplitState::Vertical, 0.5);
    auto const firstSession = tab->rootPane()->first()->session();
    h.host.applyPaneSize(
        firstSession, vtpty::PageSize { .lines = vtpty::LineCount(30), .columns = vtpty::ColumnCount(70) });
    REQUIRE(h.host.terminal(firstSession)->totalPageSize().columns.value == 70);

    h.host.applyClientSize(
        &h.client, vtpty::PageSize { .lines = vtpty::LineCount(30), .columns = vtpty::ColumnCount(100) });

    CHECK(h.host.terminal(firstSession)->totalPageSize().columns.value == 50);
}

TEST_CASE("the client area is resolved from every attached client, by policy", "[vthost][host][size]")
{
    // One grid, several clients. Which size it is has to be a function of ALL the reports, or two
    // differently-sized clients take turns resizing every application on the daemon -- which is
    // what "the last ResizeRequest wins" meant in practice.
    auto const areaOf = [](SessionHost const& host) {
        return std::pair { unbox<int>(host.pageSize().columns), unbox<int>(host.pageSize().lines) };
    };
    auto const wide = vtpty::PageSize { .lines = vtpty::LineCount(20), .columns = vtpty::ColumnCount(200) };
    auto const tall = vtpty::PageSize { .lines = vtpty::LineCount(60), .columns = vtpty::ColumnCount(80) };

    SECTION("latest: the most recent report wins, whichever client made it")
    {
        auto h = HostHarness {}; // ClientSizePolicy::Latest is the default
        std::ignore = h.host.createTab();

        std::ignore = h.host.applyClientSize(&h.client, wide);
        CHECK(areaOf(h.host) == std::pair { 200, 20 });
        std::ignore = h.host.applyClientSize(&h.otherClient, tall);
        CHECK(areaOf(h.host) == std::pair { 80, 60 });
        // Re-reporting an area already held must not flip the answer back and forth: the sequence
        // moves, but the resolved size does not, so the two clients settle instead of fighting.
        std::ignore = h.host.applyClientSize(&h.otherClient, tall);
        CHECK(areaOf(h.host) == std::pair { 80, 60 });
    }

    SECTION("smallest: the intersection, so neither client is asked to show more than it can")
    {
        auto h = HostHarness { vthost::ClientSizePolicy::Smallest };
        std::ignore = h.host.createTab();

        std::ignore = h.host.applyClientSize(&h.client, wide);
        std::ignore = h.host.applyClientSize(&h.otherClient, tall);
        // Per axis, not per client: neither reported 80x20, but that is the largest grid BOTH can
        // display. Picking one client's whole area would hand the other a grid it cannot show.
        CHECK(areaOf(h.host) == std::pair { 80, 20 });
    }

    SECTION("largest: the union, so every client sees everything any client can")
    {
        auto h = HostHarness { vthost::ClientSizePolicy::Largest };
        std::ignore = h.host.createTab();

        std::ignore = h.host.applyClientSize(&h.client, wide);
        std::ignore = h.host.applyClientSize(&h.otherClient, tall);
        CHECK(areaOf(h.host) == std::pair { 200, 60 });
    }
}

TEST_CASE("a detaching client stops counting toward the client area", "[vthost][host][size]")
{
    // The case that rots silently if the registry is never pruned: a client that is GONE would keep
    // every application on the daemon at its dimensions for the daemon's whole life. The entry is
    // keyed by the stream subscription precisely so leaving drops it.
    auto h = HostHarness {};
    auto* tab = h.host.createTab();
    REQUIRE(tab != nullptr);

    h.host.subscribeStream(&h.client);
    h.host.subscribeStream(&h.otherClient);
    std::ignore = h.host.applyClientSize(
        &h.client, vtpty::PageSize { .lines = vtpty::LineCount(50), .columns = vtpty::ColumnCount(200) });
    std::ignore = h.host.applyClientSize(
        &h.otherClient, vtpty::PageSize { .lines = vtpty::LineCount(24), .columns = vtpty::ColumnCount(80) });
    REQUIRE(unbox<int>(h.host.pageSize().columns) == 80); // latest: the small client

    h.host.unsubscribeStream(&h.otherClient);

    // Only the large client is left, so its area is the only one there is to resolve to -- and the
    // panes must have been re-projected onto it, not merely the number updated.
    CHECK(unbox<int>(h.host.pageSize().columns) == 200);
    CHECK(h.host.terminal(tab->rootPane()->session())->totalPageSize().columns.value == 200);

    h.host.unsubscribeStream(&h.client);
    // With nobody left the area STAYS: the sessions are still hosted and must be some size, and the
    // last known one beats reverting to a default nobody asked for.
    CHECK(unbox<int>(h.host.pageSize().columns) == 200);
}

TEST_CASE("applyClientSize is race-free against a concurrent terminal writer", "[vthost][host][concurrency]")
{
    auto h = HostHarness {};
    auto* tab = h.host.createTab();
    REQUIRE(tab != nullptr);
    h.host.splitActivePane(tab->id(), SplitState::Vertical, 0.5);
    REQUIRE(h.host.sessionCount() == 2);

    auto* firstTerm = h.host.terminal(tab->rootPane()->first()->session());
    REQUIRE(firstTerm != nullptr);

    // The writer mutates a leaf's grid under _stateMutex (writeToScreen); reproject
    // now resizes the same terminal under that lock too (the fix). Under TSan an
    // unlocked resizeScreen would be flagged racing this writer's grid mutation.
    auto stop = std::atomic<bool> { false };
    auto writer = std::thread { [&] {
        for (auto i = 0; !stop.load(std::memory_order_relaxed); ++i)
            firstTerm->writeToScreen(std::format("row-{}\r\n", i));
    } };

    for (auto const i: std::views::iota(0, 300))
        std::ignore =
            h.host.applyClientSize(&h.client,
                                   vtpty::PageSize { .lines = vtpty::LineCount(20 + (i % 20)),
                                                     .columns = vtpty::ColumnCount(60 + (i % 40)) });

    stop.store(true, std::memory_order_relaxed);
    writer.join();

    CHECK(h.host.sessionCount() == 2); // no state torn: both sessions intact
}

namespace
{

/// Records the stream fan-out one attached client would receive.
struct StreamRecorder final: vthost::SessionStreamEvents
{
    std::vector<uint64_t> screens;
    std::vector<std::string> output;

    void sessionScreenUpdated(vtworkspace::SessionId session) override { screens.push_back(session.value); }
    void sessionOutput(vtworkspace::SessionId session, std::string const& bytes) override
    {
        output.push_back(std::format("{}:{}", session.value, bytes));
    }
};

core::async::Task<void> waitFor(core::net::EventLoop* loop, std::function<bool()> ready)
{
    using namespace std::chrono_literals;
    for (auto i = 0; i < 2000 && !ready(); ++i)
        co_await loop->delay(1ms);
}

} // namespace

TEST_CASE("stream events fan out to every subscriber independently", "[vthost][host]")
{
    auto const source = core::net::makeDefaultBackend();
    auto loop = core::net::EventLoop { *source };
    auto host = SessionHost { loop,
                              [](vtbackend::PageSize size, std::optional<vtpty::Process::ExecInfo> const&) {
                                  return std::make_unique<vtpty::MockPty>(size);
                              },
                              vtbackend::Settings {},
                              core::defaultEnvironment(),
                              vthost::SessionHostConfig { .pumping = vthost::SessionPumping::Manual } };
    auto first = StreamRecorder {};
    auto second = StreamRecorder {};
    host.subscribeStream(&first);
    host.subscribeStream(&second);

    REQUIRE(host.createTab() != nullptr);
    auto const session = host.model().window(host.windowId())->activeTab()->rootPane()->session();
    auto* terminal = host.terminal(session);
    REQUIRE(terminal != nullptr);

    // A screen update reaches BOTH subscribers.
    terminal->writeToScreen("hello");
    loop.blockOn(waitFor(&loop, [&] { return !first.screens.empty() && !second.screens.empty(); }));
    REQUIRE(!first.screens.empty());
    REQUIRE(!second.screens.empty());
    CHECK(first.screens.front() == session.value);
    CHECK(second.screens.front() == session.value);

    // The raw byte tap fans out too: drive one read through the TappingPty on
    // this thread (standing in for the session's pump thread).
    auto& tapped = dynamic_cast<vthost::TappingPty&>(terminal->device());
    auto& mock = dynamic_cast<vtpty::MockPty&>(tapped.inner());
    mock.appendStdOutBuffer("raw-bytes");
    auto pool = crispy::BufferObjectPool<char> { 4096 };
    auto const storage = pool.allocateBufferObject();
    std::ignore = tapped.read(*storage, std::nullopt, 4096);
    loop.blockOn(waitFor(&loop, [&] { return !first.output.empty() && !second.output.empty(); }));
    auto const expected = std::format("{}:raw-bytes", session.value);
    REQUIRE(!first.output.empty());
    REQUIRE(!second.output.empty());
    CHECK(first.output.front() == expected);
    CHECK(second.output.front() == expected);

    // Unsubscribing one observer must NOT silence the other — the regression
    // the single-slot handlers had (a disconnecting client nulled the shared
    // slot, muting every remaining client).
    host.unsubscribeStream(&first);
    auto const firstScreensSeen = first.screens.size();
    auto const secondScreensSeen = second.screens.size();
    terminal->writeToScreen("again");
    loop.blockOn(waitFor(&loop, [&] { return second.screens.size() > secondScreensSeen; }));
    CHECK(second.screens.size() > secondScreensSeen);
    CHECK(first.screens.size() == firstScreensSeen);
}

// ---------------------------------------------------------------------------
// Diagnostics. A daemon that cannot spawn a shell, or whose model refuses a
// tab, used to fail in complete silence: the caller got a null pointer and the
// user got a client that simply never opened a pane.

TEST_CASE("a failing PTY factory is reported", "[vthost][host][diagnostics]")
{
    auto capture = core::log::ScopedCapture {};

    auto source = core::net::testing::ScriptedBackend {};
    auto loop = core::net::EventLoop { source };
    auto host = SessionHost { loop,
                              [](vtbackend::PageSize, std::optional<vtpty::Process::ExecInfo> const&) {
                                  return std::unique_ptr<vtpty::Pty> {};
                              },
                              vtbackend::Settings {},
                              core::defaultEnvironment(),
                              vthost::SessionHostConfig { .pumping = vthost::SessionPumping::Manual } };

    CHECK(host.createTab() == nullptr);
    CHECK(host.sessionCount() == 0);
    CHECK(capture.contains("PTY factory failed"));
}

TEST_CASE("reaping an orphaned session after a model refusal is reported", "[vthost][host][diagnostics]")
{
    auto capture = core::log::ScopedCapture {};
    auto h = HostHarness {};
    REQUIRE(h.host.createTab() != nullptr);

    h.host.splitActivePane(vtworkspace::TabId { 4711 }, SplitState::Vertical, 0.5);

    CHECK(h.host.sessionCount() == 1); // the orphan really was reaped
    CHECK(capture.contains("model refused the split of tab 4711"));
    CHECK(capture.contains("reaping orphaned session"));
}

TEST_CASE("session spawn and exit are recorded once each", "[vthost][host][diagnostics]")
{
    auto capture = core::log::ScopedCapture { "vthost.session" };
    auto h = HostHarness {};
    auto* tab = h.host.createTab();
    REQUIRE(tab != nullptr);
    auto const session = tab->rootPane()->session();

    CHECK(capture.count("spawned") == 1);

    h.host.handleSessionExit(session);
    CHECK(capture.count("exited") == 1);
    CHECK(capture.contains(std::format("session {} exited", session.value)));
}

// Regression: `contour daemon` could not be stopped once it hosted a session. SIGTERM logged "shut
// down" and then hung forever, needing SIGKILL — a service manager would escalate, and Ctrl+C looked
// like a wedged process.
//
// HostedSession's destructor joins the pump thread, and the pump reads with NO timeout. A read on a
// closed device reports EAGAIN, which processInputOnce answers with "retry", so the pump re-entered
// an untimed read and parked forever with no further wakeup coming. The loop needed an exit
// condition, which is what the GUI's mainLoop has had all along (`_terminating`).
//
// The teardown is driven through a future so a regression REPORTS itself: the wait_for below fails
// with a diagnosable assertion naming this contract. The joining thread is still stuck afterwards,
// so the process then hangs at exit and ctest's timeout ends it — a failure either way, but one that
// says what broke instead of only which test never returned.
TEST_CASE("a hosted session with a parked pump can still be torn down", "[vthost][host]")
{
    auto source = core::net::testing::ScriptedBackend {};
    auto loop = core::net::EventLoop { source };
    auto* blocking = static_cast<BlockingPty*>(nullptr);
    auto host = std::make_unique<SessionHost>(
        loop,
        [&blocking](vtbackend::PageSize size, std::optional<vtpty::Process::ExecInfo> const&) {
            auto pty = std::make_unique<BlockingPty>(size);
            blocking = pty.get();
            return pty;
        },
        vtbackend::Settings {},
        core::defaultEnvironment(),
        vthost::SessionHostConfig { .pumping = vthost::SessionPumping::Threaded });

    REQUIRE(host->createTab() != nullptr);
    REQUIRE(blocking != nullptr);
    REQUIRE(host->sessionCount() == 1);

    // Let the pump reach its blocking read, so the teardown really has to interrupt a parked thread
    // rather than racing it before it ever got there.
    for (auto const _: std::views::iota(0, 200))
    {
        std::ignore = _;
        if (blocking->reads() > 0)
            break;
        std::this_thread::sleep_for(std::chrono::milliseconds { 5 });
    }
    CHECK(blocking->reads() > 0);

    auto teardown = std::async(std::launch::async, [&host] { host.reset(); });
    REQUIRE(teardown.wait_for(std::chrono::seconds { 10 }) == std::future_status::ready);
    teardown.get();
}

TEST_CASE("the loop thread leaves a resize's report to the session's reader", "[vthost][host]")
{
    // The loop thread takes a session's terminal lock to resize it, and with in-band resize reports on
    // (DECSET 2048) the resize queues a report for the application. Written where the lock is released, it
    // blocked the loop -- and every client of the daemon with it -- for as long as the application did not
    // read its input. The release wakes the session's reader instead, whose loop writes it.
    auto const source = core::net::makeDefaultBackend();
    auto loop = core::net::EventLoop { *source };
    auto* pty = static_cast<vthost::testing::StallingPty*>(nullptr);
    auto host =
        SessionHost { loop,
                      [&pty](vtbackend::PageSize size, std::optional<vtpty::Process::ExecInfo> const&) {
                          auto made = std::make_unique<vthost::testing::StallingPty>(size);
                          pty = made.get();
                          return made;
                      },
                      vtbackend::Settings {},
                      core::defaultEnvironment(),
                      vthost::SessionHostConfig { .pumping = vthost::SessionPumping::Threaded } };
    auto* tab = host.createTab();
    REQUIRE(tab != nullptr);
    REQUIRE(pty != nullptr);
    auto const session = tab->rootPane()->session();
    auto* terminal = host.terminal(session);
    REQUIRE(terminal != nullptr);
    terminal->writeToScreen("\033[?2048h");
    pty->stall();

    auto resize = std::async(std::launch::async, [&] {
        return host.applyPaneSize(
            session, vtpty::PageSize { .lines = vtpty::LineCount(20), .columns = vtpty::ColumnCount(60) });
    });
    auto const returned = resize.wait_for(std::chrono::seconds { 5 });
    pty->drain(); // whichever way it went, so that the resize can end
    CHECK(resize.get() == vthost::SizeChange::Applied);
    CHECK(returned == std::future_status::ready);
    CHECK(pty->wakeups() > 0);

    // The reader's loop sends it once woken, as a hosted session's pump flushes after every read.
    for ([[maybe_unused]] auto const tick: std::views::iota(0, 500))
    {
        if (pty->written().contains("\033[48;20;60;"))
            break;
        std::this_thread::sleep_for(std::chrono::milliseconds { 10 });
    }
    CHECK(pty->written().contains("\033[48;20;60;"));
}

#ifndef _WIN32
TEST_CASE("a hosted session whose program reads no input terminates at once", "[vthost][host]")
{
    // A client's input parks the session's pump in its PTY write once the program leaves the PTY's buffers
    // full. On a real PTY that write held the lock the PTY's close() and isClosed() take, so terminating the
    // session -- what the loop thread does as its pane closes, to be rid of the program -- waited for the
    // program forever, and every client of the daemon with it. The double the other cases stall never held
    // a lock across its parked write, so this one runs on a real PTY with no program reading it.
    auto ownedPty = std::make_unique<vtpty::UnixPty>(
        vtpty::PageSize { .lines = vtpty::LineCount(24), .columns = vtpty::ColumnCount(80) }, std::nullopt);
    auto* const pty = ownedPty.get();
    auto session = vthost::HostedSession { vtworkspace::SessionId { 1 },
                                           core::defaultEnvironment(),
                                           std::move(ownedPty),
                                           vtbackend::Settings {},
                                           vtbackend::HostIdentity {},
                                           vtbackend::TerminalClocks::system(),
                                           core::platform::NativeFileSystem::instance(),
                                           {},
                                           {},
                                           {},
                                           {},
                                           {},
                                           {} };
    session.start();
    // Lines, which the slave's line discipline keeps for a reader: a megabyte of them parks the pump.
    auto input = std::string {};
    for ([[maybe_unused]] auto const line: std::views::iota(0, 16 * 1024))
        input += std::string(63, 'a') + "\n";
    session.writeInput(input);
    // The pump takes the input at once; the wait makes sure its write is parked before the close.
    std::this_thread::sleep_for(std::chrono::milliseconds { 300 });

    auto terminating = std::async(std::launch::async, [&session] { session.terminate(); });
    auto const terminated = terminating.wait_for(std::chrono::seconds { 5 });
    // Where it waits, a reader on the slave side lets the parked write through, and it after that.
    if (terminated != std::future_status::ready)
        vtpty::testing::drainSlave(pty->handle(), [&terminating] {
            return terminating.wait_for(std::chrono::seconds { 0 }) == std::future_status::ready;
        });
    terminating.get();
    CHECK(terminated == std::future_status::ready);
}
#endif

TEST_CASE("a hosted session's process exit drops its working program status before it closes",
          "[vthost][host][programstatus]")
{
    // What the pump reads, then the exit or the close: the root is working, "lint" has failed.
    auto ownedPty = std::make_unique<ExitingPty>(
        vtbackend::PageSize { .lines = vtbackend::LineCount(5), .columns = vtbackend::ColumnCount(40) },
        "\033]7501;state=working\033\\\033]7501;state=error:id=lint\033\\");
    auto* const pty = ownedPty.get();
    auto closed = std::promise<std::vector<std::string>> {};
    auto* session = static_cast<vthost::HostedSession*>(nullptr);
    auto hosted = vthost::HostedSession { vtworkspace::SessionId { 1 },
                                          core::defaultEnvironment(),
                                          std::move(ownedPty),
                                          vtbackend::Settings {},
                                          vtbackend::HostIdentity {},
                                          vtbackend::TerminalClocks::system(),
                                          core::platform::NativeFileSystem::instance(),
                                          {},
                                          {},
                                          {},
                                          {},
                                          [&] {
                                              // What the records are as the close is announced.
                                              auto& terminal = session->terminal();
                                              auto const _ = std::lock_guard { terminal };
                                              auto ids = std::vector<std::string> {};
                                              for (auto const& record: terminal.programStatus().records())
                                                  ids.push_back(record.id);
                                              closed.set_value(std::move(ids));
                                          },
                                          {} };
    session = &hosted;
    hosted.start();

    // Both records are in before the program goes.
    auto const parsed = [&] {
        auto const _ = std::lock_guard { hosted.terminal() };
        return hosted.terminal().programStatus().records().size() == 2;
    };
    for ([[maybe_unused]] auto const tick: std::views::iota(0, 500))
    {
        if (parsed())
            break;
        std::this_thread::sleep_for(std::chrono::milliseconds { 5 });
    }
    REQUIRE(parsed());

    auto announced = closed.get_future();
    SECTION("the program exits by itself: the working record goes, the error stays")
    {
        pty->exitProcess();
        REQUIRE(announced.wait_for(std::chrono::seconds { 10 }) == std::future_status::ready);
        CHECK(announced.get() == std::vector<std::string> { "lint" });
    }
    SECTION("the host closes the session: nothing is dropped, the records go with it")
    {
        hosted.terminate();
        REQUIRE(announced.wait_for(std::chrono::seconds { 10 }) == std::future_status::ready);
        CHECK(announced.get() == std::vector<std::string> { "", "lint" });
    }
}

TEST_CASE("resizing an unknown pane is reported", "[vthost][host][diagnostics]")
{
    auto capture = core::log::ScopedCapture {};
    auto h = HostHarness {};

    h.host.applyPaneSize(vtworkspace::SessionId { 9999 },
                         vtpty::PageSize { vtpty::LineCount(24), vtpty::ColumnCount(80) });

    CHECK(capture.contains("applyPaneSize: unknown session 9999"));
}

TEST_CASE("an engine operation a client asks for changes the host's grid", "[vthost][host][operations]")
{
    auto h = HostHarness {};
    auto stream = StreamRecorder {};
    h.host.subscribeStream(&stream);
    auto* tab = h.host.createTab();
    REQUIRE(tab != nullptr);
    auto const session = tab->rootPane()->session();
    auto& terminal = *h.host.terminal(session);
    for (auto const row: std::views::iota(0, 40))
        terminal.writeToScreen(std::format("row {}\r\n", row));
    terminal.writeToScreen("\033]133;A\033\\$ prompt-here");
    REQUIRE(terminal.primaryScreen().historyLineCount() > vtbackend::LineCount(0));
    stream.screens.clear();

    auto const perform = [&](vthost::proto::EngineOperation operation,
                             std::string_view text = {},
                             vthost::proto::RowAddress row = {}) {
        return h.host.performOperation(vthost::proto::PerformOperation {
            .session = session.value, .operation = operation, .text = std::string { text }, .row = row });
    };

    SECTION("ClearToPrompt drops every row above the prompt, and the mirrors are told")
    {
        CHECK(perform(vthost::proto::EngineOperation::ClearToPrompt).has_value());
        CHECK(terminal.primaryScreen().historyLineCount() == vtbackend::LineCount(0));
        CHECK(
            terminal.primaryScreen().grid().lineText(vtbackend::LineOffset(0)).starts_with("$ prompt-here"));
        CHECK(std::ranges::count(stream.screens, session.value) == 1);
    }

    SECTION("ClearHistoryAndReset drops the scrollback")
    {
        CHECK(perform(vthost::proto::EngineOperation::ClearHistoryAndReset).has_value());
        CHECK(terminal.primaryScreen().historyLineCount() == vtbackend::LineCount(0));
        CHECK_FALSE(stream.screens.empty());
    }

    SECTION("SoftReset resets the modes and keeps the scrollback")
    {
        terminal.writeToScreen("\033[4h"); // IRM, which DECSTR resets
        REQUIRE(terminal.isModeEnabled(vtbackend::AnsiMode::Insert));
        CHECK(perform(vthost::proto::EngineOperation::SoftReset).has_value());
        CHECK_FALSE(terminal.isModeEnabled(vtbackend::AnsiMode::Insert));
        CHECK(terminal.primaryScreen().historyLineCount() > vtbackend::LineCount(0));
    }

    SECTION("WriteScreen parses its text as the application's output")
    {
        CHECK(
            perform(vthost::proto::EngineOperation::WriteScreen, "\r\033[2Kwritten-by-request").has_value());
        CHECK(terminal.primaryScreen()
                  .grid()
                  .lineText(terminal.primaryScreen().cursor().position.line)
                  .starts_with("written-by-request"));
    }

    SECTION("SetUserMark and ClearUserMark mark and unmark a row's logical line, in its generation")
    {
        auto const& grid = terminal.primaryScreen().grid();
        auto const promptRow = terminal.primaryScreen().cursor().position.line;
        auto const prompt = vthost::proto::RowAddress { .stableId = grid.stableLineIdOf(promptRow),
                                                        .generation = grid.stableIdGeneration() };
        auto const marked = [&] {
            return grid.lineAt(promptRow).isFlagEnabled(vtbackend::LineFlag::UserMark);
        };

        CHECK(perform(vthost::proto::EngineOperation::SetUserMark, {}, prompt).has_value());
        CHECK(marked());
        CHECK(std::ranges::count(stream.screens, session.value) == 1); // every mirror follows

        // An id taken in a generation gone since names no row the client meant.
        CHECK(perform(vthost::proto::EngineOperation::ClearUserMark,
                      {},
                      vthost::proto::RowAddress { .stableId = prompt.stableId,
                                                  .generation = prompt.generation + 1 })
                  .has_value());
        CHECK(marked());

        CHECK(perform(vthost::proto::EngineOperation::ClearUserMark, {}, prompt).has_value());
        CHECK_FALSE(marked());
    }

    SECTION("an unknown session or operation performs nothing, and says why")
    {
        auto const history = terminal.primaryScreen().historyLineCount();
        CHECK(h.host
                  .performOperation(vthost::proto::PerformOperation {
                      .session = 987654, .operation = vthost::proto::EngineOperation::ClearToPrompt })
                  .error()
              == vthost::OperationError::UnknownSession);
        CHECK(perform(static_cast<vthost::proto::EngineOperation>(200)).error()
              == vthost::OperationError::UnknownOperation);
        CHECK(terminal.primaryScreen().historyLineCount() == history);
        CHECK(stream.screens.empty());
    }

    h.host.unsubscribeStream(&stream);
}

TEST_CASE("a host normalizes settings it cannot serve sessions with", "[vthost][host]")
{
    // HostHarness hands over a bare vtbackend::Settings, whose maxHistoryLineCount is LineCount(0).
    // That is a valid terminal and a broken session HOST: with no history nothing that scrolled out
    // between two deltas can be named, so every scrolling batch is promoted to a full snapshot and no
    // client can ever be served scrollback. Enforced at the mechanism rather than only where the
    // daemon builds its configuration, because SessionHost has many callers.
    auto h = HostHarness {};
    REQUIRE(std::holds_alternative<vtbackend::LineCount>(h.host.settings().historyLimits.capacity));
    CHECK(unbox<int>(std::get<vtbackend::LineCount>(h.host.settings().historyLimits.capacity))
          == vthost::DefaultSessionHistoryLineCount);

    auto* tab = h.host.createTab();
    REQUIRE(tab != nullptr);
    auto* terminal = h.host.terminal(tab->rootPane()->session());
    REQUIRE(terminal != nullptr);

    // And the session really got it: rows pushed off the page land in grid history rather than
    // vanishing, which is the whole point of the invariant.
    auto const pageLines = unbox<int>(terminal->pageSize().lines);
    for (auto const line: std::views::iota(0, pageLines + 20))
        terminal->writeToScreen(std::format("line {}\r\n", line));

    CHECK(unbox<int>(terminal->primaryScreen().grid().historyLineCount()) >= 20);
}

TEST_CASE("a spawn request's settings reach only the session it creates", "[vthost][host]")
{
    auto h = HostHarness {};

    // The host's own settings back a plain creation ...
    auto* plain = h.host.createTab();
    REQUIRE(plain != nullptr);
    auto* plainTerminal = h.host.terminal(plain->rootPane()->session());
    REQUIRE(plainTerminal != nullptr);
    CHECK(plainTerminal->terminalId() == h.host.settings().terminalId);

    // ... while a request overrides them for its session alone.
    auto requested = h.host.settings();
    requested.terminalId = vtbackend::VTType::VT340;
    requested.historyLimits = vtbackend::HistoryLimits::plain(vtbackend::LineCount(4242));
    auto* custom = h.host.createTab(vthost::SessionSpawnRequest { .settings = requested });
    REQUIRE(custom != nullptr);
    auto* customTerminal = h.host.terminal(custom->rootPane()->session());
    REQUIRE(customTerminal != nullptr);
    CHECK(customTerminal->terminalId() == vtbackend::VTType::VT340);
    CHECK(unbox<int>(customTerminal->primaryScreen().grid().maxHistoryLineCount()) == 4242);

    // The earlier session is untouched -- that is what makes two clients on different profiles
    // conflict-free rather than a race over one shared terminal.
    CHECK(plainTerminal->terminalId() == h.host.settings().terminalId);
}

TEST_CASE("a spawn request cannot talk a session out of the host invariants", "[vthost][host]")
{
    auto h = HostHarness {};

    // The request can have travelled in from a client, so seedSession re-normalizes rather than
    // trusting it: zero scrollback is not something a peer gets to ask for.
    auto requested = vtbackend::Settings {};
    requested.historyLimits = vtbackend::HistoryLimits::plain(vtbackend::LineCount(0));
    auto* tab = h.host.createTab(vthost::SessionSpawnRequest { .settings = requested });
    REQUIRE(tab != nullptr);
    auto* terminal = h.host.terminal(tab->rootPane()->session());
    REQUIRE(terminal != nullptr);
    CHECK(unbox<int>(terminal->primaryScreen().grid().maxHistoryLineCount())
          == vthost::DefaultSessionHistoryLineCount);
}

TEST_CASE("a spawn request never dictates the session's page size", "[vthost][host]")
{
    // The host projects the pane tree onto the client area itself, so a client's own page size is
    // not a preference it gets to state here -- it would desynchronize the terminal from its PTY.
    auto h = HostHarness {};
    std::ignore =
        h.host.applyClientSize(&h.client, vtpty::PageSize { vtpty::LineCount(30), vtpty::ColumnCount(100) });

    auto requested = h.host.settings();
    requested.pageSize = vtbackend::PageSize { vtbackend::LineCount(5), vtbackend::ColumnCount(5) };
    auto* tab = h.host.createTab(vthost::SessionSpawnRequest { .settings = requested });
    REQUIRE(tab != nullptr);
    auto* terminal = h.host.terminal(tab->rootPane()->session());
    REQUIRE(terminal != nullptr);
    CHECK(terminal->pageSize() == h.host.pageSize());
}

TEST_CASE("createTab honours the window the request names", "[vthost][host]")
{
    // The daemon really does host several windows, and every client→server layout verb names its
    // target by a SESSION (window ids are minted per model, so a client's are not the host's).
    // createTab used its own `_window` unconditionally, so a "+" clicked in a second attach-mode
    // window opened its tab in the FIRST one — and the window clicked in gained nothing.
    auto h = HostHarness {};
    auto* second = h.host.createWindow();
    REQUIRE(second != nullptr);
    REQUIRE(second->id() != h.host.windowId());
    auto const inSecond = second->activeTab()->rootPane()->session();

    // Unnamed: the host's own window, as before.
    auto* defaulted = h.host.createTab();
    REQUIRE(defaulted != nullptr);
    CHECK(h.host.model().windowOfTab(defaulted->id()) == h.host.windowId());

    // Named by a session of the SECOND window: the tab lands there.
    auto* targeted = h.host.createTab({}, inSecond);
    REQUIRE(targeted != nullptr);
    CHECK(h.host.model().windowOfTab(targeted->id()) == second->id());

    // A session this host does not hold falls back to its own window rather than failing.
    auto* unknown = h.host.createTab({}, vtworkspace::SessionId { 99999 });
    REQUIRE(unknown != nullptr);
    CHECK(h.host.model().windowOfTab(unknown->id()) == h.host.windowId());
}

TEST_CASE("SessionHost realizes a single-tab startup layout into its one window", "[vthost][host][layout]")
{
    auto source = core::net::testing::ScriptedBackend {};
    auto loop = core::net::EventLoop { source };
    auto layout = vtworkspace::Layout { .tabs = { leafTab("nvim") } };

    auto host = SessionHost { loop,
                              [](vtbackend::PageSize size, std::optional<vtpty::Process::ExecInfo> const&) {
                                  return std::make_unique<vtpty::MockPty>(size);
                              },
                              vtbackend::Settings {},
                              core::defaultEnvironment(),
                              vthost::SessionHostConfig { .pumping = vthost::SessionPumping::Manual,
                                                          .sizePolicy = vthost::ClientSizePolicy::Latest,
                                                          .startupLayout = layout } };

    REQUIRE(host.sessionCount() == 1);
    auto* window = host.model().window(host.windowId());
    REQUIRE(window != nullptr);
    REQUIRE(window->tabCount() == 1);

    auto* tab = window->tabAt(0);
    REQUIRE(tab != nullptr);
    CHECK(tab->rootPane()->isLeaf());
    REQUIRE(host.terminal(tab->rootPane()->session()) != nullptr);
}

TEST_CASE("SessionHost realizes a multi-tab, multi-pane startup layout", "[vthost][host][layout]")
{
    auto source = core::net::testing::ScriptedBackend {};
    auto loop = core::net::EventLoop { source };

    auto splitTab = vtworkspace::LayoutTab {};
    splitTab.title = "servers";
    splitTab.root.orientation = SplitState::Vertical;
    auto left = vtworkspace::LayoutPane {};
    left.command = "npm";
    left.arguments = { "run", "dev" };
    left.ratio = 0.6;
    auto right = vtworkspace::LayoutPane {};
    right.command = "htop";
    splitTab.root.children = { left, right };

    auto layout = vtworkspace::Layout { .tabs = { leafTab("nvim"), splitTab } };

    auto host = SessionHost { loop,
                              [](vtbackend::PageSize size, std::optional<vtpty::Process::ExecInfo> const&) {
                                  return std::make_unique<vtpty::MockPty>(size);
                              },
                              vtbackend::Settings {},
                              core::defaultEnvironment(),
                              vthost::SessionHostConfig { .pumping = vthost::SessionPumping::Manual,
                                                          .sizePolicy = vthost::ClientSizePolicy::Latest,
                                                          .startupLayout = layout } };

    REQUIRE(host.sessionCount() == 3); // one leaf tab + two-pane split tab
    auto* window = host.model().window(host.windowId());
    REQUIRE(window != nullptr);
    REQUIRE(window->tabCount() == 2);

    auto* editorTab = window->tabAt(0);
    REQUIRE(editorTab != nullptr);
    CHECK(editorTab->rootPane()->isLeaf());

    auto* serversTab = window->tabAt(1);
    REQUIRE(serversTab != nullptr);
    REQUIRE_FALSE(serversTab->rootPane()->isLeaf());
    REQUIRE(serversTab->rootPane()->first() != nullptr);
    REQUIRE(serversTab->rootPane()->second() != nullptr);
    CHECK(host.terminal(serversTab->rootPane()->first()->session()) != nullptr);
    CHECK(host.terminal(serversTab->rootPane()->second()->session()) != nullptr);
}

TEST_CASE("SessionHost with an empty startup layout keeps today's single-default-tab behavior",
          "[vthost][host][layout]")
{
    // The default-parameter path: every existing call site (HostHarness included) that does not
    // pass a layout must be completely unaffected.
    auto source = core::net::testing::ScriptedBackend {};
    auto loop = core::net::EventLoop { source };

    auto host = SessionHost { loop,
                              [](vtbackend::PageSize size, std::optional<vtpty::Process::ExecInfo> const&) {
                                  return std::make_unique<vtpty::MockPty>(size);
                              },
                              vtbackend::Settings {},
                              core::defaultEnvironment(),
                              vthost::SessionHostConfig { .pumping = vthost::SessionPumping::Manual,
                                                          .sizePolicy = vthost::ClientSizePolicy::Latest } };

    CHECK(host.sessionCount() == 0);
    auto* window = host.model().window(host.windowId());
    REQUIRE(window != nullptr);
    CHECK(window->tabCount() == 0);
}

TEST_CASE("SessionHost honors a startup layout pane's command/arguments override, "
          "still ignoring its profile override",
          "[vthost][host][layout]")
{
    // Command/arguments/directory overrides ARE honored (mirrors AppSessionFactory::createPty's
    // program-overlay rule for the local GUI path); profile stays out of scope (the daemon has
    // no Config object to resolve an arbitrary named profile at startup) and is still ignored.
    auto source = core::net::testing::ScriptedBackend {};
    auto loop = core::net::EventLoop { source };

    auto tab = vtworkspace::LayoutTab {};
    tab.root.command = "nvim";
    tab.root.arguments = { "-R" };
    tab.root.profile = "some-other-profile"; // still not honored
    auto layout = vtworkspace::Layout { .tabs = { tab } };

    auto seen = std::optional<vtpty::Process::ExecInfo> {};
    auto host = SessionHost { loop,
                              [&](vtbackend::PageSize size,
                                  std::optional<vtpty::Process::ExecInfo> const& commandOverride) {
                                  seen = commandOverride;
                                  return std::make_unique<vtpty::MockPty>(size);
                              },
                              vtbackend::Settings {},
                              core::defaultEnvironment(),
                              vthost::SessionHostConfig { .pumping = vthost::SessionPumping::Manual,
                                                          .sizePolicy = vthost::ClientSizePolicy::Latest,
                                                          .startupLayout = layout } };

    REQUIRE(host.sessionCount() == 1);
    auto* window = host.model().window(host.windowId());
    REQUIRE(window != nullptr);
    REQUIRE(window->tabCount() == 1);
    REQUIRE(host.terminal(window->tabAt(0)->rootPane()->session()) != nullptr);

    REQUIRE(seen.has_value());
    CHECK(seen->program == "nvim");
    REQUIRE(seen->arguments.size() == 1);
    CHECK(seen->arguments.front() == "-R");
}

TEST_CASE("SessionHost honors a startup layout pane's directory-only override, "
          "leaving its command untouched",
          "[vthost][host][layout]")
{
    // A pane can set ONLY a directory override without naming a command. Mirrors
    // AppSessionFactory::createPty: an engaged override with an empty program must not wipe the
    // profile shell's default program/arguments — only the working directory changes.
    auto source = core::net::testing::ScriptedBackend {};
    auto loop = core::net::EventLoop { source };

    auto tab = vtworkspace::LayoutTab {};
    tab.root.directory = std::filesystem::path { "/tmp/project" };
    // command, arguments, and profile are left unset/empty
    auto layout = vtworkspace::Layout { .tabs = { tab } };

    auto seen = std::optional<vtpty::Process::ExecInfo> {};
    auto host = SessionHost { loop,
                              [&](vtbackend::PageSize size,
                                  std::optional<vtpty::Process::ExecInfo> const& commandOverride) {
                                  seen = commandOverride;
                                  return std::make_unique<vtpty::MockPty>(size);
                              },
                              vtbackend::Settings {},
                              core::defaultEnvironment(),
                              vthost::SessionHostConfig { .pumping = vthost::SessionPumping::Manual,
                                                          .sizePolicy = vthost::ClientSizePolicy::Latest,
                                                          .startupLayout = layout } };

    REQUIRE(host.sessionCount() == 1);
    auto* window = host.model().window(host.windowId());
    REQUIRE(window != nullptr);
    REQUIRE(window->tabCount() == 1);
    REQUIRE(host.terminal(window->tabAt(0)->rootPane()->session()) != nullptr);

    REQUIRE(seen.has_value());
    CHECK(seen->program.empty()); // no command named: program/arguments stay untouched
    CHECK(seen->workingDirectory == std::filesystem::path { "/tmp/project" });
}

TEST_CASE("SessionHost engages no override for a startup layout pane's arguments without a command",
          "[vthost][host][layout]")
{
    // Arguments name what a command is run with, so a pane carrying them without one is malformed.
    // Such a pane must not engage an override at all: an empty-program ExecInfo would tell the
    // factory this session overrides the shell while carrying nothing to run. The arguments are
    // dropped, with a log line rather than in silence.
    auto source = core::net::testing::ScriptedBackend {};
    auto loop = core::net::EventLoop { source };

    auto tab = vtworkspace::LayoutTab {};
    tab.root.arguments = { "--flag", "value" };
    // command, directory and profile are left unset/empty
    auto layout = vtworkspace::Layout { .tabs = { tab } };

    auto seen = std::optional<vtpty::Process::ExecInfo> {};
    auto host = SessionHost { loop,
                              [&](vtbackend::PageSize size,
                                  std::optional<vtpty::Process::ExecInfo> const& commandOverride) {
                                  seen = commandOverride;
                                  return std::make_unique<vtpty::MockPty>(size);
                              },
                              vtbackend::Settings {},
                              core::defaultEnvironment(),
                              vthost::SessionHostConfig { .pumping = vthost::SessionPumping::Manual,
                                                          .sizePolicy = vthost::ClientSizePolicy::Latest,
                                                          .startupLayout = layout } };

    REQUIRE(host.sessionCount() == 1);

    // The pane still spawns the daemon's configured shell: the override is always engaged now (it carries
    // the pane's ids), but names no program, arguments or directory, which the factory reads as "the
    // configured one".
    REQUIRE(seen.has_value());
    CHECK(seen->program.empty());
    CHECK(seen->arguments.empty());
    CHECK(seen->workingDirectory.empty());
}

TEST_CASE("SessionHost still passes through an engaged-but-empty command with its arguments",
          "[vthost][host][layout]")
{
    // A pane whose command is an empty string is engaged -- leaf.command holds a value, just an empty one --
    // unlike the unset-command case above. SessionHost still builds and forwards the override here: it is
    // vtpty::Process::applyCommandOverride (the shared overlay both the daemon and the local-GUI
    // path use) that declines to overlay an empty program, so the arguments end up dropped
    // downstream rather than here. This pane is just as malformed as the unset-command one and
    // must log the same "ignoring pane arguments" warning (see SessionHost.cpp) -- this test
    // pins the override that reaches the factory; the warning itself has no harness to assert on.
    auto source = core::net::testing::ScriptedBackend {};
    auto loop = core::net::EventLoop { source };

    auto tab = vtworkspace::LayoutTab {};
    tab.root.command = std::string {};
    tab.root.arguments = { "--flag", "value" };
    auto layout = vtworkspace::Layout { .tabs = { tab } };

    auto seen = std::optional<vtpty::Process::ExecInfo> {};
    auto host = SessionHost { loop,
                              [&](vtbackend::PageSize size,
                                  std::optional<vtpty::Process::ExecInfo> const& commandOverride) {
                                  seen = commandOverride;
                                  return std::make_unique<vtpty::MockPty>(size);
                              },
                              vtbackend::Settings {},
                              core::defaultEnvironment(),
                              vthost::SessionHostConfig { .pumping = vthost::SessionPumping::Manual,
                                                          .sizePolicy = vthost::ClientSizePolicy::Latest,
                                                          .startupLayout = layout } };

    REQUIRE(host.sessionCount() == 1);
    REQUIRE(seen.has_value());
    CHECK(seen->program.empty());
    CHECK(seen->arguments == std::vector<std::string> { "--flag", "value" });
}

TEST_CASE("SessionHost falls back to an empty window when every startup-layout seed is refused",
          "[vthost][host][layout]")
{
    // A PTY factory that always fails (e.g. resource exhaustion at daemon startup) must not
    // leave a half-built or crashed host: realization produces zero tabs, and the window stays
    // empty -- NativeSession::completeHandshake's existing first-attach fallback is unaffected by
    // this test (it lives above SessionHost) but this confirms SessionHost itself degrades
    // gracefully.
    auto source = core::net::testing::ScriptedBackend {};
    auto loop = core::net::EventLoop { source };
    auto layout = vtworkspace::Layout { .tabs = { leafTab("nvim"), leafTab("htop") } };

    auto host = SessionHost { loop,
                              [](vtbackend::PageSize, std::optional<vtpty::Process::ExecInfo> const&)
                                  -> std::unique_ptr<vtpty::Pty> { return nullptr; },
                              vtbackend::Settings {},
                              core::defaultEnvironment(),
                              vthost::SessionHostConfig { .pumping = vthost::SessionPumping::Manual,
                                                          .sizePolicy = vthost::ClientSizePolicy::Latest,
                                                          .startupLayout = layout } };

    CHECK(host.sessionCount() == 0);
    auto* window = host.model().window(host.windowId());
    REQUIRE(window != nullptr);
    CHECK(window->tabCount() == 0);
}

TEST_CASE("a hosted session decides a command directory's locality against the host's identity",
          "[vthost][host][blocks]")
{
    // A record's locality is decided where the command starts -- on the host -- and travels as
    // decided, so a host that does not know its own name reads every OSC 7 naming it as another
    // machine's.
    auto const localityUnder = [](vtbackend::HostIdentity identity, std::string_view reportedDirectory) {
        auto h = HostHarness { vthost::ClientSizePolicy::Latest, std::move(identity) };
        auto* tab = h.host.createTab();
        REQUIRE(tab != nullptr);
        auto* terminal = h.host.terminal(tab->rootPane()->session());
        REQUIRE(terminal != nullptr);

        terminal->writeToScreen(reportedDirectory);
        terminal->writeToScreen(vtbackend::test::typedAtPrompt("make") + "\033]133;C\033\\");
        auto const* record = terminal->commandBlocks().current();
        REQUIRE(record != nullptr);
        return record->workingDirectory.locality;
    };
    auto const zeta =
        vtbackend::HostIdentity { .machineId = "3deb5353d3ba43d08201c136a47ead7b", .hostname = "zeta" };

    SECTION("by its host name, from an OSC 7 URL")
    {
        constexpr auto Osc7 = std::string_view { "\033]7;file://zeta/home/user\033\\" };
        CHECK(localityUnder(zeta, Osc7) == vtbackend::ContextLocality::Local);
        CHECK(localityUnder(vtbackend::HostIdentity {}, Osc7) == vtbackend::ContextLocality::Foreign);
    }

    SECTION("by its machine id, from an OSC 3008 context")
    {
        constexpr auto Context = std::string_view {
            "\033]3008;start=shell-1;type=shell;machineid=3deb5353d3ba43d08201c136a47ead7b;cwd=/srv\033\\"
        };
        CHECK(localityUnder(zeta, Context) == vtbackend::ContextLocality::Local);
        // A host with no machine id of its own is not the machine a context's id names
        // (vtbackend::sameMachine): a daemon on Windows, say, under a shell in WSL.
        CHECK(localityUnder(vtbackend::HostIdentity {}, Context) == vtbackend::ContextLocality::Foreign);
    }
}

TEST_CASE("a hosted session's terminal reads the clocks the host was given", "[vthost][host][clocks]")
{
    // Record times and the session epoch travel to every client, so a test of the daemon must be able to
    // drive them as it drives a local terminal's.
    auto const start = std::chrono::sys_days { std::chrono::year { 2031 } / std::chrono::March / 4 }
                       + std::chrono::hours { 5 } + std::chrono::milliseconds { 250 };
    auto steadyClock = core::platform::ManualClock {};
    auto wallClock = core::platform::ManualWallClock { start };
    core::net::testing::ScriptedBackend source;
    core::net::EventLoop loop { source };
    SessionHost host { loop,
                       [](vtbackend::PageSize size, std::optional<vtpty::Process::ExecInfo> const&) {
                           return std::make_unique<vtpty::MockPty>(size);
                       },
                       vtbackend::Settings {},
                       core::defaultEnvironment(),
                       vthost::SessionHostConfig { .pumping = vthost::SessionPumping::Manual,
                                                   .clocks = vtbackend::TerminalClocks {
                                                       .steady = steadyClock,
                                                       .wall = wallClock,
                                                       .localTime = &vtbackend::fixedOffsetLocalTime<> } } };

    auto const* tab = host.createTab();
    REQUIRE(tab != nullptr);
    auto const* terminal = host.terminal(tab->rootPane()->session());
    REQUIRE(terminal != nullptr);

    CHECK(terminal->sessionEpoch() == std::chrono::floor<std::chrono::seconds>(start));
    wallClock.advance(std::chrono::minutes { 3 });
    CHECK(terminal->wallTimeNow() == start + std::chrono::minutes { 3 });
}

TEST_CASE("SessionHost.extension.foregroundPoll", "[vthost][host][extensions]")
{
    // The hosted programs are asked who runs in the foreground once a second -- and only while an extension
    // follows it: a host whose extensions do not wakes no loop for it. Time is the loop's injected clock,
    // advanced by the case, and the loop turned a turn at a time.
    auto clock = core::platform::ManualClock {};
    auto const source = core::net::makeDefaultBackend();
    auto loop = core::net::EventLoop { *source, clock };
    auto const hosting = std::make_shared<vthost::testing::FakeExtensionHosting>();
    auto host = SessionHost { loop,
                              [](vtbackend::PageSize size, std::optional<vtpty::Process::ExecInfo> const&) {
                                  return std::make_unique<vtpty::MockPty>(size);
                              },
                              vtbackend::Settings {},
                              core::defaultEnvironment(),
                              vthost::SessionHostConfig { .pumping = vthost::SessionPumping::Manual,
                                                          .extensions = hosting } };
    REQUIRE(host.createTab() != nullptr);
    auto& extensions = *hosting->sessions.front();
    CHECK_FALSE(host.pollsForegroundProcesses());

    // What a build that landed with an extension following the foreground process does.
    extensions.fake.subscribe(
        vtbackend::StatusLineEventKinds { vtbackend::StatusLineEventKind::ForegroundProcess });
    extensions.replaced();
    CHECK(host.pollsForegroundProcesses());

    // A tick while it still does re-arms.
    clock.advance(std::chrono::milliseconds { 1100 });
    for ([[maybe_unused]] auto const turn: std::views::iota(0, 3))
        std::ignore = loop.runOnce(std::chrono::milliseconds { 0 });
    CHECK(host.pollsForegroundProcesses());

    // And after one that no longer does, the next tick does not.
    extensions.fake.subscribe(vtbackend::StatusLineEventKinds {});
    extensions.replaced();
    clock.advance(std::chrono::milliseconds { 1100 });
    for ([[maybe_unused]] auto const turn: std::views::iota(0, 3))
        std::ignore = loop.runOnce(std::chrono::milliseconds { 0 });
    CHECK_FALSE(host.pollsForegroundProcesses());
}

TEST_CASE("SessionHost.extension.repaintsOnlyForClients", "[vthost][host][extensions]")
{
    // The indicator row a session's extensions draw is repainted for a client to be sent: not while none is
    // attached, however much output arrives, and a client attaching is handed it as it is now.
    struct Watcher final: vthost::SessionStreamEvents
    {
        [[nodiscard]] bool mirrorsScreens() const noexcept override { return false; }
    };

    auto const source = core::net::makeDefaultBackend();
    auto loop = core::net::EventLoop { *source };
    auto const hosting = std::make_shared<vthost::testing::FakeExtensionHosting>();
    hosting->prepare = [](vtbackend::FakeStatusLineExtensions& fake) {
        fake.takeOver("plasma");
        fake.publish("plasma", vthost::testing::frameOf("FIRST"));
    };
    auto host = SessionHost { loop,
                              [](vtbackend::PageSize size, std::optional<vtpty::Process::ExecInfo> const&) {
                                  return std::make_unique<vtpty::MockPty>(size);
                              },
                              vtbackend::Settings {},
                              core::defaultEnvironment(),
                              vthost::SessionHostConfig { .pumping = vthost::SessionPumping::Manual,
                                                          .extensions = hosting } };
    auto const* tab = host.createTab();
    REQUIRE(tab != nullptr);
    auto* terminal = host.terminal(tab->rootPane()->session());
    REQUIRE(terminal != nullptr);
    auto& extensions = *hosting->sessions.front();
    auto const turn = [&] {
        for ([[maybe_unused]] auto const _: std::views::iota(0, 4))
            std::ignore = loop.runOnce(std::chrono::milliseconds { 0 });
    };
    auto const row = [&] {
        auto const _ = std::lock_guard { *terminal };
        return terminal->indicatorStatusLineDisplay().grid().lineAt(vtbackend::LineOffset(0)).toUtf8Trimmed();
    };
    turn();
    REQUIRE(row() == "FIRST");

    // Detached -- the daemon's own observers show no screen: a new frame, and output, repaint nothing.
    auto watcher = Watcher {};
    host.subscribeStream(&watcher);
    extensions.publish("plasma", vthost::testing::frameOf("SECOND", 2));
    for (auto const i: std::views::iota(0, 5))
        terminal->writeToScreen(std::format("line {}\r\n", i));
    turn();
    CHECK(row() == "FIRST");

    // A client attaching is handed the row as it is now ...
    auto mirror = vthost::SessionStreamEvents {};
    host.subscribeStream(&mirror);
    CHECK(row() == "SECOND");

    // ... and, while it is attached, the row follows the frames.
    extensions.publish("plasma", vthost::testing::frameOf("THIRD", 3));
    terminal->writeToScreen("more\r\n");
    turn();
    CHECK(row() == "THIRD");

    host.unsubscribeStream(&mirror);
    host.unsubscribeStream(&watcher);
}

TEST_CASE("SessionHost.extension.hostRowCarriesTheGitBranch", "[vthost][host][extensions][git]")
{
    // A row an extension draws is the host's, {Git} included: the client mirrors it, and for a client on
    // another machine it is the only place the branch can come from. A branch the host's lookup finds after
    // the row was drawn is repainted and sent, as an extension's frame is.
    struct Mirror final: vthost::SessionStreamEvents
    {
        int updates = 0;
        void sessionScreenUpdated(vtworkspace::SessionId /*session*/) override { ++updates; }
    };

    auto files = core::platform::testing::InMemoryFileSystem {};
    files.addFile("/r/.git/HEAD", "ref: refs/heads/main\n");
    REQUIRE(files.createDirectories("/r/src"));
    auto git = vtbackend::test::InMemoryGitFileSystem { files };
    auto lookups = core::async::testing::ManualExecutor {};

    auto const source = core::net::makeDefaultBackend();
    auto loop = core::net::EventLoop { *source };
    auto const hosting = std::make_shared<vthost::testing::FakeExtensionHosting>();
    hosting->prepare = [](vtbackend::FakeStatusLineExtensions& fake) {
        fake.publish("ext", vthost::testing::frameOf("E"));
    };
    auto settings = vtbackend::Settings {};
    settings.statusDisplayType = vtbackend::StatusDisplayType::Indicator;
    settings.indicatorStatusLine.left = "{Extension:ext} {Git}";
    settings.indicatorStatusLine.middle.clear();
    settings.indicatorStatusLine.right.clear();
    auto host =
        SessionHost { loop,
                      [](vtbackend::PageSize size, std::optional<vtpty::Process::ExecInfo> const&) {
                          return std::make_unique<vtpty::MockPty>(size);
                      },
                      std::move(settings),
                      core::defaultEnvironment(),
                      vthost::SessionHostConfig {
                          .pumping = vthost::SessionPumping::Manual,
                          .fileSystem = files,
                          .git = vtbackend::GitHeadServices { .executor = lookups, .fileSystem = git },
                          .extensions = hosting,
                      } };
    auto const* tab = host.createTab();
    REQUIRE(tab != nullptr);
    auto* terminal = host.terminal(tab->rootPane()->session());
    REQUIRE(terminal != nullptr);
    auto mirror = Mirror {};
    host.subscribeStream(&mirror);

    auto const turn = [&] {
        for ([[maybe_unused]] auto const _: std::views::iota(0, 4))
            std::ignore = loop.runOnce(std::chrono::milliseconds { 0 });
    };
    auto const row = [&] {
        auto const _ = std::lock_guard { *terminal };
        return terminal->indicatorStatusLineDisplay().grid().lineAt(vtbackend::LineOffset(0)).toUtf8Trimmed();
    };
    // The lookups the repaints asked for, run as the probe thread would, and what their answers ask of the
    // loop.
    auto const settle = [&] {
        turn();
        lookups.drain();
        turn();
    };

    terminal->writeToScreen("\033]7;file:///r/src\033\\");
    settle();
    CHECK(row() == "E main");

    // A branch switch the shell's next directory report reveals reaches the client too.
    REQUIRE(files.writeFile("/r/.git/HEAD", "ref: refs/heads/next\n"));
    terminal->writeToScreen("\033]7;file:///r/src\033\\");
    turn();
    auto const before = mirror.updates;
    CHECK(row() == "E main");
    lookups.drain();
    turn();
    CHECK(row() == "E next");
    CHECK(mirror.updates > before);

    host.unsubscribeStream(&mirror);
}

TEST_CASE("SessionHost.extension.foregroundPollFollowsLoadedSubscriptions", "[vthost][host][extensions]")
{
    // What an extension follows is known only once it has loaded on its lane -- after its build was swapped
    // in and the replace was told. The frame it publishes then is what arms the poll, and one published after
    // it stopped following disarms it at the next tick.
    auto clock = core::platform::ManualClock {};
    auto const source = core::net::makeDefaultBackend();
    auto loop = core::net::EventLoop { *source, clock };
    auto const hosting = std::make_shared<vthost::testing::FakeExtensionHosting>();
    auto host = SessionHost { loop,
                              [](vtbackend::PageSize size, std::optional<vtpty::Process::ExecInfo> const&) {
                                  return std::make_unique<vtpty::MockPty>(size);
                              },
                              vtbackend::Settings {},
                              core::defaultEnvironment(),
                              vthost::SessionHostConfig { .pumping = vthost::SessionPumping::Manual,
                                                          .extensions = hosting } };
    REQUIRE(host.createTab() != nullptr);
    auto& extensions = *hosting->sessions.front();
    auto const turn = [&] {
        for ([[maybe_unused]] auto const _: std::views::iota(0, 3))
            std::ignore = loop.runOnce(std::chrono::milliseconds { 0 });
    };

    // The build lands: replaced while nothing is loaded, so nothing is followed yet.
    extensions.replaced();
    turn();
    REQUIRE_FALSE(host.pollsForegroundProcesses());

    // Loaded, it follows the foreground process, and publishes its first frame.
    extensions.fake.subscribe(
        vtbackend::StatusLineEventKinds { vtbackend::StatusLineEventKind::ForegroundProcess });
    extensions.publish("echo", vthost::testing::frameOf("loaded"));
    turn();
    CHECK(host.pollsForegroundProcesses());

    // A frame published after it stopped following: the next tick does not re-arm.
    extensions.fake.subscribe(vtbackend::StatusLineEventKinds {});
    extensions.publish("echo", vthost::testing::frameOf("idle", 2));
    clock.advance(std::chrono::milliseconds { 1100 });
    turn();
    CHECK_FALSE(host.pollsForegroundProcesses());
}

TEST_CASE("SessionHost.extension.foregroundPollAfterReplace", "[vthost][host][extensions]")
{
    // A replace tells of extensions that have not loaded yet: what they follow is not what the ones before
    // them did, so the next frame that says they follow the foreground process arms the poll again.
    auto clock = core::platform::ManualClock {};
    auto const source = core::net::makeDefaultBackend();
    auto loop = core::net::EventLoop { *source, clock };
    auto const hosting = std::make_shared<vthost::testing::FakeExtensionHosting>();
    auto host = SessionHost { loop,
                              [](vtbackend::PageSize size, std::optional<vtpty::Process::ExecInfo> const&) {
                                  return std::make_unique<vtpty::MockPty>(size);
                              },
                              vtbackend::Settings {},
                              core::defaultEnvironment(),
                              vthost::SessionHostConfig { .pumping = vthost::SessionPumping::Manual,
                                                          .extensions = hosting } };
    REQUIRE(host.createTab() != nullptr);
    auto& extensions = *hosting->sessions.front();
    auto const turn = [&] {
        clock.advance(std::chrono::milliseconds { 1100 });
        for ([[maybe_unused]] auto const _: std::views::iota(0, 3))
            std::ignore = loop.runOnce(std::chrono::milliseconds { 0 });
    };
    auto const following =
        vtbackend::StatusLineEventKinds { vtbackend::StatusLineEventKind::ForegroundProcess };
    // A set that has loaded and follows the foreground process.
    auto const loadFollowing = [&](int generation) {
        extensions.fake.subscribe(following);
        extensions.publish("echo", vthost::testing::frameOf("loaded", static_cast<uint64_t>(generation)));
        turn();
    };
    // A replace: the new set has not loaded, so it follows nothing yet.
    auto const replaceUnloaded = [&] {
        extensions.fake.subscribe(vtbackend::StatusLineEventKinds {});
        extensions.replaced();
        turn();
    };

    extensions.replaced();
    loadFollowing(1);
    REQUIRE(host.pollsForegroundProcesses());

    SECTION("a follower replaced by another follower")
    {
        replaceUnloaded();
        CHECK_FALSE(host.pollsForegroundProcesses());
        loadFollowing(2);
        CHECK(host.pollsForegroundProcesses());
    }

    SECTION("a follower replaced by none, then by a follower")
    {
        replaceUnloaded();
        CHECK_FALSE(host.pollsForegroundProcesses());
        replaceUnloaded();
        loadFollowing(2);
        CHECK(host.pollsForegroundProcesses());
    }
}

TEST_CASE("SessionHost.extension.questionOutlivesTheAskingSession", "[vthost][host][extensions]")
{
    // Two panes run the same extension, and both ask; the question is asked once, in the first. That pane
    // exits before anyone answers: a runner asks only once, so the question must not go with it. It moves to
    // the other pane whose extensions run, and is told again there.
    struct Asked final: vthost::SessionStreamEvents
    {
        void sessionPermissionRequested(vtworkspace::SessionId session,
                                        vthost::proto::PermissionRequest const& request) override
        {
            asked.emplace_back(session.value, request.session);
        }
        std::vector<std::pair<uint64_t, uint64_t>> asked; ///< The session told of, and the one it names.
    };

    auto const source = core::net::makeDefaultBackend();
    auto loop = core::net::EventLoop { *source };
    auto const hosting = std::make_shared<vthost::testing::FakeExtensionHosting>();
    auto host = SessionHost { loop,
                              [](vtbackend::PageSize size, std::optional<vtpty::Process::ExecInfo> const&) {
                                  return std::make_unique<vtpty::MockPty>(size);
                              },
                              vtbackend::Settings {},
                              core::defaultEnvironment(),
                              vthost::SessionHostConfig { .pumping = vthost::SessionPumping::Manual,
                                                          .extensions = hosting } };
    auto const* first = host.createTab();
    REQUIRE(first != nullptr);
    auto const firstSession = first->rootPane()->session();
    auto const* second = host.createTab();
    REQUIRE(second != nullptr);
    auto const secondSession = second->rootPane()->session();
    REQUIRE(hosting->sessions.size() == 2);
    auto observer = Asked {};
    host.subscribeStream(&observer);
    auto const turn = [&] {
        for ([[maybe_unused]] auto const _: std::views::iota(0, 3))
            std::ignore = loop.runOnce(std::chrono::milliseconds { 0 });
    };

    auto const question = vthost::proto::PermissionRequest { .session = 0,
                                                             .hostId = "git@/ext/git.wasm",
                                                             .name = "git",
                                                             .version = 1,
                                                             .digest = "d1",
                                                             .commands = {} };
    hosting->sessions[0]->askPermission(question);
    hosting->sessions[1]->askPermission(question);
    turn();
    REQUIRE(host.pendingPermissionRequests().size() == 1);
    REQUIRE(host.pendingPermissionRequests().front().session == firstSession.value);

    SECTION("the asking pane exits: another pane with extensions inherits the question")
    {
        host.handleSessionExit(firstSession);
        REQUIRE(host.pendingPermissionRequests().size() == 1);
        CHECK(host.pendingPermissionRequests().front().session == secondSession.value);
        REQUIRE_FALSE(observer.asked.empty());
        CHECK(observer.asked.back() == std::pair { secondSession.value, secondSession.value });
    }

    SECTION("no pane with extensions remains: the question goes")
    {
        host.handleSessionExit(firstSession);
        host.handleSessionExit(secondSession);
        CHECK(host.pendingPermissionRequests().empty());
    }
    host.unsubscribeStream(&observer);
}

TEST_CASE("SessionHost.extension.replacedHearTheForegroundProcess", "[vthost][host][extensions]")
{
    // The poll tells each program once, so the extensions a reload brings, while the same program keeps
    // running, hear it from the terminal: what extensionsReplaced() has it replay.
    using vtbackend::StatusLineEvent;
    using vtbackend::StatusLineEvents::ForegroundProcessChanged;
    // A real backend: a replace that leaves an extension following the foreground process arms the poll.
    auto clock = core::platform::ManualClock {};
    auto const source = core::net::makeDefaultBackend();
    auto loop = core::net::EventLoop { *source, clock };
    auto const hosting = std::make_shared<vthost::testing::FakeExtensionHosting>();
    auto host = SessionHost { loop,
                              [](vtbackend::PageSize size, std::optional<vtpty::Process::ExecInfo> const&) {
                                  return std::make_unique<vtpty::MockPty>(size);
                              },
                              vtbackend::Settings {},
                              core::defaultEnvironment(),
                              vthost::SessionHostConfig { .pumping = vthost::SessionPumping::Manual,
                                                          .extensions = hosting } };
    auto const* const tab = host.createTab();
    REQUIRE(tab != nullptr);
    auto& extensions = *hosting->sessions.front();
    auto* const terminal = host.terminal(tab->rootPane()->session());
    REQUIRE(terminal != nullptr);
    extensions.fake.subscribe(
        vtbackend::StatusLineEventKinds { vtbackend::StatusLineEventKind::ForegroundProcess });
    extensions.replaced();
    {
        // What the poll does once it saw vim: tells it, under the lock, and never again while it runs.
        auto const guard = std::lock_guard { *terminal };
        terminal->foregroundProcessChanged("vim");
    }
    auto const vim = StatusLineEvent { ForegroundProcessChanged { .name = "vim" } };
    REQUIRE(std::ranges::count(extensions.fake.posted(), vim) == 1);

    extensions.replaced();
    CHECK(std::ranges::count(extensions.fake.posted(), vim) == 2);
}

#ifdef CONTOUR_EXTENSIONS

namespace
{

/// A command capture that runs nothing: it counts the runs, and holds each until the daemon's command
/// shutdown is requested or the run's own timeout passes -- a command that would run for its timeout. A
/// run's own stop is not heeded: the case pins that the daemon's shutdown alone ends it.
class HoldingCapture final: public vtpty::CommandCapture
{
  public:
    explicit HoldingCapture(core::async::StopToken shutdown): _shutdown { std::move(shutdown) } {}

    [[nodiscard]] std::expected<std::string, vtpty::CommandCaptureError> run(
        std::string_view /*command*/,
        std::chrono::milliseconds timeout,
        core::async::StopToken /*stop*/) override
    {
        runs.fetch_add(1);
        lastTimeout.store(timeout);
        auto const until = std::chrono::steady_clock::now() + timeout;
        while (!_shutdown.stop_requested() && std::chrono::steady_clock::now() < until)
            std::this_thread::yield();
        return std::unexpected(
            vtpty::CommandCaptureError { .kind = vtpty::CommandCaptureError::Kind::Cancelled, .cause = {} });
    }

    std::atomic<int> runs = 0;
    std::atomic<std::chrono::milliseconds> lastTimeout {};

  private:
    core::async::StopToken _shutdown;
};

/// What a stream observer saw of a session's status-line extensions, and when.
struct ExtensionTimeline final: vthost::SessionStreamEvents
{
    std::function<bool()> rowShowsPending;
    std::optional<std::chrono::steady_clock::time_point> asked;
    std::optional<std::chrono::steady_clock::time_point> shown;

    void sessionPermissionRequested(vtworkspace::SessionId, vthost::proto::PermissionRequest const&) override
    {
        if (!asked)
            asked = std::chrono::steady_clock::now();
    }
    void sessionScreenUpdated(vtworkspace::SessionId) override
    {
        if (!shown && rowShowsPending && rowShowsPending())
            shown = std::chrono::steady_clock::now();
    }
};

/// A daemon's extensions, on the real runtime, and a host running one session that shows them in its
/// indicator row. Declared in the order they must go: the host first, then the extensions it owns, before
/// the loop they post to.
struct DaemonExtensionsHarness
{
    /// @param module The extension's module bytes.
    /// @param permission What the user allows its commands.
    /// @param config The extension's settings.
    /// @param extensionLimits What the extension may use.
    DaemonExtensionsHarness(std::string const& module,
                            vtextension::CommandPermission permission,
                            vtextension::ConfigMap config = {},
                            vtextension::ExtensionLimits extensionLimits = {}):
        limits { extensionLimits }
    {
        REQUIRE(fileSystem.files.writeFile(modulePath, module).has_value());
        auto made = std::make_shared<vthost::DaemonExtensions>(
            vthost::DaemonExtensionsConfig {
                .declared = vtextension::DeclaredExtensions { .extensions = { vtextension::DeclaredExtension {
                                                                  .name = "spawner",
                                                                  .path = modulePath,
                                                                  .config = std::move(config),
                                                                  .runCommand = permission } },
                                                              .takeOver = std::nullopt },
                .runtime =
                    vtextension::ExtensionRuntimeConfig { .grantsFile = "/config/extension-grants.daemon.txt",
                                                          .bundledDirectories = {},
                                                          .workerCount = 1,
                                                          .limits = limits } },
            vthost::DaemonExtensionServices {
                .runtime = vtextension::ExtensionRuntimeServices { .backend = *wasmBackend,
                                                                   .capture = capture,
                                                                   .wallClock = wallClock,
                                                                   .localTime = localTime,
                                                                   .environment = core::defaultEnvironment(),
                                                                   .fileSystem = fileSystem },
                .commandShutdown = commandShutdown });
        extensions = made.get();
        auto settings = vtbackend::Settings {};
        settings.indicatorStatusLine.left = "[{Extension:spawner}]";
        settings.indicatorStatusLine.middle.clear();
        settings.indicatorStatusLine.right.clear();
        // The host is the extensions' only owner, as in a daemon: they go with it.
        host = std::make_unique<SessionHost>(
            loop,
            [](vtbackend::PageSize size, std::optional<vtpty::Process::ExecInfo> const&) {
                return std::make_unique<vtpty::MockPty>(size);
            },
            std::move(settings),
            core::defaultEnvironment(),
            vthost::SessionHostConfig { .pumping = vthost::SessionPumping::Manual,
                                        .extensions = std::move(made) });
    }

    /// @return The session's indicator row.
    [[nodiscard]] std::string indicatorText(vtworkspace::SessionId session) const
    {
        auto* const terminal = host->terminal(session);
        auto const guard = std::lock_guard { *terminal };
        return terminal->indicatorStatusLineDisplay().grid().lineAt(vtbackend::LineOffset(0)).toUtf8Trimmed();
    }

    /// Turns the loop until @p done holds, or for @p bound at most.
    void turnUntil(std::function<bool()> const& done,
                   std::chrono::milliseconds bound = std::chrono::seconds { 20 })
    {
        auto const deadline = std::chrono::steady_clock::now() + bound;
        loop.blockOn(core::net::pollUntil(
            &loop, [&] { return done() || std::chrono::steady_clock::now() > deadline; }));
    }

    /// What the extensions' engines are created from; first, so that it outlives them.
    std::unique_ptr<vtextension::wasm::IWasmBackend> wasmBackend = vtextension::wasm::makeDefaultBackend();
    std::filesystem::path modulePath { "/extensions/spawner.wasm" };
    vthost::testing::GatedFileSystem fileSystem;
    core::async::StopSource commandShutdown;
    HoldingCapture capture { commandShutdown.get_token() };
    core::platform::SystemWallClock wallClock;
    vtbackend::LocalTimeConverter localTime { &vtbackend::fixedOffsetLocalTime<> };
    vtextension::ExtensionLimits limits {};
    std::unique_ptr<core::net::IoBackend> source = core::net::makeDefaultBackend();
    core::net::EventLoop loop { *source };
    vthost::DaemonExtensions* extensions = nullptr; ///< Owned by the host.
    std::unique_ptr<SessionHost> host;
};

} // namespace

TEST_CASE("SessionHost.extension.buildInFlightAtTeardown", "[vthost][host][extensions]")
{
    // A build still loading as the host goes finishes on the build thread and posts its result to the loop.
    // That result holds the engine, which must be released to the runtime that is still alive -- on the
    // loop, with no host it could reach. ASan is the judge.
    auto h = DaemonExtensionsHarness { "not a module", vtextension::CommandPermission::Deny };
    REQUIRE(h.host->createTab() != nullptr);
    auto const deadline = std::chrono::steady_clock::now() + std::chrono::seconds { 20 };
    while (!h.fileSystem.entered() && std::chrono::steady_clock::now() < deadline)
        std::this_thread::yield();
    REQUIRE(h.fileSystem.entered());

    // The loader is let go only while the host is being destroyed, which joins the build thread.
    auto opener = std::thread { [&] {
        std::this_thread::sleep_for(std::chrono::milliseconds { 100 });
        h.fileSystem.open();
    } };
    h.host.reset();
    opener.join();
    // What a daemon's loop does after its host: it turns once more, then goes.
    std::ignore = h.loop.runOnce(std::chrono::milliseconds { 0 });
    SUCCEED("the build's result was released while the runtime lived");
}

TEST_CASE("SessionHost.extension.controlVerb", "[vthost][host][extensions][control][threading]")
{
    // A daemon-hosted extension calls a control verb on its worker; the daemon's loop serves it through the
    // control API, as the extension's pane, once the user granted the manifest's `control read`. The guest
    // shows what it was answered: the refusal first, then the daemon's version.
    auto const module = vtextension::testing::withManifest(
        vtextension::testing::toModule(vtextension::testing::ControlReportingModule),
        "name = reporter\nversion = 1\ncontrol read\n");
    auto h =
        DaemonExtensionsHarness { std::string(reinterpret_cast<char const*>(module.data()), module.size()),
                                  vtextension::CommandPermission::Ask };
    auto wall = core::platform::ManualWallClock {};
    auto const service = vthost::control::ControlService {
        *h.host,
        vthost::control::ControlServiceConfig { .version = "e2e-1.2.3", .socketPath = "", .clock = wall }
    };
    h.fileSystem.open();
    REQUIRE(h.host->createTab() != nullptr);
    auto const session = h.host->model().window(h.host->windowId())->activeTab()->rootPane()->session();
    auto client = vthost::SessionStreamEvents {};
    h.host->subscribeStream(&client);

    h.turnUntil([&] { return h.indicatorText(session).contains("not granted yet"); });
    INFO(h.indicatorText(session));
    CHECK(h.indicatorText(session).contains("needs grant 'read': not granted yet"));
    REQUIRE(h.host->pendingPermissionRequests().size() == 1);
    auto const request = h.host->pendingPermissionRequests().front();
    CHECK(request.control == std::vector<std::string> { "read" });

    h.host->answerPermission(request.hostId, request.digest, vthost::GrantState::Granted);
    h.turnUntil([&] { return h.indicatorText(session).contains("e2e-1.2.3"); });
    CHECK(h.indicatorText(session).contains("e2e-1.2.3"));
    h.host->unsubscribeStream(&client);
}

TEST_CASE("SessionHost.extension.controlClosesItsOwnPane", "[vthost][host][extensions][control][threading]")
{
    // A daemon-hosted extension closes its own pane. The loop serves the close, which destroys the pane's
    // extensions -- the caller among them -- before it answers. Destroying them must not wait for the call
    // in flight: that would hold the loop, every session's, until the guest's deadline passed.
    auto const module = vtextension::testing::withManifest(
        vtextension::testing::toModule(vtextension::testing::PaneClosingModule),
        "name = closer\nversion = 1\ncontrol write\n");
    // A deadline far beyond the test's run: a loop that waits for it is told apart from one that does not.
    auto limits = vtextension::ExtensionLimits {};
    limits.callDeadline = std::chrono::seconds { 60 };
    auto h =
        DaemonExtensionsHarness { std::string(reinterpret_cast<char const*>(module.data()), module.size()),
                                  vtextension::CommandPermission::Ask,
                                  {},
                                  limits };
    auto wall = core::platform::ManualWallClock {};
    auto const service = vthost::control::ControlService {
        *h.host,
        vthost::control::ControlServiceConfig { .version = "e2e-1.2.3", .socketPath = "", .clock = wall }
    };
    h.fileSystem.open();
    REQUIRE(h.host->createTab() != nullptr);
    auto const session = h.host->model().window(h.host->windowId())->activeTab()->rootPane()->session();
    auto client = vthost::SessionStreamEvents {};
    h.host->subscribeStream(&client);

    h.turnUntil([&] { return h.indicatorText(session).contains("not granted yet"); });
    INFO(h.indicatorText(session));
    CHECK(h.indicatorText(session).contains("needs grant 'write': not granted yet"));
    REQUIRE(h.host->pendingPermissionRequests().size() == 1);
    auto const request = h.host->pendingPermissionRequests().front();

    auto const granted = std::chrono::steady_clock::now();
    h.host->answerPermission(request.hostId, request.digest, vthost::GrantState::Granted);
    h.turnUntil([&] { return h.host->terminal(session) == nullptr; });
    CHECK(h.host->terminal(session) == nullptr);
    // Far below the deadline the loop would otherwise have been held for.
    CHECK(std::chrono::steady_clock::now() - granted < std::chrono::seconds { 30 });
    h.host->unsubscribeStream(&client);
}

    #ifdef VTHOST_TEST_ECHO_WASM
TEST_CASE("SessionHost.extension.noFont", "[vthost][host][extensions][guest]")
{
    // A daemon draws in no font, and the native protocol carries no client's: its terminals are never told
    // one, so an extension's `state` reports the font as zeros, which the world documents as unknown. The
    // echo guest, as the harness's one extension, renders the state it read at load.
    auto in = std::ifstream { VTHOST_TEST_ECHO_WASM, std::ios::binary };
    REQUIRE(in.good());
    auto const module =
        std::string { std::istreambuf_iterator<char> { in }, std::istreambuf_iterator<char> {} };
    auto h =
        DaemonExtensionsHarness { module, vtextension::CommandPermission::Deny, { { "state", "describe" } } };
    h.fileSystem.open();
    REQUIRE(h.host->createTab() != nullptr);
    auto const session = h.host->model().window(h.host->windowId())->activeTab()->rootPane()->session();
    // A client is attached: the daemon repaints the row only for one to be sent.
    auto client = vthost::SessionStreamEvents {};
    h.host->subscribeStream(&client);

    h.turnUntil([&] { return h.indicatorText(session).contains("pt "); });
    INFO(h.indicatorText(session));
    CHECK(h.indicatorText(session).ends_with(";0pt 0x0]"));
    h.host->unsubscribeStream(&client);
}
    #else
TEST_CASE("SessionHost.extension.noFont", "[vthost][host][extensions][guest]")
{
    SKIP("the echo test guest is not built (CONTOUR_EXTENSIONS_BUNDLED=OFF)");
}
    #endif

    #ifdef VTHOST_TEST_SPAWNER_WASM

namespace
{

/// @return The bytes of the `spawner` test guest, which runs `echo {word}` and renders the outcome.
[[nodiscard]] std::string spawnerModule()
{
    auto in = std::ifstream { VTHOST_TEST_SPAWNER_WASM, std::ios::binary };
    REQUIRE(in.good());
    return std::string { std::istreambuf_iterator<char> { in }, std::istreambuf_iterator<char> {} };
}

} // namespace

TEST_CASE("SessionHost.extension.askNoClient", "[vthost][host][extensions][guest]")
{
    // With no client attached nobody can answer the extension's question: its command call fails as pending,
    // at once, the question waits for a client, and nothing blocks -- neither its worker nor the loop.
    auto h = DaemonExtensionsHarness { spawnerModule(), vtextension::CommandPermission::Ask };
    h.fileSystem.open();
    auto timeline = ExtensionTimeline {};
    h.host->subscribeStream(&timeline);
    REQUIRE(h.host->createTab() != nullptr);
    auto const session = h.host->model().window(h.host->windowId())->activeTab()->rootPane()->session();
    timeline.rowShowsPending = [&] {
        return h.indicatorText(session) == "[err:pending]";
    };

    h.turnUntil([&] { return timeline.shown.has_value(); });
    h.host->unsubscribeStream(&timeline);

    CHECK(h.indicatorText(session) == "[err:pending]");
    REQUIRE(timeline.asked.has_value());
    REQUIRE(timeline.shown.has_value());
    // The question opened while the call ran, and the frame it rendered followed within the call's own
    // deadline: a call that waited for an answer would never have rendered at all.
    CHECK(*timeline.shown - *timeline.asked < h.limits.callDeadline + std::chrono::milliseconds { 250 });
    CHECK(h.capture.runs.load() == 0);
    REQUIRE(h.host->pendingPermissionRequests().size() == 1);
    auto const& request = h.host->pendingPermissionRequests().front();
    CHECK(request.session == session.value);
    CHECK(request.name == "spawner");
    CHECK(request.hostId == "spawner@/extensions/spawner.wasm");
    REQUIRE(request.commands.size() == 1);
    CHECK(request.commands.front().argv == std::vector<std::string> { "echo", "{word}" });
    CHECK(h.extensions->lookup(request.hostId, request.digest) == vthost::GrantState::Pending);
    CHECK(h.extensions->runningWorkers() == 1);
}

TEST_CASE("SessionHost.extension.commandInFlightAtTeardown", "[vthost][host][extensions][guest]")
{
    // A command an extension runs as the host goes is cancelled, not waited out for its timeout: the
    // daemon exits when asked to.
    auto h = DaemonExtensionsHarness { spawnerModule(), vtextension::CommandPermission::Allow };
    h.fileSystem.open();
    REQUIRE(h.host->createTab() != nullptr);
    h.turnUntil([&] { return h.capture.runs.load() > 0; });
    REQUIRE(h.capture.runs.load() > 0);
    REQUIRE(h.capture.lastTimeout.load() >= std::chrono::seconds { 1 });

    auto const started = std::chrono::steady_clock::now();
    h.host.reset();
    CHECK(std::chrono::steady_clock::now() - started < h.capture.lastTimeout.load() / 2);
}

    #else

TEST_CASE("SessionHost.extension.askNoClient", "[vthost][host][extensions][guest]")
{
    SKIP("the spawner test guest is not built (CONTOUR_EXTENSIONS_BUNDLED=OFF)");
}

TEST_CASE("SessionHost.extension.commandInFlightAtTeardown", "[vthost][host][extensions][guest]")
{
    SKIP("the spawner test guest is not built (CONTOUR_EXTENSIONS_BUNDLED=OFF)");
}

    #endif
#endif

namespace
{

/// @param mainShell What the default profile "main" runs.
/// @param mainHistory The scrollback "main" keeps.
/// @return The catalog the profile cases resolve in: "main" (the default), "build" running /bin/build-shell
///         with 5000 lines, "ops" /bin/ops-shell with 2000.
std::shared_ptr<vthost::ProfileCatalog const> profileCatalog(
    std::string mainShell = "/bin/zsh", vtbackend::LineCount mainHistory = vtbackend::LineCount(1000))
{
    using vthost::testing::fakeProfile;
    auto main = fakeProfile(std::move(mainShell), mainHistory);
    main.shell.env["ONLY_MAIN"] = "1"; // set by the default profile alone
    auto ops = fakeProfile("/bin/ops-shell", vtbackend::LineCount(2000));
    ops.sandbox = vthost::SandboxEscape::Contained; // escape_sandbox: false, unlike the default
    return std::make_shared<vthost::testing::FakeProfileCatalog>(
        std::map<std::string, vthost::ResolvedProfile> {
            { "main", std::move(main) },
            { "build", fakeProfile("/bin/build-shell", vtbackend::LineCount(5000)) },
            { "ops", ops },
        },
        "main");
}

/// A host over @ref profileCatalog that records the shell every session it spawns was given, on a loop
/// timed by a clock the case advances.
struct ProfileHost
{
    explicit ProfileHost(vtworkspace::Layout layout = {},
                         std::shared_ptr<vthost::ProfileCatalog const> catalog = profileCatalog()):
        host { loop,
               [this](vtbackend::PageSize size,
                      std::optional<vtpty::Process::ExecInfo> const& commandOverride) {
                   programs.push_back(commandOverride ? commandOverride->program : std::string {});
                   shells.push_back(commandOverride.value_or(vtpty::Process::ExecInfo {}));
                   return std::make_unique<vtpty::MockPty>(size);
               },
               vtbackend::Settings {},
               core::defaultEnvironment(),
               vthost::SessionHostConfig { .pumping = vthost::SessionPumping::Manual,
                                           .startupLayout = std::move(layout),
                                           .profiles = std::move(catalog),
                                           .profilePtys = recordProfileSpawn() } }
    {
    }

    core::platform::ManualClock clock { core::platform::SteadyTimePoint {} + std::chrono::seconds { 10 } };
    std::unique_ptr<core::net::IoBackend> source = core::net::makeDefaultBackend();
    core::net::EventLoop loop { *source, clock };
    std::vector<std::string> programs {};            ///< The program each spawn was given, in spawn order.
    std::vector<vtpty::Process::ExecInfo> shells {}; ///< The whole shell each spawn was given, in order.
    std::vector<vthost::SandboxEscape> sandboxes {}; ///< The sandbox policy each profile spawn was given.

    /// @return A profile PTY factory recording what each spawn was given, as the PtyFactory above does.
    [[nodiscard]] vthost::ProfilePtyFactory recordProfileSpawn()
    {
        return [this](vtbackend::PageSize size,
                      vtpty::Process::ExecInfo const& shell,
                      vthost::SandboxEscape sandbox) {
            programs.push_back(shell.program);
            shells.push_back(shell);
            sandboxes.push_back(sandbox);
            return std::make_unique<vtpty::MockPty>(size);
        };
    }
    SessionHost host;

    /// @return The session hosted in @p tab's first pane.
    [[nodiscard]] vtworkspace::SessionId sessionOf(vtworkspace::Tab const& tab) const
    {
        return tab.rootPane()->isLeaf() ? tab.rootPane()->session() : tab.rootPane()->first()->session();
    }
};

} // namespace

TEST_CASE("a session spawned under a profile name runs that profile's shell and settings",
          "[vthost][profiles]")
{
    auto h = ProfileHost {};
    auto const* tab = h.host.createTab({ .profileName = "build" });
    REQUIRE(tab != nullptr);
    auto const session = h.sessionOf(*tab);
    REQUIRE(h.programs == std::vector<std::string> { "/bin/build-shell" });
    CHECK(h.host.terminal(session)->settings().historyLimits.guaranteed
          == vtbackend::MaxHistoryLineCount { vtbackend::LineCount(5000) });
    CHECK(h.host.hostedSession(session)->profileName() == "build");
}

TEST_CASE("a profile's shell still learns the pane's own ids", "[vthost][profiles][ids]")
{
    // A profile that sets a CONTOUR_PANE of its own loses to the real one, and keeps its other variables.
    auto main = vthost::testing::fakeProfile("/bin/zsh", vtbackend::LineCount(1000));
    main.shell.env["A"] = "1";
    main.shell.env["CONTOUR_PANE"] = "bogus";
    auto catalog = std::make_shared<vthost::testing::FakeProfileCatalog>(
        std::map<std::string, vthost::ResolvedProfile> { { "main", std::move(main) } }, "main");
    auto h = ProfileHost { {}, catalog };
    REQUIRE(h.host.createTab() != nullptr);
    REQUIRE(h.shells.size() == 1);
    CHECK(h.shells.back().env.at("A") == "1");
    CHECK(h.shells.back().env.at("CONTOUR_PANE").starts_with("pane:"));
    CHECK(h.shells.back().env.at("CONTOUR_TAB").starts_with("tab:"));
    CHECK(h.shells.back().env.at("CONTOUR_WINDOW").starts_with("window:"));
}

TEST_CASE("an unknown profile name runs the host's default profile", "[vthost][profiles]")
{
    // Review focus 4: a Windows laptop's pwsh profile against a Linux host -- the session still runs.
    auto h = ProfileHost {};
    auto const* tab = h.host.createTab({ .profileName = "pwsh" });
    REQUIRE(tab != nullptr);
    CHECK(h.programs == std::vector<std::string> { "/bin/zsh" });
    CHECK(h.host.hostedSession(h.sessionOf(*tab))->profileName() == "main");
}

TEST_CASE("a startup layout pane's profile is honoured", "[vthost][profiles]")
{
    auto tab = vtworkspace::LayoutTab {};
    tab.profile = "build";
    tab.root.orientation = vtworkspace::SplitState::Horizontal;
    tab.root.children.emplace_back();
    tab.root.children.emplace_back();
    tab.root.children.back().profile = "ops";
    auto h = ProfileHost { vtworkspace::Layout { .tabs = { tab } } };
    CHECK(h.programs == std::vector<std::string> { "/bin/build-shell", "/bin/ops-shell" });
}

TEST_CASE("a session named no profile runs the host's default profile", "[vthost][profiles]")
{
    auto h = ProfileHost {};
    auto const* tab = h.host.createTab();
    REQUIRE(tab != nullptr);
    CHECK(h.programs == std::vector<std::string> { "/bin/zsh" });
    CHECK(h.host.terminal(h.sessionOf(*tab))->settings().historyLimits.guaranteed
          == vtbackend::MaxHistoryLineCount { vtbackend::LineCount(1000) });
    // Unnamed, so it follows whichever profile is the default.
    CHECK(h.host.hostedSession(h.sessionOf(*tab))->profileName().empty());
}

TEST_CASE("a session spawned under another profile carries none of the default profile's environment",
          "[vthost][profiles]")
{
    auto h = ProfileHost {};
    REQUIRE(h.host.createTab({ .profileName = "build" }) != nullptr);
    REQUIRE(h.host.createTab() != nullptr);
    REQUIRE(h.shells.size() == 2);
    CHECK_FALSE(h.shells[0].env.contains("ONLY_MAIN"));
    CHECK(h.shells[1].env.at("ONLY_MAIN") == "1");
}

TEST_CASE("after a reload a spawn naming no profile runs the reloaded default", "[vthost][profiles][reload]")
{
    auto h = ProfileHost {};
    h.host.replaceProfiles(profileCatalog("/bin/zsh-next", vtbackend::LineCount(3000)));
    auto const* tab = h.host.createTab();
    REQUIRE(tab != nullptr);
    CHECK(h.programs == std::vector<std::string> { "/bin/zsh-next" });
    CHECK(h.host.terminal(h.sessionOf(*tab))->settings().historyLimits.guaranteed
          == vtbackend::MaxHistoryLineCount { vtbackend::LineCount(3000) });
}

TEST_CASE("a hosted shell replacing the daemon's carries only its own environment", "[vthost][profiles]")
{
    auto base = vtpty::Process::ExecInfo {};
    base.program = "/bin/zsh";
    base.arguments = { "-l" };
    base.env["ONLY_DEFAULT"] = "1";
    auto profile = vtpty::Process::ExecInfo {};
    profile.program = "/bin/fish";
    profile.env["ONLY_WORK"] = "1";
    profile.env["CONTOUR_MUX"] = "/elsewhere";
    auto const hosted = vtpty::Process::Environment { { "CONTOUR_MUX", "/run/contour/default" } };

    auto const replaced = vthost::hostedShellFor(base, profile, vthost::ShellOverride::Replaces, hosted);
    CHECK(replaced.program == "/bin/fish");
    CHECK(replaced.arguments.empty());
    CHECK_FALSE(replaced.env.contains("ONLY_DEFAULT"));
    CHECK(replaced.env.at("ONLY_WORK") == "1");
    CHECK(replaced.env.at("CONTOUR_MUX") == "/run/contour/default"); // the daemon's own still wins

    // A layout pane's directory-only override against a host without profiles keeps the daemon's shell.
    auto pane = vtpty::Process::ExecInfo {};
    pane.workingDirectory = "/tmp/project";
    auto const overlaid = vthost::hostedShellFor(base, pane, vthost::ShellOverride::Overlays, hosted);
    CHECK(overlaid.program == "/bin/zsh");
    CHECK(overlaid.arguments == std::vector<std::string> { "-l" });
    CHECK(overlaid.workingDirectory == "/tmp/project");
    CHECK(overlaid.env.at("ONLY_DEFAULT") == "1");
}

TEST_CASE("replaceProfiles re-limits running sessions and serves new ones from the new catalog",
          "[vthost][profiles][reload]")
{
    auto h = ProfileHost {};
    auto const* tab = h.host.createTab({ .profileName = "main" });
    REQUIRE(tab != nullptr);
    auto const running = h.sessionOf(*tab);
    auto* terminal = h.host.terminal(running);

    h.host.replaceProfiles(profileCatalog("/bin/zsh-next", vtbackend::LineCount(3000)));

    CHECK(terminal->settings().historyLimits.guaranteed
          == vtbackend::MaxHistoryLineCount { vtbackend::LineCount(3000) });
    CHECK(h.host.terminal(running) == terminal);                  // the same terminal...
    CHECK(h.programs == std::vector<std::string> { "/bin/zsh" }); // ...and no PTY respawned
    REQUIRE(h.host.createTab({ .profileName = "main" }) != nullptr);
    CHECK(h.programs == std::vector<std::string> { "/bin/zsh", "/bin/zsh-next" });
}

TEST_CASE("replaceProfiles leaves the limits a client stated for an unnamed session",
          "[vthost][profiles][reload]")
{
    // A client that shares no ProfileByName states its own settings; the host's default profile is not its.
    auto h = ProfileHost {};
    auto stated = vtbackend::Settings {};
    stated.historyLimits =
        vtbackend::HistoryLimits::plain(vtbackend::MaxHistoryLineCount { vtbackend::LineCount(200) });
    auto const* tab = h.host.createTab({ .settings = stated });
    REQUIRE(tab != nullptr);
    auto* terminal = h.host.terminal(h.sessionOf(*tab));
    auto const before = terminal->settings().historyLimits;
    REQUIRE(before.guaranteed == vtbackend::MaxHistoryLineCount { vtbackend::LineCount(200) });

    h.host.replaceProfiles(profileCatalog("/bin/zsh-next", vtbackend::LineCount(3000)));

    CHECK(terminal->settings().historyLimits == before);
}

TEST_CASE("the reload watcher hands each catalog its hook yields to the host", "[vthost][profiles][reload]")
{
    using namespace std::chrono_literals;
    auto h = ProfileHost {};
    auto const* tab = h.host.createTab({ .profileName = "main" });
    REQUIRE(tab != nullptr);
    auto* terminal = h.host.terminal(h.sessionOf(*tab));

    // A few turns: a deadline that came due is found on a turn after the one that advanced past it.
    auto const turn = [&h] {
        for ([[maybe_unused]] auto const i: std::views::iota(0, 5))
            std::ignore = h.loop.runOnce(0ms);
    };
    auto polls = 0;
    auto next = std::shared_ptr<vthost::ProfileCatalog const> {};
    auto const watcher = vthost::ConfigReloadWatcher { h.loop,
                                                       h.host,
                                                       { .poll =
                                                             [&] {
                                                                 ++polls;
                                                                 return std::exchange(next, nullptr);
                                                             },
                                                         .interval = 2000ms } };
    turn();
    CHECK(polls == 0); // not before its interval

    h.clock.advance(2000ms);
    turn();
    CHECK(polls == 1); // nothing yielded: nothing replaced
    CHECK(terminal->settings().historyLimits.guaranteed
          == vtbackend::MaxHistoryLineCount { vtbackend::LineCount(1000) });

    next = profileCatalog("/bin/zsh", vtbackend::LineCount(4000));
    h.clock.advance(2000ms);
    turn();
    CHECK(polls == 2);
    CHECK(terminal->settings().historyLimits.guaranteed
          == vtbackend::MaxHistoryLineCount { vtbackend::LineCount(4000) });
}

TEST_CASE("a session takes escape_sandbox from its own profile", "[vthost][profiles]")
{
    // A sandbox setting inherited from the default profile would run a contained profile's shell on the host.
    auto h = ProfileHost {};
    REQUIRE(h.host.createTab({ .profileName = "ops" }) != nullptr);
    REQUIRE(h.host.createTab() != nullptr);
    REQUIRE(h.host.createTab({ .profileName = "nope" }) != nullptr);
    CHECK(h.sandboxes
          == std::vector { vthost::SandboxEscape::Contained,
                           vthost::SandboxEscape::Escapes,
                           vthost::SandboxEscape::Escapes });
}

namespace
{

/// Remembers the order in which a host announced the end of its sessions.
struct EndRecorder final: vthost::SessionStreamEvents
{
    std::vector<std::pair<vtworkspace::SessionId, vthost::SessionEnd>> ended {};
    std::vector<std::string> order {};

    void sessionEnded(vtworkspace::SessionId session, vthost::SessionEnd cause) override
    {
        ended.emplace_back(session, cause);
        order.push_back(std::format("ended({})", session.value));
    }
    void sessionClosed(vtworkspace::SessionId session) override
    {
        order.push_back(std::format("closed({})", session.value));
    }
};

} // namespace

TEST_CASE("SessionHost reports why each session ended", "[vthost][snapshot]")
{
    auto h = HostHarness {};
    auto recorder = EndRecorder {};
    h.host.subscribeStream(&recorder);
    auto* tab = h.host.createTab();
    REQUIRE(tab != nullptr);
    h.host.splitActivePane(tab->id(), SplitState::Horizontal, 0.5);
    h.host.splitActivePane(tab->id(), SplitState::Horizontal, 0.5);
    REQUIRE(h.host.sessionCount() == 3);
    auto const a = tab->rootPane()->first()->session();
    auto const b = tab->rootPane()->second()->first()->session();
    auto const c = tab->rootPane()->second()->second()->session();

    h.host.closeSession(a);
    h.host.handleSessionExit(b);
    using Entry = std::pair<vtworkspace::SessionId, vthost::SessionEnd>;
    CHECK(recorder.ended
          == std::vector<Entry> { { a, vthost::SessionEnd::Requested }, { b, vthost::SessionEnd::Exited } });
    CHECK(recorder.order
          == std::vector<std::string> { std::format("ended({})", a.value),
                                        std::format("closed({})", a.value),
                                        std::format("ended({})", b.value),
                                        std::format("closed({})", b.value) });

    h.host.endSession(c, vthost::SessionEnd::Killed);
    CHECK(recorder.ended.back() == Entry { c, vthost::SessionEnd::Killed });
    h.host.unsubscribeStream(&recorder);
}

#ifndef _WIN32
namespace
{
    #ifdef __linux__
// How a shell that closed its terminal and lives on ends: the host's close of the master hangs it up.
constexpr auto ClosedTerminalEnd = std::optional { vthost::SessionEnd::Killed };
    #else
// Not asserted elsewhere. macOS detaches a terminal from its session at the terminal's last close, so the
// host's close of the master hangs nobody up, and the shell sleeps on: Exited, its end not known yet when
// the pump asks. FreeBSD ends it either way, depending on whether its hangup lands while the pump waits.
constexpr auto ClosedTerminalEnd = std::optional<vthost::SessionEnd> {};
    #endif
} // namespace

TEST_CASE("a hosted shell killed by a signal ends Killed, one that exits ends Exited",
          "[vthost][snapshot][threading]")
{
    // The PTY reports its hangup before the child is necessarily reaped; the pump's short poll of the exit
    // status is what tells a kill from an exit, and the kill-grace rule of the snapshot rests on it.
    struct Row
    {
        std::string script;
        std::optional<vthost::SessionEnd> expected; ///< Nullopt where how it ends is not asserted.
    };
    for (auto const& row: {
             Row { "kill -9 $$", vthost::SessionEnd::Killed },
             // Closing its terminal while it lives on makes the pump see the hangup first, and the host's
             // close then hangs the shell up (where it does: @see ClosedTerminalEnd): there is no status yet
             // when the pump asks, only a moment later.
             Row { "exec 0<&- 1>&- 2>&-; sleep 5", ClosedTerminalEnd },
             Row { "exit 3", vthost::SessionEnd::Exited },
         })
    {
        INFO(row.script);
        auto const backend = core::net::makeDefaultBackend();
        auto loop = core::net::EventLoop { *backend };
        auto host =
            SessionHost { loop,
                          [&row](vtbackend::PageSize size, std::optional<vtpty::Process::ExecInfo> const&) {
                              auto shell = vtpty::Process::ExecInfo {};
                              shell.program = "/bin/sh";
                              shell.arguments = { "-c", row.script };
                              return std::make_unique<vtpty::Process>(shell,
                                                                      vtpty::createPty(size, std::nullopt),
                                                                      false,
                                                                      std::make_shared<vtpty::NoPlacement>());
                          },
                          vtbackend::Settings {},
                          core::defaultEnvironment(),
                          vthost::SessionHostConfig { .pumping = vthost::SessionPumping::Threaded } };
        auto recorder = EndRecorder {};
        host.subscribeStream(&recorder);
        REQUIRE(host.createTab() != nullptr);

        auto const deadline = std::chrono::steady_clock::now() + std::chrono::seconds { 10 };
        while (recorder.ended.empty() && std::chrono::steady_clock::now() < deadline)
            std::ignore = loop.runOnce(std::chrono::milliseconds { 10 });
        REQUIRE(recorder.ended.size() == 1);
        if (row.expected)
            CHECK(recorder.ended.front().second == *row.expected);
        host.unsubscribeStream(&recorder);
    }
}
#endif

TEST_CASE("ending a session twice reports it once", "[vthost][snapshot]")
{
    auto h = HostHarness {};
    auto recorder = EndRecorder {};
    h.host.subscribeStream(&recorder);
    auto* tab = h.host.createTab();
    REQUIRE(tab != nullptr);
    auto const session = tab->rootPane()->session();
    h.host.closeSession(session);
    h.host.handleSessionExit(session);
    CHECK(recorder.ended.size() == 1);
    h.host.unsubscribeStream(&recorder);
}

TEST_CASE("paneRuntime reports the reported directory and the spawn profile", "[vthost][snapshot]")
{
    SECTION("a directory the shell reported, on a MockPty")
    {
        auto h = HostHarness { vthost::ClientSizePolicy::Latest,
                               vtbackend::HostIdentity { .machineId = {}, .hostname = "zeta" } };
        auto* tab = h.host.createTab();
        REQUIRE(tab != nullptr);
        auto const session = tab->rootPane()->session();
        auto const before = h.host.paneRuntime(session);
        CHECK_FALSE(before.workingDirectory);
        CHECK_FALSE(before.foregroundProcess);
        CHECK_FALSE(before.profile);

        h.host.terminal(session)->writeToScreen("\033]7;file://zeta/tmp/work\033\\");
        auto const after = h.host.paneRuntime(session);
        CHECK(after.workingDirectory == std::filesystem::path { "/tmp/work" });
        CHECK_FALSE(after.foregroundProcess);
    }
    SECTION("the profile a session was spawned under")
    {
        auto h = ProfileHost {};
        auto const* const named = h.host.createTab({ .profileName = "build" });
        auto const* const unnamed = h.host.createTab();
        REQUIRE(named != nullptr);
        REQUIRE(unnamed != nullptr);
        CHECK(h.host.paneRuntime(h.sessionOf(*named)).profile == "build");
        CHECK_FALSE(h.host.paneRuntime(h.sessionOf(*unnamed)).profile);
    }
    SECTION("an unknown session")
    {
        auto h = HostHarness {};
        auto const runtime = h.host.paneRuntime(vtworkspace::SessionId { 4711 });
        CHECK_FALSE(runtime.workingDirectory);
        CHECK_FALSE(runtime.profile);
    }
}

// {{{ Object ids

namespace
{

/// Model events that do nothing, for a probe that overrides the one it watches.
struct NullModelEvents: vtworkspace::ModelEvents
{
    void tabAdded(vtworkspace::WindowId, vtworkspace::TabId, int) override {}
    void tabClosed(vtworkspace::WindowId, vtworkspace::TabId, int) override {}
    void tabMoved(vtworkspace::WindowId, vtworkspace::TabId, int, int) override {}
    void activeTabChanged(vtworkspace::WindowId, vtworkspace::TabId, int) override {}
    void paneSplit(vtworkspace::TabId, vtworkspace::PaneId, vtworkspace::PaneId) override {}
    void paneClosed(vtworkspace::TabId, vtworkspace::PaneId, vtworkspace::PaneId) override {}
    void activePaneChanged(vtworkspace::TabId, vtworkspace::PaneId) override {}
    void paneRatioChanged(vtworkspace::TabId, vtworkspace::PaneId, double) override {}
    void tabTitleChanged(vtworkspace::TabId) override {}
    void tabColorChanged(vtworkspace::TabId) override {}
};

} // namespace

TEST_CASE("createTab mints window, pane and tab ids and exports them to the shell", "[vthost][host][ids]")
{
    auto h = HostHarness {};
    auto* tab = h.host.createTab();
    REQUIRE(tab != nullptr);
    auto const session = tab->rootPane()->session();

    // The window is minted at construction, then the tab's pane, then the tab.
    CHECK(h.host.objects().uuidOf(h.host.windowId()) == seq(1));
    CHECK(h.host.objects().uuidOf(session) == seq(2));
    CHECK(h.host.objects().uuidOf(tab->id()) == seq(3));
    REQUIRE(h.seen.size() == 1);
    CHECK(h.seen.back().env.at("CONTOUR_PANE") == ref(vtcontrol::ObjectKind::Pane, 2));
    CHECK(h.seen.back().env.at("CONTOUR_TAB") == ref(vtcontrol::ObjectKind::Tab, 3));
    CHECK(h.seen.back().env.at("CONTOUR_WINDOW") == ref(vtcontrol::ObjectKind::Window, 1));
    // The override engaged for the ids names no program: the configured shell runs.
    CHECK(h.seen.back().program.empty());
}

TEST_CASE("the tab's id is bound before observers hear of the tab", "[vthost][host][ids]")
{
    struct Probe final: NullModelEvents
    {
        void tabAdded(vtworkspace::WindowId /*window*/, vtworkspace::TabId tab, int /*index*/) override
        {
            uuid = host->objects().uuidOf(tab);
        }
        SessionHost* host = nullptr;
        std::optional<vtcontrol::Uuid> uuid;
    };
    auto h = HostHarness {};
    auto probe = Probe {};
    probe.host = &h.host;
    h.host.subscribe(&probe);
    REQUIRE(h.host.createTab() != nullptr);
    h.host.unsubscribe(&probe);
    CHECK(probe.uuid == seq(3));
}

TEST_CASE("a split pane gets a new id and its tab's id in CONTOUR_TAB", "[vthost][host][ids]")
{
    auto h = HostHarness {};
    auto* tab = h.host.createTab();
    REQUIRE(tab != nullptr);

    h.host.splitActivePane(tab->id(), SplitState::Vertical, 0.5);

    REQUIRE(h.seen.size() == 2);
    CHECK(h.seen.back().env.at("CONTOUR_PANE") == ref(vtcontrol::ObjectKind::Pane, 4));
    CHECK(h.seen.back().env.at("CONTOUR_TAB") == ref(vtcontrol::ObjectKind::Tab, 3));
    CHECK(h.seen.back().env.at("CONTOUR_WINDOW") == ref(vtcontrol::ObjectKind::Window, 1));
    CHECK(h.host.objects().uuidOf(tab->rootPane()->second()->session()) == seq(4));
    // The first pane keeps its own.
    CHECK(h.host.objects().uuidOf(tab->rootPane()->first()->session()) == seq(2));
}

TEST_CASE("a new window gets its own id, and its first pane lives in it", "[vthost][host][ids]")
{
    auto h = HostHarness {};
    auto* window = h.host.createWindow();
    REQUIRE(window != nullptr);

    // pane, tab, window.
    CHECK(h.host.objects().uuidOf(window->id()) == seq(4));
    CHECK(h.seen.back().env.at("CONTOUR_PANE") == ref(vtcontrol::ObjectKind::Pane, 2));
    CHECK(h.seen.back().env.at("CONTOUR_TAB") == ref(vtcontrol::ObjectKind::Tab, 3));
    CHECK(h.seen.back().env.at("CONTOUR_WINDOW") == ref(vtcontrol::ObjectKind::Window, 4));
    auto const* tab = window->activeTab();
    REQUIRE(tab != nullptr);
    CHECK(h.host.objects().uuidOf(tab->id()) == seq(3));
}

TEST_CASE("startup layout panes share their tab's and window's ids", "[vthost][host][ids]")
{
    auto tab = vtworkspace::LayoutTab {};
    tab.root.orientation = vtworkspace::SplitState::Horizontal;
    tab.root.children.emplace_back();
    tab.root.children.emplace_back();
    auto seen = std::vector<vtpty::Process::ExecInfo> {};
    auto source = core::net::testing::ScriptedBackend {};
    auto loop = core::net::EventLoop { source };
    auto host = SessionHost {
        loop,
        [&](vtbackend::PageSize size, std::optional<vtpty::Process::ExecInfo> const& spawn) {
            seen.push_back(spawn.value_or(vtpty::Process::ExecInfo {}));
            return std::make_unique<vtpty::MockPty>(size);
        },
        vtbackend::Settings {},
        core::defaultEnvironment(),
        vthost::SessionHostConfig { .pumping = vthost::SessionPumping::Manual,
                                    .startupLayout = vtworkspace::Layout { .tabs = { tab } },
                                    .uuids = std::make_shared<vtcontrol::testing::SequentialUuidSource>() }
    };
    REQUIRE(seen.size() == 2);
    CHECK(seen[0].env.at("CONTOUR_WINDOW") == ref(vtcontrol::ObjectKind::Window, 1));
    CHECK(seen[1].env.at("CONTOUR_WINDOW") == ref(vtcontrol::ObjectKind::Window, 1));
    CHECK(seen[0].env.at("CONTOUR_PANE") == ref(vtcontrol::ObjectKind::Pane, 2));
    CHECK(seen[0].env.at("CONTOUR_TAB") == ref(vtcontrol::ObjectKind::Tab, 3));
    CHECK(seen[1].env.at("CONTOUR_PANE") == ref(vtcontrol::ObjectKind::Pane, 4));
    CHECK(seen[1].env.at("CONTOUR_TAB") == ref(vtcontrol::ObjectKind::Tab, 3));
}

TEST_CASE("preset ids and labels are honoured", "[vthost][host][ids]")
{
    auto h = HostHarness {};
    auto const pane = seq(100);
    auto const tabId = seq(101);
    auto* tab = h.host.createTab(vthost::SessionSpawnRequest {
        .presetIds =
            vthost::PresetObjectIds { .pane = pane, .tab = tabId, .paneLabel = "logs", .tabLabel = "ops" } });
    REQUIRE(tab != nullptr);

    CHECK(h.host.objects().uuidOf(tab->rootPane()->session()) == pane);
    CHECK(h.host.objects().uuidOf(tab->id()) == tabId);
    CHECK(h.host.objects().labelOf(vtcontrol::ObjectRef { vtcontrol::ObjectKind::Pane, pane }) == "logs");
    CHECK(h.host.objects().labelOf(vtcontrol::ObjectRef { vtcontrol::ObjectKind::Tab, tabId }) == "ops");
    CHECK(h.seen.back().env.at("CONTOUR_PANE")
          == vtcontrol::toString(vtcontrol::ObjectRef { vtcontrol::ObjectKind::Pane, pane }));

    // The source minted nothing for them: the next ids continue from the window's.
    auto* next = h.host.createTab();
    REQUIRE(next != nullptr);
    CHECK(h.host.objects().uuidOf(next->rootPane()->session()) == seq(2));
    CHECK(h.host.objects().uuidOf(next->id()) == seq(3));
}

TEST_CASE("closed objects are forgotten and ids never repeat", "[vthost][host][ids]")
{
    auto h = HostHarness {};
    auto* tab = h.host.createTab();
    REQUIRE(tab != nullptr);
    auto const session = tab->rootPane()->session();
    auto const tabId = tab->id();
    auto const paneRef = vtcontrol::ObjectRef { vtcontrol::ObjectKind::Pane, seq(2) };
    auto const tabRef = vtcontrol::ObjectRef { vtcontrol::ObjectKind::Tab, seq(3) };
    REQUIRE(h.host.objects().find(paneRef));

    h.host.handleSessionExit(session);

    CHECK_FALSE(h.host.objects().uuidOf(session));
    CHECK_FALSE(h.host.objects().uuidOf(tabId));
    CHECK_FALSE(h.host.objects().find(paneRef));
    CHECK_FALSE(h.host.objects().find(tabRef));

    auto* again = h.host.createTab();
    REQUIRE(again != nullptr);
    CHECK(h.host.objects().uuidOf(again->rootPane()->session()) == seq(4));
    CHECK(h.host.objects().uuidOf(again->id()) == seq(5));
}

TEST_CASE("observers can still name what closed while they are told", "[vthost][host][ids]")
{
    struct Probe final: NullModelEvents
    {
        void tabClosed(vtworkspace::WindowId /*window*/, vtworkspace::TabId tab, int /*index*/) override
        {
            uuid = host->objects().uuidOf(tab);
        }
        SessionHost* host = nullptr;
        std::optional<vtcontrol::Uuid> uuid;
    };
    auto h = HostHarness {};
    auto* tab = h.host.createTab();
    REQUIRE(tab != nullptr);
    auto probe = Probe {};
    probe.host = &h.host;
    h.host.subscribe(&probe);
    h.host.handleSessionExit(tab->rootPane()->session());
    h.host.unsubscribe(&probe);
    CHECK(probe.uuid == seq(3));
}

TEST_CASE("a refused split leaves no pane id behind", "[vthost][host][ids]")
{
    auto h = HostHarness {};
    REQUIRE(h.host.createTab() != nullptr);
    h.host.splitActivePane(vtworkspace::TabId { 4711 }, SplitState::Vertical, 0.5);
    CHECK_FALSE(h.host.objects().find(vtcontrol::ObjectRef { vtcontrol::ObjectKind::Pane, seq(4) }));
}

TEST_CASE("applyCommandOverride overlays env entries", "[vthost][host]")
{
    auto shell = vtpty::Process::ExecInfo {};
    shell.program = "/bin/zsh";
    shell.env = { { "A", "0" }, { "B", "2" } };
    auto commandOverride = std::optional { vtpty::Process::ExecInfo {} };
    commandOverride->env = { { "A", "1" } };
    vtpty::Process::applyCommandOverride(shell, commandOverride);
    CHECK(shell.env == vtpty::Process::Environment { { "A", "1" }, { "B", "2" } });
    CHECK(shell.program == "/bin/zsh");
}

// }}}

namespace
{
/// Keeps every signal the host fans out, in order.
struct SignalRecorder final: vthost::SessionStreamEvents
{
    void sessionSignal(vtworkspace::SessionId session, vthost::SessionSignal const& signal) override
    {
        signals.emplace_back(session, signal);
    }

    template <typename T>
    [[nodiscard]] std::vector<T> all() const
    {
        auto found = std::vector<T> {};
        for (auto const& [session, signal]: signals)
            if (auto const* value = std::get_if<T>(&signal))
                found.push_back(*value);
        return found;
    }

    std::vector<std::pair<vtworkspace::SessionId, vthost::SessionSignal>> signals;
};
} // namespace

TEST_CASE("hosted sessions fan out block, title and cwd signals", "[vthost][host][signals]")
{
    auto h = HostHarness {};
    auto recorder = SignalRecorder {};
    auto const subscription = vthost::makeScopedStreamSubscription(h.host, recorder);
    auto* tab = h.host.createTab();
    REQUIRE(tab != nullptr);
    auto const session = tab->rootPane()->session();
    auto& terminal = *h.host.terminal(session);
    auto const drain = [&] {
        std::ignore = h.loop.runUntilIdle();
    };

    terminal.writeToScreen("\033]133;A\007$ \033]133;B\007make\r\n\033]133;C\007out\r\n\033]133;D;3\007");
    // Nothing reaches an observer before the loop runs: the parser thread only posts.
    CHECK(recorder.signals.empty());
    drain();
    auto const finished = recorder.all<vthost::CommandBlockFinishedSignal>();
    REQUIRE(finished.size() == 1);
    CHECK(finished.front().summary.exitCode == 3);
    auto const changed = recorder.all<vthost::CommandBlockChangedSignal>();
    CHECK(std::ranges::any_of(changed, [&](auto const& c) { return c.id == finished.front().summary.id; }));
    CHECK(recorder.signals.front().first == session);

    terminal.writeToScreen("\033]2;hello\007");
    drain();
    auto const titles = recorder.all<vthost::TitleChangedSignal>();
    REQUIRE(titles.size() == 1);
    CHECK(titles.front().title == "hello");

    terminal.writeToScreen("\033]7;file://h/tmp\033\\");
    terminal.writeToScreen("\033]7;file://h/tmp\033\\");
    drain();
    CHECK(recorder.all<vthost::WorkingDirectoryChangedSignal>().size() == 1);
}
