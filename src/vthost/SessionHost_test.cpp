// SPDX-License-Identifier: Apache-2.0
#include <vtpty/MockPty.hpp>

#include <crispy/BufferObject.hpp>
#include <crispy/LogSink.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <format>
#include <functional>
#include <future>
#include <memory>
#include <mutex>
#include <ranges>
#include <string>
#include <string_view>
#include <thread>
#include <tuple>
#include <vector>

#include <coro/Task.hpp>
#include <net/EventLoop.hpp>
#include <net/PollEventSource.hpp>
#include <net/testing/ScriptedEventSource.hpp>
#include <vthost/SessionHost.hpp>
#include <vthost/TappingPty.hpp>
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
    explicit HostHarness(vthost::ClientSizePolicy sizePolicy = vthost::ClientSizePolicy::Latest):
        policy { sizePolicy }
    {
        host.subscribe(&recorder);
    }

    net::testing::ScriptedEventSource source;
    net::EventLoop loop { source };
    RecordingEvents recorder;
    StubClient client;
    StubClient otherClient;          ///< For the multi-client policy cases.
    vthost::ClientSizePolicy policy; // set by the constructor, before `host` reads it
    SessionHost host { loop,
                       [](vtbackend::PageSize size, std::optional<vtpty::Process::ExecInfo> const&) {
                           return std::make_unique<vtpty::MockPty>(size);
                       },
                       vtbackend::Settings {},
                       crispy::defaultEnvironment(),
                       /*startPumps=*/false,
                       policy };
};

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

coro::Task<void> waitFor(net::EventLoop* loop, std::function<bool()> ready)
{
    using namespace std::chrono_literals;
    for (auto i = 0; i < 2000 && !ready(); ++i)
        co_await loop->delay(1ms);
}

} // namespace

TEST_CASE("stream events fan out to every subscriber independently", "[vthost][host]")
{
    auto source = net::PollEventSource {};
    auto loop = net::EventLoop { source };
    auto host = SessionHost { loop,
                              [](vtbackend::PageSize size, std::optional<vtpty::Process::ExecInfo> const&) {
                                  return std::make_unique<vtpty::MockPty>(size);
                              },
                              vtbackend::Settings {},
                              crispy::defaultEnvironment(),
                              /*startPumps=*/false };
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
    auto capture = logstore::ScopedCapture {};

    auto source = net::testing::ScriptedEventSource {};
    auto loop = net::EventLoop { source };
    auto host = SessionHost { loop,
                              [](vtbackend::PageSize, std::optional<vtpty::Process::ExecInfo> const&) {
                                  return std::unique_ptr<vtpty::Pty> {};
                              },
                              vtbackend::Settings {},
                              crispy::defaultEnvironment(),
                              /*startPumps=*/false };

    CHECK(host.createTab() == nullptr);
    CHECK(host.sessionCount() == 0);
    CHECK(capture.contains("PTY factory failed"));
}

TEST_CASE("reaping an orphaned session after a model refusal is reported", "[vthost][host][diagnostics]")
{
    auto capture = logstore::ScopedCapture {};
    auto h = HostHarness {};
    REQUIRE(h.host.createTab() != nullptr);

    h.host.splitActivePane(vtworkspace::TabId { 4711 }, SplitState::Vertical, 0.5);

    CHECK(h.host.sessionCount() == 1); // the orphan really was reaped
    CHECK(capture.contains("model refused the split of tab 4711"));
    CHECK(capture.contains("reaping orphaned session"));
}

