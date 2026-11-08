// SPDX-License-Identifier: Apache-2.0
#include <vtbackend/core/Image.hpp>
#include <vtbackend/core/Primitives.hpp>
#include <vtbackend/core/SoftGlyphRef.hpp>
#include <vtbackend/screen/Screen.hpp>
#include <vtbackend/testing/MockTerm.hpp>
#include <vtbackend/testing/TestHelpers.hpp>

#include <catch2/catch_test_macros.hpp>

#include <format>
#include <memory>
#include <optional>
#include <ranges>
#include <string>
#include <string_view>

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
        REQUIRE(ra.glyph != nullptr);
        REQUIRE(rb.glyph != nullptr);
        CHECK(ra.glyph->id == rb.glyph->id);
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
