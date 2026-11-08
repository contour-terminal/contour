// SPDX-License-Identifier: Apache-2.0
#include <vtbackend/core/Image.hpp>
#include <vtbackend/core/Primitives.hpp>
#include <vtbackend/core/SoftGlyphRef.hpp>
#include <vtbackend/render/RenderBuffer.hpp>
#include <vtbackend/screen/Screen.hpp>
#include <vtbackend/testing/MockTerm.hpp>
#include <vtbackend/testing/TestHelpers.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <atomic>
#include <format>
#include <memory>
#include <optional>
#include <ranges>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

using namespace vtbackend;
using namespace vtbackend::test;

namespace
{

constexpr auto TestRef = SoftGlyphRef { .font = 1, .position = 0x41 };

/// Lists the columns of line 0 that carry a soft glyph reference.
std::vector<int> softGlyphColumns(Screen& screen, int columns)
{
    auto result = std::vector<int> {};
    for (auto const column: std::views::iota(0, columns))
        if (screen.at(LineOffset(0), ColumnOffset(column)).softGlyph())
            result.push_back(column);
    return result;
}

constexpr auto TestPage = PageSize { LineCount(2), ColumnCount(10) };

} // namespace

TEST_CASE("SoftGlyph.ICH and DCH shift soft glyphs with their cells", "[screen][SoftGlyph]")
{
    auto mock = MockTerm { TestPage };
    auto& screen = mock.terminal.primaryScreen();
    mock.writeToScreen("ABCDEFGH");
    screen.at(LineOffset(0), ColumnOffset(3)).setSoftGlyph(TestRef);

    mock.writeToScreen("\033[1;1H\033[2@"); // ICH 2 at column 0
    CHECK(softGlyphColumns(screen, 10) == std::vector<int> { 5 });
    CHECK(screen.at(LineOffset(0), ColumnOffset(5)).softGlyph() == TestRef);

    mock.writeToScreen("\033[1;1H\033[2P"); // DCH 2
    CHECK(softGlyphColumns(screen, 10) == std::vector<int> { 3 });
    CHECK(screen.at(LineOffset(0), ColumnOffset(3)).softGlyph() == TestRef);
}

TEST_CASE("SoftGlyph.DCH drops the glyph of a deleted cell", "[screen][SoftGlyph]")
{
    auto mock = MockTerm { TestPage };
    auto& screen = mock.terminal.primaryScreen();
    mock.writeToScreen("ABCDEFGH");
    screen.at(LineOffset(0), ColumnOffset(1)).setSoftGlyph(TestRef);
    screen.at(LineOffset(0), ColumnOffset(7)).setSoftGlyph(TestRef);

    mock.writeToScreen("\033[1;1H\033[2P"); // deletes columns 0 and 1
    CHECK(softGlyphColumns(screen, 10) == std::vector<int> { 5 });
}

TEST_CASE("SoftGlyph.DECIC and DECDC shift soft glyphs", "[screen][SoftGlyph]")
{
    auto mock = MockTerm { TestPage };
    auto& screen = mock.terminal.primaryScreen();
    mock.writeToScreen("ABCDEFGH\r\nabcdefgh");
    screen.at(LineOffset(0), ColumnOffset(3)).setSoftGlyph(TestRef);

    mock.writeToScreen("\033[1;1H\033[2'}"); // DECIC 2
    CHECK(softGlyphColumns(screen, 10) == std::vector<int> { 5 });

    mock.writeToScreen("\033[1;1H\033[2'~"); // DECDC 2
    CHECK(softGlyphColumns(screen, 10) == std::vector<int> { 3 });
}

