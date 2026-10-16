// SPDX-License-Identifier: Apache-2.0
//
// Unit tests for RenderBufferBuilder: the colours a frame draws a cell in, the role it gives the cells of a
// link, and renderDetachedLine -- a grid row copied as host chrome, with the row's own colours and SGR,
// none of the cursor, selection or highlight that belong to where it really is.

#include <vtbackend/core/CellFlags.hpp>
#include <vtbackend/core/LinkDecoration.hpp>
#include <vtbackend/screen/RenderBufferBuilder.hpp>
#include <vtbackend/testing/MockTerm.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <chrono>
#include <map>
#include <memory>
#include <ranges>
#include <string_view>
#include <tuple>
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

/// The cells the terminal renders on screen row @p row, by column.
[[nodiscard]] std::map<int, RenderCell> renderedRow(MockTerm<>& mock, LineOffset row)
{
    mock.terminal.refreshRenderBuffer();
    auto const buffer = mock.terminal.renderBuffer();
    auto cells = std::map<int, RenderCell> {};
    for (auto const& cell: buffer.get().cells)
        if (cell.position.line == row)
            cells.emplace(unbox<int>(cell.position.column), cell);
    return cells;
}

/// Replaces screen row @p row with a batched line of @p text that one link, new to the terminal, covers
/// whole.
///
/// The parser does not leave such a row behind: whatever it writes under a link is a mixed line. A row
/// that is one link throughout is what a line assembled from SoA data is -- and what the renderer's batched
/// path takes for plain text.
///
/// @return The link's id.
HyperlinkId replaceRowWithLinkedText(MockTerm<>& mock, LineOffset row, std::string_view text)
{
    auto& hyperlinks = mock.terminal.hyperlinks();
    auto const link = hyperlinks.nextHyperlinkId++;
    hyperlinks.cache.emplace(
        link, std::make_shared<HyperlinkInfo>(HyperlinkInfo { .userId = {}, .uri = "https://a.b/" }));

    auto const columns = mock.terminal.pageSize().columns;
    auto soa = LineSoA {};
    initializeLineSoA(soa, columns);
    std::ignore = writeTextToSoA(soa, 0, text, GraphicsAttributes {}, link, /*asciiHint=*/true);
    mock.terminal.primaryScreen().grid().lineAt(row) = Line(LineFlags {}, std::move(soa), columns);
    return link;
}
} // namespace

TEST_CASE("RenderBufferBuilder.osc8LinkCarriesARoleNotAHardcodedUnderline", "[renderbuffer][links]")
{
    // An OSC 8 link's cells carry a role, and the colour scheme's colour for it; which decorator draws it is
    // the renderer's profile to say, so the cell's own underline flags stay clear. The cells around the link
    // link nowhere and carry nothing.
    auto mock = MockTerm { PageSize { LineCount(3), ColumnCount(20) } };
    mock.writeToScreen("\033]8;;https://a.b/\033\\link\033]8;;\033\\ rest");
    auto const& palette = mock.terminal.colorPalette();

    // The pointer of a fresh terminal rests over cell (0, 0), which would read the link as hovered.
    mock.terminal.sendMouseLeaveEvent();
    auto cells = renderedRow(mock, LineOffset(0));
    CHECK(cells.at(0).linkRole == LinkDecorationRole::Normal);
    CHECK_FALSE(cells.at(0).attributes.flags.any(UnderlineMask));
    CHECK(cells.at(0).attributes.decorationColor == palette.hyperlinkDecoration.normal);
    CHECK(cells.at(5).linkRole == LinkDecorationRole::None);
    CHECK_FALSE(cells.at(5).attributes.flags.any(UnderlineMask));

    // The pointer over one cell of the link takes the whole link into its hover role.
    mock.terminal.sendMouseMoveEvent(Modifier::None,
                                     CellLocation { .line = LineOffset(0), .column = ColumnOffset(1) },
                                     PixelCoordinate {},
                                     false);
    cells = renderedRow(mock, LineOffset(0));
    CHECK(cells.at(0).linkRole == LinkDecorationRole::Hover);
    CHECK(cells.at(3).linkRole == LinkDecorationRole::Hover);
    CHECK(cells.at(3).attributes.decorationColor == palette.hyperlinkDecoration.hover);
    CHECK(cells.at(5).linkRole == LinkDecorationRole::None);
}

