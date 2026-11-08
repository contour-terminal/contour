// SPDX-License-Identifier: Apache-2.0
#include <vtbackend/core/SoftGlyphRef.hpp>
#include <vtbackend/state/StateFields.hpp>
#include <vtbackend/state/TerminalState.hpp>
#include <vtbackend/state/TerminalStateCodec.hpp>
#include <vtbackend/testing/MockTerm.hpp>
#include <vtbackend/testing/StateHandover.hpp>

#include <catch2/catch_test_macros.hpp>

#include <expected>
#include <format>
#include <memory>
#include <optional>
#include <ranges>
#include <string>
#include <string_view>
#include <vector>

using namespace vtbackend;
using namespace std::string_view_literals;

namespace
{
/// Two small fonts: 1 draws a lit top row at 'A' and 'B'; 2 a full cell at 'A'.
constexpr auto FontOne = "\033P1;0;1;10;0;0;20;0{ A~~~~~/~~~~~;@@@@@/@@@@@\033\\"sv;
constexpr auto FontTwo = "\033P2;0;1;10;0;0;20;0{ B~~~~~/~~~~~\033\\"sv;

/// Hands @p source over through the byte codec and requires it to succeed.
std::unique_ptr<MockTerm<>> handOver(MockTerm<> const& source, test::HandoverSetup const& setup)
{
    auto target = test::handedOver(source, setup);
    REQUIRE(target.has_value());
    return std::move(*target);
}

/// @return The state of a terminal that loaded the fonts, decoded back from its bytes.
TerminalState captured(MockTerm<> const& mock)
{
    auto decoded = decodeTerminalState(encodeTerminalState(mock.terminal.captureState()));
    REQUIRE(decoded.has_value());
    return std::move(*decoded);
}
} // namespace

TEST_CASE("soft fonts and per-cell references round-trip", "[StateRoundTrip][SoftFont]")
{
    auto const setup = test::HandoverSetup { .pageSize = PageSize { LineCount(3), ColumnCount(12) },
                                             .history = LineCount(10) };
    auto source = setup.makeTerminal();
    source->writeToScreen(FontOne);
    source->writeToScreen(FontTwo);
    // Soft cells in the page, and enough lines to push some into the scrollback.
    for (auto const line: std::views::iota(0, 5))
    {
        source->writeToScreen(std::format("\033[{};1H", 1 + (line % 3)));
        source->writeToScreen("plain\r\n");
    }
    auto& screen = source->terminal.primaryScreen();
    screen.at(LineOffset(0), ColumnOffset(2)).setSoftGlyph({ .font = 1, .position = 0x41 });
    screen.at(LineOffset(1), ColumnOffset(4)).setSoftGlyph({ .font = 2, .position = 0x42 });
    screen.at(LineOffset(-1), ColumnOffset(0)).setSoftGlyph({ .font = 0, .position = 0x7E });
    REQUIRE(unbox<int>(screen.grid().historyLineCount()) > 0);

    auto const target = handOver(*source, setup);
    auto const& restored = target->terminal.primaryScreen();

    auto const& want = source->terminal.softFonts().fonts();
    auto const& got = target->terminal.softFonts().fonts();
    for (auto const number: std::views::iota(size_t { 0 }, want.size()))
    {
        REQUIRE(static_cast<bool>(want[number]) == static_cast<bool>(got[number]));
        if (!want[number])
            continue;
        CHECK(got[number]->designator == want[number]->designator);
        CHECK(got[number]->declaration == want[number]->declaration);
        CHECK(got[number]->geometry.fullWidth == want[number]->geometry.fullWidth);
        CHECK(got[number]->geometry.fullHeight == want[number]->geometry.fullHeight);
        CHECK(got[number]->geometry.textWidth == want[number]->geometry.textWidth);
        for (auto const position: std::views::iota(size_t { 0 }, size_t { 96 }))
        {
            auto const& a = want[number]->glyphs[position];
            auto const& b = got[number]->glyphs[position];
            REQUIRE(static_cast<bool>(a) == static_cast<bool>(b));
            if (a)
            {
                CHECK(a->id == b->id);
                CHECK(a->coverage == b->coverage);
            }
        }
        CHECK(got[number]->errorGlyph->coverage == want[number]->errorGlyph->coverage);
    }

    for (auto const offset: std::views::iota(-unbox<int>(restored.grid().historyLineCount()), 3))
        for (auto const column: std::views::iota(0, 12))
            CHECK(
                restored.at(LineOffset(offset), ColumnOffset(column)).softGlyph()
                == source->terminal.primaryScreen().at(LineOffset(offset), ColumnOffset(column)).softGlyph());
    CHECK(restored.at(LineOffset(0), ColumnOffset(2)).softGlyph()
          == std::optional { SoftGlyphRef { .font = 1, .position = 0x41 } });
    CHECK(restored.at(LineOffset(-1), ColumnOffset(0)).softGlyph()
          == std::optional { SoftGlyphRef { .font = 0, .position = 0x7E } });
}