TEST_CASE("SoftGlyph.ICH shifts image fragments too", "[screen][SoftGlyph]")
{
    auto mock = MockTerm { TestPage };
    auto& screen = mock.terminal.primaryScreen();
    mock.writeToScreen("ABCDEFGH");

    auto const image = std::make_shared<Image>(
        ImageId(1), ImageFormat::RGBA, Image::Data(4, 0xFF), ImageSize { Width(1), Height(1) }, [](auto) {});
    auto const rasterized = std::make_shared<RasterizedImage>(image,
                                                              ImageAlignment::TopStart,
                                                              ImageResize::NoResize,
                                                              RGBAColor { 0, 0, 0, 0xFF },
                                                              GridSize { LineCount(1), ColumnCount(1) },
                                                              ImageSize { Width(1), Height(1) });
    screen.at(LineOffset(0), ColumnOffset(3)).setImageFragment(rasterized, CellLocation {});

    mock.writeToScreen("\033[1;1H\033[2@");
    CHECK(!screen.at(LineOffset(0), ColumnOffset(3)).imageFragment());
    CHECK(screen.at(LineOffset(0), ColumnOffset(5)).imageFragment());

    mock.writeToScreen("\033[1;1H\033[2P");
    CHECK(!screen.at(LineOffset(0), ColumnOffset(5)).imageFragment());
    CHECK(screen.at(LineOffset(0), ColumnOffset(3)).imageFragment());
}

namespace
{

/// One 15-column, two-sixel-row glyph, as in the vt100.net VT320 .fnt files.
constexpr auto Glyph = std::string_view { "~~~~~~~~~~~~~~~/~~~~~~~~~~~~~~~" };

/// A VT320-shaped DECDLD (Pfn 0, Pcn 1, Pe 1, full cell) for designator B with two glyphs, framed by
/// @p introducer and @p terminator.
[[nodiscard]] std::string vt320Font(std::string_view introducer,
                                    std::string_view terminator,
                                    std::string_view separator = ";")
{
    return std::format("{}0;1;1;0;0;2;0;0{{B{}{}{}{}", introducer, Glyph, separator, Glyph, terminator);
}

[[nodiscard]] std::string sevenBitFont()
{
    return vt320Font("\033P", "\033\\");
}

constexpr auto OneLine = PageSize { LineCount(3), ColumnCount(20) };

} // namespace

TEST_CASE("a VT320 font loaded at Pcn=1 renders at '!'", "[screen][softfont]")
{
    auto mock = MockTerm { OneLine };
    mock.writeToScreen(sevenBitFont());
    mock.writeToScreen("\033(B!");
    auto& screen = mock.terminal.primaryScreen();
    auto const cell = screen.at(LineOffset(0), ColumnOffset(0));
    CHECK(cell.codepoint(0) == U'!');
    CHECK(cell.softGlyph() == SoftGlyphRef { .font = 0, .position = 0x21 });
}

TEST_CASE("a raw 8-bit DECDLD (0x90 ... 0x9C) loads like the 7-bit form", "[screen][softfont]")
{
    auto sevenBit = MockTerm { OneLine };
    sevenBit.writeToScreen(sevenBitFont());

    auto eightBit = MockTerm { OneLine };
    eightBit.writeToScreen(vt320Font("\x90", "\x9c", ";\r\n"));

    auto const a = sevenBit.terminal.softFonts().font(0);
    auto const b = eightBit.terminal.softFonts().font(0);
    REQUIRE(a != nullptr);
    REQUIRE(b != nullptr);
    CHECK(a->designator == b->designator);
    CHECK(a->geometry.fullWidth == b->geometry.fullWidth);
    CHECK(a->geometry.fullHeight == b->geometry.fullHeight);
    for (auto const position: { 0x21, 0x22 })
    {
        auto const ra =
            sevenBit.terminal.softFonts().resolve({ .font = 0, .position = static_cast<uint8_t>(position) });
        auto const rb =
            eightBit.terminal.softFonts().resolve({ .font = 0, .position = static_cast<uint8_t>(position) });
        REQUIRE(ra != nullptr);
        REQUIRE(rb != nullptr);
        CHECK(ra->id == rb->id);
    }
}

TEST_CASE("a loaded font takes over a standard designator", "[screen][softfont]")
{
    auto mock = MockTerm { OneLine };
    mock.writeToScreen(sevenBitFont());
    mock.writeToScreen("\033(B!");
    CHECK(mock.terminal.primaryScreen().at(LineOffset(0), ColumnOffset(0)).softGlyph().has_value());

    mock.writeToScreen("\033c\033(B!");
    CHECK(mock.terminal.softFonts().font(0) == nullptr);
    CHECK_FALSE(mock.terminal.primaryScreen().at(LineOffset(0), ColumnOffset(0)).softGlyph().has_value());
}