TEST_CASE("RenderBufferBuilder.hoverNamesOnlyTheLinkUnderThePointer", "[renderbuffer][links]")
{
    auto mock = MockTerm { PageSize { LineCount(3), ColumnCount(20) } };
    mock.writeToScreen("\033]8;;https://a.b/\033\\one\033]8;;\033\\ "
                       "\033]8;;https://c.d/\033\\two\033]8;;\033\\");

    mock.terminal.sendMouseMoveEvent(Modifier::None,
                                     CellLocation { .line = LineOffset(0), .column = ColumnOffset(5) },
                                     PixelCoordinate {},
                                     false);
    auto const cells = renderedRow(mock, LineOffset(0));
    CHECK(cells.at(0).linkRole == LinkDecorationRole::Normal);
    CHECK(cells.at(2).linkRole == LinkDecorationRole::Normal);
    CHECK(cells.at(4).linkRole == LinkDecorationRole::Hover);
    CHECK(cells.at(6).linkRole == LinkDecorationRole::Hover);

    // Hovering is the frame's reading of the pointer, not a mark left on the link: once the pointer leaves,
    // the link is at rest again.
    mock.terminal.sendMouseLeaveEvent();
    auto const rested = renderedRow(mock, LineOffset(0));
    CHECK(rested.at(4).linkRole == LinkDecorationRole::Normal);
    CHECK(rested.at(6).linkRole == LinkDecorationRole::Normal);
}

TEST_CASE("RenderBufferBuilder.hoverFollowsTheContentUnderAStillPointer", "[renderbuffer][links]")
{
    // The frame asks what is under the pointer; no pointer event is needed to tell it that a scroll has put
    // another link there.
    auto mock = MockTerm { PageSize { LineCount(3), ColumnCount(10) }, LineCount(10) };
    mock.writeToScreen("h1\r\nh2\r\n"
                       "\033]8;;https://a.b/\033\\AAAA\033]8;;\033\\\r\n"
                       "\033]8;;https://c.d/\033\\BBBB\033]8;;\033\\\r\n"
                       "plain");
    auto const roleAt = [&](int row) {
        return renderedRow(mock, LineOffset(row)).at(0).linkRole;
    };

    // On screen: A on row 0, B on row 1, "plain" on row 2; the pointer is over A.
    mock.terminal.sendMouseMoveEvent(Modifier::None,
                                     CellLocation { .line = LineOffset(0), .column = ColumnOffset(1) },
                                     PixelCoordinate {},
                                     false);
    REQUIRE(roleAt(0) == LinkDecorationRole::Hover);
    REQUIRE(roleAt(1) == LinkDecorationRole::Normal);

    // One row back into the history: "h2" is under the pointer now, A has moved to row 1, B to row 2.
    // No pointer event came, yet A is not hovered any more.
    REQUIRE(mock.terminal.viewport().scrollUp(LineCount(1)));
    CHECK(roleAt(1) == LinkDecorationRole::Normal);
    CHECK(roleAt(2) == LinkDecorationRole::Normal);

    // The pointer moves to row 1, where A is; scrolling back brings B under it, and the hover goes along.
    mock.terminal.sendMouseMoveEvent(Modifier::None,
                                     CellLocation { .line = LineOffset(1), .column = ColumnOffset(1) },
                                     PixelCoordinate {},
                                     false);
    REQUIRE(roleAt(1) == LinkDecorationRole::Hover); // A
    REQUIRE(mock.terminal.viewport().scrollDown(LineCount(1)));
    CHECK(roleAt(0) == LinkDecorationRole::Normal); // A
    CHECK(roleAt(1) == LinkDecorationRole::Hover);  // B
}

TEST_CASE("RenderBufferBuilder.anEvictedLinkIsNotDecorated", "[renderbuffer][links]")
{
    // A link the terminal no longer holds opens nothing, so it is not drawn as one; a link it still holds is.
    auto mock = MockTerm { PageSize { LineCount(3), ColumnCount(20) } };
    mock.terminal.sendMouseLeaveEvent();
    mock.writeToScreen("\033]8;;https://old.example/\033\\old\033]8;;\033\\ "
                       "\033]8;;https://kept.example/\033\\kept\033]8;;\033\\");
    auto const& screen = mock.terminal.primaryScreen();
    auto const oldLink =
        screen.hyperlinkIdAt(CellLocation { .line = LineOffset(0), .column = ColumnOffset(0) });
    auto const keptLink =
        screen.hyperlinkIdAt(CellLocation { .line = LineOffset(0), .column = ColumnOffset(4) });
    REQUIRE(oldLink != HyperlinkId {});
    REQUIRE(keptLink != HyperlinkId {});
    REQUIRE(oldLink != keptLink);

    // A frame refreshes both, the one it meets last being the more recent.
    auto cells = renderedRow(mock, LineOffset(0));
    REQUIRE(cells.at(0).linkRole == LinkDecorationRole::Normal);
    REQUIRE(cells.at(4).linkRole == LinkDecorationRole::Normal);

    // Fill the cache; the one more link pushes out the least recently used, which is "old".
    auto& cache = mock.terminal.hyperlinks().cache;
    auto fillers = std::views::iota(size_t { 0 }, cache.capacity() - cache.size() + 1);
    for (auto const index: fillers)
        cache.emplace(HyperlinkId { static_cast<uint16_t>(10000 + index) },
                      std::make_shared<HyperlinkInfo>(HyperlinkInfo { .userId = {}, .uri = "x:" }));
    REQUIRE_FALSE(cache.contains(oldLink));
    REQUIRE(cache.contains(keptLink));

    cells = renderedRow(mock, LineOffset(0));
    CHECK(cells.at(0).linkRole == LinkDecorationRole::None);
    CHECK(cells.at(2).linkRole == LinkDecorationRole::None);
    CHECK(cells.at(4).linkRole == LinkDecorationRole::Normal);
    CHECK(cells.at(7).linkRole == LinkDecorationRole::Normal);
}

