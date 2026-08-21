// SPDX-License-Identifier: Apache-2.0
#include <vtbackend/core/Primitives.hpp>

#include <vtpty/MockPty.hpp>

#include <crispy/LogSink.hpp>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <chrono>
#include <cstddef>
#include <format>
#include <functional>
#include <map>
#include <memory>
#include <ranges>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

#include <coro/WhenAll.hpp>
#include <net/EventLoop.hpp>
#include <net/PollEventSource.hpp>
#include <net/testing/CoroTestSupport.hpp>
#include <net/testing/InMemoryTransport.hpp>
#include <vthost/NativeSession.hpp>
#include <vthost/SessionHost.hpp>
#include <vthost/TappingPty.hpp>
#include <vtworkspace/Pane.hpp>
#include <vtworkspace/Tab.hpp>

using namespace std::chrono_literals;

using coro::Task;
using vthost::NativeSession;
using vthost::SessionHost;
namespace proto = vthost::proto;

namespace
{

/// Writes the pre-encoded request bytes onto the wire.
Task<void> feedBytes(net::ISocket* client, std::vector<std::byte> const* bytes)
{
    std::ignore = co_await client->write(std::span<std::byte const> { bytes->data(), bytes->size() });
}

/// Decodes server PDUs until @p done says to stop (or the stream ends), then closes the client
/// end — the session's read loop sees EOF and finishes.
///
/// The bound is a predicate rather than a count because the two things tests wait for are shaped
/// differently: a fixed number of PDUs, or a marker one of them carries. How many pieces a
/// snapshot run takes depends on how its rows happen to partition, which is exactly what a test
/// of the partitioning must not hardcode.
/// @param done Consulted after each decoded frame; the collector stops when it returns true.
Task<void> collectUntil(net::ISocket* client,
                        std::function<bool(std::vector<proto::DecodedFrame> const&)> done,
                        std::vector<proto::DecodedFrame>* out)
{
    auto buffer = std::vector<std::byte> {};
    auto scratch = std::array<std::byte, 4096> {};
    while (!done(*out))
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

/// Collects exactly @p expected PDUs.
Task<void> collectPdus(net::ISocket* client, std::size_t expected, std::vector<proto::DecodedFrame>* out)
{
    co_await collectUntil(client, [expected](auto const& got) { return got.size() >= expected; }, out);
}

/// @return Whether @p frame terminates a snapshot run.
bool endsSnapshotRun(proto::DecodedFrame const& frame)
{
    auto const* delta = std::get_if<proto::Delta>(&frame.pdu);
    return delta != nullptr && delta->snapshot != 0 && delta->completesSnapshot();
}

/// Collects a whole snapshot run, plus @p extra PDUs after it.
Task<void> collectSnapshotRun(net::ISocket* client,
                              std::vector<proto::DecodedFrame>* out,
                              std::size_t extra = 0)
{
    co_await collectUntil(
        client,
        [extra](std::vector<proto::DecodedFrame> const& got) {
            auto const end = std::ranges::find_if(got, endsSnapshotRun);
            return end != got.end() && std::cmp_greater(std::distance(end, got.end()), extra);
        },
        out);
}

/// driveExchange, but bounded by a snapshot run's completion instead of a PDU count.
Task<void> driveSnapshotRun(NativeSession* session,
                            net::ISocket* client,
                            std::vector<std::byte> const* request,
                            std::vector<proto::DecodedFrame>* out,
                            std::size_t extra = 0)
{
    co_await coro::whenAll(
        session->run(), feedBytes(client, request), collectSnapshotRun(client, out, extra));
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

/// The printable text of @p line, skipping the empty cells a trimmed row leaves behind.
/// @param line A row as it arrived on the wire.
/// @return Its codepoints as ASCII, in column order.
std::string textOf(proto::WireLine const& line)
{
    auto text = std::string {};
    for (auto const& cell: line.cells)
        if (cell.codepoint != 0)
            text += static_cast<char>(cell.codepoint);
    return text;
}

/// @param history The hosted terminals' scrollback depth.
/// @return Host settings with @p history worth of scrollback.
vtbackend::Settings hostSettings(vtbackend::LineCount history)
{
    auto settings = vtbackend::Settings {};
    settings.historyLimits = vtbackend::HistoryLimits::plain(history);
    return settings;
}

/// What a NativeHarness is built with. One struct rather than positional defaults, so a
/// test needing only the second knob does not have to name the first.
struct HarnessOptions
{
    std::size_t writeQueueBytes = NativeSession::DefaultWriteQueueBytes;
    vtbackend::LineCount history { 0 }; ///< The hosted terminals' scrollback depth.
};

/// Drives a NativeSession over an in-memory socket pair.
struct NativeHarness
{
    explicit NativeHarness(HarnessOptions opts = {}): options { opts } {}

    net::PollEventSource source;
    net::EventLoop loop { source };
    HarnessOptions options {}; // set by the constructor, before `host` and `session` read it
    SessionHost host { loop,
                       [](vtbackend::PageSize size) { return std::make_unique<vtpty::MockPty>(size); },
                       hostSettings(options.history),
                       crispy::defaultEnvironment(),
                       /*startPumps=*/false };
    net::testing::SocketPair pair = *net::testing::makeSocketPair(loop);
    std::unique_ptr<NativeSession> session =
        std::make_unique<NativeSession>(loop,
                                        host,
                                        vthost::ConnectionId { .endpoint = "test", .index = 1 },
                                        std::move(pair.first),
                                        options.writeQueueBytes);

    /// What `serveNativeClient` installs in production, and what a resize resync now rides: the
    /// host announces a moved grid to its stream subscribers rather than the receiving connection
    /// resyncing itself. Screen updates still come from the helpers above, because the harness
    /// runs with the pumps stopped.
    vthost::ScopedStreamSubscription subscription = makeScopedStreamSubscription(host, *session);

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
Task<void> flipToAltScreen(NativeHarness* h, vtworkspace::SessionId id)
{
    co_await h->loop.delay(5ms);
    h->host.terminal(id)->writeToScreen("\033[?1049hALT!");
    h->session->sessionScreenUpdated(id);
}

/// Once the attach snapshot has landed: appends to the SAME primary screen and
/// kicks the debounced flush, so a NON-snapshot (incremental) delta follows.
Task<void> appendThenUpdate(NativeHarness* h, vtworkspace::SessionId id)
{
    co_await h->loop.delay(5ms);
    h->host.terminal(id)->writeToScreen("more");
    h->session->sessionScreenUpdated(id);
}

/// Writes @p rows lines of wide, tagged text into @p session, so its snapshot is a real multiple
/// of a send-queue bound rather than a handful of trimmed lines.
/// @param tag Prefixes each row, so a test can tell one pane's rows from another's.
void fillRows(NativeHarness& h, vtworkspace::SessionId session, int rows, std::string_view tag)
{
    auto lines = std::string {};
    for (auto const row: std::views::iota(0, rows))
        lines += std::format("{}-{}{}\r\n", tag, row, std::string(60, 'x'));
    h.host.terminal(session)->writeToScreen(lines);
}

/// Once the attach snapshot has landed: repositions ONLY the cursor (writing no
/// cell content) and kicks the debounced flush.
Task<void> moveCursorThenUpdate(NativeHarness* h, vtworkspace::SessionId id)
{
    co_await h->loop.delay(5ms);
    h->host.terminal(id)->writeToScreen("\033[10;5H"); // CUP: move cursor to row 10, col 5
    h->session->sessionScreenUpdated(id);
}

/// Once the attach snapshot has landed: sets the window title (OSC 2) and kicks the
/// debounced flush, so an incremental delta carrying the new title follows.
Task<void> setTitleThenUpdate(NativeHarness* h, vtworkspace::SessionId id)
{
    co_await h->loop.delay(5ms);
    h->host.terminal(id)->writeToScreen("\033]2;my-title\033\\"); // OSC 2: set window title
    h->session->sessionScreenUpdated(id);
}

/// Once the attach snapshot has landed: evicts the scrollback (ED 3) and kicks the debounced
/// flush. Deliberately the one operation that changes NO row and moves NO cursor — Grid::clearHistory
/// bumps no generation and dirties no line, so the floor is the only thing that moves.
Task<void> clearHistoryThenUpdate(NativeHarness* h, vtworkspace::SessionId id)
{
    co_await h->loop.delay(5ms);
    h->host.terminal(id)->writeToScreen("\033[3J"); // ED 3: erase the saved lines
    h->session->sessionScreenUpdated(id);
}

/// Schedules a debounce flush, then disconnects before it can fire.
Task<void> kickThenDisconnect(NativeHarness* h, vtworkspace::SessionId id)
{
    co_await h->loop.delay(5ms);
    h->session->sessionScreenUpdated(id); // parks the 20ms debounce flush
    h->pair.second->close();              // client gone: run() must settle the flush
}

/// Feeds @p bytes, lets the session answer (and its 20ms debounce fire), then closes the
/// client end.
///
/// The close is what lets a test count what the session sent: `collectPdus` otherwise stops
/// at a guessed number, which can only assert that at least that many arrived — useless when
/// the claim is that NOTHING more follows. Stopping at EOF makes the count exact.
Task<void> feedThenDrain(NativeHarness* h, std::vector<std::byte> const* bytes)
{
    std::ignore = co_await h->pair.second->write(std::span<std::byte const> { bytes->data(), bytes->size() });
    co_await h->loop.delay(60ms);
    h->pair.second->close();
}

/// Runs one request to completion and returns EVERY PDU the session sent in answer.
std::vector<proto::DecodedFrame> exchangeAll(NativeHarness* h, std::vector<proto::DecodedPdu> const& pdus)
{
    auto const bytes = encodeRequest(pdus);
    auto received = std::vector<proto::DecodedFrame> {};
    // A ceiling far above any expected answer: the drain below ends collection, not this.
    h->loop.blockOn(net::testing::allOf(
        h->session->run(), feedThenDrain(h, &bytes), collectPdus(h->pair.second.get(), 1000, &received)));
    return received;
}

/// One host, TWO connections — the shape every multi-client property needs, and which the
/// single-connection harness above cannot express: a claim about what the OTHER client received
/// has no other client to receive it.
///
/// Both sessions are subscribed to the host's stream events here, because `serveNativeClient` is
/// what does that in production and nothing in NativeSession does it for itself.
struct TwoClientHarness
{
    net::PollEventSource source;
    net::EventLoop loop { source };
    SessionHost host { loop,
                       [](vtbackend::PageSize size) { return std::make_unique<vtpty::MockPty>(size); },
                       hostSettings(vtbackend::LineCount(0)),
                       crispy::defaultEnvironment(),
                       /*startPumps=*/false };
    net::testing::SocketPair firstPair = *net::testing::makeSocketPair(loop);
    net::testing::SocketPair secondPair = *net::testing::makeSocketPair(loop);
    std::unique_ptr<NativeSession> serverOne = std::make_unique<NativeSession>(
        loop, host, vthost::ConnectionId { .endpoint = "test", .index = 1 }, std::move(firstPair.first));
    std::unique_ptr<NativeSession> serverTwo = std::make_unique<NativeSession>(
        loop, host, vthost::ConnectionId { .endpoint = "test", .index = 2 }, std::move(secondPair.first));
    vthost::ScopedStreamSubscription subscriptionOne = makeScopedStreamSubscription(host, *serverOne);
    vthost::ScopedStreamSubscription subscriptionTwo = makeScopedStreamSubscription(host, *serverTwo);
};

/// Feeds @p bytes to @p client after @p delay, so a request can be ordered after another client's.
Task<void> feedAfter(net::EventLoop* loop,
                     net::ISocket* client,
                     std::vector<std::byte> const* bytes,
                     std::chrono::milliseconds delay)
{
    co_await loop->delay(delay);
    std::ignore = co_await client->write(std::span<std::byte const> { bytes->data(), bytes->size() });
}

/// Closes @p client after @p delay, ending its collector at EOF so a count is exact.
Task<void> closeAfter(net::EventLoop* loop, net::ISocket* client, std::chrono::milliseconds delay)
{
    co_await loop->delay(delay);
    client->close();
}

/// @return The last SessionState in @p frames, or nullptr if there is none.
[[nodiscard]] proto::SessionState const* lastSessionState(std::vector<proto::DecodedFrame> const& frames)
{
    proto::SessionState const* found = nullptr;
    for (auto const& frame: frames)
        if (auto const* state = std::get_if<proto::SessionState>(&frame.pdu))
            found = state;
    return found;
}

/// @return How many SessionState PDUs @p frames holds — one per snapshot, so this counts resyncs.
[[nodiscard]] std::size_t sessionStateCount(std::vector<proto::DecodedFrame> const& frames)
{
    return static_cast<std::size_t>(std::ranges::count_if(
        frames, [](auto const& frame) { return std::holds_alternative<proto::SessionState>(frame.pdu); }));
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

namespace vthost
{
/// Exposes NativeSession's private follow map to the leak regression test.
struct NativeSessionFollowTester
{
    static bool follows(NativeSession const& session, vtworkspace::SessionId id)
    {
        return session._followed.contains(id.value);
    }
    static std::size_t followedCount(NativeSession const& session) { return session._followed.size(); }
};
} // namespace vthost

TEST_CASE("the native handshake answers ServerHello and a full snapshot", "[vthost][native]")
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
    CHECK(textOf(delta->lines.front()) == "hello native");
}

TEST_CASE("the attach snapshot carries scrollback that predates the attach", "[vthost][native]")
{
    // The property the whole feature rests on, and the one the user documentation got backwards:
    // a client that was not there when the output happened still receives it. The snapshot is
    // built by Grid::forEachValidLine, which walks from -historyLineCount() rather than from the
    // page top -- so the rows that scrolled off BEFORE the ClientHello travel with the page.
    //
    // Distinct from "a burst that outran the scrollback floor" below, which asserts on the resync
    // snapshot a later burst provokes. This one asserts on the ATTACH snapshot itself, with no
    // output at all after the handshake.
    auto h = NativeHarness { { .history = vtbackend::LineCount(50) } };
    h.host.createTab();
    auto const sessionId = h.host.model().window(h.host.windowId())->activeTab()->rootPane()->session();

    // 40 rows into a 25-line page: 16 of them (the page holds the last 24 plus the cursor row)
    // are scrollback by the time anyone attaches.
    auto lines = std::string {};
    for (auto const i: std::views::iota(0, 40))
        lines += std::format("row-{}\r\n", i);
    h.host.terminal(sessionId)->writeToScreen(lines);

    // Expect: ServerHello, LayoutState, SessionState, Delta (snapshot).
    auto const received = h.exchange({ proto::ClientHello {} }, 4);
    REQUIRE(received.size() == 4);
    auto const* delta = std::get_if<proto::Delta>(&received[3].pdu);
    REQUIRE(delta != nullptr);
    CHECK(delta->snapshot == 1);
    // More rows than the page holds: the surplus IS the scrollback.
    CHECK(delta->lines.size() > 25);

    // And it is the right scrollback, not merely the right amount of it. row-0 scrolled off long
    // before the attach, so finding it proves the history travelled rather than the page.
    auto const carries = [delta](std::string_view row) {
        return std::ranges::any_of(delta->lines, [row](auto const& line) { return textOf(line) == row; });
    };
    CHECK(carries("row-0"));
    CHECK(carries("row-39"));
}

TEST_CASE("a row's trailing fill cells do not travel", "[vthost][native]")
{
    // The mirror clears every row under its fill before writing the cells it received, so
    // the columns padding a short line out to the grid's width say nothing the clear did
    // not already say — at ~24 wire bytes each. Across a scrollback that padding IS the
    // snapshot: it is what made a resize able to overflow a client's send queue.
    auto h = NativeHarness {};
    h.host.createTab();
    auto const sessionId = h.host.model().window(h.host.windowId())->activeTab()->rootPane()->session();
    h.host.terminal(sessionId)->writeToScreen("hello native");

    auto const received = h.exchange({ proto::ClientHello {} }, 4);
    REQUIRE(received.size() == 4);
    auto const* delta = std::get_if<proto::Delta>(&received[3].pdu);
    REQUIRE(delta != nullptr);
    REQUIRE(delta->lines.size() == 25);

    auto const& written = delta->lines.front();
    CHECK(written.columns == 80);             // the row is still eighty columns wide...
    CHECK(written.cells.size() == 12);        // ...but only "hello native" travels
    CHECK(delta->lines.back().cells.empty()); // an untouched row ships no cells at all

    // What does travel is unchanged — the trim may only remove what the fill reproduces.
    auto text = std::string {};
    for (auto const& cell: written.cells)
        text += static_cast<char>(cell.codepoint);
    CHECK(text == "hello native");
}

TEST_CASE("a trailing run that differs from the fill still travels", "[vthost][native]")
{
    // The trim's predicate is "indistinguishable from this line's fill", not "blank": a run
    // of spaces carrying a background colour is invisible as text and load-bearing as paint.
    // Getting this wrong silently truncates coloured regions, so it is pinned here.
    auto h = NativeHarness {};
    h.host.createTab();
    auto const sessionId = h.host.model().window(h.host.windowId())->activeTab()->rootPane()->session();
    // Four cells of text, then four blanks on a red background, on a default-filled line.
    h.host.terminal(sessionId)->writeToScreen("bar\033[41m    \033[m");

    auto const received = h.exchange({ proto::ClientHello {} }, 4);
    REQUIRE(received.size() == 4);
    auto const* delta = std::get_if<proto::Delta>(&received[3].pdu);
    REQUIRE(delta != nullptr);

    auto const& written = delta->lines.front();
    REQUIRE(written.cells.size() == 7); // "bar" + four painted blanks, nothing beyond
    for (auto const& cell: written.cells | std::views::drop(3))
    {
        CHECK(cell.codepoint == U' ');                    // nothing to read...
        CHECK(cell.background != written.fillBackground); // ...but not the line's fill either
    }
}

TEST_CASE("Input PDUs land in the target pane's PTY", "[vthost][native]")
{
    auto h = NativeHarness {};
    h.host.createTab();
    auto const sessionId = h.host.model().window(h.host.windowId())->activeTab()->rootPane()->session();

    auto input = proto::Input { .session = sessionId.value, .data = {} };
    for (auto const ch: std::string_view { "ls\r" })
        input.data.push_back(static_cast<std::byte>(ch));

    std::ignore = h.exchange({ proto::ClientHello {}, proto::DecodedPdu { input } }, 3);

    auto& tapped = dynamic_cast<vthost::TappingPty&>(h.host.terminal(sessionId)->device());
    auto& mock = dynamic_cast<vtpty::MockPty&>(tapped.inner());
    CHECK(mock.stdinBuffer() == "ls\r");
}

TEST_CASE("FetchImage for an unknown id answers ImageGone", "[vthost][native]")
{
    auto h = NativeHarness {};
    h.host.createTab();

    // ServerHello, LayoutState, then ImageGone, and only then the attach snapshot
    // (SessionState + Delta). The snapshot is QUEUED by the handshake and emitted by the streamer
    // on a later loop turn, so a request the client pipelined behind its hello is answered first.
    // Nothing depends on the old order: replies are correlated by serial, not by position — and
    // the client asserting its client area right after the hello now RESIZES before the snapshot
    // is captured, so the one grid it receives is already the right shape.
    auto const received =
        h.exchange({ proto::ClientHello {}, proto::DecodedPdu { proto::FetchImage { .imageId = 4242 } } }, 5);
    REQUIRE(received.size() == 5);
    auto const* gone = std::get_if<proto::ImageGone>(&received[2].pdu);
    REQUIRE(gone != nullptr);
    CHECK(gone->imageId == 4242);
    CHECK(received[2].serial == 2); // correlated to the request's serial
    CHECK(std::get_if<proto::SessionState>(&received[3].pdu) != nullptr);
    CHECK(std::get_if<proto::Delta>(&received[4].pdu) != nullptr);
}

TEST_CASE("a version-mismatched hello is answered and the session ends", "[vthost][native]")
{
    auto h = NativeHarness {};
    h.host.createTab();

    auto const received = h.exchange({ proto::ClientHello { .codecVersion = 9999 } }, 2);
    // Only the ServerHello arrives — no snapshot follows a failed handshake.
    REQUIRE(received.size() == 1);
    CHECK(std::holds_alternative<proto::ServerHello>(received[0].pdu));
}

TEST_CASE("an alternate-screen flip forces a resync snapshot with SessionState", "[vthost][native]")
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
    CHECK(textOf(delta->lines.front()) == "ALT!");
}

TEST_CASE("a snapshot carries the input-encoding state, resets included", "[vthost][native]")
{
    // The two protocols that decide how a CLIENT encodes keys are pulled and diffed, and on a
    // snapshot they travel in SessionState rather than a Delta changed-flag. A snapshot must
    // therefore state them outright — including at their defaults, which is where an app lands
    // when it stops asking. The server records what it sent and never repeats it, so a default
    // omitted here is a default the client never hears about.
    auto h = NativeHarness {};
    h.host.createTab();
    auto const sessionId = h.host.model().window(h.host.windowId())->activeTab()->rootPane()->session();
    // Negotiate both BEFORE attaching, so the attach snapshot has something to carry.
    h.host.terminal(sessionId)->writeToScreen("\033[>5u\033[>4;2m");

    auto const bytes = encodeRequest({ proto::DecodedPdu { proto::ClientHello {} } });
    auto received = std::vector<proto::DecodedFrame> {};
    h.loop.blockOn(net::testing::allOf(h.session->run(),
                                       feedBytes(h.pair.second.get(), &bytes),
                                       collectPdus(h.pair.second.get(), 6, &received),
                                       flipToAltScreen(&h, sessionId)));
    REQUIRE(received.size() == 6);

    auto const* primary = std::get_if<proto::SessionState>(&received[2].pdu);
    REQUIRE(primary != nullptr);
    CHECK(primary->kittyKeyboardFlags == 5);
    CHECK(primary->modifyOtherKeys == 2);

    // The alternate-screen flip snapshots again; the state must be restated, not assumed.
    auto const* alt = std::get_if<proto::SessionState>(&received[4].pdu);
    REQUIRE(alt != nullptr);
    CHECK(alt->kittyKeyboardFlags == 5);
    CHECK(alt->modifyOtherKeys == 2);
}

TEST_CASE("a burst that outran the scrollback floor is served as a snapshot", "[vthost][native]")
{
    // An incremental delta can only name rows back to the grid's scrollback floor
    // (Grid::scrolledOutDepthSince clamps there). When more rows scrolled off than the floor
    // still covers, reporting the remainder and staying quiet about the rest is worse than
    // resynchronizing: the client scrolls every unreported id through its page as a BLANK row,
    // fabricating scrollback the session never contained.
    auto h = NativeHarness { { .history = vtbackend::LineCount(10) } };
    h.host.createTab();
    auto const sessionId = h.host.model().window(h.host.windowId())->activeTab()->rootPane()->session();

    auto burst = [](NativeHarness* harness, vtworkspace::SessionId id) -> Task<void> {
        co_await harness->loop.delay(5ms);
        // Far more rows than the 10-line floor can name, in one batch.
        auto lines = std::string {};
        for (auto const i: std::views::iota(0, 100))
            lines += std::format("line-{}\r\n", i);
        harness->host.terminal(id)->writeToScreen(lines);
        harness->session->sessionScreenUpdated(id);
    }(&h, sessionId);

    auto const bytes = encodeRequest({ proto::DecodedPdu { proto::ClientHello {} } });
    auto received = std::vector<proto::DecodedFrame> {};
    h.loop.blockOn(net::testing::allOf(h.session->run(),
                                       feedBytes(h.pair.second.get(), &bytes),
                                       collectPdus(h.pair.second.get(), 6, &received),
                                       std::move(burst)));
    REQUIRE(received.size() == 6);

    // [0] ServerHello, [1] LayoutState, [2] SessionState, [3] Delta (the attach snapshot),
    // then the burst's [4] SessionState + [5] Delta. That the burst produced a SessionState
    // at all is already the tell: only a snapshot emits one.
    REQUIRE(std::get_if<proto::SessionState>(&received[4].pdu) != nullptr);
    auto const* delta = std::get_if<proto::Delta>(&received[5].pdu);
    REQUIRE(delta != nullptr);
    CHECK(delta->snapshot == 1);
    // A snapshot carries the whole addressable grid: the page plus the history still held.
    CHECK(delta->lines.size() > 10);
}

TEST_CASE("a snapshot anchors the cursor so the following delta is incremental", "[vthost][native]")
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

    auto const text = textOf(incremental->lines.front());
    CHECK(text.starts_with("first")); // the snapshot content is still there
    CHECK(text.contains("more"));     // with the newly appended bytes
}

TEST_CASE("a cursor-only move still produces a delta so the mirror's cursor tracks", "[vthost][native]")
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

TEST_CASE("a scrollback eviction reaches the client though no row changed", "[vthost][native]")
{
    // `clear`/CSI 3 J (and tmux's clear-history) evicts the scrollback through Grid::clearHistory,
    // which deliberately bumps no generation and dirties no line — the floor is the ONLY signal.
    // Gated on what the delta itself knows, the PDU was dropped: the mirror's own detector for this
    // case (ScreenMirror's `floorOutranScroll`) waited on a delta that never arrived, so an attached
    // window kept rendering the entire scrollback the daemon had thrown away.
    auto h = NativeHarness { { .history = vtbackend::LineCount(10) } };
    h.host.createTab();
    auto const sessionId = h.host.model().window(h.host.windowId())->activeTab()->rootPane()->session();
    // More than the 25-line page holds, so rows actually reach the scrollback: with no history at
    // all the eviction moves no floor and the test would assert nothing.
    auto lines = std::string {};
    for (auto const i: std::views::iota(0, 40))
        lines += std::format("line-{}\r\n", i);
    h.host.terminal(sessionId)->writeToScreen(lines);
    REQUIRE(h.host.terminal(sessionId)->primaryScreen().grid().historyLineCount() > vtbackend::LineCount(0));

    auto const bytes = encodeRequest({ proto::DecodedPdu { proto::ClientHello {} } });
    auto received = std::vector<proto::DecodedFrame> {};
    h.loop.blockOn(net::testing::allOf(h.session->run(),
                                       feedBytes(h.pair.second.get(), &bytes),
                                       collectPdus(h.pair.second.get(), 5, &received),
                                       clearHistoryThenUpdate(&h, sessionId)));
    REQUIRE(received.size() == 5);

    // [0] ServerHello, [1] LayoutState, [2] SessionState, [3] Delta(snapshot), [4] the eviction.
    auto const* snapshot = std::get_if<proto::Delta>(&received[3].pdu);
    REQUIRE(snapshot != nullptr);
    auto const* evicted = std::get_if<proto::Delta>(&received[4].pdu);
    REQUIRE(evicted != nullptr);
    CHECK(evicted->lines.empty());                       // nothing was rewritten...
    CHECK(evicted->stableFloor > snapshot->stableFloor); // ...the floor alone moved
}

TEST_CASE("a window-title change is carried in the following incremental delta", "[vthost][native]")
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
                                       setTitleThenUpdate(&h, sessionId)));
    REQUIRE(received.size() == 5);

    // [4] the incremental delta carries the title, gated on the diff against the last-sent one
    // (windowTitle() is compared as a string_view, so an UNCHANGED title allocates nothing).
    auto const* delta = std::get_if<proto::Delta>(&received[4].pdu);
    REQUIRE(delta != nullptr);
    CHECK(delta->snapshot == 0);
    CHECK(delta->titleChanged == 1);
    CHECK(delta->title == "my-title");
}

