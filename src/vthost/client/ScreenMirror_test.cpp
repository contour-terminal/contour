// SPDX-License-Identifier: Apache-2.0
#include <vtbackend/screen/Terminal.hpp>

#include <vtpty/MockPty.hpp>

#include <crispy/Base64.hpp>

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <format>
#include <functional>
#include <memory>
#include <optional>
#include <ranges>
#include <string>
#include <string_view>
#include <tuple>
#include <vector>

#include <coro/WhenAll.hpp>
#include <net/EventLoop.hpp>
#include <net/PollEventSource.hpp>
#include <net/testing/InMemoryTransport.hpp>
#include <vthost/GridWire.hpp>
#include <vthost/NativeSession.hpp>
#include <vthost/SessionHost.hpp>
#include <vthost/client/NativeClient.hpp>
#include <vthost/client/ScreenMirror.hpp>
#include <vthost/testing/GridParity.hpp>
#include <vtworkspace/Pane.hpp>
#include <vtworkspace/Tab.hpp>

using coro::Task;
using vthost::NativeSession;
using vthost::SessionHost;
using vthost::client::NativeClient;
using vthost::client::ScreenMirror;
namespace proto = vthost::proto;
using namespace std::chrono_literals;

namespace
{

/// The hosted session's scrollback depth. DELIBERATELY shallower than the mirror's —
/// the production relationship, and what makes a history comparison assert anything
/// (on two zero-history terminals "the histories match" is the empty claim 0 == 0).
constexpr auto ServerHistoryLines = vtbackend::LineCount(100);

/// The mirror terminal's scrollback depth.
constexpr auto MirrorHistoryLines = vtbackend::LineCount(1000);

/// Settings with the Good Image Protocol enabled on both ends, so an inline
/// image drawn on the server round-trips through the mirror's re-emit.
/// @param history The scrollback depth the terminal keeps.
vtbackend::Settings gipSettings(vtbackend::LineCount history)
{
    auto settings = vtbackend::Settings {};
    settings.goodImageProtocol = true;
    settings.maxHistoryLineCount = history;
    return settings;
}

/// A mirror-terminal event sink recording the transient events (bell,
/// notification, clipboard write) the mirror is expected to reproduce.
struct RecordingEvents final: vtbackend::Terminal::NullEvents
{
    int bells = 0;
    std::string notifyTitle;
    std::string notifyBody;
    std::string clipboard;

    void bell() override { ++bells; }
    void notify(std::string_view title, std::string_view body) override
    {
        notifyTitle = title;
        notifyBody = body;
    }
    void copyToClipboard(std::string_view data) override { clipboard = data; }
};

/// The full closed loop: the REAL server serves deltas of a REAL terminal, the REAL client mirrors
/// them into RemoteScreen, and the ScreenMirror populates a LOCAL mirror terminal from them — whose
/// grid must then equal the server terminal's, which is what the [parity] cases below measure.
struct MirrorHarness
{
    net::PollEventSource source;
    net::EventLoop loop { source };
    SessionHost host { loop,
                       [](vtbackend::PageSize size) { return std::make_unique<vtpty::MockPty>(size); },
                       gipSettings(ServerHistoryLines),
                       crispy::defaultEnvironment(),
                       /*startPumps=*/false };
    net::testing::SocketPair pair = *net::testing::makeSocketPair(loop);
    std::unique_ptr<NativeSession> server = std::make_unique<NativeSession>(
        loop, host, vthost::ConnectionId { .endpoint = "test", .index = 1 }, std::move(pair.first));
    std::unique_ptr<NativeClient> client;

    RecordingEvents mirrorEvents;
    std::unique_ptr<vtbackend::Terminal> mirror;
    /// Constructed in the body, not here: it binds to `mirror` for life, so it cannot exist before
    /// the terminal does. @see ScreenMirror's constructor.
    std::unique_ptr<ScreenMirror> populator;

    MirrorHarness()
    {
        auto settings = gipSettings(MirrorHistoryLines);
        mirror = std::make_unique<vtbackend::Terminal>(mirrorEvents,
                                                       crispy::defaultEnvironment(),
                                                       std::make_unique<vtpty::MockPty>(settings.pageSize),
                                                       std::move(settings),
                                                       std::chrono::steady_clock::now());
        populator = std::make_unique<ScreenMirror>(*mirror);
        // Construct client after mirror — the handler lambdas capture `this` and
        // access `mirror`, so mirror must be ready first.
        client = std::make_unique<NativeClient>(
            loop,
            std::move(pair.second),
            NativeClient::HandshakeOptions {},
            NativeClient::UpdateHandler {
                [this](vthost::client::RemoteScreen const& screen, proto::Delta const& delta) {
                    populator->apply(screen, delta);
                } },
            NativeClient::ImageHandler {
                [this](vthost::client::RemoteScreen const& screen, uint32_t imageId) {
                    populator->applyImage(screen, imageId);
                } },
            NativeClient::SessionEventHandler {
                [this](vthost::client::RemoteScreen const& screen, proto::SessionEventPdu const& event) {
                    std::ignore = screen;
                    populator->applyEvent(event);
                } },
            NativeClient::LayoutHandler {});
        // Deliver the host's stream fan-out (bell / notify / clipboard, and screen
        // updates) to the session, exactly as the daemon's serveNativeClient does —
        // so the transient-event path (Terminal::Events -> host -> NativeSession) is
        // exercised, not just the manually-poked sessionScreenUpdated.
        host.subscribeStream(server.get());
    }

    ~MirrorHarness() { host.unsubscribeStream(server.get()); }

    MirrorHarness(MirrorHarness const&) = delete;
    MirrorHarness& operator=(MirrorHarness const&) = delete;
    MirrorHarness(MirrorHarness&&) = delete;
    MirrorHarness& operator=(MirrorHarness&&) = delete;

    [[nodiscard]] vtbackend::Terminal* serverTerminal(vtworkspace::SessionId session)
    {
        return host.terminal(session);
    }
};

Task<void> waitUntil(net::EventLoop* loop, std::function<bool()> ready)
{
    for (auto i = 0; i < 1000 && !ready(); ++i)
        co_await loop->delay(1ms);
}

Task<void> drive(MirrorHarness* h, Task<void> scenario)
{
    co_await coro::whenAll(h->server->run(), h->client->run(), std::move(scenario));
}

/// Writes @p bytes on the server terminal and pushes the resulting delta.
void serverWrites(MirrorHarness* h, vtworkspace::SessionId session, std::string_view bytes)
{
    h->serverTerminal(session)->writeToScreen(bytes);
    h->server->sessionScreenUpdated(session);
}

/// Writes @p reset on the server BATCHED with an alternate-screen entry, then waits for
/// the mirror to follow the flip.
///
/// The batching is the point: the page flip is what makes the resulting push a SNAPSHOT,
/// and a snapshot routes live state through SessionState rather than a Delta changed-flag.
/// A reset that lands this way is the case a replay gating on "non-default" drops.
Task<void> resetInsideSnapshot(MirrorHarness* h, vtworkspace::SessionId session, std::string_view reset)
{
    serverWrites(h, session, std::format("{}\033[?1049h", reset));
    co_await waitUntil(&h->loop, [&] { return h->mirror->isAlternateScreen(); });
    REQUIRE(h->mirror->isAlternateScreen());
}

/// One 5x1 wire row at @p stableId.
proto::WireLine rowAt(int64_t stableId)
{
    auto line = proto::WireLine {};
    line.stableId = stableId;
    line.columns = 5;
    return line;
}

/// One 5x1 wire row at @p stableId carrying @p text (ASCII, at most 5 columns).
proto::WireLine rowAt(int64_t stableId, std::string_view text)
{
    auto line = rowAt(stableId);
    for (auto const ch: text)
    {
        auto cell = proto::WireCell {};
        cell.codepoint = static_cast<char32_t>(ch);
        line.cells.push_back(cell);
    }
    return line;
}

/// Asserts the mirror's scrollback matches the server's, row for row.
/// @param mirrorGrid The mirror terminal's primary grid.
/// @param serverGrid The hosted session's primary grid.
void checkHistoryMatches(vtbackend::Grid const& mirrorGrid, vtbackend::Grid const& serverGrid)
{
    REQUIRE(mirrorGrid.historyLineCount() == serverGrid.historyLineCount());
    for (auto const offset: std::views::iota(1, unbox<int>(serverGrid.historyLineCount()) + 1))
        CHECK(mirrorGrid.lineText(vtbackend::LineOffset(-offset))
              == serverGrid.lineText(vtbackend::LineOffset(-offset)));
}

/// @p count lines of "row-N", each terminated, for a test that needs the page to overflow.
std::string numberedRows(int count)
{
    auto lines = std::string {};
    for (auto const i: std::views::iota(0, count))
        lines += std::format("row-{}\r\n", i);
    return lines;
}

/// A bare 5x1 mirror terminal plus the ScreenMirror bound to it, for the unit-level cases that
/// drive the mirror by hand instead of through the full server/client loop.
struct BareMirror
{
    vtbackend::Terminal::NullEvents events;
    std::unique_ptr<vtbackend::Terminal> terminal;
    std::unique_ptr<ScreenMirror> mirror;

    explicit BareMirror(vtbackend::LineCount history)
    {
        auto settings = vtbackend::Settings {};
        settings.pageSize = vtbackend::PageSize { vtbackend::LineCount(1), vtbackend::ColumnCount(5) };
        settings.maxHistoryLineCount = history;
        terminal = std::make_unique<vtbackend::Terminal>(events,
                                                         crispy::defaultEnvironment(),
                                                         std::make_unique<vtpty::MockPty>(settings.pageSize),
                                                         std::move(settings),
                                                         std::chrono::steady_clock::now());
        mirror = std::make_unique<ScreenMirror>(*terminal);
    }

    [[nodiscard]] int historyLines() const
    {
        return unbox<int>(terminal->primaryScreen().grid().historyLineCount());
    }
};

} // namespace

TEST_CASE("a server-side clear drops the mirror's local scrollback", "[vthost][mirror]")
{
    // Unit-level: drive ScreenMirror directly, and assert on the mirror terminal's OWN scrollback
    // depth. The mirror populates the grid, so "did it discard local history?" is a question about
    // the grid — it used to be answered by looking for an ESC[3J in the emitted bytes.
    auto bare = BareMirror { vtbackend::LineCount(100) };
    auto screen = vthost::client::RemoteScreen {};
    screen.columns = 5;
    screen.lines = 1;

    // Prime with a screen that HAS scrollback: rows 7..10 with a one-line page means the mirror
    // scrolls three of them into its own history.
    auto seed = proto::Delta {};
    seed.snapshot = 1;
    seed.stableViewportBase = 10;
    seed.stableFloor = 7;
    for (auto const id: std::views::iota(int64_t { 7 }, int64_t { 11 }))
        seed.lines.push_back(rowAt(id));
    screen.apply(seed);
    bare.mirror->apply(screen, seed); // primes (full replay), remembers floor 7
    REQUIRE(bare.historyLines() == 3);

    // An ordinary incremental delta (floor unchanged) stays incremental: it must
    // NOT touch scrollback.
    auto tick = proto::Delta {};
    tick.stableViewportBase = 10;
    tick.stableFloor = 7;
    tick.lines.push_back(rowAt(10));
    screen.apply(tick);
    bare.mirror->apply(screen, tick);
    CHECK(bare.historyLines() == 3);

    // A `clear`/CSI 3 J jumps the floor to the viewport base with NO line change
    // and NO generation bump. The incremental path would leave ghost scrollback,
    // so the mirror must fall back to a DISCARDING full replay.
    auto cleared = proto::Delta {};
    cleared.stableViewportBase = 10;
    cleared.stableFloor = 10;
    screen.apply(cleared);
    bare.mirror->apply(screen, cleared);
    CHECK(bare.historyLines() == 0);
}

TEST_CASE("a wire hyperlink id reused for another URI re-points the mirror", "[vthost][mirror]")
{
    // The sender's HyperlinkId is a uint16_t that wraps and reuses ids, and NativeSession re-sends
    // the {id, URI} pair precisely so a reused id cannot pin the mirror to a stale URI. A write-once
    // cache dropped that re-send, and a freshly printed link in an attached pane then opened a
    // completely unrelated older URL — while the same session viewed locally opened the right one.
    auto bare = BareMirror { vtbackend::LineCount(100) };
    auto screen = vthost::client::RemoteScreen {};
    screen.columns = 5;
    screen.lines = 1;

    auto const linkedRow = [](int64_t id, uint16_t wireLink) {
        auto row = rowAt(id, "abcde");
        for (auto& cell: row.cells)
            cell.hyperlink = wireLink;
        return row;
    };
    auto const uriAt = [&](vtbackend::ColumnOffset column) {
        auto const& stored = bare.terminal->primaryScreen().grid().lineAt(vtbackend::LineOffset(0)).storage();
        auto const info = bare.terminal->hyperlinks().hyperlinkById(stored.hyperlinks[unbox<size_t>(column)]);
        return info ? info->uri : std::string {};
    };

    auto seed = proto::Delta {};
    seed.snapshot = 1;
    seed.stableViewportBase = 10;
    seed.stableFloor = 10;
    seed.hyperlinks.push_back(proto::HyperlinkEntry { .id = 1, .uri = "https://example.com/first" });
    seed.lines.push_back(linkedRow(10, 1));
    screen.apply(seed);
    bare.mirror->apply(screen, seed);
    CHECK(uriAt(vtbackend::ColumnOffset(0)) == "https://example.com/first");

    // The same wire id, now naming a DIFFERENT URI: the counter wrapped on the server.
    auto reused = proto::Delta {};
    reused.stableViewportBase = 10;
    reused.stableFloor = 10;
    reused.hyperlinks.push_back(proto::HyperlinkEntry { .id = 1, .uri = "https://example.com/second" });
    reused.lines.push_back(linkedRow(10, 1));
    screen.apply(reused);
    bare.mirror->apply(screen, reused);
    CHECK(uriAt(vtbackend::ColumnOffset(0)) == "https://example.com/second");

    // Re-sending the mapping the mirror already holds must NOT mint a second local id: the server
    // re-sends every URI after a dropped frame, and a fresh id per repeat would churn the LRU.
    auto const idBefore = bare.terminal->hyperlinks().nextHyperlinkId;
    auto repeat = proto::Delta {};
    repeat.stableViewportBase = 10;
    repeat.stableFloor = 10;
    repeat.hyperlinks.push_back(proto::HyperlinkEntry { .id = 1, .uri = "https://example.com/second" });
    repeat.lines.push_back(linkedRow(10, 1));
    screen.apply(repeat);
    bare.mirror->apply(screen, repeat);
    CHECK(bare.terminal->hyperlinks().nextHyperlinkId == idBefore);
}

