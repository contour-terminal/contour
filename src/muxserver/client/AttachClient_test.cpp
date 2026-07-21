// SPDX-License-Identifier: Apache-2.0
#include <vtpty/MockPty.h>

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <memory>
#include <string>

#include <coro/WhenAll.hpp>
#include <muxserver/NativeSession.h>
#include <muxserver/SessionHost.h>
#include <muxserver/TappingPty.h>
#include <muxserver/client/AttachClient.h>
#include <net/EventLoop.h>
#include <net/PollEventSource.h>
#include <net/testing/InMemoryTransport.h>
#include <vtmux/Pane.h>
#include <vtmux/Tab.h>

using coro::Task;
using muxserver::NativeSession;
using muxserver::SessionHost;
using muxserver::client::AttachClient;
using muxserver::client::RemoteScreen;
namespace proto = muxserver::proto;
using namespace std::chrono_literals;

namespace
{

/// The full server<->client pair over one in-memory socket: the REAL
/// NativeSession serves what the REAL AttachClient mirrors.
struct EndToEndHarness
{
    net::PollEventSource source;
    net::EventLoop loop { source };
    SessionHost host { loop,
                       [](vtbackend::PageSize size) { return std::make_unique<vtpty::MockPty>(size); },
                       vtbackend::Settings {},
                       /*startPumps=*/false };
    net::testing::SocketPair pair = *net::testing::makeSocketPair(loop);
    std::unique_ptr<NativeSession> server =
        std::make_unique<NativeSession>(loop, host, std::move(pair.first));
    std::unique_ptr<AttachClient> client = std::make_unique<AttachClient>(loop, std::move(pair.second));
};

/// Waits until @p ready holds (bounded), one loop tick at a time.
Task<void> waitUntil(net::EventLoop* loop, std::function<bool()> ready)
{
    for (auto i = 0; i < 1000 && !ready(); ++i)
        co_await loop->delay(1ms);
}

Task<void> scenario(EndToEndHarness* h, vtmux::SessionId sessionId)
{
    // 1. Attach: the handshake answers and the snapshot mirrors the screen.
    co_await waitUntil(&h->loop, [&] { return !h->client->screens().empty(); });
    REQUIRE(h->client->connected());
    REQUIRE(h->client->screens().contains(sessionId.value));
    {
        auto const& screen = h->client->screens().at(sessionId.value);
        CHECK(screen.columns == 80);
        CHECK(screen.lines == 25);
        CHECK(screen.viewportText().starts_with("hello e2e\n"));
    }

    // 2. Increment: new terminal output becomes a (debounced) delta.
    h->host.terminal(sessionId)->writeToScreen("\r\nsecond line");
    h->server->onScreenUpdated(sessionId); // what the daemon glue wires up
    co_await waitUntil(&h->loop, [&] {
        return h->client->screens().at(sessionId.value).viewportText().contains("second line");
    });
    CHECK(h->client->screens().at(sessionId.value).viewportText().contains("second line"));

    // 3. Input flows back into the pane's PTY.
    h->client->sendInput(sessionId.value, "ls\r");
    auto& tapped = dynamic_cast<muxserver::TappingPty&>(h->host.terminal(sessionId)->device());
    auto& mock = dynamic_cast<vtpty::MockPty&>(tapped.inner());
    co_await waitUntil(&h->loop, [&] { return !mock.stdinBuffer().empty(); });
    CHECK(mock.stdinBuffer() == "ls\r");

    // 4. A resize proposal comes back as an authoritative snapshot.
    h->client->requestResize(100, 40);
    co_await waitUntil(&h->loop, [&] { return h->client->screens().at(sessionId.value).columns == 100; });
    CHECK(h->client->screens().at(sessionId.value).lines == 40);
    CHECK(h->host.pageSize() == vtpty::PageSize { vtpty::LineCount(40), vtpty::ColumnCount(100) });

    h->client->detach(); // ends both run() loops
}

Task<void> driveEndToEnd(EndToEndHarness* h, vtmux::SessionId sessionId)
{
    co_await coro::whenAll(h->server->run(), h->client->run(), scenario(h, sessionId));
}

} // namespace

TEST_CASE("attach mirrors, updates, inputs and resizes end to end", "[muxserver][attach]")
{
    auto h = EndToEndHarness {};
    h.host.createTab();
    auto const sessionId = h.host.model().window(h.host.windowId())->activeTab()->rootPane()->session();
    h.host.terminal(sessionId)->writeToScreen("hello e2e");

    h.loop.blockOn(driveEndToEnd(&h, sessionId));

    // The scenario ran to completion (its REQUIREs did not abort the drive).
    CHECK(h.client->screens().at(sessionId.value).columns == 100);
}

TEST_CASE("RemoteScreen renders blank rows and trims trailing space", "[muxserver][attach]")
{
    auto screen = RemoteScreen {};
    screen.columns = 5;
    screen.lines = 2;

    auto delta = proto::Delta {};
    delta.stableViewportBase = 10;
    auto line = proto::WireLine {};
    line.stableId = 10;
    line.columns = 5;
    for (auto const ch: { U'h', U'i', U'\0', U'\0', U'\0' })
    {
        auto cell = proto::WireCell {};
        cell.codepoint = ch;
        line.cells.push_back(cell);
    }
    delta.lines.push_back(line);
    screen.apply(delta);

    CHECK(screen.viewportText() == "hi\n\n"); // row 11 is unknown -> blank
    CHECK(screen.rowAt(0) != nullptr);
    CHECK(screen.rowAt(1) == nullptr);
}
