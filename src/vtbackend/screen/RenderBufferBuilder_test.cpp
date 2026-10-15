// SPDX-License-Identifier: Apache-2.0
//
// Unit tests for RenderBufferBuilder: the colours a frame draws a cell in, and renderDetachedLine -- a grid
// row copied as host chrome, with the row's own colours and SGR, none of the cursor, selection or
// highlight that belong to where it really is.

#include <vtbackend/screen/RenderBufferBuilder.hpp>
#include <vtbackend/testing/MockTerm.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <chrono>
#include <vector>

using namespace vtbackend;
using namespace std::chrono_literals;

namespace
{
/// Grid row @p row copied by RenderBufferBuilder::renderDetachedLine() into a fresh vector.
[[nodiscard]] std::vector<RenderCell> copyOf(Terminal const& terminal,
                                             Line const& row,
                                             LineOffset screenRow,
                                             ColumnCount columns)
{
    auto cells = std::vector<RenderCell> {};
    RenderBufferBuilder::renderDetachedLine(cells, terminal, row, screenRow, columns);
    return cells;
}

/// The cell drawn at column @p column of the top row, in a frame built with the terminal's clock at @p now.
[[nodiscard]] RenderCell drawnCellAt(MockTerm<>& mc,
                                     std::chrono::steady_clock::time_point now,
                                     ColumnOffset column)
{
    mc.terminal.tick(now);
    mc.terminal.refreshRenderBuffer(true);
    auto const frame = mc.terminal.renderBuffer();
    auto const& cells = frame.get().cells;
    auto const cell = std::ranges::find(
        cells, CellLocation { .line = LineOffset(0), .column = column }, &RenderCell::position);
    REQUIRE(cell != cells.end());
    return *cell;
}

/// The colours column @p column of the top row is drawn in, in a frame built with the terminal's clock at
/// @p now.
[[nodiscard]] RGBColorPair drawnColorsAt(MockTerm<>& mc,
                                         std::chrono::steady_clock::time_point now,
                                         ColumnOffset column)
{
    auto const cell = drawnCellAt(mc, now, column);
    return RGBColorPair { .foreground = cell.attributes.foregroundColor,
                          .background = cell.attributes.backgroundColor };
}
} // namespace

TEST_CASE("RenderBufferBuilder.aLinkedCellIsDrawnWithItsLinksDecoration", "[renderbuffer]")
{
    // An OSC 8 link is drawn dotted underlined, in the palette's colour for links; the cells around it link
    // nowhere and are drawn without. The link starts in column 1, away from the mouse, which rests over
    // column 0 and would draw a link there as hovered.
    auto mc = MockTerm { PageSize { LineCount(3), ColumnCount(10) } };
    mc.writeToScreen("a\033]8;;https://example.com/\033\\L\033]8;;\033\\N");
    auto const now = mc.terminal.currentTime();

    auto const linked = drawnCellAt(mc, now, ColumnOffset(1));
    CHECK(linked.attributes.flags.contains(CellFlag::DottedUnderline));
    CHECK(linked.attributes.decorationColor == mc.terminal.colorPalette().hyperlinkDecoration.normal);

    for (auto const column: { ColumnOffset(0), ColumnOffset(2) })
    {
        auto const unlinked = drawnCellAt(mc, now, column);
        CHECK(!unlinked.attributes.flags.contains(CellFlag::DottedUnderline));
        CHECK(!unlinked.attributes.flags.contains(CellFlag::Underline));
    }
}

TEST_CASE("RenderBufferBuilder.blinkingTextIsDrawnInTheBlinkPhaseOfTheFramesClock", "[renderbuffer]")
{
    // SGR 5 follows the slow blinker, whose period is a second, and SGR 6 the rapid one, at 600 ms. Each
    // shows its text in full where a period starts, and fades it into the background halfway through.
    auto mc = MockTerm { PageSize { LineCount(3), ColumnCount(10) } };
    mc.writeToScreen("\033[5mS\033[6mR\033[m");
    auto const periodStart = std::chrono::steady_clock::time_point {};
    auto const slow = ColumnOffset(0);
    auto const rapid = ColumnOffset(1);
    auto const isFaded = [](RGBColorPair colors) {
        return colors.foreground == colors.background;
    };

    SECTION("both in full where their periods start")
    {
        CHECK(!isFaded(drawnColorsAt(mc, periodStart, slow)));
        CHECK(!isFaded(drawnColorsAt(mc, periodStart, rapid)));
    }

    SECTION("the slow blinker faded halfway through its period, the rapid one not")
    {
        CHECK(isFaded(drawnColorsAt(mc, periodStart + 500ms, slow)));
        CHECK(!isFaded(drawnColorsAt(mc, periodStart + 500ms, rapid)));
    }

    SECTION("the rapid blinker faded halfway through its period, the slow one not")
    {
        CHECK(isFaded(drawnColorsAt(mc, periodStart + 300ms, rapid)));
        CHECK(!isFaded(drawnColorsAt(mc, periodStart + 300ms, slow)));
    }
}