TEST_CASE("an OSC 3008 context reaches the mirror and stamps its lines", "[vthost][mirror]")
{
    // The mirror drives a real vtbackend::Terminal, so a replicated context makes tinting, the
    // breadcrumb and the cwd resolver work on an attached pane with no client-side code of their own
    // -- provided the record and the per-line id actually arrive.
    auto bare = BareMirror { vtbackend::LineCount(100) };
    auto screen = vthost::client::RemoteScreen {};
    screen.columns = 5;
    screen.lines = 1;

    auto container = proto::WireContext {};
    container.id = 7;
    container.identifier = "box";
    container.type = static_cast<uint8_t>(vtbackend::ContextType::Container);
    container.present = static_cast<uint16_t>(vtbackend::ContextField::Container)
                        | static_cast<uint16_t>(vtbackend::ContextField::WorkingDirectory);
    container.container = "foobar";
    container.workingDirectory = "/app";

    auto seed = proto::Delta {};
    seed.snapshot = 1;
    seed.stableViewportBase = 10;
    seed.stableFloor = 10;
    seed.contexts.push_back(container);
    seed.contextChanged = 1;
    seed.activeContext = 7;
    auto row = rowAt(10, "abcde");
    row.contextId = 7;
    seed.lines.push_back(row);
    screen.apply(seed);
    bare.mirror->apply(screen, seed);

    auto const& contexts = bare.terminal->contexts();
    REQUIRE(contexts.active() != nullptr);
    CHECK(contexts.active()->identifier == "box");
    CHECK(contexts.active()->type == vtbackend::ContextType::Container);
    CHECK(contexts.active()->container == "foobar");
    CHECK(contexts.active()->workingDirectory == "/app");

    // The line resolves through the MIRROR's own id space, which need not equal the sender's.
    auto const lineContext =
        bare.terminal->primaryScreen().grid().lineAt(vtbackend::LineOffset(0)).contextId();
    REQUIRE(!!lineContext);
    REQUIRE(contexts.find(lineContext) != nullptr);
    CHECK(contexts.find(lineContext)->identifier == "box");
}

TEST_CASE("a context re-announced with new metadata updates the mirror in place", "[vthost][mirror]")
{
    // systemd re-announces the shell context on every prompt, and `end=` records an outcome on a
    // command context: both reinitialise a record IN PLACE, under the same id AND the same identifier.
    // A gate keyed on the identifier alone therefore replicated the first announcement and nothing
    // after it, leaving an attached pane resolving a directory the session left long ago.
    auto bare = BareMirror { vtbackend::LineCount(100) };
    auto screen = vthost::client::RemoteScreen {};
    screen.columns = 5;
    screen.lines = 1;

    auto shell = proto::WireContext {};
    shell.id = 4;
    shell.identifier = "sh";
    shell.type = static_cast<uint8_t>(vtbackend::ContextType::Shell);
    shell.present = static_cast<uint16_t>(vtbackend::ContextField::WorkingDirectory);
    shell.workingDirectory = "/home/user";

    auto seed = proto::Delta {};
    seed.snapshot = 1;
    seed.stableViewportBase = 10;
    seed.stableFloor = 10;
    seed.contexts.push_back(shell);
    seed.contextChanged = 1;
    seed.activeContext = 4;
    seed.lines.push_back(rowAt(10, "abcde"));
    screen.apply(seed);
    bare.mirror->apply(screen, seed);

    auto const& contexts = bare.terminal->contexts();
    REQUIRE(contexts.active() != nullptr);
    auto const localId = contexts.active()->id;
    CHECK(contexts.active()->workingDirectory == "/home/user");

    // The SAME context, re-announced after a `cd`. Same id, same identifier, different cwd.
    shell.workingDirectory = "/home/user/src";
    auto update = proto::Delta {};
    update.stableViewportBase = 10;
    update.stableFloor = 10;
    update.contexts.push_back(shell);
    screen.apply(update);
    bare.mirror->apply(screen, update);

    REQUIRE(contexts.active() != nullptr);
    // Updated in place, so every line already stamped with this id still resolves to it.
    CHECK(contexts.active()->id == localId);
    CHECK(contexts.active()->workingDirectory == "/home/user/src");
}

TEST_CASE("a context ancestry whose parent links form a cycle does not spin the mirror", "[vthost][mirror]")
{
    // The wire's context table ACCUMULATES and the sender's id space wraps, so two records can end up
    // naming each other as parent. RemoteScreen rebuilds the ancestry by walking those links, and a
    // walk that only refused a SELF-reference would append to contextChain until it ran out of memory.
    auto screen = vthost::client::RemoteScreen {};

    auto makeContext = [](uint16_t id, uint16_t parent) {
        auto context = proto::WireContext {};
        context.id = id;
        context.parent = parent;
        context.identifier = std::format("ctx-{}", id);
        return context;
    };

    auto delta = proto::Delta {};
    delta.contexts.push_back(makeContext(1, 2));
    delta.contexts.push_back(makeContext(2, 1));
    delta.contextChanged = 1;
    delta.activeContext = 1;
    screen.apply(delta);

    // Each id visited once, innermost last after the reverse.
    CHECK(screen.contextChain == std::vector<uint16_t> { 2, 1 });
}

TEST_CASE("a wire context id reused for another context does not re-point old lines", "[vthost][mirror]")
{
    // The sender's ContextId is a uint16_t that wraps, exactly as HyperlinkId does. A line written
    // before a reuse legitimately points at the OLD context, so the mirror must mint a fresh local id
    // rather than rewriting the record behind the one it already handed out.
    auto bare = BareMirror { vtbackend::LineCount(100) };
    auto screen = vthost::client::RemoteScreen {};
    screen.columns = 5;
    screen.lines = 1;

    auto makeContext = [](uint16_t id, std::string_view identifier) {
        auto context = proto::WireContext {};
        context.id = id;
        context.identifier = identifier;
        context.type = static_cast<uint8_t>(vtbackend::ContextType::Command);
        return context;
    };

    auto first = proto::Delta {};
    first.snapshot = 1;
    first.stableViewportBase = 10;
    first.stableFloor = 10;
    first.contexts.push_back(makeContext(1, "cmd-one"));
    auto rowOne = rowAt(10, "aaaaa");
    rowOne.contextId = 1;
    first.lines.push_back(rowOne);
    screen.apply(first);
    bare.mirror->apply(screen, first);

    auto const& grid = bare.terminal->primaryScreen().grid();
    auto const idOne = grid.lineAt(vtbackend::LineOffset(0)).contextId();
    REQUIRE(bare.terminal->contexts().find(idOne) != nullptr);
    CHECK(bare.terminal->contexts().find(idOne)->identifier == "cmd-one");

    // The same wire id, now naming a DIFFERENT context: the counter wrapped on the server.
    auto second = proto::Delta {};
    second.stableViewportBase = 10;
    second.stableFloor = 10;
    second.contexts.push_back(makeContext(1, "cmd-two"));
    auto rowTwo = rowAt(10, "bbbbb");
    rowTwo.contextId = 1;
    second.lines.push_back(rowTwo);
    screen.apply(second);
    bare.mirror->apply(screen, second);

    auto const idTwo = grid.lineAt(vtbackend::LineOffset(0)).contextId();
    CHECK(idTwo != idOne);
    REQUIRE(bare.terminal->contexts().find(idTwo) != nullptr);
    CHECK(bare.terminal->contexts().find(idTwo)->identifier == "cmd-two");

    // And the record the earlier line still points at was NOT rewritten underneath it.
    REQUIRE(bare.terminal->contexts().find(idOne) != nullptr);
    CHECK(bare.terminal->contexts().find(idOne)->identifier == "cmd-one");
}

TEST_CASE("a snapshot asserts an EMPTY context ancestry", "[vthost][mirror]")
{
    // The Kitty-keyboard bug, in this feature's shape: a snapshot routes the ancestry through
    // SessionState rather than a changed-gate, so an ancestry that emptied since the last snapshot
    // must arrive AS empty or the mirror keeps showing a context that has ended.
    auto bare = BareMirror { vtbackend::LineCount(100) };
    auto screen = vthost::client::RemoteScreen {};
    screen.columns = 5;
    screen.lines = 1;

    auto context = proto::WireContext {};
    context.id = 3;
    context.identifier = "box";
    context.type = static_cast<uint8_t>(vtbackend::ContextType::Container);

    auto seed = proto::Delta {};
    seed.snapshot = 1;
    seed.stableViewportBase = 10;
    seed.stableFloor = 10;
    seed.contexts.push_back(context);
    seed.contextChanged = 1;
    seed.activeContext = 3;
    seed.lines.push_back(rowAt(10, "abcde"));
    screen.apply(seed);
    bare.mirror->apply(screen, seed);
    REQUIRE(bare.terminal->contexts().active() != nullptr);

    // A fresh snapshot whose ancestry is empty.
    auto state = proto::SessionState {};
    state.columns = 5;
    state.lines = 1;
    state.contexts.push_back(context);
    // contextChain deliberately left empty
    screen.apply(state);
    bare.mirror->fullReplay(screen, vthost::client::LocalHistory::Keep);

    CHECK(bare.terminal->contexts().active() == nullptr);
    CHECK(bare.terminal->contexts().depth() == 0);
}

TEST_CASE("a peer-announced viewport jump cannot spin the mirror", "[vthost][mirror]")
{
    // `stableViewportBase` is a wire field, and every row of the announced advance costs a
    // Screen::scrollUp plus a writeRow — on the reactor thread, under the terminal lock. Unbounded,
    // a base naming a far-away row froze the attached window for as long as the arithmetic said.
    // The bound loses nothing: the skipped rows are neither named by the server (so they would be
    // written as blanks) nor still held by this terminal when the loop ends.
    auto bare = BareMirror { vtbackend::LineCount(100) };
    auto screen = vthost::client::RemoteScreen {};
    screen.columns = 5;
    screen.lines = 1;

    auto seed = proto::Delta {};
    seed.snapshot = 1;
    seed.stableViewportBase = 10;
    seed.stableFloor = 7;
    for (auto const id: std::views::iota(int64_t { 7 }, int64_t { 11 }))
        seed.lines.push_back(rowAt(id, "seed"));
    screen.apply(seed);
    bare.mirror->apply(screen, seed);
    REQUIRE(bare.historyLines() == 3);

    // A jump of five million rows, naming none of them. Unbounded this is five million scrolls;
    // bounded it is the mirror's own capacity, so the test simply has to RETURN.
    auto jump = proto::Delta {};
    jump.stableViewportBase = 5'000'010;
    jump.stableFloor = 7; // deliberately unchanged, so floorOutranScroll does not fire
    screen.apply(jump);
    bare.mirror->apply(screen, jump);

    // Everything it could still be holding was streamed in: its history is full, and no deeper.
    CHECK(bare.historyLines() == 100);
}

TEST_CASE("the mirror terminal reproduces text, SGR and cursor", "[vthost][mirror]")
{
    auto h = MirrorHarness {};
    h.host.createTab();
    auto const session = h.host.model().window(h.host.windowId())->activeTab()->rootPane()->session();
    h.serverTerminal(session)->writeToScreen("plain \033[1;31mbold-red\033[0m \033[4;58;5;33munder\033[0m");

    auto scenario = [](MirrorHarness* h, vtworkspace::SessionId session) -> Task<void> {
        co_await waitUntil(&h->loop, [&] {
            return h->mirror->primaryScreen().grid().renderMainPageText().contains("bold-red");
        });

        auto const& serverGrid = h->serverTerminal(session)->primaryScreen().grid();
        auto const& mirrorGrid = h->mirror->primaryScreen().grid();
        CHECK(mirrorGrid.renderMainPageText() == serverGrid.renderMainPageText());

        // The SGR state made it: bold flag and colors of "bold-red"'s first cell.
        auto const& serverRow = serverGrid.lineAt(vtbackend::LineOffset(0)).storage();
        auto const& mirrorRow = mirrorGrid.lineAt(vtbackend::LineOffset(0)).storage();
        auto const column = 6; // first cell of "bold-red"
        CHECK(mirrorRow.sgr[column].flags == serverRow.sgr[column].flags);
        CHECK(mirrorRow.sgr[column].foregroundColor == serverRow.sgr[column].foregroundColor);
        auto const underColumn = 15; // first cell of "under"
        CHECK(mirrorRow.sgr[underColumn].underlineColor == serverRow.sgr[underColumn].underlineColor);

        // The cursor landed where the server's cursor is.
        CHECK(h->mirror->primaryScreen().cursor().position
              == h->serverTerminal(session)->primaryScreen().cursor().position);

        h->client->detach();
    }(&h, session);

    h.loop.blockOn(drive(&h, std::move(scenario)));
}

TEST_CASE("DEC pages beyond primary/alternate mirror faithfully", "[vthost][mirror]")
{
    auto h = MirrorHarness {};
    h.host.createTab();
    auto const session = h.host.model().window(h.host.windowId())->activeTab()->rootPane()->session();
    h.serverTerminal(session)->writeToScreen("page-zero-here");

    auto scenario = [](MirrorHarness* h, vtworkspace::SessionId session) -> Task<void> {
        // Page 0 (primary) mirrors onto the mirror's primary buffer.
        co_await waitUntil(&h->loop, [&] {
            return h->mirror->primaryScreen().grid().renderMainPageText().contains("page-zero-here");
        });
        CHECK(!h->mirror->isAlternateScreen());

        // Live-switch to DEC page 1 (NP) and write there. DECPCCM couples the
        // display by default, so the user now sees page 1 — which shares the
        // "Alternate" wire screen-type with pages 2..14 and the xterm alt page, yet
        // is a distinct grid. The daemon must force a resync onto it (the page-index
        // gate, not the primary-vs-alt one, is what catches this).
        serverWrites(h, session, "\033[1Upage-one-here"); // NP -> page 1, then write
        co_await waitUntil(&h->loop, [&] {
            return h->mirror->isAlternateScreen()
                   && h->mirror->alternateScreen().grid().renderMainPageText().contains("page-one-here");
        });
        // The mirror shows page 1's content, not page 0's bleeding through.
        CHECK(!h->mirror->alternateScreen().grid().renderMainPageText().contains("page-zero-here"));
        CHECK(h->mirror->alternateScreen().grid().renderMainPageText()
              == h->serverTerminal(session)->pageAt(vtbackend::PageIndex(1)).grid().renderMainPageText());

        // Switch back to page 0 (PP): the mirror leaves the alternate buffer and
        // page 0's preserved content returns from the daemon's fresh snapshot.
        serverWrites(h, session, "\033[1V"); // PP -> page 0
        co_await waitUntil(&h->loop, [&] {
            return !h->mirror->isAlternateScreen()
                   && h->mirror->primaryScreen().grid().renderMainPageText().contains("page-zero-here");
        });
        CHECK(!h->mirror->primaryScreen().grid().renderMainPageText().contains("page-one-here"));

        h->client->detach();
    }(&h, session);

    h.loop.blockOn(drive(&h, std::move(scenario)));
}

