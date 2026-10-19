// SPDX-License-Identifier: Apache-2.0
#include <vtbackend/links/ScanText.hpp>
#include <vtbackend/testing/MockTerm.hpp>

#include <catch2/catch_test_macros.hpp>

#include <string>
#include <string_view>
#include <utility>
#include <vector>

using namespace vtbackend;

namespace
{
/// Row 0 of a 20-column terminal that printed @p text.
ScanRow scanRowOf(std::string_view text, ColumnCount columns = ColumnCount(20))
{
    auto mock = MockTerm { PageSize { LineCount(3), columns } };
    mock.writeToScreen(text);
    return makeScanRow(
        mock.terminal.primaryScreen().grid().lineAt(LineOffset(0)), LineOffset(0), LineContinuation::No);
}

ScanText single(ScanRow row)
{
    auto rows = std::vector<ScanRow> { std::move(row) };
    return ScanText { rows, ScanClip {} };
}
} // namespace

TEST_CASE("ScanText.asciiRowIsItsOwnText", "[links]")
{
    auto const row = scanRowOf("see https://x.y/");
    CHECK(row.text.starts_with("see https://x.y/"));
    CHECK(row.text.size() == 20); // trailing empty cells are spaces
    CHECK(row.original.empty());
    REQUIRE(row.cellByteStart.size() == 20);
    CHECK(row.cellByteStart[4] == 4);
}

TEST_CASE("ScanText.wideCharacterContinuationRepeatsItsLeader", "[links]")
{
    auto const text = single(scanRowOf("東京a"));
    // 東 (3 bytes) covers columns 0-1, 京 columns 2-3, 'a' column 4.
    CHECK(text.text().starts_with("東京a"));
    CHECK(text.firstCellAt(0) == 0);
    CHECK(text.lastCellBefore(3) == 1); // 東's second column belongs to it
    CHECK(text.firstCellAt(6) == 4);
    CHECK(text.positionOf(text.firstCellAt(6)).column == ColumnOffset(4));
}

TEST_CASE("ScanText.clusterKeepsEveryCodepointInOneCell", "[links]")
{
    auto const text = single(scanRowOf("e\xCC\x81x")); // e + U+0301 COMBINING ACUTE, then x
    CHECK(text.text().starts_with("e\xCC\x81x"));
    CHECK(text.firstCellAt(3) == 1); // x is column 1, not 2
}

TEST_CASE("ScanText.separatorsAndWidePunctuationBlankByteForByte", "[links]")
{
    // U+3000 IDEOGRAPHIC SPACE, U+00A0 NO-BREAK SPACE and U+FF09 FULLWIDTH RIGHT PARENTHESIS read as word
    // breaks; ASCII ')' does not.
    auto const row = scanRowOf("a\xE3\x80\x80"
                               "b\xC2\xA0"
                               "c\xEF\xBC\x89)");
    CHECK(row.text.starts_with("a   b  c   )"));
    CHECK(row.original.starts_with("a\xE3\x80\x80"
                                   "b\xC2\xA0"
                                   "c\xEF\xBC\x89)"));
    CHECK(row.text.size() == row.original.size());
}

TEST_CASE("ScanText.osc8CellsBlank", "[links]")
{
    auto const row = scanRowOf("\033]8;;https://a.b/\033\\link\033]8;;\033\\ x");
    CHECK(row.text.starts_with("     x"));
    CHECK(row.original.starts_with("link x"));
}

TEST_CASE("ScanText.blankRowIsSpaces", "[links]")
{
    auto const row = scanRowOf("");
    CHECK(row.text == std::string(20, ' '));
    CHECK(row.cellByteStart.back() == 19);
}

TEST_CASE("ScanText.joinsWrappedRowsIntoOneLogicalLine", "[links]")
{
    auto mock = MockTerm { PageSize { LineCount(4), ColumnCount(10) } };
    mock.writeToScreen("0123456789abc\r\nnext");
    auto const& grid = mock.terminal.primaryScreen().grid();
    auto rows = std::vector<ScanRow> {};
    for (auto const line: { 0, 1, 2 })
        rows.push_back(
            makeScanRow(grid.lineAt(LineOffset(line)),
                        LineOffset(line),
                        grid.isLineWrapped(LineOffset(line)) ? LineContinuation::Yes : LineContinuation::No));

    auto const lines = joinLogicalLines(rows, LineContinuation::No);
    REQUIRE(lines.size() == 2);
    CHECK(lines[0].rowCount() == 2);
    CHECK(lines[0].text().starts_with("0123456789abc"));
    CHECK(lines[0].positionOf(lines[0].firstCellAt(11))
          == ScanPosition { .row = 1, .column = ColumnOffset(1) });
    CHECK(lines[0].clip() == ScanClip {});
    CHECK(lines[0].cellIndexOf(ScanPosition { .row = 1, .column = ColumnOffset(1) }) == 11);
    CHECK(lines[1].lineOf(0) == LineOffset(2));
    CHECK(lines[1].clip() == ScanClip {});
}