TEST_CASE("copy yields the underlying characters", "[screen][softfont]")
{
    auto mock = MockTerm { OneLine };
    mock.writeToScreen(sevenBitFont());
    mock.writeToScreen("A\033)B\x0E!\x0F"
                       "B"); // plain, soft (G1 shifted out), plain
    mock.terminal.setSelector(
        std::make_unique<LinearSelection>(mock.terminal.selectionHelper(),
                                          CellLocation { .line = LineOffset(0), .column = ColumnOffset(0) },
                                          [] {}));
    (void) mock.terminal.selector()->extend(
        CellLocation { .line = LineOffset(0), .column = ColumnOffset(2) });
    CHECK(mock.terminal.extractSelectionText() == "A!B");
    // The middle cell is a soft one: the text above came from its underlying character.
    CHECK(mock.terminal.primaryScreen().at(LineOffset(0), ColumnOffset(1)).softGlyph().has_value());
    CHECK(!mock.terminal.primaryScreen().at(LineOffset(0), ColumnOffset(0)).softGlyph().has_value());
    CHECK(!mock.terminal.primaryScreen().at(LineOffset(0), ColumnOffset(2)).softGlyph().has_value());
}

TEST_CASE("space, DEL and non-ASCII stay plain while a soft set is active", "[screen][softfont]")
{
    auto mock = MockTerm { OneLine };
    mock.writeToScreen(sevenBitFont());
    mock.writeToScreen("\033(B!"
                       " "
                       "\x7f"
                       "\"" // 0x22
                       "世");
    auto& screen = mock.terminal.primaryScreen();
    CHECK(screen.at(LineOffset(0), ColumnOffset(0)).softGlyph().has_value());
    CHECK_FALSE(screen.at(LineOffset(0), ColumnOffset(1)).softGlyph().has_value());
    CHECK(screen.at(LineOffset(0), ColumnOffset(1)).codepoint(0) == U' ');
    CHECK_FALSE(screen.at(LineOffset(0), ColumnOffset(2)).softGlyph().has_value()); // DEL
    CHECK(screen.at(LineOffset(0), ColumnOffset(3)).softGlyph()
          == SoftGlyphRef { .font = 0, .position = 0x22 });
    CHECK_FALSE(screen.at(LineOffset(0), ColumnOffset(4)).softGlyph().has_value());
    CHECK(screen.at(LineOffset(0), ColumnOffset(4)).codepoint(0) == U'世');
}

TEST_CASE("overwriting a soft cell with plain text drops the reference", "[screen][softfont]")
{
    auto mock = MockTerm { OneLine };
    mock.writeToScreen(sevenBitFont());
    mock.writeToScreen("\033(B!");
    REQUIRE(mock.terminal.primaryScreen().at(LineOffset(0), ColumnOffset(0)).softGlyph().has_value());

    mock.writeToScreen("\033P1;1;2;0;0;2;0;0{C\033\\"); // Pe=2 loading font 1 erases font 0
    mock.writeToScreen("\033(B\033[1;1Hxyz");
    auto& screen = mock.terminal.primaryScreen();
    CHECK_FALSE(screen.at(LineOffset(0), ColumnOffset(0)).softGlyph().has_value());
    CHECK(screen.at(LineOffset(0), ColumnOffset(0)).codepoint(0) == U'x');
}

TEST_CASE("erase drops the reference", "[screen][softfont]")
{
    auto mock = MockTerm { OneLine };
    mock.writeToScreen(sevenBitFont());
    mock.writeToScreen("\033(B!\033[1;1H\033[1X");
    CHECK_FALSE(mock.terminal.primaryScreen().at(LineOffset(0), ColumnOffset(0)).softGlyph().has_value());
}

TEST_CASE("an undefined position in a loaded font still writes a reference", "[screen][softfont]")
{
    auto mock = MockTerm { OneLine };
    mock.writeToScreen(sevenBitFont());
    mock.writeToScreen("\033(B\"");
    auto const cell = mock.terminal.primaryScreen().at(LineOffset(0), ColumnOffset(0));
    CHECK(cell.codepoint(0) == U'"');
    CHECK(cell.softGlyph() == SoftGlyphRef { .font = 0, .position = 0x22 });
}