TEST_CASE("a decoupled cursor page hides the mirror's cursor", "[vthost][mirror]")
{
    auto h = MirrorHarness {};
    h.host.createTab();
    auto const session = h.host.model().window(h.host.windowId())->activeTab()->rootPane()->session();
    h.serverTerminal(session)->writeToScreen("visible-page-zero");

    auto scenario = [](MirrorHarness* h, vtworkspace::SessionId session) -> Task<void> {
        // Coupled (the default): the mirror shows page 0 with a visible cursor.
        co_await waitUntil(&h->loop, [&] {
            return h->mirror->primaryScreen().grid().renderMainPageText().contains("visible-page-zero");
        });
        CHECK(h->mirror->isModeEnabled(vtbackend::DECMode::VisibleCursor));

        // Decouple the display from the cursor (DECPCCM reset), then move VT output
        // to page 1. The user keeps looking at page 0, so the fat GUI hides the
        // cursor (it now belongs to an off-screen page) — the daemon must mirror that
        // by withholding DECTCEM even though the app never hid the cursor itself.
        serverWrites(h, session, "\033[?64l\033[1Uhidden-on-page-one"); // DECPCCM off, NP, write
        co_await waitUntil(&h->loop,
                           [&] { return !h->mirror->isModeEnabled(vtbackend::DECMode::VisibleCursor); });
        CHECK(!h->mirror->isModeEnabled(vtbackend::DECMode::VisibleCursor));
        // The displayed page is still 0: its content stands and page 1 does not bleed
        // through — VT output landed on a page the user is not looking at.
        CHECK(h->mirror->primaryScreen().grid().renderMainPageText().contains("visible-page-zero"));
        CHECK(!h->mirror->primaryScreen().grid().renderMainPageText().contains("hidden-on-page-one"));
        CHECK(!h->mirror->isAlternateScreen());

        h->client->detach();
    }(&h, session);

    h.loop.blockOn(drive(&h, std::move(scenario)));
}

TEST_CASE("Kitty keyboard flags mirror so the client encodes keys alike", "[vthost][mirror]")
{
    auto h = MirrorHarness {};
    h.host.createTab();
    auto const session = h.host.model().window(h.host.windowId())->activeTab()->rootPane()->session();
    h.serverTerminal(session)->writeToScreen("shell-ready");

    auto scenario = [](MirrorHarness* h, vtworkspace::SessionId session) -> Task<void> {
        co_await waitUntil(&h->loop, [&] {
            return h->mirror->primaryScreen().grid().renderMainPageText().contains("shell-ready");
        });
        // Neither side has negotiated the Kitty keyboard protocol yet.
        REQUIRE(h->mirror->keyboardProtocol().flags().value() == 0);

        // The app pushes Kitty flags (CSI > flags u). The mirror must adopt the same
        // flags, or a keypress it encodes would not match what the app negotiated.
        serverWrites(h, session, "\033[>5u"); // push flags 5 (disambiguate | report-alternate)
        co_await waitUntil(&h->loop, [&] { return h->mirror->keyboardProtocol().flags().value() == 5; });
        CHECK(h->serverTerminal(session)->keyboardProtocol().flags().value() == 5);
        CHECK(h->mirror->keyboardProtocol().flags().value() == 5);

        // A later change propagates too: the app raises the flags (here via the
        // set-exactly form, mode 1).
        serverWrites(h, session, "\033[=13;1u"); // set flags to 13 (adds report-all-keys)
        co_await waitUntil(&h->loop, [&] { return h->mirror->keyboardProtocol().flags().value() == 13; });
        CHECK(h->mirror->keyboardProtocol().flags().value() == 13);

        // And the way DOWN propagates as well. A protocol that only ever mirrors
        // enablement leaves the client encoding keys nobody asked for.
        serverWrites(h, session, "\033[=0;1u");
        co_await waitUntil(&h->loop, [&] { return h->mirror->keyboardProtocol().flags().value() == 0; });
        CHECK(h->mirror->keyboardProtocol().flags().value() == 0);

        h->client->detach();
    }(&h, session);

    h.loop.blockOn(drive(&h, std::move(scenario)));
}

TEST_CASE("a Kitty-flag reset carried by a snapshot reaches the mirror", "[vthost][mirror]")
{
    // The `tmux attach` bug. A shell pops its CSI-u flags before running a child, and
    // the child enters the alternate screen — both inside one debounce window, so ONE
    // push carries them. That push is a snapshot (the displayed page changed), and a
    // snapshot routes live state through SessionState rather than a Delta changed-flag,
    // where the server records it as sent and never mentions it again. A full replay
    // that emitted the flags only when non-zero therefore dropped the reset for good:
    // the mirror kept CSI-u encoding, and Ctrl+A reached tmux as literal `CSI 97;5u`.
    auto h = MirrorHarness {};
    h.host.createTab();
    auto const session = h.host.model().window(h.host.windowId())->activeTab()->rootPane()->session();
    h.serverTerminal(session)->writeToScreen("shell-ready");

    auto scenario = [](MirrorHarness* h, vtworkspace::SessionId session) -> Task<void> {
        co_await waitUntil(&h->loop, [&] {
            return h->mirror->primaryScreen().grid().renderMainPageText().contains("shell-ready");
        });

        serverWrites(h, session, "\033[>5u"); // the shell pushes flags 5
        co_await waitUntil(&h->loop, [&] { return h->mirror->keyboardProtocol().flags().value() == 5; });
        REQUIRE(h->mirror->keyboardProtocol().flags().value() == 5);

        co_await resetInsideSnapshot(h, session, "\033[<u"); // pop the flag stack
        REQUIRE(h->serverTerminal(session)->keyboardProtocol().flags().value() == 0);
        CHECK(h->mirror->keyboardProtocol().flags().value() == 0);

        h->client->detach();
    }(&h, session);

    h.loop.blockOn(drive(&h, std::move(scenario)));
}

TEST_CASE("the progress indicator mirrors", "[vthost][mirror]")
{
    // OSC 9;4 is per-session state, so it rides the gated delta and the snapshot alike — a thin
    // client must show the same bar the fat client would, including one already in flight when it
    // attaches.
    auto h = MirrorHarness {};
    h.host.createTab();
    auto const session = h.host.model().window(h.host.windowId())->activeTab()->rootPane()->session();
    h.serverTerminal(session)->writeToScreen("shell-ready");

    auto scenario = [](MirrorHarness* h, vtworkspace::SessionId session) -> Task<void> {
        co_await waitUntil(&h->loop, [&] {
            return h->mirror->primaryScreen().grid().renderMainPageText().contains("shell-ready");
        });
        REQUIRE(h->mirror->progress() == vtbackend::Progress {});

        serverWrites(h, session, "\033]9;4;1;40\033\\");
        co_await waitUntil(&h->loop, [&] { return h->mirror->progress().percentage == 40; });
        CHECK(h->mirror->progress()
              == vtbackend::Progress { .state = vtbackend::ProgressState::Normal, .percentage = 40 });

        // A state carrying no percentage of its own keeps the one already mirrored, so the client
        // must be applying the server's RESOLVED progress rather than re-deriving it.
        serverWrites(h, session, "\033]9;4;2\033\\");
        co_await waitUntil(&h->loop,
                           [&] { return h->mirror->progress().state == vtbackend::ProgressState::Error; });
        CHECK(h->mirror->progress().percentage == 40);

        // A LIVE indicator carried by a snapshot: the SessionState field exists for exactly this,
        // and only a snapshot exercises it, since a delta would carry the same value through its
        // gated field instead. This is the "attaching mid-operation adopts the bar already in
        // flight" property -- without the SessionState half, progress would reach a client only if
        // it happened to change after that client attached.
        serverWrites(h, session, "\033]9;4;1;35\033\\");
        co_await waitUntil(&h->loop, [&] { return h->mirror->progress().percentage == 35; });
        // Switching the displayed page forces a snapshot, and this one says nothing about progress
        // -- so the value can only arrive through SessionState. Entering the alternate screen rather
        // than resetInsideSnapshot(), which asserts it is entered and so cannot be used twice.
        serverWrites(h, session, "\033[?1049h");
        co_await waitUntil(&h->loop, [&] { return h->mirror->isAlternateScreen(); });
        REQUIRE(h->serverTerminal(session)->progress().percentage == 35);
        CHECK(h->mirror->progress()
              == vtbackend::Progress { .state = vtbackend::ProgressState::Normal, .percentage = 35 });

        // And a withdrawal inside a snapshot must arrive too. Leaving the alternate screen is the
        // page switch this time, since the mirror is already on it from the case above.
        serverWrites(h, session, "\033]9;4;0\033\\\033[?1049l");
        co_await waitUntil(&h->loop, [&] { return !h->mirror->isAlternateScreen(); });
        REQUIRE(h->serverTerminal(session)->progress() == vtbackend::Progress {});
        CHECK(h->mirror->progress() == vtbackend::Progress {});

        h->client->detach();
    }(&h, session);

    h.loop.blockOn(drive(&h, std::move(scenario)));
}

TEST_CASE("xterm's modifyOtherKeys level mirrors", "[vthost][mirror]")
{
    // The other protocol an app uses to ask for modified keys as escape sequences. It
    // went unmirrored while vtbackend read XTMODKEYS' resource selector as the level,
    // which left a client on legacy encoding for any app choosing it over CSI u.
    auto h = MirrorHarness {};
    h.host.createTab();
    auto const session = h.host.model().window(h.host.windowId())->activeTab()->rootPane()->session();
    h.serverTerminal(session)->writeToScreen("shell-ready");

    auto scenario = [](MirrorHarness* h, vtworkspace::SessionId session) -> Task<void> {
        co_await waitUntil(&h->loop, [&] {
            return h->mirror->primaryScreen().grid().renderMainPageText().contains("shell-ready");
        });
        REQUIRE(h->mirror->modifyOtherKeys() == 0);

        // `CSI > 4 ; 2 m` — resource 4 (modifyOtherKeys), value 2.
        serverWrites(h, session, "\033[>4;2m");
        co_await waitUntil(&h->loop, [&] { return h->mirror->modifyOtherKeys() == 2; });
        CHECK(h->serverTerminal(session)->modifyOtherKeys() == 2);
        CHECK(h->mirror->modifyOtherKeys() == 2);

        // A reset inside a snapshot must arrive too, exactly as for the Kitty flags.
        co_await resetInsideSnapshot(h, session, "\033[>4m");
        REQUIRE(h->serverTerminal(session)->modifyOtherKeys() == 0);
        CHECK(h->mirror->modifyOtherKeys() == 0);

        h->client->detach();
    }(&h, session);

    h.loop.blockOn(drive(&h, std::move(scenario)));
}

TEST_CASE("a cursor-shape reset carried by a snapshot reaches the mirror", "[vthost][mirror]")
{
    // Same asymmetry as the Kitty flags: DECSCUSR Ps 0 IS the default, so a replay that
    // emitted the shape only when non-zero stranded the mirror on the previous one.
    auto h = MirrorHarness {};
    h.host.createTab();
    auto const session = h.host.model().window(h.host.windowId())->activeTab()->rootPane()->session();
    h.serverTerminal(session)->writeToScreen("shell-ready");

    auto scenario = [](MirrorHarness* h, vtworkspace::SessionId session) -> Task<void> {
        co_await waitUntil(&h->loop, [&] {
            return h->mirror->primaryScreen().grid().renderMainPageText().contains("shell-ready");
        });

        serverWrites(h, session, "\033[4 q"); // steady underline
        co_await waitUntil(&h->loop,
                           [&] { return h->mirror->cursorShape() == vtbackend::CursorShape::Underscore; });
        REQUIRE(h->mirror->cursorShape() == vtbackend::CursorShape::Underscore);

        co_await resetInsideSnapshot(h, session, "\033[0 q"); // back to the configured default
        CHECK(h->mirror->cursorShape() == h->serverTerminal(session)->cursorShape());

        h->client->detach();
    }(&h, session);

    h.loop.blockOn(drive(&h, std::move(scenario)));
}

TEST_CASE("a resync keeps scrollback the server can no longer name", "[vthost][mirror]")
{
    // The "scrollback goes blank when I scroll up" bug. The mirror's history is REAL
    // history it built by streaming rows through its page, and it is routinely deeper
    // than the server's (each end keeps what it was configured for). A full replay that
    // erased it — ESC[3J — and rebuilt from the server's remaining rows therefore
    // truncated the user's scrollback on every resize, page flip and resync, which in
    // practice meant every time a full-screen app started or quit.
    auto h = MirrorHarness {};
    h.host.createTab();
    auto const session = h.host.model().window(h.host.windowId())->activeTab()->rootPane()->session();

    auto scenario = [](MirrorHarness* h, vtworkspace::SessionId session) -> Task<void> {
        // Bring the SERVER to its history capacity first. While history is still filling
        // the grid grows its ring and bumps its generation, and a generation change is a
        // legitimate reason to discard — so nothing about the mirror's depth is provable
        // until the server is saturated.
        auto fill = std::string { "filler" };
        for (auto const i: std::views::iota(0, unbox<int>(ServerHistoryLines) + 30))
        {
            fill += "\r\n";
            fill += std::format("fill-{}", i);
        }
        serverWrites(h, session, fill);
        co_await waitUntil(&h->loop, [&] {
            return h->mirror->primaryScreen().grid().renderMainPageText().contains(
                std::format("fill-{}", unbox<int>(ServerHistoryLines) + 29));
        });

        // From here the server evicts one row for every row added, so each further batch
        // is a plain incremental delta and the mirror — configured deeper — keeps what
        // the server drops. That divergence is the whole point: it is the state the user
        // is in whenever their profile's history outruns the daemon's.
        constexpr auto Batches = 6;
        constexpr auto PerBatch = 10;
        for (auto const batch: std::views::iota(0, Batches))
        {
            auto lines = std::string {};
            for (auto const i: std::views::iota(0, PerBatch))
            {
                lines += "\r\n";
                lines += std::format("keep-{}", (batch * PerBatch) + i);
            }
            serverWrites(h, session, lines);
            co_await waitUntil(&h->loop, [&] {
                return h->mirror->primaryScreen().grid().renderMainPageText().contains(
                    std::format("keep-{}", (batch * PerBatch) + PerBatch - 1));
            });
        }

        auto const& mirrorGrid = h->mirror->primaryScreen().grid();
        auto const deepHistory = mirrorGrid.historyLineCount();
        // The premise: the mirror out-remembers the server. Without it the rest proves nothing.
        REQUIRE(deepHistory > h->serverTerminal(session)->primaryScreen().grid().historyLineCount());
        auto const oldestLine = mirrorGrid.lineText(vtbackend::LineOffset(-unbox<int>(deepHistory)));

        // Force a resync the server did NOT discard history for: enter and leave the
        // alternate screen, the way starting and quitting `less` does.
        serverWrites(h, session, "\033[?1049h");
        co_await waitUntil(&h->loop, [&] { return h->mirror->isAlternateScreen(); });
        serverWrites(h, session, "\033[?1049l");
        co_await waitUntil(&h->loop, [&] { return !h->mirror->isAlternateScreen(); });

        CHECK(mirrorGrid.historyLineCount() == deepHistory);
        CHECK(mirrorGrid.lineText(vtbackend::LineOffset(-unbox<int>(deepHistory))) == oldestLine);

        h->client->detach();
    }(&h, session);

    h.loop.blockOn(drive(&h, std::move(scenario)));
}

