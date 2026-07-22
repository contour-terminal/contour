// SPDX-License-Identifier: Apache-2.0
#include <vtbackend/primitives.h>

#include <vtpty/MockPty.h>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <chrono>
#include <cstddef>
#include <memory>
#include <string>
#include <utility>
#include <variant>
#include <vector>

#include <coro/WhenAll.hpp>
#include <muxserver/NativeSession.h>
#include <muxserver/SessionHost.h>
#include <muxserver/TappingPty.h>
#include <net/EventLoop.h>
#include <net/PollEventSource.h>
#include <net/testing/CoroTestSupport.h>
#include <net/testing/InMemoryTransport.h>
#include <vtmux/Pane.h>
#include <vtmux/Tab.h>

using namespace std::chrono_literals;

using coro::Task;
using muxserver::NativeSession;
using muxserver::SessionHost;
namespace proto = muxserver::proto;

namespace
{

/// Writes the pre-encoded request bytes onto the wire.
Task<void> feedBytes(net::ISocket* client, std::vector<std::byte> const* bytes)
{
    std::ignore = co_await client->write(std::span<std::byte const> { bytes->data(), bytes->size() });
}

/// Decodes server PDUs until @p expected arrived (or the stream ended), then
/// closes the client end — the session's read loop sees EOF and finishes.
Task<void> collectPdus(net::ISocket* client, std::size_t expected, std::vector<proto::DecodedFrame>* out)
{
    auto buffer = std::vector<std::byte> {};
    auto scratch = std::array<std::byte, 4096> {};
    while (out->size() < expected)
    {
        auto decoded = proto::decodePdu(buffer);
        if (decoded)
        {
            buffer.erase(buffer.begin(), buffer.begin() + static_cast<long>(decoded->consumed));
            out->push_back(std::move(*decoded));
            continue;
        }
        if (decoded.error() != proto::DecodeError::NeedMoreData)
            break;
        auto const n = co_await client->read(scratch);
        if (!n.has_value() || *n == 0)
            break;
        buffer.insert(buffer.end(), scratch.begin(), scratch.begin() + static_cast<long>(*n));
    }
    client->close();
}

Task<void> driveExchange(NativeSession* session,
                         net::ISocket* client,
                         std::vector<std::byte> const* request,
                         std::size_t expected,
                         std::vector<proto::DecodedFrame>* out)
{
    co_await coro::whenAll(session->run(), feedBytes(client, request), collectPdus(client, expected, out));
}

/// Encodes @p pdus into one contiguous request, serials counting from 1.
std::vector<std::byte> encodeRequest(std::vector<proto::DecodedPdu> const& pdus)
{
    auto request = proto::Writer {};
    auto serial = uint64_t { 1 };
    for (auto const& pdu: pdus)
        proto::encodePdu(request, serial++, pdu);
    return { request.view().begin(), request.view().end() };
}

/// Drives a NativeSession over an in-memory socket pair.
struct NativeHarness
{
    explicit NativeHarness(std::size_t maxWriteQueueBytes = NativeSession::DefaultWriteQueueBytes):
        writeQueueBytes { maxWriteQueueBytes }
    {
    }

    net::PollEventSource source;
    net::EventLoop loop { source };
    SessionHost host { loop,
                       [](vtbackend::PageSize size) { return std::make_unique<vtpty::MockPty>(size); },
                       vtbackend::Settings {},
                       /*startPumps=*/false };
    net::testing::SocketPair pair = *net::testing::makeSocketPair(loop);
    std::size_t writeQueueBytes; // set by the constructor, before `session` reads it
    std::unique_ptr<NativeSession> session =
        std::make_unique<NativeSession>(loop, host, std::move(pair.first), writeQueueBytes);

