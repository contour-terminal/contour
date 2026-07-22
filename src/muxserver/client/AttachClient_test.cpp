// SPDX-License-Identifier: Apache-2.0
#include <vtpty/MockPty.h>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <variant>

#ifndef _WIN32
    #include <csignal>

    #include <unistd.h>
#endif

#include <coro/Cancellation.hpp>
#include <coro/WhenAll.hpp>
#include <coro/WhenAny.hpp>
#include <muxserver/Daemon.h>
#include <muxserver/MuxServer.h>
#include <muxserver/NativeSession.h>
#include <muxserver/PduPump.h>
#include <muxserver/SessionHost.h>
#include <muxserver/TappingPty.h>
#include <muxserver/client/AttachClient.h>
#include <net/EventLoop.h>
#include <net/ISocket.h>
#include <net/PollEventSource.h>
#include <net/Sockets.h>
#include <net/Tls.h>
#include <net/WriteQueue.h>
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

TEST_CASE("RemoteScreen tracks, replaces and evicts image cells", "[muxserver][attach]")
{
    auto screen = RemoteScreen {};
    screen.columns = 5;
    screen.lines = 1;

    // A snapshot with an image tile on a history row (7) and the viewport row (10).
    auto seed = proto::Delta {};
    seed.snapshot = 1;
    seed.stableViewportBase = 10;
    seed.stableFloor = 7;
    for (auto const id: { 7, 8, 9, 10 })
    {
        auto line = proto::WireLine {};
        line.stableId = id;
        line.columns = 5;
        seed.lines.push_back(line);
    }
    seed.imageCells.push_back(proto::ImageCellEntry { .stableId = 7, .column = 1, .imageId = 3 });
    seed.imageCells.push_back(proto::ImageCellEntry { .stableId = 10, .column = 0, .imageId = 3 });
    screen.apply(seed);

    REQUIRE(screen.imageAt(7, 1) != nullptr);
    CHECK(screen.imageAt(7, 1)->imageId == 3);
    CHECK(screen.imageAt(10, 0) != nullptr);
    CHECK(screen.imageAt(10, 4) == nullptr); // no entry at that column

    // Redrawing row 10 without an image cell clears that row's image coverage,
    // while an untouched row keeps its own.
    auto redraw = proto::Delta {};
    redraw.stableViewportBase = 10;
    redraw.stableFloor = 7;
    auto redrawn = proto::WireLine {};
    redrawn.stableId = 10;
    redrawn.columns = 5;
    redraw.lines.push_back(redrawn);
    screen.apply(redraw);
    CHECK(screen.imageAt(10, 0) == nullptr);
    CHECK(screen.imageAt(7, 1) != nullptr);

    // A floor jump evicts history row 7, taking its image cells with it.
    auto cleared = proto::Delta {};
    cleared.stableViewportBase = 10;
    cleared.stableFloor = 10;
    screen.apply(cleared);
    CHECK(screen.imageAt(7, 1) == nullptr);
}

TEST_CASE("RemoteScreen.dropImage clears pixels and the cells that referenced it", "[muxserver][attach]")
{
    auto screen = RemoteScreen {};

    auto seed = proto::Delta {};
    seed.snapshot = 1;
    seed.stableViewportBase = 0;
    for (auto const id: { 0, 1 })
    {
        auto line = proto::WireLine {};
        line.stableId = id;
        seed.lines.push_back(line);
    }
    seed.imageCells.push_back(proto::ImageCellEntry { .stableId = 0, .column = 1, .imageId = 3 });
    seed.imageCells.push_back(proto::ImageCellEntry { .stableId = 0, .column = 2, .imageId = 3 });
    seed.imageCells.push_back(proto::ImageCellEntry { .stableId = 1, .column = 0, .imageId = 4 });
    screen.apply(seed);

    // Pretend both images were fetched.
    screen.images.insert_or_assign(3u,
                                   proto::ImageData { .imageId = 3, .width = 1, .height = 1, .data = {} });
    screen.images.insert_or_assign(4u,
                                   proto::ImageData { .imageId = 4, .width = 1, .height = 1, .data = {} });
    screen.requestedImages.insert(3u);
    screen.requestedImages.insert(4u);

    screen.dropImage(3u);

    CHECK(screen.imageData(3) == nullptr);
    CHECK_FALSE(screen.requestedImages.contains(3u));
    CHECK(screen.imageAt(0, 1) == nullptr);
    CHECK(screen.imageAt(0, 2) == nullptr);
    CHECK(screen.imageAt(1, 0) != nullptr); // image 4 is untouched
    CHECK(screen.imageData(4) != nullptr);
}

