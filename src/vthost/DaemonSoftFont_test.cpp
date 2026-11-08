// SPDX-License-Identifier: Apache-2.0
#include <vtbackend/grid/Line.hpp>
#include <vtbackend/screen/Terminal.hpp>
#include <vtbackend/screen/TerminalClocks.hpp>

#include <vtpty/MockPty.hpp>

#include <core/async/WhenAll.hpp>
#include <core/net/EventLoop.hpp>
#include <core/net/IoBackend.hpp>
#include <core/net/testing/CoroTestSupport.hpp>
#include <core/net/testing/InMemoryTransport.hpp>
#include <core/platform/NativeFileSystem.hpp>

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <format>
#include <functional>
#include <memory>
#include <optional>
#include <ranges>
#include <string>
#include <string_view>
#include <vector>

#include <vthost/NativeSession.hpp>
#include <vthost/SessionHost.hpp>
#include <vthost/SoftFontWire.hpp>
#include <vthost/client/NativeClient.hpp>
#include <vthost/client/ScreenMirror.hpp>
#include <vthost/testing/GridParity.hpp>
#include <vthost/testing/ScenarioGuard.hpp>
#include <vthost/testing/TestHelpers.hpp>
#include <vtworkspace/Pane.hpp>
#include <vtworkspace/Tab.hpp>

using core::async::Task;
using vthost::NativeSession;
using vthost::SessionHost;
using vthost::client::NativeClient;
using vthost::client::ScreenMirror;
using vthost::testing::newSession;
namespace proto = vthost::proto;
using namespace std::chrono_literals;

namespace
{

/// DECDLD of soft font 1 under the designator " A", one glyph (for '!') of the given sixel @p value, then
/// that font designated into G1 and shifted out, so that '!' paints the glyph.
std::string fontAndGlyph(char value = '@')
{
    return std::format("\033P1;0;0;10;0;0;20;0{{ A{}\033\\\033) A\x0E", value);
}

/// Whether the offered capabilities of the client include Capability::SoftFonts.
enum class ClientOffers : uint8_t
{
    NoSoftFonts,
    SoftFonts,
};

/// The real server serving a real client whose ScreenMirror populates a terminal of its own.
struct SoftFontHarness
{
    explicit SoftFontHarness(ClientOffers offers = ClientOffers::SoftFonts,
                             std::size_t writeQueueBytes = NativeSession::DefaultWriteQueueBytes):
        mirrorOffers { offers }, queueBytes { writeQueueBytes }
    {
        auto settings = vtbackend::Settings {};
        settings.historyLimits = vtbackend::HistoryLimits::plain(vtbackend::LineCount(1000));
        mirror = std::make_unique<vtbackend::Terminal>(mirrorEvents,
                                                       core::defaultEnvironment(),
                                                       core::platform::NativeFileSystem::instance(),
                                                       std::make_unique<vtpty::MockPty>(settings.pageSize),
                                                       std::move(settings),
                                                       std::nullopt,
                                                       vtbackend::TerminalClocks::system(),
                                                       vtbackend::randomSemanticBlockTokens());
        populator = std::make_unique<ScreenMirror>(*mirror, vthost::DaemonMachine::Same);
    }

    /// Builds the connection once the server terminal holds what the case wants it to hold at attach.
    void connect()
    {
        server = std::make_unique<NativeSession>(loop,
                                                 host,
                                                 vthost::ConnectionId { .endpoint = "test", .index = 1 },
                                                 std::move(pair.first),
                                                 queueBytes);
        auto capabilities = proto::CapabilitySet {};
        if (mirrorOffers == ClientOffers::SoftFonts)
            capabilities = proto::CapabilitySet::of({ proto::Capability::SoftFonts });
        client = std::make_unique<NativeClient>(
            loop,
            std::move(pair.second),
            NativeClient::HandshakeOptions { .capabilities = std::move(capabilities) },
            vtbackend::CommandBlockStoreLimits {},
            NativeClient::UpdateHandler {
                [this](vthost::client::RemoteScreen const& screen, proto::Delta const& delta) {
                    populator->apply(screen, delta);
                } },
            NativeClient::ImageHandler {},
            NativeClient::SessionEventHandler {
                [this](vthost::client::RemoteScreen const&, proto::SessionEventPdu const& event) {
                    populator->applyEvent(event);
                } },
            NativeClient::LayoutHandler {});
    }