TEST_CASE("RenderBufferBuilder.detachedLine.appendsAfterWhatIsThere", "[renderbuffer][stickyheader]")
{
    auto mc = MockTerm { PageSize { LineCount(5), ColumnCount(20) } };
    mc.writeToScreen("ab");
    auto const& row = mc.terminal.primaryScreen().grid().lineAt(LineOffset(0));

    auto cells = copyOf(mc.terminal, row, LineOffset(0), ColumnCount(20));
    RenderBufferBuilder::renderDetachedLine(cells, mc.terminal, row, LineOffset(1), ColumnCount(20));
    REQUIRE(cells.size() == 4);
    // Each copy its own shaping group.
    CHECK(cells[1].groupEnd);
    CHECK(cells[2].groupStart);
    CHECK(cells[2].position == CellLocation { .line = LineOffset(1), .column = ColumnOffset(0) });
}

TEST_CASE("RenderBufferBuilder.detachedLine.keepsTheRowsOwnColours", "[renderbuffer][stickyheader]")
{
    auto mc = MockTerm { PageSize { LineCount(5), ColumnCount(20) } };
    mc.writeToScreen("\033[31mab\033[44mcd\033[m ef");
    auto const& palette = mc.terminal.colorPalette();
    auto const& row = mc.terminal.primaryScreen().grid().lineAt(LineOffset(0));

    auto const cells = copyOf(mc.terminal, row, LineOffset(3), ColumnCount(5));

    REQUIRE(cells.size() == 5);
    CHECK(cells[0].codepoints == U"a");
    // Placed on the row it is drawn over, not the one it was copied from.
    CHECK(cells[0].position == CellLocation { .line = LineOffset(3), .column = ColumnOffset(0) });
    CHECK(cells[4].position == CellLocation { .line = LineOffset(3), .column = ColumnOffset(4) });

    // The application's colours survive the copy ...
    CHECK(cells[0].attributes.foregroundColor == palette.normalColor(1));
    CHECK(cells[2].attributes.backgroundColor == palette.normalColor(4));
    // ... and a cell left on the page background stays on it, for whatever the copy is drawn over to show.
    CHECK(cells[0].attributes.backgroundColor == palette.defaultBackground);

    CHECK(cells.front().groupStart);
    CHECK(cells.back().groupEnd);
}

TEST_CASE("RenderBufferBuilder.detachedLine.copiesOnlyWhatShows", "[renderbuffer][stickyheader]")
{
    // A twenty-column limit, and two columns of text: the eighteen default cells after them draw nothing.
    auto mc = MockTerm { PageSize { LineCount(5), ColumnCount(20) } };
    mc.writeToScreen("ab");
    auto const& row = mc.terminal.primaryScreen().grid().lineAt(LineOffset(0));

    auto const cells = copyOf(mc.terminal, row, LineOffset(0), ColumnCount(20));
    REQUIRE(cells.size() == 2);
    CHECK(cells[1].codepoints == U"b");
}

TEST_CASE("RenderBufferBuilder.detachedLine.aWideGlyphAtTheEndOfTheTextIsCopiedWhole",
          "[renderbuffer][stickyheader]")
{
    auto mc = MockTerm { PageSize { LineCount(5), ColumnCount(20) } };
    mc.writeToScreen("a中");
    auto const& row = mc.terminal.primaryScreen().grid().lineAt(LineOffset(0));

    auto const cells = copyOf(mc.terminal, row, LineOffset(0), ColumnCount(20));
    REQUIRE(cells.size() >= 2);
    CHECK(cells[1].codepoints == U"中");
}

TEST_CASE("RenderBufferBuilder.detachedLine.dropsAWideGlyphThatWouldStraddleTheLimit",
          "[renderbuffer][stickyheader]")
{
    auto mc = MockTerm { PageSize { LineCount(5), ColumnCount(20) } };
    mc.writeToScreen("a中b");
    auto const& row = mc.terminal.primaryScreen().grid().lineAt(LineOffset(0));

    // '中' occupies columns 1-2; a limit of two columns cannot hold it, so the copy stops after 'a'.
    auto const cells = copyOf(mc.terminal, row, LineOffset(0), ColumnCount(2));
    REQUIRE(cells.size() == 1);
    CHECK(cells[0].codepoints == U"a");
}

TEST_CASE("RenderBufferBuilder.detachedLine.aBlankRowCopiesToNothing", "[renderbuffer][stickyheader]")
{
    auto mc = MockTerm { PageSize { LineCount(5), ColumnCount(20) } };
    auto const& row = mc.terminal.primaryScreen().grid().lineAt(LineOffset(1));
    REQUIRE(row.isBlank());
    CHECK(copyOf(mc.terminal, row, LineOffset(0), ColumnCount(20)).empty());
}