namespace
{

/// Encodes @p pdu with @p serial and enqueues it onto @p writer (test-only helper).
void enqueuePdu(net::WriteQueue& writer, uint64_t serial, proto::DecodedPdu const& pdu)
{
    auto sink = proto::Writer {};
    proto::encodePdu(sink, serial, pdu);
    auto const bytes = sink.view();
    REQUIRE(writer.enqueue(std::string { reinterpret_cast<char const*>(bytes.data()), bytes.size() }));
}

/// A hand-driven server: answers the handshake, pushes a snapshot whose single
/// row carries an image cell (id 7), then serves the client's FetchImage with
/// @p reply. Exercises the serial-correlated (session-less) image reply path
/// without needing a real rasterized image.
Task<void> fakeImageServer(net::EventLoop* loop, net::ISocket* socket, proto::ImageData reply)
{
    auto writer = net::WriteQueue { *loop, socket, std::size_t { 1 } * 1024 * 1024 };
    co_await muxserver::pumpPdus(socket, [&](proto::DecodedFrame const& frame) {
        if (std::holds_alternative<proto::ClientHello>(frame.pdu))
        {
            enqueuePdu(writer, frame.serial, proto::DecodedPdu { proto::ServerHello {} });
            auto delta = proto::Delta {};
            delta.session = 1;
            delta.snapshot = 1;
            delta.stableViewportBase = 0;
            auto line = proto::WireLine {};
            line.stableId = 0;
            line.columns = 4;
            delta.lines.push_back(line);
            delta.imageCells.push_back(proto::ImageCellEntry { .stableId = 0, .column = 1, .imageId = 7 });
            enqueuePdu(writer, 0, proto::DecodedPdu { delta });
            return true;
        }
        if (auto const* fetch = std::get_if<proto::FetchImage>(&frame.pdu))
        {
            CHECK(fetch->session == 1);
            CHECK(fetch->imageId == 7);
            reply.imageId = fetch->imageId;
            // The reply deliberately carries NO session: only the serial routes it.
            enqueuePdu(writer, frame.serial, proto::DecodedPdu { reply });
            return true;
        }
        return true;
    });
}

/// Waits for image 7's pixels to reach the client's cache, then detaches.
Task<void> awaitImageThenDetach(net::EventLoop* loop, AttachClient* client, bool* seen)
{
    co_await net::testing::waitUntil(loop, [&] {
        auto const it = client->screens().find(1);
        return it != client->screens().end() && it->second.imageData(7) != nullptr;
    });
    auto const& screen = client->screens().at(1);
    auto const* data = screen.imageData(7);
    REQUIRE(data != nullptr);
    CHECK(data->imageId == 7);
    CHECK(data->width == 2);
    CHECK(data->height == 3);
    CHECK(screen.imageAt(0, 1) != nullptr);
    *seen = true;
    client->detach();
}

} // namespace

TEST_CASE("attach fetches image pixels on demand and caches them", "[muxserver][attach]")
{
    auto source = net::PollEventSource {};
    auto loop = net::EventLoop { source };
    auto pair = *net::testing::makeSocketPair(loop);
    auto* serverSock = pair.first.get();
    auto client = AttachClient { loop, std::move(pair.second) };

    auto imageEvent = false;
    client.setImageHandler([&](RemoteScreen const&, uint32_t id) {
        if (id == 7)
            imageEvent = true;
    });

    auto reply = proto::ImageData { .imageId = 0, .width = 2, .height = 3, .data = {} };
    reply.data.resize(static_cast<std::size_t>(2 * 3 * 4), std::byte { 0x80 });

    auto seen = false;
    loop.blockOn(net::testing::allOf(client.run(),
                                     fakeImageServer(&loop, serverSock, reply),
                                     awaitImageThenDetach(&loop, &client, &seen)));

    CHECK(seen);
    CHECK(imageEvent);
}

