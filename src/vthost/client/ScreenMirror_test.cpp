// SPDX-License-Identifier: Apache-2.0
#include <vtbackend/core/StickyHeaderKind.hpp>
#include <vtbackend/core/TextScale.hpp>
#include <vtbackend/grid/CellProxy.hpp>
#include <vtbackend/grid/LineSoA.hpp>
#include <vtbackend/input/vi/HintModeHandler.hpp>
#include <vtbackend/render/RenderBuffer.hpp>
#include <vtbackend/screen/Terminal.hpp>
#include <vtbackend/screen/TerminalClocks.hpp>
#include <vtbackend/shell/CommandBlock.hpp>
#include <vtbackend/shell/StickyHeader.hpp>
#include <vtbackend/testing/TestHelpers.hpp>

#include <vtpty/MockPty.hpp>

#include <core/Base64.hpp>
#include <core/async/WhenAll.hpp>
#include <core/net/EventLoop.hpp>
#include <core/net/IoBackend.hpp>
#include <core/net/testing/InMemoryTransport.hpp>
#include <core/platform/Clock.hpp>
#include <core/platform/NativeFileSystem.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <format>
#include <functional>
#include <limits>
#include <memory>
#include <mutex>
#include <optional>
#include <ranges>
#include <set>
#include <string>
#include <string_view>
#include <tuple>
#include <utility>
#include <vector>

#include <vthost/CommandBlockWire.hpp>
#include <vthost/GridWire.hpp>
#include <vthost/HostIdentity.hpp>
#include <vthost/NativeSession.hpp>
#include <vthost/SessionHost.hpp>
#include <vthost/TimeWire.hpp>
#include <vthost/client/NativeClient.hpp>
#include <vthost/client/ScreenMirror.hpp>
#include <vthost/testing/FakeExtensionHosting.hpp>
#include <vthost/testing/GridParity.hpp>
#include <vthost/testing/TestHelpers.hpp>
#include <vtworkspace/Pane.hpp>
#include <vtworkspace/Tab.hpp>

using core::async::Task;
using vtbackend::test::commandEnd;
using vtbackend::test::commandRun;
using vtbackend::test::recordIds;
using vtbackend::test::reportedCommandStart;
using vthost::NativeSession;
using vthost::SessionHost;
using vthost::client::NativeClient;
using vthost::client::ScreenMirror;
using vthost::testing::finishedWire;
using vthost::testing::newSession;
using vthost::testing::promptingWire;
using vthost::testing::runningWire;
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
vtbackend::Settings gipSettings(vtbackend::HistoryLimits history)
{
    auto settings = vtbackend::Settings {};
    settings.goodImageProtocol = true;
    settings.historyLimits = history;
    return settings;
}

/// @param history The scrollback depth the terminal keeps, guaranteed and capacity alike.
vtbackend::Settings gipSettings(vtbackend::LineCount history)
{
    return gipSettings(vtbackend::HistoryLimits::plain(history));
}

/// A mirror-terminal event sink recording the transient events (bell,
/// notification, clipboard write) the mirror is expected to reproduce.
struct RecordingEvents final: vtbackend::Terminal::NullEvents
{
    int bells = 0;
    std::string notifyTitle;
    std::string notifyBody;
    std::vector<vtbackend::DesktopNotification> shownNotifications; ///< OSC 99 notifications, in order.
    std::string clipboard;
    std::vector<vtbackend::CommandBlockSummary> finishedBlocks;
    vtbackend::HostIdentity identity; ///< The machine the mirror terminal says it runs on.
    /// What new output does to the mirror's view when scrolled back: it stays, as a headless terminal's does.
    vtbackend::OutputFollowing following = vtbackend::OutputFollowing::Stay;

    [[nodiscard]] vtbackend::LocalIdentity localIdentity() const noexcept override { return identity.view(); }
    [[nodiscard]] vtbackend::OutputFollowing outputFollowing() const noexcept override { return following; }
    void bell() override { ++bells; }
    void notify(std::string_view title, std::string_view body) override
    {
        notifyTitle = title;
        notifyBody = body;
    }
    void showDesktopNotification(vtbackend::DesktopNotification const& notification) override
    {
        shownNotifications.push_back(notification);
    }
    void copyToClipboard(std::string_view data) override { clipboard = data; }
    int screenUpdates = 0; ///< Frames that brought output.
    int screenReplays = 0; ///< Frames that repainted what the mirror showed already.
    void screenUpdated() override { ++screenUpdates; }
    void screenReplayed() override { ++screenReplays; }
    void commandBlockFinished(vtbackend::CommandBlockSummary const& summary) override
    {
        finishedBlocks.push_back(summary);
    }
    /// What the user asked of the mirror's marks, for the test to send the host as a frontend would.
    std::vector<vtbackend::UserMarkRequest> userMarkRequests;
    void userMarkRequested(vtbackend::UserMarkRequest const& request) override
    {
        userMarkRequests.push_back(request);
    }
};

/// The full closed loop: the REAL server serves deltas of a REAL terminal, the REAL client mirrors
/// them into RemoteScreen, and the ScreenMirror populates a LOCAL mirror terminal from them — whose
/// grid must then equal the server terminal's, which is what the [parity] cases below measure.
struct MirrorHarness
{
    std::unique_ptr<core::net::IoBackend> source = core::net::makeDefaultBackend();
    core::net::EventLoop loop { *source };
    SessionHost host;
    core::net::testing::SocketPair pair = *core::net::testing::makeSocketPair(loop);
    std::unique_ptr<NativeSession> server = std::make_unique<NativeSession>(
        loop, host, vthost::ConnectionId { .endpoint = "test", .index = 1 }, std::move(pair.first));
    std::unique_ptr<NativeClient> client;

    /// Where the daemon runs relative to the mirror: one process, so one machine.
    static constexpr auto Daemon = vthost::DaemonMachine::Same;

    RecordingEvents mirrorEvents;
    std::unique_ptr<vtbackend::Terminal> mirror;
    /// Constructed in the body, not here: it binds to `mirror` for life, so it cannot exist before
    /// the terminal does. @see ScreenMirror's constructor.
    std::unique_ptr<ScreenMirror> populator;