TEST_CASE("a designated but erased font falls back to the plain character", "[screen][softfont]")
{
    auto mock = MockTerm { OneLine };
    mock.writeToScreen(sevenBitFont());
    mock.writeToScreen("\033(B\033P1;1;2;0;0;2;0;0{C\033\\!");
    auto const cell = mock.terminal.primaryScreen().at(LineOffset(0), ColumnOffset(0));
    CHECK(cell.codepoint(0) == U'!');
    CHECK_FALSE(cell.softGlyph().has_value());
}

namespace
{

/// Refreshes the render buffer and returns the soft glyph the cell at column 0 of line 0 carries, plus
/// whether that cell was found at all.
struct RenderedCell
{
    bool found = false;
    std::optional<crispy::StrongHash> glyph; ///< the id of the glyph the cell carries, if any
    std::u32string codepoints;
};

[[nodiscard]] RenderedCell renderedCellAt(MockTerm<>& mock, int column)
{
    mock.terminal.markScreenDirty();
    (void) mock.terminal.refreshRenderBuffer();
    auto const buffer = mock.terminal.renderBuffer();
    for (auto const& cell: buffer.get().cells)
        if (cell.position.line == LineOffset(0) && cell.position.column == ColumnOffset(column))
        {
            auto const glyphs = std::span { buffer.get().softGlyphs };
            auto const it = std::ranges::find(glyphs, cell.position, &RenderSoftGlyph::position);
            return { .found = true,
                     .glyph = it != glyphs.end() ? std::optional { it->glyph->id } : std::nullopt,
                     .codepoints = cell.codepoints };
        }
    return {};
}

} // namespace

TEST_CASE("render cell carries the defined glyph", "[screen][softfont]")
{
    auto mock = MockTerm { OneLine };
    mock.writeToScreen(sevenBitFont());
    mock.writeToScreen("\033(B!");
    auto const rendered = renderedCellAt(mock, 0);
    REQUIRE(rendered.found);
    REQUIRE(rendered.glyph.has_value());
    auto const expected = mock.terminal.softFonts().resolve({ .font = 0, .position = 0x21 });
    REQUIRE(expected != nullptr);
    CHECK(rendered.glyph == expected->id);
}

TEST_CASE("render cell carries the error glyph for an undefined position", "[screen][softfont]")
{
    auto mock = MockTerm { OneLine };
    mock.writeToScreen(sevenBitFont());
    mock.writeToScreen("\033(B0"); // 0x30: the font defines only 0x21 and 0x22
    auto const undefined = mock.terminal.softFonts().resolve({ .font = 0, .position = 0x30 });
    REQUIRE(undefined != nullptr);
    CHECK(undefined == mock.terminal.softFonts().font(0)->errorGlyph);
    auto const rendered = renderedCellAt(mock, 0);
    REQUIRE(rendered.glyph.has_value());
    CHECK(rendered.glyph == undefined->id);
}

TEST_CASE("render cell has no soft glyph after RIS, and shows its codepoint", "[screen][softfont]")
{
    auto mock = MockTerm { OneLine };
    mock.writeToScreen(sevenBitFont());
    mock.writeToScreen("\033(B!");
    REQUIRE(renderedCellAt(mock, 0).glyph.has_value());

    // The cell outlives its font only when the font goes while the reference stays: erase the font alone.
    mock.writeToScreen("\033P1;1;2;0;0;2;0;0{C\033\\");
    auto const rendered = renderedCellAt(mock, 0);
    REQUIRE(rendered.found);
    CHECK_FALSE(rendered.glyph.has_value());
    CHECK(rendered.codepoints == U"!");
}

TEST_CASE("redefinition shows on the next render buffer", "[screen][softfont]")
{
    auto mock = MockTerm { OneLine };
    mock.writeToScreen(sevenBitFont());
    mock.writeToScreen("\033(B!");
    auto const before = renderedCellAt(mock, 0);
    REQUIRE(before.glyph.has_value());

    // Same position, different bitmap.
    mock.writeToScreen("\033P0;1;0;0;0;2;0;0{B?~~~~~~~~~~~~~~/~~~~~~~~~~~~~~~\033\\");
    auto const after = renderedCellAt(mock, 0);
    REQUIRE(after.glyph.has_value());
    CHECK(after.glyph != before.glyph);
}