namespace
{

/// Attaches a client (bearing @p clientToken) to a server that requires
/// @p serverToken, and reports whether a snapshot arrived (accept) or not
/// (reject: the server answers the handshake then drops the connection).
void tokenAttach(std::string serverToken, std::string clientToken, bool* gotSnapshot)
{
    auto source = net::PollEventSource {};
    auto loop = net::EventLoop { source };
    auto host = SessionHost { loop,
                              [](vtbackend::PageSize size) { return std::make_unique<vtpty::MockPty>(size); },
                              vtbackend::Settings {},
                              /*startPumps=*/false };
    host.createTab();
    auto pair = *net::testing::makeSocketPair(loop);
    auto server = NativeSession {
        loop, host, std::move(pair.first), NativeSession::DefaultWriteQueueBytes, std::move(serverToken)
    };
    auto client = AttachClient { loop, std::move(pair.second), std::move(clientToken) };

    auto scenario = [](net::EventLoop* loop, AttachClient* client, bool* got) -> Task<void> {
        // On accept a snapshot arrives quickly; on reject the server drops us and
        // no snapshot ever comes. Bounded poll either way.
        for (auto i = 0; i < 300 && client->screens().empty(); ++i)
            co_await loop->delay(1ms);
        *got = !client->screens().empty();
        client->detach();
    }(&loop, &client, gotSnapshot);

    loop.blockOn(net::testing::allOf(server.run(), client.run(), std::move(scenario)));
}

} // namespace

TEST_CASE("attach enforces a preshared auth token", "[muxserver][attach]")
{
    auto accepted = false;
    tokenAttach("s3cr3t", "s3cr3t", &accepted);
    CHECK(accepted);

    auto rejected = false;
    tokenAttach("s3cr3t", "wrong", &rejected);
    CHECK_FALSE(rejected);

    // No token configured accepts any client (the AF_UNIX default).
    auto open = false;
    tokenAttach("", "whatever", &open);
    CHECK(open);
}

namespace
{

/// Connects over TCP, mirrors the snapshot, detaches, then closes the server so
/// its accept loop unwinds. Records whether a snapshot arrived.
Task<void> tcpAttachDriver(net::EventLoop* loop, muxserver::MuxServer* server, std::uint16_t port, bool* saw)
{
    auto connected = co_await net::connect(loop, "127.0.0.1", port);
    if (connected)
    {
        auto client = AttachClient { *loop, std::move(*connected), "tok" };
        auto scenario = [](net::EventLoop* loop, AttachClient* client, bool* saw) -> Task<void> {
            for (auto i = 0; i < 300 && client->screens().empty(); ++i)
                co_await loop->delay(1ms);
            *saw = !client->screens().empty();
            client->detach();
        }(loop, &client, saw);
        co_await coro::whenAll(client.run(), std::move(scenario));
    }
    server->close();
}

} // namespace

TEST_CASE("attach mirrors over a real TCP transport with token auth", "[muxserver][attach]")
{
    auto source = net::PollEventSource {};
    auto loop = net::EventLoop { source };
    auto host = SessionHost { loop,
                              [](vtbackend::PageSize size) { return std::make_unique<vtpty::MockPty>(size); },
                              vtbackend::Settings {},
                              /*startPumps=*/false };
    host.createTab();

    // A native server on an ephemeral loopback TCP port, token-guarded.
    auto listener = net::listen(loop, "127.0.0.1", 0);
    REQUIRE(listener.has_value());
    auto const port = (*listener)->localPort();
    auto server =
        muxserver::MuxServer { loop, std::move(*listener), muxserver::makeNativeHandler(loop, host, "tok") };

    auto saw = false;
    loop.blockOn(net::testing::allOf(server.serve(), tcpAttachDriver(&loop, &server, port, &saw)));
    CHECK(saw);
}