TEST_CASE("a hyperlink URI is delivered on first reference", "[vthost][native]")
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

TEST_CASE("a debounce flush pending at disconnect resolves before run() returns", "[vthost][native]")
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

TEST_CASE("a closed session's follow state is pruned", "[vthost][native]")
{
    auto h = NativeHarness {};
    h.host.createTab();
    auto const sessionId = h.host.model().window(h.host.windowId())->activeTab()->rootPane()->session();

    // The handshake snapshot makes the session followed (pushDelta seeds _followed).
    std::ignore = h.exchange({ proto::ClientHello {} }, 3);
    REQUIRE(vthost::NativeSessionFollowTester::follows(*h.session, sessionId));

    // The host learning its own session closed must fan out to stream subscribers,
    // and the session must drop the per-session follow state (otherwise it leaks
    // for the connection's whole lifetime -- one entry per session ever opened).
    h.host.subscribeStream(h.session.get());
    h.host.handleSessionExit(sessionId);
    h.host.unsubscribeStream(h.session.get());

    CHECK_FALSE(vthost::NativeSessionFollowTester::follows(*h.session, sessionId));
    CHECK(vthost::NativeSessionFollowTester::followedCount(*h.session) == 0);
}

TEST_CASE("a client that overflows the write queue is disconnected", "[vthost][native]")
{
    auto done = false;
    auto watchdogFired = false;
    // No reply fits an 8-byte bound: the very first send overflows, and the
    // session must apply the queue's disconnect contract instead of silently
    // under-serving the client from then on.
    auto h = NativeHarness { { .writeQueueBytes = 8 } };
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

TEST_CASE("attaching to several scrollback-heavy panes is not refused", "[vthost][native]")
{
    // THE regression this whole pacing mechanism exists for. A daemon holding a couple of panes
    // with real scrollback became permanently unattachable: completeHandshake pushed one full-grid
    // snapshot per leaf pane synchronously, inside the PDU pump's handler, so the drain coroutine
    // -- which EventLoop::spawn only queues -- could not run until the burst finished. The whole
    // attach payload therefore had to fit under the send-queue bound at once, the second pane's
    // snapshot did not fit, and the connection was dropped before a single byte reached the client.
    // Nothing on the daemon changed between attempts, so every reconnect reproduced it exactly.
    auto capture = logstore::ScopedCapture {};

    // A bound far smaller than the snapshots it must carry, which is the shape of the report:
    // 4 MiB against two panes of ~1.8 MB each. The point is that the SUM exceeding the bound must
    // no longer matter, because no two pieces are ever in the queue at the same time.
    auto h = NativeHarness { { .writeQueueBytes = std::size_t { 64 } * 1024,
                               .history = vtbackend::LineCount(30) } };

    auto sessions = std::vector<vtworkspace::SessionId> {};
    for (auto const tab: std::views::iota(0, 3))
    {
        h.host.createTab();
        auto const id = h.host.model().window(h.host.windowId())->activeTab()->rootPane()->session();
        sessions.push_back(id);
        fillRows(h, id, 50, std::format("tab{}-row", tab));
    }

    // ServerHello, LayoutState, then SessionState + one snapshot Delta per pane.
    auto const received = h.exchange({ proto::ClientHello {} }, 8);
    REQUIRE(received.size() == 8);
    CHECK_FALSE(capture.contains("send queue overflow"));

    // Every pane arrived, and arrived whole: a snapshot that is not superseded and not truncated.
    auto snapshots = std::map<uint64_t, proto::Delta> {};
    for (auto const& frame: received)
        if (auto const* delta = std::get_if<proto::Delta>(&frame.pdu))
        {
            CHECK(delta->snapshot == 1);
            snapshots.emplace(delta->session, *delta);
        }
    REQUIRE(snapshots.size() == sessions.size());
    for (auto const tab: std::views::iota(0, 3))
    {
        auto const found = snapshots.find(sessions[static_cast<std::size_t>(tab)].value);
        REQUIRE(found != snapshots.end());
        auto const carries = [&](std::string_view row) {
            return std::ranges::any_of(found->second.lines,
                                       [row](auto const& line) { return textOf(line).starts_with(row); });
        };
        // The oldest row is scrollback that predates the attach; the newest is on the page.
        CHECK(carries(std::format("tab{}-row-0x", tab)));
        CHECK(carries(std::format("tab{}-row-49x", tab)));
    }
}

TEST_CASE("a snapshot larger than one frame travels as a marked run", "[vthost][native]")
{
    // The other cliff: one pane's snapshot is the whole grid INCLUDING all scrollback, built into
    // a single PDU. Past proto::MaxFrameSize the peer's own decoder rejects it, so no amount of
    // pacing would deliver it. It is split instead, and each piece names its place so the mirror
    // knows which one may clear what it holds and which one completes the run.
    auto h = NativeHarness { { .history = vtbackend::LineCount(4000) } };
    h.host.createTab();
    auto const sessionId = h.host.model().window(h.host.windowId())->activeTab()->rootPane()->session();

    fillRows(h, sessionId, 4000, "row");

    // ServerHello, LayoutState, SessionState, then however many pieces the rows partition into.
    auto const bytes = encodeRequest({ proto::DecodedPdu { proto::ClientHello {} } });
    auto received = std::vector<proto::DecodedFrame> {};
    h.loop.blockOn(driveSnapshotRun(h.session.get(), h.pair.second.get(), &bytes, &received));

    auto pieces = std::vector<proto::Delta> {};
    for (auto const& frame: received)
        if (auto const* delta = std::get_if<proto::Delta>(&frame.pdu))
            pieces.push_back(*delta);
    REQUIRE(pieces.size() >= 2);

    // Every piece says it is a snapshot -- a piece must never be mistaken for an increment -- and
    // the run is marked First, then Middle, then Last.
    for (auto const& piece: pieces)
        CHECK(piece.snapshot == 1);
    CHECK(pieces.front().part() == proto::SnapshotPart::First);
    CHECK(pieces.back().part() == proto::SnapshotPart::Last);
    for (auto const& middle: pieces | std::views::drop(1) | std::views::take(pieces.size() - 2))
        CHECK(middle.part() == proto::SnapshotPart::Middle);

    // No piece is anywhere near the frame cap, and the rows are partitioned across them rather
    // than repeated: the run delivers each row exactly once.
    auto seen = std::set<int64_t> {};
    auto total = std::size_t { 0 };
    for (auto const& piece: pieces)
    {
        for (auto const& line: piece.lines)
            seen.insert(line.stableId);
        total += piece.lines.size();
    }
    CHECK(seen.size() == total);
    // Every addressable row travelled exactly once: the 4000 rows written, plus the empty row the
    // cursor sits on after the last CRLF. None is dropped at a piece boundary and none is repeated.
    CHECK(total == 4001);
}

TEST_CASE("an increment is held back until the snapshot it would interleave with finishes",
          "[vthost][native]")
{
    // A snapshot travels as a run of PDUs, and the mirror only repaints when the run completes.
    // An incremental delta slipped BETWEEN two pieces would be published immediately, having the
    // client repaint a grid it has only half received -- every row the run has not delivered yet
    // rendering as absent. So a session with a snapshot queued or in flight gets no increment; it
    // stays pending and goes out once the run is done.
    auto h = NativeHarness { { .writeQueueBytes = std::size_t { 64 } * 1024,
                               .history = vtbackend::LineCount(4000) } };
    h.host.createTab();
    auto const sessionId = h.host.model().window(h.host.windowId())->activeTab()->rootPane()->session();

    fillRows(h, sessionId, 4000, "row");

    // Output arriving while the attach run is still streaming: the debounce fires 20ms later,
    // well inside a run that pauses for the write queue to drain between each of its ~25 pieces.
    auto const bytes = encodeRequest({ proto::DecodedPdu { proto::ClientHello {} } });
    auto received = std::vector<proto::DecodedFrame> {};
    h.loop.blockOn(
        net::testing::allOf(driveSnapshotRun(h.session.get(), h.pair.second.get(), &bytes, &received, 1),
                            appendThenUpdate(&h, sessionId)));

    // The run is intact: every Delta up to and including the terminating piece is a snapshot
    // piece, with no increment wedged among them.
    auto sawRunEnd = false;
    auto increments = 0;
    for (auto const& frame: received)
    {
        auto const* delta = std::get_if<proto::Delta>(&frame.pdu);
        if (delta == nullptr)
            continue;
        if (delta->snapshot == 0)
        {
            CHECK(sawRunEnd); // the whole point: never before the run ended
            ++increments;
            continue;
        }
        CHECK_FALSE(sawRunEnd);
        sawRunEnd = delta->completesSnapshot();
    }
    CHECK(sawRunEnd);
    // And the increment was held, not dropped: the rows written mid-run still arrive.
    CHECK(increments == 1);
}

// ---------------------------------------------------------------------------
// Diagnostics. Every failure below was completely SILENT before the logging
// pass: the daemon dropped the connection and said nothing, which is precisely
// the class of bug these regression tests exist to prevent recurring.

TEST_CASE("a codec-version mismatch is reported with both versions", "[vthost][native][diagnostics]")
{
    auto capture = logstore::ScopedCapture {};
    auto harness = NativeHarness {};
    auto const received = harness.exchange(
        { proto::DecodedPdu { proto::ClientHello { .codecVersion = proto::CodecVersion - 1 } } }, 1);

    // The peer still gets its ServerHello so it can report the mismatch itself.
    REQUIRE(received.size() == 1);
    CHECK(std::holds_alternative<proto::ServerHello>(received[0].pdu));

    CHECK(capture.contains("test#1"));
    CHECK(capture.contains("handshake rejected"));
    CHECK(capture.contains(std::format("v{}", proto::CodecVersion - 1)));
    CHECK(capture.contains(std::format("v{}", proto::CodecVersion)));
}

TEST_CASE("a missing ClientHello names what arrived instead", "[vthost][native][diagnostics]")
{
    auto capture = logstore::ScopedCapture {};
    auto harness = NativeHarness {};
    std::ignore = harness.exchange({ proto::DecodedPdu { proto::CreateTab {} } }, 1);

    CHECK(capture.contains("handshake rejected"));
    CHECK(capture.contains("expected ClientHello, got CreateTab"));
}

TEST_CASE("a token mismatch is reported without either token", "[vthost][native][diagnostics]")
{
    // The wire answer deliberately makes this indistinguishable from a version mismatch, so the
    // daemon-side log is the ONLY place the real reason exists. It must still not write the
    // secret down.
    auto capture = logstore::ScopedCapture {};

    auto source = net::PollEventSource {};
    auto loop = net::EventLoop { source };
    auto host = SessionHost { loop,
                              [](vtbackend::PageSize size) { return std::make_unique<vtpty::MockPty>(size); },
                              vtbackend::Settings {},
                              crispy::defaultEnvironment(),
                              /*startPumps=*/false };
    auto pair = *net::testing::makeSocketPair(loop);
    auto session = std::make_unique<NativeSession>(
        loop,
        host,
        vthost::ConnectionId { .endpoint = "native-tcp", .index = 3, .peer = "10.0.0.7" },
        std::move(pair.first),
        NativeSession::DefaultWriteQueueBytes,
        "the-right-token");

    auto const bytes = encodeRequest({ proto::DecodedPdu {
        proto::ClientHello { .codecVersion = proto::CodecVersion, .token = "the-wrong-token" } } });
    auto received = std::vector<proto::DecodedFrame> {};
    loop.blockOn(driveExchange(session.get(), pair.second.get(), &bytes, 1, &received));

    CHECK(capture.contains("native-tcp#3@10.0.0.7"));
    CHECK(capture.contains("preshared token mismatch"));
    CHECK_FALSE(capture.contains("the-right-token"));
    CHECK_FALSE(capture.contains("the-wrong-token"));
}

TEST_CASE("a client area that did not change is not answered with a resync", "[vthost][native]")
{
    // THE regression these two cases exist to hold: a resync is a full-grid snapshot including
    // all scrollback, and clients re-assert their client area on every attach. Answering an
    // unchanged area with one snapshot per pane is what overflowed the send queue and got
    // healthy clients disconnected — every time, since the daemon's history does not shrink.
    auto h = NativeHarness {};
    h.host.createTab();
    auto const spawned = h.host.pageSize();

    auto const received = exchangeAll(
        &h,
        { proto::DecodedPdu { proto::ClientHello {} },
          proto::DecodedPdu { proto::ResizeRequest { .columns = unbox<uint32_t>(spawned.columns),
                                                     .lines = unbox<uint32_t>(spawned.lines) } } });

    // Exactly the attach answer: ServerHello, LayoutState, SessionState, Delta.
    REQUIRE(received.size() == 4);
    auto const* state = std::get_if<proto::SessionState>(&received[2].pdu);
    REQUIRE(state != nullptr);
    CHECK(state->columns == unbox<uint32_t>(spawned.columns));
    CHECK(state->lines == unbox<uint32_t>(spawned.lines));
}

TEST_CASE("a client area that did change is answered with a resync", "[vthost][native]")
{
    // The other half, so the case above cannot pass vacuously: if reprojection stopped
    // answering real resizes, the dimensions below stay at the spawned size.
    auto h = NativeHarness {};
    h.host.createTab();
    auto const spawned = h.host.pageSize();
    auto const grown = vtpty::PageSize { .lines = spawned.lines + vtpty::LineCount(3),
                                         .columns = spawned.columns + vtpty::ColumnCount(7) };

    auto const received =
        exchangeAll(&h,
                    { proto::DecodedPdu { proto::ClientHello {} },
                      proto::DecodedPdu { proto::ResizeRequest { .columns = unbox<uint32_t>(grown.columns),
                                                                 .lines = unbox<uint32_t>(grown.lines) } } });

    // Still four PDUs, and that is the coalescing working rather than the resync missing: the
    // attach snapshot was still unwritten when the resize superseded it, so the client is told
    // the new geometry ONCE instead of being handed two whole grids back to back.
    REQUIRE(received.size() == 4);
    auto const* state = std::get_if<proto::SessionState>(&received[2].pdu);
    REQUIRE(state != nullptr);
    CHECK(state->columns == unbox<uint32_t>(grown.columns));
    CHECK(state->lines == unbox<uint32_t>(grown.lines));
    auto const* delta = std::get_if<proto::Delta>(&received[3].pdu);
    REQUIRE(delta != nullptr);
    CHECK(delta->snapshot == 1);
}

TEST_CASE("an out-of-range resize proposal is reported", "[vthost][native][diagnostics]")
{
    auto capture = logstore::ScopedCapture {};
    auto harness = NativeHarness {};
    std::ignore =
        harness.exchange({ proto::DecodedPdu { proto::ClientHello {} },
                           proto::DecodedPdu { proto::ResizeRequest { .columns = 1'000'000, .lines = 24 } } },
                         1);

    CHECK(capture.contains("rejecting client area 1000000x24 (out of range)"));
}

TEST_CASE("an unexpected PDU is reported rather than silently dropped", "[vthost][native][diagnostics]")
{
    auto capture = logstore::ScopedCapture {};
    auto harness = NativeHarness {};
    // A server-to-client PDU sent the wrong way: valid on the wire, meaningless here.
    std::ignore = harness.exchange(
        { proto::DecodedPdu { proto::ClientHello {} }, proto::DecodedPdu { proto::ServerHello {} } }, 1);

    CHECK(capture.contains("ignoring unexpected ServerHello"));
}

TEST_CASE("the PDU trace records both directions when enabled", "[vthost][native][diagnostics]")
{
    auto capture = logstore::ScopedCapture { "vthost.trace.proto" };
    auto harness = NativeHarness {};
    std::ignore = harness.exchange({ proto::DecodedPdu { proto::ClientHello {} } }, 1);

    CHECK(capture.contains("test#1 recv #1 ClientHello"));
    CHECK(capture.contains("test#1 send #1 ServerHello"));
    // Sizes, never contents.
    CHECK(capture.contains("bytes)"));
}

TEST_CASE("the PDU trace is disabled by default", "[vthost][native][diagnostics]")
{
    // It is one line per frame on a busy session; enabling it by accident would be a
    // performance and a privacy problem at once.
    auto const* const category = logstore::get("vthost.trace.proto");
    REQUIRE(category != nullptr);
    CHECK_FALSE(category->isEnabled());
}

TEST_CASE("a ClientHello's settings reach the sessions that client creates", "[vthost][native]")
{
    auto h = NativeHarness { HarnessOptions { .history = vtbackend::LineCount(500) } };

    // An EMPTY daemon, so the session below exists precisely because this client attached — it must
    // inherit the client's profile just like a tab the client goes on to open.
    auto hello = proto::ClientHello {};
    hello.sessionSettings = proto::WireSessionSettings {
        .historyLineCount = 7000,
        .terminalId = 41, // VT420
        .graphemeClustering = 0,
        .allowReflowOnResize = 0,
        .maxImageRegisterCount = 512,
    };

    // ServerHello, LayoutState, SessionState, Delta.
    std::ignore = h.exchange({ proto::DecodedPdu { hello } }, 4);

    auto const sessionId = h.host.model().window(h.host.windowId())->activeTab()->rootPane()->session();
    auto* terminal = h.host.terminal(sessionId);
    REQUIRE(terminal != nullptr);
    CHECK(terminal->terminalId() == vtbackend::VTType::VT420);
    CHECK(unbox<int>(terminal->primaryScreen().grid().maxHistoryLineCount()) == 7000);

    // The host's own settings are NOT rewritten: they remain the default for anything the daemon or
    // another client creates.
    REQUIRE(std::holds_alternative<vtbackend::LineCount>(h.host.settings().historyLimits.capacity));
    CHECK(unbox<int>(std::get<vtbackend::LineCount>(h.host.settings().historyLimits.capacity)) == 500);
}

TEST_CASE("a ClientHello without settings leaves the host's own in force", "[vthost][native]")
{
    auto h = NativeHarness { HarnessOptions { .history = vtbackend::LineCount(500) } };

    std::ignore = h.exchange({ proto::ClientHello {} }, 4);

    auto const sessionId = h.host.model().window(h.host.windowId())->activeTab()->rootPane()->session();
    auto* terminal = h.host.terminal(sessionId);
    REQUIRE(terminal != nullptr);
    CHECK(unbox<int>(terminal->primaryScreen().grid().maxHistoryLineCount()) == 500);
    CHECK(terminal->terminalId() == h.host.settings().terminalId);
}

TEST_CASE("a client cannot ask a session out of the host invariants", "[vthost][native]")
{
    auto h = NativeHarness { HarnessOptions { .history = vtbackend::LineCount(500) } };

    // The wire is remote input, so the request is re-normalized rather than trusted. Zero scrollback
    // is the configuration that makes every scrolling batch a full snapshot; an unknown terminalId
    // number cannot be clamped into anything sensible, so the daemon's own stands.
    auto hello = proto::ClientHello {};
    hello.sessionSettings = proto::WireSessionSettings { .historyLineCount = 0, .terminalId = 42 };

    std::ignore = h.exchange({ proto::DecodedPdu { hello } }, 4);

    auto const sessionId = h.host.model().window(h.host.windowId())->activeTab()->rootPane()->session();
    auto* terminal = h.host.terminal(sessionId);
    REQUIRE(terminal != nullptr);
    CHECK(unbox<int>(terminal->primaryScreen().grid().maxHistoryLineCount())
          == vthost::DefaultSessionHistoryLineCount);
    CHECK(terminal->terminalId() == h.host.settings().terminalId);
}

TEST_CASE("a resize one client asked for resyncs EVERY attached client", "[vthost][native]")
{
    // Maximizing one window left the other rendering the old grid. The resync was issued by the
    // connection that received the ResizeRequest, over its OWN follow map, so no other client heard
    // about it -- and `Terminal::resizeScreen` raises no screen update, so nothing else would tell
    // them either until the session next produced output. A shared grid has to announce itself.
    auto h = TwoClientHarness {};
    h.host.createTab();

    auto const hello = encodeRequest({ proto::DecodedPdu { proto::ClientHello {} } });
    // 40x10 is nothing like the host's default, so the resize cannot be a no-op once clamped.
    auto const helloThenResize = encodeRequest({
        proto::DecodedPdu { proto::ClientHello {} },
        proto::DecodedPdu { proto::ResizeRequest { .columns = 40, .lines = 10 } },
    });

    auto fromOne = std::vector<proto::DecodedFrame> {};
    auto fromTwo = std::vector<proto::DecodedFrame> {};

    // Client two attaches first and then sits idle; client one attaches and resizes. The delay
    // orders the resize after BOTH handshakes, so client two is attached when the grid moves --
    // which is the whole scenario. Collection ends at EOF, so the counts below are exact.
    h.loop.blockOn(net::testing::allOf(h.serverOne->run(),
                                       h.serverTwo->run(),
                                       feedAfter(&h.loop, h.secondPair.second.get(), &hello, 0ms),
                                       feedAfter(&h.loop, h.firstPair.second.get(), &helloThenResize, 20ms),
                                       closeAfter(&h.loop, h.firstPair.second.get(), 90ms),
                                       closeAfter(&h.loop, h.secondPair.second.get(), 90ms),
                                       collectPdus(h.firstPair.second.get(), 1000, &fromOne),
                                       collectPdus(h.secondPair.second.get(), 1000, &fromTwo)));

    // The client that asked: its attach snapshot, then the resize snapshot.
    auto const* const oneState = lastSessionState(fromOne);
    REQUIRE(oneState != nullptr);
    CHECK(oneState->columns == 40);
    CHECK(oneState->lines == 10);

    // The client that did NOT ask, which is the regression: it must have been resynced too, and its
    // LAST snapshot must describe the new grid rather than the one it attached at.
    auto const* const twoState = lastSessionState(fromTwo);
    REQUIRE(twoState != nullptr);
    CHECK(twoState->columns == 40);
    CHECK(twoState->lines == 10);
    // Two snapshots, not one: the attach plus the resize. Equal sizes alone would also pass if the
    // second client had simply attached late, after the resize.
    CHECK(sessionStateCount(fromTwo) == 2);
}