    /// Encodes @p pdus, runs the exchange, returns the first @p expected server PDUs.
    std::vector<proto::DecodedFrame> exchange(std::vector<proto::DecodedPdu> const& pdus,
                                              std::size_t expected)
    {
        auto const bytes = encodeRequest(pdus);
        auto received = std::vector<proto::DecodedFrame> {};
        loop.blockOn(driveExchange(session.get(), pair.second.get(), &bytes, expected, &received));
        return received;
    }
};

/// Once the handshake had time to land: flips the hosted terminal to the
/// alternate screen and kicks the debounced flush.
Task<void> flipToAltScreen(NativeHarness* h, vtmux::SessionId id)
{
    co_await h->loop.delay(5ms);
    h->host.terminal(id)->writeToScreen("\033[?1049hALT!");
    h->session->sessionScreenUpdated(id);
}

/// Once the attach snapshot has landed: appends to the SAME primary screen and
/// kicks the debounced flush, so a NON-snapshot (incremental) delta follows.
Task<void> appendThenUpdate(NativeHarness* h, vtmux::SessionId id)
{
    co_await h->loop.delay(5ms);
    h->host.terminal(id)->writeToScreen("more");
    h->session->sessionScreenUpdated(id);
}

/// Once the attach snapshot has landed: repositions ONLY the cursor (writing no
/// cell content) and kicks the debounced flush.
Task<void> moveCursorThenUpdate(NativeHarness* h, vtmux::SessionId id)
{
    co_await h->loop.delay(5ms);
    h->host.terminal(id)->writeToScreen("\033[10;5H"); // CUP: move cursor to row 10, col 5
    h->session->sessionScreenUpdated(id);
}

/// Schedules a debounce flush, then disconnects before it can fire.
Task<void> kickThenDisconnect(NativeHarness* h, vtmux::SessionId id)
{
    co_await h->loop.delay(5ms);
    h->session->sessionScreenUpdated(id); // parks the 20ms debounce flush
    h->pair.second->close();              // client gone: run() must settle the flush
}

Task<void> runThenMark(NativeSession* session, bool* done)
{
    co_await session->run();
    *done = true;
}

/// Bounds the overflow test on regression: if the session never disconnects the
/// client, close it from outside after ~1s so the test fails instead of hanging.
Task<void> closeWatchdog(NativeHarness* h, bool const* done, bool* fired)
{
    if (!co_await net::testing::waitUntil(&h->loop, [done] { return *done; }))
    {
        *fired = true;
        h->pair.second->close();
    }
}

} // namespace

namespace muxserver
{
/// Exposes NativeSession's private follow map to the leak regression test.
struct NativeSessionFollowTester
{
    static bool follows(NativeSession const& session, vtmux::SessionId id)
    {
        return session._followed.contains(id.value);
    }
    static std::size_t followedCount(NativeSession const& session) { return session._followed.size(); }
};
} // namespace muxserver

TEST_CASE("the native handshake answers ServerHello and a full snapshot", "[muxserver][native]")
{
    auto h = NativeHarness {};
    h.host.createTab();
    auto const sessionId = h.host.model().window(h.host.windowId())->activeTab()->rootPane()->session();
    h.host.terminal(sessionId)->writeToScreen("hello native");

    // Expect: ServerHello, LayoutState, SessionState, Delta (snapshot).
    auto const received = h.exchange({ proto::ClientHello {} }, 4);
    REQUIRE(received.size() == 4);

    auto const* hello = std::get_if<proto::ServerHello>(&received[0].pdu);
    REQUIRE(hello != nullptr);
    CHECK(hello->codecVersion == proto::CodecVersion);

    // The layout leads the snapshot so the client builds its tabs before content.
    auto const* layout = std::get_if<proto::LayoutState>(&received[1].pdu);
    REQUIRE(layout != nullptr);
    REQUIRE(layout->tabs.size() == 1);
    CHECK(layout->tabs.front().root.session == sessionId.value);

    auto const* state = std::get_if<proto::SessionState>(&received[2].pdu);
    REQUIRE(state != nullptr);
    CHECK(state->session == sessionId.value);
    CHECK(state->columns == 80);
    CHECK(state->lines == 25);

    auto const* delta = std::get_if<proto::Delta>(&received[3].pdu);
    REQUIRE(delta != nullptr);
    CHECK(delta->snapshot == 1);
    REQUIRE(delta->lines.size() == 25); // the whole page ships on attach

    // The written text arrived cell by cell on the first row.
    auto text = std::string {};
    for (auto const& cell: delta->lines.front().cells)
        if (cell.codepoint != 0)
            text += static_cast<char>(cell.codepoint);
    CHECK(text == "hello native");
}

