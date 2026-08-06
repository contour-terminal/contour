// SPDX-License-Identifier: Apache-2.0
#include <vtpty/MockPty.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <chrono>
#include <format>
#include <functional>
#include <memory>
#include <ranges>
#include <string>
#include <vector>

#include <coro/WhenAll.hpp>
#include <net/EventLoop.hpp>
#include <net/PollEventSource.hpp>
#include <net/testing/InMemoryTransport.hpp>
#include <vthost/SessionHost.hpp>
#include <vthost/tmux/ControlSession.hpp>
#include <vthost/tmux/TmuxClientModel.hpp>
#include <vtworkspace/Pane.hpp>
#include <vtworkspace/Tab.hpp>

using coro::Task;
using vthost::SessionHost;
using vthost::tmux::ControlSession;
using vthost::tmux::TmuxClientModel;
using vthost::tmux::TmuxGateway;
using namespace std::chrono_literals;

namespace
{

Task<void> waitFor(net::EventLoop* loop, std::function<bool()> ready)
{
    for (auto i = 0; i < 2000 && !ready(); ++i)
        co_await loop->delay(1ms);
}

/// Gateway + model attached to OUR control-mode server over one in-memory
/// socket: window enumeration, history replay and live output mirror the
/// server-side session end to end.
struct ModelHarness
{
    net::PollEventSource source;
    net::EventLoop loop { source };
    SessionHost host { loop,
                       [](vtbackend::PageSize size) { return std::make_unique<vtpty::MockPty>(size); },
                       vtbackend::Settings {},
                       crispy::defaultEnvironment(),
                       /*startPumps=*/false };
    net::testing::SocketPair pair = *net::testing::makeSocketPair(loop);
    std::unique_ptr<ControlSession> server = std::make_unique<ControlSession>(
        loop, host, vthost::ConnectionId { .endpoint = "test", .index = 1 }, std::move(pair.first), [] {
            return std::int64_t { 1000 };
        });
    TmuxClientModel model;
    std::unique_ptr<TmuxGateway> gateway = std::make_unique<TmuxGateway>(loop, std::move(pair.second), model);

    ModelHarness() { model.bind(*gateway); }
};

Task<void> mirrorScenario(ModelHarness* h, vtworkspace::SessionId sessionId, std::uint64_t paneId)
{
    // 1. Attach: %session-changed triggers window enumeration; the pane's
    //    history (written before attach) arrives via capture-pane replay.
    co_await waitFor(&h->loop, [&] {
        auto* pane = h->model.pane(paneId);
        return pane != nullptr && pane->pageText().contains("history-line");
    });
    REQUIRE(h->model.windows().size() == 1);
    REQUIRE(h->model.paneCount() == 1);
    REQUIRE(h->model.pane(paneId) != nullptr);
    CHECK(h->model.pane(paneId)->pageText().contains("history-line"));

    // 2. Live %output lands on top of the replayed history.
    h->host.subscribeStream(h->server.get());
    h->server->sessionOutput(sessionId, "\r\nlive-line");
    co_await waitFor(&h->loop, [&] { return h->model.pane(paneId)->pageText().contains("live-line"); });
    // The newline at the replayed page's bottom row scrolls one line — the
    // replay left the cursor exactly where a full-page capture ends.
    CHECK(h->model.pane(paneId)->pageText().contains("live-line"));

    h->gateway->detach();
}

/// Runs @p scenario against a live server and gateway. whenAll yields an awaiter rather than a
/// Task, so blockOn needs this wrapper.
Task<void> drive(ModelHarness* h, Task<void> scenario)
{
    co_await coro::whenAll(h->server->run(), h->gateway->run(), std::move(scenario));
}

} // namespace

TEST_CASE("the client model mirrors windows, history and live output", "[vthost][tmuxclient]")
{
    auto h = ModelHarness {};
    h.host.createTab();
    auto* tab = h.host.model().window(h.host.windowId())->activeTab();
    auto const sessionId = tab->rootPane()->session();
    auto const paneId = tab->rootPane()->id().value;
    h.host.terminal(sessionId)->writeToScreen("history-line");

    h.loop.blockOn(drive(&h, mirrorScenario(&h, sessionId, paneId)));

    CHECK(h.model.windows().begin()->second.panes == std::vector { paneId });
}

