// SPDX-License-Identifier: Apache-2.0
#include <vtpty/MockPty.h>

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include <coro/WhenAll.hpp>
#include <muxserver/SessionHost.h>
#include <muxserver/TappingPty.h>
#include <muxserver/tmux/ControlSession.h>
#include <muxserver/tmux/LayoutString.h>
#include <muxserver/tmux/TmuxGateway.h>
#include <net/EventLoop.h>
#include <net/PollEventSource.h>
#include <net/testing/InMemoryTransport.h>
#include <vtmux/Pane.h>
#include <vtmux/Tab.h>

using coro::Task;
using muxserver::SessionHost;
using muxserver::tmux::ControlSession;
using muxserver::tmux::GatewayEvents;
using muxserver::tmux::TmuxGateway;
using namespace std::chrono_literals;

namespace
{

/// Records every notification the gateway dispatches.
struct RecordingEvents final: GatewayEvents
{
    std::vector<uint64_t> windowsAdded;
    std::vector<std::string> layouts;
    std::string output;
    bool sawExit = false;

    void windowAdded(uint64_t window) override { windowsAdded.push_back(window); }
    void layoutChanged(uint64_t, std::string_view layout) override { layouts.emplace_back(layout); }
    void outputReceived(uint64_t, std::string_view bytes) override { output += bytes; }
    void exited(std::string_view) override { sawExit = true; }
};

/// Our gateway attached to OUR control-mode server: the loopback validates
/// both protocol halves against each other (the server itself is oracle-
/// verified against tmux 3.7b).
struct LoopbackHarness
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
    RecordingEvents events;
    std::unique_ptr<TmuxGateway> gateway =
        std::make_unique<TmuxGateway>(loop, std::move(pair.second), events);
};

Task<void> waitUntil(net::EventLoop* loop, std::function<bool()> ready)
{
    for (auto i = 0; i < 1000 && !ready(); ++i)
        co_await loop->delay(1ms);
}

Task<void> scenario(LoopbackHarness* h)
{
    // 1. The opening guard completes: notifications are un-gated.
    co_await waitUntil(&h->loop, [&] { return h->gateway->initialised(); });
    REQUIRE(h->gateway->initialised());

    // 2. new-window: the response correlates FIFO; %window-add and
    //    %layout-change arrive as notifications.
    auto newWindowOk = false;
    h->gateway->sendCommand("new-window",
                            [&](bool ok, std::vector<std::string> const&) { newWindowOk = ok; });
    co_await waitUntil(&h->loop, [&] { return !h->events.windowsAdded.empty() && newWindowOk; });
    REQUIRE(newWindowOk);
    REQUIRE(h->events.windowsAdded.size() == 1);
    REQUIRE_FALSE(h->events.layouts.empty());
    // The advertised layout parses with our own codec.
    CHECK(muxserver::tmux::parseLayout(h->events.layouts.front()).has_value());

    // 3. An unknown command correlates as an error.
    auto errorSeen = false;
    h->gateway->sendCommand("frobnicate", [&](bool ok, std::vector<std::string> const&) { errorSeen = !ok; });

    // 4. list-windows returns its body lines to the right callback.
    auto listing = std::vector<std::string> {};
    h->gateway->sendCommand("list-windows",
                            [&](bool, std::vector<std::string> const& body) { listing = body; });
    co_await waitUntil(&h->loop, [&] { return errorSeen && !listing.empty(); });
    REQUIRE(errorSeen);
    REQUIRE(listing.size() == 1);
    CHECK(listing.front().starts_with("0: @"));

    // 5. sendKeys lands in the pane's PTY, quoted through send-keys -l.
    auto const paneId = h->host.model().window(h->host.windowId())->activeTab()->rootPane()->id().value;
    auto const session = h->host.model().window(h->host.windowId())->activeTab()->rootPane()->session();
    h->gateway->sendKeys(paneId, "echo 'hi'");
    auto& tapped = dynamic_cast<muxserver::TappingPty&>(h->host.terminal(session)->device());
    auto& mock = dynamic_cast<vtpty::MockPty&>(tapped.inner());
    co_await waitUntil(&h->loop, [&] { return !mock.stdinBuffer().empty(); });
    CHECK(mock.stdinBuffer() == "echo 'hi'");

    // 6. Detach: the server answers %exit and both loops wind down.
    h->gateway->detach();
    co_await waitUntil(&h->loop, [&] { return h->events.sawExit; });
    CHECK(h->events.sawExit);
}

Task<void> driveLoopback(LoopbackHarness* h)
{
    co_await coro::whenAll(h->server->run(), h->gateway->run(), scenario(h));
}

} // namespace

TEST_CASE("the gateway drives our control-mode server end to end", "[muxserver][gateway]")
{
    auto h = LoopbackHarness {};
    h.loop.blockOn(driveLoopback(&h));

    CHECK(h.events.sawExit);
    CHECK(h.host.model().window(h.host.windowId())->tabCount() == 1);
}
