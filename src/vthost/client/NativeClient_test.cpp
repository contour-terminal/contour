// SPDX-License-Identifier: Apache-2.0
#include <vtpty/MockPty.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <format>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <ranges>
#include <set>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

#ifndef _WIN32
    #include <csignal>

    #include <unistd.h>
#endif

#include <core/async/Cancellation.hpp>
#include <core/async/WhenAll.hpp>
#include <core/async/WhenAny.hpp>
#include <core/log/LogSink.hpp>
#include <core/net/EventLoop.hpp>
#include <core/net/ISocket.hpp>
#include <core/net/IoBackend.hpp>
#include <core/net/Sockets.hpp>
#include <core/net/Tls.hpp>
#include <core/net/WriteQueue.hpp>
#include <core/net/testing/CoroTestSupport.hpp>
#include <core/net/testing/InMemoryTransport.hpp>

#include <vthost/ConnectionAcceptor.hpp>
#include <vthost/Daemon.hpp>
#include <vthost/HostIdentity.hpp>
#include <vthost/LoopDrain.hpp>
#include <vthost/NativeSession.hpp>
#include <vthost/PduPump.hpp>
#include <vthost/SessionHost.hpp>
#include <vthost/SessionSettings.hpp>
#include <vthost/TappingPty.hpp>
#include <vthost/client/LayoutReconstruction.hpp>
#include <vthost/client/NativeClient.hpp>
#ifdef CONTOUR_EXTENSIONS
    #include <vthost/DaemonExtensions.hpp>
    #include <vthost/client/DaemonGrantReplies.hpp>
#endif
#include <vthost/testing/FakeExtensionHosting.hpp>
#include <vthost/testing/FakeProfileCatalog.hpp>
#include <vthost/testing/PreNegotiationCodec.hpp>
#include <vthost/testing/ScenarioGuard.hpp>
#include <vthost/testing/ShippedCodec.hpp>
#include <vthost/testing/TestHelpers.hpp>
#include <vtworkspace/Pane.hpp>
#include <vtworkspace/Tab.hpp>

using core::async::Task;
using vthost::NativeSession;
using vthost::SessionHost;
using vthost::client::NativeClient;
using vthost::client::RemoteScreen;
using vthost::testing::finishedWire;
using vthost::testing::guardScenario;
using vthost::testing::newSession;
using vthost::testing::promptingWire;
using vthost::testing::runningWire;
namespace proto = vthost::proto;
using namespace std::chrono_literals;

namespace
{

/// @param token The preshared token each connection must present; empty for none.
/// @return The services of a native endpoint guarded by @p token.
vthost::NativeSessionServices tokenGuarded(std::string token)
{
    if (token.empty())
        return vthost::NativeSessionServices {};
    return vthost::NativeSessionServices { .handshake = {
                                               .acceptedTokens = { { .token = std::move(token) } } } };
}

/// The full server<->client pair over one in-memory socket: the REAL
/// NativeSession serves what the REAL NativeClient mirrors.
/// @return An empty snapshot PDU marked as @p part — the scaffolding every run test needs.
proto::Delta snapshotPiece(proto::SnapshotPart part)
{
    auto delta = proto::Delta {};
    delta.snapshot = 1;
    delta.snapshotPart = std::to_underlying(part);
    return delta;
}

struct EndToEndHarness
{
    std::unique_ptr<core::net::IoBackend> source = core::net::makeDefaultBackend();
    core::net::EventLoop loop { *source };
    SessionHost host { loop,
                       [](vtbackend::PageSize size, std::optional<vtpty::Process::ExecInfo> const&) {
                           return std::make_unique<vtpty::MockPty>(size);
                       },
                       vtbackend::Settings {},
                       core::defaultEnvironment(),
                       vthost::SessionHostConfig { .pumping = vthost::SessionPumping::Manual } };
    core::net::testing::SocketPair pair = *core::net::testing::makeSocketPair(loop);
    core::net::ISocket* serverConn =
        pair.first.get(); ///< Captured before the move, to simulate a daemon exit.
    std::unique_ptr<NativeSession> server = std::make_unique<NativeSession>(
        loop, host, vthost::ConnectionId { .endpoint = "test", .index = 1 }, std::move(pair.first));
    std::unique_ptr<NativeClient> client =
        std::make_unique<NativeClient>(loop,
                                       std::move(pair.second),
                                       NativeClient::HandshakeOptions {},
                                       vtbackend::CommandBlockStoreLimits {},
                                       NativeClient::UpdateHandler {},
                                       NativeClient::ImageHandler {},
                                       NativeClient::SessionEventHandler {},
                                       NativeClient::LayoutHandler {});
};

Task<void> scenario(EndToEndHarness* h, vtworkspace::SessionId sessionId)
{
    // 1. Attach: the handshake answers and the snapshot mirrors the screen.
    co_await core::net::testing::waitUntil(&h->loop, [&] { return !h->client->screens().empty(); });
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
    co_await core::net::testing::waitUntil(&h->loop, [&] {
        return h->client->screens().at(sessionId.value).viewportText().contains("second line");
    });
    CHECK(h->client->screens().at(sessionId.value).viewportText().contains("second line"));

    // 3. Input flows back into the pane's PTY.
    h->client->sendInput(sessionId.value, "ls\r");
    auto& tapped = dynamic_cast<vthost::TappingPty&>(h->host.terminal(sessionId)->device());
    auto& mock = dynamic_cast<vtpty::MockPty&>(tapped.inner());
    co_await core::net::testing::waitUntil(&h->loop, [&] { return !mock.stdinBuffer().empty(); });
    CHECK(mock.stdinBuffer() == "ls\r");

    // 4. A client-area proposal comes back as an authoritative snapshot.
    h->client->requestResize(100, 40);
    co_await core::net::testing::waitUntil(
        &h->loop, [&] { return h->client->screens().at(sessionId.value).columns == 100; });
    CHECK(h->client->screens().at(sessionId.value).lines == 40);
    CHECK(h->host.pageSize() == vtpty::PageSize { vtpty::LineCount(40), vtpty::ColumnCount(100) });

    // 5. A per-pane report sizes THAT pane's PTY, leaving the client area alone.
    h->client->resizePane(sessionId.value, 70, 40);
    co_await core::net::testing::waitUntil(
        &h->loop, [&] { return h->client->screens().at(sessionId.value).columns == 70; });
    CHECK(h->host.terminal(sessionId)->totalPageSize().columns.value == 70);
    CHECK(h->host.pageSize() == vtpty::PageSize { vtpty::LineCount(40), vtpty::ColumnCount(100) });

    h->client->detach(); // ends both run() loops
}

Task<void> driveEndToEnd(EndToEndHarness* h, vtworkspace::SessionId sessionId)
{
    co_await core::async::whenAll(h->server->run(), h->client->run(), scenario(h, sessionId));
}

} // namespace

TEST_CASE("attach mirrors, updates, inputs and resizes end to end", "[vthost][attach]")
{
    auto h = EndToEndHarness {};
    auto const sessionId = newSession(h.host);
    h.host.terminal(sessionId)->writeToScreen("hello e2e");

    h.loop.blockOn(driveEndToEnd(&h, sessionId));

    // The scenario ran to completion (no REQUIRE in it aborted the drive): the mirror carries the
    // per-pane width step 5 reported, not the client area step 4 proposed.
    CHECK(h.client->screens().at(sessionId.value).columns == 70);
}

TEST_CASE("RemoteScreen renders blank rows and trims trailing space", "[vthost][attach]")
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

TEST_CASE("RemoteScreen accumulates a chunked snapshot and clears only on its first piece",
          "[vthost][attach]")
{
    // A snapshot too large for one frame arrives as a run. Only its FIRST piece may clear what
    // the mirror holds -- a Middle or Last piece that cleared would throw away the rows its own
    // run just delivered, leaving the mirror showing the tail of its own snapshot.
    auto const rowAt = [](int64_t stableId, char32_t ch) {
        auto line = proto::WireLine {};
        line.stableId = stableId;
        line.columns = 1;
        auto cell = proto::WireCell {};
        cell.codepoint = ch;
        line.cells.push_back(cell);
        return line;
    };
    auto const piece = [&](proto::SnapshotPart part, int64_t stableId, char32_t ch) {
        auto delta = snapshotPiece(part);
        delta.stableViewportBase = 0;
        delta.lines.push_back(rowAt(stableId, ch));
        return delta;
    };

    auto screen = RemoteScreen {};
    screen.columns = 1;
    screen.lines = 3;

    // Something to be superseded, so "the first piece clears" is actually observable.
    screen.apply(piece(proto::SnapshotPart::Whole, 0, U'o'));
    REQUIRE(screen.rows.size() == 1);

    screen.apply(piece(proto::SnapshotPart::First, 0, U'a'));
    screen.apply(piece(proto::SnapshotPart::Middle, 1, U'b'));
    screen.apply(piece(proto::SnapshotPart::Last, 2, U'c'));

    // All three rows survived the run, and the row the superseded snapshot left is gone.
    REQUIRE(screen.rows.size() == 3);
    CHECK(screen.viewportText() == "a\nb\nc\n");

    // A run whose tail was dropped in favour of a newer snapshot is what makes the marker
    // necessary rather than merely convenient: the new run's First must still clear, even though
    // the piece before it said more was coming.
    screen.apply(piece(proto::SnapshotPart::First, 0, U'z'));
    REQUIRE(screen.rows.size() == 1);
    CHECK(screen.viewportText() == "z\n\n\n");
}

TEST_CASE("a screen knows a chunked snapshot is still arriving", "[vthost][attach]")
{
    // What gates the reply PDUs that carry no marker of their own. An ImageData answer lands in
    // the same stream the run is still using — the client asked for it on seeing a new image id in
    // an earlier piece — so without this the frontend would be handed, and would paint, a grid it
    // has only half received.
    auto screen = RemoteScreen {};
    CHECK_FALSE(screen.snapshotInProgress); // nothing received yet

    screen.apply(snapshotPiece(proto::SnapshotPart::First));
    CHECK(screen.snapshotInProgress);
    screen.apply(snapshotPiece(proto::SnapshotPart::Middle));
    CHECK(screen.snapshotInProgress);
    screen.apply(snapshotPiece(proto::SnapshotPart::Last));
    CHECK_FALSE(screen.snapshotInProgress);

    // An unsplit snapshot is never "in progress", and neither is an increment.
    screen.apply(snapshotPiece(proto::SnapshotPart::Whole));
    CHECK_FALSE(screen.snapshotInProgress);
    screen.apply(snapshotPiece(proto::SnapshotPart::First));
    REQUIRE(screen.snapshotInProgress);
    screen.apply(proto::Delta {}); // a plain delta ends a run the server abandoned
    CHECK_FALSE(screen.snapshotInProgress);
}

TEST_CASE("RemoteScreen drops history the server discarded via the floor", "[vthost][attach]")
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

TEST_CASE("RemoteScreen keeps the server's eviction count", "[vthost][attach]")
{
    auto screen = RemoteScreen {};
    auto delta = proto::Delta {};
    delta.stableFloor = 7;
    delta.evictedRowCount = 42;
    screen.apply(delta);
    CHECK(screen.evictedRowCount == 42);

    // A reset on the server starts its count over, and the screen follows it down.
    delta.evictedRowCount = 0;
    screen.apply(delta);
    CHECK(screen.evictedRowCount == 0);
}

TEST_CASE("the mirror's retention bound follows the client's profile", "[vthost][attach]")
{
    // The bound used to be a hardcoded 10 000, which silently contradicted a user who had asked
    // for deeper scrollback: the daemon served the rows and the mirror threw them away. It is now
    // whatever `history.limit` the client stated in its ClientHello, so the two ends agree.
    //
    // Driven through RemoteScreen directly — the bound is fixed at its construction, and all
    // NativeClient adds is deciding the value once (@see NativeClient::resolveHistoryKeep).
    auto seedRows = [](RemoteScreen& screen, int64_t base, int64_t depth) {
        auto seed = proto::Delta {};
        seed.stableViewportBase = base;
        seed.stableFloor = 0; // the server holds everything; only the client's bound may evict
        for (auto const id: std::views::iota(base - depth, base + 1))
        {
            auto line = proto::WireLine {};
            line.stableId = id;
            line.columns = 5;
            seed.lines.push_back(line);
        }
        screen.apply(seed);
    };

    SECTION("a finite bound keeps that many rows above the viewport")
    {
        auto screen = RemoteScreen { 4 };
        seedRows(screen, 100, 20);
        CHECK(screen.rows.contains(96)); // exactly at the bound
        CHECK_FALSE(screen.rows.contains(95));
    }

    SECTION("an unbounded client keeps everything the server still holds")
    {
        auto screen = RemoteScreen { std::nullopt };
        seedRows(screen, 100, 20);
        CHECK(screen.rows.contains(80)); // the whole seeded depth survives
        CHECK(screen.rows.size() == 21);
    }

    SECTION("the server's floor still evicts, unbounded or not")
    {
        // The bound widens what a client keeps; it never overrides the server. A row the daemon
        // no longer holds is gone whatever the profile says.
        auto screen = RemoteScreen { std::nullopt };
        seedRows(screen, 100, 20);
        auto cleared = proto::Delta {};
        cleared.stableViewportBase = 100;
        cleared.stableFloor = 100;
        screen.apply(cleared);
        CHECK_FALSE(screen.rows.contains(80));
        CHECK(screen.rows.contains(100));
    }
}