TEST_CASE("soft font redefinition races rendering", "[softfont][threading]")
{
    auto mock = MockTerm { OneLine };
    mock.writeToScreen(sevenBitFont());
    mock.writeToScreen("\033(B!\"");
    constexpr auto Iterations = 1000;
    auto checked = std::atomic<size_t> { 0 };

    auto writer = std::thread { [&] {
        for (auto const index: std::views::iota(0, Iterations))
        {
            // Alternate two bitmaps for position 0x21 (Pcn=1); the sixel data differs per parity.
            auto const bitmap =
                (index % 2) == 0 ? "~~~~~~~~~~~~~~~/~~~~~~~~~~~~~~~" : "N~~~~~~~~~~~~~~/~~~~~~~~~~~~~~~";
            mock.writeToScreen(std::format("\033P0;1;0;0;0;2;0;0{{B{}\033\\", bitmap));
        }
    } };

    for (auto const index: std::views::iota(0, Iterations))
    {
        (void) index;
        (void) mock.terminal.refreshRenderBuffer();
        auto const buffer = mock.terminal.renderBuffer();
        for (auto const& entry: buffer.get().softGlyphs)
        {
            // Every byte must be readable: the glyph is owned by the buffer, not the live font.
            auto sum = size_t { 0 };
            for (auto const value: entry.glyph->coverage)
                sum += value;
            checked.fetch_add(1 + (sum & 0), std::memory_order_relaxed);
        }
    }
    writer.join();
    CHECK(checked.load() > 0);
}

TEST_CASE("soft glyphs are listed by position, sorted by line and then column", "[screen][softfont]")
{
    auto mock = MockTerm { OneLine };
    mock.writeToScreen(sevenBitFont());
    mock.writeToScreen("\033(B");
    // Written out of order, and away from line 0 and column 0, so an ordering by insertion or by hash
    // bucket cannot pass by accident.
    mock.writeToScreen("\033[3;2H!");
    mock.writeToScreen("\033[2;7H\"");
    mock.writeToScreen("\033[2;3H!");
    mock.writeToScreen("\033[2;12H\"");

    mock.terminal.markScreenDirty();
    (void) mock.terminal.refreshRenderBuffer();
    auto const buffer = mock.terminal.renderBuffer();

    auto const positions = std::ranges::to<std::vector>(buffer.get().softGlyphs
                                                        | std::views::transform(&RenderSoftGlyph::position));
    auto const expected = std::vector<CellLocation> {
        { .line = LineOffset(1), .column = ColumnOffset(2) },
        { .line = LineOffset(1), .column = ColumnOffset(6) },
        { .line = LineOffset(1), .column = ColumnOffset(11) },
        { .line = LineOffset(2), .column = ColumnOffset(1) },
    };
    CHECK(positions == expected);
    CHECK(std::ranges::is_sorted(positions));
    CHECK(std::ranges::all_of(buffer.get().softGlyphs,
                              [](auto const& entry) { return entry.glyph != nullptr; }));
}

TEST_CASE("a DECDLD with a trailing empty 9th parameter loads (vt100.net .fnt header)", "[screen][softfont]")
{
    // The files on vt100.net/dec/vt320 are framed exactly so: 8-bit DCS, a `;` after Pcss, the final, Dscs B,
    // glyphs separated by CR LF, 8-bit ST.
    auto mock = MockTerm { OneLine };
    auto const font = std::format("\x90"
                                  "0;1;1;0;0;2;0;0;{{B{}\r\n;{}\x9c",
                                  Glyph,
                                  Glyph);
    mock.writeToScreen(font);
    REQUIRE(mock.terminal.softFonts().font(0) != nullptr);
    mock.writeToScreen("\033(B!");
    auto const cell = mock.terminal.primaryScreen().at(LineOffset(0), ColumnOffset(0));
    CHECK(cell.softGlyph() == SoftGlyphRef { .font = 0, .position = 0x21 });
}