namespace
{

/// Connects over TCP, wraps the connection in TLS (client role), mirrors the
/// snapshot, detaches, then closes the server. Records whether a snapshot came.
Task<void> tlsTcpAttachDriver(net::EventLoop* loop,
                              muxserver::MuxServer* server,
                              std::shared_ptr<net::ITlsContext> clientTls,
                              std::uint16_t port,
                              bool* saw)
{
    auto connected = co_await net::connect(loop, "127.0.0.1", port);
    if (connected)
    {
        auto client = AttachClient { *loop, clientTls->wrap(std::move(*connected)), "tok" };
        auto scenario = [](net::EventLoop* loop, AttachClient* client, bool* saw) -> Task<void> {
            for (auto i = 0; i < 500 && client->screens().empty(); ++i)
                co_await loop->delay(1ms);
            *saw = !client->screens().empty();
            client->detach();
        }(loop, &client, saw);
        co_await coro::whenAll(client.run(), std::move(scenario));
    }
    server->close();
}

} // namespace

TEST_CASE("attach mirrors over TLS-encrypted TCP with token auth", "[muxserver][attach]")
{
    auto source = net::PollEventSource {};
    auto loop = net::EventLoop { source };
    auto host = SessionHost { loop,
                              [](vtbackend::PageSize size) { return std::make_unique<vtpty::MockPty>(size); },
                              vtbackend::Settings {},
                              /*startPumps=*/false };
    host.createTab();

    auto serverTls = net::makeSelfSignedServerContext();
    REQUIRE(serverTls.has_value());
    auto clientTls = net::makeTlsClientContext();
    REQUIRE(clientTls.has_value());

    auto listener = net::listen(loop, "127.0.0.1", 0);
    REQUIRE(listener.has_value());
    auto const port = (*listener)->localPort();

    // A TLS-wrapping native handler: each accepted socket is encrypted (server
    // role) before the native protocol runs over it — the daemon's TCP path.
    auto nativeHandler = muxserver::makeNativeHandler(loop, host, "tok");
    auto tlsContext = *serverTls;
    auto handler = [tlsContext, nativeHandler](std::unique_ptr<net::ISocket> socket) {
        return nativeHandler(tlsContext->wrap(std::move(socket)));
    };
    auto server = muxserver::MuxServer { loop, std::move(*listener), handler };

    auto saw = false;
    loop.blockOn(
        net::testing::allOf(server.serve(), tlsTcpAttachDriver(&loop, &server, *clientTls, port, &saw)));
    CHECK(saw); // snapshot mirrored across TLS-over-TCP with a valid token
}

namespace
{

/// Waits for the daemon's layout to arrive on the client, then detaches.
Task<void> awaitLayout(net::EventLoop* loop, AttachClient* client, std::optional<proto::LayoutState>* layout)
{
    co_await net::testing::waitUntil(loop, [layout] { return layout->has_value(); });
    client->detach();
}

} // namespace

TEST_CASE("attach receives the daemon's tab and pane layout", "[muxserver][attach]")
{
    auto source = net::PollEventSource {};
    auto loop = net::EventLoop { source };
    auto host = SessionHost { loop,
                              [](vtbackend::PageSize size) { return std::make_unique<vtpty::MockPty>(size); },
                              vtbackend::Settings {},
                              /*startPumps=*/false };
    auto* tab = host.createTab();
    // Split the tab into two panes (a vertical divider at 60/40).
    host.splitActivePane(tab->id(), vtmux::SplitState::Vertical, 0.6);

    auto pair = *net::testing::makeSocketPair(loop);
    auto server = NativeSession { loop, host, std::move(pair.first) };
    auto client = AttachClient { loop, std::move(pair.second) };

    auto layout = std::optional<proto::LayoutState> {};
    client.setLayoutHandler([&layout](proto::LayoutState const& received) { layout = received; });

    loop.blockOn(net::testing::allOf(server.run(), client.run(), awaitLayout(&loop, &client, &layout)));

    REQUIRE(layout.has_value());
    REQUIRE(layout->tabs.size() == 1);
    auto const& root = layout->tabs.front().root;
    CHECK(root.split == std::to_underlying(vtmux::SplitState::Vertical)); // an internal split node
    CHECK(root.ratio == 6000);                                            // 0.6 x 10000
    REQUIRE(root.children.size() == 2);
    // Both children are leaves carrying distinct sessions.
    CHECK(root.children[0].split == 0);
    CHECK(root.children[1].split == 0);
    CHECK(root.children[0].session != 0);
    CHECK(root.children[1].session != 0);
    CHECK(root.children[0].session != root.children[1].session);
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