TEST_CASE("ScanText.leadingContinuationIsClippedAtTheFront", "[links]")
{
    auto rows = std::vector<ScanRow> { makePlainScanRow("tail", LineOffset(5), LineContinuation::Yes) };
    auto const lines = joinLogicalLines(rows, LineContinuation::Yes);
    REQUIRE(lines.size() == 1);
    CHECK(lines[0].clip() == ScanClip { .front = ScanEdge::Clipped, .back = ScanEdge::Clipped });
}

TEST_CASE("ScanText.budgetCutsALongLogicalLine", "[links]")
{
    auto rows = std::vector<ScanRow> {};
    for (auto const line: { 0, 1, 2, 3 })
        rows.push_back(makePlainScanRow(
            "abcd", LineOffset(line), line == 0 ? LineContinuation::No : LineContinuation::Yes));
    auto const lines = joinLogicalLines(rows, LineContinuation::No, 8);
    REQUIRE(lines.size() == 2);
    CHECK(lines[0].clip() == ScanClip { .front = ScanEdge::Whole, .back = ScanEdge::Clipped });
    CHECK(lines[1].clip() == ScanClip { .front = ScanEdge::Clipped, .back = ScanEdge::Whole });
}

TEST_CASE("ScanText.plainRowIsOneCellPerCodepoint", "[links]")
{
    auto const text = single(makePlainScanRow("ä/b"));
    CHECK(text.firstCellAt(2) == 1); // '/' follows the two bytes of ä
    CHECK(text.cellIndexOf(ScanPosition { .row = 0, .column = ColumnOffset(2) }) == 2);
}

TEST_CASE("ScanText.plainRowBlanksAsTheGridRowDoes", "[links]")
{
    // The settings page previews a pattern on a plain row: one table decides what reads as a word break for
    // both, so the preview cannot disagree with the terminal. U+3000 IDEOGRAPHIC SPACE, U+00A0 NO-BREAK SPACE
    // and U+FF09 FULLWIDTH RIGHT PARENTHESIS blank; ASCII ')' does not.
    auto const printed = std::string_view { "a\xE3\x80\x80"
                                            "b\xC2\xA0"
                                            "c\xEF\xBC\x89)" };
    auto const row = makePlainScanRow(printed);
    CHECK(row.text == "a   b  c   )");
    CHECK(row.original == printed);
    CHECK(scanRowOf(printed).text.starts_with(row.text));
}

TEST_CASE("ScanText.lastCellBeforeClampsAtBothEnds", "[links]")
{
    auto const text = single(makePlainScanRow("abc"));
    CHECK(text.lastCellBefore(3) == 2);
    CHECK(text.lastCellBefore(100) == 2); // past the end is the last cell
    CHECK(text.lastCellBefore(0) == 0);

    auto const empty = single(makePlainScanRow(""));
    CHECK(empty.lastCellBefore(0) == 0);
    CHECK(empty.firstCellAt(0) == 0);
}

TEST_CASE("ScanText.blankedWidePunctuationKeepsItsTwoCellsOnOneByteRange", "[links]")
{
    // U+FF09 is three bytes over two columns: both cells begin at byte 1, 'b' follows at byte 4.
    auto const row = scanRowOf("a\xEF\xBC\x89"
                               "b");
    REQUIRE(row.cellByteStart.size() == 20);
    CHECK(row.cellByteStart[0] == 0);
    CHECK(row.cellByteStart[1] == 1);
    CHECK(row.cellByteStart[2] == 1);
    CHECK(row.cellByteStart[3] == 4);
    CHECK(row.text.starts_with("a   b"));

    auto const text = single(row);
    CHECK(text.original(0, 5)
          == "a\xEF\xBC\x89"
             "b");
    CHECK(text.lastCellBefore(4) == 2); // the fullwidth parenthesis's second column
}