TEST_CASE("the most recent designator wins after a round trip", "[StateRoundTrip][SoftFont]")
{
    // Fonts 2 and 0 share the designator " A"; font 0 was loaded last, so it must still win.
    auto const setup = test::HandoverSetup {};
    auto source = setup.makeTerminal();
    source->writeToScreen("\033P2;0;1;10;0;0;20;0{ A~~~~~/~~~~~\033\\"sv);
    source->writeToScreen("\033P0;0;1;10;0;0;20;0{ A@@@@@/@@@@@\033\\"sv);
    REQUIRE(source->terminal.softFonts().fontForDesignator(" A") == std::optional<uint8_t> { 0 });

    auto const target = handOver(*source, setup);
    CHECK(target->terminal.softFonts().fontForDesignator(" A") == std::optional<uint8_t> { 0 });
}

namespace
{
/// @return A terminal that loaded font 1 and drew soft cells with it, as a state.
TerminalState stateWithFont(test::HandoverSetup const& setup)
{
    auto source = setup.makeTerminal();
    source->writeToScreen(FontOne);
    source->writeToScreen("\033) A\x0E!\"\x0F text"sv);
    source->terminal.primaryScreen()
        .at(LineOffset(0), ColumnOffset(1))
        .setSoftGlyph({ .font = 1, .position = 0x41 });
    auto state = captured(*source);
    REQUIRE(state.keyStrings.softFonts.size() == 1);
    return state;
}

/// @return Whether @p state restores into a terminal of @p setup.
std::expected<void, StateDecodeError> restoreOf(TerminalState const& state, test::HandoverSetup const& setup)
{
    auto const settings = settingsFor(state, setup.settings());
    REQUIRE(settings.has_value());
    auto target = setup.makeTerminal(*settings);
    return target->terminal.restoreState(state);
}
} // namespace

TEST_CASE("a persisted soft font restores", "[StateRoundTrip][SoftFont]")
{
    auto const setup = test::HandoverSetup {};
    CHECK(restoreOf(stateWithFont(setup), setup).has_value());
}

TEST_CASE("a soft font whose glyph bits do not match width times height is rejected",
          "[StateRoundTrip][SoftFont]")
{
    auto const setup = test::HandoverSetup {};
    auto state = stateWithFont(setup);
    auto& glyph = state.keyStrings.softFonts.front().glyphs.front();
    SECTION("one byte too many")
    {
        glyph.packedCoverage.push_back(0);
    }
    SECTION("one byte too few")
    {
        glyph.packedCoverage.pop_back();
    }
    SECTION("a width other than the font's")
    {
        glyph.width = static_cast<uint8_t>(glyph.width + 1);
    }
    auto const restored = restoreOf(state, setup);
    REQUIRE_FALSE(restored.has_value());
    CHECK(restored.error() == StateDecodeError::InvalidValue);
}

TEST_CASE("a soft font with an impossible number, position or load rank is rejected",
          "[StateRoundTrip][SoftFont]")
{
    auto const setup = test::HandoverSetup {};
    auto state = stateWithFont(setup);
    auto& font = state.keyStrings.softFonts.front();
    SECTION("font number 3")
    {
        font.number = 3;
    }
    SECTION("glyph position 0x1F")
    {
        font.glyphs.front().position = 0x1F;
    }
    SECTION("glyph position 0x80")
    {
        font.glyphs.front().position = 0x80;
    }
    SECTION("load rank 3")
    {
        font.loadRank = 3;
    }
    SECTION("the same font twice")
    {
        state.keyStrings.softFonts.push_back(font);
    }
    CHECK_FALSE(restoreOf(state, setup).has_value());
}

TEST_CASE("an out-of-range soft-glyph triple is rejected", "[StateRoundTrip][SoftFont]")
{
    auto const setup = test::HandoverSetup {};
    auto source = setup.makeTerminal();
    source->writeToScreen(FontOne);
    auto& screen = source->terminal.primaryScreen();

    auto const rejected = [&](SoftGlyphRef ref) {
        screen.at(LineOffset(0), ColumnOffset(3)).setSoftGlyph(ref);
        auto const decoded = decodeTerminalState(encodeTerminalState(source->terminal.captureState()));
        auto const restored =
            decoded ? restoreOf(*decoded, setup)
                    : std::expected<void, StateDecodeError> { std::unexpected(decoded.error()) };
        return !restored.has_value();
    };
    // A cell shows 0x21..0x7E: SP and DEL are plain, as the Delta decoder also judges them.
    CHECK(rejected({ .font = 2, .position = 0x21 }) == false);
    CHECK(rejected({ .font = 0, .position = 0x7E }) == false);
    CHECK(rejected({ .font = 2, .position = 0x20 }));
    CHECK(rejected({ .font = 0, .position = 0x7F }));
    CHECK(rejected({ .font = 3, .position = 0x41 }));
    CHECK(rejected({ .font = 1, .position = 0x1F }));
    CHECK(rejected({ .font = 1, .position = 0x80 }));
    CHECK(rejected({ .font = 1, .position = 0 }));
}