TEST_CASE("a resize keeps scrollback the server can no longer name", "[vthost][mirror]")
{
    // The resize case of the same bug, and the one that needed a rule of its own. A resize makes the
    // server reflow, which destroys its row identity and bumps the generation — and "the generation
    // changed" used to mean "discard local scrollback". So resizing the window truncated the user's
    // history to the daemon's depth, every time.
    //
    // It is sound to keep it precisely because this terminal reflowed the very same rows at the very
    // same moment: its history is the reflowed truth, not a stale copy. The [parity] resize probes
    // are what establish that the two reflows agree; this asserts the DEPTH the agreement buys, which
    // they cannot see (there both ends hold equally little).
    auto h = MirrorHarness {};
    h.host.createTab();
    auto const session = h.host.model().window(h.host.windowId())->activeTab()->rootPane()->session();

    auto scenario = [](MirrorHarness* h, vtworkspace::SessionId session) -> Task<void> {
        // Saturate the server's history FIRST. The mirror only out-remembers the server through
        // INCREMENTAL deltas — the attach snapshot can never make it deeper, because a snapshot
        // carries exactly the rows the server still holds. So the depth has to be built in two
        // stages: fill to capacity, then add batches the server evicts as it accepts them.
        auto fill = std::string { "row-0" };
        auto const filled = unbox<int>(ServerHistoryLines) + 30;
        // The CRLF is appended separately, and the braces are not optional: the spell gate splits
        // identifiers on case boundaries, so a lowercase word glued straight onto an escape has no
        // boundary to split on and arrives as one nonsense token. @see the batch loop below.
        for (auto const i: std::views::iota(1, filled))
        {
            fill += "\r\n";
            fill += std::format("row-{}", i);
        }
        serverWrites(h, session, fill);
        co_await waitUntil(&h->loop, [&] {
            return h->mirror->primaryScreen().grid().renderMainPageText().contains(
                std::format("row-{}", filled - 1));
        });

        constexpr auto Batches = 6;
        constexpr auto PerBatch = 10;
        for (auto const batch: std::views::iota(0, Batches))
        {
            auto lines = std::string {};
            for (auto const i: std::views::iota(0, PerBatch))
            {
                lines += "\r\n";
                lines += std::format("keep-{}", (batch * PerBatch) + i);
            }
            serverWrites(h, session, lines);
            co_await waitUntil(&h->loop, [&] {
                return h->mirror->primaryScreen().grid().renderMainPageText().contains(
                    std::format("keep-{}", (batch * PerBatch) + PerBatch - 1));
            });
        }

        auto const& mirrorGrid = h->mirror->primaryScreen().grid();
        // Trimmed, because a row's text is padded to its width — and the width is exactly what the
        // resize below changes, so an untrimmed comparison would fail on the padding alone.
        auto const rowText = [&mirrorGrid](int fromOldest) {
            auto text = mirrorGrid.lineText(vtbackend::LineOffset(-fromOldest));
            while (!text.empty() && text.back() == ' ')
                text.pop_back();
            return text;
        };
        auto const historyText = [&mirrorGrid, &rowText] {
            auto out = std::string {};
            for (auto const offset:
                 std::views::iota(1, unbox<int>(mirrorGrid.historyLineCount()) + 1) | std::views::reverse)
                out += rowText(offset) + "\n";
            return out;
        };
        auto const deepHistory = mirrorGrid.historyLineCount();
        auto const& serverGrid = h->serverTerminal(session)->primaryScreen().grid();
        REQUIRE(deepHistory > serverGrid.historyLineCount());
        // The oldest row THIS side still holds. The server evicted it long ago, so its survival
        // below cannot be explained by anything being re-fetched.
        auto const oldest = rowText(unbox<int>(deepHistory));
        REQUIRE(!oldest.empty());
        REQUIRE_FALSE(serverGrid.renderMainPageText().contains(oldest));

        // Resize both ends the way the GUI does: the local terminal first, then the area upstream.
        auto const target = vtbackend::PageSize { vtbackend::LineCount(25), vtbackend::ColumnCount(60) };
        h->mirror->resizeScreen(target);
        h->client->requestResize(60, 25);
        co_await waitUntil(&h->loop,
                           [h, session, target] { return h->serverTerminal(session)->pageSize() == target; });
        // Wait on the snapshot having LANDED rather than on a delay: the reflowed page matching is
        // both the observable and the thing that has to be true.
        co_await waitUntil(&h->loop, [h, session] {
            return h->mirror->primaryScreen().grid().renderMainPageText()
                   == h->serverTerminal(session)->primaryScreen().grid().renderMainPageText();
        });

        CHECK(mirrorGrid.historyLineCount() >= deepHistory);
        CHECK(historyText().contains(oldest));
        CHECK(mirrorGrid.historyLineCount() > serverGrid.historyLineCount());
        // Still the OLDEST row, not merely present somewhere: these rows never wrapped, so a
        // faithful reflow re-chops nothing and the depth is unchanged rather than just sufficient.
        CHECK(rowText(unbox<int>(mirrorGrid.historyLineCount())) == oldest);

        h->client->detach();
    }(&h, session);

    h.loop.blockOn(drive(&h, std::move(scenario)));
}

TEST_CASE("scrolled-out rows land in the mirror's local scrollback", "[vthost][mirror]")
{
    auto h = MirrorHarness {};
    h.host.createTab();
    auto const session = h.host.model().window(h.host.windowId())->activeTab()->rootPane()->session();
    h.serverTerminal(session)->writeToScreen("first-line");

    auto scenario = [](MirrorHarness* h, vtworkspace::SessionId session) -> Task<void> {
        co_await waitUntil(&h->loop, [&] {
            return h->mirror->primaryScreen().grid().renderMainPageText().contains("first-line");
        });

        // Scroll far enough that "first-line" leaves the viewport (25 lines).
        auto scrolled = std::string {};
        for (auto i = 0; i < 30; ++i)
            scrolled += std::format("\r\nline-{}", i);
        serverWrites(h, session, scrolled);
        co_await waitUntil(&h->loop, [&] {
            return h->mirror->primaryScreen().grid().renderMainPageText().contains("line-29");
        });

        auto const& serverGrid = h->serverTerminal(session)->primaryScreen().grid();
        auto const& mirrorGrid = h->mirror->primaryScreen().grid();
        CHECK(mirrorGrid.renderMainPageText() == serverGrid.renderMainPageText());
        checkHistoryMatches(mirrorGrid, serverGrid);

        h->client->detach();
    }(&h, session);

    h.loop.blockOn(drive(&h, std::move(scenario)));
}

TEST_CASE("attaching to a session with scrollback receives it as local scrollback", "[vthost][mirror]")
{
    // Everything else in this file writes AFTER the client is up, so it measures the incremental
    // path: rows that scroll by while someone is watching. This measures the other half — history
    // that already existed when the client arrived, which travels in the attach snapshot and has
    // to be streamed into the mirror's own scrollback rather than merely painted on its page.
    //
    // The user documentation claimed for a while that this did not happen at all. Nothing here
    // contradicted it, because nothing here exercised it.
    auto h = MirrorHarness {};
    h.host.createTab();
    auto const session = h.host.model().window(h.host.windowId())->activeTab()->rootPane()->session();

    // Written before blockOn, so before NativeClient::run sends the ClientHello: no client saw a
    // byte of this happen.
    h.serverTerminal(session)->writeToScreen(numberedRows(40));

    auto scenario = [](MirrorHarness* h, vtworkspace::SessionId session) -> Task<void> {
        co_await waitUntil(&h->loop, [h] {
            return h->mirror->primaryScreen().grid().renderMainPageText().contains("row-39");
        });

        auto const& serverGrid = h->serverTerminal(session)->primaryScreen().grid();
        auto const& mirrorGrid = h->mirror->primaryScreen().grid();
        // The claim is empty unless rows actually scrolled off before the attach.
        REQUIRE(serverGrid.historyLineCount() > vtbackend::LineCount(0));
        checkHistoryMatches(mirrorGrid, serverGrid);
        // Named explicitly: row-0 is the oldest line the session ever produced, so finding it at
        // the BOTTOM of the history proves the rows arrived in order, not merely in the right
        // number — which an equal-count comparison alone would not catch.
        CHECK(mirrorGrid.lineText(vtbackend::LineOffset(-unbox<int>(mirrorGrid.historyLineCount())))
                  .starts_with("row-0"));

        h->client->detach();
    }(&h, session);

    h.loop.blockOn(drive(&h, std::move(scenario)));
}

TEST_CASE("a client that attached on the alternate screen gets the primary's scrollback", "[vthost][mirror]")
{
    // Attaching does not always find a session on the primary page. The daemon snapshots the
    // DISPLAYED page, so a client arriving while vim/less/htop is up mirrors the alternate grid —
    // which has no scrollback of its own — and the primary's history reaches it only with the
    // snapshot that the flip back forces.
    //
    // The mirror used to drop it there. LocalHistory::Keep is right for an ordinary alt-exit,
    // where the mirror accumulated the primary's history itself and re-streaming would append a
    // second copy; but a mirror primed on the alternate screen has NO primary history, and Keep
    // faithfully preserved that nothing. Which branch it even took was down to whether two
    // unrelated grids' generation counters happened to collide.
    //
    // The other side of it — Keep still winning where there IS history to preserve, so an alt-exit
    // does not truncate a deep mirror to the daemon's depth — is "a resync keeps scrollback the
    // server can no longer name" above, which drives the same round trip on a saturated mirror.
    auto h = MirrorHarness {};
    h.host.createTab();
    auto const session = h.host.model().window(h.host.windowId())->activeTab()->rootPane()->session();

    // Both before blockOn: scrollback on the primary, then a full-screen application over it.
    h.serverTerminal(session)->writeToScreen(numberedRows(40));
    h.serverTerminal(session)->writeToScreen("\033[?1049hALT-SCREEN");

    auto scenario = [](MirrorHarness* h, vtworkspace::SessionId session) -> Task<void> {
        // The attach lands on the alternate screen, where there is no history to have.
        co_await waitUntil(&h->loop, [h] { return h->mirror->isAlternateScreen(); });
        REQUIRE(h->mirror->isAlternateScreen());
        REQUIRE(h->mirror->primaryScreen().grid().historyLineCount() == vtbackend::LineCount(0));

        // The application exits. The displayed page changes, which forces a snapshot of the
        // primary grid — its history included.
        serverWrites(h, session, "\033[?1049l");
        co_await waitUntil(&h->loop, [h] { return !h->mirror->isAlternateScreen(); });
        REQUIRE_FALSE(h->mirror->isAlternateScreen());

        auto const& serverGrid = h->serverTerminal(session)->primaryScreen().grid();
        auto const& mirrorGrid = h->mirror->primaryScreen().grid();
        REQUIRE(serverGrid.historyLineCount() > vtbackend::LineCount(0));
        checkHistoryMatches(mirrorGrid, serverGrid);

        h->client->detach();
    }(&h, session);

    h.loop.blockOn(drive(&h, std::move(scenario)));
}

TEST_CASE("a streamed history row keeps its fill colour", "[vthost][mirror]")
{
    // Unit-level: drive ScreenMirror directly.
    //
    // A row never sends the trailing columns that already match its own fill, and a row that
    // is uniformly its fill sends NO cells at all (@see toWireLine). Reconstructing them is
    // therefore the receiver's job, and a receiver that renders only the cells it was given
    // loses the colour of everything past the last one — which for a uniformly-filled row is
    // the whole row, so it arrives blank-on-default instead of blank-on-red.
    auto bare = BareMirror { vtbackend::LineCount(100) };
    auto screen = vthost::client::RemoteScreen {};
    screen.columns = 5;
    screen.lines = 1;

    auto constexpr RedBackground = uint32_t { 0x3000001 };

    // A one-row viewport with two rows of history below the floor. The older history row is
    // uniformly red: no cells, fill only — exactly what the wire carries for a cleared row.
    auto seed = proto::Delta {};
    seed.snapshot = 1;
    seed.stableViewportBase = 2;
    seed.stableFloor = 0;
    auto red = rowAt(0);
    red.fillBackground = RedBackground;
    seed.lines.push_back(red);
    seed.lines.push_back(rowAt(1));
    seed.lines.push_back(rowAt(2));
    screen.apply(seed);

    bare.mirror->apply(screen, seed);

    // Read back through the daemon's own serializer, so the assertion is "the row would go back
    // on the wire as red" rather than a guess about which cell to inspect.
    auto const& grid = bare.terminal->primaryScreen().grid();
    auto const historyRow = vtbackend::LineOffset(-2); // rows 0 and 1 scrolled into history
    auto const readBack = vthost::toWireLine(grid, historyRow, grid.lineAt(historyRow));
    CHECK(readBack.fillBackground == RedBackground);
}

TEST_CASE("hyperlinks survive the mirror round trip", "[vthost][mirror]")
{
    auto h = MirrorHarness {};
    h.host.createTab();
    auto const session = h.host.model().window(h.host.windowId())->activeTab()->rootPane()->session();
    h.serverTerminal(session)->writeToScreen(
        "\033]8;;https://example.com/doc\033\\linked\033]8;;\033\\ plain");

    auto scenario = [](MirrorHarness* h) -> Task<void> {
        co_await waitUntil(&h->loop, [&] {
            return h->mirror->primaryScreen().grid().renderMainPageText().contains("linked");
        });

        auto const& mirrorRow = h->mirror->primaryScreen().grid().lineAt(vtbackend::LineOffset(0)).storage();
        auto const linkId = mirrorRow.hyperlinks[0];
        REQUIRE(linkId != vtbackend::HyperlinkId(0));
        auto const info = h->mirror->hyperlinks().hyperlinkById(linkId);
        REQUIRE(info);
        CHECK(info->uri == "https://example.com/doc");
        // "plain" carries no link.
        CHECK(mirrorRow.hyperlinks[8] == vtbackend::HyperlinkId(0));

        h->client->detach();
    }(&h);

    h.loop.blockOn(drive(&h, std::move(scenario)));
}