TEST_CASE("the replay carries scrollback and not just the visible page", "[vthost][tmuxclient]")
{
    // The replay used to be `capture-pane -peqJ`, whose default start line is the TOP OF THE
    // VISIBLE PAGE — so a mirrored pane opened showing the current screen and nothing else, and
    // everything the session scrolled away before we attached was simply unreachable. Our own
    // control-mode server (the one this harness drives) was already answering `-S -` for tmux
    // clients, so the two directions disagreed about what an attach replays.
    //
    // Two halves have to hold for this to work, and the test fails if either regresses: the
    // command has to ASK for the history (`-S -`), and the replay terminal has to have somewhere
    // to PUT it (a PaneView built with zero scrollback would drop it just as silently).
    auto h = ModelHarness {};
    h.host.createTab();
    auto* tab = h.host.model().window(h.host.windowId())->activeTab();
    auto const sessionId = tab->rootPane()->session();
    auto const paneId = tab->rootPane()->id().value;

    // Written before the loop runs, so before the attach: far more than the 25-line page holds.
    auto lines = std::string {};
    for (auto i = 0; i < 40; ++i)
        lines += std::format("row-{}\r\n", i);
    h.host.terminal(sessionId)->writeToScreen(lines);

    auto scenario = [](ModelHarness* h, std::uint64_t paneId) -> Task<void> {
        co_await waitFor(&h->loop, [&] {
            auto* pane = h->model.pane(paneId);
            return pane != nullptr && pane->pageText().contains("row-39");
        });
        auto* pane = h->model.pane(paneId);
        REQUIRE(pane != nullptr);

        auto const& grid = pane->terminal().primaryScreen().grid();
        // row-0 scrolled off long before the attach, so it can only be here if the replay reached
        // past the page. Read at the bottom of the history rather than searched for: that is where
        // the session's oldest line belongs, so it also pins the order the replay arrived in.
        REQUIRE(grid.historyLineCount() > vtbackend::LineCount(0));
        CHECK(
            grid.lineText(vtbackend::LineOffset(-unbox<int>(grid.historyLineCount()))).starts_with("row-0"));

        h->gateway->detach();
    }(&h, paneId);

    h.loop.blockOn(drive(&h, std::move(scenario)));
}

TEST_CASE("the replay join keeps a capture's leading blank rows", "[vthost][tmuxclient]")
{
    // A real tmux server reports an untouched row as an EMPTY string, so a pane whose history
    // begins with blank rows (an Enter on an empty prompt) starts the capture with them. Separating
    // rows on "the accumulator is still empty" appended nothing AND emitted no line break for those
    // rows, shifting the whole replay up by as many rows — after which the live %output stream
    // continued from a cursor above where the real pane has it, and the shell's next prompt
    // overwrote the tail of the replayed output.
    using vthost::tmux::joinReplayRows;

    CHECK(joinReplayRows({ "", "", "$ ls" }) == "\r\n\r\n$ ls");
    CHECK(joinReplayRows({ "one", "", "three" }) == "one\r\n\r\nthree");
    // No trailing newline after the last row: it would scroll the first replayed row off the page.
    CHECK(joinReplayRows({ "one", "two" }) == "one\r\ntwo");
    CHECK(joinReplayRows({ "only" }) == "only");
    CHECK(joinReplayRows({}).empty());
    // Trailing blanks are rows too — they place the cursor where the real pane has it.
    CHECK(joinReplayRows({ "text", "", "" }) == "text\r\n\r\n");
}