TEST_CASE("a screen shrunk below its soft glyphs still hands over", "[StateRoundTrip][SoftFont]")
{
    auto const setup = test::HandoverSetup { .pageSize = PageSize { LineCount(3), ColumnCount(20) },
                                             .history = LineCount(10) };
    auto source = setup.makeTerminal();
    source->writeToScreen(FontOne);
    source->writeToScreen("\033[1;16H\033) A\x0E!\"\x0F");
    source->terminal.flushInput();
    source->terminal.resizeScreen(PageSize { LineCount(3), ColumnCount(10) });

    auto const target = test::handedOver(*source, setup);
    REQUIRE(target.has_value());

    source->terminal.resizeScreen(PageSize { LineCount(3), ColumnCount(20) });
    auto& screen = source->terminal.primaryScreen();
    for (auto const col: std::views::iota(0, 20))
        CHECK(!screen.at(LineOffset(0), ColumnOffset(col)).softGlyph());
}

namespace
{
/// Gives @p font a cell of @p width by @p height, its glyphs resized to match.
void resizeCell(SoftFontState& font, uint8_t width, uint8_t height)
{
    font.fullWidth = width;
    font.fullHeight = height;
    for (auto& glyph: font.glyphs)
    {
        glyph.width = width;
        glyph.height = height;
        glyph.packedCoverage.assign(((size_t { width } * height) + 7) / 8, uint8_t { 0 });
    }
}
} // namespace

TEST_CASE("a soft font whose cell or designator is out of bounds is rejected", "[StateRoundTrip][SoftFont]")
{
    auto const setup = test::HandoverSetup {};
    auto state = stateWithFont(setup);
    auto& font = state.keyStrings.softFonts.front();
    REQUIRE(restoreOf(state, setup).has_value());
    SECTION("a cell wider than 16")
    {
        resizeCell(font, 17, 10);
    }
    SECTION("a cell taller than 32")
    {
        resizeCell(font, 10, 33);
    }
    SECTION("an empty cell")
    {
        resizeCell(font, 0, 0);
    }
    SECTION("text wider than its cell")
    {
        font.textWidth = static_cast<uint8_t>(font.fullWidth + 1);
    }
    SECTION("a designator of four bytes")
    {
        font.designator = " @AB";
    }
    CHECK_FALSE(restoreOf(state, setup).has_value());
}

TEST_CASE("a restored font merges a Pe=1 reload without throwing", "[StateRoundTrip][SoftFont]")
{
    // The largest cell a font may have (16x32), restored, then extended: the merge indexes the scratch
    // glyph with the restored geometry, which is why the codec has to bound it.
    auto const setup = test::HandoverSetup {};
    auto state = stateWithFont(setup);
    resizeCell(state.keyStrings.softFonts.front(), 16, 32);
    auto const settings = settingsFor(state, setup.settings());
    REQUIRE(settings.has_value());
    auto target = setup.makeTerminal(*settings);
    REQUIRE(target->terminal.restoreState(state).has_value());
    CHECK_NOTHROW(target->writeToScreen("\033P1;1;1;10;0;0;20;0{ A~~~~~/~~~~~\033\\"sv));
}

TEST_CASE("a restored line holding soft glyphs is Spanning whatever shape the state claimed",
          "[StateRoundTrip][SoftFont]")
{
    auto const setup = test::HandoverSetup {};
    auto source = setup.makeTerminal();
    source->writeToScreen(FontOne);
    source->writeToScreen("\033) A\x0E!\x0F");
    auto& line = source->terminal.primaryScreen().grid().lineAt(LineOffset(0));
    REQUIRE(line.storage().softGlyphs);
    // A state from a writer that did not degrade the line: the table must still be cleared by later writes.
    source->terminal.primaryScreen().grid().lineAt(LineOffset(0)).storage().shape = LineShape::Uniform;

    auto const target = handOver(*source, setup);
    auto const& restored = target->terminal.primaryScreen().grid().lineAt(LineOffset(0)).storage();
    REQUIRE(restored.softGlyphs);
    CHECK(restored.shape == LineShape::Spanning);

    target->writeToScreen("\033[1;1Hx");
    CHECK(!target->terminal.primaryScreen().at(LineOffset(0), ColumnOffset(0)).softGlyph());
}

TEST_CASE("a text font declared wider than its 36-line cell hands over", "[StateRoundTrip][SoftFont]")
{
    // Pss 11 is 80x36: a 10x10 cell, so a declared width of 12 is clipped to 10 when the font is built.
    auto const setup = test::HandoverSetup {};
    auto source = setup.makeTerminal();
    source->writeToScreen("\033P1;0;1;12;11;0;10;0{ A~~~~~/~~~~~\033\\"sv);
    auto const font = source->terminal.softFonts().font(1);
    REQUIRE(font);
    CHECK(int { font->geometry.textWidth } <= int { font->geometry.fullWidth });

    auto const target = test::handedOver(*source, setup);
    REQUIRE(target.has_value());
    CHECK(static_cast<bool>((*target)->terminal.softFonts().font(1)));
}