TEST_CASE("wide characters and scaled text reproduce in the mirror", "[vthost][mirror]")
{
    auto h = MirrorHarness {};
    h.host.createTab();
    auto const session = h.host.model().window(h.host.windowId())->activeTab()->rootPane()->session();
    h.serverTerminal(session)->writeToScreen("wide: 你好 end\r\n\033]66;s=2;Big\033\\ tail");

    auto scenario = [](MirrorHarness* h, vtworkspace::SessionId session) -> Task<void> {
        co_await waitUntil(&h->loop, [&] {
            return h->mirror->primaryScreen().grid().renderMainPageText().contains("tail");
        });

        auto const& serverGrid = h->serverTerminal(session)->primaryScreen().grid();
        auto const& mirrorGrid = h->mirror->primaryScreen().grid();
        CHECK(mirrorGrid.renderMainPageText() == serverGrid.renderMainPageText());

        // The wide glyph occupies two columns in both.
        auto const& serverWide = serverGrid.lineAt(vtbackend::LineOffset(0)).storage();
        auto const& mirrorWide = mirrorGrid.lineAt(vtbackend::LineOffset(0)).storage();
        CHECK(mirrorWide.widths[6] == serverWide.widths[6]);

        // The OSC 66 block kept its scale, on the head row and the band below.
        auto const& serverScaled = serverGrid.lineAt(vtbackend::LineOffset(1)).storage();
        auto const& mirrorScaled = mirrorGrid.lineAt(vtbackend::LineOffset(1)).storage();
        CHECK(mirrorScaled.scales[0] == 2);
        CHECK(mirrorScaled.scales[0] == serverScaled.scales[0]);
        auto const& serverBand = serverGrid.lineAt(vtbackend::LineOffset(2)).storage();
        auto const& mirrorBand = mirrorGrid.lineAt(vtbackend::LineOffset(2)).storage();
        CHECK(mirrorBand.scales[0] == serverBand.scales[0]);

        h->client->detach();
    }(&h, session);

    h.loop.blockOn(drive(&h, std::move(scenario)));
}

TEST_CASE("input-relevant DEC modes mirror into the local terminal", "[vthost][mirror]")
{
    auto h = MirrorHarness {};
    h.host.createTab();
    auto const session = h.host.model().window(h.host.windowId())->activeTab()->rootPane()->session();
    h.serverTerminal(session)->writeToScreen("prompt");

    auto scenario = [](MirrorHarness* h, vtworkspace::SessionId session) -> Task<void> {
        co_await waitUntil(&h->loop, [&] {
            return h->mirror->primaryScreen().grid().renderMainPageText().contains("prompt");
        });
        CHECK(h->mirror->isModeEnabled(vtbackend::DECMode::VisibleCursor));
        CHECK(!h->mirror->mouseProtocol().has_value());

        // The app (vim, say) flips input modes WITHOUT touching any cell —
        // the pure-mode delta must still reach the mirror.
        serverWrites(h, session, "\033[?1h\033[?1000h\033[?1006h\033[?2004h\033[?25l");
        co_await waitUntil(&h->loop, [&] { return h->mirror->mouseProtocol().has_value(); });
        CHECK(h->mirror->isModeEnabled(vtbackend::DECMode::UseApplicationCursorKeys));
        CHECK(h->mirror->isModeEnabled(vtbackend::DECMode::BracketedPaste));
        CHECK(!h->mirror->isModeEnabled(vtbackend::DECMode::VisibleCursor));
        // The mouse half is asserted on the RESOLVED state, not on mode bits: that is the state
        // that decides whether a click is encoded at all, and it is what travels on the wire.
        CHECK(h->mirror->mouseProtocol() == vtbackend::MouseProtocol::NormalTracking);
        CHECK(h->mirror->mouseTransport() == vtbackend::MouseTransport::SGR);

        // And back off again.
        serverWrites(h, session, "\033[?1l\033[?1000l\033[?1006l\033[?2004l\033[?25h");
        co_await waitUntil(&h->loop, [&] { return !h->mirror->mouseProtocol().has_value(); });
        CHECK(!h->mirror->isModeEnabled(vtbackend::DECMode::UseApplicationCursorKeys));
        CHECK(!h->mirror->isModeEnabled(vtbackend::DECMode::BracketedPaste));
        CHECK(h->mirror->isModeEnabled(vtbackend::DECMode::VisibleCursor));
        CHECK(h->mirror->mouseTransport() == vtbackend::MouseTransport::Default);

        h->client->detach();
    }(&h, session);

    h.loop.blockOn(drive(&h, std::move(scenario)));
}

TEST_CASE("a mirror primed while the app already tracks the mouse can encode a click", "[vthost][mirror]")
{
    // THE bug report: with two clients on one session, the mouse worked only in the client that
    // was attached when the application turned tracking ON. A client attaching afterwards -- or a
    // pane re-bound later -- built its mirror against a session where the modes were ALREADY set,
    // and that first sync is what used to destroy them: applying the whole mirrored-mode table
    // meant applying the DISABLED rows too, and 1003-off cleared the protocol 1000/1002-on had
    // just selected while 1016-off cleared the SGR transport.
    //
    // Nothing here is written after the mirror exists: tracking is enabled BEFORE the loop runs,
    // so the mirror's very first sync is the one under test.
    auto h = MirrorHarness {};
    h.host.createTab();
    auto const session = h.host.model().window(h.host.windowId())->activeTab()->rootPane()->session();
    h.serverTerminal(session)->writeToScreen("prompt");
    h.serverTerminal(session)->writeToScreen("\033[?1000h\033[?1002h\033[?1006h"); // htop, mc, nvim

    auto scenario = [](MirrorHarness* h) -> Task<void> {
        co_await waitUntil(&h->loop, [&] {
            return h->mirror->primaryScreen().grid().renderMainPageText().contains("prompt");
        });

        CHECK(h->mirror->mouseProtocol() == vtbackend::MouseProtocol::ButtonTracking);
        CHECK(h->mirror->mouseTransport() == vtbackend::MouseTransport::SGR);

        // The property all of that exists for, asserted on the BYTES rather than on the state that
        // produces them: a click must reach the application as an SGR mouse report instead of
        // starting a local text selection. `CSI <` is SGR's own introducer, so this would not pass
        // on a mirror that had fallen back to the default (`CSI M`) encoding either.
        auto& mirrorPty = static_cast<vtpty::MockPty&>(h->mirror->device());
        mirrorPty.stdinBuffer().clear();
        std::ignore = h->mirror->sendMousePressEvent(
            vtbackend::Modifiers {}, vtbackend::MouseButton::Left, vtbackend::PixelCoordinate {}, false);
        h->mirror->flushInput();
        CHECK(mirrorPty.stdinBuffer().starts_with("\033[<0;"));

        h->client->detach();
    }(&h);

    h.loop.blockOn(drive(&h, std::move(scenario)));
}

TEST_CASE("the mirror follows the protocol the app chose LAST, not the table order", "[vthost][mirror]")
{
    // Replaying a mode SET cannot express this: 1002 and 1003 are both "on", and which one is in
    // effect is whichever the application named last. Table order would answer AnyEventTracking
    // here regardless, so the mirror would report motion events the app never asked for.
    auto h = MirrorHarness {};
    h.host.createTab();
    auto const session = h.host.model().window(h.host.windowId())->activeTab()->rootPane()->session();
    h.serverTerminal(session)->writeToScreen("prompt");
    h.serverTerminal(session)->writeToScreen("\033[?1003h\033[?1002h"); // any-event first, then button

    auto scenario = [](MirrorHarness* h, vtworkspace::SessionId session) -> Task<void> {
        co_await waitUntil(&h->loop, [&] {
            return h->mirror->primaryScreen().grid().renderMainPageText().contains("prompt");
        });
        REQUIRE(h->serverTerminal(session)->mouseProtocol() == vtbackend::MouseProtocol::ButtonTracking);
        CHECK(h->mirror->mouseProtocol() == vtbackend::MouseProtocol::ButtonTracking);

        h->client->detach();
    }(&h, session);

    h.loop.blockOn(drive(&h, std::move(scenario)));
}

TEST_CASE("a resize resyncs the mirror through a full replay", "[vthost][mirror]")
{
    auto h = MirrorHarness {};
    h.host.createTab();
    auto const session = h.host.model().window(h.host.windowId())->activeTab()->rootPane()->session();
    h.serverTerminal(session)->writeToScreen("before-resize");

    auto scenario = [](MirrorHarness* h, vtworkspace::SessionId session) -> Task<void> {
        co_await waitUntil(&h->loop, [&] {
            return h->mirror->primaryScreen().grid().renderMainPageText().contains("before-resize");
        });

        // The controller's job on a size change: resize the local terminal,
        // then let the snapshot delta repaint it. The test plays controller.
        h->client->setUpdateHandler(
            [h](vthost::client::RemoteScreen const& screen, proto::Delta const& delta) {
                auto const size =
                    vtbackend::PageSize { vtbackend::LineCount(static_cast<int>(screen.lines)),
                                          vtbackend::ColumnCount(static_cast<int>(screen.columns)) };
                if (h->mirror->pageSize() != size)
                    h->mirror->resizeScreen(size);
                h->populator->apply(screen, delta);
            });
        h->client->requestResize(100, 40);
        serverWrites(h, session, "\r\nafter-resize");
        co_await waitUntil(&h->loop, [&] {
            return h->mirror->pageSize().columns == vtbackend::ColumnCount(100)
                   && h->mirror->primaryScreen().grid().renderMainPageText().contains("after-resize");
        });

        auto const& serverGrid = h->serverTerminal(session)->primaryScreen().grid();
        auto const& mirrorGrid = h->mirror->primaryScreen().grid();
        CHECK(mirrorGrid.renderMainPageText() == serverGrid.renderMainPageText());

        h->client->detach();
    }(&h, session);

    h.loop.blockOn(drive(&h, std::move(scenario)));
}

TEST_CASE("inline images round-trip into the mirror via GIP", "[vthost][mirror]")
{
    auto h = MirrorHarness {};
    h.host.createTab();
    auto const session = h.host.model().window(h.host.windowId())->activeTab()->rootPane()->session();

    // Draw a 2-cell-wide, 1-cell-tall image at the home position on the server via a
    // GIP oneshot (StretchToFill, so both cells are covered). The client fetches the
    // pixels and the mirror re-emits them as GIP, materialising image fragments.
    auto const pixels = std::vector<uint8_t>(static_cast<std::size_t>(8 * 8 * 4), 0xC0);
    auto const body = crispy::base64::encode(
        std::string_view { reinterpret_cast<char const*>(pixels.data()), pixels.size() });
    h.serverTerminal(session)->writeToScreen(
        std::format("\033P!go=s,f=3,w=8,h=8,c=2,r=1,z=3;!{}\033\\", body));

    auto scenario = [](MirrorHarness* h, vtworkspace::SessionId session) -> Task<void> {
        co_await waitUntil(&h->loop, [&] {
            auto const& row = h->mirror->primaryScreen().grid().lineAt(vtbackend::LineOffset(0)).storage();
            return row.imageFragments.has_value() && row.imageFragments->contains(0)
                   && row.imageFragments->contains(1);
        });

        auto const& serverRow =
            h->serverTerminal(session)->primaryScreen().grid().lineAt(vtbackend::LineOffset(0)).storage();
        auto const& mirrorRow = h->mirror->primaryScreen().grid().lineAt(vtbackend::LineOffset(0)).storage();
        REQUIRE(serverRow.imageFragments.has_value());
        REQUIRE(mirrorRow.imageFragments.has_value());
        // Every cell the server covered with the image is covered in the mirror.
        for (auto const& [column, fragment]: *serverRow.imageFragments)
        {
            std::ignore = fragment;
            CHECK(mirrorRow.imageFragments->contains(column));
        }

        h->client->detach();
    }(&h, session);

    h.loop.blockOn(drive(&h, std::move(scenario)));
}

TEST_CASE("a live window-title change reaches the mirror", "[vthost][mirror]")
{
    auto h = MirrorHarness {};
    h.host.createTab();
    auto const session = h.host.model().window(h.host.windowId())->activeTab()->rootPane()->session();
    h.serverTerminal(session)->writeToScreen("work");

    auto scenario = [](MirrorHarness* h, vtworkspace::SessionId session) -> Task<void> {
        co_await waitUntil(&h->loop, [&] {
            return h->mirror->primaryScreen().grid().renderMainPageText().contains("work");
        });

        // Change ONLY the title (OSC 2, no cell change) — the title-only delta must
        // still push and the mirror must re-title.
        serverWrites(h, session, "\033]2;my-title\033\\");
        co_await waitUntil(&h->loop, [&] { return h->mirror->windowTitle() == "my-title"; });
        CHECK(h->mirror->windowTitle() == "my-title");
        CHECK(h->serverTerminal(session)->windowTitle() == "my-title");

        h->client->detach();
    }(&h, session);

    h.loop.blockOn(drive(&h, std::move(scenario)));
}

TEST_CASE("bell, notification and clipboard events reach the mirror", "[vthost][mirror]")
{
    auto h = MirrorHarness {};
    h.host.createTab();
    auto const session = h.host.model().window(h.host.windowId())->activeTab()->rootPane()->session();
    h.serverTerminal(session)->writeToScreen("ready");

    auto scenario = [](MirrorHarness* h, vtworkspace::SessionId session) -> Task<void> {
        co_await waitUntil(&h->loop, [&] {
            return h->mirror->primaryScreen().grid().renderMainPageText().contains("ready");
        });

        // Bell (BEL) — a transient event, re-emitted as BEL into the mirror.
        serverWrites(h, session, "\a");
        co_await waitUntil(&h->loop, [&] { return h->mirrorEvents.bells > 0; });
        CHECK(h->mirrorEvents.bells > 0);

        // Desktop notification (OSC 777 notify;title;body) → mirror's notify().
        serverWrites(h, session, "\033]777;notify;Build;done ok\033\\");
        co_await waitUntil(&h->loop, [&] { return h->mirrorEvents.notifyTitle == "Build"; });
        CHECK(h->mirrorEvents.notifyBody == "done ok");

        // Clipboard write (OSC 52) → mirror's copyToClipboard() with decoded text.
        auto const encoded = crispy::base64::encode(std::string_view { "clip-text" });
        serverWrites(h, session, std::format("\033]52;c;{}\033\\", encoded));
        co_await waitUntil(&h->loop, [&] { return h->mirrorEvents.clipboard == "clip-text"; });
        CHECK(h->mirrorEvents.clipboard == "clip-text");

        h->client->detach();
    }(&h, session);

    h.loop.blockOn(drive(&h, std::move(scenario)));
}