TEST_CASE("Input PDUs land in the target pane's PTY", "[muxserver][native]")
{
    auto h = NativeHarness {};
    h.host.createTab();
    auto const sessionId = h.host.model().window(h.host.windowId())->activeTab()->rootPane()->session();

    auto input = proto::Input { .session = sessionId.value, .data = {} };
    for (auto const ch: std::string_view { "ls\r" })
        input.data.push_back(static_cast<std::byte>(ch));

    std::ignore = h.exchange({ proto::ClientHello {}, proto::DecodedPdu { input } }, 3);

    auto& tapped = dynamic_cast<muxserver::TappingPty&>(h.host.terminal(sessionId)->device());
    auto& mock = dynamic_cast<vtpty::MockPty&>(tapped.inner());
    CHECK(mock.stdinBuffer() == "ls\r");
}

TEST_CASE("FetchImage for an unknown id answers ImageGone", "[muxserver][native]")
{
    auto h = NativeHarness {};
    h.host.createTab();

    // ServerHello, LayoutState, SessionState, Delta (snapshot), then ImageGone.
    auto const received =
        h.exchange({ proto::ClientHello {}, proto::DecodedPdu { proto::FetchImage { .imageId = 4242 } } }, 5);
    REQUIRE(received.size() == 5);
    auto const* gone = std::get_if<proto::ImageGone>(&received[4].pdu);
    REQUIRE(gone != nullptr);
    CHECK(gone->imageId == 4242);
    CHECK(received[4].serial == 2); // correlated to the request's serial
}

TEST_CASE("a version-mismatched hello is answered and the session ends", "[muxserver][native]")
{
    auto h = NativeHarness {};
    h.host.createTab();

    auto const received = h.exchange({ proto::ClientHello { .codecVersion = 9999 } }, 2);
    // Only the ServerHello arrives — no snapshot follows a failed handshake.
    REQUIRE(received.size() == 1);
    CHECK(std::holds_alternative<proto::ServerHello>(received[0].pdu));
}

TEST_CASE("an alternate-screen flip forces a resync snapshot with SessionState", "[muxserver][native]")
{
    auto h = NativeHarness {};
    h.host.createTab();
    auto const sessionId = h.host.model().window(h.host.windowId())->activeTab()->rootPane()->session();

    auto const bytes = encodeRequest({ proto::DecodedPdu { proto::ClientHello {} } });
    auto received = std::vector<proto::DecodedFrame> {};
    h.loop.blockOn(net::testing::allOf(h.session->run(),
                                       feedBytes(h.pair.second.get(), &bytes),
                                       collectPdus(h.pair.second.get(), 6, &received),
                                       flipToAltScreen(&h, sessionId)));
    REQUIRE(received.size() == 6);

    // [0] ServerHello, [1] LayoutState, [2] SessionState(primary), [3] Delta,
    // [4] SessionState(alternate), [5] Delta.
    auto const* primary = std::get_if<proto::SessionState>(&received[2].pdu);
    REQUIRE(primary != nullptr);
    CHECK(primary->screenType == std::to_underlying(vtbackend::ScreenType::Primary));

    // The flip is announced (SessionState is the only carrier of the screen
    // type) and served as a snapshot of the alternate grid — not diffed against
    // the primary grid's unrelated delta cursor.
    auto const* alt = std::get_if<proto::SessionState>(&received[4].pdu);
    REQUIRE(alt != nullptr);
    CHECK(alt->screenType == std::to_underlying(vtbackend::ScreenType::Alternate));

    auto const* delta = std::get_if<proto::Delta>(&received[5].pdu);
    REQUIRE(delta != nullptr);
    CHECK(delta->snapshot == 1);
    auto text = std::string {};
    for (auto const& cell: delta->lines.front().cells)
        if (cell.codepoint != 0)
            text += static_cast<char>(cell.codepoint);
    CHECK(text == "ALT!");
}