TEST_CASE("layout ingest creates, resizes and prunes panes", "[vthost][tmuxclient]")
{
    // No gateway bound: pure model behaviour (panes count as replayed).
    auto model = TmuxClientModel {};

    // A 160x50 window split side-by-side into %1 (80 wide) and %2 (79 wide).
    auto const split = "5e willy,160x50,0,0{80x50,0,0,1,79x50,81,0,2}";
    // Compute the real checksum so parseLayout accepts the string.
    auto const body = std::string_view { split }.substr(std::string_view { split }.find(',') + 1);
    auto const layout = std::format("{:04x},{}", vthost::tmux::layoutChecksum(body), std::string { body });

    model.layoutChanged(7, layout);
    REQUIRE(model.paneCount() == 2);
    REQUIRE(model.pane(1) != nullptr);
    CHECK(model.pane(1)->terminal().pageSize()
          == vtpty::PageSize { vtpty::LineCount(50), vtpty::ColumnCount(80) });
    CHECK(model.windows().at(7).panes == std::vector<std::uint64_t> { 1, 2 });
    CHECK(model.windows().at(7).tree->leafCount() == 2);

    // The window collapses to a single pane: %2 leaves the window, %1 is resized.
    auto const single = std::format("{:04x},160x50,0,0,1", vthost::tmux::layoutChecksum("160x50,0,0,1"));
    model.layoutChanged(7, single);
    CHECK(model.paneCount() == 1);   // no window holds %2 any more...
    CHECK(model.pane(2) != nullptr); // ...but it is PARKED, not destroyed: the ingest cannot yet
                                     // tell a close from a move to a sibling window, and its live
                                     // sink must keep taking %output until the verdict settles.
    CHECK(model.pane(1)->terminal().pageSize()
          == vtpty::PageSize { vtpty::LineCount(50), vtpty::ColumnCount(160) });

    // The burst boundary settles it: nothing adopted %2, so it really is closed.
    model.notificationsDrained();
    CHECK(model.pane(2) == nullptr);

    model.windowClosed(7);
    CHECK(model.paneCount() == 0);
    CHECK(model.windows().empty());
}

namespace
{

/// A frontend's stand-in pane backend (the GUI injects a ChannelPty feeder).
struct RecordingSink final: vthost::tmux::PaneSink
{
    std::string received;
    int columns = 0;
    int lines = 0;

    void feed(std::string_view bytes) override { received.append(bytes); }
    void resize(int newColumns, int newLines) override
    {
        columns = newColumns;
        lines = newLines;
    }
};

/// Records every structural notification a frontend would realize.
struct RecordingModelEvents final: vthost::tmux::TmuxModelEvents
{
    std::vector<std::string> log;

    void windowAdded(uint64_t window) override { log.push_back(std::format("windowAdded:{}", window)); }
    void windowClosed(uint64_t window) override { log.push_back(std::format("windowClosed:{}", window)); }
    void windowRenamed(uint64_t window, std::string const& name) override
    {
        log.push_back(std::format("windowRenamed:{}:{}", window, name));
    }
    void paneAdded(uint64_t window, uint64_t pane, int columns, int lines) override
    {
        log.push_back(std::format("paneAdded:{}:{}:{}x{}", window, pane, columns, lines));
    }
    void paneRemoved(uint64_t window, uint64_t pane) override
    {
        log.push_back(std::format("paneRemoved:{}:{}", window, pane));
    }
    void paneMoved(uint64_t fromWindow, uint64_t toWindow, uint64_t pane) override
    {
        log.push_back(std::format("paneMoved:{}:{}:{}", fromWindow, toWindow, pane));
    }
    void layoutTreeChanged(uint64_t window) override
    {
        log.push_back(std::format("layoutTreeChanged:{}", window));
    }
    void exited(std::string const& reason) override { log.push_back(std::format("exited:{}", reason)); }
};

[[nodiscard]] std::string checksummedLayout(std::string_view body)
{
    return std::format("{:04x},{}", vthost::tmux::layoutChecksum(body), std::string { body });
}

} // namespace