TEST_CASE("a live cursor-shape change reaches the mirror", "[vthost][mirror]")
{
    auto h = MirrorHarness {};
    h.host.createTab();
    auto const session = h.host.model().window(h.host.windowId())->activeTab()->rootPane()->session();
    h.serverTerminal(session)->writeToScreen("x");

    auto scenario = [](MirrorHarness* h, vtworkspace::SessionId session) -> Task<void> {
        co_await waitUntil(
            &h->loop, [&] { return h->mirror->primaryScreen().grid().renderMainPageText().contains("x"); });

        // DECSCUSR steady bar (Ps=6): a cursor-shape-only change must push and
        // re-shape the mirror's cursor.
        serverWrites(h, session, "\033[6 q");
        co_await waitUntil(&h->loop, [&] { return h->mirror->cursorShape() == vtbackend::CursorShape::Bar; });
        CHECK(h->mirror->cursorShape() == vtbackend::CursorShape::Bar);
        CHECK(h->serverTerminal(session)->cursorShape() == vtbackend::CursorShape::Bar);

        h->client->detach();
    }(&h, session);

    h.loop.blockOn(drive(&h, std::move(scenario)));
}

TEST_CASE("a live working-directory change reaches the mirror", "[vthost][mirror]")
{
    auto h = MirrorHarness {};
    h.host.createTab();
    auto const session = h.host.model().window(h.host.windowId())->activeTab()->rootPane()->session();
    h.serverTerminal(session)->writeToScreen("y");

    auto scenario = [](MirrorHarness* h, vtworkspace::SessionId session) -> Task<void> {
        co_await waitUntil(
            &h->loop, [&] { return h->mirror->primaryScreen().grid().renderMainPageText().contains("y"); });

        // OSC 7: a cwd-only change must push and reach the mirror terminal's
        // currentWorkingDirectory (which the GUI queries for split-in-same-dir).
        serverWrites(h, session, "\033]7;file:///home/user/project\033\\");
        co_await waitUntil(
            &h->loop, [&] { return h->mirror->currentWorkingDirectory() == "file:///home/user/project"; });
        CHECK(h->mirror->currentWorkingDirectory() == "file:///home/user/project");
        CHECK(h->serverTerminal(session)->currentWorkingDirectory() == "file:///home/user/project");

        h->client->detach();
    }(&h, session);

    h.loop.blockOn(drive(&h, std::move(scenario)));
}

TEST_CASE("a live default-color change reaches the mirror", "[vthost][mirror]")
{
    auto h = MirrorHarness {};
    h.host.createTab();
    auto const session = h.host.model().window(h.host.windowId())->activeTab()->rootPane()->session();
    h.serverTerminal(session)->writeToScreen("z");

    auto scenario = [](MirrorHarness* h, vtworkspace::SessionId session) -> Task<void> {
        co_await waitUntil(
            &h->loop, [&] { return h->mirror->primaryScreen().grid().renderMainPageText().contains("z"); });

        // OSC 10/11: change the default foreground/background (no cell change).
        serverWrites(h, session, "\033]10;rgb:12/34/56\033\\\033]11;rgb:ab/cd/ef\033\\");
        co_await waitUntil(&h->loop,
                           [&] { return h->mirror->colorPalette().defaultForeground.value() == 0x123456; });
        CHECK(h->mirror->colorPalette().defaultForeground.value() == 0x123456);
        CHECK(h->mirror->colorPalette().defaultBackground.value() == 0xABCDEF);
        // The server saw the same change.
        CHECK(h->serverTerminal(session)->colorPalette().defaultForeground.value() == 0x123456);

        h->client->detach();
    }(&h, session);

    h.loop.blockOn(drive(&h, std::move(scenario)));
}

TEST_CASE("the status-display state reaches the mirror", "[vthost][mirror]")
{
    auto h = MirrorHarness {};
    h.host.createTab();
    auto const session = h.host.model().window(h.host.windowId())->activeTab()->rootPane()->session();
    h.serverTerminal(session)->writeToScreen("s");

    auto scenario = [](MirrorHarness* h, vtworkspace::SessionId session) -> Task<void> {
        co_await waitUntil(
            &h->loop, [&] { return h->mirror->primaryScreen().grid().renderMainPageText().contains("s"); });

        // DECSSDT 1: show the indicator status line (no change to the main grid) —
        // the first slice of multi-page support beyond primary/alternate.
        serverWrites(h, session, "\033[1$~");
        co_await waitUntil(&h->loop, [&] {
            return h->mirror->statusDisplayType() == vtbackend::StatusDisplayType::Indicator;
        });
        CHECK(h->mirror->statusDisplayType() == vtbackend::StatusDisplayType::Indicator);
        CHECK(h->serverTerminal(session)->statusDisplayType() == vtbackend::StatusDisplayType::Indicator);

        h->client->detach();
    }(&h, session);

    h.loop.blockOn(drive(&h, std::move(scenario)));
}

TEST_CASE("a pushed then popped status display round-trips through the mirror", "[vthost][mirror]")
{
    auto h = MirrorHarness {};
    h.host.createTab();
    auto const session = h.host.model().window(h.host.windowId())->activeTab()->rootPane()->session();
    h.serverTerminal(session)->writeToScreen("s");

    auto scenario = [](MirrorHarness* h, vtworkspace::SessionId session) -> Task<void> {
        co_await waitUntil(
            &h->loop, [&] { return h->mirror->primaryScreen().grid().renderMainPageText().contains("s"); });
        REQUIRE(h->mirror->statusDisplayType() == vtbackend::StatusDisplayType::None);

        // KAM set (CSI 2 h) PUSHES the indicator status display onto the server's
        // save/restore stack; KAM reset (CSI 2 l) POPS it back. The stack is
        // server-side — the mirror only tracks the EFFECTIVE type (pull+diff), so a
        // push shows the indicator and a pop restores what was displayed before.
        serverWrites(h, session, "\033[2h"); // KAM on -> pushStatusDisplay(Indicator)
        co_await waitUntil(&h->loop, [&] {
            return h->mirror->statusDisplayType() == vtbackend::StatusDisplayType::Indicator;
        });
        CHECK(h->mirror->statusDisplayType() == vtbackend::StatusDisplayType::Indicator);

        serverWrites(h, session, "\033[2l"); // KAM off -> popStatusDisplay()
        co_await waitUntil(
            &h->loop, [&] { return h->mirror->statusDisplayType() == vtbackend::StatusDisplayType::None; });
        CHECK(h->mirror->statusDisplayType() == vtbackend::StatusDisplayType::None);
        CHECK(h->serverTerminal(session)->statusDisplayType() == vtbackend::StatusDisplayType::None);

        h->client->detach();
    }(&h, session);

    h.loop.blockOn(drive(&h, std::move(scenario)));
}

TEST_CASE("host-writable status-line content reaches the mirror", "[vthost][mirror]")
{
    auto h = MirrorHarness {};
    h.host.createTab();
    auto const session = h.host.model().window(h.host.windowId())->activeTab()->rootPane()->session();
    h.serverTerminal(session)->writeToScreen("body");

    auto scenario = [](MirrorHarness* h, vtworkspace::SessionId session) -> Task<void> {
        co_await waitUntil(&h->loop, [&] {
            return h->mirror->primaryScreen().grid().renderMainPageText().contains("body");
        });

        // Show the host-writable status line (DECSSDT 2), switch writes to it
        // (DECSASD 1), write custom content, switch back to the main display
        // (DECSASD 0). The app's status-line text must reach the mirror's status page.
        serverWrites(h, session, "\033[2$~\033[1$}STATUSBAR\033[0$}");
        // Sanity: the server itself put the content on its host-writable status line.
        REQUIRE(h->serverTerminal(session)->statusDisplayType()
                == vtbackend::StatusDisplayType::HostWritable);
        REQUIRE(h->serverTerminal(session)
                    ->hostWritableStatusLineDisplay()
                    .grid()
                    .lineText(vtbackend::LineOffset(0))
                    .contains("STATUSBAR"));
        co_await waitUntil(&h->loop, [&] {
            return h->mirror->hostWritableStatusLineDisplay()
                .grid()
                .lineText(vtbackend::LineOffset(0))
                .contains("STATUSBAR");
        });
        CHECK(h->mirror->hostWritableStatusLineDisplay()
                  .grid()
                  .lineText(vtbackend::LineOffset(0))
                  .contains("STATUSBAR"));
        // The main grid is untouched.
        CHECK(h->mirror->primaryScreen().grid().renderMainPageText().contains("body"));

        h->client->detach();
    }(&h, session);

    h.loop.blockOn(drive(&h, std::move(scenario)));
}

// ---------------------------------------------------------------------------------------------
// The thin-vs-fat parity oracle.
//
// Daemon mode is only complete when a pane holds the same grid whether its session lives in
// this process or in the daemon. These cases drive one content shape each through the REAL
// server + client + mirror loop and compare the two grids field by field, so a gap is named
// rather than noticed.
// ---------------------------------------------------------------------------------------------

namespace
{
/// Drives @p bytes through the loop, then records every parity gap into @p out.
///
/// A plain coroutine taking everything BY VALUE rather than a capturing lambda: a coroutine
/// frame outlives the full-expression that created it, so a `[&]` capture of the caller's
/// locals dangles the moment it first suspends (ASan calls it stack-use-after-scope).
/// Parameters are copied into the frame; captures are not.
/// @param h The harness whose server terminal receives @p bytes.
/// @param session The hosted session.
/// @param bytes What the "application" writes.
/// @param settle Holds once the mirror has caught up.
/// @param out Receives the gaps; must outlive the loop run.
Task<void> collectGaps(MirrorHarness* h,
                       vtworkspace::SessionId session,
                       std::string bytes,
                       std::function<bool()> settle,
                       vthost::testing::ComparedPage page,
                       std::vector<vthost::testing::ParityGap>* out)
{
    serverWrites(h, session, bytes);
    co_await waitUntil(&h->loop, std::move(settle));
    // Then let the two grids converge, judged by the comparison itself — cheaply, stopping at the
    // first divergence. A settle needle only proves the TEXT arrived, and not every field travels
    // with the cells whose text it matched: an inline image lands in the mirror's per-cell side
    // table a frame later, so comparing the instant the needle holds races that delivery. Native
    // speed happened to win that race and valgrind's slowdown lost it, which is a property of the
    // machine rather than of the mirror. If the two never agree the wait simply times out and the
    // full report below says why, so this cannot hide a gap. @see collectGapsAfterResize, which
    // has always waited this way for the same reason.
    co_await waitUntil(&h->loop, [h, session, page] {
        return vthost::testing::compareGrids(*h->serverTerminal(session), *h->mirror, page, 1).empty();
    });
    *out = vthost::testing::compareGrids(*h->serverTerminal(session), *h->mirror, page);
    h->client->detach();
}

/// Runs @p bytes through the whole server → client → mirror loop and returns the gaps.
///
/// Pass an EMPTY @p bytes to judge the attach SNAPSHOT instead of the incremental path: whatever
/// the server was written with before `blockOn` is already in its grid, and the empty write only
/// kicks the debounce. The snapshot rebuilds the mirror's grid by an entirely different route
/// (ScreenMirror::fullReplay, streaming history through the page), so that spelling is the only
/// parity coverage it gets.
std::vector<vthost::testing::ParityGap> gapsAfter(
    MirrorHarness* h,
    vtworkspace::SessionId session,
    std::string bytes,
    std::function<bool()> settle,
    vthost::testing::ComparedPage page = vthost::testing::ComparedPage::Active)
{
    auto gaps = std::vector<vthost::testing::ParityGap> {};
    h->loop.blockOn(drive(h, collectGaps(h, session, std::move(bytes), std::move(settle), page, &gaps)));
    return gaps;
}

/// Asserts a thin pane's grid is INDISTINGUISHABLE from what the fat GUI would hold.
///
/// This replaced a ratchet that pinned each shape's known divergence set. The ratchet existed
/// because four of the seven shapes below could not reach parity while the client rendered from
/// re-serialized escape sequences — `LineFlag::Wrapped` alone has no spelling in that vocabulary.
/// Populating the grid directly closed all four at once, so there is nothing left to pin, and the
/// assertion can be the real one: no gaps at all.
///
/// Should a future shape diverge, do not reintroduce a per-shape allowance without saying why in
/// the test — an allowed gap is a feature that is silently missing in daemon mode.
/// @param gaps What compareGrids reported.
void checkParity(std::vector<vthost::testing::ParityGap> const& gaps)
{
    INFO("parity gaps:\n"
         << vthost::testing::describeGaps(gaps) << "\nBy field:\n"
         << vthost::testing::summarizeGaps(gaps));
    CHECK(gaps.empty());
}

/// @p count SGR-coloured "row-N" lines followed by "last", for the scrollback parity probes.
///
/// The braces are load-bearing: the SGR and the text are appended separately so the spell gate does
/// not read the SGR terminator and the word as one token, and without them the second append would
/// fall outside the loop.
/// @param count How many rows to emit before the "last" tail.
std::string colouredRows(int count)
{
    auto scrolled = std::string {};
    for (auto const i: std::views::iota(0, count))
    {
        scrolled += std::format("\033[3{}m", i % 8);
        scrolled += std::format("row-{}\033[m\r\n", i);
    }
    scrolled += "last";
    return scrolled;
}

/// @return True once @p needle shows on the mirror's page.
std::function<bool()> mirrorShows(MirrorHarness* h, std::string needle)
{
    return [h, needle = std::move(needle)] {
        return h->mirror->primaryScreen().grid().renderMainPageText().contains(needle);
    };
}

// The probes below compare two grids, so two grids that are equally EMPTY of the thing under test
// pass trivially — and a probe that never engaged its feature looks exactly like one that proved
// parity for it. Each new shape therefore states what the SERVER must be holding, so the probe
// fails loudly if the escape sequence it drives ever stops doing what it was written for.

/// @return The line flags the server's row @p row carries.
[[nodiscard]] vtbackend::LineFlags serverLineFlags(MirrorHarness* h, vtworkspace::SessionId session, int row)
{
    auto const& grid = h->serverTerminal(session)->primaryScreen().grid();
    return grid.lineAt(vtbackend::LineOffset(row)).flags();
}

/// @return Whether any cell of the server's active page carries @p flag.
[[nodiscard]] bool serverHasCellFlag(MirrorHarness* h,
                                     vtworkspace::SessionId session,
                                     vtbackend::CellFlag flag)
{
    auto const* terminal = h->serverTerminal(session);
    // The DISPLAYED page, matching what the oracle compares — not the xterm alt page, which is a
    // different grid from DEC pages 1..14. @see GridParity's pageOf.
    auto const& grid = terminal->displayedPage().grid();
    for (auto const row: std::views::iota(0, unbox<int>(grid.pageSize().lines)))
    {
        auto const& line = grid.lineAt(vtbackend::LineOffset(row));
        if (line.isBlank())
            continue;
        for (auto const column: std::views::iota(std::size_t { 0 }, unbox<std::size_t>(line.size())))
            if (line.storage().sgr[column].flags.contains(flag))
                return true;
    }
    return false;
}

/// @return Whether any row of the server's active page carries an image fragment.
[[nodiscard]] bool serverHasImage(MirrorHarness* h, vtworkspace::SessionId session)
{
    auto const& grid = h->serverTerminal(session)->primaryScreen().grid();
    for (auto const row: std::views::iota(0, unbox<int>(grid.pageSize().lines)))
    {
        auto const& fragments = grid.lineAt(vtbackend::LineOffset(row)).storage().imageFragments;
        if (fragments && !fragments->empty())
            return true;
    }
    return false;
}

/// Writes @p bytes, then resizes BOTH ends and records the gaps once they settle.
///
/// This is the case the whole design rests on. Reflow runs twice — once on the daemon's grid, once
/// on the client's — and nothing synchronizes them: the client reflows because its window changed,
/// the daemon because the client told it the new area. They agree only if both grids were faithful
/// copies to begin with, wrap flags included, since reflow is deterministic in
/// (content, flags, old size, new size). That is why this probe exists rather than an argument.
///
/// The order matches production: the pane resizes its own terminal first (reflowing local
/// scrollback), reports the area upstream, and the daemon answers with a snapshot of its own
/// reflowed grid.
/// @param h The harness.
/// @param session The hosted session.
/// @param bytes What the "application" writes before the resize.
/// @param settleNeedle Text whose appearance means the pre-resize content arrived.
/// @param columns The new width.
/// @param lines The new height.
/// @param out Receives the gaps; must outlive the loop run.
Task<void> collectGapsAfterResize(MirrorHarness* h,
                                  vtworkspace::SessionId session,
                                  std::string bytes,
                                  std::string settleNeedle,
                                  uint32_t columns,
                                  uint32_t lines,
                                  std::vector<vthost::testing::ParityGap>* out)
{
    serverWrites(h, session, bytes);
    co_await waitUntil(&h->loop, mirrorShows(h, std::move(settleNeedle)));

    auto const target = vtbackend::PageSize { vtbackend::LineCount::cast_from(lines),
                                              vtbackend::ColumnCount::cast_from(columns) };
    h->mirror->resizeScreen(target);
    h->client->requestResize(columns, lines);

    // Both ends must actually BE at the new size before comparing: an agreement reached before
    // either of them reflowed would read as success while proving nothing.
    co_await waitUntil(&h->loop, [h, session, target] {
        return h->serverTerminal(session)->pageSize() == target && h->mirror->pageSize() == target;
    });
    // Then wait for the resize snapshot to land, judged by the comparison itself — cheaply, stopping
    // at the first divergence. If they never agree the wait simply times out and the full report
    // below says why, so this cannot hide a gap; it only avoids guessing at a settling delay.
    co_await waitUntil(&h->loop, [h, session] {
        return vthost::testing::compareGrids(
                   *h->serverTerminal(session), *h->mirror, vthost::testing::ComparedPage::Active, 1)
            .empty();
    });

    *out = vthost::testing::compareGrids(*h->serverTerminal(session), *h->mirror);
    h->client->detach();
}

/// Runs @p bytes through the loop, resizes both ends to @p columns x @p lines, and returns the gaps.
std::vector<vthost::testing::ParityGap> gapsAfterResize(MirrorHarness* h,
                                                        vtworkspace::SessionId session,
                                                        std::string bytes,
                                                        std::string settleNeedle,
                                                        uint32_t columns,
                                                        uint32_t lines)
{
    auto gaps = std::vector<vthost::testing::ParityGap> {};
    h->loop.blockOn(drive(h,
                          collectGapsAfterResize(
                              h, session, std::move(bytes), std::move(settleNeedle), columns, lines, &gaps)));
    return gaps;
}
} // namespace