TEST_CASE("a snapshot anchors the cursor so the following delta is incremental", "[muxserver][native]")
{
    auto h = NativeHarness {};
    h.host.createTab();
    auto const sessionId = h.host.model().window(h.host.windowId())->activeTab()->rootPane()->session();
    h.host.terminal(sessionId)->writeToScreen("first");

    auto const bytes = encodeRequest({ proto::DecodedPdu { proto::ClientHello {} } });
    auto received = std::vector<proto::DecodedFrame> {};
    h.loop.blockOn(net::testing::allOf(h.session->run(),
                                       feedBytes(h.pair.second.get(), &bytes),
                                       collectPdus(h.pair.second.get(), 5, &received),
                                       appendThenUpdate(&h, sessionId)));
    REQUIRE(received.size() == 5);

    // [0] ServerHello, [1] LayoutState, [2] SessionState, [3] Delta(snapshot),
    // [4] Delta(incremental).
    auto const* snapshot = std::get_if<proto::Delta>(&received[3].pdu);
    REQUIRE(snapshot != nullptr);
    CHECK(snapshot->snapshot == 1);

    auto const* incremental = std::get_if<proto::Delta>(&received[4].pdu);
    REQUIRE(incremental != nullptr);
    // Anchored past the snapshot: the follow-up is a real diff, not another resync
    // (a stale cursor would force snapshot==1) and not a rescan of every row.
    CHECK(incremental->snapshot == 0);
    REQUIRE(incremental->lines.size() == 1);

    auto text = std::string {};
    for (auto const& cell: incremental->lines.front().cells)
        if (cell.codepoint != 0)
            text += static_cast<char>(cell.codepoint);
    CHECK(text.starts_with("first")); // the snapshot content is still there
    CHECK(text.contains("more"));     // with the newly appended bytes
}

TEST_CASE("a cursor-only move still produces a delta so the mirror's cursor tracks", "[muxserver][native]")
{
    auto h = NativeHarness {};
    h.host.createTab();
    auto const sessionId = h.host.model().window(h.host.windowId())->activeTab()->rootPane()->session();
    h.host.terminal(sessionId)->writeToScreen("first"); // leaves the cursor after the text

    auto const bytes = encodeRequest({ proto::DecodedPdu { proto::ClientHello {} } });
    auto received = std::vector<proto::DecodedFrame> {};
    h.loop.blockOn(net::testing::allOf(h.session->run(),
                                       feedBytes(h.pair.second.get(), &bytes),
                                       collectPdus(h.pair.second.get(), 5, &received),
                                       moveCursorThenUpdate(&h, sessionId)));
    REQUIRE(received.size() == 5);

    // [0] ServerHello, [1] LayoutState, [2] SessionState, [3] Delta(snapshot), [4] cursor-only move.
    // Without the cursor gate the [4] send is suppressed (no changed cell, no mode flip) and the
    // mirror's cursor would stay where the snapshot left it.
    auto const* delta = std::get_if<proto::Delta>(&received[4].pdu);
    REQUIRE(delta != nullptr);
    CHECK(delta->snapshot == 0);
    CHECK(delta->cursorLine == 9);   // CUP row 10, 0-based
    CHECK(delta->cursorColumn == 4); // CUP column 5, 0-based
}

