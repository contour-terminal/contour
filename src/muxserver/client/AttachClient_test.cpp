// SPDX-License-Identifier: Apache-2.0
#include <vtpty/MockPty.h>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>

#ifndef _WIN32
    #include <csignal>

    #include <unistd.h>
#endif

#include <coro/Cancellation.hpp>
#include <coro/WhenAll.hpp>
#include <coro/WhenAny.hpp>
#include <muxserver/Daemon.h>
#include <muxserver/NativeSession.h>
#include <muxserver/SessionHost.h>
#include <muxserver/TappingPty.h>
#include <muxserver/client/AttachClient.h>
#include <net/EventLoop.h>
#include <net/ISocket.h>
#include <net/PollEventSource.h>
#include <net/testing/CoroTestSupport.h>
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
    net::ISocket* serverConn = pair.first.get(); ///< Captured before the move, to simulate a daemon exit.
    std::unique_ptr<NativeSession> server =
        std::make_unique<NativeSession>(loop, host, std::move(pair.first));
    std::unique_ptr<AttachClient> client = std::make_unique<AttachClient>(loop, std::move(pair.second));
};

Task<void> scenario(EndToEndHarness* h, vtmux::SessionId sessionId)
{
    // 1. Attach: the handshake answers and the snapshot mirrors the screen.
    co_await net::testing::waitUntil(&h->loop, [&] { return !h->client->screens().empty(); });
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
    h->server->sessionScreenUpdated(sessionId); // what the daemon glue wires up
    co_await net::testing::waitUntil(&h->loop, [&] {
        return h->client->screens().at(sessionId.value).viewportText().contains("second line");
    });
    CHECK(h->client->screens().at(sessionId.value).viewportText().contains("second line"));

    // 3. Input flows back into the pane's PTY.
    h->client->sendInput(sessionId.value, "ls\r");
    auto& tapped = dynamic_cast<muxserver::TappingPty&>(h->host.terminal(sessionId)->device());
    auto& mock = dynamic_cast<vtpty::MockPty&>(tapped.inner());
    co_await net::testing::waitUntil(&h->loop, [&] { return !mock.stdinBuffer().empty(); });
    CHECK(mock.stdinBuffer() == "ls\r");

    // 4. A resize proposal comes back as an authoritative snapshot.
    h->client->requestResize(100, 40);
    co_await net::testing::waitUntil(&h->loop,
                                     [&] { return h->client->screens().at(sessionId.value).columns == 100; });
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

TEST_CASE("RemoteScreen drops history the server discarded via the floor", "[muxserver][attach]")
{
    auto screen = RemoteScreen {};
    screen.columns = 5;
    screen.lines = 1;

    // A screen with scrollback: viewport row 10, history rows 7..9 above it.
    auto seed = proto::Delta {};
    seed.stableViewportBase = 10;
    seed.stableFloor = 7; // the server still holds rows >= 7
    for (auto const id: { 7, 8, 9, 10 })
    {
        auto line = proto::WireLine {};
        line.stableId = id;
        line.columns = 5;
        seed.lines.push_back(line);
    }
    screen.apply(seed);
    CHECK(screen.rows.contains(7));
    CHECK(screen.rows.contains(9));

    // A `clear`/CSI 3 J on the server jumps the floor to the viewport base with NO
    // line changes and NO generation bump — the floor is the only signal, and the
    // client must drop the evicted history instead of showing ghost scrollback.
    auto cleared = proto::Delta {};
    cleared.stableViewportBase = 10;
    cleared.stableFloor = 10;
    screen.apply(cleared);

    CHECK_FALSE(screen.rows.contains(7));
    CHECK_FALSE(screen.rows.contains(9));
    CHECK(screen.rows.contains(10)); // the viewport row survives
}

// The attach-flow composition (Daemon.cpp) hard-codes STDIN/STDOUT and a real
// socket connect, so it is not headless-constructible. These tests drive the same
// building blocks it composes — whenAny(run(), parked-input, trackTtySize) and the
// real SigwinchNotifier — against the in-memory server/client pair.
#ifndef _WIN32
namespace
{

using muxserver::SigwinchNotifier;
using muxserver::trackTtySize;

/// Stands in for the parked stdin pump: awaits readability on an fd that never
/// becomes readable, recording that it unwound via cancellation (as pumpStdin does).
Task<void> parkOnFd(net::EventLoop* loop, net::NativeHandle fd, bool* cancelled)
{
    try
    {
        co_await loop->waitReadable(fd);
    }
    catch (coro::OperationCancelled const&)
    {
        *cancelled = true;
        throw; // let the whenAny runner swallow it, exactly like the real input pump
    }
}

/// Mirrors attachFlow's select-semantics: run() raced against a parked input pump.
Task<void> raceRunAgainstPark(EndToEndHarness* h,
                              net::NativeHandle parkFd,
                              bool* parkCancelled,
                              bool* raceReturned)
{
    std::ignore = co_await coro::whenAny(h->client->run(), parkOnFd(&h->loop, parkFd, parkCancelled));
    *raceReturned = true;
}

/// Once the handshake lands, drops the daemon's socket — the client then sees EOF,
/// exactly as it would when a real daemon exits.
Task<void> closeDaemonWhenReady(EndToEndHarness* h)
{
    co_await net::testing::waitUntil(&h->loop, [&] { return h->client->connected(); });
    h->serverConn->close();
}

/// Mirrors attachFlow's resize seam: run() raced against the SIGWINCH size tracker.
Task<void> raceRunAgainstTracker(EndToEndHarness* h, net::NativeHandle winchFd, std::function<void()> propose)
{
    std::ignore =
        co_await coro::whenAny(h->client->run(), trackTtySize(&h->loop, winchFd, std::move(propose)));
}

/// Waits for the initial proposal, fires a real SIGWINCH after bumping the reported
/// width, waits for the re-proposal to reach the daemon, then detaches.
Task<void> driveWinchController(EndToEndHarness* h, std::uint32_t* reportedCols, bool* winchRaised)
{
    co_await net::testing::waitUntil(&h->loop, [&] {
        return h->host.pageSize() == vtpty::PageSize { vtpty::LineCount(30), vtpty::ColumnCount(90) };
    });

    *reportedCols = 120;
    *winchRaised = ::raise(SIGWINCH) == 0;

    co_await net::testing::waitUntil(&h->loop, [&] {
        return h->host.pageSize() == vtpty::PageSize { vtpty::LineCount(30), vtpty::ColumnCount(120) };
    });
    h->client->detach();
}

} // namespace

TEST_CASE("attach flow completes on daemon close with input still parked", "[muxserver][attach]")
{
    auto h = EndToEndHarness {};
    h.host.createTab();

    // A pipe read end standing in for local stdin that NEVER receives input.
    auto stdinFds = std::array<int, 2> {};
    REQUIRE(::pipe(stdinFds.data()) == 0);

    auto parkCancelled = false;
    auto raceReturned = false;
    h.loop.blockOn(net::testing::allOf(h.server->run(),
                                       raceRunAgainstPark(&h, stdinFds[0], &parkCancelled, &raceReturned),
                                       closeDaemonWhenReady(&h)));

    // whenAll only returns once the race resolved: run() completed on the daemon's
    // close and the parked "stdin" sibling was cancelled rather than left hanging.
    CHECK(raceReturned);
    CHECK(parkCancelled);

    ::close(stdinFds[0]);
    ::close(stdinFds[1]);
}

TEST_CASE("SIGWINCH re-proposes the local size to the daemon", "[muxserver][attach]")
{
    auto h = EndToEndHarness {};
    h.host.createTab();

    auto notifier = SigwinchNotifier {};
    REQUIRE(notifier.valid());

    // The proposal reports an injectable width so the winch-driven re-proposal is
    // distinguishable from the initial one (90 -> 120).
    auto reportedCols = std::uint32_t { 90 };
    auto propose = [&] {
        h.client->requestResize(reportedCols, 30);
    };

    auto winchRaised = false;
    h.loop.blockOn(net::testing::allOf(h.server->run(),
                                       raceRunAgainstTracker(&h, notifier.readFd(), propose),
                                       driveWinchController(&h, &reportedCols, &winchRaised)));

    CHECK(winchRaised);
    // 120x30 is a geometry only the SIGWINCH-driven re-proposal could produce; the
    // initial proposal reported 90. So the signal reached the daemon end to end.
    CHECK(h.host.pageSize() == vtpty::PageSize { vtpty::LineCount(30), vtpty::ColumnCount(120) });
}
#endif // !_WIN32