TEST_CASE("PARITY plain text and SGR", "[vthost][parity]")
{
    auto h = MirrorHarness {};
    h.host.createTab();
    auto const session = h.host.model().window(h.host.windowId())->activeTab()->rootPane()->session();
    auto const gaps = gapsAfter(&h,
                                session,
                                "\033[1;4;31mbold underline red\033[m plain\r\n"
                                "\033[48;5;27m"
                                "bg\033[m "
                                "\033[58;5;9m\033[4:3m"
                                "curly\033[m",
                                mirrorShows(&h, "curly"));
    // The curly underline is deliberate. It used to degrade to a plain one, because the mirror
    // reached the client through SGR and the shared SgrFlagCodes table maps CurlyUnderlined,
    // DottedUnderline and DashedUnderline all onto plain `4` (SgrFlagCode holds a single int and
    // cannot spell `4:3`). Populating the grid carries the CellFlag itself, so no SGR spelling has
    // to exist. That table is still wrong for capture-pane, which is a separate fix.
    checkParity(gaps);
}

TEST_CASE("PARITY wide characters and grapheme clusters", "[vthost][parity]")
{
    auto h = MirrorHarness {};
    h.host.createTab();
    auto const session = h.host.model().window(h.host.windowId())->activeTab()->rootPane()->session();
    auto const gaps = gapsAfter(&h, session, "\u4F60\u597D e\u0301 \U0001F600 end", mirrorShows(&h, "end"));
    checkParity(gaps); // width, cluster extras and codepoints all survive
}

TEST_CASE("PARITY a wrapped logical line", "[vthost][parity]")
{
    // The load-bearing case for reflow: whether the mirror knows this row continues the one
    // above it. There is no escape sequence that says so.
    auto h = MirrorHarness {};
    h.host.createTab();
    auto const session = h.host.model().window(h.host.windowId())->activeTab()->rootPane()->session();
    auto const wide = std::string(100, 'w') + "-tail";
    auto const gaps = gapsAfter(&h, session, wide, mirrorShows(&h, "-tail"));
    // `Wrapped` used to be carried on the wire and dropped, because no escape sequence sets it —
    // its only producer in the engine is a real autowrap (Screen::crlfIfWrapPending). The mirror's
    // own reflow therefore worked from wrap state it had invented. This case is why the client
    // populates the grid instead of emitting sequences into it.
    checkParity(gaps);
}

TEST_CASE("PARITY OSC 133 shell-integration marks", "[vthost][parity]")
{
    auto h = MirrorHarness {};
    h.host.createTab();
    auto const session = h.host.model().window(h.host.windowId())->activeTab()->rootPane()->session();
    auto const gaps = gapsAfter(&h,
                                session,
                                "\033]133;A\033\\$ \033]133;B\033\\ls\r\n"
                                "\033]133;C\033\\file-one\r\n\033]133;D;0\033\\done",
                                mirrorShows(&h, "done"));
    // Marked / PromptEnd / OutputStart / CommandEnd, and the two offsets that say WHERE the prompt
    // and the previous command's output stopped. This is what the three shell-integration features
    // the GUI actually has are built on — jump-to-prompt (findMarkerUpwards), the command-block scan
    // (CommandBlocks) and the accessibility prompt span (PromptRegion). All three read the grid,
    // which is why none of them needed a wire surface of its own.
    checkParity(gaps);
}

TEST_CASE("PARITY DECDWL double-width line", "[vthost][parity]")
{
    auto h = MirrorHarness {};
    h.host.createTab();
    auto const session = h.host.model().window(h.host.windowId())->activeTab()->rootPane()->session();
    auto const gaps = gapsAfter(&h, session, "\033#6wide-line", mirrorShows(&h, "wide-line"));
    checkParity(gaps); // DECDWL/DECDHL used to render as ordinary lines
}

TEST_CASE("PARITY DECDHL double-height halves", "[vthost][parity]")
{
    // The two halves are SEPARATE flags on separate rows, and a receiver that carries only
    // DoubleWidth would render both halves as one wide line.
    auto h = MirrorHarness {};
    h.host.createTab();
    auto const session = h.host.model().window(h.host.windowId())->activeTab()->rootPane()->session();
    auto const gaps =
        gapsAfter(&h, session, "\033#3top-half\r\n\033#4bottom-half", mirrorShows(&h, "bottom-half"));
    REQUIRE(serverLineFlags(&h, session, 0).contains(vtbackend::LineFlag::DoubleHeightTop));
    REQUIRE(serverLineFlags(&h, session, 1).contains(vtbackend::LineFlag::DoubleHeightBottom));
    checkParity(gaps);
}

TEST_CASE("PARITY protected cells", "[vthost][parity]")
{
    // TWO protection flags, honoured by OPPOSITE erase families: DECSCA's CharacterProtected is
    // spared by the SELECTIVE erases, SPA/EPA's CharacterProtectedISO by the regular ones. A client
    // holding the wrong one erases exactly the cells the server would keep, so this is a divergence
    // that only shows up later, on the next erase.
    auto h = MirrorHarness {};
    h.host.createTab();
    auto const session = h.host.model().window(h.host.windowId())->activeTab()->rootPane()->session();
    auto const gaps = gapsAfter(&h,
                                session,
                                "\033[1\"q"
                                "decsca-on\033[0\"q plain\r\n"
                                "\033V"
                                "spa-on\033W plain-again",
                                mirrorShows(&h, "plain-again"));
    REQUIRE(serverHasCellFlag(&h, session, vtbackend::CellFlag::CharacterProtected));
    REQUIRE(serverHasCellFlag(&h, session, vtbackend::CellFlag::CharacterProtectedISO));
    checkParity(gaps);
}

TEST_CASE("PARITY the alternate screen", "[vthost][parity]")
{
    // A full-screen app's page is a grid of its own. The comparison follows the ACTIVE screen, so
    // this measures the alternate one — including that the mirror actually switched to it.
    auto h = MirrorHarness {};
    h.host.createTab();
    auto const session = h.host.model().window(h.host.windowId())->activeTab()->rootPane()->session();
    auto const gaps =
        gapsAfter(&h, session, "main-page\r\n\033[?1049h\033[1;1H\033[7malt-inverse\033[m alt-plain", [&h] {
            return h.mirror->isAlternateScreen()
                   && h.mirror->alternateScreen().grid().renderMainPageText().contains("alt-plain");
        });
    REQUIRE(h.serverTerminal(session)->isAlternateScreen());
    REQUIRE(serverHasCellFlag(&h, session, vtbackend::CellFlag::Inverse));
    checkParity(gaps);
}

TEST_CASE("PARITY the host-writable status line", "[vthost][parity]")
{
    // DECSSDT 2 gives the app a second page. Its rows come off the same toWireLine as page rows and
    // are trimmed the same way, so a receiver that skips the fill loses a coloured status bar's
    // colour past its last glyph.
    auto h = MirrorHarness {};
    h.host.createTab();
    auto const session = h.host.model().window(h.host.windowId())->activeTab()->rootPane()->session();
    auto const statusShows = [&h] {
        return h.mirror->hostWritableStatusLineDisplay()
            .grid()
            .lineText(vtbackend::LineOffset(0))
            .contains("STATUS");
    };
    auto const gaps = gapsAfter(&h,
                                session,
                                "body\033[2$~\033[1$}\033[44;33mSTATUS\033[m\033[0$}",
                                statusShows,
                                vthost::testing::ComparedPage::HostWritableStatus);
    // The status page really carries the bar, colour and all — otherwise this compares two blank
    // pages and calls it parity.
    auto const& statusRow =
        h.serverTerminal(session)->hostWritableStatusLineDisplay().grid().lineAt(vtbackend::LineOffset(0));
    REQUIRE(statusRow.toUtf8Trimmed().contains("STATUS"));
    REQUIRE(statusRow.storage().sgr[0].backgroundColor != vtbackend::Color {});
    checkParity(gaps);
}

TEST_CASE("PARITY OSC 66 scaled text blocks", "[vthost][parity]")
{
    // A scaled block is the only thing in the grid taller than one row: its head carries the scale
    // and the rows BELOW carry MulticellContinuation cells. Both halves have to survive, or the
    // block renders as plain text or leaves orphaned continuations behind.
    auto h = MirrorHarness {};
    h.host.createTab();
    auto const session = h.host.model().window(h.host.windowId())->activeTab()->rootPane()->session();
    auto const gaps =
        gapsAfter(&h, session, "\033]66;s=3:n=1:d=2:v=1:h=1;Scaled\033\\ after", mirrorShows(&h, "after"));
    REQUIRE(serverHasCellFlag(&h, session, vtbackend::CellFlag::MulticellContinuation));
    checkParity(gaps);
}

TEST_CASE("PARITY an inline image", "[vthost][parity]")
{
    // Images live in a per-cell side table, not in the cells, and their placement is reconstructed
    // from the covered cells' offsets plus the alignment and resize policies. Those two policies were
    // absent from the wire until this work; without them the receiver rasterizes a differently
    // cropped image over the right cells.
    auto h = MirrorHarness {};
    h.host.createTab();
    auto const session = h.host.model().window(h.host.windowId())->activeTab()->rootPane()->session();
    // A 2x2 RGB image (GIP), then text below it so the settle condition is observable.
    auto const pixels = std::string { "\xff\x00\x00\x00\xff\x00\x00\x00\xff\xff\xff\xff", 12 };
    auto const gaps = gapsAfter(&h,
                                session,
                                // a=1 (TopStart) and z=3 (StretchToFill) are both NON-default, so
                                // the two policy fields are load-bearing here: a receiver that
                                // assumed the defaults would crop and stretch the source differently
                                // over the very same cells, and the probe would catch it.
                                std::format("\033P!go=u,n=parityImage,f=2,w=2,h=2;!{}\033\\"
                                            "\033P!go=r,n=parityImage,c=2,r=2,a=1,z=3\033\\"
                                            "\033[5;1H"
                                            "below-image",
                                            crispy::base64::encode(pixels)),
                                mirrorShows(&h, "below-image"));
    REQUIRE(serverHasImage(&h, session));
    checkParity(gaps);
}

TEST_CASE("PARITY a DEC page the user is not looking at", "[vthost][parity]")
{
    // Only the DISPLAYED page travels, so an off-screen page must not bleed into what the mirror
    // holds — and the displayed one must survive output landing elsewhere. DECPCCM off decouples the
    // display from the cursor, then NP moves VT output to page 1 while the user still sees page 0.
    auto h = MirrorHarness {};
    h.host.createTab();
    auto const session = h.host.model().window(h.host.windowId())->activeTab()->rootPane()->session();
    auto const gaps = gapsAfter(&h,
                                session,
                                "\033[41m"
                                "visible-page-zero\033[m"
                                "\033[?64l\033[1U\033[32m"
                                "hidden-on-page-one\033[m",
                                [&h] {
                                    return !h.mirror->isModeEnabled(vtbackend::DECMode::VisibleCursor)
                                           && h.mirror->primaryScreen().grid().renderMainPageText().contains(
                                               "visible-page-zero");
                                });
    // The other page really did receive the write, so "no bleed" is a claim about something.
    REQUIRE(h.serverTerminal(session)->currentScreen().grid().renderMainPageText().contains(
        "hidden-on-page-one"));
    checkParity(gaps);
}