    [[nodiscard]] vtbackend::Terminal* serverTerminal(vtworkspace::SessionId session)
    {
        return host.terminal(session);
    }

    /// Writes @p bytes on the server terminal and pushes the resulting delta.
    void serverWrites(vtworkspace::SessionId session, std::string_view bytes)
    {
        serverTerminal(session)->writeToScreen(bytes);
        server->sessionScreenUpdated(session);
    }

    ClientOffers mirrorOffers;
    std::size_t queueBytes;
    std::unique_ptr<core::net::IoBackend> source = core::net::makeDefaultBackend();
    core::net::EventLoop loop { *source };
    SessionHost host { loop,
                       [](vtbackend::PageSize size, std::optional<vtpty::Process::ExecInfo> const&) {
                           return std::make_unique<vtpty::MockPty>(size);
                       },
                       [] {
                           auto settings = vtbackend::Settings {};
                           settings.historyLimits =
                               vtbackend::HistoryLimits::plain(vtbackend::LineCount(1000));
                           return settings;
                       }(),
                       core::defaultEnvironment(),
                       vthost::SessionHostConfig { .pumping = vthost::SessionPumping::Manual } };
    core::net::testing::SocketPair pair = *core::net::testing::makeSocketPair(loop);
    vtbackend::Terminal::NullEvents mirrorEvents;
    std::unique_ptr<vtbackend::Terminal> mirror;
    std::unique_ptr<ScreenMirror> populator;
    std::unique_ptr<NativeSession> server;
    std::unique_ptr<NativeClient> client;
};

/// Polls until @p ready holds, failing the scenario when it never does.
Task<void> requireUntil(core::net::EventLoop* loop, std::function<bool()> ready)
{
    auto const reached = co_await core::net::testing::waitUntil(loop, std::move(ready), 2000);
    REQUIRE(reached);
}

Task<void> drive(SoftFontHarness* h, Task<void> scenario)
{
    co_await core::async::whenAll(
        h->server->run(),
        h->client->run(),
        vthost::testing::guardScenario(&h->loop, [h] { h->client->detach(); }, std::move(scenario)));
}

/// @return The soft glyph the cell at (@p line, @p column) of @p terminal's main page shows, if any.
std::optional<vtbackend::SoftGlyphRef> softGlyphAt(vtbackend::Terminal& terminal, int line, uint16_t column)
{
    return terminal.primaryScreen()
        .grid()
        .lineAt(vtbackend::LineOffset(line))
        .useCellAt(vtbackend::ColumnOffset::cast_from(column))
        .softGlyph();
}

} // namespace

TEST_CASE("a font defined before attach reaches the client", "[vthost][softfont][daemon]")
{
    auto h = SoftFontHarness {};
    auto const session = newSession(h.host);
    h.serverTerminal(session)->writeToScreen(fontAndGlyph() + "!a!");
    h.connect();

    auto scenario = [](SoftFontHarness* h, vtworkspace::SessionId session) -> Task<void> {
        co_await requireUntil(&h->loop, [&] {
            return h->mirror->softFonts().resolve({ .font = 1, .position = 0x21 }) != nullptr;
        });
        co_await requireUntil(&h->loop, [&] { return softGlyphAt(*h->mirror, 0, 2).has_value(); });

        auto const serverGlyph =
            h->serverTerminal(session)->softFonts().resolve({ .font = 1, .position = 0x21 });
        auto const mirrorGlyph = h->mirror->softFonts().resolve({ .font = 1, .position = 0x21 });
        REQUIRE(serverGlyph != nullptr);
        REQUIRE(mirrorGlyph != nullptr);
        CHECK(mirrorGlyph->id == serverGlyph->id);
        CHECK(h->mirror->softFonts().fontForDesignator(" A") == 1);

        CHECK(softGlyphAt(*h->mirror, 0, 0) == vtbackend::SoftGlyphRef { .font = 1, .position = 0x21 });
        // The 'a' between them is in the shifted set too, a position the font never defined: it shows the
        // error glyph.
        CHECK(softGlyphAt(*h->mirror, 0, 1) == vtbackend::SoftGlyphRef { .font = 1, .position = 0x61 });
        CHECK(softGlyphAt(*h->mirror, 0, 2) == vtbackend::SoftGlyphRef { .font = 1, .position = 0x21 });
        auto const gaps = vthost::testing::compareGrids(*h->serverTerminal(session), *h->mirror);
        CHECK(vthost::testing::describeGaps(gaps).empty());
        h->client->detach();
    }(&h, session);

    h.loop.blockOn(drive(&h, std::move(scenario)));
}