TEST_CASE("RenderBufferBuilder.aRowOneLinkFillsIsDecorated", "[renderbuffer][links]")
{
    // Every column carries the same link, so the row is a batched line, and a batched line draws no
    // decoration at all. The cursor is on the row below: a row under it is never batched.
    auto mock = MockTerm { PageSize { LineCount(3), ColumnCount(10) } };
    mock.terminal.sendMouseLeaveEvent(); // a fresh terminal's pointer rests over cell (0, 0)
    mock.writeToScreen("\r\n");
    auto const link = replaceRowWithLinkedText(mock, LineOffset(0), "0123456789");
    REQUIRE(mock.terminal.primaryScreen().grid().lineAt(LineOffset(0)).isTrivialBuffer());
    auto const& palette = mock.terminal.colorPalette();

    auto cells = renderedRow(mock, LineOffset(0));
    REQUIRE(cells.size() == 10);
    for (auto const& [column, cell]: cells)
    {
        CHECK(cell.linkRole == LinkDecorationRole::Normal);
        CHECK(cell.attributes.decorationColor == palette.hyperlinkDecoration.normal);
    }

    // The pointer over it takes the whole row into the hover role.
    REQUIRE(mock.terminal.hyperlinks().hyperlinkById(link) != nullptr);
    mock.terminal.sendMouseMoveEvent(Modifier::None,
                                     CellLocation { .line = LineOffset(0), .column = ColumnOffset(4) },
                                     PixelCoordinate {},
                                     false);
    cells = renderedRow(mock, LineOffset(0));
    REQUIRE(cells.size() == 10);
    for (auto const& [column, cell]: cells)
    {
        CHECK(cell.linkRole == LinkDecorationRole::Hover);
        CHECK(cell.attributes.decorationColor == palette.hyperlinkDecoration.hover);
    }
}

TEST_CASE("RenderBufferBuilder.aTrivialRowWithoutALinkStaysBatched", "[renderbuffer][links]")
{
    // The link decoration costs the plain row nothing: it still goes out as one RenderLine, without cells.
    auto mock = MockTerm { PageSize { LineCount(3), ColumnCount(10) } };
    mock.writeToScreen("plain\r\n");
    mock.terminal.refreshRenderBuffer();
    auto const buffer = mock.terminal.renderBuffer();
    CHECK(std::ranges::none_of(buffer.get().cells,
                               [](RenderCell const& cell) { return cell.position.line == LineOffset(0); }));
    CHECK(std::ranges::any_of(buffer.get().lines,
                              [](RenderLine const& line) { return line.lineOffset == LineOffset(0); }));
}

TEST_CASE("RenderBufferBuilder.detachedLine.aLinkKeepsItsRole", "[renderbuffer][links][stickyheader]")
{
    // A copy on the sticky header is no hover target -- the header takes the pointer itself -- so its link
    // is at rest.
    auto mc = MockTerm { PageSize { LineCount(5), ColumnCount(20) } };
    mc.writeToScreen("\033]8;;https://a.b/\033\\ab\033]8;;\033\\c");
    auto const& row = mc.terminal.primaryScreen().grid().lineAt(LineOffset(0));

    auto const cells = copyOf(mc.terminal, row, LineOffset(0), ColumnCount(20));
    REQUIRE(cells.size() == 3);
    CHECK(cells[0].linkRole == LinkDecorationRole::Normal);
    CHECK(cells[1].linkRole == LinkDecorationRole::Normal);
    CHECK(cells[0].attributes.decorationColor == mc.terminal.colorPalette().hyperlinkDecoration.normal);
    CHECK_FALSE(cells[0].attributes.flags.any(UnderlineMask));
    CHECK(cells[2].linkRole == LinkDecorationRole::None);
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