TEST_CASE("PARITY scrollback after content scrolled out", "[vthost][parity]")
{
    auto h = MirrorHarness {};
    h.host.createTab();
    auto const session = h.host.model().window(h.host.windowId())->activeTab()->rootPane()->session();
    auto const gaps = gapsAfter(&h, session, colouredRows(40), mirrorShows(&h, "last"));
    checkParity(gaps); // scrolled-out rows keep their text and colour
}

TEST_CASE("PARITY scrollback delivered by the attach snapshot", "[vthost][parity]")
{
    // The same shape as the probe above, but produced BEFORE anyone attached. The rows reach the
    // mirror through fullReplay's history streaming rather than through scrollInRow, so this is
    // the only parity probe covering that route — and the route the user documentation used to
    // say did not exist.
    auto h = MirrorHarness {};
    h.host.createTab();
    auto const session = h.host.model().window(h.host.windowId())->activeTab()->rootPane()->session();
    // Written before blockOn: the client is not running yet, so this is history it never watched.
    // The empty `bytes` below is what makes this judge the snapshot rather than a delta.
    h.serverTerminal(session)->writeToScreen(colouredRows(40));
    auto const gaps = gapsAfter(&h, session, "", mirrorShows(&h, "last"));
    checkParity(gaps); // text, colour and wrap flags survive the snapshot, history included
}

TEST_CASE("PARITY wrapped content reflows alike when narrowed", "[vthost][parity]")
{
    // Narrowing SPLITS logical lines: the daemon's grid grows rows, and so must the client's. Both
    // run vtbackend's own reflow over their own copy, so this measures whether the copy was faithful
    // enough for two independent reflows to land on the same grid.
    auto h = MirrorHarness {};
    h.host.createTab();
    auto const session = h.host.model().window(h.host.windowId())->activeTab()->rootPane()->session();
    auto const wide = std::string(100, 'w') + "-tail";
    auto const gaps = gapsAfterResize(&h, session, wide, "-tail", 40, 25);
    // The content really is one logical line spanning several rows; without that this narrows two
    // grids of unwrapped text and learns nothing about reflow.
    REQUIRE(serverLineFlags(&h, session, 1).contains(vtbackend::LineFlag::Wrapped));
    checkParity(gaps);
}

TEST_CASE("PARITY wrapped content reflows alike when widened", "[vthost][parity]")
{
    // The other direction, and the harder one: widening REJOINS a logical line, which is only
    // possible for a receiver that knows which rows were continuations. It is the case that could
    // not work at all while `Wrapped` was dropped on the floor.
    auto h = MirrorHarness {};
    h.host.createTab();
    auto const session = h.host.model().window(h.host.windowId())->activeTab()->rootPane()->session();
    auto const wide = std::string(100, 'w') + "-tail";
    auto const gaps = gapsAfterResize(&h, session, wide, "-tail", 120, 25);
    // Widening REJOINED the split rows: what was two wrapped rows is now one unwrapped line. That is
    // the state the assertion is about, and it is only reachable from correct wrap flags.
    REQUIRE_FALSE(serverLineFlags(&h, session, 1).contains(vtbackend::LineFlag::Wrapped));
    checkParity(gaps);
}

TEST_CASE("PARITY hyperlinks", "[vthost][parity]")
{
    auto h = MirrorHarness {};
    h.host.createTab();
    auto const session = h.host.model().window(h.host.windowId())->activeTab()->rootPane()->session();
    auto const gaps = gapsAfter(
        &h, session, "\033]8;;https://example.com/a\033\\link\033]8;;\033\\ tail", mirrorShows(&h, "tail"));
    checkParity(gaps); // compared by URI, since ids are per-terminal counters
}

TEST_CASE("a blank row's whole fill rendition travels, and costs no cells", "[vthost][mirror]")
{
    // The fill names the row's WHOLE pen (@see GridWire.h), so a row erased under a rendition the
    // two colours alone cannot describe — `\e[7m\e[2J`, `\e[4:3m\e[K` — is still omitted rather
    // than materialized. That matters precisely where the omission earns its keep: a full-screen
    // clear over a deep scrollback would otherwise pay a cell per column of every history row.
    auto grid = vtbackend::Grid { vtbackend::PageSize { vtbackend::LineCount(1), vtbackend::ColumnCount(4) },
                                  false,
                                  vtbackend::LineCount(0) };
    auto const row = vtbackend::LineOffset(0);

    auto const fill = vtbackend::GraphicsAttributes {
        .backgroundColor = vtbackend::IndexedColor::Red,
        .underlineColor = vtbackend::IndexedColor::Blue,
        .flags = vtbackend::CellFlags { vtbackend::CellFlag::Inverse, vtbackend::CellFlag::CurlyUnderlined }
    };
    grid.lineAt(row).reset(vtbackend::LineFlags {}, fill);
    REQUIRE(grid.lineAt(row).isBlank());

    auto const wire = vthost::toWireLine(grid, row, grid.lineAt(row));
    CHECK(wire.cells.empty()); // nothing to send: the fill describes the row exactly
    CHECK(wire.fillBackground == vthost::rawColor(vtbackend::Color { vtbackend::IndexedColor::Red }));
    CHECK(wire.fillUnderlineColor == vthost::rawColor(vtbackend::Color { vtbackend::IndexedColor::Blue }));
    CHECK(wire.fillFlags == static_cast<uint32_t>(fill.flags.value()));

    // And what a receiver reconstructs for those absent columns wears all of it — the contract the
    // omission rests on.
    auto const expanded = vthost::expandToFullWidth(wire);
    REQUIRE(expanded.size() == 4);
    CHECK(expanded.front() == vthost::wireCellOf(fill));
    CHECK(expanded.back() == expanded.front());
}

TEST_CASE("a trailing run equal to a flag-carrying fill is still trimmed", "[vthost][mirror]")
{
    // The other half of the same regression: a NON-blank row wearing such a fill used to find no
    // trailing run equal to fillCellOf and travel whole, because fillCellOf could not spell the
    // flags the row was actually filled with.
    auto grid = vtbackend::Grid { vtbackend::PageSize { vtbackend::LineCount(1), vtbackend::ColumnCount(8) },
                                  false,
                                  vtbackend::LineCount(0) };
    auto const row = vtbackend::LineOffset(0);
    auto const fill =
        vtbackend::GraphicsAttributes { .flags = vtbackend::CellFlags { vtbackend::CellFlag::Inverse } };
    grid.lineAt(row).reset(vtbackend::LineFlags {}, fill);
    grid.lineAt(row).useCellAt(vtbackend::ColumnOffset(0)).write(fill, U'x', 1);

    auto const wire = vthost::toWireLine(grid, row, grid.lineAt(row));
    CHECK(wire.cells.size() == 1); // only the written column; the rest is the fill
    CHECK(vthost::expandToFullWidth(wire).size() == 8);
}

// A row the server changed IN PLACE in its scrollback (the OSC 133 case: a shell's semantic marks
// land on a logical line's HEAD, which for a wrapped prompt has already scrolled off the page) must
// reach the mirror's own history. Bounding the incremental path at the old viewport top dropped
// those rows silently — they were stored in RemoteScreen::rows and never written anywhere — so
// prompt navigation and semantic-block selection answered from stale marks in attach mode while the
// same session in a local window behaved correctly.
TEST_CASE("an in-place scrollback change reaches the mirror's history", "[vthost][mirror]")
{
    auto bare = BareMirror { vtbackend::LineCount(100) };
    auto screen = vthost::client::RemoteScreen {};
    screen.columns = 5;
    screen.lines = 1;

    // Rows 7..10 with a one-line page: 7, 8 and 9 land in the mirror's own scrollback.
    auto seed = proto::Delta {};
    seed.snapshot = 1;
    seed.stableViewportBase = 10;
    seed.stableFloor = 7;
    for (auto const id: std::views::iota(int64_t { 7 }, int64_t { 11 }))
        seed.lines.push_back(rowAt(id, std::format("old{}", id % 10)));
    screen.apply(seed);
    bare.mirror->apply(screen, seed);
    REQUIRE(bare.historyLines() == 3);
    REQUIRE(bare.terminal->primaryScreen().grid().lineText(vtbackend::LineOffset(-2)).starts_with("old8"));

    // An ordinary incremental delta — nothing scrolled, the viewport did not move — that reports
    // history row 8 changed.
    auto touched = proto::Delta {};
    touched.stableViewportBase = 10;
    touched.stableFloor = 7;
    touched.lines.push_back(rowAt(8, "NEW8"));
    screen.apply(touched);
    bare.mirror->apply(screen, touched);

    // Applied at its own offset, without disturbing its neighbours or the page.
    CHECK(bare.historyLines() == 3);
    auto const& grid = bare.terminal->primaryScreen().grid();
    CHECK(grid.lineText(vtbackend::LineOffset(-2)).starts_with("NEW8"));
    CHECK(grid.lineText(vtbackend::LineOffset(-3)).starts_with("old7"));
    CHECK(grid.lineText(vtbackend::LineOffset(-1)).starts_with("old9"));
    CHECK(grid.lineText(vtbackend::LineOffset(0)).starts_with("old0"));
}

// The bound on how deep an in-place change may be placed is the mirror's OWN history, and a row it
// has no data for is never blanked: local scrollback is routinely deeper than what the server can
// still name, so clearing a row the server merely stopped naming would destroy history.
TEST_CASE("an in-place change below the mirror's history is skipped, not misplaced", "[vthost][mirror]")
{
    auto bare = BareMirror { vtbackend::LineCount(2) }; // room for two history rows only
    auto screen = vthost::client::RemoteScreen {};
    screen.columns = 5;
    screen.lines = 1;

    auto seed = proto::Delta {};
    seed.snapshot = 1;
    seed.stableViewportBase = 10;
    seed.stableFloor = 5;
    for (auto const id: std::views::iota(int64_t { 5 }, int64_t { 11 }))
        seed.lines.push_back(rowAt(id, std::format("r{}", id)));
    screen.apply(seed);
    bare.mirror->apply(screen, seed);
    REQUIRE(bare.historyLines() == 2); // 8 and 9 survive; 5..7 were evicted locally

    auto const before = bare.terminal->primaryScreen().grid().lineText(vtbackend::LineOffset(-2));

    // Row 6 is far below what the mirror still holds: it must be dropped, never written at a
    // wrapped-around or clamped offset.
    auto touched = proto::Delta {};
    touched.stableViewportBase = 10;
    touched.stableFloor = 5;
    touched.lines.push_back(rowAt(6, "XXXX"));
    screen.apply(touched);
    bare.mirror->apply(screen, touched);

    CHECK(bare.historyLines() == 2);
    CHECK(bare.terminal->primaryScreen().grid().lineText(vtbackend::LineOffset(-2)) == before);
    CHECK_FALSE(bare.terminal->primaryScreen().grid().lineText(vtbackend::LineOffset(-1)).contains("XXXX"));
}

// Every enum-ish wire field is validated before it is cast. An out-of-range StatusDisplayType used
// to be `static_cast` straight into the enum, reaching Terminal::statusLineHeight() whose switch
// ends in crispy::unreachable() — undefined behaviour in the attached client.
TEST_CASE("out-of-range status-display bytes are rejected, not cast", "[vthost][mirror]")
{
    auto bare = BareMirror { vtbackend::LineCount(10) };
    auto const ownType = bare.terminal->statusDisplayType();
    auto const ownActive = bare.terminal->activeStatusDisplay();

    auto screen = vthost::client::RemoteScreen {};
    screen.columns = 5;
    screen.lines = 1;
    screen.statusDisplayType = 3;   // no such enumerator (None/Indicator/HostWritable = 0..2)
    screen.activeStatusDisplay = 9; // no such enumerator (Main/StatusLine/Indicator = 0..2)

    auto seed = proto::Delta {};
    seed.snapshot = 1;
    seed.stableViewportBase = 0;
    seed.lines.push_back(rowAt(0, "hello"));
    screen.apply(seed);
    bare.mirror->apply(screen, seed);

    // Neither value moved the terminal off a state its own switches cover.
    CHECK(bare.terminal->statusDisplayType() == ownType);
    CHECK(bare.terminal->activeStatusDisplay() == ownActive);
    CHECK(bare.terminal->statusLineHeight() == vtbackend::LineCount(0));

    // A LIVE (non-snapshot) delta carrying the same garbage is rejected the same way.
    auto tick = proto::Delta {};
    tick.stableViewportBase = 0;
    tick.statusChanged = 1;
    tick.statusDisplayType = 200;
    tick.activeStatusDisplay = 200;
    screen.apply(tick);
    bare.mirror->apply(screen, tick);
    CHECK(bare.terminal->statusDisplayType() == ownType);
    CHECK(bare.terminal->activeStatusDisplay() == ownActive);
}

// LNM (ANSI mode 20) decides whether Return sends CR or CR LF, and in attach mode the CLIENT
// encodes the keystroke — so a mode set on the daemon that never reaches the mirror leaves the
// hosted application waiting for a line terminator it asked for and will never see.
TEST_CASE("LNM mirrors so the client encodes Return the way the app asked", "[vthost][mirror]")
{
    auto h = MirrorHarness {};
    h.host.createTab();
    auto const session = h.host.model().window(h.host.windowId())->activeTab()->rootPane()->session();

    // Observations are RECORDED and asserted after the loop, never asserted inside the scenario: a
    // failing REQUIRE there throws past the `detach()` that winds the two run loops down, and the
    // whenAll then waits for children that can no longer finish — a failure would hang instead.
    auto lnmInitially = true;
    auto lnmAfterSet = false;
    auto lnmAfterReset = true;

    auto scenario = [](MirrorHarness* h,
                       vtworkspace::SessionId session,
                       bool* initially,
                       bool* afterSet,
                       bool* afterReset) -> Task<void> {
        auto const lnm = [h] {
            return h->mirror->isModeEnabled(vtbackend::AnsiMode::AutomaticNewLine);
        };

        serverWrites(h, session, "x");
        co_await waitUntil(&h->loop, [&] {
            return h->mirror->primaryScreen().grid().lineText(vtbackend::LineOffset(0)).starts_with("x");
        });
        *initially = lnm();

        // The application sets LNM on the daemon.
        serverWrites(h, session, "\033[20h");
        co_await waitUntil(&h->loop, lnm);
        *afterSet = lnm();

        // And resets it again — a mode that returned to its default must travel too.
        serverWrites(h, session, "\033[20l");
        co_await waitUntil(&h->loop, [&] { return !lnm(); });
        *afterReset = lnm();

        h->client->detach();
    };
    h.loop.blockOn(drive(&h, scenario(&h, session, &lnmInitially, &lnmAfterSet, &lnmAfterReset)));

    CHECK_FALSE(lnmInitially);
    CHECK(lnmAfterSet);
    CHECK_FALSE(lnmAfterReset);
}