TEST_CASE("redefinition after attach updates the client's glyph", "[vthost][softfont][daemon]")
{
    auto h = SoftFontHarness {};
    auto const session = newSession(h.host);
    h.serverTerminal(session)->writeToScreen(fontAndGlyph('@') + "!");
    h.connect();

    auto scenario = [](SoftFontHarness* h, vtworkspace::SessionId session) -> Task<void> {
        co_await requireUntil(&h->loop, [&] {
            return h->mirror->softFonts().resolve({ .font = 1, .position = 0x21 }) != nullptr;
        });
        auto const before = h->mirror->softFonts().resolve({ .font = 1, .position = 0x21 });
        REQUIRE(before != nullptr);

        // Redefined in place: the cell is not rewritten, only the glyph its reference names.
        h->serverWrites(session, "\033P1;0;0;10;0;0;20;0{ A~\033\\");
        co_await requireUntil(&h->loop, [&] {
            auto const now = h->mirror->softFonts().resolve({ .font = 1, .position = 0x21 });
            return now != nullptr && now->id != before->id;
        });
        auto const serverGlyph =
            h->serverTerminal(session)->softFonts().resolve({ .font = 1, .position = 0x21 });
        auto const mirrorGlyph = h->mirror->softFonts().resolve({ .font = 1, .position = 0x21 });
        REQUIRE(serverGlyph != nullptr);
        REQUIRE(mirrorGlyph != nullptr);
        CHECK(mirrorGlyph->id == serverGlyph->id);
        CHECK(mirrorGlyph->id != before->id);
        CHECK(softGlyphAt(*h->mirror, 0, 0).has_value());
        h->client->detach();
    }(&h, session);

    h.loop.blockOn(drive(&h, std::move(scenario)));
}

TEST_CASE("a client without the capability sees the underlying characters", "[vthost][softfont][daemon]")
{
    auto h = SoftFontHarness { ClientOffers::NoSoftFonts };
    auto const session = newSession(h.host);
    h.serverTerminal(session)->writeToScreen(fontAndGlyph() + "!a!");
    h.connect();

    auto scenario = [](SoftFontHarness* h, vtworkspace::SessionId session) -> Task<void> {
        co_await requireUntil(
            &h->loop, [&] { return h->mirror->primaryScreen().grid().renderMainPageText().contains("!a!"); });
        // The server's cells do carry the glyph; the client was told of neither it nor the font.
        REQUIRE(softGlyphAt(*h->serverTerminal(session), 0, 0).has_value());
        CHECK_FALSE(softGlyphAt(*h->mirror, 0, 0).has_value());
        CHECK_FALSE(softGlyphAt(*h->mirror, 0, 2).has_value());
        CHECK(h->mirror->softFonts().font(1) == nullptr);
        CHECK(h->mirror->primaryScreen().grid().renderMainPageText().contains("!a!"));
        h->client->detach();
    }(&h, session);

    h.loop.blockOn(drive(&h, std::move(scenario)));
}

TEST_CASE("RIS on the server makes the client fall back to text", "[vthost][softfont][daemon]")
{
    auto h = SoftFontHarness {};
    auto const session = newSession(h.host);
    h.serverTerminal(session)->writeToScreen(fontAndGlyph() + "!");
    h.connect();

    auto scenario = [](SoftFontHarness* h, vtworkspace::SessionId session) -> Task<void> {
        co_await requireUntil(&h->loop, [&] { return softGlyphAt(*h->mirror, 0, 0).has_value(); });
        REQUIRE(h->mirror->softFonts().font(1) != nullptr);

        // RIS drops the fonts and the designation; '!' is then plain text again.
        h->serverWrites(session, "\033c!");
        co_await requireUntil(&h->loop, [&] { return h->mirror->softFonts().font(1) == nullptr; });
        co_await requireUntil(&h->loop, [&] {
            return h->mirror->primaryScreen().grid().renderMainPageText().starts_with("!");
        });
        CHECK(h->mirror->softFonts().font(1) == nullptr);
        CHECK(h->serverTerminal(session)->softFonts().font(1) == nullptr);
        CHECK_FALSE(softGlyphAt(*h->mirror, 0, 0).has_value());
        CHECK(h->mirror->primaryScreen().grid().renderMainPageText().starts_with("!"));
        h->client->detach();
    }(&h, session);

    h.loop.blockOn(drive(&h, std::move(scenario)));
}