TEST_CASE("a hyperlink URI is delivered on first reference", "[muxserver][native]")
{
    auto h = NativeHarness {};
    h.host.createTab();
    auto const sessionId = h.host.model().window(h.host.windowId())->activeTab()->rootPane()->session();
    // OSC 8 hyperlink around "link"; the URI must ride the snapshot's side table.
    h.host.terminal(sessionId)->writeToScreen("\033]8;;https://contour.example\033\\link\033]8;;\033\\");

    auto const received = h.exchange({ proto::ClientHello {} }, 4);
    REQUIRE(received.size() == 4);
    auto const* delta = std::get_if<proto::Delta>(&received[3].pdu);
    REQUIRE(delta != nullptr);
    REQUIRE(delta->hyperlinks.size() == 1);
    CHECK(delta->hyperlinks.front().uri == "https://contour.example");
}

TEST_CASE("a debounce flush pending at disconnect resolves before run() returns", "[muxserver][native]")
{
    auto h = NativeHarness {};
    h.host.createTab();
    auto const sessionId = h.host.model().window(h.host.windowId())->activeTab()->rootPane()->session();

    auto const bytes = encodeRequest({ proto::DecodedPdu { proto::ClientHello {} } });
    auto received = std::vector<proto::DecodedFrame> {};
    h.loop.blockOn(net::testing::allOf(h.session->run(),
                                       feedBytes(h.pair.second.get(), &bytes),
                                       collectPdus(h.pair.second.get(), 4, &received),
                                       kickThenDisconnect(&h, sessionId)));

    // The daemon frees the session the moment run() returns; a flush coroutine
    // still parked in its debounce delay would then resume on freed memory
    // (ASan turns that into a hard failure right here).
    h.session.reset();
    h.loop.blockOn(net::testing::sleepFor(&h.loop, 30ms));
    SUCCEED("the debounce flush settled before the session was destroyed");
}

TEST_CASE("a closed session's follow state is pruned", "[muxserver][native]")
{
    auto h = NativeHarness {};
    h.host.createTab();
    auto const sessionId = h.host.model().window(h.host.windowId())->activeTab()->rootPane()->session();

    // The handshake snapshot makes the session followed (pushDelta seeds _followed).
    std::ignore = h.exchange({ proto::ClientHello {} }, 3);
    REQUIRE(muxserver::NativeSessionFollowTester::follows(*h.session, sessionId));

    // The host learning its own session closed must fan out to stream subscribers,
    // and the session must drop the per-session follow state (otherwise it leaks
    // for the connection's whole lifetime -- one entry per session ever opened).
    h.host.subscribeStream(h.session.get());
    h.host.handleSessionExit(sessionId);
    h.host.unsubscribeStream(h.session.get());

    CHECK_FALSE(muxserver::NativeSessionFollowTester::follows(*h.session, sessionId));
    CHECK(muxserver::NativeSessionFollowTester::followedCount(*h.session) == 0);
}

TEST_CASE("a client that overflows the write queue is disconnected", "[muxserver][native]")
{
    auto done = false;
    auto watchdogFired = false;
    // No reply fits an 8-byte bound: the very first send overflows, and the
    // session must apply the queue's disconnect contract instead of silently
    // under-serving the client from then on.
    auto h = NativeHarness { 8 };
    h.host.createTab();

    auto const bytes = encodeRequest({ proto::DecodedPdu { proto::ClientHello {} } });
    auto received = std::vector<proto::DecodedFrame> {};
    h.loop.blockOn(net::testing::allOf(runThenMark(h.session.get(), &done),
                                       feedBytes(h.pair.second.get(), &bytes),
                                       collectPdus(h.pair.second.get(), 1, &received),
                                       closeWatchdog(&h, &done, &watchdogFired)));

    CHECK(!watchdogFired); // the SESSION closed the connection, not the watchdog
    CHECK(received.empty());
}