TEST_CASE("session spawn and exit are recorded once each", "[vthost][host][diagnostics]")
{
    auto capture = logstore::ScopedCapture { "vthost.session" };
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
    auto source = net::testing::ScriptedEventSource {};
    auto loop = net::EventLoop { source };
    auto* blocking = static_cast<BlockingPty*>(nullptr);
    auto host = std::make_unique<SessionHost>(
        loop,
        [&blocking](vtbackend::PageSize size, std::optional<vtpty::Process::ExecInfo> const&) {
            auto pty = std::make_unique<BlockingPty>(size);
            blocking = pty.get();
            return pty;
        },
        vtbackend::Settings {},
        crispy::defaultEnvironment(),
        /*startPumps=*/true);

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

TEST_CASE("resizing an unknown pane is reported", "[vthost][host][diagnostics]")
{
    auto capture = logstore::ScopedCapture {};
    auto h = HostHarness {};

    h.host.applyPaneSize(vtworkspace::SessionId { 9999 },
                         vtpty::PageSize { vtpty::LineCount(24), vtpty::ColumnCount(80) });

    CHECK(capture.contains("applyPaneSize: unknown session 9999"));
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
    auto source = net::testing::ScriptedEventSource {};
    auto loop = net::EventLoop { source };
    auto layout = vtworkspace::Layout { .tabs = { leafTab("nvim") } };

    auto host = SessionHost { loop,
                              [](vtbackend::PageSize size, std::optional<vtpty::Process::ExecInfo> const&) {
                                  return std::make_unique<vtpty::MockPty>(size);
                              },
                              vtbackend::Settings {},
                              crispy::defaultEnvironment(),
                              /*startPumps=*/false,
                              vthost::ClientSizePolicy::Latest,
                              layout };

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
    auto source = net::testing::ScriptedEventSource {};
    auto loop = net::EventLoop { source };

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
                              crispy::defaultEnvironment(),
                              /*startPumps=*/false,
                              vthost::ClientSizePolicy::Latest,
                              layout };

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
    auto source = net::testing::ScriptedEventSource {};
    auto loop = net::EventLoop { source };

    auto host = SessionHost { loop,
                              [](vtbackend::PageSize size, std::optional<vtpty::Process::ExecInfo> const&) {
                                  return std::make_unique<vtpty::MockPty>(size);
                              },
                              vtbackend::Settings {},
                              crispy::defaultEnvironment(),
                              /*startPumps=*/false,
                              vthost::ClientSizePolicy::Latest };

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
    auto source = net::testing::ScriptedEventSource {};
    auto loop = net::EventLoop { source };

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
                              crispy::defaultEnvironment(),
                              /*startPumps=*/false,
                              vthost::ClientSizePolicy::Latest,
                              layout };

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
    auto source = net::testing::ScriptedEventSource {};
    auto loop = net::EventLoop { source };

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
                              crispy::defaultEnvironment(),
                              /*startPumps=*/false,
                              vthost::ClientSizePolicy::Latest,
                              layout };

    REQUIRE(host.sessionCount() == 1);
    auto* window = host.model().window(host.windowId());
    REQUIRE(window != nullptr);
    REQUIRE(window->tabCount() == 1);
    REQUIRE(host.terminal(window->tabAt(0)->rootPane()->session()) != nullptr);

    REQUIRE(seen.has_value());
    CHECK(seen->program.empty()); // no command named: program/arguments stay untouched
    CHECK(seen->workingDirectory == std::filesystem::path { "/tmp/project" });
}

TEST_CASE("SessionHost falls back to an empty window when every startup-layout seed is refused",
          "[vthost][host][layout]")
{
    // A PTY factory that always fails (e.g. resource exhaustion at daemon startup) must not
    // leave a half-built or crashed host: realization produces zero tabs, and the window stays
    // empty -- NativeSession::completeHandshake's existing first-attach fallback is unaffected by
    // this test (it lives above SessionHost) but this confirms SessionHost itself degrades
    // gracefully.
    auto source = net::testing::ScriptedEventSource {};
    auto loop = net::EventLoop { source };
    auto layout = vtworkspace::Layout { .tabs = { leafTab("nvim"), leafTab("htop") } };

    auto host = SessionHost { loop,
                              [](vtbackend::PageSize, std::optional<vtpty::Process::ExecInfo> const&)
                                  -> std::unique_ptr<vtpty::Pty> { return nullptr; },
                              vtbackend::Settings {},
                              crispy::defaultEnvironment(),
                              /*startPumps=*/false,
                              vthost::ClientSizePolicy::Latest,
                              layout };

    CHECK(host.sessionCount() == 0);
    auto* window = host.model().window(host.windowId());
    REQUIRE(window != nullptr);
    CHECK(window->tabCount() == 0);
}