TEST_CASE("two fonts sharing a designator: the client selects the most recently loaded",
          "[vthost][softfont][daemon]")
{
    auto h = SoftFontHarness {};
    auto const session = newSession(h.host);
    // Font 2 is loaded first and font 1 later, both as " A": index order would answer 2, load order 1.
    h.serverTerminal(session)->writeToScreen("\033P2;0;0;10;0;0;20;0{ A@\033\\"
                                             "\033P1;0;0;10;0;0;20;0{ A~\033\\");
    h.connect();

    auto scenario = [](SoftFontHarness* h, vtworkspace::SessionId session) -> Task<void> {
        co_await requireUntil(&h->loop, [&] {
            return h->mirror->softFonts().font(1) != nullptr && h->mirror->softFonts().font(2) != nullptr;
        });
        REQUIRE(h->serverTerminal(session)->softFonts().fontForDesignator(" A") == 1);
        CHECK(h->mirror->softFonts().fontForDesignator(" A") == 1);
        CHECK(h->mirror->softFonts().loadRanks() == h->serverTerminal(session)->softFonts().loadRanks());
        h->client->detach();
    }(&h, session);

    h.loop.blockOn(drive(&h, std::move(scenario)));
}

TEST_CASE("soft-glyph cells in a chunked snapshot survive", "[vthost][softfont][daemon]")
{
    // A send-queue bound this small makes a snapshot piece 16 KiB, so a few hundred full rows span several.
    constexpr auto QueueBytes = std::size_t { 64 } * 1024;
    constexpr auto Rows = 400;
    auto h = SoftFontHarness { ClientOffers::SoftFonts, QueueBytes };
    auto const session = newSession(h.host);
    auto text = fontAndGlyph();
    for (auto const row: std::views::iota(0, Rows))
        text += std::format("{}{}\r\n", std::string(40, '!'), row % 2 == 0 ? "x" : "y");
    h.serverTerminal(session)->writeToScreen(text);

    // The scenario is only worth its name when the server really splits this grid.
    {
        auto const& grid = h.serverTerminal(session)->primaryScreen().grid();
        auto wire = std::vector<proto::WireLine> {};
        for (auto const offset:
             std::views::iota(-unbox<int>(grid.historyLineCount()), unbox<int>(grid.pageSize().lines)))
            wire.push_back(vthost::toWireLine(
                grid, vtbackend::LineOffset(offset), grid.lineAt(vtbackend::LineOffset(offset))));
        REQUIRE(proto::partitionSnapshotRows(wire, NativeSession::snapshotPieceBytes(QueueBytes)).size()
                >= 3);
    }
    h.connect();

    auto scenario = [](SoftFontHarness* h, vtworkspace::SessionId session) -> Task<void> {
        co_await requireUntil(&h->loop, [&] {
            auto const& grid = h->mirror->primaryScreen().grid();
            return h->mirror->softFonts().resolve({ .font = 1, .position = 0x21 }) != nullptr
                   && unbox<int>(grid.historyLineCount()) > 300 && softGlyphAt(*h->mirror, 0, 0).has_value();
        });
        // Every row of the mirror, those of the first and of the last piece alike, shows its glyphs.
        auto const& serverGrid = h->serverTerminal(session)->primaryScreen().grid();
        auto const& mirrorGrid = h->mirror->primaryScreen().grid();
        REQUIRE(mirrorGrid.historyLineCount() == serverGrid.historyLineCount());
        auto missing = 0;
        for (auto const offset: std::views::iota(-unbox<int>(serverGrid.historyLineCount()), 0))
        {
            auto const& row = mirrorGrid.lineAt(vtbackend::LineOffset(offset)).storage();
            if (!row.softGlyphs || !row.softGlyphs->contains(0) || !row.softGlyphs->contains(39))
                ++missing;
        }
        CHECK(missing == 0);
        auto const gaps = vthost::testing::compareGrids(*h->serverTerminal(session), *h->mirror);
        CHECK(vthost::testing::describeGaps(gaps).empty());
        h->client->detach();
    }(&h, session);

    h.loop.blockOn(drive(&h, std::move(scenario)));
}