TEST_CASE("injected sinks and observers see the mirrored structure", "[vthost][tmuxclient]")
{
    auto* lastSink = static_cast<RecordingSink*>(nullptr);
    auto model = TmuxClientModel { [&](uint64_t /*pane*/, int columns, int lines) {
        auto sink = std::make_unique<RecordingSink>();
        sink->columns = columns;
        sink->lines = lines;
        lastSink = sink.get();
        return sink;
    } };
    auto events = RecordingModelEvents {};
    model.subscribe(&events);

    model.windowAdded(7);
    model.layoutChanged(7, checksummedLayout("160x50,0,0{80x50,0,0,1,79x50,81,0,2}"));
    REQUIRE(model.paneCount() == 2);
    CHECK(model.pane(1) == nullptr); // custom sinks are not replay views

    // Live output reaches the injected sink (no gateway: panes are replayed).
    model.outputReceived(2, "live-bytes");
    REQUIRE(lastSink != nullptr);
    CHECK(lastSink->received == "live-bytes");

    model.layoutChanged(7, checksummedLayout("160x50,0,0,1"));
    // %2 leaving the layout is not reported removed inside the prune ingest: that
    // ingest cannot yet tell a close from a move to a sibling window, so it parks
    // %2. The verdict settles at the burst boundary — once the batch drained and
    // no adoption claimed it, it is a genuine close.
    model.notificationsDrained();
    model.windowRenamed(7, "renamed");
    model.exited("done");
    model.windowClosed(7);

    auto const expected = std::vector<std::string> {
        "windowAdded:7",       "paneAdded:7:1:80x50", "paneAdded:7:2:79x50",     "layoutTreeChanged:7",
        "layoutTreeChanged:7", "paneRemoved:7:2",     "windowRenamed:7:renamed", "exited:done",
        "paneRemoved:7:1",     "windowClosed:7",
    };
    CHECK(events.log == expected);

    // An unsubscribed observer hears nothing further.
    model.unsubscribe(&events);
    model.windowAdded(9);
    CHECK(events.log == expected);
}

TEST_CASE("a pane moved between windows survives either layout-change order", "[vthost][tmuxclient]")
{
    // No gateway: panes are backed by replay PaneViews (replayed immediately),
    // so pane() resolves and %output routes to a real terminal we can inspect.
    auto model = TmuxClientModel {};
    auto events = RecordingModelEvents {};
    model.subscribe(&events);

    // @1 holds %1 and %2; @2 holds %3. Seed %2 with identifiable content so we
    // can prove its terminal is re-parented, not destroyed and recreated.
    model.layoutChanged(1, checksummedLayout("160x50,0,0{80x50,0,0,1,79x50,81,0,2}"));
    model.layoutChanged(2, checksummedLayout("160x50,0,0,3"));
    REQUIRE(model.paneCount() == 3);
    model.outputReceived(2, "before-move");
    REQUIRE(model.pane(2) != nullptr);
    REQUIRE(model.pane(2)->pageText().contains("before-move"));

    // join-pane moves %2 from @1 into @2. tmux emits a %layout-change for both
    // windows; the model must survive whichever arrives first.
    auto const dstAdopts = checksummedLayout("160x50,0,0{80x50,0,0,3,79x50,81,0,2}");
    auto const srcDrops = checksummedLayout("160x50,0,0,1");

    SECTION("destination-first (the reviewer's scenario)")
    {
        model.layoutChanged(2, dstAdopts); // @2 adopts %2 first
        model.layoutChanged(1, srcDrops);  // @1's stale layout-change arrives after
    }
    SECTION("source-first (real tmux order: pane briefly in neither window)")
    {
        model.layoutChanged(1, srcDrops);  // @1 drops %2 first
        model.layoutChanged(2, dstAdopts); // @2 adopts %2 after
    }

    // Whichever order: the live pane survived and now belongs to @2.
    CHECK(model.paneCount() == 3);
    REQUIRE(model.pane(2) != nullptr);
    CHECK(model.windows().at(1).panes == std::vector<std::uint64_t> { 1 });
    CHECK(model.windows().at(2).panes == std::vector<std::uint64_t> { 3, 2 });

    // The move is reported as a re-parent, never as a destroy: no paneRemoved
    // fires for the pane (its %output would otherwise be dropped afterwards).
    CHECK(std::ranges::find(events.log, "paneMoved:1:2:2") != events.log.end());
    CHECK(
        std::ranges::none_of(events.log, [](std::string const& e) { return e.starts_with("paneRemoved"); }));

    // The same terminal kept its prior content and still routes fresh %output.
    CHECK(model.pane(2)->pageText().contains("before-move"));
    model.outputReceived(2, "after-move");
    CHECK(model.pane(2)->pageText().contains("after-move"));
}