TEST_CASE("a DECDLD with ten or more parameters still loads", "[screen][softfont]")
{
    auto mock = MockTerm { OneLine };
    mock.writeToScreen(std::format("\033P0;1;1;0;0;2;0;0;;;;;{{B{};{}\033\\", Glyph, Glyph));
    REQUIRE(mock.terminal.softFonts().font(0) != nullptr);
    mock.writeToScreen("\033(B!");
    CHECK(mock.terminal.primaryScreen().at(LineOffset(0), ColumnOffset(0)).softGlyph()
          == SoftGlyphRef { .font = 0, .position = 0x21 });
}

TEST_CASE("an input method preedit over soft cells carries none of their glyphs", "[screen][softfont]")
{
    auto mock = MockTerm { OneLine };
    mock.writeToScreen(sevenBitFont());
    // Soft glyphs at columns 0..2, the cursor back on column 0, and "xy" composed there: the preedit
    // takes over columns 0 and 1, and column 2 stays the grid's.
    mock.writeToScreen("\033(B!\"!\033[1;1H");
    mock.terminal.updateInputMethodPreeditString("xy");
    mock.terminal.markScreenDirty();
    (void) mock.terminal.refreshRenderBuffer();
    auto const buffer = mock.terminal.renderBuffer();

    auto const columns =
        std::ranges::to<std::vector>(buffer.get().softGlyphs | std::views::transform([](auto const& entry) {
                                         return entry.position.column;
                                     }));
    CHECK(columns == std::vector { ColumnOffset(2) });
}

TEST_CASE("DECCRA copies soft glyphs with their cells", "[screen][softfont]")
{
    auto mock = MockTerm { TestPage };
    mock.writeToScreen(sevenBitFont());
    mock.writeToScreen("\033(B!\"");
    // Copy the two cells of line 1 to line 2, one column to the right.
    mock.writeToScreen("\033[1;1;1;2;1;2;2;1$v");
    auto& screen = mock.terminal.primaryScreen();
    CHECK(!screen.at(LineOffset(1), ColumnOffset(0)).softGlyph());
    CHECK(screen.at(LineOffset(1), ColumnOffset(1)).softGlyph()
          == SoftGlyphRef { .font = 0, .position = 0x21 });
    CHECK(screen.at(LineOffset(1), ColumnOffset(2)).softGlyph()
          == SoftGlyphRef { .font = 0, .position = 0x22 });
    CHECK(screen.at(LineOffset(1), ColumnOffset(1)).codepoint(0) == U'!');
}

TEST_CASE("DECSERA erases the soft glyph of a cell", "[screen][softfont]")
{
    auto mock = MockTerm { OneLine };
    mock.writeToScreen(sevenBitFont());
    mock.writeToScreen("\033(B!\"");
    auto& screen = mock.terminal.primaryScreen();
    REQUIRE(screen.at(LineOffset(0), ColumnOffset(0)).softGlyph().has_value());

    mock.writeToScreen("\033[1;1;1;1${"); // DECSERA over the first cell only
    CHECK_FALSE(screen.at(LineOffset(0), ColumnOffset(0)).softGlyph().has_value());
    CHECK(screen.at(LineOffset(0), ColumnOffset(0)).codepoint(0) == U' ');
    CHECK(screen.at(LineOffset(0), ColumnOffset(1)).softGlyph().has_value());
}

TEST_CASE("a wide character written over soft cells leaves no soft glyph behind", "[screen][softfont]")
{
    auto mock = MockTerm { OneLine };
    mock.writeToScreen(sevenBitFont());
    mock.writeToScreen("\033(B!\"");
    mock.writeToScreen("\033(B\033[1;1H世"); // plain US-ASCII again, so the wide character is plain text
    auto& screen = mock.terminal.primaryScreen();
    CHECK(screen.at(LineOffset(0), ColumnOffset(0)).codepoint(0) == U'世');
    CHECK_FALSE(screen.at(LineOffset(0), ColumnOffset(0)).softGlyph().has_value());
    CHECK_FALSE(screen.at(LineOffset(0), ColumnOffset(1)).softGlyph().has_value());
}
