// SPDX-License-Identifier: Apache-2.0
#include <vtpty/MockPty.h>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <chrono>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include <coro/WhenAll.hpp>
#include <net/EventLoop.h>
#include <net/PollEventSource.h>
#include <net/testing/InMemoryTransport.h>
#include <vthost/SessionHost.h>
#include <vthost/tmux/ControlSession.h>
#include <vthost/tmux/TmuxClientModel.h>
#include <vtworkspace/Pane.h>
#include <vtworkspace/Tab.h>

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
                       /*startPumps=*/false };
    net::testing::SocketPair pair = *net::testing::makeSocketPair(loop);
    std::unique_ptr<ControlSession> server = std::make_unique<ControlSession>(
        loop, host, std::move(pair.first), [] { return std::int64_t { 1000 }; });
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

Task<void> driveMirror(ModelHarness* h, vtworkspace::SessionId sessionId, std::uint64_t paneId)
{
    co_await coro::whenAll(h->server->run(), h->gateway->run(), mirrorScenario(h, sessionId, paneId));
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

    h.loop.blockOn(driveMirror(&h, sessionId, paneId));

    CHECK(h.model.windows().begin()->second.panes == std::vector { paneId });
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

    // The window collapses to a single pane: %2 is pruned, %1 resized.
    auto const single = std::format("{:04x},160x50,0,0,1", vthost::tmux::layoutChecksum("160x50,0,0,1"));
    model.layoutChanged(7, single);
    CHECK(model.paneCount() == 1);
    CHECK(model.pane(2) == nullptr);
    CHECK(model.pane(1)->terminal().pageSize()
          == vtpty::PageSize { vtpty::LineCount(50), vtpty::ColumnCount(160) });

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