    /// @param mirrorBlockLimits How many command-block records the client keeps: its mirror terminal's store,
    ///        and its NativeClient's tables likewise, as a GUI configures both.
    /// @param serverHistory The hosted sessions' scrollback limits.
    /// @param mirrorHistory The mirror terminal's scrollback depth.
    /// @param extensions The status-line extensions the hosted sessions run; none by default.
    /// @param adjustServer What the hosted sessions' settings differ in; nothing by default.
    explicit MirrorHarness(
        vtbackend::CommandBlockStoreLimits mirrorBlockLimits = {},
        vtbackend::HistoryLimits serverHistory = vtbackend::HistoryLimits::plain(ServerHistoryLines),
        vtbackend::LineCount mirrorHistory = MirrorHistoryLines,
        std::shared_ptr<vthost::IExtensionHosting> extensions = {},
        std::function<void(vtbackend::Settings&)> const& adjustServer = {}):
        host { loop,
               [](vtbackend::PageSize size, std::optional<vtpty::Process::ExecInfo> const&) {
                   return std::make_unique<vtpty::MockPty>(size);
               },
               [&] {
                   auto settings = gipSettings(serverHistory);
                   if (adjustServer)
                       adjustServer(settings);
                   return settings;
               }(),
               core::defaultEnvironment(),
               vthost::SessionHostConfig { .pumping = vthost::SessionPumping::Manual,
                                           .extensions = std::move(extensions) } }
    {
        auto settings = gipSettings(mirrorHistory);
        settings.commandBlockLimits = mirrorBlockLimits;
        mirror = std::make_unique<vtbackend::Terminal>(mirrorEvents,
                                                       core::defaultEnvironment(),
                                                       core::platform::NativeFileSystem::instance(),
                                                       std::make_unique<vtpty::MockPty>(settings.pageSize),
                                                       std::move(settings),
                                                       std::nullopt,
                                                       vtbackend::TerminalClocks::system(),
                                                       vtbackend::randomSemanticBlockTokens());
        populator = std::make_unique<ScreenMirror>(*mirror, Daemon);
        // Construct client after mirror — the handler lambdas capture `this` and
        // access `mirror`, so mirror must be ready first.
        client = std::make_unique<NativeClient>(
            loop,
            std::move(pair.second),
            // ProgramStatus, as the GUI offers it: the set rides only a connection that shares it.
            NativeClient::HandshakeOptions {
                .capabilities = proto::CapabilitySet::of({ proto::Capability::ProgramStatus }) },
            mirrorBlockLimits,
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
        // The session subscribes itself to the host's stream fan-out (bell / notify / clipboard,
        // and screen updates) as its hello is accepted, so the transient-event path
        // (Terminal::Events -> host -> NativeSession) is exercised, not just the manually-poked
        // sessionScreenUpdated.
    }

    ~MirrorHarness() = default;

    MirrorHarness(MirrorHarness const&) = delete;
    MirrorHarness& operator=(MirrorHarness const&) = delete;
    MirrorHarness(MirrorHarness&&) = delete;
    MirrorHarness& operator=(MirrorHarness&&) = delete;

    [[nodiscard]] vtbackend::Terminal* serverTerminal(vtworkspace::SessionId session)
    {
        return host.terminal(session);
    }
};

Task<void> waitUntil(core::net::EventLoop* loop, std::function<bool()> ready)
{
    for (auto i = 0; i < 1000 && !ready(); ++i)
        co_await loop->delay(1ms);
}

/// The longest a scenario may take before the client is detached under it.
constexpr auto ScenarioDeadline = std::chrono::seconds { 30 };

/// Runs @p scenario, detaching the client when it fails: a REQUIRE that throws leaves the scenario before its
/// own detach(), and the client's run() would wait for the daemon forever, with blockOn parked under it.
Task<void> detachOnFailure(MirrorHarness* h, Task<void> scenario, bool* finished)
{
    auto failure = std::exception_ptr {};
    try
    {
        co_await std::move(scenario);
    }
    catch (...)
    {
        failure = std::current_exception();
    }
    *finished = true;
    if (!failure)
        co_return;
    h->client->detach();
    std::rethrow_exception(failure);
}

/// Detaches the client once ScenarioDeadline passes with the scenario still running: the bound on a scenario
/// that awaits something that never comes.
Task<void> scenarioWatchdog(MirrorHarness* h, bool const* finished)
{
    auto const deadline = std::chrono::steady_clock::now() + ScenarioDeadline;
    while (!*finished && std::chrono::steady_clock::now() < deadline)
        co_await h->loop.delay(10ms);
    if (*finished)
        co_return;
    FAIL_CHECK("the scenario did not finish in time");
    h->client->detach();
}

Task<void> drive(MirrorHarness* h, Task<void> scenario)
{
    auto finished = false;
    co_await core::async::whenAll(h->server->run(),
                                  h->client->run(),
                                  detachOnFailure(h, std::move(scenario), &finished),
                                  scenarioWatchdog(h, &finished));
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

/// What a BareMirror is built with.
struct BareMirrorShape
{
    vtbackend::LineCount lines { 1 };                           ///< The page's height.
    vtbackend::ColumnCount columns { 5 };                       ///< The page's width.
    vtbackend::LineCount history { 0 };                         ///< The scrollback the mirror terminal keeps.
    vthost::DaemonMachine daemon = vthost::DaemonMachine::Same; ///< Where the daemon runs, relative to it.
    vtbackend::HostIdentity identity {}; ///< The machine the mirror terminal says it runs on.
    /// The clocks the mirror terminal reads; the system's when unset. Borrowed: they must outlive it.
    std::optional<vtbackend::TerminalClocks> clocks {};
};

/// A bare mirror terminal, five columns wide unless its shape says otherwise, plus the ScreenMirror bound to
/// it, for the unit-level cases that drive the mirror by hand instead of through the full server/client loop.
/// Its events keep what the terminal raises (RecordingEvents), as MirrorHarness's do.
struct BareMirror
{
    RecordingEvents events;
    std::unique_ptr<vtbackend::Terminal> terminal;
    std::unique_ptr<ScreenMirror> mirror;

    /// A 5x1 mirror keeping @p history lines of scrollback.
    explicit BareMirror(vtbackend::LineCount history): BareMirror { BareMirrorShape { .history = history } }
    {
    }

    /// @param shape What it is built with.
    explicit BareMirror(BareMirrorShape shape)
    {
        events.identity = std::move(shape.identity);
        auto settings = vtbackend::Settings {};
        settings.pageSize = vtbackend::PageSize { shape.lines, shape.columns };
        settings.historyLimits = vtbackend::HistoryLimits::plain(shape.history);
        terminal =
            std::make_unique<vtbackend::Terminal>(events,
                                                  core::defaultEnvironment(),
                                                  core::platform::NativeFileSystem::instance(),
                                                  std::make_unique<vtpty::MockPty>(settings.pageSize),
                                                  std::move(settings),
                                                  std::nullopt,
                                                  shape.clocks.value_or(vtbackend::TerminalClocks::system()),
                                                  vtbackend::randomSemanticBlockTokens());
        mirror = std::make_unique<ScreenMirror>(*terminal, shape.daemon);
    }

    [[nodiscard]] int historyLines() const
    {
        return unbox<int>(terminal->primaryScreen().grid().historyLineCount());
    }
};

/// A row @p columns wide at @p stableId, blank but for @p ch at @p column.
proto::WireLine wideRowAt(int64_t stableId, uint32_t columns, uint32_t column, char32_t ch)
{
    auto line = proto::WireLine {};
    line.stableId = stableId;
    line.columns = columns;
    line.cells.resize(column + 1);
    for (auto& cell: line.cells)
        cell.codepoint = U' ';
    line.cells.back().codepoint = ch;
    return line;
}

/// A host grid of 40x20 for the viewport cases, its rows 1..20, holding @p rows, the cursor at @p cursor.
/// @param rows The rows the snapshot carries; the others are blank.
/// @param cursor Where the host's cursor is.
/// @return The delta, applied to @p screen as well.
proto::Delta largeGridSnapshot(vthost::client::RemoteScreen& screen,
                               std::vector<proto::WireLine> rows,
                               vtbackend::CellLocation cursor)
{
    screen.columns = 40;
    screen.lines = 20;
    auto delta = proto::Delta {};
    delta.snapshot = 1;
    delta.generation = 1;
    delta.stableViewportBase = 1;
    delta.stableFloor = 1;
    delta.cursorLine = unbox<int32_t>(cursor.line);
    delta.cursorColumn = unbox<int32_t>(cursor.column);
    delta.lines = std::move(rows);
    screen.apply(delta);
    return delta;
}

/// @return Where the host's cursor at (@p line, @p column) is.
vtbackend::CellLocation cellAt(int line, int column)
{
    return vtbackend::CellLocation { .line = vtbackend::LineOffset(line),
                                     .column = vtbackend::ColumnOffset(column) };
}

} // namespace

TEST_CASE("a mirror smaller than the remote grid shows the cursor's region", "[vthost][mirror][viewport]")
{
    // The host's grid is 40x20 (another client's size); this one shows 10x5 of it. The cursor at the
    // bottom-right corner puts the window at the grid's last 5 rows and last 10 columns.
    auto bare = BareMirror { BareMirrorShape { .lines = vtbackend::LineCount(5),
                                               .columns = vtbackend::ColumnCount(10) } };
    auto screen = vthost::client::RemoteScreen {};
    auto const delta = largeGridSnapshot(screen, { wideRowAt(20, 40, 39, U'X') }, cellAt(19, 39));
    bare.mirror->apply(screen, delta);

    auto const& page = bare.terminal->primaryScreen();
    CHECK(page.grid().lineText(vtbackend::LineOffset(4)) == "         X");
    CHECK(page.cursor().position == cellAt(4, 9));
    CHECK(bare.terminal->reportedCellOrigin() == cellAt(15, 30));
    // Nothing of the host's page above the window went into history.
    CHECK(bare.historyLines() == 0);
}

TEST_CASE("the mirror pans when the cursor moves past the view", "[vthost][mirror][viewport]")
{
    auto bare = BareMirror { BareMirrorShape { .lines = vtbackend::LineCount(5),
                                               .columns = vtbackend::ColumnCount(10) } };
    auto screen = vthost::client::RemoteScreen {};
    auto const first =
        largeGridSnapshot(screen, { wideRowAt(1, 40, 0, U'A'), wideRowAt(20, 40, 39, U'X') }, cellAt(2, 2));
    bare.mirror->apply(screen, first);
    auto const& page = bare.terminal->primaryScreen();
    REQUIRE(bare.terminal->reportedCellOrigin() == vtbackend::CellLocation {});
    REQUIRE(page.grid().lineText(vtbackend::LineOffset(0)).starts_with("A"));
    REQUIRE(page.cursor().position == cellAt(2, 2));

    // The cursor moves to the bottom-right corner in an ordinary delta, no row changed: the page is
    // rewritten.
    auto move = proto::Delta {};
    move.generation = 1;
    move.stableViewportBase = 1;
    move.stableFloor = 1;
    move.cursorLine = 19;
    move.cursorColumn = 39;
    screen.apply(move);
    bare.mirror->apply(screen, move);

    CHECK(bare.terminal->reportedCellOrigin() == cellAt(15, 30));
    CHECK(page.grid().lineText(vtbackend::LineOffset(4)) == "         X");
    CHECK_FALSE(page.grid().lineText(vtbackend::LineOffset(0)).starts_with("A"));
    CHECK(page.cursor().position == cellAt(4, 9));
}

TEST_CASE("a mirror at least as large as the remote grid is not panned", "[vthost][mirror][viewport]")
{
    auto bare = BareMirror { BareMirrorShape { .lines = vtbackend::LineCount(25),
                                               .columns = vtbackend::ColumnCount(50) } };
    auto screen = vthost::client::RemoteScreen {};
    auto const delta = largeGridSnapshot(screen, { wideRowAt(20, 40, 39, U'X') }, cellAt(19, 39));
    bare.mirror->apply(screen, delta);

    auto const& page = bare.terminal->primaryScreen();
    CHECK(bare.terminal->reportedCellOrigin() == vtbackend::CellLocation {});
    CHECK(page.grid().lineText(vtbackend::LineOffset(19)).substr(39, 1) == "X");
    CHECK(page.cursor().position == cellAt(19, 39));
}

TEST_CASE("a restarted mirror applies a new connection's snapshot from scratch", "[vthost][mirror]")
{
    auto bare = BareMirror { BareMirrorShape { .lines = vtbackend::LineCount(1),
                                               .history = vtbackend::LineCount(10) } };

    // The lost connection's screen: generation 5, the page at row 100, two rows of history above it.
    auto lost = vthost::client::RemoteScreen {};
    lost.columns = 5;
    lost.lines = 1;
    auto seed = proto::Delta {};
    seed.snapshot = 1;
    seed.generation = 5;
    seed.stableViewportBase = 100;
    seed.stableFloor = 98;
    seed.lines = { rowAt(98, "a"), rowAt(99, "b"), rowAt(100, "old") };
    lost.apply(seed);
    bare.mirror->apply(lost, seed);
    REQUIRE(bare.terminal->primaryScreen().grid().renderMainPageText().contains("old"));
    REQUIRE(bare.historyLines() == 2);

    // A new connection, to a daemon that restarted: generation 1, its own stable ids from 1.
    bare.mirror->restart();
    auto fresh = vthost::client::RemoteScreen {};
    fresh.columns = 5;
    fresh.lines = 1;
    auto first = proto::Delta {};
    first.snapshot = 1;
    first.generation = 1;
    first.stableViewportBase = 1;
    first.stableFloor = 1;
    first.lines = { rowAt(1, "new") };
    fresh.apply(first);
    bare.mirror->apply(fresh, first);

    CHECK(bare.terminal->primaryScreen().grid().renderMainPageText().contains("new"));
    CHECK_FALSE(bare.terminal->primaryScreen().grid().renderMainPageText().contains("old"));
    CHECK(bare.historyLines() == 0);
}

TEST_CASE("a mirror's replays are repaints, and only its deltas are output", "[vthost][mirror]")
{
    // A replay repaints what the host already showed -- the attach, a ResyncRequired snapshot, a resize --
    // and must not read as news to whatever tracks unseen output (the GUI's tab badge).
    auto bare = BareMirror { vtbackend::LineCount(10) };
    auto screen = vthost::client::RemoteScreen {};
    screen.columns = 5;
    screen.lines = 1;

    SECTION("the first replay into a fresh terminal")
    {
        auto seed = proto::Delta {};
        seed.snapshot = 1;
        seed.stableViewportBase = 1;
        seed.stableFloor = 1;
        seed.lines = { rowAt(1, "a") };
        screen.apply(seed);
        bare.mirror->apply(screen, seed);
        CHECK(bare.events.screenReplays == 1);
        CHECK(bare.events.screenUpdates == 0);

        auto tick = proto::Delta {};
        tick.stableViewportBase = 1;
        tick.stableFloor = 1;
        tick.lines = { rowAt(1, "b") };
        screen.apply(tick);
        bare.mirror->apply(screen, tick);
        CHECK(bare.events.screenReplays == 1);
        CHECK(bare.events.screenUpdates == 1);

        // A server resync while connected: a snapshot delta.
        auto resync = proto::Delta {};
        resync.snapshot = 1;
        resync.stableViewportBase = 1;
        resync.stableFloor = 1;
        resync.lines = { rowAt(1, "b") };
        screen.apply(resync);
        bare.mirror->apply(screen, resync);
        CHECK(bare.events.screenReplays == 2);
        CHECK(bare.events.screenUpdates == 1);
    }

    SECTION("an explicit full replay, as binding a pane makes")
    {
        bare.mirror->fullReplay(screen, vthost::client::LocalHistory::Discard);
        CHECK(bare.events.screenReplays == 1);
        CHECK(bare.events.screenUpdates == 0);
    }
}

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

TEST_CASE("a mirror's scrolled-back view follows the host's output as its listener asks", "[vthost][mirror]")
{
    // The mirror applies the host's output as a parse batch applies a local application's: under the terminal
    // lock, where a view scrolled back into the history is brought down to the lines it added if the listener
    // follows output. A row the host repainted in place added none.
    using vtbackend::OutputFollowing;
    struct Row
    {
        std::string_view name;
        OutputFollowing following;
        /// The host's viewport base after the output: one past the seed's scrolls a row in.
        int64_t outputBase;
        OutputFollowing outcome;
    };
    auto const rows = std::array {
        Row { .name = "a listener that stays",
              .following = OutputFollowing::Stay,
              .outputBase = 11,
              .outcome = OutputFollowing::Stay },
        Row { .name = "a listener that follows",
              .following = OutputFollowing::Follow,
              .outputBase = 11,
              .outcome = OutputFollowing::Follow },
        Row { .name = "a row repainted in place",
              .following = OutputFollowing::Follow,
              .outputBase = 10,
              .outcome = OutputFollowing::Stay },
    };

    for (auto const& row: rows)
    {
        INFO(row.name);
        auto bare = BareMirror { vtbackend::LineCount(100) };
        bare.events.following = row.following;
        bare.terminal->settings().autoScrollOnUpdate = true;
        auto screen = vthost::client::RemoteScreen {};
        screen.columns = 5;
        screen.lines = 1;

        auto seed = proto::Delta {};
        seed.snapshot = 1;
        seed.stableViewportBase = 10;
        seed.stableFloor = 7;
        for (auto const id: std::views::iota(int64_t { 7 }, int64_t { 11 }))
            seed.lines.push_back(rowAt(id));
        screen.apply(seed);
        bare.mirror->apply(screen, seed);
        REQUIRE(bare.terminal->viewport().scrollUp(vtbackend::LineCount(2)));

        auto output = proto::Delta {};
        output.stableViewportBase = row.outputBase;
        output.stableFloor = 7;
        output.lines.push_back(rowAt(row.outputBase, "new"));
        screen.apply(output);
        bare.mirror->apply(screen, output);

        auto const outcome =
            bare.terminal->viewport().scrolled() ? OutputFollowing::Stay : OutputFollowing::Follow;
        CHECK(outcome == row.outcome);
    }
}

TEST_CASE("a server-side clear takes what named the mirror's dropped rows with them", "[vthost][mirror]")
{
    // A hint session over the history labels rows for the whole session. The host's `clear` drops them from
    // the mirror's scrollback as ED 3 drops them from the host's, so the session must not go on naming them.
    auto bare = BareMirror { vtbackend::LineCount(100) };
    auto screen = vthost::client::RemoteScreen {};
    screen.columns = 5;
    screen.lines = 1;

    auto seed = proto::Delta {};
    seed.snapshot = 1;
    seed.stableViewportBase = 10;
    seed.stableFloor = 7;
    for (auto const id: std::views::iota(int64_t { 7 }, int64_t { 11 }))
        seed.lines.push_back(rowAt(id, "ab"));
    screen.apply(seed);
    bare.mirror->apply(screen, seed);
    REQUIRE(bare.historyLines() == 3);

    auto& terminal = *bare.terminal;
    auto const specs =
        std::array { vtbackend::LinkPatternSpec { .name = "ab", .regex = "ab", .activation = {} } };
    terminal.activateHintMode(
        vtbackend::HintModeRequest { .patterns = { .table = std::make_shared<vtbackend::LinkPatterns const>(
                                                       vtbackend::LinkPatterns::compile(specs)),
                                                   .names = { "ab" },
                                                   .resolve = {} },
                                     .action = vtbackend::HintAction::Copy,
                                     .scope = vtbackend::HintScope::Scrollback,
                                     .scrollbackLimit = vtbackend::LineCount(100) });
    REQUIRE(std::ranges::any_of(terminal.hintMatches(), [](vtbackend::HintMatch const& match) {
        return match.start.line < vtbackend::LineOffset(0);
    }));

    auto cleared = proto::Delta {};
    cleared.stableViewportBase = 10;
    cleared.stableFloor = 10;
    screen.apply(cleared);
    bare.mirror->apply(screen, cleared);
    REQUIRE(bare.historyLines() == 0);

    auto const top = terminal.primaryScreen().grid().addressableTop();
    for (auto const& match: terminal.hintMatches())
        CHECK(match.start.line >= top);
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

TEST_CASE("a context change relabels no mirror row the wire did not carry", "[vthost][mirror]")
{
    // A mirror's rows carry the contexts the host gave them. Handing a new active context to the mirror's
    // own screens relabelled the row its cursor still stood on from the delta before -- a row the host left
    // in the old context -- and every row the cursor reached afterwards that the host left without one.
    auto bare = BareMirror { vtbackend::LineCount(10) };
    auto screen = vthost::client::RemoteScreen {};
    screen.columns = 5;
    screen.lines = 1;

    auto const appContext = [](uint16_t id, uint16_t parent, std::string_view identifier) {
        auto context = proto::WireContext {};
        context.id = id;
        context.parent = parent;
        context.identifier = identifier;
        context.type = static_cast<uint8_t>(vtbackend::ContextType::App);
        return context;
    };
    auto const contextAt = [&bare](int row) {
        auto const id = bare.terminal->primaryScreen().grid().lineAt(vtbackend::LineOffset(row)).contextId();
        auto const* const record = bare.terminal->contexts().find(id);
        return record != nullptr ? record->identifier : std::string {};
    };

    auto seed = proto::Delta {};
    seed.snapshot = 1;
    seed.stableViewportBase = 10;
    seed.stableFloor = 10;
    seed.contexts.push_back(appContext(1, 0, "one"));
    seed.contextChanged = 1;
    seed.activeContext = 1;
    auto first = rowAt(10, "first");
    first.contextId = 1;
    seed.lines.push_back(first);
    screen.apply(seed);
    bare.mirror->apply(screen, seed);
    REQUIRE(contextAt(0) == "one");

    // The host's cursor moved on to a new row, and a nested context began there: only that row is the
    // new context's.
    auto nested = proto::Delta {};
    nested.stableViewportBase = 11;
    nested.stableFloor = 10;
    nested.contexts.push_back(appContext(2, 1, "two"));
    nested.contextChanged = 1;
    nested.activeContext = 2;
    auto second = rowAt(11, "two");
    second.contextId = 2;
    nested.lines.push_back(second);
    screen.apply(nested);
    bare.mirror->apply(screen, nested);

    // The ancestry follows the host all the same.
    REQUIRE(bare.terminal->contexts().active() != nullptr);
    CHECK(bare.terminal->contexts().active()->identifier == "two");
    CHECK(bare.terminal->contexts().depth() == 2);
    CHECK(contextAt(-1) == "one");
    CHECK(contextAt(0) == "two");

    // A row the host carries no context on scrolls in, and the mirror's cursor lands on it.
    auto plain = proto::Delta {};
    plain.stableViewportBase = 12;
    plain.stableFloor = 10;
    plain.lines.push_back(rowAt(12, "plain"));
    screen.apply(plain);
    bare.mirror->apply(screen, plain);

    CHECK(contextAt(-1) == "two");
    CHECK(contextAt(0).empty());
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
    auto const session = newSession(h.host);
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
    auto const session = newSession(h.host);
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

TEST_CASE("the status line DECSASD selects hides the mirror's cursor", "[vthost][mirror]")
{
    auto h = MirrorHarness {};
    auto const session = newSession(h.host);
    h.serverTerminal(session)->writeToScreen("main-display-here");

    auto scenario = [](MirrorHarness* h, vtworkspace::SessionId session) -> Task<void> {
        co_await waitUntil(&h->loop, [&] {
            return h->mirror->primaryScreen().grid().renderMainPageText().contains("main-display-here");
        });
        CHECK(h->mirror->isModeEnabled(vtbackend::DECMode::VisibleCursor));

        // DECSASD sends output to the host-writable status line, whose cursor is then the one shown. Its
        // position does not travel -- the cursor sent is the page on display's -- so the mirror draws none,
        // rather than one at its own status line's column 0: the daemon withholds DECTCEM.
        serverWrites(h, session, "\033[2$~\033[1$}\033[1;5Hstatus"); // DECSSDT, DECSASD, column 4, write
        co_await waitUntil(&h->loop,
                           [&] { return !h->mirror->isModeEnabled(vtbackend::DECMode::VisibleCursor); });
        CHECK(!h->mirror->isModeEnabled(vtbackend::DECMode::VisibleCursor));

        // Back on the main display, the cursor is shown again.
        serverWrites(h, session, "\033[0$}");
        co_await waitUntil(&h->loop,
                           [&] { return h->mirror->isModeEnabled(vtbackend::DECMode::VisibleCursor); });
        CHECK(h->mirror->isModeEnabled(vtbackend::DECMode::VisibleCursor));

        h->client->detach();
    }(&h, session);

    h.loop.blockOn(drive(&h, std::move(scenario)));
}

TEST_CASE("a decoupled cursor page hides the mirror's cursor", "[vthost][mirror]")
{
    auto h = MirrorHarness {};
    auto const session = newSession(h.host);
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
    auto const session = newSession(h.host);
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
    auto const session = newSession(h.host);
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
    auto const session = newSession(h.host);
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

namespace
{
/// The ids of the mirror terminal's program-status records, read under its lock.
std::vector<std::string> mirroredIds(MirrorHarness* h)
{
    auto const _ = std::lock_guard { *h->mirror };
    auto ids = std::vector<std::string> {};
    for (auto const& record: h->mirror->programStatus().records())
        ids.push_back(record.id);
    return ids;
}
} // namespace

TEST_CASE("ScreenMirror applies ProgramStatus to the mirror terminal", "[vthost][client][programstatus]")
{
    auto h = MirrorHarness {};
    auto const session = newSession(h.host);
    // In flight before the client attaches: the snapshot carries it.
    h.serverTerminal(session)->writeToScreen("\033]7501;state=done:id=build:app=make\033\\shell-ready");

    // Captureless: the coroutine outlives the closure that started it.
    auto scenario = [](MirrorHarness* h, vtworkspace::SessionId session) -> Task<void> {
        co_await waitUntil(&h->loop, [&] { return mirroredIds(h) == std::vector<std::string> { "build" }; });
        REQUIRE(mirroredIds(h) == std::vector<std::string> { "build" });

        // A report after the attach, with an ancestor's app to inherit.
        serverWrites(h, session, "\033]7501;state=blocked:id=build/test:kind=question:progress=60\033\\");
        co_await waitUntil(&h->loop, [&] { return mirroredIds(h).size() == 2; });
        {
            auto const _ = std::lock_guard { *h->mirror };
            auto const& set = h->mirror->programStatus();
            REQUIRE(set.records().size() == 2);
            auto const& blocked = set.records().back();
            CHECK(blocked.id == "build/test");
            CHECK(blocked.state == vtbackend::ProgramState::Blocked);
            CHECK(blocked.kind == vtbackend::BlockedKind::Question);
            CHECK(blocked.progress == uint8_t { 60 });
            CHECK(set.appOf(blocked) == "make");
        }

        // A clear empties the mirror too: an empty set is news.
        serverWrites(h, session, "\033]7501;state=clear\033\\");
        co_await waitUntil(&h->loop, [&] { return mirroredIds(h).empty(); });
        CHECK(mirroredIds(h).empty());

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
    auto const session = newSession(h.host);
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
    auto const session = newSession(h.host);
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
    auto const session = newSession(h.host);

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
    auto const session = newSession(h.host);

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
    auto const session = newSession(h.host);
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
    auto const session = newSession(h.host);

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

namespace
{
/// The absolute line number the gutter gives @p grid's top page row: every row the grid evicted for
/// good, then its history, then the row itself.
/// @param grid The grid to number.
/// @return The page's top row number, counted from 1.
[[nodiscard]] uint64_t pageTopLineNumber(vtbackend::Grid const& grid)
{
    return grid.evictedRowCount() + static_cast<uint64_t>(-unbox<int64_t>(grid.addressableTop())) + 1;
}

/// @p count command blocks of @p outputLines lines each, every one opened by an OSC 133 prompt mark,
/// for a test that needs the host's block-atomic trim to drop rows.
/// @param first The first block's number, which names its lines.
/// @param count How many blocks.
/// @param outputLines How many output lines follow each prompt.
/// @return The bytes to write.
std::string commandBlocks(int first, int count, int outputLines)
{
    auto bytes = std::string {};
    for (auto const block: std::views::iota(first, first + count))
    {
        bytes += std::format("\r\n\033]133;A\033\\prompt-{}", block);
        for (auto const line: std::views::iota(0, outputLines))
            bytes += std::format("\r\nout-{}-{}", block, line);
    }
    return bytes;
}
} // namespace

TEST_CASE("a remote pane numbers its lines as the host does across a block-atomic trim", "[vthost][mirror]")
{
    // With history headroom the host drops a whole command block at once, so its floor jumps further
    // than its page scrolled, and the mirror rebuilds its history from what the host still holds.
    auto h = MirrorHarness { {},
                             vtbackend::HistoryLimits { .guaranteed = vtbackend::LineCount(10),
                                                        .capacity = vtbackend::LineCount(40) } };
    auto const session = newSession(h.host);
    h.serverTerminal(session)->writeToScreen(commandBlocks(0, 7, 7));

    auto scenario = [](MirrorHarness* h, vtworkspace::SessionId session) -> Task<void> {
        auto const& serverGrid = h->serverTerminal(session)->primaryScreen().grid();
        auto const& mirrorGrid = h->mirror->primaryScreen().grid();
        co_await waitUntil(&h->loop, [&] { return mirrorGrid.renderMainPageText().contains("out-6-6"); });
        REQUIRE(serverGrid.evictedRowCount() == 0);

        serverWrites(h, session, commandBlocks(7, 2, 7));
        co_await waitUntil(&h->loop, [&] { return mirrorGrid.renderMainPageText().contains("out-8-6"); });

        // The claim is empty unless the trim dropped rows.
        REQUIRE(serverGrid.evictedRowCount() > 0);
        CHECK(pageTopLineNumber(mirrorGrid) == pageTopLineNumber(serverGrid));

        h->client->detach();
    }(&h, session);

    h.loop.blockOn(drive(&h, std::move(scenario)));
}

TEST_CASE("a remote pane's line numbers start over when the host resets", "[vthost][mirror]")
{
    auto h = MirrorHarness {};
    auto const session = newSession(h.host);
    h.serverTerminal(session)->writeToScreen(numberedRows(40));

    auto scenario = [](MirrorHarness* h, vtworkspace::SessionId session) -> Task<void> {
        auto const& serverGrid = h->serverTerminal(session)->primaryScreen().grid();
        auto const& mirrorGrid = h->mirror->primaryScreen().grid();
        co_await waitUntil(&h->loop, [&] { return mirrorGrid.renderMainPageText().contains("row-39"); });
        REQUIRE(mirrorGrid.historyLineCount() > vtbackend::LineCount(0));

        serverWrites(h, session, "\033cafter-reset"); // RIS
        co_await waitUntil(&h->loop, [&] {
            return mirrorGrid.renderMainPageText().contains("after-reset")
                   && !mirrorGrid.renderMainPageText().contains("row-39");
        });

        REQUIRE(pageTopLineNumber(serverGrid) == 1);
        CHECK(pageTopLineNumber(mirrorGrid) == 1);

        h->client->detach();
    }(&h, session);

    h.loop.blockOn(drive(&h, std::move(scenario)));
}

TEST_CASE("a remote pane attached after an eviction numbers its lines as the host does", "[vthost][mirror]")
{
    auto h = MirrorHarness {};
    auto const session = newSession(h.host);
    // More than the host's page and history hold, written before the client attaches.
    h.serverTerminal(session)->writeToScreen(numberedRows(150));

    auto scenario = [](MirrorHarness* h, vtworkspace::SessionId session) -> Task<void> {
        auto const& serverGrid = h->serverTerminal(session)->primaryScreen().grid();
        auto const& mirrorGrid = h->mirror->primaryScreen().grid();
        co_await waitUntil(&h->loop, [&] { return mirrorGrid.renderMainPageText().contains("row-149"); });

        REQUIRE(serverGrid.evictedRowCount() > 0);
        CHECK(pageTopLineNumber(mirrorGrid) == pageTopLineNumber(serverGrid));

        // And it keeps doing so while the rows the host evicts scroll by.
        serverWrites(h, session, "\r\n" + numberedRows(30));
        co_await waitUntil(&h->loop, [&] { return mirrorGrid.renderMainPageText().contains("row-29"); });
        CHECK(pageTopLineNumber(mirrorGrid) == pageTopLineNumber(serverGrid));

        h->client->detach();
    }(&h, session);

    h.loop.blockOn(drive(&h, std::move(scenario)));
}

TEST_CASE("a mirror keeping less history than the server numbers its lines as the server does",
          "[vthost][mirror]")
{
    // The server holds rows 5..10 and has evicted four before them, so its row 10 is number 10. The
    // mirror keeps two history rows, so its top row is the server's row 8, and its numbering has to
    // count the server rows above that it never kept.
    auto bare = BareMirror { vtbackend::LineCount(2) };
    auto screen = vthost::client::RemoteScreen {};
    screen.columns = 5;
    screen.lines = 1;
    auto seed = proto::Delta {};
    seed.snapshot = 1;
    seed.stableViewportBase = 10;
    seed.stableFloor = 5;
    seed.evictedRowCount = 4;
    for (auto const id: std::views::iota(int64_t { 5 }, int64_t { 11 }))
        seed.lines.push_back(rowAt(id));
    screen.apply(seed);
    bare.mirror->apply(screen, seed);

    auto const& grid = bare.terminal->primaryScreen().grid();
    REQUIRE(bare.historyLines() == 2);
    CHECK(pageTopLineNumber(grid) == 10);
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
    auto const session = newSession(h.host);

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
    auto const session = newSession(h.host);
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
    auto const session = newSession(h.host);
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

TEST_CASE("a fractionally scaled wire cell takes its mirrored line off the batched path", "[vthost][mirror]")
{
    // `OSC 66 ; n=1:d=2:w=1` sizes a cell without widening it: scale 1, one column, no continuation.
    // applyWireCell restores the sizing over what writeCellToSoA reset, so the restore must degrade the
    // line as CellProxy::setTextScale does. Left Uniform, the line renders batched, at full size.
    auto const fraction = vtbackend::CellScale { .numerator = 1, .denominator = 2 };

    auto mirrored = vtbackend::LineSoA {};
    vtbackend::initializeLineSoA(mirrored, vtbackend::ColumnCount(4));
    auto cell = vthost::wireCellOf(vtbackend::GraphicsAttributes {}); // the pen the line's cells wear
    cell.codepoint = U'x';
    cell.textScaleExtras = vtbackend::packTextScaleExtras(fraction);
    vthost::applyWireCell(mirrored, 1, cell, vtbackend::HyperlinkId {});
    CHECK(mirrored.shape >= vtbackend::shapeImpliedByCells(mirrored));

    auto written = vtbackend::LineSoA {};
    vtbackend::initializeLineSoA(written, vtbackend::ColumnCount(4));
    vtbackend::CellProxy(written, 1).write(vtbackend::GraphicsAttributes {}, U'x', 1);
    vtbackend::CellProxy(written, 1).setTextScale(fraction);
    CHECK(mirrored.shape == written.shape);
}

TEST_CASE("input-relevant DEC modes mirror into the local terminal", "[vthost][mirror]")
{
    auto h = MirrorHarness {};
    auto const session = newSession(h.host);
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
    auto const session = newSession(h.host);
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
        std::ignore = h->mirror->sendMousePressEvent(vtbackend::Modifiers {},
                                                     vtbackend::MouseButton::Left,
                                                     vtbackend::CellLocation {},
                                                     vtbackend::PixelCoordinate {},
                                                     false);
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
    auto const session = newSession(h.host);
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
    auto const session = newSession(h.host);
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
    auto const session = newSession(h.host);

    // Draw a 2-cell-wide, 1-cell-tall image at the home position on the server via a
    // GIP oneshot (StretchToFill, so both cells are covered). The client fetches the
    // pixels and the mirror re-emits them as GIP, materialising image fragments.
    auto const pixels = std::vector<uint8_t>(static_cast<std::size_t>(8 * 8 * 4), 0xC0);
    auto const body = core::base64::encode(
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
    auto const session = newSession(h.host);
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
    auto const session = newSession(h.host);
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
        auto const encoded = core::base64::encode(std::string_view { "clip-text" });
        serverWrites(h, session, std::format("\033]52;c;{}\033\\", encoded));
        co_await waitUntil(&h->loop, [&] { return h->mirrorEvents.clipboard == "clip-text"; });
        CHECK(h->mirrorEvents.clipboard == "clip-text");

        h->client->detach();
    }(&h, session);

    h.loop.blockOn(drive(&h, std::move(scenario)));
}

TEST_CASE("an OSC 99 notification reaches the mirror with its occasion", "[vthost][mirror][notification]")
{
    // Raised through the mirror's OSC 99 path, so the frontend decides when to show it by the rule a
    // local session's OSC 99 is decided by -- not as an OSC 777 notice, which states no occasion.
    auto h = MirrorHarness {};
    auto const session = newSession(h.host);
    h.serverTerminal(session)->writeToScreen("ready");

    auto scenario = [](MirrorHarness* h, vtworkspace::SessionId session) -> Task<void> {
        co_await waitUntil(&h->loop, [&] {
            return h->mirror->primaryScreen().grid().renderMainPageText().contains("ready");
        });

        serverWrites(h, session, "\033]99;i=deploy:o=always;Deployed\033\\");
        serverWrites(h, session, "\033]99;i=pause:o=invisible;Paused\033\\");
        co_await waitUntil(&h->loop, [&] { return h->mirrorEvents.shownNotifications.size() >= 2; });

        h->client->detach();
    }(&h, session);

    h.loop.blockOn(drive(&h, std::move(scenario)));

    auto const& shown = h.mirrorEvents.shownNotifications;
    REQUIRE(shown.size() == 2);
    CHECK(shown[0].title == "Deployed");
    CHECK(shown[0].occasion == vtbackend::DisplayOccasion::Always);
    CHECK(shown[1].title == "Paused");
    CHECK(shown[1].occasion == vtbackend::DisplayOccasion::Invisible);
    CHECK(h.mirrorEvents.notifyTitle.empty());
}

TEST_CASE("a live cursor-shape change reaches the mirror", "[vthost][mirror]")
{
    auto h = MirrorHarness {};
    auto const session = newSession(h.host);
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
    auto const session = newSession(h.host);
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
    auto const session = newSession(h.host);
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
    auto const session = newSession(h.host);
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
    auto const session = newSession(h.host);
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

TEST_CASE("ScreenMirror.extension.applied", "[vthost][mirror][extensions]")
{
    // The indicator row the daemon's extensions draw is shown on the mirror in place of the mirror's own
    // template, which knows of no extension; once they stop drawing it, the mirror draws its own again.
    auto hosting = std::make_shared<vthost::testing::FakeExtensionHosting>();
    hosting->prepare = [](vtbackend::FakeStatusLineExtensions& fake) {
        fake.takeOver("plasma");
        fake.publish("plasma", vthost::testing::frameOf("\033[1mPLASMA\033[m"));
    };
    auto h = MirrorHarness { {},
                             vtbackend::HistoryLimits::plain(ServerHistoryLines),
                             MirrorHistoryLines,
                             hosting,
                             [](vtbackend::Settings& settings) {
                                 settings.indicatorStatusLine.left = "template";
                                 settings.indicatorStatusLine.middle.clear();
                                 settings.indicatorStatusLine.right.clear();
                             } };
    // The mirror shows its indicator, as a client whose profile asks for it does: shown before the mirror
    // binds to it, which takes the terminal's own display as the least it shows.
    h.mirror->setStatusDisplay(vtbackend::StatusDisplayType::Indicator);
    h.populator = std::make_unique<ScreenMirror>(*h.mirror, MirrorHarness::Daemon);
    auto const session = newSession(h.host);
    REQUIRE(hosting->sessions.size() == 1);

    auto scenario = [](MirrorHarness* h, vthost::testing::FakeExtensionHosting* hosting) -> Task<void> {
        auto const indicatorText = [h] {
            // What the next frame paints, as a frontend's render would.
            h->mirror->refreshRenderBuffer();
            return h->mirror->indicatorStatusLineDisplay()
                .grid()
                .lineAt(vtbackend::LineOffset(0))
                .toUtf8Trimmed();
        };
        co_await waitUntil(&h->loop, [&] { return indicatorText() == "PLASMA"; });
        CHECK(indicatorText() == "PLASMA");
        // Cell by cell, attributes and all.
        CHECK(h->mirror->indicatorStatusLineDisplay()
                  .at(vtbackend::LineOffset(0), vtbackend::ColumnOffset(0))
                  .isFlagEnabled(vtbackend::CellFlag::Bold));
        CHECK(h->mirror->indicatorStatusLineDisplay().grid().lineAt(vtbackend::LineOffset(0)).size()
              == h->mirror->indicatorStatusLineDisplay().pageSize().columns);

        hosting->sessions.front()->publish("plasma", vthost::testing::frameOf("NEXT", 2));
        co_await waitUntil(&h->loop, [&] { return indicatorText() == "NEXT"; });
        CHECK(indicatorText() == "NEXT");

        // Nothing takes the row over any more: the mirror's own template is back.
        hosting->sessions.front()->fake.takeOver(std::nullopt);
        auto& server =
            *h->host.terminal(h->host.model().window(h->host.windowId())->activeTab()->rootPane()->session());
        {
            auto const guard = std::lock_guard { server };
            server.statusLineExtensionsReplaced();
        }
        h->server->sessionScreenUpdated(
            h->host.model().window(h->host.windowId())->activeTab()->rootPane()->session());
        co_await waitUntil(&h->loop, [&] { return indicatorText() != "NEXT"; });
        CHECK(indicatorText() != "NEXT");
        CHECK_FALSE(indicatorText().empty());

        h->client->detach();
    }(&h, hosting.get());
    h.loop.blockOn(drive(&h, std::move(scenario)));
    std::ignore = session;
}

TEST_CASE("host-writable status-line content reaches the mirror", "[vthost][mirror]")
{
    auto h = MirrorHarness {};
    auto const session = newSession(h.host);
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
    auto const session = newSession(h.host);
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
    auto const session = newSession(h.host);
    auto const gaps = gapsAfter(&h, session, "\u4F60\u597D e\u0301 \U0001F600 end", mirrorShows(&h, "end"));
    checkParity(gaps); // width, cluster extras and codepoints all survive
}

TEST_CASE("PARITY a wrapped logical line", "[vthost][parity]")
{
    // The load-bearing case for reflow: whether the mirror knows this row continues the one
    // above it. There is no escape sequence that says so.
    auto h = MirrorHarness {};
    auto const session = newSession(h.host);
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
    auto const session = newSession(h.host);
    auto const gaps =
        gapsAfter(&h, session, commandRun("ls", { "file-one" }) + "done", mirrorShows(&h, "done"));
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
    auto const session = newSession(h.host);
    auto const gaps = gapsAfter(&h, session, "\033#6wide-line", mirrorShows(&h, "wide-line"));
    checkParity(gaps); // DECDWL/DECDHL used to render as ordinary lines
}

TEST_CASE("PARITY DECDHL double-height halves", "[vthost][parity]")
{
    // The two halves are SEPARATE flags on separate rows, and a receiver that carries only
    // DoubleWidth would render both halves as one wide line.
    auto h = MirrorHarness {};
    auto const session = newSession(h.host);
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
    auto const session = newSession(h.host);
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
    auto const session = newSession(h.host);
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
    auto const session = newSession(h.host);
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
    auto const session = newSession(h.host);
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
    auto const session = newSession(h.host);
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
                                            core::base64::encode(pixels)),
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
    auto const session = newSession(h.host);
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
    auto const session = newSession(h.host);
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
    auto const session = newSession(h.host);
    // Written before blockOn: the client is not running yet, so this is history it never watched.
    // The empty `bytes` below is what makes this judge the snapshot rather than a delta.
    h.serverTerminal(session)->writeToScreen(colouredRows(40));
    auto const gaps = gapsAfter(&h, session, "", mirrorShows(&h, "last"));
    checkParity(gaps); // text, colour and wrap flags survive the snapshot, history included
}

TEST_CASE("PARITY a host that cleared everything above its prompt", "[vthost][parity][semanticblocks]")
{
    // Clearing to the prompt drops the host's history and the page rows above the prompt at once, so the
    // host's floor jumps further than its page scrolled. That is the mirror's signal to rebuild from what the
    // host still holds; without it the rows the host dropped would stay in the mirror's scrollback.
    auto h = MirrorHarness {};
    auto const session = newSession(h.host);
    h.serverTerminal(session)->writeToScreen(numberedRows(40) + "\033]133;A\033\\$ prompt-here");

    auto gaps = std::vector<vthost::testing::ParityGap> {};
    auto scenario = [](MirrorHarness* h,
                       vtworkspace::SessionId session,
                       std::vector<vthost::testing::ParityGap>* out) -> Task<void> {
        auto& server = *h->serverTerminal(session);
        auto const& mirrorGrid = h->mirror->primaryScreen().grid();
        co_await waitUntil(&h->loop, mirrorShows(h, "prompt-here"));
        REQUIRE(mirrorGrid.historyLineCount() > vtbackend::LineCount(0));

        {
            auto const guard = std::lock_guard { server };
            REQUIRE(server.clearToPrompt().has_value());
        }
        h->server->sessionScreenUpdated(session);
        co_await waitUntil(&h->loop,
                           [&] { return mirrorGrid.historyLineCount() == vtbackend::LineCount(0); });

        CHECK(mirrorGrid.lineText(vtbackend::LineOffset(0)).starts_with("$ prompt-here"));
        CHECK(pageTopLineNumber(mirrorGrid) == pageTopLineNumber(server.primaryScreen().grid()));
        *out = vthost::testing::compareGrids(server, *h->mirror);
        h->client->detach();
    }(&h, session, &gaps);

    h.loop.blockOn(drive(&h, std::move(scenario)));
    checkParity(gaps);
}

TEST_CASE("PARITY a client that asked its host to clear to the prompt", "[vthost][parity][operations]")
{
    // The client's terminal is a mirror: clearing it locally would be undone by the next delta, and leave the
    // host's rows standing for every other client. Asked through the protocol, the host clears its own grid
    // and the mirror follows.
    auto h = MirrorHarness {};
    auto const session = newSession(h.host);
    h.serverTerminal(session)->writeToScreen(numberedRows(40) + "\033]133;A\033\\$ prompt-here");

    auto gaps = std::vector<vthost::testing::ParityGap> {};
    auto scenario = [](MirrorHarness* h,
                       vtworkspace::SessionId session,
                       std::vector<vthost::testing::ParityGap>* out) -> Task<void> {
        auto& server = *h->serverTerminal(session);
        auto const& mirrorGrid = h->mirror->primaryScreen().grid();
        co_await waitUntil(&h->loop, mirrorShows(h, "prompt-here"));
        REQUIRE(mirrorGrid.historyLineCount() > vtbackend::LineCount(0));

        h->client->performOperation(proto::PerformOperation {
            .session = session.value, .operation = proto::EngineOperation::ClearToPrompt });
        co_await waitUntil(&h->loop,
                           [&] { return mirrorGrid.historyLineCount() == vtbackend::LineCount(0); });

        CHECK(server.primaryScreen().historyLineCount() == vtbackend::LineCount(0));
        CHECK(mirrorGrid.lineText(vtbackend::LineOffset(0)).starts_with("$ prompt-here"));
        *out = vthost::testing::compareGrids(server, *h->mirror);
        h->client->detach();
    }(&h, session, &gaps);

    h.loop.blockOn(drive(&h, std::move(scenario)));
    checkParity(gaps);
}

TEST_CASE("PARITY a client that asked its host to reset and write", "[vthost][parity][operations]")
{
    // A hard reset drops the host's history and a written text lands on its page; the mirror follows both.
    auto h = MirrorHarness {};
    auto const session = newSession(h.host);
    h.serverTerminal(session)->writeToScreen(numberedRows(40) + "last-row");

    auto gaps = std::vector<vthost::testing::ParityGap> {};
    auto scenario = [](MirrorHarness* h,
                       vtworkspace::SessionId session,
                       std::vector<vthost::testing::ParityGap>* out) -> Task<void> {
        auto& server = *h->serverTerminal(session);
        auto const& mirrorGrid = h->mirror->primaryScreen().grid();
        co_await waitUntil(&h->loop, mirrorShows(h, "last-row"));
        REQUIRE(mirrorGrid.historyLineCount() > vtbackend::LineCount(0));

        h->client->performOperation(proto::PerformOperation {
            .session = session.value, .operation = proto::EngineOperation::ClearHistoryAndReset });
        h->client->performOperation(
            proto::PerformOperation { .session = session.value,
                                      .operation = proto::EngineOperation::WriteScreen,
                                      .text = "after-reset" });
        co_await waitUntil(&h->loop, mirrorShows(h, "after-reset"));
        co_await waitUntil(&h->loop,
                           [&] { return mirrorGrid.historyLineCount() == vtbackend::LineCount(0); });

        CHECK(server.primaryScreen().historyLineCount() == vtbackend::LineCount(0));
        CHECK(mirrorGrid.lineText(vtbackend::LineOffset(0)).starts_with("after-reset"));
        *out = vthost::testing::compareGrids(server, *h->mirror);
        h->client->detach();
    }(&h, session, &gaps);

    h.loop.blockOn(drive(&h, std::move(scenario)));
    checkParity(gaps);
}

TEST_CASE("PARITY wrapped content reflows alike when narrowed", "[vthost][parity]")
{
    // Narrowing SPLITS logical lines: the daemon's grid grows rows, and so must the client's. Both
    // run vtbackend's own reflow over their own copy, so this measures whether the copy was faithful
    // enough for two independent reflows to land on the same grid.
    auto h = MirrorHarness {};
    auto const session = newSession(h.host);
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
    auto const session = newSession(h.host);
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
    auto const session = newSession(h.host);
    auto const gaps = gapsAfter(
        &h, session, "\033]8;;https://example.com/a\033\\link\033]8;;\033\\ tail", mirrorShows(&h, "tail"));
    checkParity(gaps); // compared by URI, since ids are per-terminal counters
}

TEST_CASE("PARITY a context that begins after the cursor moved", "[vthost][parity]")
{
    // One push carries a cursor move and an OSC 3008 start=: the host gives the new context to the row its
    // cursor moved TO, so the mirror must not hand it to the row its own cursor still stood on.
    auto h = MirrorHarness {};
    auto const session = newSession(h.host);
    h.serverTerminal(session)->writeToScreen("\033]3008;start=one;type=container\033\\first\r\n");

    auto const scenario = [](MirrorHarness* h,
                             vtworkspace::SessionId session,
                             std::vector<vthost::testing::ParityGap>* out) -> Task<void> {
        // Delivered apart from the attach, so the mirror's cursor sits on the row below "first".
        co_await waitUntil(&h->loop, mirrorShows(h, "first"));
        serverWrites(h, session, "\033[H\033]3008;start=two;type=app\033\\second");
        co_await waitUntil(&h->loop, mirrorShows(h, "second"));
        co_await waitUntil(&h->loop, [h, session] {
            return vthost::testing::compareGrids(
                       *h->serverTerminal(session), *h->mirror, vthost::testing::ComparedPage::Active, 1)
                .empty();
        });
        *out = vthost::testing::compareGrids(*h->serverTerminal(session), *h->mirror);
        h->client->detach();
    };
    auto gaps = std::vector<vthost::testing::ParityGap> {};
    h.loop.blockOn(drive(&h, scenario(&h, session, &gaps)));

    // The probe engaged: the host holds the two rows in two different contexts.
    auto const* const server = h.serverTerminal(session);
    auto const contextAt = [server](int row) {
        auto const id = server->primaryScreen().grid().lineAt(vtbackend::LineOffset(row)).contextId();
        auto const* const record = server->contexts().find(id);
        return record != nullptr ? record->identifier : std::string {};
    };
    REQUIRE(contextAt(0) == "two");
    REQUIRE(contextAt(1) == "one");
    checkParity(gaps);
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

// The mirror writes a scrollback row as any other writer of one must: through Grid::changingLineAt, which
// has the grid's own change scan reach that deep. Nothing scrolled, so a row written through the plain
// accessor is never stamped, and whatever reads the mirror's grid by its changes never sees the new text.
TEST_CASE("an in-place scrollback change is announced to the mirror grid's change scan", "[vthost][mirror]")
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

    // Everything the seed wrote has been seen.
    auto& grid = bare.terminal->primaryScreen().grid();
    auto cursor = vtbackend::GridDeltaCursor {};
    std::ignore = grid.forEachLineChangedSince(cursor, [](vtbackend::LineOffset, vtbackend::Line const&) {});

    auto touched = proto::Delta {};
    touched.stableViewportBase = 10;
    touched.stableFloor = 7;
    touched.lines.push_back(rowAt(8, "NEW8"));
    screen.apply(touched);
    bare.mirror->apply(screen, touched);

    auto changed = std::vector<int> {};
    CHECK(grid.forEachLineChangedSince(cursor, [&](vtbackend::LineOffset offset, vtbackend::Line const&) {
        changed.push_back(unbox<int>(offset));
    }) == vtbackend::GridDeltaResult::Delta);
    CHECK(changed == std::vector { -2 });
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
// ends in core::unreachable() — undefined behaviour in the attached client.
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
    auto const session = newSession(h.host);

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

// ---------------------------------------------------------------------------------------------
// Command blocks: row identity and birth stamps.
// ---------------------------------------------------------------------------------------------

TEST_CASE("a wire row's command-block id and birth stamp land on the mirror line", "[vthost][mirror][blocks]")
{
    auto bare = BareMirror { vtbackend::LineCount(10) };
    auto screen = vthost::client::RemoteScreen {};
    screen.columns = 5;
    screen.lines = 1;

    auto seed = proto::Delta {};
    seed.snapshot = 1;
    seed.stableViewportBase = 10;
    seed.stableFloor = 10;
    auto row = rowAt(10, "abcde");
    row.blockId = 42;
    row.bornAt = 7;
    seed.lines.push_back(row);
    screen.apply(seed);
    bare.mirror->apply(screen, seed);

    auto const& line = bare.terminal->primaryScreen().grid().lineAt(vtbackend::LineOffset(0));
    CHECK(line.blockId() == vtbackend::CommandBlockId { 42 }); // the HOST's id, untranslated
    CHECK(line.bornAt() == 7);

    // A stamp wider than the line's 24 bits saturates rather than wrapping into a plausible time.
    auto wide = proto::Delta {};
    wide.stableViewportBase = 10;
    wide.stableFloor = 10;
    auto stamped = rowAt(10, "abcde");
    stamped.bornAt = 0xFFFFFFFF;
    wide.lines.push_back(stamped);
    screen.apply(wide);
    bare.mirror->apply(screen, wide);
    auto const& restamped = bare.terminal->primaryScreen().grid().lineAt(vtbackend::LineOffset(0));
    CHECK(restamped.bornAt() == vtbackend::Line::MaxBornAt);
    // And a row the host says belongs to no block belongs to none here either: the rewrite does not
    // keep the id the row held before.
    CHECK(restamped.blockId() == vtbackend::CommandBlockId {});

    // All ones reads as MaxBornAt whether the stamp saturates or merely drops its high byte, so it
    // cannot tell the two apart; a stamp just past the 24 bits can. Dropping the high byte would
    // make it 7, the very stamp the row was first given.
    auto past = proto::Delta {};
    past.stableViewportBase = 10;
    past.stableFloor = 10;
    auto overflowing = rowAt(10, "abcde");
    overflowing.bornAt = vtbackend::Line::MaxBornAt + 1 + 7;
    past.lines.push_back(overflowing);
    screen.apply(past);
    bare.mirror->apply(screen, past);
    CHECK(bare.terminal->primaryScreen().grid().lineAt(vtbackend::LineOffset(0)).bornAt()
          == vtbackend::Line::MaxBornAt);
}

TEST_CASE("adopting a command block leaves the mirror's row ids to the wire", "[vthost][mirror][blocks]")
{
    // A mirror's rows carry the ids the host gave them. Were adopting a running record to make it the
    // block the mirror's own cursor stamps rows with, the line the cursor is on would be relabelled, and
    // a row the cursor reached that the host left without a block would take the id here alone.
    auto bare = BareMirror { vtbackend::LineCount(10) };
    auto screen = vthost::client::RemoteScreen {};
    screen.columns = 5;
    screen.lines = 1;

    auto seed = proto::Delta {};
    seed.snapshot = 1;
    seed.stableViewportBase = 10;
    seed.stableFloor = 10;
    auto prompt = rowAt(10, "$ ls");
    prompt.blockId = 4;
    seed.lines.push_back(prompt);
    screen.apply(seed);
    bare.mirror->apply(screen, seed);

    auto running = vtbackend::CommandBlockRecord {};
    running.id = vtbackend::CommandBlockId { 5 };
    running.state = vtbackend::CommandBlockState::Running;
    bare.terminal->adoptCommandBlock(std::move(running), vtbackend::AdoptMode::Live);

    auto const& grid = bare.terminal->primaryScreen().grid();
    REQUIRE(bare.terminal->commandBlocks().find(vtbackend::CommandBlockId { 5 }) != nullptr);
    CHECK(grid.lineAt(vtbackend::LineOffset(0)).blockId() == vtbackend::CommandBlockId { 4 });

    // A row the host carries no block on scrolls in, and the mirror's cursor lands on it.
    auto next = proto::Delta {};
    next.stableViewportBase = 11;
    next.stableFloor = 10;
    next.lines.push_back(rowAt(11, "out"));
    screen.apply(next);
    bare.mirror->apply(screen, next);

    CHECK(grid.lineAt(vtbackend::LineOffset(-1)).blockId() == vtbackend::CommandBlockId { 4 });
    CHECK(grid.lineAt(vtbackend::LineOffset(0)).blockId() == vtbackend::CommandBlockId {});
}

TEST_CASE("the parity oracle names a block-id and a birth-stamp divergence", "[vthost][parity]")
{
    // Two mirrors fed the same row except for the field under test: the oracle must name exactly
    // that field. Equal grids pass trivially, so this is what proves the two new rows are compared.
    auto const fedWith = [](uint32_t blockId, uint32_t bornAt) {
        auto bare = std::make_unique<BareMirror>(vtbackend::LineCount(10));
        auto screen = vthost::client::RemoteScreen {};
        screen.columns = 5;
        screen.lines = 1;
        auto seed = proto::Delta {};
        seed.snapshot = 1;
        seed.stableViewportBase = 10;
        seed.stableFloor = 10;
        auto row = rowAt(10, "abcde");
        row.blockId = blockId;
        row.bornAt = bornAt;
        seed.lines.push_back(row);
        screen.apply(seed);
        bare->mirror->apply(screen, seed);
        return bare;
    };
    auto const reference = fedWith(7, 61);
    auto const diverging = [&](uint32_t blockId, uint32_t bornAt) {
        auto const other = fedWith(blockId, bornAt);
        auto fields = std::set<std::string> {};
        for (auto const& gap: vthost::testing::compareGrids(*reference->terminal, *other->terminal))
            fields.insert(gap.field);
        return fields;
    };

    CHECK(diverging(7, 61).empty());
    CHECK(diverging(8, 61) == std::set<std::string> { "blockId" });
    CHECK(diverging(7, 62) == std::set<std::string> { "bornAt" });
}

TEST_CASE("PARITY command-block ids and birth stamps", "[vthost][parity]")
{
    auto h = MirrorHarness {};
    auto const session = newSession(h.host);
    // The command's output ends in "ABCDEFGHI中X" written on the page's last ten columns: the wide
    // character does not fit the row's one remaining column, moves on to the next row and leaves
    // CellFlag::WrapPadding behind. The mirror has to keep that bit, or copy, word selection and
    // capture-pane on the client take the padding for a blank the host leaves out.
    auto const columns = unbox<int>(h.serverTerminal(session)->pageSize().columns);
    auto const gaps = gapsAfter(&h,
                                session,
                                std::format("\033]133;A\033\\$ \033]133;B\033\\make\r\n"
                                            "\033]133;C;cmdline_url=make\033\\built\r\n"
                                            "\033[{}GABCDEFGHI中X\r\n"
                                            "\033]133;D;0\033\\done",
                                            columns - 9),
                                mirrorShows(&h, "done"));
    // The probe engaged: the server stamped the rows this case is about, so two grids equally EMPTY
    // of block ids cannot pass for parity.
    auto const& serverLine =
        h.serverTerminal(session)->primaryScreen().grid().lineAt(vtbackend::LineOffset(0));
    REQUIRE(serverLine.blockId().value != 0);
    REQUIRE(serverLine.bornAt() != 0);
    // And it holds the padding the wide character left, in the column it passed over.
    REQUIRE(serverHasCellFlag(&h, session, vtbackend::CellFlag::WrapPadding));
    checkParity(gaps);
    // Read off the lines as well: the oracle compares both grids through toWireLine, so a toWireLine
    // that left a field out would read zero on both sides and agree.
    auto const& mirrorLine = h.mirror->primaryScreen().grid().lineAt(vtbackend::LineOffset(0));
    CHECK(mirrorLine.blockId() == serverLine.blockId());
    CHECK(mirrorLine.bornAt() == serverLine.bornAt());
}

namespace
{
/// Waits until @p settle holds, then detaches, so `drive` returns with the mirror caught up.
Task<void> settleThenDetach(MirrorHarness* h, std::function<bool()> settle)
{
    co_await waitUntil(&h->loop, std::move(settle));
    h->client->detach();
}
} // namespace

TEST_CASE("the mirror counts birth stamps from the host's session epoch", "[vthost][mirror][blocks]")
{
    auto h = MirrorHarness {};
    auto const session = newSession(h.host);
    auto* const server = h.serverTerminal(session);
    // An hour apart, so a mirror counting from its OWN epoch is visibly wrong rather than off by the
    // milliseconds between the two terminals' construction.
    server->adoptSessionEpoch(server->sessionEpoch() - std::chrono::hours(1));
    server->writeToScreen("stamped");

    h.loop.blockOn(drive(&h, settleThenDetach(&h, mirrorShows(&h, "stamped"))));

    auto const& serverLine = server->primaryScreen().grid().lineAt(vtbackend::LineOffset(0));
    auto const& mirrorLine = h.mirror->primaryScreen().grid().lineAt(vtbackend::LineOffset(0));
    REQUIRE(serverLine.bornAt() != 0); // the probe engaged: the host stamped the row
    CHECK(h.mirror->sessionEpoch() == server->sessionEpoch());
    CHECK(mirrorLine.bornAt() == serverLine.bornAt());
    CHECK(h.mirror->lineBirthTime(mirrorLine) == server->lineBirthTime(serverLine));
}

TEST_CASE("a peer's session epoch at the clock's limits leaves every birth time defined",
          "[vthost][mirror][blocks]")
{
    // The epoch is a peer's value, and the mirror floors it, adds up to Line::MaxBornAt seconds to it for
    // a row's birth time, and subtracts it from its own clock to stamp a row it writes itself. Each of
    // those overflows near the limits of a nanosecond clock unless the terminal holds the value where it
    // adopts it.
    constexpr auto LatestStamp = vtbackend::Line::MaxBornAt - 1; // the latest stamp that still names a time
    auto const mirrorWithEpoch = [](int64_t epoch) {
        auto bare = std::make_unique<BareMirror>(vtbackend::LineCount(10));
        auto screen = vthost::client::RemoteScreen {};
        auto state = proto::SessionState {};
        state.columns = 5;
        state.lines = 1;
        state.sessionEpoch = epoch;
        screen.apply(state);
        auto seed = proto::Delta {};
        seed.snapshot = 1;
        seed.stableViewportBase = 10;
        seed.stableFloor = 10;
        auto row = rowAt(10, "abcde");
        row.bornAt = LatestStamp;
        seed.lines.push_back(row);
        screen.apply(seed);
        bare->mirror->apply(screen, seed);
        return bare;
    };
    auto const rowOf = [](BareMirror const& bare) -> vtbackend::Line const& {
        return bare.terminal->primaryScreen().grid().lineAt(vtbackend::LineOffset(0));
    };
    auto const latestBirth = std::chrono::seconds { LatestStamp - 1 };

    SECTION("an epoch past the latest instant is the latest")
    {
        auto const bare = mirrorWithEpoch(std::numeric_limits<int64_t>::max());
        auto const epoch = std::chrono::floor<std::chrono::seconds>(vtbackend::LatestHeldInstant);
        CHECK(bare->terminal->sessionEpoch() == epoch);
        CHECK(bare->terminal->lineBirthTime(rowOf(*bare)) == epoch + latestBirth);

        // A row the mirror's own clock stamps: still before the epoch, so the session's first second.
        bare->terminal->writeToScreen("\r\n");
        CHECK(bare->terminal->lineBirthTime(rowOf(*bare)) == epoch);
    }

    SECTION("an epoch before the earliest instant is the earliest")
    {
        auto const bare = mirrorWithEpoch(std::numeric_limits<int64_t>::min());
        auto const epoch = std::chrono::system_clock::time_point {};
        CHECK(bare->terminal->sessionEpoch() == epoch);
        CHECK(bare->terminal->lineBirthTime(rowOf(*bare)) == epoch + latestBirth);

        // Further from the epoch than a stamp reaches: born, but when is unknown.
        bare->terminal->writeToScreen("\r\n");
        CHECK(rowOf(*bare).bornAt() == vtbackend::Line::MaxBornAt);
        CHECK_FALSE(bare->terminal->lineBirthTime(rowOf(*bare)).has_value());
    }
}

// ---------------------------------------------------------------------------------------------
// Command blocks: the records the mirror adopts.
// ---------------------------------------------------------------------------------------------

namespace
{
/// @p blocks as a snapshot's SessionState.
proto::SessionState stateWith(std::vector<proto::WireCommandBlock> blocks)
{
    auto state = proto::SessionState {};
    state.commandBlocks = std::move(blocks);
    return state;
}

/// A 5x3 BareMirror with ten lines of scrollback, and the RemoteScreen feeding it — the unit harness for the
/// record cases below.
struct BlockMirror: BareMirror
{
    vthost::client::RemoteScreen screen;

    /// @param daemon Where the daemon runs relative to the mirror.
    /// @param identity The machine the mirror terminal says it runs on.
    /// @param clocks The clocks the mirror terminal reads; the system's when unset.
    explicit BlockMirror(vthost::DaemonMachine daemon = vthost::DaemonMachine::Same,
                         vtbackend::HostIdentity identity = {},
                         std::optional<vtbackend::TerminalClocks> clocks = std::nullopt):
        BareMirror { BareMirrorShape { .lines = vtbackend::LineCount(3),
                                       .history = vtbackend::LineCount(10),
                                       .daemon = daemon,
                                       .identity = std::move(identity),
                                       .clocks = clocks } }
    {
    }

    /// Delivers @p state and a snapshot of @p rows (rows 10..12, blank, when empty) — an attach the
    /// first time, a resync after that.
    void snapshot(proto::SessionState state, std::vector<proto::WireLine> rows = {})
    {
        state.columns = 5;
        state.lines = 3;
        screen.apply(state);
        auto delta = proto::Delta {};
        delta.snapshot = 1;
        delta.stableViewportBase = 10;
        delta.stableFloor = 10;
        delta.lines = rows.empty() ? std::vector { rowAt(10), rowAt(11), rowAt(12) } : std::move(rows);
        screen.apply(delta);
        mirror->apply(screen, delta);
    }

    /// Delivers an increment carrying @p blocks, retiring @p retired and rewriting @p rows, over an unmoved
    /// viewport.
    void increment(std::vector<proto::WireCommandBlock> blocks,
                   std::vector<uint32_t> retired = {},
                   std::vector<proto::WireLine> rows = {})
    {
        auto delta = proto::Delta {};
        delta.stableViewportBase = 10;
        delta.stableFloor = 10;
        delta.commandBlocks = std::move(blocks);
        delta.retiredCommandBlocks = std::move(retired);
        delta.lines = std::move(rows);
        screen.apply(delta);
        mirror->apply(screen, delta);
    }

    /// @return The mirror's record for host id @p id, or nullptr.
    [[nodiscard]] vtbackend::CommandBlockRecord const* record(uint32_t id) const
    {
        return terminal->commandBlocks().find(vtbackend::CommandBlockId { id });
    }
};

/// @return The newest record's state in @p terminal's store, or nullopt when it holds none.
std::optional<vtbackend::CommandBlockState> newestState(vtbackend::Terminal const& terminal)
{
    auto const records = terminal.commandBlocks().records();
    if (records.empty())
        return std::nullopt;
    return records.back().state;
}

/// Whether @p mirror's store holds exactly @p server's records, over every field the wire carries: each
/// mirrored record is what a resend of the server's would rebuild, on a mirror on the server's machine.
bool storesMatch(vtbackend::Terminal const& server, vtbackend::Terminal const& mirror)
{
    auto matches = server.commandBlocks().size() == mirror.commandBlocks().size();
    server.commandBlocks().forEachRecord([&](vtbackend::CommandBlockRecord const& record) {
        auto const* const mirrored = mirror.commandBlocks().find(record.id);
        matches = matches && mirrored != nullptr
                  && *mirrored
                         == vthost::fromWireCommandBlock(
                             vthost::toWireCommandBlock(record, vthost::HostHead::Held), mirrored);
    });
    return matches;
}

/// Waits for the mirror to hold a Running block, has the host finish it, waits for the mirror to
/// see the finish, then detaches.
Task<void> finishWhileAttached(MirrorHarness* h, vtworkspace::SessionId session)
{
    co_await waitUntil(&h->loop,
                       [h] { return newestState(*h->mirror) == vtbackend::CommandBlockState::Running; });
    serverWrites(h, session, commandEnd(0));
    co_await waitUntil(&h->loop,
                       [h] { return newestState(*h->mirror) == vtbackend::CommandBlockState::Finished; });
    h->client->detach();
}
} // namespace

TEST_CASE("the attach snapshot's command blocks are adopted without finish events",
          "[vthost][mirror][blocks]")
{
    auto m = BlockMirror {};
    m.snapshot(stateWith({ finishedWire(1, 0), runningWire(2) }));

    REQUIRE(m.record(1) != nullptr);
    CHECK(m.record(1)->state == vtbackend::CommandBlockState::Finished);
    REQUIRE(m.record(2) != nullptr);
    CHECK(m.record(2)->state == vtbackend::CommandBlockState::Running);
    CHECK(m.record(2)->commandLine.raw() == "cmd-2");
    CHECK(m.events.finishedBlocks.empty()); // it finished before this client looked: history, not news
}

TEST_CASE("a finish the mirror watched start raises exactly one finish event", "[vthost][mirror][blocks]")
{
    auto m = BlockMirror {};
    m.snapshot(stateWith({ runningWire(2) }));
    m.increment({ finishedWire(2, 3) });

    REQUIRE(m.events.finishedBlocks.size() == 1);
    CHECK(m.events.finishedBlocks.front().id == vtbackend::CommandBlockId { 2 });
    CHECK(m.events.finishedBlocks.front().exitCode == 3);
    CHECK(m.events.finishedBlocks.front().commandLine.raw() == "cmd-2");

    m.increment({});                     // a delta that says nothing about blocks
    m.increment({ finishedWire(2, 3) }); // the same record re-sent
    CHECK(m.events.finishedBlocks.size() == 1);
}

TEST_CASE("a command that started and finished between two deltas still raises one",
          "[vthost][mirror][blocks]")
{
    auto m = BlockMirror {};
    m.snapshot(stateWith({ finishedWire(1, 0) }));
    m.increment({ finishedWire(2, 1) }); // never seen Running: it was quicker than one debounce window

    REQUIRE(m.events.finishedBlocks.size() == 1);
    CHECK(m.events.finishedBlocks.front().id == vtbackend::CommandBlockId { 2 });
}

TEST_CASE("an older record the mirror never held is adopted silently", "[vthost][mirror][blocks]")
{
    // The remainder of a capped snapshot, or a record this client's own bound evicted: history. A
    // reported finish only ever happens to the host's NEWEST record, so this cannot swallow news.
    auto m = BlockMirror {};
    m.snapshot(stateWith({ finishedWire(10, 0) }));
    m.increment({ finishedWire(4, 1) });

    CHECK(m.record(4) != nullptr);
    CHECK(m.events.finishedBlocks.empty());
}

TEST_CASE("a retired record leaves the mirror's store", "[vthost][mirror][blocks]")
{
    auto m = BlockMirror {};
    m.snapshot(stateWith({ finishedWire(1, 0), runningWire(2) }));
    auto const stampOfOne = m.terminal->commandBlocks().stampedRecords().front().changeStamp;
    m.increment({}, { 2 });

    CHECK(m.record(2) == nullptr);
    REQUIRE(m.record(1) != nullptr); // everything else is kept, as it was
    CHECK(m.record(1)->state == vtbackend::CommandBlockState::Finished);
    // In place: the record kept was not taken out and adopted again.
    CHECK(m.terminal->commandBlocks().stampedRecords().front().changeStamp == stampOfOne);
    CHECK(m.events.finishedBlocks.empty()); // and announced nothing
}

TEST_CASE("a sync adopts only the records that changed since the last one", "[vthost][mirror][blocks]")
{
    // The table is not walked per delta: the records a sync looks at are the ones its revision index names
    // as changed. One the store lost meanwhile, unchanged in the table, is therefore not adopted again.
    auto m = BlockMirror {};
    m.snapshot(stateWith({ finishedWire(1, 0), finishedWire(2, 0) }));
    m.terminal->commandBlocks().retire(std::array { vtbackend::CommandBlockId { 1 } });

    m.increment({ runningWire(3) });

    CHECK(m.record(1) == nullptr);
    CHECK(m.record(2) != nullptr);
    CHECK(m.record(3) != nullptr);
}

TEST_CASE("the host's ids starting over leave the mirror the new epoch's records alone",
          "[vthost][mirror][blocks]")
{
    // At the end of the id space the host forgets every record and mints 1 again, and the walk that
    // sends the new 1 retires every old id in the same delta. Were the new 1 adopted while the old ids
    // were still held, it would sort as the oldest record: history, its finish never announced.
    constexpr auto LastId = std::numeric_limits<uint32_t>::max() - 1; // the largest id the host mints
    auto m = BlockMirror {};
    m.snapshot(stateWith({ finishedWire(LastId - 1, 0), runningWire(LastId) }));
    m.increment({ finishedWire(1, 4) }, { LastId - 1, LastId });

    CHECK(m.record(LastId - 1) == nullptr);
    CHECK(m.record(LastId) == nullptr);
    REQUIRE(m.record(1) != nullptr);
    REQUIRE(m.events.finishedBlocks.size() == 1);
    CHECK(m.events.finishedBlocks.front().id == vtbackend::CommandBlockId { 1 });
    CHECK(m.events.finishedBlocks.front().exitCode == 4);
}

TEST_CASE("a row the host no longer sends forgets its id when the host's ids start over",
          "[vthost][mirror][blocks]")
{
    // The host forgets its own rows' ids as its ids start over, and sends the rows it still holds again; a
    // row only the mirror holds -- its deeper scrollback -- it never sends. Kept, that row's id would name
    // the new epoch's block of the same number: its colour, its tooltip, the target of its actions.
    constexpr auto LastId = std::numeric_limits<uint32_t>::max() - 1;
    auto m = BlockMirror {};
    auto first = rowAt(10, "$ one"); // the session's first block, whose record left both stores long ago
    first.blockId = 1;
    auto last = rowAt(11, "$ max");
    last.blockId = LastId;
    m.snapshot(stateWith({ runningWire(LastId) }), { first, last, rowAt(12) });
    auto const& grid = m.terminal->primaryScreen().grid();
    REQUIRE(grid.lineAt(vtbackend::LineOffset(0)).blockId() == vtbackend::CommandBlockId { 1 });

    // The next prompt starts the ids over: the host retires LastId, mints 1, and re-sends the rows it holds.
    auto startedOver = proto::Delta {};
    startedOver.stableViewportBase = 10;
    startedOver.stableFloor = 10;
    startedOver.commandBlockIdEpoch = 1;
    startedOver.commandBlocks = { promptingWire(1) };
    startedOver.retiredCommandBlocks = { LastId };
    auto prompt = rowAt(12, "$ new");
    prompt.blockId = 1;
    prompt.flags = static_cast<uint16_t>(vtbackend::LineFlags { vtbackend::LineFlag::Marked }.value());
    startedOver.lines = { rowAt(11, "$ max"), prompt };
    m.screen.apply(startedOver);
    m.mirror->apply(m.screen, startedOver);

    CHECK(grid.lineAt(vtbackend::LineOffset(0)).blockId() == vtbackend::CommandBlockId {});
    CHECK(grid.lineAt(vtbackend::LineOffset(2)).blockId() == vtbackend::CommandBlockId { 1 });
    auto const* const block = m.terminal->commandBlockAt(vtbackend::LineOffset(0));
    CHECK((block == nullptr || block->id != vtbackend::CommandBlockId { 1 }));

    // A later update of the same epoch forgets nothing more.
    auto later = proto::Delta {};
    later.stableViewportBase = 10;
    later.stableFloor = 10;
    later.commandBlockIdEpoch = 1;
    m.screen.apply(later);
    m.mirror->apply(m.screen, later);
    CHECK(grid.lineAt(vtbackend::LineOffset(2)).blockId() == vtbackend::CommandBlockId { 1 });
}

TEST_CASE("a resync snapshot still reports a finish the mirror saw start", "[vthost][mirror][blocks]")
{
    // A resize resyncs a LIVE connection: this client was attached when the command ended, so its
    // finish is owed — unlike an attach, which is the mirror's first replay.
    auto m = BlockMirror {};
    m.snapshot(stateWith({ runningWire(4) }));
    m.snapshot(stateWith({ finishedWire(4, 0) }));

    CHECK(m.events.finishedBlocks.size() == 1);
}

TEST_CASE("reattaching never replays a finish that happened while detached", "[vthost][mirror][blocks]")
{
    // A reattach is a fresh NativeController, so a fresh ScreenMirror whose first replay is Prime: this
    // first attach to an already-finished block IS the reattach case.
    auto h = MirrorHarness {};
    auto const session = newSession(h.host);
    h.serverTerminal(session)->writeToScreen(reportedCommandStart("make") + "built\r\n" + commandEnd(0));

    h.loop.blockOn(drive(&h, settleThenDetach(&h, [mirror = h.mirror.get()] {
        return newestState(*mirror) == vtbackend::CommandBlockState::Finished;
    })));

    REQUIRE(newestState(*h.mirror) == vtbackend::CommandBlockState::Finished);
    CHECK(h.mirrorEvents.finishedBlocks.empty());
    CHECK(storesMatch(*h.serverTerminal(session), *h.mirror));
}

TEST_CASE("a command finishing while attached raises exactly one finish on the client",
          "[vthost][mirror][blocks]")
{
    auto h = MirrorHarness {};
    auto const session = newSession(h.host);
    h.serverTerminal(session)->writeToScreen(reportedCommandStart("sleep%2010"));

    h.loop.blockOn(drive(&h, finishWhileAttached(&h, session)));

    REQUIRE(h.mirrorEvents.finishedBlocks.size() == 1);
    CHECK(h.mirrorEvents.finishedBlocks.front().commandLine.raw() == "sleep 10");
    CHECK(h.mirrorEvents.finishedBlocks.front().exitCode == 0);
    CHECK(storesMatch(*h.serverTerminal(session), *h.mirror));
}

namespace
{
/// One whole command cycle, as a shell with OSC 133 integration writes it: prompt, command @p name, output,
/// exit 0.
std::string commandCycle(std::string_view name)
{
    return reportedCommandStart(name) + "out\r\n" + commandEnd(0);
}

/// Waits for the attach to bring the newest records in, records what the mirror held then in @p primed,
/// has the host run one more command, waits for the mirror to see it finish, then detaches.
Task<void> runOneMoreWhileAttached(MirrorHarness* h,
                                   vtworkspace::SessionId session,
                                   std::vector<uint32_t>* primed)
{
    auto const newest = h->serverTerminal(session)->commandBlocks().currentId();
    co_await waitUntil(&h->loop, [h, newest] { return h->mirror->commandBlocks().currentId() == newest; });
    *primed = recordIds(h->mirror->commandBlocks());
    serverWrites(h, session, commandCycle("late"));
    co_await waitUntil(&h->loop, [h] { return !h->mirrorEvents.finishedBlocks.empty(); });
    h->client->detach();
}
} // namespace

TEST_CASE("a client keeping fewer records than its host mirrors the newest ones", "[vthost][mirror][blocks]")
{
    // The client's command_blocks.max_records below the host's: the attach brings the newest records in,
    // the oldest dropped first and none of them announced; a command finishing afterwards is adopted and
    // announced like on any client, the oldest giving way to it.
    auto h = MirrorHarness { vtbackend::CommandBlockStoreLimits { .maxRecords = 3 } };
    auto const session = newSession(h.host);
    for (auto const index: std::views::iota(0, 6))
        h.serverTerminal(session)->writeToScreen(commandCycle(std::format("cmd-{}", index)));
    auto const hostIds = recordIds(h.serverTerminal(session)->commandBlocks());
    REQUIRE(hostIds.size() == 6);

    auto primed = std::vector<uint32_t> {};
    h.loop.blockOn(drive(&h, runOneMoreWhileAttached(&h, session, &primed)));

    CHECK(primed == std::vector<uint32_t>(hostIds.end() - 3, hostIds.end()));
    REQUIRE(h.mirrorEvents.finishedBlocks.size() == 1);
    CHECK(h.mirrorEvents.finishedBlocks.front().commandLine.raw() == "late");
    auto const late = h.serverTerminal(session)->commandBlocks().currentId();
    CHECK(recordIds(h.mirror->commandBlocks())
          == std::vector<uint32_t> { hostIds[4], hostIds[5], late.value });
    REQUIRE(h.client->screens().contains(unbox(session)));
    CHECK(h.client->screens().at(unbox(session)).commandBlocks.size() == 3);
}

// ---------------------------------------------------------------------------------------------
// Command blocks: a record's locality, judged on the daemon's machine.
// ---------------------------------------------------------------------------------------------

namespace
{
/// @return The identity of the machine the daemon in the locality cases below runs on.
vtbackend::HostIdentity daemonHost()
{
    return vtbackend::HostIdentity { .machineId = "3deb5353d3ba43d08201c136a47ead7b", .hostname = "zeta" };
}

/// A finished record whose working directory the daemon judged local to its own machine.
proto::WireCommandBlock localWire(uint32_t id)
{
    auto block = finishedWire(id, 0);
    block.workingDirectory = "/home/user/src";
    block.locality = std::to_underlying(vtbackend::ContextLocality::Local);
    return block;
}

/// @return The locality a mirror on the machine @p client names holds for a record @p daemon called Local,
///         the mirror judged by the two identities as an attach judges it.
vtbackend::ContextLocality mirroredLocality(vtbackend::HostIdentity const& daemon,
                                            vtbackend::HostIdentity const& client)
{
    auto m =
        BlockMirror { vthost::daemonMachineOf(vthost::appSpecificIdentity(daemon.view()), client.view()) };
    m.snapshot(stateWith({ localWire(1) }));
    REQUIRE(m.record(1) != nullptr);
    return m.record(1)->workingDirectory.locality;
}
} // namespace

TEST_CASE("a record local to the daemon stays local on a client on the same machine",
          "[vthost][mirror][blocks]")
{
    SECTION("by machine id, whatever each calls itself")
    {
        CHECK(mirroredLocality(daemonHost(),
                               { .machineId = "3deb5353d3ba43d08201c136a47ead7b", .hostname = "zeta-laptop" })
              == vtbackend::ContextLocality::Local);
    }
    SECTION("by host name, ignoring case, where neither knows a machine id")
    {
        CHECK(
            mirroredLocality({ .machineId = {}, .hostname = "zeta" }, { .machineId = {}, .hostname = "ZETA" })
            == vtbackend::ContextLocality::Local);
    }
}

TEST_CASE("a record local to the daemon is foreign on a client on another machine",
          "[vthost][mirror][blocks]")
{
    // A `contour client` attached over TCP from elsewhere: a block resolving the command's directory here
    // would open a path on the wrong machine.
    SECTION("another machine id, under the same name")
    {
        CHECK(mirroredLocality(daemonHost(),
                               { .machineId = "77aa5353d3ba43d08201c136a47ead7b", .hostname = "zeta" })
              == vtbackend::ContextLocality::Foreign);
    }
    SECTION("another host name, where a machine id is unknown")
    {
        CHECK(mirroredLocality(daemonHost(), { .machineId = {}, .hostname = "eta" })
              == vtbackend::ContextLocality::Foreign);
    }
    SECTION("the same host name, where only the daemon knows a machine id")
    {
        // A daemon in WSL, a client on the Windows desktop it runs on.
        CHECK(mirroredLocality(daemonHost(), { .machineId = {}, .hostname = "zeta" })
              == vtbackend::ContextLocality::Foreign);
    }
}

TEST_CASE("a record local to a daemon that states no identity is foreign", "[vthost][mirror][blocks]")
{
    // Nothing proves the daemon is this machine, so its Local is not taken for this machine's.
    CHECK(mirroredLocality(vtbackend::HostIdentity {}, daemonHost()) == vtbackend::ContextLocality::Foreign);
}

TEST_CASE("a peer's record instants at the clock's limits are held where the mirror adopts them",
          "[vthost][mirror][blocks]")
{
    // The wire converts them as sent; the terminal's store holds them to the range it computes with.
    auto m = BlockMirror {};
    auto extreme = finishedWire(1, 0);
    extreme.promptStartedAt = std::numeric_limits<int64_t>::min();
    extreme.commandStartedAt = std::numeric_limits<int64_t>::max();
    extreme.duration = std::numeric_limits<int64_t>::max();
    m.snapshot(stateWith({ extreme }));

    REQUIRE(m.record(1) != nullptr);
    CHECK(m.record(1)->promptStartedAt == vtbackend::EarliestHeldInstant);
    CHECK(m.record(1)->commandStartedAt == vtbackend::LatestHeldInstant);
    CHECK(m.record(1)->duration == vtbackend::LongestHeldSpan);
}

TEST_CASE("every directory a record names is foreign on a client on another machine",
          "[vthost][mirror][blocks]")
{
    // Whatever the daemon judged it, a directory there is no directory here -- and a record that names none
    // keeps saying so.
    auto m = BlockMirror { vthost::DaemonMachine::Other };
    auto unknown = localWire(1);
    unknown.locality = std::to_underlying(vtbackend::ContextLocality::Unknown);
    auto foreign = localWire(2);
    foreign.locality = std::to_underlying(vtbackend::ContextLocality::Foreign);
    auto nowhere = localWire(3);
    nowhere.workingDirectory.clear();
    nowhere.locality = std::to_underlying(vtbackend::ContextLocality::Unknown);
    m.snapshot(stateWith({ unknown, foreign, nowhere }));

    REQUIRE(m.record(1) != nullptr);
    CHECK(m.record(1)->workingDirectory.locality == vtbackend::ContextLocality::Foreign);
    REQUIRE(m.record(2) != nullptr);
    CHECK(m.record(2)->workingDirectory.locality == vtbackend::ContextLocality::Foreign);
    REQUIRE(m.record(3) != nullptr);
    CHECK(m.record(3)->workingDirectory.locality == vtbackend::ContextLocality::Unknown);
}

TEST_CASE("a mirrored OSC 7 directory on another machine does not resolve as local",
          "[vthost][mirror][blocks]")
{
    // An OSC 7 URL with no host names the machine whose program wrote it: the daemon's, which here is
    // another one. Opening /home/u/proj on this machine would open a different directory, or none.
    auto const resolvedOn = [](vthost::DaemonMachine daemon, vtbackend::CwdPurpose purpose) {
        auto m = BlockMirror { daemon };
        auto state = stateWith({});
        state.cwd = "file:///home/u/proj";
        m.snapshot(std::move(state));
        auto const guard = std::lock_guard { *m.terminal };
        return m.terminal->resolveWorkingDirectory(purpose);
    };

    CHECK_FALSE(resolvedOn(vthost::DaemonMachine::Other, vtbackend::CwdPurpose::OpenLocally).has_value());
    CHECK_FALSE(resolvedOn(vthost::DaemonMachine::Other, vtbackend::CwdPurpose::Spawn).has_value());
    // Where the user is still shows, as somewhere else.
    auto const shown = resolvedOn(vthost::DaemonMachine::Other, vtbackend::CwdPurpose::Display);
    REQUIRE(shown.has_value());
    CHECK(shown->path == "/home/u/proj");
    CHECK(shown->locality == vtbackend::ContextLocality::Foreign);

    // On the daemon's own machine it is the directory it names.
    auto const local = resolvedOn(vthost::DaemonMachine::Same, vtbackend::CwdPurpose::OpenLocally);
    REQUIRE(local.has_value());
    CHECK(local->path == "/home/u/proj");
    CHECK(local->locality == vtbackend::ContextLocality::Local);
}

TEST_CASE("a mirror decodes a working directory exactly once", "[vthost][mirror][blocks]")
{
    // The host keeps an OSC 7 URL as the application wrote it, and sends it on so; a record's directory it
    // resolved, and so decoded, already. The mirror decodes the one and takes the other as it is: a '%' in
    // a directory name stays a '%' either way.
    auto m = BlockMirror {};
    auto block = localWire(1);
    block.workingDirectory = "/home/u/50%41 x";
    auto state = stateWith({ block });
    state.cwd = "file:///home/u/50%2541%20x";
    m.snapshot(std::move(state));

    REQUIRE(m.record(1) != nullptr);
    CHECK(m.record(1)->workingDirectory.path == "/home/u/50%41 x");
    auto const guard = std::lock_guard { *m.terminal };
    auto const shown = m.terminal->resolveWorkingDirectory(vtbackend::CwdPurpose::Display);
    REQUIRE(shown.has_value());
    CHECK(shown->path == "/home/u/50%41 x");
}

TEST_CASE("a mirror on the daemon's machine judges a context's derived machine id against its own",
          "[vthost][mirror]")
{
    // A daemon sends a context's machine id derived for the application, never as the context named it, so
    // the mirror names this machine the same way to compare: the locality comes out as it would here.
    auto const self = daemonHost();
    auto const resolvedFor = [&](std::string machineId) {
        auto m = BlockMirror { vthost::DaemonMachine::Same, self };
        auto shell = proto::WireContext {};
        shell.id = 1;
        shell.identifier = "shell";
        shell.type = static_cast<uint8_t>(vtbackend::ContextType::Shell);
        shell.present = static_cast<uint16_t>(vtbackend::ContextField::MachineId)
                        | static_cast<uint16_t>(vtbackend::ContextField::WorkingDirectory);
        shell.machineId = std::move(machineId);
        shell.workingDirectory = "/srv";
        auto state = stateWith({});
        state.contexts = { shell };
        state.contextChain = { 1 };
        m.snapshot(std::move(state));
        auto const guard = std::lock_guard { *m.terminal };
        return m.terminal->resolveWorkingDirectory(vtbackend::CwdPurpose::Display);
    };
    auto const localityFor = [&](std::string machineId) {
        auto const resolved = resolvedFor(std::move(machineId));
        REQUIRE(resolved.has_value());
        CHECK(resolved->path == "/srv");
        return resolved->locality;
    };

    CHECK(localityFor(vthost::appSpecificMachineId(self.machineId)) == vtbackend::ContextLocality::Local);
    CHECK(localityFor(vthost::appSpecificMachineId("77aa5353d3ba43d08201c136a47ead7b"))
          == vtbackend::ContextLocality::Foreign);
    // The id as the context named it is not what a daemon sends, and names no machine here.
    CHECK(localityFor(self.machineId) == vtbackend::ContextLocality::Foreign);
}

TEST_CASE("a demoted record re-sent unchanged is still a resend", "[vthost][mirror][blocks]")
{
    // The mirror holds Foreign where the wire says Local, so a comparison blind to the demotion would take
    // every resend for a change.
    auto m = BlockMirror { vthost::DaemonMachine::Other };
    m.snapshot(stateWith({ localWire(1) }));
    REQUIRE(m.record(1) != nullptr);
    REQUIRE(m.record(1)->workingDirectory.locality == vtbackend::ContextLocality::Foreign);
    auto const revision = m.terminal->commandBlocks().revision();

    // A change and its undoing between two syncs, as when the mirror's callback missed one: the table's
    // revision moves past the mirror's, and the record it reaches is the one the mirror already holds.
    auto changed = localWire(1);
    changed.commandLine = "changed";
    auto missed = proto::Delta {};
    missed.stableViewportBase = 10;
    missed.stableFloor = 10;
    missed.commandBlocks = { changed };
    m.screen.apply(missed);
    m.increment({ localWire(1) });

    CHECK(m.terminal->commandBlocks().revision() == revision);
    CHECK(m.record(1)->workingDirectory.locality == vtbackend::ContextLocality::Foreign);
    CHECK(m.events.finishedBlocks.empty());
}

// ---------------------------------------------------------------------------------------------
// Command blocks: a block's head, in the mirror's own row ids.
// ---------------------------------------------------------------------------------------------

namespace
{
/// @p row as a row of block @p id, carrying @p marks.
proto::WireLine blockRow(proto::WireLine row, uint32_t id, vtbackend::LineFlags marks = {})
{
    row.blockId = id;
    row.flags = static_cast<uint16_t>(marks.value());
    return row;
}

/// @return The page row of @p m's mirror that record @p id's head names, or nullopt when it has no head
/// there.
std::optional<int> mirroredHeadRow(BlockMirror const& m, uint32_t id)
{
    auto const* const record = m.record(id);
    if (record == nullptr)
        return std::nullopt;
    return vtbackend::headOffsetOf(m.terminal->primaryScreen().grid(), *record).transform([](auto head) {
        return unbox<int>(head);
    });
}

/// @return The text of the row @p terminal's newest block is headed at, or nullopt when it has no head on a
/// row.
std::optional<std::string> newestHeadText(vtbackend::Terminal const& terminal)
{
    auto const& grid = terminal.primaryScreen().grid();
    auto const records = terminal.commandBlocks().records();
    if (records.empty())
        return std::nullopt;
    return vtbackend::headOffsetOf(grid, records.back()).transform([&](vtbackend::LineOffset head) {
        return grid.lineTextTrimmed(head);
    });
}

/// Waits for the mirror to head the running block at its prompt, has the host erase the page above the
/// cursor (ED 1) and print on below it, waits for the mirror to show that, then detaches.
Task<void> eraseAboveWhileAttached(MirrorHarness* h, vtworkspace::SessionId session)
{
    co_await waitUntil(&h->loop, [h] { return newestHeadText(*h->mirror) == "$"; });
    serverWrites(h, session, "\033[1J4\r\n5\r\n");
    co_await waitUntil(&h->loop, mirrorShows(h, "5"));
    h->client->detach();
}
} // namespace

TEST_CASE("the mirror records a block's head where it wrote the head row", "[vthost][mirror][blocks]")
{
    auto m = BlockMirror {};
    m.snapshot(stateWith({ finishedWire(7, 0), runningWire(8) }),
               { blockRow(rowAt(10, "$ ls"), 7, vtbackend::LineFlag::Marked),
                 blockRow(rowAt(11, "a"), 7),
                 rowAt(12) });

    auto const& grid = m.terminal->primaryScreen().grid();
    REQUIRE(m.record(7) != nullptr);
    CHECK(m.record(7)->headStableId == grid.stableLineIdOf(vtbackend::LineOffset(0)));
    CHECK(m.record(7)->headIdGeneration == grid.stableIdGeneration());
    // Block 8's head row never arrived: its head is unknown — never a row id from the host's space.
    REQUIRE(m.record(8) != nullptr);
    CHECK(m.record(8)->headIdGeneration == vtbackend::UnknownHeadGeneration);
}

TEST_CASE("a mirrored record's head resolves to the mirror's own head row", "[vthost][mirror][blocks]")
{
    auto h = MirrorHarness {};
    auto const session = newSession(h.host);
    h.serverTerminal(session)->writeToScreen(reportedCommandStart("ls") + "file\r\n" + commandEnd(0));

    h.loop.blockOn(drive(&h, settleThenDetach(&h, [mirror = h.mirror.get()] {
        return newestState(*mirror) == vtbackend::CommandBlockState::Finished;
    })));

    auto const* const record = h.mirror->commandBlocks().find(h.mirror->commandBlocks().currentId());
    REQUIRE(record != nullptr);
    auto const& grid = h.mirror->primaryScreen().grid();
    REQUIRE(record->headIdGeneration == grid.stableIdGeneration());

    auto heads = 0;
    for (auto const row: std::views::iota(0, unbox<int>(grid.pageSize().lines)))
    {
        if (grid.stableLineIdOf(vtbackend::LineOffset(row)) != record->headStableId)
            continue;
        auto const& line = grid.lineAt(vtbackend::LineOffset(row));
        CHECK(line.blockId() == record->id);
        CHECK(line.flags().contains(vtbackend::LineFlag::Marked));
        ++heads;
    }
    CHECK(heads == 1);
}

TEST_CASE("a Discard replay re-anchors heads", "[vthost][mirror][blocks]")
{
    // A Discard replay that rebuilds history re-streams every row under ids the mirror's grid mints anew,
    // while its stable-id generation stays put (Grid::clearHistory() is deliberately no bump): a head kept
    // from the first replay names a row that now holds something else.
    auto m = BlockMirror {};
    auto const head = blockRow(rowAt(10, "$ ls"), 7, vtbackend::LineFlag::Marked);
    m.snapshot(stateWith({ finishedWire(7, 0) }), { head, rowAt(11), rowAt(12) });

    auto const& grid = m.terminal->primaryScreen().grid();
    REQUIRE(m.record(7) != nullptr);
    auto const firstHead = m.record(7)->headStableId;
    REQUIRE(firstHead == grid.stableLineIdOf(vtbackend::LineOffset(0)));

    // A host-side reset: a generation bump at an unchanged size is a Discard replay. Three rows of
    // history above the page are streamed through it first, so the head row (host id 10) scrolls in
    // at the bottom under a newly minted mirror id, and the id the first replay recorded ends up on
    // the blank history row 7.
    auto rebuilt = proto::Delta {};
    rebuilt.snapshot = 1;
    rebuilt.generation = 1;
    rebuilt.stableViewportBase = 10;
    rebuilt.stableFloor = 7;
    rebuilt.lines = { rowAt(7), rowAt(8), rowAt(9), head, rowAt(11), rowAt(12) };
    m.screen.apply(rebuilt);
    m.mirror->apply(m.screen, rebuilt);
    REQUIRE(grid.historyLineCount() == vtbackend::LineCount(3));

    REQUIRE(m.record(7) != nullptr);
    CHECK(m.record(7)->headStableId != firstHead);
    CHECK(m.record(7)->headStableId == grid.stableLineIdOf(vtbackend::LineOffset(0))); // row 10, page top
    CHECK(m.record(7)->headIdGeneration == grid.stableIdGeneration());
}

TEST_CASE("a replay that writes other rows under the head's id re-heads by the rule",
          "[vthost][mirror][blocks]")
{
    // A host whose history limit changed renumbers its rows, and the replay that follows writes the page with
    // other rows than before under the ids the mirror's rows already have. The id the head had now holds the
    // block's output start: a row of the block carrying a head mark, which an erase would have left alone.
    auto m = BlockMirror {};
    m.snapshot(stateWith({ finishedWire(7, 0) }),
               { rowAt(10),
                 blockRow(rowAt(11, "$ ls"), 7, vtbackend::LineFlag::Marked),
                 blockRow(rowAt(12, "a"), 7, vtbackend::LineFlag::OutputStart) });
    REQUIRE(mirroredHeadRow(m, 7) == 1);

    auto shifted = proto::Delta {};
    shifted.snapshot = 1;
    shifted.generation = 1;
    shifted.stableViewportBase = 11;
    shifted.stableFloor = 11;
    shifted.lines = { blockRow(rowAt(11, "$ ls"), 7, vtbackend::LineFlag::Marked),
                      blockRow(rowAt(12, "a"), 7, vtbackend::LineFlag::OutputStart),
                      rowAt(13) };
    m.screen.apply(shifted);
    m.mirror->apply(m.screen, shifted);

    CHECK(mirroredHeadRow(m, 7) == 0);
}

TEST_CASE("the mirror heads a block at its lowest Marked row, as the host does", "[vthost][mirror][blocks]")
{
    // An erase under the cursor hands an older prompt's row, its mark kept, to the running block: above that
    // block's own prompt. A replay writes it first, top-down; the host heads the block at the lower one.
    auto m = BlockMirror {};
    m.snapshot(stateWith({ runningWire(7) }),
               { blockRow(rowAt(10, "$ old"), 7, vtbackend::LineFlag::Marked),
                 blockRow(rowAt(11, "$ ls"), 7, vtbackend::LineFlag::Marked),
                 blockRow(rowAt(12), 7) });

    CHECK(mirroredHeadRow(m, 7) == 1);
}

TEST_CASE("a record that arrives after its head row is headed when it arrives", "[vthost][mirror][blocks]")
{
    // A capped snapshot states the newest records; the older ones follow as increments, after the rows they
    // head were streamed.
    auto m = BlockMirror {};
    m.snapshot(stateWith({ finishedWire(9, 0) }),
               { blockRow(rowAt(10, "$ ls"), 4, vtbackend::LineFlag::Marked),
                 rowAt(11),
                 blockRow(rowAt(12, "$"), 9, vtbackend::LineFlag::Marked) });
    REQUIRE(m.record(4) == nullptr);

    m.increment({ finishedWire(4, 0) });

    CHECK(mirroredHeadRow(m, 4) == 0);
    CHECK(mirroredHeadRow(m, 9) == 2);
}

TEST_CASE("a head row that arrives after its record heads it", "[vthost][mirror][blocks]")
{
    auto m = BlockMirror {};
    m.snapshot(stateWith({ runningWire(7) }));
    REQUIRE(m.record(7) != nullptr);
    REQUIRE(m.record(7)->headIdGeneration == vtbackend::UnknownHeadGeneration);

    m.increment({}, {}, { blockRow(rowAt(11, "$ ls"), 7, vtbackend::LineFlag::Marked) });

    CHECK(mirroredHeadRow(m, 7) == 1);
}

TEST_CASE("a host erase that takes a block's head row re-heads the mirrored block",
          "[vthost][mirror][blocks]")
{
    // The host re-heads the block at the next row of it a head is found by. The mirror is told only that the
    // row was rewritten.
    auto m = BlockMirror {};
    m.snapshot(stateWith({ runningWire(7) }),
               { blockRow(rowAt(10, "$ seq"), 7, vtbackend::LineFlag::Marked),
                 blockRow(rowAt(11, "1"), 7, vtbackend::LineFlag::OutputStart),
                 blockRow(rowAt(12, "2"), 7) });
    REQUIRE(mirroredHeadRow(m, 7) == 0);

    m.increment({}, {}, { rowAt(10) });

    CHECK(mirroredHeadRow(m, 7) == 1);
}

TEST_CASE("a mirrored block is re-headed where the host re-heads it after an erase",
          "[vthost][mirror][blocks]")
{
    // `seq 1 3; printf '\e[1J'; seq 4 5`, run while attached: ED 1 blanks the prompt row and the output
    // start's row to no block, so the host heads the block at the topmost row still carrying its id.
    auto h = MirrorHarness {};
    auto const session = newSession(h.host);
    h.serverTerminal(session)->writeToScreen(reportedCommandStart("seq") + "1\r\n2\r\n3\r\n");

    h.loop.blockOn(drive(&h, eraseAboveWhileAttached(&h, session)));

    REQUIRE(newestHeadText(*h.serverTerminal(session)) == "4"); // the probe engaged: the host re-headed it
    CHECK(newestHeadText(*h.mirror) == "4");
}

TEST_CASE("an attached client shows the placeholder for a block whose head row the host evicted",
          "[vthost][mirror][blocks][stickyheader]")
{
    // `make` prints far more than the host's history keeps, so its prompt row and its ;C row have both left
    // the host before the client attaches. The client is never sent them, and nothing on its side tells a
    // head row that left from one yet to arrive: the host says which, and the very top of the client's
    // scrollback then names the command as the host's own does.
    auto h = MirrorHarness {};
    auto const session = newSession(h.host);
    auto const lines = 3 * unbox<int>(ServerHistoryLines);
    auto output = reportedCommandStart("make");
    for (auto const line: std::views::iota(0, lines))
        output += std::format("out {}\r\n", line);
    h.serverTerminal(session)->writeToScreen(output);
    auto const& serverBlocks = h.serverTerminal(session)->commandBlocks();
    REQUIRE(serverBlocks.size() == 1);
    REQUIRE(vtbackend::isHeadEvicted(h.serverTerminal(session)->primaryScreen().grid(),
                                     serverBlocks.records().front())); // the probe engaged

    h.loop.blockOn(
        drive(&h, settleThenDetach(&h, [&h, settled = mirrorShows(&h, std::format("out {}", lines - 1))] {
            return settled() && newestState(*h.mirror) == vtbackend::CommandBlockState::Running;
        })));

    REQUIRE(h.mirror->viewport().scrollToTop());
    auto const header = vtbackend::test::renderOf(*h.mirror).stickyHeader;
    REQUIRE(header.has_value());
    CHECK(header->kind == vtbackend::StickyHeaderKind::Evicted);
    auto text = std::u32string {};
    for (auto const& cell: header->cells)
        text += cell.codepoints;
    CHECK(text == U"⋯ make — earlier output evicted");
}

namespace
{
/// Waits for the mirror to settle on the host's running command, notes in @p neverHeld whether the client
/// lacks its head row, then has the host print @p lines more lines, a few at a time: each batch pushed and
/// mirrored before the next, so every frame is an increment and none states the records afresh. Then waits
/// for the client to learn the head row is gone, and detaches.
Task<void> printPastTheHeadWhileAttached(MirrorHarness* h,
                                         vtworkspace::SessionId session,
                                         int lines,
                                         bool* neverHeld)
{
    auto const& mirrorGrid = h->mirror->primaryScreen().grid();
    auto const newest = [h] {
        return h->mirror->commandBlocks().records().back();
    };
    co_await waitUntil(&h->loop,
                       [h] { return newestState(*h->mirror) == vtbackend::CommandBlockState::Running; });
    *neverHeld = !vtbackend::headOffsetOf(mirrorGrid, newest()).has_value()
                 && !vtbackend::isHeadEvicted(mirrorGrid, newest());

    constexpr auto Batch = 10;
    for (auto const batch: std::views::iota(0, lines / Batch))
    {
        auto text = std::string {};
        for (auto const line: std::views::iota(batch * Batch, (batch + 1) * Batch))
            text += std::format("more {}\r\n", line);
        serverWrites(h, session, text);
        co_await waitUntil(&h->loop, mirrorShows(h, std::format("more {}", ((batch + 1) * Batch) - 1)));
    }
    co_await waitUntil(&h->loop, [&] { return vtbackend::isHeadEvicted(mirrorGrid, newest()); });
    h->client->detach();
}
} // namespace

TEST_CASE("an attached client that never held a block's head row learns when the host lets it go",
          "[vthost][mirror][blocks][stickyheader]")
{
    // The client keeps less history than the host. It attaches with `make`'s prompt still in the host's
    // history but deeper than its own, so it never holds that row, and nothing on its side can tell when the
    // row leaves the host. The host's head going is a change of the record, so the next increment carries it
    // again, saying so, and the very top of the client's scrollback then names the command.
    auto h =
        MirrorHarness { {}, vtbackend::HistoryLimits::plain(ServerHistoryLines), vtbackend::LineCount(10) };
    auto const session = newSession(h.host);
    auto* const server = h.serverTerminal(session);
    auto const page = unbox<int>(server->pageSize().lines);
    auto output = reportedCommandStart("make");
    for (auto const line: std::views::iota(0, page + 20))
        output += std::format("out {}\r\n", line);
    server->writeToScreen(output);
    auto const headEvictedOnServer = [server] {
        return vtbackend::isHeadEvicted(server->primaryScreen().grid(),
                                        server->commandBlocks().records().back());
    };
    REQUIRE_FALSE(headEvictedOnServer()); // the host still holds the head row

    auto neverHeld = false;
    h.loop.blockOn(drive(
        &h, printPastTheHeadWhileAttached(&h, session, 2 * unbox<int>(ServerHistoryLines), &neverHeld)));
    REQUIRE(neverHeld);             // the probe engaged: the client attached without the head row
    REQUIRE(headEvictedOnServer()); // and the host let it go while the client was attached

    REQUIRE(h.mirror->viewport().scrollToTop());
    auto const header = vtbackend::test::renderOf(*h.mirror).stickyHeader;
    REQUIRE(header.has_value());
    CHECK(header->kind == vtbackend::StickyHeaderKind::Evicted);
}

namespace
{
/// Waits for the mirror to head the running block at its prompt, noting in @p heldTheHead whether it did.
/// Then the host's command steps up into the older command's output and erases from there to the end of
/// the page, which takes every row the block has, and prints @p lines lines on below. The erase and the
/// lines are each pushed and mirrored before the next is written, and neither wait reads the mirror's
/// store, which would have it look at its heads in between. Then detaches.
Task<void> eraseTheBlockThenPrintWhileAttached(MirrorHarness* h,
                                               vtworkspace::SessionId session,
                                               int lines,
                                               bool* heldTheHead)
{
    co_await waitUntil(&h->loop, [h] { return newestHeadText(*h->mirror) == "$"; });
    *heldTheHead = newestHeadText(*h->mirror) == "$";

    serverWrites(h, session, "\033[3;3H\033[J");
    co_await waitUntil(&h->loop, [shown = mirrorShows(h, "out 2")] { return !shown(); });

    auto text = std::string { "\r\n" };
    for (auto const line: std::views::iota(0, lines))
        text += std::format("more {}\r\n", line);
    serverWrites(h, session, text);
    co_await waitUntil(&h->loop, mirrorShows(h, std::format("more {}", lines - 1)));
    h->client->detach();
}

/// Scrolls @p terminal's viewport to have the row reading @p text on top, and draws a frame.
/// @return The kind of the sticky header the frame shows, or nullopt for none.
std::optional<vtbackend::StickyHeaderKind> headerKindOver(vtbackend::Terminal& terminal,
                                                          std::string_view text)
{
    auto const& grid = terminal.primaryScreen().grid();
    auto const rows =
        std::views::iota(-unbox<int>(grid.historyLineCount()), unbox<int>(grid.pageSize().lines));
    auto const row = std::ranges::find_if(
        rows, [&](int offset) { return grid.lineTextTrimmed(vtbackend::LineOffset(offset)) == text; });
    REQUIRE(row != rows.end());
    REQUIRE(terminal.viewport().scrollLineToTop(vtbackend::LineOffset(*row)));
    return vtbackend::test::renderOf(terminal).stickyHeader.transform(
        [](auto const& header) { return header.kind; });
}
} // namespace

TEST_CASE("an attached client learns when an erase takes every row of a block whose head it holds",
          "[vthost][mirror][blocks][stickyheader]")
{
    // `make` steps up into the output of the `ls` before it and erases to the end of the page, which takes
    // every row `make` has, its prompt with them: the host's head is gone as surely as one the history let
    // go of. The client erases the same rows, but looks at its heads only when something reads them, and by
    // then `make` has printed on below, on rows that carry it again -- the topmost would pass for its head.
    // So the host says the head is gone with the next increment, and where it shows the placeholder over the
    // first of those rows, so does the client.
    auto h = MirrorHarness {};
    auto const session = newSession(h.host);
    auto* const server = h.serverTerminal(session);
    server->writeToScreen(commandRun("ls", { "a", "b" }) + reportedCommandStart("make")
                          + "out 0\r\nout 1\r\nout 2\r\n");
    auto const lines = unbox<int>(server->pageSize().lines) + 5; // `more 0` ends up in the history

    auto heldTheHead = false;
    h.loop.blockOn(drive(&h, eraseTheBlockThenPrintWhileAttached(&h, session, lines, &heldTheHead)));
    REQUIRE(heldTheHead); // the probe engaged: the client had headed `make` before the erase
    // And the host's head went with the erase: its history let no row go, and the rows printed since carry
    // `make` again.
    REQUIRE(
        vtbackend::isHeadEvicted(server->primaryScreen().grid(), server->commandBlocks().records().back()));

    CHECK(headerKindOver(*server, "more 0") == vtbackend::StickyHeaderKind::Evicted);
    CHECK(headerKindOver(*h.mirror, "more 0") == vtbackend::StickyHeaderKind::Evicted);
}

TEST_CASE("a client whose clock is behind the host's reads a running command's time as the host does",
          "[vthost][mirror][blocks][stickyheader]")
{
    // The command started on the host 40 s ago by the host's clock; the client's clock is 40 s behind, so
    // by its own clock the host's start instant is now. Read against its own clock, a running command's
    // time is the skew, not how long it ran.
    using vthost::testing::PromptedAt;
    auto const startedAt = vthost::fromWireTime(runningWire(2).commandStartedAt);
    auto steadyClock = core::platform::ManualClock {};
    auto wallClock = core::platform::ManualWallClock { startedAt };
    auto m = BlockMirror { vthost::DaemonMachine::Other,
                           {},
                           vtbackend::TerminalClocks { .steady = steadyClock,
                                                       .wall = wallClock,
                                                       .localTime = &vtbackend::systemLocalTime } };

    auto state = stateWith({ runningWire(2) });
    state.hostTime = vthost::toWireTime(startedAt + 40s);
    m.snapshot(std::move(state));

    REQUIRE(m.record(2) != nullptr);
    CHECK(m.record(2)->commandStartedAt == startedAt - 40s);
    CHECK(m.record(2)->promptStartedAt == vthost::fromWireTime(PromptedAt) - 40s);
    CHECK(vtbackend::stickyHeaderChip(*m.record(2), wallClock.now()) == U"running 40s");
}

TEST_CASE("a client whose clock is behind the host's reads a row's time as it reads its block's",
          "[vthost][mirror][blocks][gutter]")
{
    // The host's session began as the prompt was drawn, and its command started a second later, by the
    // host's clock; the client's clock is 40 s behind. The prompt's row and the output's first row were born
    // at those instants: read on the client's clock, the gutter says what the block's tooltip says.
    using vthost::testing::PromptedAt;
    auto const promptedAt = vthost::fromWireTime(PromptedAt);
    auto steadyClock = core::platform::ManualClock {};
    auto wallClock = core::platform::ManualWallClock { promptedAt };
    auto m = BlockMirror { vthost::DaemonMachine::Other,
                           {},
                           vtbackend::TerminalClocks { .steady = steadyClock,
                                                       .wall = wallClock,
                                                       .localTime = &vtbackend::systemLocalTime } };

    auto state = stateWith({ runningWire(2) });
    state.hostTime = vthost::toWireTime(promptedAt + 40s);
    state.sessionEpoch = PromptedAt;
    auto prompt = rowAt(10, "$ cmd");
    prompt.blockId = 2;
    prompt.bornAt = 1; // the session's first second
    auto output = rowAt(11, "out");
    output.blockId = 2;
    output.bornAt = 2; // the second after
    m.snapshot(std::move(state), { prompt, output, rowAt(12) });

    auto const* const record = m.record(2);
    REQUIRE(record != nullptr);
    REQUIRE(record->promptStartedAt == promptedAt - 40s); // the block reads its instants on this clock
    auto const& grid = m.terminal->primaryScreen().grid();
    CHECK(m.terminal->lineBirthTime(grid.lineAt(vtbackend::LineOffset(0))) == record->promptStartedAt);
    CHECK(m.terminal->lineBirthTime(grid.lineAt(vtbackend::LineOffset(1))) == record->commandStartedAt);
}

TEST_CASE("a client on the daemon's own machine leaves the host's instants as they are",
          "[vthost][mirror][blocks]")
{
    // One machine, one clock: whatever the two readings differ by is the time the reading took to arrive.
    auto steadyClock = core::platform::ManualClock {};
    auto wallClock = core::platform::ManualWallClock { vthost::fromWireTime(vthost::testing::PromptedAt) };
    auto m = BlockMirror { vthost::DaemonMachine::Same,
                           {},
                           vtbackend::TerminalClocks { .steady = steadyClock,
                                                       .wall = wallClock,
                                                       .localTime = &vtbackend::systemLocalTime } };

    auto state = stateWith({ runningWire(2) });
    state.hostTime = vthost::testing::PromptedAt + 40'000'000'000;
    m.snapshot(std::move(state));

    REQUIRE(m.record(2) != nullptr);
    CHECK(m.record(2)->commandStartedAt == vthost::fromWireTime(runningWire(2).commandStartedAt));
}

// ---------------------------------------------------------------------------------------------
// User marks: the host's, mirrored with its rows, and indexed for the scrollbar in the mirror's own ids.
// ---------------------------------------------------------------------------------------------

namespace
{

/// Presses and releases each of @p keys on @p terminal, as a keyboard would.
void pressKeys(vtbackend::Terminal& terminal, std::string_view keys)
{
    auto const now = vtbackend::Terminal::Timestamp {};
    for (auto const key: keys)
    {
        auto const codepoint = static_cast<char32_t>(key);
        auto const identity = vtbackend::KeyIdentity { .unshiftedKey = codepoint,
                                                       .nativeVirtualKey = static_cast<uint32_t>(key) };
        for (auto const type: { vtbackend::KeyboardEventType::Press, vtbackend::KeyboardEventType::Release })
            (void) terminal.sendCharEvent(codepoint, identity, vtbackend::KeyboardModifiers {}, type, now);
    }
}

/// @return @p row as the host sends it with the user's mark on it.
proto::WireLine userMarked(proto::WireLine row)
{
    row.flags = static_cast<uint16_t>(vtbackend::LineFlags { vtbackend::LineFlag::UserMark }.value());
    return row;
}

/// The rows of @p terminal's page that carry a vi mark, by their text.
std::vector<std::string> markedRows(vtbackend::Terminal const& terminal)
{
    auto const& grid = terminal.primaryScreen().grid();
    auto marked = std::vector<std::string> {};
    for (auto const row: std::views::iota(0, unbox<int>(terminal.pageSize().lines)))
        if (grid.lineAt(vtbackend::LineOffset(row)).isFlagEnabled(vtbackend::LineFlag::UserMark))
            marked.push_back(vtbackend::test::trimRight(grid.lineText(vtbackend::LineOffset(row))));
    return marked;
}

/// Sends the host every vi mark @p events heard the user ask of @p mirror's terminal, as a frontend does: by
/// the host's row, which only the mirror can name.
void forwardUserMarks(RecordingEvents& events,
                      ScreenMirror const& mirror,
                      NativeClient& client,
                      vtworkspace::SessionId session)
{
    for (auto const& request: std::exchange(events.userMarkRequests, {}))
        if (auto const row = mirror.hostRowOf(request.stableId, request.generation))
            client.performOperation(
                proto::PerformOperation { .session = session.value,
                                          .operation = request.change == vtbackend::UserMarkChange::Added
                                                           ? proto::EngineOperation::SetUserMark
                                                           : proto::EngineOperation::ClearUserMark,
                                          .row = *row });
}

} // namespace

TEST_CASE("a user mark a host row carries reaches the mirror's mark index",
          "[vthost][mirror][scrollbar-marks]")
{
    // A row arriving with the mark never passes the funnel a local mark is written through: the index hears
    // of it from the mirror, or the scrollbar never ticks it.
    auto m = BlockMirror {};
    auto const& grid = m.terminal->primaryScreen().grid();
    auto const idOfRow = [&](int row) {
        return grid.stableLineIdOf(vtbackend::LineOffset(row));
    };

    m.snapshot(stateWith({}), { rowAt(10, "a"), userMarked(rowAt(11, "b")), rowAt(12, "c") });
    CHECK(m.terminal->userMarks().ids() == std::set { idOfRow(1) });

    // The host's row is the whole truth about its mark: a rewrite in place brings one and takes one away.
    m.increment({}, {}, { rowAt(11, "b"), userMarked(rowAt(12, "c")) });
    CHECK(m.terminal->userMarks().ids() == std::set { idOfRow(2) });

    // A replay of renumbered rows -- a host-side reset -- brings none of the marked rows back, so the marks
    // go, and the index hears.
    auto renumbered = proto::Delta {};
    renumbered.snapshot = 1;
    renumbered.generation = 1;
    renumbered.stableViewportBase = 10;
    renumbered.stableFloor = 10;
    renumbered.lines = { rowAt(10, "a"), rowAt(11, "b"), rowAt(12, "c") };
    m.screen.apply(renumbered);
    m.mirror->apply(m.screen, renumbered);
    CHECK(m.terminal->userMarks().ids().empty());
}

TEST_CASE("a vi mark on a mirror is the host's to place", "[vthost][mirror][scrollbar-marks]")
{
    // The mirror's line is left alone: it takes the mark with the host's row, the same row every other client
    // of the session takes it with.
    auto m = BlockMirror {};
    m.snapshot(stateWith({}), { rowAt(10, "a"), rowAt(11, "b"), rowAt(12, "c") });
    auto const& grid = m.terminal->primaryScreen().grid();

    // Vi's cursor starts where the terminal's is: on "b".
    m.terminal->primaryScreen().moveCursorTo(vtbackend::LineOffset(1), vtbackend::ColumnOffset(0));
    m.terminal->inputHandler().setMode(vtbackend::ViMode::Normal);
    pressKeys(*m.terminal, "mm");
    m.terminal->inputHandler().setMode(vtbackend::ViMode::Insert);

    REQUIRE(m.events.userMarkRequests.size() == 1);
    auto const request = m.events.userMarkRequests.front();
    CHECK(request.stableId == grid.stableLineIdOf(vtbackend::LineOffset(1)));
    CHECK(request.change == vtbackend::UserMarkChange::Added);
    CHECK_FALSE(grid.lineAt(vtbackend::LineOffset(1)).isFlagEnabled(vtbackend::LineFlag::UserMark));
    CHECK(m.terminal->userMarks().ids().empty());

    SECTION("the host is asked for its own row")
    {
        CHECK(m.mirror->hostRowOf(request.stableId, request.generation)
              == proto::RowAddress { .stableId = 11 });
    }

    SECTION("the host is asked for its own row after the page shrank under the vi status line")
    {
        // The status line takes a line of the page, and with the cursor on the bottom one the page's top row
        // goes into the history: every row's offset moves and no row's id does, until the host's page comes,
        // resized to match.
        m.terminal->primaryScreen().moveCursorTo(vtbackend::LineOffset(2), vtbackend::ColumnOffset(0));
        m.terminal->setStatusDisplay(vtbackend::StatusDisplayType::Indicator);
        REQUIRE(grid.lineOffsetOf(request.stableId) == vtbackend::LineOffset(0));
        CHECK(m.mirror->hostRowOf(request.stableId, request.generation)
              == proto::RowAddress { .stableId = 11 });
    }

    SECTION("a row the mirror cannot name on the host is not named")
    {
        // In another generation of the mirror's grid, the id may name another row.
        CHECK_FALSE(m.mirror->hostRowOf(request.stableId, request.generation + 1).has_value());
        // While the alternate page is shown, the numbers the mirror follows are that page's.
        auto alternate = stateWith({});
        alternate.screenType = 1;
        m.snapshot(std::move(alternate), { rowAt(20, "x"), rowAt(21, "y"), rowAt(22, "z") });
        CHECK_FALSE(m.mirror->hostRowOf(request.stableId, request.generation).has_value());
    }

    SECTION("the host's row brings the mark, and takes it away")
    {
        m.increment({}, {}, { userMarked(rowAt(11, "b")) });
        CHECK(grid.lineAt(vtbackend::LineOffset(1)).isFlagEnabled(vtbackend::LineFlag::UserMark));
        CHECK(m.terminal->userMarks().ids() == std::set { grid.stableLineIdOf(vtbackend::LineOffset(1)) });
        CHECK(m.terminal->scrollbarMarkInputs().size() == 1);

        // `mm` on a marked line asks for the mark to go. The update put the cursor back where the host's is.
        m.terminal->primaryScreen().moveCursorTo(vtbackend::LineOffset(1), vtbackend::ColumnOffset(0));
        m.terminal->inputHandler().setMode(vtbackend::ViMode::Normal);
        pressKeys(*m.terminal, "mm");
        m.terminal->inputHandler().setMode(vtbackend::ViMode::Insert);
        REQUIRE_FALSE(m.events.userMarkRequests.empty());
        CHECK(m.events.userMarkRequests.back().change == vtbackend::UserMarkChange::Removed);
        CHECK(grid.lineAt(vtbackend::LineOffset(1)).isFlagEnabled(vtbackend::LineFlag::UserMark));

        m.increment({}, {}, { rowAt(11, "b") });
        CHECK_FALSE(grid.lineAt(vtbackend::LineOffset(1)).isFlagEnabled(vtbackend::LineFlag::UserMark));
        CHECK(m.terminal->userMarks().ids().empty());
        CHECK(m.terminal->scrollbarMarkInputs().empty());
    }
}

TEST_CASE("a vi mark on a remote pane stays on its row when the pane is resized",
          "[vthost][mirror][scrollbar-marks]")
{
    // A local pane's reflow carries its marks; a remote pane's mark is the host's, whose reflow carries it,
    // and whose reflowed rows bring it back.
    auto h = MirrorHarness {};
    auto const session = newSession(h.host);
    h.serverTerminal(session)->writeToScreen(numberedRows(40) + "tail-here");

    auto marked = std::vector<std::string> {};
    auto marks = std::size_t { 0 };
    auto ticks = std::size_t { 0 };
    auto scenario = [](MirrorHarness* h,
                       vtworkspace::SessionId session,
                       std::vector<std::string>* marked,
                       std::size_t* marks,
                       std::size_t* ticks) -> Task<void> {
        co_await waitUntil(&h->loop, mirrorShows(h, "tail-here"));
        // Vi's cursor starts where the terminal's is, on "tail-here": two lines up is "row-38".
        h->mirror->inputHandler().setMode(vtbackend::ViMode::Normal);
        pressKeys(*h->mirror, "kkmm");
        h->mirror->inputHandler().setMode(vtbackend::ViMode::Insert);
        forwardUserMarks(h->mirrorEvents, *h->populator, *h->client, session);
        co_await waitUntil(&h->loop, [h] { return !markedRows(*h->mirror).empty(); });
        // CHECK, not REQUIRE: a coroutine that throws never detaches, and the loop then waits forever.
        CHECK(markedRows(*h->mirror) == std::vector<std::string> { "row-38" });

        auto const target = vtbackend::PageSize { vtbackend::LineCount(20), vtbackend::ColumnCount(60) };
        h->mirror->resizeScreen(target);
        h->client->requestResize(60, 20);
        // Parity, line flags and all: the mark is the host's, so the two ends agree on it as well.
        co_await waitUntil(&h->loop, [h, session, target] {
            return h->serverTerminal(session)->pageSize() == target
                   && vthost::testing::compareGrids(
                          *h->serverTerminal(session), *h->mirror, vthost::testing::ComparedPage::Active, 1)
                          .empty();
        });

        *marked = markedRows(*h->mirror);
        *marks = h->mirror->userMarks().ids().size();
        *ticks = h->mirror->scrollbarMarkInputs().size();
        h->client->detach();
    }(&h, session, &marked, &marks, &ticks);
    h.loop.blockOn(drive(&h, std::move(scenario)));

    REQUIRE(h.mirror->pageSize().columns == vtbackend::ColumnCount(60));
    CHECK(marked == std::vector<std::string> { "row-38" });
    CHECK(marks == 1);
    CHECK(ticks == 1);
}

// ---------------------------------------------------------------------------------------------
// Two clients of one host: a vi mark is the session's, wherever it was set.
// ---------------------------------------------------------------------------------------------

namespace
{

/// One client of a host, as MirrorHarness has one: the host's session for its connection, its client, and the
/// terminal it mirrors the hosted session into.
struct AttachedClient
{
    /// @param loop The loop both ends run on.
    /// @param host The host to attach to.
    /// @param index The connection's number, for the host's log.
    AttachedClient(core::net::EventLoop& loop, SessionHost& host, uint64_t index):
        pair { *core::net::testing::makeSocketPair(loop) },
        server { std::make_unique<NativeSession>(
            loop, host, vthost::ConnectionId { .endpoint = "test", .index = index }, std::move(pair.first)) }
    {
        auto settings = gipSettings(MirrorHistoryLines);
        mirror = std::make_unique<vtbackend::Terminal>(events,
                                                       core::defaultEnvironment(),
                                                       core::platform::NativeFileSystem::instance(),
                                                       std::make_unique<vtpty::MockPty>(settings.pageSize),
                                                       std::move(settings),
                                                       std::nullopt,
                                                       vtbackend::TerminalClocks::system(),
                                                       vtbackend::randomSemanticBlockTokens());
        populator = std::make_unique<ScreenMirror>(*mirror, MirrorHarness::Daemon);
        client = std::make_unique<NativeClient>(
            loop,
            std::move(pair.second),
            NativeClient::HandshakeOptions {},
            vtbackend::CommandBlockStoreLimits {},
            NativeClient::UpdateHandler {
                [this](vthost::client::RemoteScreen const& screen, proto::Delta const& delta) {
                    populator->apply(screen, delta);
                } },
            NativeClient::ImageHandler {},
            NativeClient::SessionEventHandler {},
            NativeClient::LayoutHandler {});
    }

    ~AttachedClient() = default;

    AttachedClient(AttachedClient const&) = delete;
    AttachedClient& operator=(AttachedClient const&) = delete;
    AttachedClient(AttachedClient&&) = delete;
    AttachedClient& operator=(AttachedClient&&) = delete;

    core::net::testing::SocketPair pair;
    std::unique_ptr<NativeSession> server;
    RecordingEvents events;
    std::unique_ptr<vtbackend::Terminal> mirror;
    std::unique_ptr<ScreenMirror> populator;
    std::unique_ptr<NativeClient> client;
};

/// A host and two clients attached to it.
struct TwoClientHarness
{
    std::unique_ptr<core::net::IoBackend> source = core::net::makeDefaultBackend();
    core::net::EventLoop loop { *source };
    SessionHost host { loop,
                       [](vtbackend::PageSize size, std::optional<vtpty::Process::ExecInfo> const&) {
                           return std::make_unique<vtpty::MockPty>(size);
                       },
                       gipSettings(ServerHistoryLines),
                       core::defaultEnvironment(),
                       vthost::SessionHostConfig { .pumping = vthost::SessionPumping::Manual,
                                                   .sizePolicy = vthost::ClientSizePolicy::Latest } };
    AttachedClient first { loop, host, 1 };
    AttachedClient second { loop, host, 2 };
};

/// Runs both connections beside @p scenario.
Task<void> drive(TwoClientHarness* h, Task<void> scenario)
{
    co_await core::async::whenAll(h->first.server->run(),
                                  h->first.client->run(),
                                  h->second.server->run(),
                                  h->second.client->run(),
                                  std::move(scenario));
}

/// What the session holds: a few lines too long for 40 columns above a short `target` line, the line a
/// client marks, four lines below it, and the cursor on the last.
std::string markedSessionText()
{
    auto text = std::string {};
    for (auto const index: std::views::iota(0, 4))
        text += std::format("long-{}-{}\r\n", index, std::string(54, '.'));
    return text + "target\r\nafter-1\r\nafter-2\r\nafter-3\r\ntail-here";
}

/// Marks `target` with vi's `mm` on @p client's terminal -- four lines up from the cursor, on "tail-here" --
/// and asks the host for the mark, as a frontend does.
void markTarget(AttachedClient& client, vtworkspace::SessionId session)
{
    client.mirror->inputHandler().setMode(vtbackend::ViMode::Normal);
    pressKeys(*client.mirror, "kkkkmm");
    client.mirror->inputHandler().setMode(vtbackend::ViMode::Insert);
    forwardUserMarks(client.events, *client.populator, *client.client, session);
}

/// @return Whether @p terminal's page shows @p needle.
bool shows(vtbackend::Terminal const& terminal, std::string_view needle)
{
    return terminal.primaryScreen().grid().renderMainPageText().contains(needle);
}

} // namespace

TEST_CASE("a vi mark one client sets shows on the other", "[vthost][mirror][scrollbar-marks]")
{
    // The mark is the session's: the host holds it, and every client mirrors the row that carries it.
    auto h = TwoClientHarness {};
    auto const session = newSession(h.host);
    h.host.terminal(session)->writeToScreen(markedSessionText());

    auto seen = std::vector<std::string> {};
    auto scenario = [](TwoClientHarness* h,
                       vtworkspace::SessionId session,
                       std::vector<std::string>* seen) -> Task<void> {
        co_await waitUntil(&h->loop, [h] {
            return shows(*h->first.mirror, "tail-here") && shows(*h->second.mirror, "tail-here");
        });
        markTarget(h->first, session);
        co_await waitUntil(&h->loop, [h] { return !markedRows(*h->second.mirror).empty(); });
        *seen = markedRows(*h->second.mirror);
        h->first.client->detach();
        h->second.client->detach();
    }(&h, session, &seen);
    h.loop.blockOn(drive(&h, std::move(scenario)));

    CHECK(seen == std::vector<std::string> { "target" });
}

TEST_CASE("a vi mark stays on its row for a client while another client resizes the session",
          "[vthost][mirror][scrollbar-marks]")
{
    // The second client narrows the session to 40 columns: the long lines above the mark wrap, and the marked
    // row moves down the page. The first client's terminal keeps its own 80 columns and reflowed nothing, so
    // only the host's row can tell it where the mark now stands.
    auto h = TwoClientHarness {};
    auto const session = newSession(h.host);
    h.host.terminal(session)->writeToScreen(markedSessionText());

    auto marked = std::vector<std::string> {};
    auto scenario = [](TwoClientHarness* h,
                       vtworkspace::SessionId session,
                       std::vector<std::string>* marked) -> Task<void> {
        co_await waitUntil(&h->loop, [h] {
            return shows(*h->first.mirror, "tail-here") && shows(*h->second.mirror, "tail-here");
        });
        markTarget(h->first, session);
        co_await waitUntil(&h->loop, [h] { return !markedRows(*h->first.mirror).empty(); });

        h->second.mirror->resizeScreen(
            vtbackend::PageSize { vtbackend::LineCount(25), vtbackend::ColumnCount(40) });
        h->second.client->requestResize(40, 25);
        co_await waitUntil(&h->loop, [h, session] {
            auto const& screens = h->first.client->screens();
            return h->host.terminal(session)->pageSize().columns == vtbackend::ColumnCount(40)
                   && screens.contains(session.value) && screens.at(session.value).columns == 40;
        });
        *marked = markedRows(*h->first.mirror);
        h->first.client->detach();
        h->second.client->detach();
    }(&h, session, &marked);
    h.loop.blockOn(drive(&h, std::move(scenario)));

    CHECK(marked == std::vector<std::string> { "target" });
}