TEST_CASE("an out-of-range soft-glyph entry is rejected", "[vthost][softfont][daemon]")
{
    auto const accepted = [](uint8_t font, uint8_t position) {
        auto delta = proto::Delta {};
        delta.session = 1;
        delta.softGlyphCells = { proto::SoftGlyphCellEntry {
            .stableId = 0, .font = font, .position = position } };
        auto stream = proto::Writer {};
        proto::encodePdu(stream, 0, proto::DecodedPdu { delta });
        return proto::decodePdu(stream.view()).has_value();
    };
    CHECK(accepted(2, 0x7E));
    CHECK_FALSE(accepted(3, 0x41));
    CHECK_FALSE(accepted(0, 0x20));
    CHECK_FALSE(accepted(0, 0x7F));

    // And a set the decoder lets through but DECDLD could not have produced leaves the mirror's fonts alone.
    auto h = SoftFontHarness {};
    h.mirror->writeToScreen(fontAndGlyph());
    auto const held = h.mirror->softFonts().font(1);
    REQUIRE(held != nullptr);

    auto bad = proto::WireSoftFont { .fontNumber = 3, .designator = " Z", .fullWidth = 1, .fullHeight = 1 };
    h.populator->applyEvent(proto::SoftFontState { .session = 1, .generation = 7, .fonts = { bad } });
    CHECK(h.mirror->softFonts().font(1) == held);
    bad.fontNumber = 0;
    bad.loadRank = 5;
    h.populator->applyEvent(proto::SoftFontState { .session = 1, .generation = 8, .fonts = { bad } });
    CHECK(h.mirror->softFonts().font(1) == held);
}

TEST_CASE("a delta touching one page row leaves the history's soft glyph rows alone",
          "[vthost][softfont][daemon]")
{
    auto h = SoftFontHarness {};
    auto const session = newSession(h.host);
    auto text = fontAndGlyph();
    for (auto const row: std::views::iota(0, 100))
        text += std::format("{}{}\r\n", std::string(10, '!'), row % 2 == 0 ? "x" : "y");
    h.serverTerminal(session)->writeToScreen(text);
    h.connect();

    auto scenario = [](SoftFontHarness* h, vtworkspace::SessionId session) -> Task<void> {
        co_await requireUntil(&h->loop, [&] {
            return unbox<int>(h->mirror->primaryScreen().grid().historyLineCount()) > 50
                   && softGlyphAt(*h->mirror, -1, 0).has_value();
        });
        auto& grid = h->mirror->primaryScreen().grid();
        REQUIRE(unbox<int>(grid.historyLineCount()) > 50);

        // A consumer of the mirror's grid, caught up with everything the attach wrote.
        auto cursor = vtbackend::GridDeltaCursor {};
        std::ignore = grid.forEachLineChangedSince(cursor, [](auto, auto const&) {});
        std::ignore = grid.forEachLineChangedSince(cursor, [](auto, auto const&) {});

        // One character on a page row; the cursor sits on an empty last row, so nothing scrolls.
        h->serverWrites(session, "z");
        co_await requireUntil(
            &h->loop, [&] { return h->mirror->primaryScreen().grid().renderMainPageText().contains("z"); });
        auto historyRowsChanged = 0;
        std::ignore = grid.forEachLineChangedSince(cursor, [&](vtbackend::LineOffset offset, auto const&) {
            historyRowsChanged += offset < vtbackend::LineOffset(0) ? 1 : 0;
        });
        CHECK(historyRowsChanged == 0);
        // And the history still shows its glyphs.
        CHECK(softGlyphAt(*h->mirror, -1, 0).has_value());
        h->client->detach();
    }(&h, session);

    h.loop.blockOn(drive(&h, std::move(scenario)));
}