TEST_CASE("ScanText.osc8RunMapsOneCellPerCharacterAndKeepsItsPrintedText", "[links]")
{
    auto const row = scanRowOf("\033]8;;https://a.b/\033\\link\033]8;;\033\\ x");
    REQUIRE(row.cellByteStart.size() >= 6);
    for (auto const cell: { 0u, 1u, 2u, 3u, 4u, 5u })
        CHECK(row.cellByteStart[cell] == cell);

    auto const text = single(row);
    CHECK(text.text().substr(0, 6) == "     x");
    CHECK(text.original(0, 4) == "link");
    CHECK(text.original(0, 6) == "link x");
}

TEST_CASE("ScanText.joinLogicalLinesBudgetDefaultsToMaxScanCells", "[links]")
{
    auto rows = std::vector<ScanRow> {};
    auto const half = std::string(MaxScanCells / 2, 'a');
    rows.push_back(makePlainScanRow(half, LineOffset(0), LineContinuation::No));
    rows.push_back(makePlainScanRow(half, LineOffset(1), LineContinuation::Yes));

    // Exactly the budget: one text, whole.
    auto const fits = joinLogicalLines(rows, LineContinuation::No);
    REQUIRE(fits.size() == 1);
    CHECK(fits[0].clip() == ScanClip {});

    // One cell over: cut in two.
    rows.push_back(makePlainScanRow("b", LineOffset(2), LineContinuation::Yes));
    auto const cut = joinLogicalLines(rows, LineContinuation::No);
    REQUIRE(cut.size() == 2);
    CHECK(cut[0].clip() == ScanClip { .front = ScanEdge::Whole, .back = ScanEdge::Clipped });
    CHECK(cut[1].clip() == ScanClip { .front = ScanEdge::Clipped, .back = ScanEdge::Whole });
}

TEST_CASE("ScanText.unwrappedRowsStayApart", "[links]")
{
    auto rows = std::vector<ScanRow> { makePlainScanRow("one", LineOffset(0)),
                                       makePlainScanRow("two", LineOffset(1)) };
    auto const lines = joinLogicalLines(rows, LineContinuation::No);
    REQUIRE(lines.size() == 2);
    CHECK(lines[0].text() == "one");
    CHECK(lines[0].lineOf(0) == LineOffset(0));
    CHECK(lines[1].text() == "two");
    CHECK(lines[1].lineOf(0) == LineOffset(1));
}

TEST_CASE("ScanText.continuationsJoinTheirHeadAndStopAtTheNextHead", "[links]")
{
    auto rows = std::vector<ScanRow> {
        makePlainScanRow("head", LineOffset(3), LineContinuation::No),
        makePlainScanRow("-mid", LineOffset(4), LineContinuation::Yes),
        makePlainScanRow("-end", LineOffset(5), LineContinuation::Yes),
        makePlainScanRow("next", LineOffset(6), LineContinuation::No),
    };
    auto const lines = joinLogicalLines(rows, LineContinuation::No);
    REQUIRE(lines.size() == 2);
    CHECK(lines[0].text() == "head-mid-end");
    CHECK(lines[0].rowCount() == 3);
    CHECK(lines[0].lineOf(0) == LineOffset(3));
    CHECK(lines[0].lineOf(2) == LineOffset(5));
    CHECK(lines[1].text() == "next");
    CHECK(lines[1].lineOf(0) == LineOffset(6));
}

TEST_CASE("ScanText.nonConsecutiveRowsBreakTheRun", "[links]")
{
    // A gap in the grid lines would break the row arithmetic every consumer relies on.
    auto rows = std::vector<ScanRow> { makePlainScanRow("aaa", LineOffset(0), LineContinuation::No),
                                       makePlainScanRow("bbb", LineOffset(7), LineContinuation::Yes) };
    auto const lines = joinLogicalLines(rows, LineContinuation::No);
    REQUIRE(lines.size() == 2);
    CHECK(lines[1].lineOf(0) == LineOffset(7));
    CHECK(lines[1].clip().front == ScanEdge::Clipped);
}

