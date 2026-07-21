// SPDX-License-Identifier: Apache-2.0
#include <vtpty/MockPty.h>

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <functional>
#include <memory>
#include <string>

#include <coro/WhenAll.hpp>
#include <muxserver/SessionHost.h>
#include <muxserver/tmux/ControlSession.h>
#include <muxserver/tmux/TmuxClientModel.h>
#include <net/EventLoop.h>
#include <net/PollEventSource.h>
#include <net/testing/InMemoryTransport.h>
#include <vtmux/Pane.h>
#include <vtmux/Tab.h>

using coro::Task;
using muxserver::SessionHost;
using muxserver::tmux::ControlSession;
using muxserver::tmux::TmuxClientModel;
using muxserver::tmux::TmuxGateway;
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

Task<void> mirrorScenario(ModelHarness* h, vtmux::SessionId sessionId, std::uint64_t paneId)
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

Task<void> driveMirror(ModelHarness* h, vtmux::SessionId sessionId, std::uint64_t paneId)
{
    co_await coro::whenAll(h->server->run(), h->gateway->run(), mirrorScenario(h, sessionId, paneId));
}

} // namespace

TEST_CASE("the client model mirrors windows, history and live output", "[muxserver][tmuxclient]")
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

TEST_CASE("layout ingest creates, resizes and prunes panes", "[muxserver][tmuxclient]")
{
    // No gateway bound: pure model behaviour (panes count as replayed).
    auto model = TmuxClientModel {};

    // A 160x50 window split side-by-side into %1 (80 wide) and %2 (79 wide).
    auto const split = "5e willy,160x50,0,0{80x50,0,0,1,79x50,81,0,2}";
    // Compute the real checksum so parseLayout accepts the string.
    auto const body = std::string_view { split }.substr(std::string_view { split }.find(',') + 1);
    auto const layout = std::format("{:04x},{}", muxserver::tmux::layoutChecksum(body), std::string { body });

    model.layoutChanged(7, layout);
    REQUIRE(model.paneCount() == 2);
    REQUIRE(model.pane(1) != nullptr);
    CHECK(model.pane(1)->terminal().pageSize()
          == vtpty::PageSize { vtpty::LineCount(50), vtpty::ColumnCount(80) });
    CHECK(model.windows().at(7).panes == std::vector<std::uint64_t> { 1, 2 });
    CHECK(model.windows().at(7).tree->leafCount() == 2);

    // The window collapses to a single pane: %2 is pruned, %1 resized.
    auto const single = std::format("{:04x},160x50,0,0,1", muxserver::tmux::layoutChecksum("160x50,0,0,1"));
    model.layoutChanged(7, single);
    CHECK(model.paneCount() == 1);
    CHECK(model.pane(2) == nullptr);
    CHECK(model.pane(1)->terminal().pageSize()
          == vtpty::PageSize { vtpty::LineCount(50), vtpty::ColumnCount(160) });

    model.windowClosed(7);
    CHECK(model.paneCount() == 0);
    CHECK(model.windows().empty());
}