TEST_CASE("a stated profile is what the client stamps on its mirrors", "[vthost][attach]")
{
    // The other half: that the wire value is READ, and read the way the configuration spells it.
    using vthost::client::NativeClient;
    auto withHistory = [](int64_t lines) {
        return NativeClient::HandshakeOptions { .sessionSettings = proto::WireSessionSettings {
                                                    .historyLineCount = lines } };
    };

    // No profile at all: nothing to derive from, so the mirror's own memory backstop applies.
    CHECK(NativeClient::resolveHistoryKeep({}) == RemoteScreen::DefaultHistoryKeep);
    CHECK(NativeClient::resolveHistoryKeep(withHistory(50000)) == 50000);
    // -1 is `history.limit: infinite` — the configuration's own spelling, not a sentinel invented
    // here (@see vthost::toWireSessionSettings).
    CHECK(NativeClient::resolveHistoryKeep(withHistory(-1)) == std::nullopt);
    // A STATED zero is not "no scrollback": the daemon reads it as its own default and hosts the
    // session with real history, so a mirror keeping none would show a session it cannot scroll.
    // The value is the daemon's, not DefaultHistoryKeep, precisely because this goes through the
    // same vthost::fromWireSessionSettings the daemon applies — which is the point of reusing it.
    CHECK(NativeClient::resolveHistoryKeep(withHistory(0)) == vthost::DefaultSessionHistoryLineCount);
    // And an absurd request is clamped rather than believed, by that same shared reader.
    CHECK(NativeClient::resolveHistoryKeep(withHistory(1'000'000'000)) == vthost::MaxSessionHistoryLineCount);
}

TEST_CASE("RemoteScreen tracks, replaces and evicts image cells", "[vthost][attach]")
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

TEST_CASE("RemoteScreen.dropImage clears pixels and the cells that referenced it", "[vthost][attach]")
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
void enqueuePdu(core::net::WriteQueue& writer, uint64_t serial, proto::DecodedPdu const& pdu)
{
    auto sink = proto::Writer {};
    proto::encodePdu(sink, serial, pdu);
    auto const bytes = sink.view();
    REQUIRE(writer.enqueue(std::string { reinterpret_cast<char const*>(bytes.data()), bytes.size() }));
}

/// @param capabilities What the daemon offers.
/// @return A daemon's answer accepting a client of this build's range.
[[nodiscard]] proto::ServerHello acceptingHello(proto::CapabilitySet capabilities = {})
{
    return proto::ServerHello { .agreedVersion = proto::SupportedVersions.max,
                                .versions = proto::SupportedVersions,
                                .capabilities = std::move(capabilities) };
}

/// A hand-driven server: answers the handshake, pushes a snapshot whose single
/// row carries an image cell (id 7), then serves the client's FetchImage with
/// @p reply. Exercises the serial-correlated (session-less) image reply path
/// without needing a real rasterized image.
Task<void> fakeImageServer(core::net::EventLoop* loop, core::net::ISocket* socket, proto::ImageData reply)
{
    auto writer = core::net::WriteQueue { *loop, socket, std::size_t { 1 } * 1024 * 1024 };
    co_await vthost::pumpPdus(socket, [&](proto::DecodedFrame const& frame) {
        if (std::holds_alternative<proto::ClientHello>(frame.pdu))
        {
            enqueuePdu(writer, frame.serial, proto::DecodedPdu { acceptingHello() });
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
Task<void> awaitImageThenDetach(core::net::EventLoop* loop, NativeClient* client, bool* seen)
{
    co_await core::net::testing::waitUntil(loop, [&] {
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

TEST_CASE("attach fetches image pixels on demand and caches them", "[vthost][attach]")
{
    auto const source = core::net::makeDefaultBackend();
    auto loop = core::net::EventLoop { *source };
    auto pair = *core::net::testing::makeSocketPair(loop);
    auto* serverSock = pair.first.get();
    auto imageEvent = false;
    auto client = NativeClient { loop,
                                 std::move(pair.second),
                                 NativeClient::HandshakeOptions {},
                                 vtbackend::CommandBlockStoreLimits {},
                                 NativeClient::UpdateHandler {},
                                 NativeClient::ImageHandler { [&](RemoteScreen const&, uint32_t id) {
                                     if (id == 7)
                                         imageEvent = true;
                                 } },
                                 NativeClient::SessionEventHandler {},
                                 NativeClient::LayoutHandler {} };

    auto reply = proto::ImageData { .imageId = 0, .width = 2, .height = 3, .data = {} };
    reply.data.resize(static_cast<std::size_t>(2 * 3 * 4), std::byte { 0x80 });

    auto seen = false;
    loop.blockOn(core::net::testing::allOf(client.run(),
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
    auto const source = core::net::makeDefaultBackend();
    auto loop = core::net::EventLoop { *source };
    auto host = SessionHost { loop,
                              [](vtbackend::PageSize size, std::optional<vtpty::Process::ExecInfo> const&) {
                                  return std::make_unique<vtpty::MockPty>(size);
                              },
                              vtbackend::Settings {},
                              core::defaultEnvironment(),
                              vthost::SessionHostConfig { .pumping = vthost::SessionPumping::Manual } };
    host.createTab();
    auto pair = *core::net::testing::makeSocketPair(loop);
    auto server = NativeSession { loop,
                                  host,
                                  vthost::ConnectionId { .endpoint = "test", .index = 1 },
                                  std::move(pair.first),
                                  NativeSession::DefaultWriteQueueBytes,
                                  tokenGuarded(std::move(serverToken)) };
    auto client = NativeClient { loop,
                                 std::move(pair.second),
                                 NativeClient::HandshakeOptions { .token = std::move(clientToken) },
                                 vtbackend::CommandBlockStoreLimits {},
                                 NativeClient::UpdateHandler {},
                                 NativeClient::ImageHandler {},
                                 NativeClient::SessionEventHandler {},
                                 NativeClient::LayoutHandler {} };

    auto scenario = [](core::net::EventLoop* loop, NativeClient* client, bool* got) -> Task<void> {
        // On accept a snapshot arrives quickly; on reject the server drops us and
        // no snapshot ever comes. Bounded poll either way.
        for (auto i = 0; i < 300 && client->screens().empty(); ++i)
            co_await loop->delay(1ms);
        *got = !client->screens().empty();
        client->detach();
    }(&loop, &client, gotSnapshot);

    loop.blockOn(core::net::testing::allOf(server.run(), client.run(), std::move(scenario)));
}

} // namespace

TEST_CASE("attach enforces a preshared auth token", "[vthost][attach]")
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

/// A daemon of contour 0.7.0, as far as a client of a later revision gets with one: it reads the client's
/// first frame as that release reads a ClientHello, answers with its version alone, and hangs up.
/// @param helloVersion Set to the version the client's hello states, if that release can read it.
Task<void> shippedDaemon(core::net::ISocket* socket, std::optional<uint32_t>* helloVersion)
{
    auto buffer = std::vector<std::byte> {};
    auto scratch = std::vector<std::byte>(4096);
    auto frame = proto::readFrame(buffer);
    while (!frame && frame.error() == proto::DecodeError::NeedMoreData)
    {
        auto const n = co_await socket->read(scratch);
        if (!n.has_value() || *n == 0)
            co_return;
        buffer.insert(buffer.end(), scratch.begin(), scratch.begin() + static_cast<long>(*n));
        frame = proto::readFrame(buffer);
    }
    if (!frame || frame->ident != vthost::testing::ShippedClientHelloTag)
        co_return;
    *helloVersion = vthost::testing::shippedClientHelloVersion(frame->body);
    // A hello that release cannot read is dropped without an answer, as the release drops it.
    if (helloVersion->has_value())
    {
        auto const reply = vthost::testing::shippedServerHello(frame->serial);
        std::ignore = co_await socket->write(std::span<std::byte const> { reply });
    }
    socket->close();
}

/// A peer that reads the client's first frame, keeps the ClientHello it decodes to, and hangs up.
/// @param hello Set to the hello the client sent, if its first frame decodes to one.
Task<void> helloCapture(core::net::ISocket* socket, std::optional<proto::ClientHello>* hello)
{
    auto buffer = std::vector<std::byte> {};
    auto scratch = std::vector<std::byte>(4096);
    auto decoded = proto::decodePdu(buffer);
    while (!decoded && decoded.error() == proto::DecodeError::NeedMoreData)
    {
        auto const n = co_await socket->read(scratch);
        if (!n.has_value() || *n == 0)
            co_return;
        buffer.insert(buffer.end(), scratch.begin(), scratch.begin() + static_cast<long>(*n));
        decoded = proto::decodePdu(buffer);
    }
    if (decoded)
        if (auto const* sent = std::get_if<proto::ClientHello>(&decoded->pdu))
            *hello = *sent;
    socket->close();
}

} // namespace

TEST_CASE("NativeClient states HandshakeOptions::clientArea in its hello", "[vthost][client]")
{
    auto const source = core::net::makeDefaultBackend();
    auto loop = core::net::EventLoop { *source };
    auto pair = *core::net::testing::makeSocketPair(loop);
    auto* daemonSocket = pair.first.get();
    auto client = NativeClient { loop,
                                 std::move(pair.second),
                                 NativeClient::HandshakeOptions {
                                     .clientArea = proto::WireClientArea { .columns = 100, .lines = 30 } },
                                 vtbackend::CommandBlockStoreLimits {},
                                 NativeClient::UpdateHandler {},
                                 NativeClient::ImageHandler {},
                                 NativeClient::SessionEventHandler {},
                                 NativeClient::LayoutHandler {} };

    auto hello = std::optional<proto::ClientHello> {};
    loop.blockOn(core::net::testing::allOf(client.run(), helloCapture(daemonSocket, &hello)));

    REQUIRE(hello.has_value());
    CHECK(hello->clientArea == proto::WireClientArea { .columns = 100, .lines = 30 });
}

TEST_CASE("a client meets a daemon of contour 0.7.0 with the version message", "[vthost][attach][handshake]")
{
    // A daemon keeps running across an upgrade. It must read the new client's hello and answer with its
    // version, and the client must read that answer: then the user is told the two revisions differ, rather
    // than meeting a connection dropped on a decode error.
    auto capture = core::log::ScopedCapture {};
    auto const source = core::net::makeDefaultBackend();
    auto loop = core::net::EventLoop { *source };
    auto pair = *core::net::testing::makeSocketPair(loop);
    auto* daemonSocket = pair.first.get();
    auto client = NativeClient {
        loop,
        std::move(pair.second),
        // Settings too: they are the part of a hello whose layout has changed since that release.
        NativeClient::HandshakeOptions {
            .token = "s3cr3t", .sessionSettings = proto::WireSessionSettings { .sixelScrolling = 0 } },
        vtbackend::CommandBlockStoreLimits {},
        NativeClient::UpdateHandler {},
        NativeClient::ImageHandler {},
        NativeClient::SessionEventHandler {},
        NativeClient::LayoutHandler {}
    };

    auto helloVersion = std::optional<uint32_t> {};
    loop.blockOn(core::net::testing::allOf(client.run(), shippedDaemon(daemonSocket, &helloVersion)));

    CHECK(helloVersion == proto::NegotiatingHelloMarker);
    auto const shipped = vthost::testing::ShippedProtocolVersion;
    CHECK(client.refusal()
          == proto::HandshakeRefusal { .ours = proto::SupportedVersions, .theirs = { shipped, shipped } });
    CHECK_FALSE(client.connected());
    CHECK(capture.contains(std::format("the daemon speaks protocol {}, from a release before", shipped)));
}

TEST_CASE("parseHostPort accepts HOST:PORT and rejects malformed input", "[vthost][attach]")
{
    using vthost::parseHostPort;

    auto const ipv4 = parseHostPort("127.0.0.1:9090");
    REQUIRE(ipv4.has_value());
    CHECK(ipv4->first == "127.0.0.1");
    CHECK(ipv4->second == 9090);

    auto const named = parseHostPort("daemon.example.com:65535");
    REQUIRE(named.has_value());
    CHECK(named->first == "daemon.example.com");
    CHECK(named->second == 65535);

    // A bracketed IPv6 literal keeps its colons; only the final :PORT is split off.
    auto const ipv6 = parseHostPort("[::1]:443");
    REQUIRE(ipv6.has_value());
    CHECK(ipv6->first == "::1");
    CHECK(ipv6->second == 443);

    CHECK_FALSE(parseHostPort("no-port").has_value());
    CHECK_FALSE(parseHostPort("host:").has_value());
    CHECK_FALSE(parseHostPort(":9090").has_value());
    CHECK_FALSE(parseHostPort("host:0").has_value());     // port 0 is not a dialable target
    CHECK_FALSE(parseHostPort("host:70000").has_value()); // out of range
    CHECK_FALSE(parseHostPort("host:12x").has_value());   // trailing garbage
}

namespace
{

/// Connects over TCP, mirrors the snapshot, detaches, then closes the server so
/// its accept loop unwinds. Records whether a snapshot arrived.
Task<void> tcpAttachDriver(core::net::EventLoop* loop,
                           vthost::ConnectionAcceptor* server,
                           std::uint16_t port,
                           bool* saw)
{
    auto connected = co_await core::net::connect(loop, "127.0.0.1", port);
    if (connected)
    {
        auto client = NativeClient { *loop,
                                     std::move(*connected),
                                     NativeClient::HandshakeOptions { .token = "tok" },
                                     vtbackend::CommandBlockStoreLimits {},
                                     NativeClient::UpdateHandler {},
                                     NativeClient::ImageHandler {},
                                     NativeClient::SessionEventHandler {},
                                     NativeClient::LayoutHandler {} };
        auto scenario = [](core::net::EventLoop* loop, NativeClient* client, bool* saw) -> Task<void> {
            for (auto i = 0; i < 300 && client->screens().empty(); ++i)
                co_await loop->delay(1ms);
            *saw = !client->screens().empty();
            client->detach();
        }(loop, &client, saw);
        co_await core::async::whenAll(client.run(), std::move(scenario));
    }
    server->close();
}

} // namespace

TEST_CASE("attach mirrors over a real TCP transport with token auth", "[vthost][attach]")
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
    host.createTab();
    // Declared after the host, so the connection flows the server spawns end while it exists.
    auto const drain = vthost::LoopDrain { loop };

    // A native server on an ephemeral loopback TCP port, token-guarded.
    auto listener = core::net::listen(loop, "127.0.0.1", 0);
    REQUIRE(listener.has_value());
    auto const port = (*listener)->boundPort();
    auto server = vthost::ConnectionAcceptor {
        loop, "test", std::move(*listener), vthost::makeNativeHandler(loop, host, tokenGuarded("tok"))
    };

    auto saw = false;
    loop.blockOn(core::net::testing::allOf(server.serve(), tcpAttachDriver(&loop, &server, port, &saw)));
    CHECK(saw);
}

namespace
{

/// Connects over TCP, wraps the connection in TLS (client role), mirrors the
/// snapshot, detaches, then closes the server. Records whether a snapshot came.
Task<void> tlsTcpAttachDriver(core::net::EventLoop* loop,
                              vthost::ConnectionAcceptor* server,
                              std::shared_ptr<core::net::ITlsContext> clientTls,
                              std::uint16_t port,
                              bool* saw)
{
    auto connected = co_await core::net::connect(loop, "127.0.0.1", port);
    if (connected)
    {
        auto client = NativeClient { *loop,
                                     clientTls->wrap(std::move(*connected), *loop),
                                     NativeClient::HandshakeOptions { .token = "tok" },
                                     vtbackend::CommandBlockStoreLimits {},
                                     NativeClient::UpdateHandler {},
                                     NativeClient::ImageHandler {},
                                     NativeClient::SessionEventHandler {},
                                     NativeClient::LayoutHandler {} };
        auto scenario = [](core::net::EventLoop* loop, NativeClient* client, bool* saw) -> Task<void> {
            for (auto i = 0; i < 500 && client->screens().empty(); ++i)
                co_await loop->delay(1ms);
            *saw = !client->screens().empty();
            client->detach();
        }(loop, &client, saw);
        co_await core::async::whenAll(client.run(), std::move(scenario));
    }
    server->close();
}

} // namespace

TEST_CASE("attach mirrors over TLS-encrypted TCP with token auth", "[vthost][attach]")
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
    host.createTab();
    // Declared after the host, so the connection flows the server spawns end while it exists.
    auto const drain = vthost::LoopDrain { loop };

    auto serverTls = core::net::makeSelfSignedServerContext();
    REQUIRE(serverTls.has_value());
    auto clientTls = core::net::makeTlsClientContext();
    REQUIRE(clientTls.has_value());

    auto listener = core::net::listen(loop, "127.0.0.1", 0);
    REQUIRE(listener.has_value());
    auto const port = (*listener)->boundPort();

    // A TLS-wrapping native handler: each accepted socket is encrypted (server
    // role) before the native protocol runs over it — the daemon's TCP path.
    auto nativeHandler = vthost::makeNativeHandler(loop, host, tokenGuarded("tok"));
    auto const& tlsContext = *serverTls;
    auto handler = [tlsContext, nativeHandler, &loop](vthost::ConnectionId id,
                                                      std::unique_ptr<core::net::ISocket> socket) {
        return nativeHandler(std::move(id), tlsContext->wrap(std::move(socket), loop));
    };
    auto server = vthost::ConnectionAcceptor { loop, "test", std::move(*listener), handler };

    auto saw = false;
    loop.blockOn(core::net::testing::allOf(server.serve(),
                                           tlsTcpAttachDriver(&loop, &server, *clientTls, port, &saw)));
    CHECK(saw); // snapshot mirrored across TLS-over-TCP with a valid token
}

TEST_CASE("a clean TLS detach ends the connection as a disconnect rather than an error", "[vthost][attach]")
{
    // TLS separates a peer that finished from one that was cut off by close_notify: a TCP FIN
    // without it reads as truncation. So a client that detaches cleanly must say close_notify
    // before it closes, or the daemon logs every ordinary detach as a failed read.
    auto capture = core::log::ScopedCapture {};

    auto const source = core::net::makeDefaultBackend();
    auto loop = core::net::EventLoop { *source };
    auto host = SessionHost { loop,
                              [](vtbackend::PageSize size, std::optional<vtpty::Process::ExecInfo> const&) {
                                  return std::make_unique<vtpty::MockPty>(size);
                              },
                              vtbackend::Settings {},
                              core::defaultEnvironment(),
                              vthost::SessionHostConfig { .pumping = vthost::SessionPumping::Manual } };
    host.createTab();
    // Declared after the host, so the connection flows the server spawns end while it exists.
    auto const drain = vthost::LoopDrain { loop };

    auto serverTls = core::net::makeSelfSignedServerContext();
    REQUIRE(serverTls.has_value());
    auto clientTls = core::net::makeTlsClientContext();
    REQUIRE(clientTls.has_value());

    auto listener = core::net::listen(loop, "127.0.0.1", 0);
    REQUIRE(listener.has_value());
    auto const port = (*listener)->boundPort();

    auto nativeHandler = vthost::makeNativeHandler(loop, host, tokenGuarded("tok"));
    auto const& tlsContext = *serverTls;
    auto handler = [tlsContext, nativeHandler, &loop](vthost::ConnectionId id,
                                                      std::unique_ptr<core::net::ISocket> socket) {
        return nativeHandler(std::move(id), tlsContext->wrap(std::move(socket), loop));
    };
    auto server = vthost::ConnectionAcceptor { loop, "test", std::move(*listener), handler };

    auto saw = false;
    loop.blockOn(core::net::testing::allOf(server.serve(),
                                           tlsTcpAttachDriver(&loop, &server, *clientTls, port, &saw)));
    REQUIRE(saw);

    // The daemon's side of the connection ends in its own spawned flow, a few turns after the
    // client's; wait for it to say how, for at most two seconds.
    auto polls = 0;
    loop.blockOn(core::net::pollUntil(&loop, [&] {
        return capture.contains("disconnected") || capture.contains("read failed") || ++polls > 2000;
    }));
    INFO(capture.text());
    CHECK(capture.contains(": disconnected"));
    CHECK_FALSE(capture.contains("read failed"));
    CHECK_FALSE(capture.contains("close_notify"));
}

TEST_CASE("attach mirrors over TLS with a generated self-signed dev certificate", "[vthost][attach]")
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
    host.createTab();
    // Declared after the host, so the connection flows the server spawns end while it exists.
    auto const drain = vthost::LoopDrain { loop };

    // Mint a dev certificate exactly as an operator would to feed --tls-cert/--tls-key.
    // Library-only (no `openssl` CLI), so this runs identically on Windows and UNIX.
    auto material = core::net::generateSelfSignedCertificate({ .commonName = "contour-daemon" });
    REQUIRE(material.has_value());
    CHECK(material->certPem.starts_with("-----BEGIN CERTIFICATE-----"));
    CHECK(material->keyPem.contains("PRIVATE KEY"));

    // Server context from the PEM cert+key (the file-content path). The client PINS
    // that exact certificate as its trust anchor and VERIFIES the peer against it —
    // stronger than the TOFU path above (which does not verify at all).
    auto serverTls = core::net::makeTlsServerContext(material->certPem, material->keyPem);
    REQUIRE(serverTls.has_value());
    auto clientTls = core::net::makeTlsClientContext(material->certPem);
    REQUIRE(clientTls.has_value());

    auto listener = core::net::listen(loop, "127.0.0.1", 0);
    REQUIRE(listener.has_value());
    auto const port = (*listener)->boundPort();

    auto nativeHandler = vthost::makeNativeHandler(loop, host, tokenGuarded("tok"));
    auto const& tlsContext = *serverTls;
    auto handler = [tlsContext, nativeHandler, &loop](vthost::ConnectionId id,
                                                      std::unique_ptr<core::net::ISocket> socket) {
        return nativeHandler(std::move(id), tlsContext->wrap(std::move(socket), loop));
    };
    auto server = vthost::ConnectionAcceptor { loop, "test", std::move(*listener), handler };

    auto saw = false;
    loop.blockOn(core::net::testing::allOf(server.serve(),
                                           tlsTcpAttachDriver(&loop, &server, *clientTls, port, &saw)));
    CHECK(saw); // snapshot mirrored across TLS whose peer cert the client verified against the dev cert
}

namespace
{

/// Waits for the daemon's layout to arrive on the client, then detaches.
Task<void> awaitLayout(core::net::EventLoop* loop,
                       NativeClient* client,
                       std::optional<proto::LayoutState>* layout)
{
    co_await core::net::testing::waitUntil(loop, [layout] { return layout->has_value(); });
    client->detach();
}

/// Awaits the initial one-tab layout, authors a second tab, and waits for the
/// daemon to honor it and re-push a two-tab layout before detaching.
Task<void> driveCreateTab(core::net::EventLoop* loop, NativeClient* client, int const* mirroredTabs)
{
    co_await core::net::testing::waitUntil(loop, [mirroredTabs] { return *mirroredTabs == 1; });
    client->createTab(0, {});
    co_await core::net::testing::waitUntil(loop, [mirroredTabs] { return *mirroredTabs == 2; });
    client->detach();
}

/// Awaits the initial single-window layout, authors a new window, and waits for
/// the daemon to push a layout for the SECOND window before detaching.
Task<void> driveCreateWindow(core::net::EventLoop* loop, NativeClient* client, std::set<uint64_t>* windows)
{
    co_await core::net::testing::waitUntil(loop, [windows] { return windows->size() == 1; });
    client->createWindow({});
    co_await core::net::testing::waitUntil(loop, [windows] { return windows->size() == 2; });
    client->detach();
}

} // namespace

TEST_CASE("attach receives the daemon's tab and pane layout", "[vthost][attach]")
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
    auto* tab = host.createTab();
    // Split the tab into two panes (a vertical divider at 60/40).
    host.splitActivePane(tab->id(), vtworkspace::SplitState::Vertical, 0.6);

    auto pair = *core::net::testing::makeSocketPair(loop);
    auto server = NativeSession {
        loop, host, vthost::ConnectionId { .endpoint = "test", .index = 1 }, std::move(pair.first)
    };
    auto layout = std::optional<proto::LayoutState> {};
    auto client = NativeClient { loop,
                                 std::move(pair.second),
                                 NativeClient::HandshakeOptions {},
                                 vtbackend::CommandBlockStoreLimits {},
                                 NativeClient::UpdateHandler {},
                                 NativeClient::ImageHandler {},
                                 NativeClient::SessionEventHandler {},
                                 NativeClient::LayoutHandler {
                                     [&layout](proto::LayoutState const& received) { layout = received; } } };

    loop.blockOn(core::net::testing::allOf(server.run(), client.run(), awaitLayout(&loop, &client, &layout)));

    REQUIRE(layout.has_value());
    REQUIRE(layout->tabs.size() == 1);
    auto const& root = layout->tabs.front().root;
    CHECK(root.split == std::to_underlying(vtworkspace::SplitState::Vertical)); // an internal split node
    CHECK(root.ratio == 6000);                                                  // 0.6 x 10000
    REQUIRE(root.children.size() == 2);
    // Both children are leaves carrying distinct sessions.
    CHECK(root.children[0].split == 0);
    CHECK(root.children[1].split == 0);
    CHECK(root.children[0].session != 0);
    CHECK(root.children[1].session != 0);
    CHECK(root.children[0].session != root.children[1].session);
}

TEST_CASE("a client authors a tab, honored by the daemon and mirrored back", "[vthost][attach]")
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
    host.createTab(); // the daemon starts with one tab

    auto pair = *core::net::testing::makeSocketPair(loop);
    auto server = NativeSession {
        loop, host, vthost::ConnectionId { .endpoint = "test", .index = 1 }, std::move(pair.first)
    };

    auto mirroredTabs = int {};
    auto client =
        NativeClient { loop,
                       std::move(pair.second),
                       NativeClient::HandshakeOptions {},
                       vtbackend::CommandBlockStoreLimits {},
                       NativeClient::UpdateHandler {},
                       NativeClient::ImageHandler {},
                       NativeClient::SessionEventHandler {},
                       NativeClient::LayoutHandler { [&mirroredTabs](proto::LayoutState const& received) {
                           mirroredTabs = static_cast<int>(received.tabs.size());
                       } } };

    loop.blockOn(
        core::net::testing::allOf(server.run(), client.run(), driveCreateTab(&loop, &client, &mirroredTabs)));

    CHECK(mirroredTabs == 2);                                     // the mirrored layout grew
    CHECK(host.model().window(host.windowId())->tabCount() == 2); // the daemon really created the tab
}

namespace
{

/// The owner's sequence on a dark client of a daemon whose profile is built light: session A exists, the
/// client turns dark, authors session B and states dark for it at once -- before B's first snapshot, built
/// under the host's light palette, need have been read -- then goes 1, 2, 1, 2 across the tabs. Switching
/// to a tab resizes its pane, which the daemon answers with a fresh snapshot.
///
/// B is learnt from the layout, as a client learns of a tab. A tab opened after the attach is first pushed
/// when its screen first changes, and B, born dark, changes nothing when told dark: its first snapshot
/// follows its shell's prompt, which @p printPrompt stands in for under manual pumping.
/// @param tabSessions The session of each tab's single pane, in the latest layout the client was handed.
/// @param printPrompt Has a session's shell print its prompt.
Task<void> driveDarkTabSwitches(core::net::EventLoop* loop,
                                NativeClient* client,
                                vtworkspace::SessionId first,
                                std::vector<uint64_t> const* tabSessions,
                                std::function<void(uint64_t)> printPrompt,
                                std::vector<vtbackend::RGBColor>* seenByTab)
{
    constexpr auto Dark = vthost::testing::FakeDualPaletteCatalog::DarkBackground;
    auto const backgroundOf = [&](uint64_t session) {
        return vtbackend::RGBColor { client->screens().at(session).defaultBackground };
    };
    auto const showsDark = [&](uint64_t session) {
        return client->screens().contains(session) && backgroundOf(session) == Dark;
    };
    auto const stateDark = [&](uint64_t session) {
        client->performOperation(proto::PerformOperation {
            .session = session, .operation = proto::EngineOperation::UseDarkColors });
    };

    auto const attached =
        co_await core::net::testing::waitUntil(loop, [&] { return client->screens().contains(first.value); });
    REQUIRE(attached);
    stateDark(first.value);
    co_await core::net::testing::waitUntil(loop, [&] { return showsDark(first.value); });

    client->createTab(0, {});
    auto const created =
        co_await core::net::testing::waitUntil(loop, [&] { return tabSessions->size() == 2; });
    REQUIRE(created);
    auto const second =
        *std::ranges::find_if(*tabSessions, [&](uint64_t session) { return session != first.value; });
    REQUIRE(second != first.value);
    stateDark(second);
    printPrompt(second);
    auto const secondShown = co_await core::net::testing::waitUntil(loop, [&] { return showsDark(second); });
    REQUIRE(secondShown);

    auto columns = uint32_t { 90 };
    auto const bind = [&](uint64_t session) {
        ++columns;
        client->resizePane(session, columns, 30);
        return core::net::testing::waitUntil(loop, [&, session] {
            return client->screens().contains(session) && client->screens().at(session).columns == columns;
        });
    };
    for (auto const session: { first.value, second, first.value, second })
    {
        auto const bound = co_await bind(session);
        REQUIRE(bound);
        seenByTab->push_back(backgroundOf(first.value));
        seenByTab->push_back(backgroundOf(second));
    }
    client->detach();
}

} // namespace

TEST_CASE("a dark client's sessions keep the dark palette across new tabs and tab switches",
          "[vthost][attach][colors]")
{
    auto const source = core::net::makeDefaultBackend();
    auto loop = core::net::EventLoop { *source };
    auto const catalog = std::make_shared<vthost::testing::FakeDualPaletteCatalog>();
    // As the daemon is built: the base settings are the default profile's, resolved under the host's own
    // (light) preference.
    auto host = SessionHost { loop,
                              [](vtbackend::PageSize size, std::optional<vtpty::Process::ExecInfo> const&) {
                                  return std::make_unique<vtpty::MockPty>(size);
                              },
                              catalog->resolve("").settings,
                              core::defaultEnvironment(),
                              vthost::SessionHostConfig { .pumping = vthost::SessionPumping::Manual,
                                                          .profiles = catalog } };
    auto const first = newSession(host);
    REQUIRE(host.terminal(first)->colorPalette().defaultBackground
            == vthost::testing::FakeDualPaletteCatalog::LightBackground);

    auto pair = *core::net::testing::makeSocketPair(loop);
    auto server = NativeSession {
        loop, host, vthost::ConnectionId { .endpoint = "test", .index = 1 }, std::move(pair.first)
    };
    auto tabSessions = std::vector<uint64_t> {};
    auto client =
        NativeClient { loop,
                       std::move(pair.second),
                       NativeClient::HandshakeOptions {},
                       vtbackend::CommandBlockStoreLimits {},
                       NativeClient::UpdateHandler {},
                       NativeClient::ImageHandler {},
                       NativeClient::SessionEventHandler {},
                       NativeClient::LayoutHandler { [&tabSessions](proto::LayoutState const& layout) {
                           tabSessions.clear();
                           for (auto const& tab: layout.tabs)
                               tabSessions.push_back(tab.root.session);
                       } } };

    auto seenByTab = std::vector<vtbackend::RGBColor> {};
    loop.blockOn(core::net::testing::allOf(
        server.run(),
        client.run(),
        guardScenario(
            &loop,
            [&client] { client.detach(); },
            driveDarkTabSwitches(
                &loop,
                &client,
                first,
                &tabSessions,
                [&](uint64_t session) {
                    host.terminal(vtworkspace::SessionId { session })->writeToScreen("$ ");
                    server.sessionScreenUpdated(vtworkspace::SessionId { session });
                },
                &seenByTab))));

    REQUIRE(seenByTab.size() == 8);
    for (auto const& background: seenByTab)
        CHECK(background == vthost::testing::FakeDualPaletteCatalog::DarkBackground);
}

namespace
{

/// A host whose profile is built light, as the daemon builds it, and a client of @p preference attached to
/// it over an in-memory socket; every default background the client's mirror is handed is recorded.
/// @param seeded Whether the host already hosts a session when the client attaches.
/// @param preference What the client's hello states.
/// @return The default background of every update the client received, in order, and the host's sessions'
///         palettes at the end.
struct AttachObservation
{
    std::vector<vtbackend::RGBColor> seen;     ///< Each update's default background, in order.
    std::vector<vtbackend::RGBColor> hostEnds; ///< Each hosted session's default background at the end.
};

/// Awaits the attach, asks for a resize so that more traffic follows it, and detaches.
Task<void> driveAttachThenResize(core::net::EventLoop* loop, NativeClient* client)
{
    auto const attached =
        co_await core::net::testing::waitUntil(loop, [&] { return !client->screens().empty(); });
    REQUIRE(attached);
    // A little more traffic after the attach, so a late light snapshot would show too.
    client->requestResize(100, 40);
    co_await core::net::testing::waitUntil(loop,
                                           [&] { return client->screens().begin()->second.columns == 100; });
    client->detach();
}

AttachObservation attachDarkClient(bool seeded, proto::WireColorPreference preference)
{
    auto const source = core::net::makeDefaultBackend();
    auto loop = core::net::EventLoop { *source };
    auto const catalog = std::make_shared<vthost::testing::FakeDualPaletteCatalog>();
    auto host = SessionHost { loop,
                              [](vtbackend::PageSize size, std::optional<vtpty::Process::ExecInfo> const&) {
                                  return std::make_unique<vtpty::MockPty>(size);
                              },
                              catalog->resolve("").settings,
                              core::defaultEnvironment(),
                              vthost::SessionHostConfig { .pumping = vthost::SessionPumping::Manual,
                                                          .profiles = catalog } };
    if (seeded)
        std::ignore = newSession(host);
    auto pair = *core::net::testing::makeSocketPair(loop);
    auto server = NativeSession {
        loop, host, vthost::ConnectionId { .endpoint = "test", .index = 1 }, std::move(pair.first)
    };
    auto observation = AttachObservation {};
    auto client =
        NativeClient { loop,
                       std::move(pair.second),
                       NativeClient::HandshakeOptions { .colorPreference = preference },
                       vtbackend::CommandBlockStoreLimits {},
                       NativeClient::UpdateHandler { [&](RemoteScreen const& screen, proto::Delta const&) {
                           observation.seen.emplace_back(screen.defaultBackground);
                       } },
                       NativeClient::ImageHandler {},
                       NativeClient::SessionEventHandler {},
                       NativeClient::LayoutHandler {} };
    loop.blockOn(core::net::testing::allOf(
        server.run(),
        client.run(),
        guardScenario(&loop, [&client] { client.detach(); }, driveAttachThenResize(&loop, &client))));
    host.model().forEachTab([&](vtworkspace::Window&, vtworkspace::Tab& tab) {
        tab.rootPane()->walkTree([&](vtworkspace::Pane& pane) {
            if (pane.isLeaf())
                observation.hostEnds.push_back(
                    host.terminal(pane.session())->colorPalette().defaultBackground);
        });
    });
    return observation;
}

} // namespace

TEST_CASE("a dark client's hello has the host build its sessions dark from the first snapshot",
          "[vthost][attach][colors]")
{
    using vthost::testing::FakeDualPaletteCatalog;
    SECTION("the session the attach spawns")
    {
        auto const observation = attachDarkClient(false, proto::WireColorPreference::Dark);
        REQUIRE_FALSE(observation.seen.empty());
        for (auto const& background: observation.seen)
            CHECK(background == FakeDualPaletteCatalog::DarkBackground);
        REQUIRE(observation.hostEnds.size() == 1);
        CHECK(observation.hostEnds.front() == FakeDualPaletteCatalog::DarkBackground);
    }

    SECTION("a session the host already had, re-seeded before its snapshot")
    {
        auto const observation = attachDarkClient(true, proto::WireColorPreference::Dark);
        REQUIRE_FALSE(observation.seen.empty());
        for (auto const& background: observation.seen)
            CHECK(background == FakeDualPaletteCatalog::DarkBackground);
    }

    SECTION("a hello that states none leaves the host's own light palette")
    {
        auto const observation = attachDarkClient(true, proto::WireColorPreference::Unstated);
        REQUIRE_FALSE(observation.seen.empty());
        for (auto const& background: observation.seen)
            CHECK(background == FakeDualPaletteCatalog::LightBackground);
    }
}

TEST_CASE("a client authors a new window, honored by the daemon (B4)", "[vthost][attach]")
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
    host.createTab(); // the daemon starts with one window (with one tab)

    auto pair = *core::net::testing::makeSocketPair(loop);
    auto server = NativeSession {
        loop, host, vthost::ConnectionId { .endpoint = "test", .index = 1 }, std::move(pair.first)
    };
    // Every LayoutState the client receives names its window; collect the ids.
    auto windows = std::set<uint64_t> {};
    auto client =
        NativeClient { loop,
                       std::move(pair.second),
                       NativeClient::HandshakeOptions {},
                       vtbackend::CommandBlockStoreLimits {},
                       NativeClient::UpdateHandler {},
                       NativeClient::ImageHandler {},
                       NativeClient::SessionEventHandler {},
                       NativeClient::LayoutHandler { [&windows](proto::LayoutState const& received) {
                           windows.insert(received.window);
                       } } };

    loop.blockOn(
        core::net::testing::allOf(server.run(), client.run(), driveCreateWindow(&loop, &client, &windows)));

    CHECK(windows.size() == 2);             // the client saw two distinct windows' layouts
    CHECK(host.model().windowCount() == 2); // the daemon really created a second window
}

TEST_CASE("RemoteScreen replaces its block table on a snapshot and stamps only what changed",
          "[vthost][attach][blocks]")
{
    auto screen = RemoteScreen {};
    auto first = proto::SessionState {};
    first.commandBlocks = { finishedWire(1), runningWire(2) };
    screen.apply(first);
    REQUIRE(screen.commandBlocks.size() == 2);
    CHECK(screen.commandBlocks.at(1).wire == finishedWire(1));
    CHECK(screen.commandBlocksRetiredAt == 0); // nothing was there to remove
    auto const revisionOfTwo = screen.commandBlocks.at(2).revision;

    auto second = proto::SessionState {};
    second.commandBlocks = { runningWire(2), promptingWire(3) };
    screen.apply(second);

    CHECK_FALSE(screen.commandBlocks.contains(1));               // the host no longer holds it ...
    CHECK(screen.commandBlocksRetiredAt > revisionOfTwo);        // ... and the mirror is told to look
    CHECK(screen.commandBlocks.at(2).revision == revisionOfTwo); // unchanged: not news
    CHECK(screen.commandBlocks.at(3).revision > revisionOfTwo);  // new: news
    CHECK(screen.commandBlocksRevision >= screen.commandBlocks.at(3).revision);

    auto const settled = screen.commandBlocksRevision;
    screen.apply(second); // a resync stating exactly what is held: no revision spent
    CHECK(screen.commandBlocksRevision == settled);
}

namespace
{
/// Whether @p screen's revision index names every record of its block table once, under the revision the
/// record last changed at, and nothing else.
bool revisionIndexMatches(RemoteScreen const& screen)
{
    return screen.commandBlocksByRevision.size() == screen.commandBlocks.size()
           && std::ranges::all_of(screen.commandBlocks, [&](auto const& entry) {
                  auto const indexed = screen.commandBlocksByRevision.find(entry.second.revision);
                  return indexed != screen.commandBlocksByRevision.end() && indexed->second == entry.first;
              });
}
} // namespace

TEST_CASE("RemoteScreen indexes every block under the revision it last changed at",
          "[vthost][attach][blocks]")
{
    auto screen = RemoteScreen {};
    auto attach = proto::SessionState {};
    attach.commandBlocks = { finishedWire(1), finishedWire(2), runningWire(3) };
    screen.apply(attach);
    CHECK(revisionIndexMatches(screen));

    auto finished = proto::Delta {};
    finished.commandBlocks = { finishedWire(3), promptingWire(4) };
    finished.retiredCommandBlocks = { 1 };
    screen.apply(finished);
    CHECK(revisionIndexMatches(screen));
    // What changed since the attach, newest change last.
    auto const changed = std::ranges::to<std::vector>(
        std::ranges::subrange(screen.commandBlocksByRevision.upper_bound(screen.commandBlocks.at(2).revision),
                              screen.commandBlocksByRevision.end())
        | std::views::values);
    CHECK(changed == std::vector<uint32_t> { 3, 4 });

    auto resync = proto::SessionState {};
    resync.commandBlocksFloor = 3;
    resync.commandBlocks = { runningWire(4) }; // 3 is gone on the host, 4 started
    screen.apply(resync);
    CHECK(revisionIndexMatches(screen));
    CHECK(screen.commandBlocks.size() == 2);
}

TEST_CASE("RemoteScreen holds a peer's records to what a record keeps", "[vthost][attach][blocks]")
{
    // A peer chooses what it sends: a 10 MB command line or working directory must cost this client what a
    // record costs the store the mirror feeds, not ten megabytes per record.
    constexpr auto Flood = std::size_t { 10 } * 1024 * 1024;
    auto screen = RemoteScreen {};
    auto hostile = proto::Delta {};
    hostile.commandBlocks = { runningWire(1), runningWire(2) };
    hostile.commandBlocks[0].commandLine = std::string(Flood, 'x');
    hostile.commandBlocks[1].workingDirectory = std::string(Flood, 'y');
    hostile.commandBlocks[1].locality = std::to_underlying(vtbackend::ContextLocality::Local);
    hostile.commandBlocks[1].endWorkingDirectory = std::string(Flood, 'z');
    hostile.commandBlocks[1].endLocality = std::to_underlying(vtbackend::ContextLocality::Local);
    screen.apply(hostile);

    auto const& commandLine = screen.commandBlocks.at(1).wire.commandLine;
    CHECK(commandLine == vtbackend::truncatedCommandLine(std::string(Flood, 'x')));
    CHECK(commandLine.capacity() < vtbackend::MaxRecordedCommandLineBytes * 2);
    auto const& wire = screen.commandBlocks.at(2).wire;
    CHECK(wire.workingDirectory.empty()); // dropped whole, as the store drops one: a cut path is another
    CHECK(wire.workingDirectory.capacity() < vtbackend::MaxRecordedWorkingDirectoryBytes);
    CHECK(wire.locality == std::to_underlying(vtbackend::ContextLocality::Unknown));
    CHECK(wire.endWorkingDirectory.empty()); // where the command left the shell, likewise
    CHECK(wire.endWorkingDirectory.capacity() < vtbackend::MaxRecordedWorkingDirectoryBytes);
    CHECK(wire.endLocality == std::to_underlying(vtbackend::ContextLocality::Unknown));

    auto const settled = screen.commandBlocksRevision;
    screen.apply(hostile); // the same flood again is a resend
    CHECK(screen.commandBlocksRevision == settled);
}

TEST_CASE("RemoteScreen keeps no more records than its cap, the oldest dropped first",
          "[vthost][attach][blocks]")
{
    auto screen = RemoteScreen { RemoteScreen::DefaultHistoryKeep,
                                 vtbackend::CommandBlockStoreLimits { .maxRecords = 3 } };

    SECTION("a delta naming ids beyond the cap")
    {
        auto flood = proto::Delta {};
        for (auto const id: std::views::iota(uint32_t { 1 }, uint32_t { 1001 }))
            flood.commandBlocks.push_back(finishedWire(id));
        screen.apply(flood);

        CHECK(std::ranges::to<std::vector>(screen.commandBlocks | std::views::keys)
              == std::vector<uint32_t> { 998, 999, 1000 });
        CHECK(screen.commandBlocksByRevision.size() == 3);
        CHECK(screen.commandBlocksRetiredAt == screen.commandBlocksRevision); // a mirror drops them too

        // Older than all a full table keeps: not taken, and no revision spent on it.
        auto const settled = screen.commandBlocksRevision;
        auto stale = proto::Delta {};
        stale.commandBlocks = { runningWire(7) };
        screen.apply(stale);
        CHECK_FALSE(screen.commandBlocks.contains(7));
        CHECK(screen.commandBlocksRevision == settled);
    }

    SECTION("a snapshot stating more than the cap")
    {
        auto attach = proto::SessionState {};
        for (auto const id: std::views::iota(uint32_t { 1 }, uint32_t { 11 }))
            attach.commandBlocks.push_back(finishedWire(id));
        screen.apply(attach);

        CHECK(std::ranges::to<std::vector>(screen.commandBlocks | std::views::keys)
              == std::vector<uint32_t> { 8, 9, 10 });
        CHECK(screen.commandBlocksByRevision.size() == 3);
    }

    SECTION("a cap of zero still keeps the newest")
    {
        auto bare = RemoteScreen { RemoteScreen::DefaultHistoryKeep,
                                   vtbackend::CommandBlockStoreLimits { .maxRecords = 0 } };
        auto two = proto::Delta {};
        two.commandBlocks = { finishedWire(1), runningWire(2) };
        bare.apply(two);
        CHECK(std::ranges::to<std::vector>(bare.commandBlocks | std::views::keys)
              == std::vector<uint32_t> { 2 });
    }
}

TEST_CASE("RemoteScreen keeps an unchanged block's entry through a snapshot", "[vthost][attach][blocks]")
{
    // A resize resyncs every pane with a snapshot naming every record, nearly all of them unchanged: each
    // keeps the entry it has, rather than a copy of the wire taking its place.
    auto screen = RemoteScreen {};
    auto attach = proto::SessionState {};
    attach.commandBlocks = { finishedWire(1), runningWire(2) };
    attach.commandBlocks.front().commandLine = std::string(100, 'x'); // held on the heap, not inline
    screen.apply(attach);
    auto const* const entry = &screen.commandBlocks.at(1);
    auto const* const commandLine = entry->wire.commandLine.data();

    auto resync = attach;
    resync.commandBlocks.back().state = 2; // the other one finished meanwhile
    screen.apply(resync);

    CHECK(&screen.commandBlocks.at(1) == entry);
    CHECK(screen.commandBlocks.at(1).wire.commandLine.data() == commandLine);
    CHECK(screen.commandBlocks.at(2).wire.state == 2);
}

TEST_CASE("RemoteScreen keeps the blocks below a snapshot's floor", "[vthost][attach][blocks]")
{
    // A capped snapshot speaks only from the oldest record it states on: what is held below that stays
    // until a delta retires it, and only what is held from there on is replaced.
    auto screen = RemoteScreen {};
    auto attach = proto::SessionState {};
    attach.commandBlocks = { finishedWire(1), finishedWire(2), finishedWire(3), runningWire(4) };
    screen.apply(attach);
    auto const retiredAt = screen.commandBlocksRetiredAt;
    auto const revisionOfOne = screen.commandBlocks.at(1).revision;

    auto capped = proto::SessionState {};
    capped.commandBlocksFloor = 3;
    capped.commandBlocks = { finishedWire(3), promptingWire(5) }; // 4 is gone on the host
    screen.apply(capped);

    CHECK(screen.commandBlocks.contains(1)); // below the floor: kept, unchanged
    CHECK(screen.commandBlocks.contains(2));
    CHECK(screen.commandBlocks.at(1).revision == revisionOfOne);
    CHECK_FALSE(screen.commandBlocks.contains(4)); // from the floor on, what it no longer names is gone
    CHECK(screen.commandBlocks.contains(5));
    CHECK(screen.commandBlocksRetiredAt > retiredAt);

    auto const settled = screen.commandBlocksRetiredAt;
    screen.apply(capped); // the same floor again retires nothing below it
    CHECK(screen.commandBlocksRetiredAt == settled);
    CHECK(screen.commandBlocks.size() == 4);
}

TEST_CASE("RemoteScreen merges incremental blocks and erases retired ones", "[vthost][attach][blocks]")
{
    auto screen = RemoteScreen {};
    auto running = proto::Delta {};
    running.commandBlocks = { runningWire(5) };
    screen.apply(running);
    auto const first = screen.commandBlocks.at(5).revision;
    CHECK(first > 0);

    screen.apply(running); // the same record again: not news
    CHECK(screen.commandBlocks.at(5).revision == first);
    CHECK(screen.commandBlocksRevision == first);

    auto finished = proto::Delta {};
    finished.commandBlocks = { finishedWire(5) };
    screen.apply(finished);
    CHECK(screen.commandBlocks.at(5).wire.state == 2);
    CHECK(screen.commandBlocks.at(5).revision > first);

    auto retire = proto::Delta {};
    retire.retiredCommandBlocks = { 5, 99 }; // 99 was never held: retiring it is a no-op
    screen.apply(retire);
    CHECK(screen.commandBlocks.empty());
    CHECK(screen.commandBlocksRetiredAt == screen.commandBlocksRevision);

    auto const quiet = screen.commandBlocksRevision;
    screen.apply(retire); // nothing left to retire: no revision spent
    CHECK(screen.commandBlocksRevision == quiet);
}

namespace
{
/// Waits until @p seen holds what the first update saw, then detaches @p client.
Task<void> awaitFirstUpdate(core::net::EventLoop* loop,
                            NativeClient* client,
                            std::optional<vtbackend::HostIdentity> const* seen)
{
    co_await core::net::testing::waitUntil(loop, [seen] { return seen->has_value(); });
    client->detach();
}
} // namespace

TEST_CASE("an attached client knows the daemon's machine before it mirrors a session", "[vthost][attach]")
{
    // A record's locality says "local to the daemon's machine"; a client attached from elsewhere can
    // only judge it if the daemon says which machine that is -- and must know before the first mirror of
    // a session is built, since that mirror is judged by it for life.
    auto const daemon =
        vtbackend::HostIdentity { .machineId = "3deb5353d3ba43d08201c136a47ead7b", .hostname = "zeta" };
    auto const source = core::net::makeDefaultBackend();
    auto loop = core::net::EventLoop { *source };
    auto host = SessionHost { loop,
                              [](vtbackend::PageSize size, std::optional<vtpty::Process::ExecInfo> const&) {
                                  return std::make_unique<vtpty::MockPty>(size);
                              },
                              vtbackend::Settings {},
                              core::defaultEnvironment(),
                              vthost::SessionHostConfig { .pumping = vthost::SessionPumping::Manual,
                                                          .sizePolicy = vthost::ClientSizePolicy::Latest,
                                                          .startupLayout = vtworkspace::Layout {},
                                                          .identity = daemon } };
    host.createTab();

    auto pair = *core::net::testing::makeSocketPair(loop);
    auto server = NativeSession {
        loop, host, vthost::ConnectionId { .endpoint = "test", .index = 1 }, std::move(pair.first)
    };
    auto seenAtFirstUpdate = std::optional<vtbackend::HostIdentity> {};
    NativeClient const* attached = nullptr;
    auto client = NativeClient { loop,
                                 std::move(pair.second),
                                 NativeClient::HandshakeOptions {},
                                 vtbackend::CommandBlockStoreLimits {},
                                 NativeClient::UpdateHandler { [&](RemoteScreen const&, proto::Delta const&) {
                                     if (!seenAtFirstUpdate)
                                         seenAtFirstUpdate = attached->daemonIdentity();
                                 } },
                                 NativeClient::ImageHandler {},
                                 NativeClient::SessionEventHandler {},
                                 NativeClient::LayoutHandler {} };
    attached = &client;

    loop.blockOn(core::net::testing::allOf(
        server.run(), client.run(), awaitFirstUpdate(&loop, &client, &seenAtFirstUpdate)));

    REQUIRE(seenAtFirstUpdate.has_value());
    CHECK(*seenAtFirstUpdate == vthost::appSpecificIdentity(daemon.view()));
    CHECK(client.daemonIdentity() == *seenAtFirstUpdate);
}

TEST_CASE("NativeClient.extension.permission", "[vthost][attach][extensions]")
{
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
    auto const sessionId = newSession(host);
    hosting->sessions.front()->askPermission(proto::PermissionRequest {
        .session = 0,
        .hostId = "git@/x/git.wasm",
        .name = "git",
        .version = 1,
        .digest = "5f3c",
        .commands = { proto::WireDeclaredCommand { .name = "status", .argv = { "git", "status" } } } });

    auto pair = *core::net::testing::makeSocketPair(loop);
    auto server = std::make_unique<NativeSession>(
        loop, host, vthost::ConnectionId { .endpoint = "test", .index = 1 }, std::move(pair.first));
    auto asked = std::vector<proto::PermissionRequest> {};
    auto client = std::unique_ptr<NativeClient> {};
    client = std::make_unique<NativeClient>(
        loop,
        std::move(pair.second),
        // Offered by whoever passes the handler, as the composition root does.
        NativeClient::HandshakeOptions {
            .capabilities = proto::CapabilitySet::of({ proto::Capability::ExtensionPermissions }) },
        vtbackend::CommandBlockStoreLimits {},
        NativeClient::UpdateHandler {},
        NativeClient::ImageHandler {},
        NativeClient::SessionEventHandler {},
        NativeClient::LayoutHandler {},
        NativeClient::PermissionHandler { [&](proto::PermissionRequest const& request) {
            asked.push_back(request);
            client->answerPermission(request.hostId, request.digest, vthost::GrantState::Denied);
        } });

    auto scenario = [](core::net::EventLoop* loop,
                       NativeClient* client,
                       vthost::testing::FakeExtensionHosting* hosting) -> Task<void> {
        co_await core::net::testing::waitUntil(loop, [&] { return !hosting->answers.empty(); });
        client->detach();
    }(&loop, client.get(), hosting.get());
    loop.blockOn(core::net::testing::allOf(server->run(), client->run(), std::move(scenario)));

    REQUIRE(asked.size() == 1);
    CHECK(asked.front().session == sessionId.value);
    CHECK(asked.front().name == "git");
    CHECK(hosting->lookup("git@/x/git.wasm", "5f3c") == vthost::GrantState::Denied);
    CHECK(host.pendingPermissionRequests().empty());
}

#ifdef CONTOUR_EXTENSIONS
TEST_CASE("DaemonGrantReplies sends each answer, and remembers it", "[vthost][attach][extensions]")
{
    struct Sent
    {
        std::string hostId;
        std::string digest;
        vthost::GrantState answer;
    };
    auto sent = std::vector<Sent> {};
    auto replies = vthost::client::DaemonGrantReplies {
        [&](std::string_view hostId, std::string_view digest, vthost::GrantState answer) {
            sent.push_back(Sent {
                .hostId = std::string { hostId }, .digest = std::string { digest }, .answer = answer });
        }
    };
    CHECK(replies.lookup("git@/x.wasm", "d1") == vtextension::GrantState::Pending);

    replies.record("git@/x.wasm", "d1", vtextension::GrantState::Granted);
    REQUIRE(sent.size() == 1);
    CHECK(sent.back().hostId == "git@/x.wasm");
    CHECK(sent.back().digest == "d1");
    CHECK(sent.back().answer == vthost::GrantState::Granted);
    CHECK(replies.lookup("git@/x.wasm", "d1") == vtextension::GrantState::Granted);
    CHECK(replies.lookup("git@/x.wasm", "d2") == vtextension::GrantState::Pending);

    replies.record("git@/x.wasm", "d2", vtextension::GrantState::Denied);
    CHECK(sent.back().answer == vthost::GrantState::Denied);
    CHECK(replies.lookup("git@/x.wasm", "d2") == vtextension::GrantState::Denied);
}

TEST_CASE("a daemon's question keeps the control permissions it asks for",
          "[vthost][attach][extensions][control]")
{
    auto manifest = vtextension::Manifest { .name = "agent", .version = 1, .commands = {}, .digest = "d1" };
    manifest.control = { vtcontrol::Permission::Read, vtcontrol::Permission::Input };
    auto const request = vthost::permissionRequestOf("agent@/x.wasm", manifest);
    CHECK(request.control == std::vector<std::string> { "read", "input" });

    auto const consent = vthost::client::consentRequestOf(request);
    REQUIRE(consent.has_value());
    CHECK(consent->manifest.control == manifest.control);
    auto const question = vtextension::consentQuestion(*consent);
    CHECK(std::ranges::any_of(question.lines, [](vtextension::ConsentLine const& line) {
        return line.text == "May type into panes (can run commands).";
    }));

    // A permission this build cannot name cannot be shown, so the question is not asked.
    auto unknown = request;
    unknown.control.emplace_back("fly");
    CHECK_FALSE(vthost::client::consentRequestOf(unknown).has_value());
}
#endif

namespace
{
/// What a scripted daemon writes: each PDU under its serial, none standing for the serial of the frame
/// answered.
using Script = std::vector<std::pair<std::optional<uint64_t>, proto::DecodedPdu>>;

/// What a scripted daemon writes in answer to each frame the client sends.
using ScriptFor = std::function<Script(proto::DecodedFrame const&)>;

/// A hand-driven daemon that writes what @p answer gives for each frame the client sends and records every
/// such frame into @p seen, until the client hangs up.
Task<void> scriptedDaemon(core::net::EventLoop* loop,
                          core::net::ISocket* socket,
                          ScriptFor answer,
                          std::vector<proto::DecodedFrame>* seen)
{
    auto writer = core::net::WriteQueue { *loop, socket, std::size_t { 1 } * 1024 * 1024 };
    co_await vthost::pumpPdus(socket, [&](proto::DecodedFrame const& frame) {
        seen->push_back(frame);
        for (auto const& [serial, pdu]: answer(frame))
            enqueuePdu(writer, serial.value_or(frame.serial), pdu);
        return true;
    });
}

/// @return What a scripted daemon writes: @p script in answer to the hello, nothing to anything else.
[[nodiscard]] ScriptFor answeringHello(Script script)
{
    return [script = std::move(script)](proto::DecodedFrame const& frame) {
        return std::holds_alternative<proto::ClientHello>(frame.pdu) ? script : Script {};
    };
}

/// @return A client of @p loop over @p socket, with no handlers.
[[nodiscard]] std::unique_ptr<NativeClient> bareClient(core::net::EventLoop& loop,
                                                       std::unique_ptr<core::net::ISocket> socket)
{
    return std::make_unique<NativeClient>(loop,
                                          std::move(socket),
                                          NativeClient::HandshakeOptions {},
                                          vtbackend::CommandBlockStoreLimits {},
                                          NativeClient::UpdateHandler {},
                                          NativeClient::ImageHandler {},
                                          NativeClient::SessionEventHandler {},
                                          NativeClient::LayoutHandler {});
}

/// @return Whether @p frame carries a PDU of type @p T.
template <typename T>
[[nodiscard]] bool carries(proto::DecodedFrame const& frame)
{
    return std::holds_alternative<T>(frame.pdu);
}
} // namespace

TEST_CASE("a PDU the daemon refused is not sent again", "[vthost][negotiation]")
{
    auto const source = core::net::makeDefaultBackend();
    auto loop = core::net::EventLoop { *source };
    auto pair = *core::net::testing::makeSocketPair(loop);
    auto* daemonSocket = pair.first.get();
    auto client = bareClient(loop, std::move(pair.second));

    // The hello's answer goes under the hello's serial; the refusal is pushed, as an unsolicited frame.
    auto script = Script {
        { std::nullopt, acceptingHello() },
        { 7,
          proto::Unsupported { .refusedSerial = 3,
                               .ident = std::to_underlying(proto::PduType::ResizePane) } },
    };
    auto seen = std::vector<proto::DecodedFrame> {};
    auto scenario = [](core::net::EventLoop* loop,
                       NativeClient* client,
                       std::vector<proto::DecodedFrame> const* seen) -> Task<void> {
        co_await core::net::testing::waitUntil(
            loop, [&] { return client->peerRefused(proto::PduType::ResizePane); });
        client->resizePane(1, 80, 24);
        // A PDU the daemon did not refuse, behind it: once it arrives, the ResizePane would have.
        client->createTab(0, {});
        co_await core::net::testing::waitUntil(
            loop, [&] { return std::ranges::any_of(*seen, carries<proto::CreateTab>); });
        client->detach();
    }(&loop, client.get(), &seen);
    loop.blockOn(core::net::testing::allOf(
        client->run(),
        scriptedDaemon(&loop, daemonSocket, answeringHello(std::move(script)), &seen),
        std::move(scenario)));

    CHECK(client->peerRefused(proto::PduType::ResizePane));
    CHECK_FALSE(client->peerRefused(proto::PduType::CreateTab));
    CHECK(std::ranges::any_of(seen, carries<proto::CreateTab>));
    CHECK_FALSE(std::ranges::any_of(seen, carries<proto::ResizePane>));
}

TEST_CASE("a client answers a daemon's unknown PDU with Unsupported, after the handshake only",
          "[vthost][negotiation]")
{
    auto const source = core::net::makeDefaultBackend();
    auto loop = core::net::EventLoop { *source };
    auto pair = *core::net::testing::makeSocketPair(loop);
    auto* daemonSocket = pair.first.get();
    auto client = bareClient(loop, std::move(pair.second));

    auto script = Script {
        { 8, proto::Invalid { .ident = 201 } }, // before the ServerHello: not answered
        { std::nullopt, acceptingHello() },
        { 9, proto::Invalid { .ident = 200 } },
    };
    auto seen = std::vector<proto::DecodedFrame> {};
    auto scenario = [](core::net::EventLoop* loop,
                       NativeClient* client,
                       std::vector<proto::DecodedFrame> const* seen) -> Task<void> {
        co_await core::net::testing::waitUntil(
            loop, [&] { return std::ranges::any_of(*seen, carries<proto::Unsupported>); });
        client->detach();
    }(&loop, client.get(), &seen);
    loop.blockOn(core::net::testing::allOf(
        client->run(),
        scriptedDaemon(&loop, daemonSocket, answeringHello(std::move(script)), &seen),
        std::move(scenario)));

    // A reply to the first would have been written before the reply to the second.
    REQUIRE(std::ranges::count_if(seen, carries<proto::Unsupported>) == 1);
    CHECK(std::ranges::find_if(seen, carries<proto::Unsupported>)->pdu
          == proto::DecodedPdu { proto::Unsupported { .refusedSerial = 9, .ident = 200 } });
    CHECK(client->connected());
}

TEST_CASE("a client applies no session PDU sent before the ServerHello", "[vthost][negotiation]")
{
    auto capture = core::log::ScopedCapture {};
    auto const source = core::net::makeDefaultBackend();
    auto loop = core::net::EventLoop { *source };
    auto pair = *core::net::testing::makeSocketPair(loop);
    auto* daemonSocket = pair.first.get();
    auto layouts = 0;
    auto client =
        NativeClient { loop,
                       std::move(pair.second),
                       NativeClient::HandshakeOptions {},
                       vtbackend::CommandBlockStoreLimits {},
                       NativeClient::UpdateHandler {},
                       NativeClient::ImageHandler {},
                       NativeClient::SessionEventHandler {},
                       NativeClient::LayoutHandler { [&](proto::LayoutState const&) { ++layouts; } } };

    // Session 5's PDUs come before the ServerHello and must not be applied; session 6's come after it, and
    // once it is mirrored, the earlier ones would have been.
    auto const stateOf = [](uint64_t session) {
        auto state = proto::SessionState {};
        state.session = session;
        state.columns = 80;
        state.lines = 24;
        return state;
    };
    auto delta = proto::Delta {};
    delta.session = 5;
    auto script = Script {
        { 0, stateOf(5) }, { 0, delta }, { 0, proto::LayoutState {} }, { std::nullopt, acceptingHello() },
        { 0, stateOf(6) },
    };
    auto seen = std::vector<proto::DecodedFrame> {};
    auto scenario = [](core::net::EventLoop* loop, NativeClient* client) -> Task<void> {
        co_await core::net::testing::waitUntil(loop, [&] { return client->screens().contains(6); });
        client->detach();
    }(&loop, &client);
    loop.blockOn(core::net::testing::allOf(
        client.run(),
        scriptedDaemon(&loop, daemonSocket, answeringHello(std::move(script)), &seen),
        std::move(scenario)));

    CHECK(client.screens().contains(6));
    CHECK_FALSE(client.screens().contains(5));
    CHECK(layouts == 0);
    CHECK(capture.contains("ignoring SessionState before the handshake"));
    CHECK(capture.contains("ignoring Delta before the handshake"));
    CHECK(capture.contains("ignoring LayoutState before the handshake"));
}

namespace
{
/// A daemon that answers the client's first frame with the bytes @p reply makes for its serial, and hangs up.
Task<void> rawDaemon(core::net::ISocket* socket, std::function<std::vector<std::byte>(uint64_t)> reply)
{
    auto buffer = std::vector<std::byte> {};
    auto scratch = std::vector<std::byte>(4096);
    auto frame = proto::readFrame(buffer);
    while (!frame && frame.error() == proto::DecodeError::NeedMoreData)
    {
        auto const n = co_await socket->read(scratch);
        if (!n.has_value() || *n == 0)
            co_return;
        buffer.insert(buffer.end(), scratch.begin(), scratch.begin() + static_cast<long>(*n));
        frame = proto::readFrame(buffer);
    }
    if (frame)
    {
        auto const bytes = reply(frame->serial);
        std::ignore = co_await socket->write(std::span<std::byte const> { bytes });
    }
    socket->close();
}

/// @return The capabilities of a client that can answer an extension's permission question.
[[nodiscard]] proto::CapabilitySet answeringPermissions()
{
    return proto::CapabilitySet::of({ proto::Capability::ExtensionPermissions });
}
} // namespace

TEST_CASE("the client reports an old daemon's refusal with both versions", "[vthost][negotiation]")
{
    auto const source = core::net::makeDefaultBackend();
    auto loop = core::net::EventLoop { *source };
    auto pair = *core::net::testing::makeSocketPair(loop);
    auto* daemonSocket = pair.first.get();
    auto client = bareClient(loop, std::move(pair.second));

    loop.blockOn(core::net::testing::allOf(client->run(), rawDaemon(daemonSocket, [](uint64_t serial) {
                                               return vthost::testing::codec3ServerHello(serial);
                                           })));

    CHECK(client->refusal() == proto::HandshakeRefusal { .ours = { 4, 4 }, .theirs = { 3, 3 } });
    CHECK_FALSE(client->negotiated().has_value());
    CHECK_FALSE(client->connected());
}

TEST_CASE("the client reports a disjoint range", "[vthost][negotiation]")
{
    auto const source = core::net::makeDefaultBackend();
    auto loop = core::net::EventLoop { *source };
    auto pair = *core::net::testing::makeSocketPair(loop);
    auto* daemonSocket = pair.first.get();
    auto client = bareClient(loop, std::move(pair.second));

    auto script = Script {
        { std::nullopt,
          proto::ServerHello { .agreedVersion = 0, .versions = { .min = 6, .max = 8 }, .capabilities = {} } },
    };
    auto seen = std::vector<proto::DecodedFrame> {};
    // No detach: run() ends on its own, once the refusal arrived.
    loop.blockOn(core::net::testing::allOf(
        client->run(), scriptedDaemon(&loop, daemonSocket, answeringHello(std::move(script)), &seen)));

    REQUIRE(client->refusal().has_value());
    CHECK(client->refusal()->ours == proto::SupportedVersions);
    CHECK(client->refusal()->theirs == proto::VersionRange { .min = 6, .max = 8 });
    CHECK_FALSE(client->negotiated().has_value());
    CHECK_FALSE(client->connected());
}

TEST_CASE("the client takes any agreed version both ends speak, and no other", "[vthost][negotiation]")
{
    /// What the client makes of the agreed version.
    enum class Verdict : uint8_t
    {
        Refused,
        Accepted,
    };
    struct Row
    {
        std::string_view name;
        uint32_t agreed;
        Verdict verdict;
    };
    // The client speaks 4-6 and the daemon 4-9: the daemon may pick lower than the highest common version.
    auto const rows = std::array {
        Row { .name = "a lower common version", .agreed = 5, .verdict = Verdict::Accepted },
        Row { .name = "one only the daemon speaks", .agreed = 9, .verdict = Verdict::Refused },
    };
    for (auto const& row: rows)
    {
        INFO(row.name);
        auto const source = core::net::makeDefaultBackend();
        auto loop = core::net::EventLoop { *source };
        auto pair = *core::net::testing::makeSocketPair(loop);
        auto* daemonSocket = pair.first.get();
        auto client = NativeClient { loop,
                                     std::move(pair.second),
                                     NativeClient::HandshakeOptions { .versions = { .min = 4, .max = 6 } },
                                     vtbackend::CommandBlockStoreLimits {},
                                     NativeClient::UpdateHandler {},
                                     NativeClient::ImageHandler {},
                                     NativeClient::SessionEventHandler {},
                                     NativeClient::LayoutHandler {} };

        auto script = Script {
            { std::nullopt,
              proto::ServerHello {
                  .agreedVersion = row.agreed, .versions = { .min = 4, .max = 9 }, .capabilities = {} } },
        };
        auto seen = std::vector<proto::DecodedFrame> {};
        auto scenario = [](core::net::EventLoop* loop, NativeClient* client) -> Task<void> {
            co_await core::net::testing::waitUntil(
                loop, [&] { return client->connected() || client->refusal().has_value(); });
            client->detach();
        }(&loop, &client);
        loop.blockOn(core::net::testing::allOf(
            client.run(),
            scriptedDaemon(&loop, daemonSocket, answeringHello(std::move(script)), &seen),
            std::move(scenario)));

        if (row.verdict == Verdict::Accepted)
        {
            REQUIRE(client.negotiated().has_value());
            CHECK(client.negotiated()->version == row.agreed);
            CHECK_FALSE(client.refusal().has_value());
            continue;
        }
        CHECK_FALSE(client.negotiated().has_value());
        REQUIRE(client.refusal().has_value());
        CHECK(
            *client.refusal()
            == proto::HandshakeRefusal { .ours = { .min = 4, .max = 6 }, .theirs = { .min = 9, .max = 9 } });
        CHECK(proto::describe(*client.refusal(), proto::PeerRole::Daemon)
              == "the daemon speaks protocol 9-9, this client speaks 4-6");
    }
}

TEST_CASE("a live client and daemon agree on the highest common version", "[vthost][negotiation]")
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
    host.createTab();
    auto pair = *core::net::testing::makeSocketPair(loop);
    auto server = NativeSession {
        loop,
        host,
        vthost::ConnectionId { .endpoint = "test", .index = 1 },
        std::move(pair.first),
        NativeSession::DefaultWriteQueueBytes,
        vthost::NativeSessionServices { .handshake = { .versions = { .min = 4, .max = 6 } } },
    };
    auto client = NativeClient {
        loop,
        std::move(pair.second),
        NativeClient::HandshakeOptions { .versions = { .min = 5, .max = 7 },
                                         .capabilities = answeringPermissions() },
        vtbackend::CommandBlockStoreLimits {},
        NativeClient::UpdateHandler {},
        NativeClient::ImageHandler {},
        NativeClient::SessionEventHandler {},
        NativeClient::LayoutHandler {},
        NativeClient::PermissionHandler { [](proto::PermissionRequest const&) {} },
    };

    auto scenario = [](core::net::EventLoop* loop, NativeClient* client) -> Task<void> {
        co_await core::net::testing::waitUntil(loop, [&] { return client->connected(); });
        client->detach();
    }(&loop, &client);
    loop.blockOn(core::net::testing::allOf(server.run(), client.run(), std::move(scenario)));

    REQUIRE(client.negotiated().has_value());
    REQUIRE(server.negotiated().has_value());
    CHECK(client.negotiated()->version == 6);
    CHECK(server.negotiated()->version == 6);
    CHECK(client.negotiated()->shared == answeringPermissions());
    CHECK_FALSE(client.refusal().has_value());
}

TEST_CASE("a permission answer is sent only to a daemon that offered to ask", "[vthost][negotiation]")
{
    /// Whether the answer reaches the daemon.
    enum class Answer : uint8_t
    {
        Withheld,
        Sent,
    };
    struct Row
    {
        std::string_view name;
        proto::CapabilitySet daemon;
        Answer answer;
    };
    auto const rows = std::array {
        Row { .name = "the daemon offers it", .daemon = answeringPermissions(), .answer = Answer::Sent },
        Row { .name = "the daemon does not", .daemon = {}, .answer = Answer::Withheld },
    };
    for (auto const& row: rows)
    {
        INFO(row.name);
        auto const source = core::net::makeDefaultBackend();
        auto loop = core::net::EventLoop { *source };
        auto pair = *core::net::testing::makeSocketPair(loop);
        auto* daemonSocket = pair.first.get();
        auto client = NativeClient {
            loop,
            std::move(pair.second),
            NativeClient::HandshakeOptions { .capabilities = answeringPermissions() },
            vtbackend::CommandBlockStoreLimits {},
            NativeClient::UpdateHandler {},
            NativeClient::ImageHandler {},
            NativeClient::SessionEventHandler {},
            NativeClient::LayoutHandler {},
            NativeClient::PermissionHandler { [](proto::PermissionRequest const&) {} },
        };
        auto seen = std::vector<proto::DecodedFrame> {};
        auto scenario = [](core::net::EventLoop* loop,
                           NativeClient* client,
                           std::vector<proto::DecodedFrame> const* seen) -> Task<void> {
            co_await core::net::testing::waitUntil(loop, [&] { return client->connected(); });
            client->answerPermission("git@/x/git.wasm", "5f3c", vthost::GrantState::Granted);
            // Behind it: once this arrived, the answer would have.
            client->createTab(0, {});
            co_await core::net::testing::waitUntil(
                loop, [&] { return std::ranges::any_of(*seen, carries<proto::CreateTab>); });
            client->detach();
        }(&loop, &client, &seen);
        loop.blockOn(core::net::testing::allOf(
            client.run(),
            scriptedDaemon(
                &loop, daemonSocket, answeringHello({ { std::nullopt, acceptingHello(row.daemon) } }), &seen),
            std::move(scenario)));

        REQUIRE(std::ranges::any_of(seen, carries<proto::CreateTab>));
        CHECK(std::ranges::any_of(seen, carries<proto::PermissionReply>) == (row.answer == Answer::Sent));
    }
}

TEST_CASE("an image fetch that was not sent awaits no reply", "[vthost][negotiation]")
{
    auto capture = core::log::ScopedCapture {};
    auto const source = core::net::makeDefaultBackend();
    auto loop = core::net::EventLoop { *source };
    auto pair = *core::net::testing::makeSocketPair(loop);
    auto* daemonSocket = pair.first.get();
    auto client = bareClient(loop, std::move(pair.second));

    // The daemon does not know FetchImage; and once the client creates a tab, it pushes an ImageData
    // unsolicited, under serial 0 -- the serial a fetch that was never sent would have recorded.
    auto const answer = [](proto::DecodedFrame const& frame) {
        if (std::holds_alternative<proto::ClientHello>(frame.pdu))
            return Script {
                { std::nullopt, acceptingHello() },
                { 7,
                  proto::Unsupported { .refusedSerial = 2,
                                       .ident = std::to_underlying(proto::PduType::FetchImage) } },
            };
        if (std::holds_alternative<proto::CreateTab>(frame.pdu))
            return Script { { 0, proto::ImageData { .imageId = 7, .width = 0, .height = 0, .data = {} } } };
        return Script {};
    };
    auto seen = std::vector<proto::DecodedFrame> {};
    auto scenario = [](core::net::EventLoop* loop,
                       NativeClient* client,
                       core::log::ScopedCapture* capture) -> Task<void> {
        co_await core::net::testing::waitUntil(
            loop, [&] { return client->peerRefused(proto::PduType::FetchImage); });
        client->fetchImage(1, 7);
        client->createTab(0, {});
        co_await core::net::testing::waitUntil(
            loop, [&] { return client->screens().contains(1) || capture->contains("unrouted ImageData"); });
        client->detach();
    }(&loop, client.get(), &capture);
    loop.blockOn(core::net::testing::allOf(
        client->run(), scriptedDaemon(&loop, daemonSocket, answer, &seen), std::move(scenario)));

    CHECK(capture.contains("unrouted ImageData"));
    CHECK_FALSE(client->screens().contains(1));
}

TEST_CASE("a client reports the release its daemon stated", "[vthost][negotiation]")
{
    auto const source = core::net::makeDefaultBackend();
    auto loop = core::net::EventLoop { *source };
    auto pair = *core::net::testing::makeSocketPair(loop);
    auto* daemonSocket = pair.first.get();
    auto client = NativeClient { loop,
                                 std::move(pair.second),
                                 NativeClient::HandshakeOptions {},
                                 vtbackend::CommandBlockStoreLimits {},
                                 NativeClient::UpdateHandler {},
                                 NativeClient::ImageHandler {},
                                 NativeClient::SessionEventHandler {},
                                 NativeClient::LayoutHandler {} };
    CHECK(client.daemonRelease().empty()); // nothing is known before the ServerHello

    auto hello = acceptingHello();
    hello.releaseVersion = "0.8.1";
    auto seen = std::vector<proto::DecodedFrame> {};
    auto scenario = [](core::net::EventLoop* loop, NativeClient* client) -> Task<void> {
        co_await core::net::testing::waitUntil(loop, [&] { return client->connected(); });
        client->detach();
    }(&loop, &client);
    loop.blockOn(core::net::testing::allOf(
        client.run(),
        scriptedDaemon(&loop, daemonSocket, answeringHello(Script { { std::nullopt, hello } }), &seen),
        std::move(scenario)));

    CHECK(client.daemonRelease() == "0.8.1");
}

namespace
{

/// Everything a phase-4 client offers on top of the version.
[[nodiscard]] proto::CapabilitySet thinClientCapabilities()
{
    return proto::CapabilitySet::of(
        { proto::Capability::AttachIntent, proto::Capability::ProfileByName, proto::Capability::TabEdits });
}

/// VT420 settings of 7000 lines: what a client states beside a profile name, for a daemon without names.
[[nodiscard]] proto::WireSessionSettings vt420Settings()
{
    return proto::WireSessionSettings { .historyLineCount = 7000,
                                        .terminalId = 41, // VT420
                                        .graphemeClustering = 0,
                                        .allowReflowOnResize = 0,
                                        .maxImageRegisterCount = 512,
                                        .sixelScrolling = 0 };
}

/// A real NativeSession serving a real NativeClient, with the handshake each states chosen by the case.
struct IntentHarness
{
    IntentHarness(vthost::NativeSessionServices services,
                  NativeClient::HandshakeOptions handshake,
                  std::shared_ptr<vthost::ProfileCatalog const> profiles = {}):
        host { loop,
               [](vtbackend::PageSize size, std::optional<vtpty::Process::ExecInfo> const&) {
                   return std::make_unique<vtpty::MockPty>(size);
               },
               vtbackend::Settings {},
               core::defaultEnvironment(),
               vthost::SessionHostConfig { .pumping = vthost::SessionPumping::Manual,
                                           .profiles = std::move(profiles) } },
        server { std::make_unique<NativeSession>(loop,
                                                 host,
                                                 vthost::ConnectionId { .endpoint = "test", .index = 1 },
                                                 std::move(pair.first),
                                                 NativeSession::DefaultWriteQueueBytes,
                                                 std::move(services)) },
        client { std::make_unique<NativeClient>(
            loop,
            std::move(pair.second),
            std::move(handshake),
            vtbackend::CommandBlockStoreLimits {},
            NativeClient::UpdateHandler {},
            NativeClient::ImageHandler {},
            NativeClient::SessionEventHandler {},
            [this](proto::LayoutState const& layout) { layouts.push_back(layout); }) }
    {
    }

    std::unique_ptr<core::net::IoBackend> source = core::net::makeDefaultBackend();
    core::net::EventLoop loop { *source };
    SessionHost host;
    core::net::testing::SocketPair pair = *core::net::testing::makeSocketPair(loop);
    std::vector<proto::LayoutState> layouts {};
    std::unique_ptr<NativeSession> server;
    std::unique_ptr<NativeClient> client;

    /// Runs both ends with @p scenario beside them; the scenario detaches the client when it is done.
    void drive(Task<void> scenario)
    {
        loop.blockOn(core::net::testing::allOf(
            server->run(),
            client->run(),
            guardScenario(&loop, [this] { client->detach(); }, std::move(scenario))));
    }

    /// @return The profile each hosted session was spawned under, in session order.
    [[nodiscard]] std::vector<std::string> profiles() const
    {
        auto names = std::vector<std::string> {};
        for (auto const id: std::views::iota(uint64_t { 1 }, uint64_t { 10 }))
            if (auto const* session = host.hostedSession(vtworkspace::SessionId { id }))
                names.push_back(session->profileName());
        return names;
    }
};

/// @return A catalog of one profile, "main", the default.
[[nodiscard]] std::shared_ptr<vthost::ProfileCatalog const> mainOnly()
{
    return std::make_shared<vthost::testing::FakeProfileCatalog>(
        std::map<std::string, vthost::ResolvedProfile> {
            { "main", vthost::testing::fakeProfile("/bin/zsh", vtbackend::LineCount(1000)) } },
        "main");
}

/// Waits until the client holds a layout, then detaches it.
Task<void> detachOnLayout(IntentHarness* h)
{
    co_await core::net::testing::waitUntil(&h->loop, [h] { return !h->layouts.empty(); });
    h->client->detach();
}

} // namespace

TEST_CASE("the client states its intent and profile and learns its windows", "[vthost][attach][intent]")
{
    auto services = vthost::NativeSessionServices {};
    services.handshake.capabilities = thinClientCapabilities();
    auto h = IntentHarness { std::move(services),
                             NativeClient::HandshakeOptions {
                                 .capabilities = thinClientCapabilities(),
                                 .attachIntent = { .kind = proto::AttachIntentKind::Launch, .windows = {} },
                                 .profileName = "main" },
                             mainOnly() };
    h.drive(detachOnLayout(&h));

    CHECK(h.client->followedWindows() == std::vector<uint64_t> { h.host.windowId().value });
    CHECK(h.client->namesProfiles());
    CHECK(h.profiles() == std::vector<std::string> { "main" }); // the attach's first session, under it
}

TEST_CASE("against a daemon without the capabilities nothing new is written", "[vthost][attach][compat]")
{
    // Review focus 3: a newer client, an older daemon.
    SECTION("the verbs carry their older bodies, and no EditTab is written")
    {
        auto const source = core::net::makeDefaultBackend();
        auto loop = core::net::EventLoop { *source };
        auto pair = *core::net::testing::makeSocketPair(loop);
        auto* daemonSocket = pair.first.get();
        auto client = NativeClient { loop,
                                     std::move(pair.second),
                                     NativeClient::HandshakeOptions {
                                         .capabilities = thinClientCapabilities(), .profileName = "main" },
                                     vtbackend::CommandBlockStoreLimits {},
                                     NativeClient::UpdateHandler {},
                                     NativeClient::ImageHandler {},
                                     NativeClient::SessionEventHandler {},
                                     NativeClient::LayoutHandler {} };
        auto seen = std::vector<proto::DecodedFrame> {};
        auto delivery = vthost::client::Delivery::Sent;
        auto scenario = [](core::net::EventLoop* loop,
                           NativeClient* client,
                           std::vector<proto::DecodedFrame> const* seen,
                           vthost::client::Delivery* delivery) -> Task<void> {
            co_await core::net::testing::waitUntil(loop, [&] { return client->connected(); });
            *delivery = client->editTab({ .session = 1, .kind = proto::TabEditKind::SetTitle, .title = "x" });
            client->createTab(0, "x");
            client->createWindow("y");
            client->splitPane(1, 1, 5000, "z");
            co_await core::net::testing::waitUntil(
                loop, [&] { return std::ranges::any_of(*seen, carries<proto::SplitPane>); });
            client->detach();
        }(&loop, &client, &seen, &delivery);
        loop.blockOn(core::net::testing::allOf(
            client.run(),
            scriptedDaemon(
                &loop, daemonSocket, answeringHello(Script { { std::nullopt, acceptingHello() } }), &seen),
            std::move(scenario)));

        CHECK(delivery == vthost::client::Delivery::Unsupported);
        CHECK_FALSE(client.namesProfiles());
        CHECK_FALSE(std::ranges::any_of(seen, carries<proto::EditTab>));
        auto const created = std::ranges::find_if(seen, carries<proto::CreateTab>);
        REQUIRE(created != seen.end());
        CHECK(std::get<proto::CreateTab>(created->pdu).profileName.empty());
        auto const window = std::ranges::find_if(seen, carries<proto::NewWindow>);
        REQUIRE(window != seen.end());
        CHECK(std::get<proto::NewWindow>(window->pdu).profileName.empty());
        auto const split = std::ranges::find_if(seen, carries<proto::SplitPane>);
        REQUIRE(split != seen.end());
        CHECK(std::get<proto::SplitPane>(split->pdu).profileName.empty());
        // The hello still states the settings, for the older path to apply.
        auto const hello = std::ranges::find_if(seen, carries<proto::ClientHello>);
        REQUIRE(hello != seen.end());
        CHECK(std::get<proto::ClientHello>(hello->pdu).profileName == "main");
    }

    SECTION("the daemon applies the client's session settings to the session it creates")
    {
        auto services = vthost::NativeSessionServices {};
        services.handshake.capabilities = {};
        auto h = IntentHarness { std::move(services),
                                 NativeClient::HandshakeOptions { .sessionSettings = vt420Settings(),
                                                                  .capabilities = thinClientCapabilities(),
                                                                  .profileName = "main" },
                                 mainOnly() };
        h.drive(detachOnLayout(&h));
        auto const session = h.host.model().window(h.host.windowId())->tabAt(0)->rootPane()->session();
        CHECK(h.host.terminal(session)->terminalId() == vtbackend::VTType::VT420);
        CHECK(h.client->followedWindows() == std::vector<uint64_t> { h.host.windowId().value });
    }
}

TEST_CASE("mergeTabIntoSplit is sent to a daemon that shares TabMerge, and the tab joins the target",
          "[vthost][attach][tab-merge][daemon]")
{
    auto h = IntentHarness { vthost::NativeSessionServices {},
                             NativeClient::HandshakeOptions { .capabilities = proto::CapabilitySet::of(
                                                                  { proto::Capability::TabMerge }) } };
    auto const* target = h.host.createTab();
    auto const* dragged = h.host.createTab();
    REQUIRE((target != nullptr && dragged != nullptr));
    auto const targetSession = target->rootPane()->session();
    auto const draggedSession = dragged->rootPane()->session();
    auto delivery = vthost::client::Delivery::Unsupported;
    auto scenario = [](IntentHarness* h,
                       vthost::client::Delivery* delivery,
                       uint64_t draggedSession,
                       uint64_t targetSession) -> Task<void> {
        co_await core::net::testing::waitUntil(&h->loop, [h] { return !h->layouts.empty(); });
        auto const tabsBefore = h->layouts.back().tabs.size();
        *delivery = h->client->mergeTabIntoSplit(
            std::array { draggedSession }, targetSession, vtworkspace::SplitSide::Left);
        co_await core::net::testing::waitUntil(
            &h->loop, [h, tabsBefore] { return h->layouts.back().tabs.size() == tabsBefore - 1; });
        h->client->detach();
    }(&h, &delivery, draggedSession.value, targetSession.value);
    h.drive(std::move(scenario));

    CHECK(delivery == vthost::client::Delivery::Sent);
    // The mirror holds the merged tree: the tab that took the drop now has both sessions as leaves.
    auto const merged = std::ranges::find_if(h.layouts.back().tabs, [&](auto const& tab) {
        return vthost::client::paneTreeHosts(tab.root, targetSession.value)
               && vthost::client::paneTreeHosts(tab.root, draggedSession.value);
    });
    CHECK(merged != h.layouts.back().tabs.end());
    CHECK(h.host.terminal(draggedSession) != nullptr);
}

TEST_CASE("a daemon's TabMergeRefused reaches the client's handler once per refusal",
          "[vthost][attach][compat][tab-merge]")
{
    auto const source = core::net::makeDefaultBackend();
    auto loop = core::net::EventLoop { *source };
    auto pair = *core::net::testing::makeSocketPair(loop);
    auto* daemonSocket = pair.first.get();
    auto refusals = std::vector<uint8_t> {};
    auto client =
        NativeClient { loop,
                       std::move(pair.second),
                       NativeClient::HandshakeOptions {
                           .capabilities = proto::CapabilitySet::of({ proto::Capability::TabMerge }) },
                       vtbackend::CommandBlockStoreLimits {},
                       NativeClient::UpdateHandler {},
                       NativeClient::ImageHandler {},
                       NativeClient::SessionEventHandler {},
                       NativeClient::LayoutHandler {},
                       NativeClient::PermissionHandler {},
                       vthost::client::LivenessPolicy {},
                       NativeClient::TabMergeRefusedHandler { [&](proto::TabMergeRefused const& refused) {
                           refusals.push_back(refused.reason);
                       } } };
    // Refuses every merge (as a daemon that finds the tab stale does), accepts the hello.
    auto const refusing = ScriptFor { [](proto::DecodedFrame const& frame) {
        if (std::holds_alternative<proto::ClientHello>(frame.pdu))
            return Script { { std::nullopt,
                              acceptingHello(proto::CapabilitySet::of({ proto::Capability::TabMerge })) } };
        if (std::holds_alternative<proto::MergeTabIntoSplit>(frame.pdu))
            return Script { { std::nullopt, proto::TabMergeRefused { .reason = 3 } } };
        return Script {};
    } };
    auto seen = std::vector<proto::DecodedFrame> {};
    auto scenario = [](core::net::EventLoop* loop,
                       NativeClient* client,
                       std::vector<uint8_t> const* refusals) -> Task<void> {
        co_await core::net::testing::waitUntil(loop, [&] { return client->connected(); });
        std::ignore =
            client->mergeTabIntoSplit(std::array<uint64_t, 1> { 1 }, 2, vtworkspace::SplitSide::Right);
        co_await core::net::testing::waitUntil(loop, [&] { return !refusals->empty(); });
        client->detach();
    }(&loop, &client, &refusals);
    loop.blockOn(core::net::testing::allOf(
        client.run(), scriptedDaemon(&loop, daemonSocket, refusing, &seen), std::move(scenario)));

    CHECK(refusals == std::vector<uint8_t> { 3 });
}

TEST_CASE("against a daemon without TabMerge no MergeTabIntoSplit is written",
          "[vthost][attach][compat][tab-merge]")
{
    // A newer client, an older daemon: the daemon shares nothing, so the verb is not written at all and the
    // caller learns it, to tear the tab off as it did before the verb existed.
    auto const source = core::net::makeDefaultBackend();
    auto loop = core::net::EventLoop { *source };
    auto pair = *core::net::testing::makeSocketPair(loop);
    auto* daemonSocket = pair.first.get();
    auto client = NativeClient { loop,
                                 std::move(pair.second),
                                 NativeClient::HandshakeOptions { .capabilities = proto::CapabilitySet::of(
                                                                      { proto::Capability::TabMerge }) },
                                 vtbackend::CommandBlockStoreLimits {},
                                 NativeClient::UpdateHandler {},
                                 NativeClient::ImageHandler {},
                                 NativeClient::SessionEventHandler {},
                                 NativeClient::LayoutHandler {} };
    auto seen = std::vector<proto::DecodedFrame> {};
    auto delivery = vthost::client::Delivery::Sent;
    auto scenario = [](core::net::EventLoop* loop,
                       NativeClient* client,
                       std::vector<proto::DecodedFrame> const* seen,
                       vthost::client::Delivery* delivery) -> Task<void> {
        co_await core::net::testing::waitUntil(loop, [&] { return client->connected(); });
        *delivery =
            client->mergeTabIntoSplit(std::array<uint64_t, 1> { 1 }, 2, vtworkspace::SplitSide::Right);
        // A verb that is written reaches the daemon in order, so this one arriving proves the other was not
        // sent.
        client->createTab(0, "x");
        co_await core::net::testing::waitUntil(
            loop, [&] { return std::ranges::any_of(*seen, carries<proto::CreateTab>); });
        client->detach();
    }(&loop, &client, &seen, &delivery);
    loop.blockOn(core::net::testing::allOf(
        client.run(),
        scriptedDaemon(
            &loop, daemonSocket, answeringHello(Script { { std::nullopt, acceptingHello() } }), &seen),
        std::move(scenario)));

    CHECK(delivery == vthost::client::Delivery::Unsupported);
    CHECK_FALSE(std::ranges::any_of(seen, carries<proto::MergeTabIntoSplit>));
}

TEST_CASE("editTab is sent when the daemon shares TabEdits", "[vthost][attach][intent]")
{
    auto h = IntentHarness { vthost::NativeSessionServices {},
                             NativeClient::HandshakeOptions { .capabilities = thinClientCapabilities() } };
    REQUIRE(h.host.createTab() != nullptr);
    auto delivery = vthost::client::Delivery::Unsupported;
    auto scenario = [](IntentHarness* h, vthost::client::Delivery* delivery) -> Task<void> {
        co_await core::net::testing::waitUntil(&h->loop, [h] { return !h->layouts.empty(); });
        auto const session = h->layouts.back().tabs.front().root.session;
        *delivery = h->client->editTab(
            { .session = session, .kind = proto::TabEditKind::SetTitle, .title = "build" });
        co_await core::net::testing::waitUntil(
            &h->loop, [h] { return h->layouts.back().tabs.front().title == "build"; });
        h->client->detach();
    }(&h, &delivery);
    h.drive(std::move(scenario));

    CHECK(delivery == vthost::client::Delivery::Sent);
    CHECK(h.layouts.back().tabs.front().title == "build");
}

namespace
{

/// Turns @p loop until @p ready holds, waiting on its I/O up to a millisecond a turn.
/// @return Whether it held within @p turns turns.
[[nodiscard]] bool turnUntil(core::net::EventLoop& loop, std::function<bool()> const& ready, int turns = 5000)
{
    for ([[maybe_unused]] auto const turn: std::views::iota(0, turns))
    {
        if (ready())
            return true;
        std::ignore = loop.runOnce(1ms);
    }
    return ready();
}

/// Runs @p client to its end, then marks @p ended.
Task<void> runToEnd(NativeClient* client, bool* ended)
{
    co_await client->run();
    *ended = true;
}

/// Runs @p daemon to its end, then marks @p ended.
Task<void> daemonToEnd(Task<void> daemon, bool* ended)
{
    co_await std::move(daemon);
    *ended = true;
}

/// A client over @p socket offering Heartbeat, probing after 5 s and giving up after 15 s.
[[nodiscard]] std::unique_ptr<NativeClient> heartbeatClient(core::net::EventLoop& loop,
                                                            std::unique_ptr<core::net::ISocket> socket)
{
    return std::make_unique<NativeClient>(
        loop,
        std::move(socket),
        NativeClient::HandshakeOptions { .capabilities =
                                             proto::CapabilitySet::of({ proto::Capability::Heartbeat }) },
        vtbackend::CommandBlockStoreLimits {},
        NativeClient::UpdateHandler {},
        NativeClient::ImageHandler {},
        NativeClient::SessionEventHandler {},
        NativeClient::LayoutHandler {},
        NativeClient::PermissionHandler {},
        vthost::client::LivenessPolicy { .idleBeforeProbe = 5s,
                                         .deadAfter = 15s,
                                         .handshakeDeadline = vthost::client::DefaultHandshakeDeadline });
}

/// A loop on a manual clock, a client on it, and a scripted daemon answering it with @p answer — driven by
/// hand, a second of the clock at a time, since a manual clock moves only when the case moves it.
struct ManualClockAttach
{
    explicit ManualClockAttach(ScriptFor answer)
    {
        loop.spawn(runToEnd(client.get(), &clientEnded));
        loop.spawn(daemonToEnd(scriptedDaemon(&loop, daemonSocket, std::move(answer), &seen), &daemonEnded));
    }

    core::platform::ManualClock clock { core::platform::SteadyTimePoint {} + 10s };
    std::unique_ptr<core::net::IoBackend> source = core::net::makeDefaultBackend();
    core::net::EventLoop loop { *source, clock };
    core::net::testing::SocketPair pair = *core::net::testing::makeSocketPair(loop);
    core::net::ISocket* daemonSocket = pair.first.get();
    std::unique_ptr<NativeClient> client = heartbeatClient(loop, std::move(pair.second));
    std::vector<proto::DecodedFrame> seen {};
    bool clientEnded = false;
    bool daemonEnded = false;

    /// Advances the clock a second at a time, letting the loop settle between steps, until the client ended
    /// or @p seconds passed.
    void letTimePass(int seconds)
    {
        for ([[maybe_unused]] auto const second: std::views::iota(0, seconds))
        {
            if (clientEnded)
                return;
            clock.advance(1s);
            std::ignore = turnUntil(loop, [] { return false; }, 20);
        }
    }

    /// Lets both flows run out, as the case ends.
    void settle()
    {
        if (!clientEnded)
            client->detach();
        CHECK(turnUntil(loop, [this] { return clientEnded && daemonEnded; }));
    }
};

/// @return A script answering the hello with one that shares Heartbeat, and nothing else.
[[nodiscard]] ScriptFor silentAfterHello()
{
    return answeringHello(Script {
        { std::nullopt, acceptingHello(proto::CapabilitySet::of({ proto::Capability::Heartbeat })) } });
}

} // namespace

TEST_CASE("a client whose daemon stops answering ends with a loss reason", "[vthost][attach][liveness]")
{
    // A daemon that reads but, past its hello, never writes: a half-open connection as the client sees it.
    auto h = ManualClockAttach { silentAfterHello() };
    REQUIRE(turnUntil(h.loop, [&h] { return h.client->connected(); }));
    h.letTimePass(16);

    CHECK(turnUntil(h.loop, [&h] { return h.clientEnded; }));
    CHECK(h.client->lossReason() == "no answer from the daemon for 15 s");
    CHECK(std::ranges::any_of(h.seen, carries<proto::Heartbeat>)); // it was probed first
    h.settle();
}

TEST_CASE("a client whose daemon never answers the hello gives up at the handshake deadline",
          "[vthost][attach][liveness]")
{
    // A connection that went half-open mid-handshake: accepted, read, and never answered.
    auto h = ManualClockAttach { [](proto::DecodedFrame const&) { return Script {}; } };
    REQUIRE(turnUntil(h.loop, [&h] { return std::ranges::any_of(h.seen, carries<proto::ClientHello>); }));
    h.letTimePass(9);
    CHECK_FALSE(h.clientEnded);
    h.letTimePass(2);

    CHECK(turnUntil(h.loop, [&h] { return h.clientEnded; }));
    CHECK_FALSE(h.client->connected());
    CHECK(h.client->lossReason() == "the daemon did not answer the handshake within 10 s");
    h.settle();
}

TEST_CASE("a client whose daemon echoes stays connected", "[vthost][attach][liveness]")
{
    auto const echoing = [](proto::DecodedFrame const& frame) -> Script {
        if (carries<proto::ClientHello>(frame))
            return Script { { std::nullopt,
                              acceptingHello(proto::CapabilitySet::of({ proto::Capability::Heartbeat })) } };
        if (auto const* heartbeat = std::get_if<proto::Heartbeat>(&frame.pdu))
            return Script { { std::nullopt, *heartbeat } };
        return Script {};
    };
    auto h = ManualClockAttach { echoing };
    REQUIRE(turnUntil(h.loop, [&h] { return h.client->connected(); }));
    h.letTimePass(40);

    CHECK_FALSE(h.clientEnded);
    CHECK_FALSE(h.client->lossReason().has_value());
    CHECK(std::ranges::count_if(h.seen, carries<proto::Heartbeat>) >= 2);
    h.settle();
}

namespace
{
/// What a client saw of a focus change that raced an unrelated layout change on a real daemon.
struct FocusRace
{
    uint64_t activated = 0;                 ///< The pane the client activated.
    uint64_t previous = 0;                  ///< The pane that was active before.
    std::vector<proto::LayoutState> pushes; ///< Every layout the client was handed after its edit.
};

/// Splits a tab on a real daemon, then has the client activate the pane that is not active while, before the
/// daemon has read that, another change -- a tab title, as a shell sets one -- makes the daemon push a layout
/// composed before the edit.
/// @param clientCapabilities What the client offers; the daemon offers its defaults.
/// @return What the client was handed from its edit on, until the daemon's echo.
[[nodiscard]] FocusRace raceFocusAgainstTitle(proto::CapabilitySet clientCapabilities)
{
    auto h =
        IntentHarness { vthost::NativeSessionServices {},
                        NativeClient::HandshakeOptions { .capabilities = std::move(clientCapabilities) } };
    auto* tab = h.host.createTab();
    REQUIRE(tab != nullptr);
    auto const tabId = tab->id();
    h.host.splitActivePane(tabId, vtworkspace::SplitState::Vertical);
    auto race = FocusRace {};
    auto scenario = [](IntentHarness* h, vtworkspace::TabId tabId, FocusRace* race) -> Task<void> {
        auto const split = co_await core::net::testing::waitUntil(
            &h->loop, [h] { return !h->layouts.empty() && h->layouts.back().tabs.front().root.isSplit(); });
        REQUIRE(split);
        auto const& shown = h->layouts.back().tabs.front();
        race->previous = shown.activePane;
        auto const other = shown.root.children[0].paneId == shown.activePane ? shown.root.children[1]
                                                                             : shown.root.children[0];
        race->activated = other.paneId;
        auto const seen = h->layouts.size();
        auto const delivery =
            h->client->editTab({ .session = other.session, .kind = proto::TabEditKind::ActivatePane });
        CHECK(delivery == vthost::client::Delivery::Sent);
        // Before the daemon reads the edit: it pushes a layout with the old pane active.
        h->host.model().setTabTitle(tabId, "racing");
        co_await core::net::testing::waitUntil(&h->loop, [h, race, seen] {
            return h->layouts.size() > seen + 1 && h->layouts.back().tabs.front().title == "racing"
                   && h->layouts.back().tabs.front().activePane == race->activated;
        });
        race->pushes.assign(h->layouts.begin() + static_cast<std::ptrdiff_t>(seen), h->layouts.end());
        h->client->detach();
    }(&h, tabId, &race);
    h.drive(std::move(scenario));
    return race;
}
} // namespace

TEST_CASE("a pane the client activates stays active over a push the daemon composed before it",
          "[vthost][attach][tab-edit-acks][daemon]")
{
    auto capabilities = thinClientCapabilities();
    capabilities.idents.push_back(std::to_underlying(proto::Capability::TabEditAcks));
    auto const race = raceFocusAgainstTitle(std::move(capabilities));

    REQUIRE(race.activated != race.previous);
    REQUIRE(race.pushes.size() >= 2);
    for (auto const& push: race.pushes)
    {
        CHECK(push.tabs.front().activePane == race.activated);
        CHECK(push.servedEdit.has_value());
    }
}

TEST_CASE("a client without TabEditAcks gets the daemon's pushes as they were",
          "[vthost][attach][tab-edit-acks][compat][daemon]")
{
    // An older client, a newer daemon: no push carries the field, and the stale one undoes the edit until the
    // echo, as it always did.
    auto const race = raceFocusAgainstTitle(thinClientCapabilities());

    REQUIRE(race.pushes.size() >= 2);
    CHECK(race.pushes.front().tabs.front().activePane == race.previous);
    CHECK(race.pushes.back().tabs.front().activePane == race.activated);
    for (auto const& push: race.pushes)
        CHECK_FALSE(push.servedEdit.has_value());
}

TEST_CASE("against a daemon without TabEditAcks a push is shown as it came",
          "[vthost][attach][tab-edit-acks][compat]")
{
    // A newer client, an older daemon: it shares TabEdits but stamps nothing, so nothing is kept in force --
    // nor left waiting for an acknowledgement that never comes.
    auto const source = core::net::makeDefaultBackend();
    auto loop = core::net::EventLoop { *source };
    auto pair = *core::net::testing::makeSocketPair(loop);
    auto* daemonSocket = pair.first.get();
    auto layouts = std::vector<proto::LayoutState> {};
    auto capabilities = thinClientCapabilities();
    capabilities.idents.push_back(std::to_underlying(proto::Capability::TabEditAcks));
    auto client =
        NativeClient { loop,
                       std::move(pair.second),
                       NativeClient::HandshakeOptions { .capabilities = std::move(capabilities) },
                       vtbackend::CommandBlockStoreLimits {},
                       NativeClient::UpdateHandler {},
                       NativeClient::ImageHandler {},
                       NativeClient::SessionEventHandler {},
                       [&layouts](proto::LayoutState const& layout) { layouts.push_back(layout); } };
    auto split = proto::WirePane { .paneId = 10, .split = 2 };
    split.children = { proto::WirePane { .paneId = 11, .session = 1 },
                       proto::WirePane { .paneId = 12, .session = 2 } };
    auto const layout =
        proto::LayoutState { .window = 1,
                             .activeTab = 0,
                             .tabs = { proto::WireTab { .tabId = 1, .activePane = 11, .root = split } } };
    auto const answer = [layout](proto::DecodedFrame const& frame) -> Script {
        if (std::holds_alternative<proto::ClientHello>(frame.pdu))
            return { { std::nullopt,
                       acceptingHello(proto::CapabilitySet::of({ proto::Capability::TabEdits })) },
                     { 0, layout } };
        // The edit arrives after a push the daemon had already composed: the old pane still active.
        if (std::holds_alternative<proto::EditTab>(frame.pdu))
            return { { 0, layout } };
        return {};
    };
    auto seen = std::vector<proto::DecodedFrame> {};
    auto scenario = [](core::net::EventLoop* loop,
                       NativeClient* client,
                       std::vector<proto::LayoutState> const* layouts) -> Task<void> {
        auto const attached = co_await core::net::testing::waitUntil(loop, [&] { return !layouts->empty(); });
        REQUIRE(attached);
        std::ignore = client->editTab({ .session = 2, .kind = proto::TabEditKind::ActivatePane });
        co_await core::net::testing::waitUntil(loop, [&] { return layouts->size() == 2; });
        client->detach();
    }(&loop, &client, &layouts);
    loop.blockOn(
        core::net::testing::allOf(client.run(),
                                  scriptedDaemon(&loop, daemonSocket, answer, &seen),
                                  guardScenario(&loop, [&client] { client.detach(); }, std::move(scenario))));

    REQUIRE(layouts.size() == 2);
    CHECK(layouts.back() == layout);
    CHECK(std::ranges::any_of(seen, carries<proto::EditTab>));
}