TEST_CASE("a pane moved into a NEW window survives the interleaved %window-add", "[vthost][tmuxclient]")
{
    // No gateway: replay PaneViews resolve immediately, so we can inspect content.
    auto model = TmuxClientModel {};
    auto events = RecordingModelEvents {};
    model.subscribe(&events);

    // @1 holds %1 and %2; seed %2 so we can prove it is re-parented, not recreated.
    model.layoutChanged(1, checksummedLayout("160x50,0,0{80x50,0,0,1,79x50,81,0,2}"));
    model.notificationsDrained();
    model.outputReceived(2, "keep-me");
    REQUIRE(model.pane(2) != nullptr);
    REQUIRE(model.pane(2)->pageText().contains("keep-me"));

    // break-pane moves %2 into a brand-new window @2. The real tmux order parks
    // %2 (source %layout-change), THEN announces the new window, THEN adopts it —
    // the %window-add lands mid-move and must not destroy the parked pane.
    model.layoutChanged(1, checksummedLayout("160x50,0,0,1")); // @1 drops %2 → parked
    model.windowAdded(2);                                      // announced BETWEEN park and reclaim
    model.layoutChanged(2, checksummedLayout("160x50,0,0,2")); // @2 adopts %2
    model.notificationsDrained();                              // burst boundary

    CHECK(model.paneCount() == 2);
    REQUIRE(model.pane(2) != nullptr);
    CHECK(model.windows().at(2).panes == std::vector<std::uint64_t> { 2 });
    // The move is a re-parent (paneMoved), never a destroy (paneRemoved).
    CHECK(std::ranges::find(events.log, "paneMoved:1:2:2") != events.log.end());
    CHECK(
        std::ranges::none_of(events.log, [](std::string const& e) { return e.starts_with("paneRemoved"); }));
    // The live terminal kept its content and still routes fresh %output.
    CHECK(model.pane(2)->pageText().contains("keep-me"));
    model.outputReceived(2, "after-move");
    CHECK(model.pane(2)->pageText().contains("after-move"));
}

// %output arriving WHILE a pane is parked mid-move must reach its live sink. Resolving a pane
// through `_panes` alone dropped it: the source window's %layout-change parks the entry, and every
// notification until the destination's reclaims it hit "unknown pane" and returned. That window is
// exactly what the parking exists to survive — a running build's log had a permanent hole in it.
TEST_CASE("output for a pane parked mid-move reaches its live sink", "[vthost][tmuxclient]")
{
    auto model = TmuxClientModel {};
    auto events = RecordingModelEvents {};
    model.subscribe(&events);

    model.layoutChanged(1, checksummedLayout("160x50,0,0{80x50,0,0,1,79x50,81,0,2}"));
    model.notificationsDrained();
    model.outputReceived(2, "before-move\r\n");
    REQUIRE(model.pane(2) != nullptr);
    REQUIRE(model.pane(2)->pageText().contains("before-move"));

    // Source-first (real tmux order): @1 drops %2, so it is parked in `_detached`.
    model.layoutChanged(1, checksummedLayout("160x50,0,0,1"));

    // The pane is live and addressable even while parked...
    REQUIRE(model.pane(2) != nullptr);
    // ...and output landing in the gap is applied rather than discarded.
    model.outputReceived(2, "during-move\r\n");
    CHECK(model.pane(2)->pageText().contains("during-move"));

    // The destination reclaims it; nothing was lost across the whole move.
    model.layoutChanged(2, checksummedLayout("160x50,0,0,2"));
    model.notificationsDrained();
    model.outputReceived(2, "after-move");

    REQUIRE(model.pane(2) != nullptr);
    auto const text = model.pane(2)->pageText();
    CHECK(text.contains("before-move"));
    CHECK(text.contains("during-move"));
    CHECK(text.contains("after-move"));
    CHECK(std::ranges::find(events.log, "paneMoved:1:2:2") != events.log.end());
}

// A pane that really IS closed still swallows its output, rather than the fix above turning every
// unknown pane into a live one.
TEST_CASE("output for a genuinely closed pane is still dropped", "[vthost][tmuxclient]")
{
    auto model = TmuxClientModel {};
    model.layoutChanged(1, checksummedLayout("160x50,0,0{80x50,0,0,1,79x50,81,0,2}"));
    model.layoutChanged(1, checksummedLayout("160x50,0,0,1"));
    model.notificationsDrained(); // the burst boundary settles the verdict: %2 closed

    CHECK(model.paneCount() == 1);
    CHECK(model.pane(2) == nullptr);
    model.outputReceived(2, "into-the-void"); // must not resurrect or crash
    CHECK(model.paneCount() == 1);
}