TEST_CASE("ScanText.positionOfMapsAcrossRowBoundaries", "[links]")
{
    auto rows = std::vector<ScanRow> {
        makePlainScanRow("abc", LineOffset(-2), LineContinuation::No),
        makePlainScanRow("def", LineOffset(-1), LineContinuation::Yes),
        makePlainScanRow("ghi", LineOffset(0), LineContinuation::Yes),
    };
    auto const lines = joinLogicalLines(rows, LineContinuation::No);
    REQUIRE(lines.size() == 1);
    auto const& text = lines[0];
    CHECK(text.positionOf(0) == ScanPosition { .row = 0, .column = ColumnOffset(0) });
    CHECK(text.positionOf(2) == ScanPosition { .row = 0, .column = ColumnOffset(2) });
    CHECK(text.positionOf(3) == ScanPosition { .row = 1, .column = ColumnOffset(0) });
    CHECK(text.positionOf(5) == ScanPosition { .row = 1, .column = ColumnOffset(2) });
    CHECK(text.positionOf(6) == ScanPosition { .row = 2, .column = ColumnOffset(0) });
    CHECK(text.positionOf(8) == ScanPosition { .row = 2, .column = ColumnOffset(2) });
    // Past the end clamps to the last cell there is.
    CHECK(text.positionOf(99) == ScanPosition { .row = 2, .column = ColumnOffset(2) });
    CHECK(text.lineOf(0) == LineOffset(-2));
    CHECK(text.lineOf(2) == LineOffset(0));
}

TEST_CASE("ScanText.scanRowsOfReadsGridRowsAndMarksWrappedOnesAsContinuations", "[links]")
{
    auto mock = MockTerm { PageSize { LineCount(4), ColumnCount(10) } };
    mock.writeToScreen("0123456789abc\r\nnext");
    auto const& grid = mock.terminal.primaryScreen().grid();

    auto const rows = scanRowsOf(grid, LineOffset(0), LineOffset(2));
    REQUIRE(rows.size() == 3);
    CHECK(rows[0].line == LineOffset(0));
    CHECK(rows[0].continuation == LineContinuation::No);
    CHECK(rows[0].text == "0123456789");
    CHECK(rows[1].line == LineOffset(1));
    CHECK(rows[1].continuation == LineContinuation::Yes);
    CHECK(rows[1].text.starts_with("abc"));
    CHECK(rows[2].continuation == LineContinuation::No);
    CHECK(rows[2].text.starts_with("next"));
}

TEST_CASE("ScanText.scanExtentOfExtendsToWholeLogicalLines", "[links]")
{
    // Rows 0 and 1 hold one logical line; row 2 is a line of its own.
    auto mock = MockTerm { PageSize { LineCount(4), ColumnCount(10) } };
    mock.writeToScreen("0123456789abc\r\nnext");
    auto const& grid = mock.terminal.primaryScreen().grid();

    SECTION("a stretch inside a wrapped line grows to both of its ends")
    {
        auto const extent = scanExtentOf(grid, LineOffset(1), LineOffset(1));
        CHECK(extent.first == LineOffset(0));
        CHECK(extent.last == LineOffset(1));
        CHECK(extent.continuesBelow == LineContinuation::No);
    }

    SECTION("an unwrapped row stays as it is")
    {
        auto const extent = scanExtentOf(grid, LineOffset(2), LineOffset(2));
        CHECK(extent.first == LineOffset(2));
        CHECK(extent.last == LineOffset(2));
    }

    SECTION("the walk is bounded by the cell budget, and says the line goes on")
    {
        auto wide = MockTerm { PageSize { LineCount(6), ColumnCount(10) } };
        wide.writeToScreen(std::string(50, 'x')); // rows 0-4, one logical line
        auto const& wideGrid = wide.terminal.primaryScreen().grid();

        // 25 cells at 10 columns reach two rows either way: the whole line.
        auto const whole = scanExtentOf(wideGrid, LineOffset(2), LineOffset(2), 25);
        CHECK(whole.first == LineOffset(0));
        CHECK(whole.last == LineOffset(4));
        CHECK(whole.continuesBelow == LineContinuation::No);

        // 10 cells reach one row either way: row 4 continues past what was read.
        auto const bounded = scanExtentOf(wideGrid, LineOffset(2), LineOffset(2), 10);
        CHECK(bounded.first == LineOffset(1));
        CHECK(bounded.last == LineOffset(3));
        CHECK(bounded.continuesBelow == LineContinuation::Yes);
    }
}
