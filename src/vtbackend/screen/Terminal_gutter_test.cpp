// SPDX-License-Identifier: Apache-2.0
#include <vtbackend/core/CommandBlockOutcome.hpp>
#include <vtbackend/render/RenderBuffer.hpp>
#include <vtbackend/screen/Gutter.hpp>
#include <vtbackend/screen/GutterSettings.hpp>
#include <vtbackend/screen/Terminal.hpp>
#include <vtbackend/screen/TerminalClocks.hpp>
#include <vtbackend/shell/BlockChipPlacement.hpp>
#include <vtbackend/shell/BlockPill.hpp>
#include <vtbackend/shell/BlockRail.hpp>
#include <vtbackend/shell/BlockStatusText.hpp>
#include <vtbackend/shell/Folding.hpp>
#include <vtbackend/shell/StickyHeader.hpp>
#include <vtbackend/testing/MockTerm.hpp>
#include <vtbackend/testing/TestHelpers.hpp>

#include <vtpty/MockPty.hpp>

#include <core/Environment.hpp>
#include <core/platform/Clock.hpp>

#include <libunicode/convert.h>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdlib>
#include <format>
#include <map>
#include <memory>
#include <optional>
#include <ranges>
#include <set>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

using namespace vtbackend;
using namespace vtbackend::test;
using namespace std::chrono_literals;

namespace chrono = std::chrono;

namespace
{
/// 10:00:00 UTC on 2026-10-09.
constexpr auto SessionStart =
    chrono::system_clock::time_point { chrono::sys_days { chrono::year { 2026 } / chrono::October / 9 }
                                       + 10h };

// The time zone is as mandatory as the clocks beside it: a conversion left out, or a null one, is refused at
// compile time rather than called when the first timestamp is drawn.
static_assert(!std::is_default_constructible_v<LocalTimeConverter>);
static_assert(!std::is_constructible_v<LocalTimeConverter, std::nullptr_t>);
static_assert(std::is_convertible_v<decltype(&systemLocalTime), LocalTimeConverter>);

/// Whether `TerminalClocks { .steady = ..., .wall = ... }` compiles without naming a time zone.
template <typename Steady, typename Wall>
concept LeavesOutTheTimeZone =
    requires { TerminalClocks { .steady = std::declval<Steady>(), .wall = std::declval<Wall>() }; };

static_assert(!LeavesOutTheTimeZone<core::platform::ManualClock&, core::platform::ManualWallClock&>);
} // namespace

TEST_CASE("Gutter.terminal.localTimeGoesThroughTheInjectedConverter", "[gutter]")
{
    auto mc = MockTerm { PageSize { LineCount(5), ColumnCount(20) }, &fixedOffsetLocalTime<90> };
    CHECK(mc.terminal.localTimeOf(SessionStart)
          == chrono::local_days { chrono::year { 2026 } / chrono::October / 9 } + 11h + 30min);
}

namespace
{
/// @p prefix followed by the one character @p glyph.
[[nodiscard]] std::u32string withGlyph(std::u32string prefix, char32_t glyph)
{
    prefix.push_back(glyph);
    return prefix;
}

/// One annotation of a frame, read back from the cells it was laid out as.
struct Annotation
{
    CellLocation position;       ///< Where its first cell is.
    std::u32string text;         ///< What its cells spell.
    RenderAttributes attributes; ///< Its first cell's colours.
};

/// The annotations of @p buffer, one per shaping group of RenderBuffer::annotations.
[[nodiscard]] std::vector<Annotation> annotationsOf(RenderBuffer const& buffer)
{
    auto annotations = std::vector<Annotation> {};
    for (auto const& cell: buffer.annotations)
    {
        if (cell.groupStart || annotations.empty())
            annotations.push_back(
                Annotation { .position = cell.position, .text = {}, .attributes = cell.attributes });
        annotations.back().text += cell.codepoints;
    }
    return annotations;
}
} // namespace

TEST_CASE("Gutter.terminal.aBlockIsColouredByHowItEnded", "[gutter]")
{
    auto mc = MockTerm { PageSize { LineCount(10), ColumnCount(20) } };
    mc.terminal.settings().gutter =
        GutterSettings { .foldMarkers = true, .exitStatus = true, .userMarks = false };
    runCommand(mc, "true", { "ok" }, 0);   // head on row 0, output on row 1
    runCommand(mc, "false", { "bad" }, 1); // head on row 2, output on row 3

    auto const buffer = renderOf(mc.terminal);
    auto const& palette = mc.terminal.colorPalette();
    auto const colourOf = [&](int row) {
        auto const cell = std::ranges::find(buffer.gutter, LineOffset(row), &RenderGutterCell::lineOffset);
        REQUIRE(cell != buffer.gutter.end());
        return cell->attributes.foregroundColor;
    };

    // A success the scheme does not colour looks like the fold column always did.
    CHECK(colourOf(0) == palette.foldMarkerColors().foreground);
    CHECK(colourOf(1) == palette.foldMarkerColors().foreground);
    // Every glyph of a failed block takes the failure colour, its body as much as its head.
    CHECK(colourOf(2) == palette.blockStatusColor(CommandBlockOutcome::Failure));
    CHECK(colourOf(3) == palette.blockStatusColor(CommandBlockOutcome::Failure));
}

TEST_CASE("Gutter.terminal.aRowErasedInsideABlockKeepsTheBlocksColour", "[gutter]")
{
    // A row an erase blanks while the cursor is elsewhere carries no block until the cursor reaches it
    // again. That is no information, not "outside any block": the fold the row lies in still names its
    // block, so it is drawn like the rest of the fold.
    auto mc = MockTerm { PageSize { LineCount(10), ColumnCount(20) } };
    mc.terminal.settings().gutter =
        GutterSettings { .foldMarkers = true, .exitStatus = true, .userMarks = false };
    startCommand(mc, "false");                    // head on row 0; the output starts on row 1
    mc.writeToScreen("a\r\nb\r\nc\r\nd\r\n");     // rows 1-4
    mc.writeToScreen("\033[2;1H\033[J\033[6;1H"); // erase below row 1, then step over rows 2-4 to row 5
    mc.writeToScreen(commandEnd(1));
    auto const& grid = mc.terminal.primaryScreen().grid();
    REQUIRE(grid.lineAt(LineOffset(1)).blockId() != CommandBlockId {});
    REQUIRE(grid.lineAt(LineOffset(2)).blockId() == CommandBlockId {});
    REQUIRE(grid.lineAt(LineOffset(4)).blockId() == CommandBlockId {});

    auto const failure = mc.terminal.colorPalette().blockStatusColor(CommandBlockOutcome::Failure);
    auto const buffer = renderOf(mc.terminal);
    auto const rows = gutterRows(buffer, 1);
    // The command failed, so its fold is drawn heavy -- on the erased rows as much as on the others.
    auto const heavy = [](BlockColumnGlyph glyph) {
        return blockColumnCodepoint(glyph, FoldWeight::Heavy);
    };
    CHECK(rows.at(0) == withGlyph(U"", heavy(BlockColumnGlyph::FoldExpanded)));
    CHECK(rows.at(1) == withGlyph(U"", heavy(BlockColumnGlyph::FoldBody)));
    CHECK(rows.at(3) == withGlyph(U"", heavy(BlockColumnGlyph::FoldBody)));
    CHECK(rows.at(4) == withGlyph(U"", heavy(BlockColumnGlyph::FoldBodyEnd)));
    REQUIRE(buffer.gutter.size() == 5);
    for (auto const& cell: buffer.gutter)
    {
        CAPTURE(cell.lineOffset);
        CHECK(cell.attributes.foregroundColor == failure);
    }
}

TEST_CASE("Gutter.terminal.aRunningCommandShowsATriangle", "[gutter]")
{
    auto mc = MockTerm { PageSize { LineCount(10), ColumnCount(20) } };
    mc.terminal.settings().gutter =
        GutterSettings { .foldMarkers = true, .exitStatus = true, .userMarks = false };
    startCommand(mc, "sleep 9");
    mc.writeToScreen("still going\r\n");

    auto const buffer = renderOf(mc.terminal);
    // Only the head: a running command has nothing to fold yet.
    REQUIRE(buffer.gutter.size() == 1);
    CHECK(buffer.gutter[0].lineOffset == LineOffset(0));
    CHECK(buffer.gutter[0].column == ColumnOffset(-1));
    CHECK(buffer.gutter[0].codepoint == U'􏀂');
    CHECK(buffer.gutter[0].attributes.foregroundColor
          == mc.terminal.colorPalette().blockStatusColor(CommandBlockOutcome::Running));
}

TEST_CASE("Gutter.terminal.withFoldMarkersOffAFinishedHeadShowsTheDot", "[gutter]")
{
    auto mc = MockTerm { PageSize { LineCount(10), ColumnCount(20) } };
    mc.terminal.settings().gutter =
        GutterSettings { .foldMarkers = false, .exitStatus = true, .userMarks = false };
    runCommand(mc, "ls", { "file1", "file2" });

    auto const buffer = renderOf(mc.terminal);
    REQUIRE(buffer.gutter.size() == 1);
    CHECK(buffer.gutter[0].lineOffset == LineOffset(0));
    CHECK(buffer.gutter[0].codepoint == U'􏀄');
}

TEST_CASE("Gutter.terminal.aUserMarkShowsADiamondOverTheFoldBody", "[gutter]")
{
    auto mc = MockTerm { PageSize { LineCount(10), ColumnCount(20) } };
    mc.terminal.settings().gutter =
        GutterSettings { .foldMarkers = true, .exitStatus = false, .userMarks = true };
    runCommand(mc, "ls", { "file1", "file2" });
    mc.terminal.primaryScreen().setLogicalLineFlags(LineOffset(1), LineFlag::UserMark, true);

    auto const rows = gutterRows(renderOf(mc.terminal), 1);
    CHECK(rows.at(0) == withGlyph(U"", foldMarkerGlyph(FoldMarker::Expanded)));
    CHECK(rows.at(1) == U"􏀃");
    CHECK(rows.at(2) == withGlyph(U"", foldMarkerGlyph(FoldMarker::BodyEnd)));

    mc.terminal.settings().gutter.userMarks = false;
    CHECK(gutterRows(renderOf(mc.terminal), 1).at(1) == withGlyph(U"", foldMarkerGlyph(FoldMarker::Body)));
}

TEST_CASE("Gutter.terminal.absoluteNumbersCountEveryRowTheSessionHad", "[gutter]")
{
    // Three rows of page and two of history: ten rows written leave five evicted.
    auto mc = MockTerm { PageSize { LineCount(3), ColumnCount(10) }, LineCount(2) };
    mc.terminal.settings().gutter = GutterSettings { .foldMarkers = false,
                                                     .exitStatus = false,
                                                     .userMarks = false,
                                                     .lineNumbers = LineNumberMode::Absolute,
                                                     .lineNumberWidth = 3 };
    mc.writeToScreen("1\r\n2\r\n3\r\n4\r\n5\r\n6\r\n7\r\n8\r\n9\r\n10");

    auto const rows = gutterRows(renderOf(mc.terminal), 3);
    CHECK(rows.at(0) == U"  8");
    CHECK(rows.at(1) == U"  9");
    CHECK(rows.at(2) == U" 10");

    // The numbers belong to the rows, so scrolling into the history takes them along.
    REQUIRE(mc.terminal.viewport().scrollUp(LineCount(2)));
    auto const scrolled = gutterRows(renderOf(mc.terminal), 3);
    CHECK(scrolled.at(0) == U"  6");
    CHECK(scrolled.at(2) == U"  8");
}

TEST_CASE("Gutter.terminal.relativeNumbersCountVisibleRowsFromTheCursor", "[gutter]")
{
    auto mc = MockTerm { PageSize { LineCount(10), ColumnCount(20) } };
    mc.terminal.settings().gutter = GutterSettings { .foldMarkers = true,
                                                     .exitStatus = false,
                                                     .userMarks = false,
                                                     .lineNumbers = LineNumberMode::Relative,
                                                     .lineNumberWidth = 3 };
    runCommand(mc, "a", { "1", "2", "3" }); // head on row 0, output on rows 1-3
    REQUIRE(mc.terminal.primaryScreen().cursor().position.line == LineOffset(4));

    // Three columns of numbers, then the block column.
    auto const expanded = gutterRows(renderOf(mc.terminal), 4);
    CHECK(expanded.at(0) == withGlyph(U"  4", foldMarkerGlyph(FoldMarker::Expanded)));
    CHECK(expanded.at(3) == withGlyph(U"  1", foldMarkerGlyph(FoldMarker::BodyEnd)));
    CHECK(expanded.at(4) == U"  0 ");

    // Collapsed, the head is ONE visible row above the cursor -- exactly where `1k` lands.
    collapseOnlyFold(mc);
    auto const collapsed = gutterRows(renderOf(mc.terminal), 4);
    auto const head = std::ranges::find_if(collapsed, [](auto const& row) {
        return row.second.back() == foldMarkerGlyph(FoldMarker::Collapsed);
    });
    REQUIRE(head != collapsed.end());
    CHECK(head->second == withGlyph(U"  1", foldMarkerGlyph(FoldMarker::Collapsed)));
}

TEST_CASE("Gutter.terminal.relativeNumbersAreTheVisibleDistanceOnEveryDrawnRow", "[gutter]")
{
    // Scrolled into the history past a collapsed fold, each drawn row still reads its own visible distance
    // from the cursor.
    auto mc = MockTerm { PageSize { LineCount(6), ColumnCount(20) }, LineCount(30) };
    mc.terminal.settings().gutter = GutterSettings { .foldMarkers = false,
                                                     .exitStatus = false,
                                                     .userMarks = false,
                                                     .lineNumbers = LineNumberMode::Relative,
                                                     .lineNumberWidth = 3 };
    runCommand(mc, "a", { "1", "2", "3" });
    runCommand(mc, "b", { "4", "5", "6", "7" });
    runCommand(mc, "c", { "8", "9" });
    mc.terminal.foldState().collapse(mc.terminal.foldRanges()[1].headStableId);
    REQUIRE(mc.terminal.viewport().scrollUp(LineCount(3)));

    auto const rows = gutterRows(renderOf(mc.terminal), 3);
    auto const cursorLine = mc.terminal.primaryScreen().cursor().position.line;
    REQUIRE(rows.size() == 6);
    for (auto const& [screenRow, text]: rows)
    {
        auto const gridLine = mc.terminal.viewport().translateScreenToGridLine(LineOffset(screenRow));
        auto const distance = std::abs(mc.terminal.visibleDistance(cursorLine, gridLine));
        CAPTURE(screenRow, distance);
        CHECK(text == lineNumberText(static_cast<uint64_t>(distance), 3).view());
    }
}

TEST_CASE("Gutter.terminal.inViModeTheNumbersFollowTheViCursor", "[gutter]")
{
    auto mc = MockTerm { PageSize { LineCount(10), ColumnCount(20) } };
    mc.terminal.settings().gutter = GutterSettings { .foldMarkers = false,
                                                     .exitStatus = false,
                                                     .userMarks = false,
                                                     .lineNumbers = LineNumberMode::Hybrid,
                                                     .lineNumberWidth = 3 };
    mc.writeToScreen("a\r\nb\r\nc\r\nd");
    mc.terminal.inputHandler().setMode(ViMode::Normal);
    mc.terminal.moveNormalModeCursorTo(CellLocation { .line = LineOffset(1), .column = ColumnOffset(0) });

    // Hybrid: the Vi cursor's row shows its own number, every other row its distance from it.
    auto const rows = gutterRows(renderOf(mc.terminal), 3);
    CHECK(rows.at(0) == U"  1");
    CHECK(rows.at(1) == U"  2");
    CHECK(rows.at(3) == U"  2");
}

TEST_CASE("Gutter.terminal.aRowShowsWhenItWasFirstReached", "[gutter]")
{
    auto mc = MockTerm { PageSize { LineCount(5), ColumnCount(20) } };
    mc.terminal.settings().gutter =
        GutterSettings { .foldMarkers = false, .exitStatus = false, .userMarks = false, .timestamps = true };

    mc.wallClock.advance(10h);
    mc.writeToScreen("a\r\n"); // rows 0 and 1 are reached at 10:00:00 UTC
    mc.wallClock.advance(5s);
    mc.writeToScreen("b\r\nc"); // row 2 at 10:00:05

    // "%H:%M:%S" and the gap after it.
    auto const rows = gutterRows(renderOf(mc.terminal), 9);
    CHECK(rows.at(0) == U"10:00:00 ");
    CHECK_FALSE(rows.contains(1)); // the same second as the row above: not repeated
    CHECK(rows.at(2) == U"10:00:05 ");
    CHECK_FALSE(rows.contains(3)); // never reached
}

TEST_CASE("Gutter.terminal.theAlternateScreenHasAnEmptyGutter", "[gutter]")
{
    // vim, less and htop own the page: their rows have no history to number, no birth time worth showing
    // and no command blocks.
    auto mc = MockTerm { PageSize { LineCount(10), ColumnCount(20) } };
    mc.terminal.settings().gutter = GutterSettings { .foldMarkers = true,
                                                     .exitStatus = false,
                                                     .userMarks = false,
                                                     .lineNumbers = LineNumberMode::Absolute,
                                                     .timestamps = true };
    runCommand(mc, "ls", { "file1", "file2" });
    collapseOnlyFold(mc);

    auto const primary = renderOf(mc.terminal);
    REQUIRE_FALSE(primary.gutter.empty());
    REQUIRE(annotationsOf(primary).size() == 1);

    mc.writeToScreen("\033[?1049h");
    auto const alternate = renderOf(mc.terminal);
    CHECK(alternate.gutter.empty());
    CHECK(alternate.annotations.empty());

    // And leaving it gives everything back.
    mc.writeToScreen("\033[?1049l");
    auto const back = renderOf(mc.terminal);
    CHECK(back.gutter.size() == primary.gutter.size());
    CHECK(annotationsOf(back).size() == 1);
}

TEST_CASE("Gutter.terminal.withoutShellIntegrationTheBlockColumnStaysEmpty", "[gutter]")
{
    // A shell that emits no OSC 133 still gets numbers, which need no shell, and an empty block column
    // beside them.
    auto mc = MockTerm { PageSize { LineCount(10), ColumnCount(20) } };
    mc.terminal.settings().gutter = GutterSettings { .foldMarkers = true,
                                                     .exitStatus = false,
                                                     .userMarks = false,
                                                     .lineNumbers = LineNumberMode::Absolute,
                                                     .lineNumberWidth = 3 };
    mc.writeToScreen("just some output\r\nand more\r\n");

    auto const buffer = renderOf(mc.terminal);
    CHECK(mc.terminal.commandBlocks().size() == 0);
    CHECK(std::ranges::none_of(buffer.gutter,
                               [](RenderGutterCell const& cell) { return cell.column == ColumnOffset(-1); }));
    CHECK(gutterRows(buffer, 4).at(0) == U"  1 ");
    CHECK(buffer.annotations.empty());
}

TEST_CASE("Gutter.terminal.aCollapsedFoldIsLabelledWithWhatItHides", "[gutter]")
{
    auto mc = MockTerm { PageSize { LineCount(10), ColumnCount(30) } }; // room for the whole chip
    mc.terminal.settings().gutter =
        GutterSettings { .foldMarkers = true, .exitStatus = false, .userMarks = false };
    runCommand(mc, "ls", { "file1", "file2", "file3" });
    CHECK(renderOf(mc.terminal).annotations.empty()); // expanded: nothing hidden, nothing to say

    collapseOnlyFold(mc);

    auto const buffer = renderOf(mc.terminal);
    auto const annotations = annotationsOf(buffer);
    REQUIRE(annotations.size() == 1);
    auto const head = std::ranges::find(
        buffer.gutter, foldMarkerGlyph(FoldMarker::Collapsed), &RenderGutterCell::codepoint);
    REQUIRE(head != buffer.gutter.end());
    CHECK(annotations[0].position.line == head->lineOffset);
    CHECK(annotations[0].position.column == ColumnOffset(5)); // after "$ ls" and one blank
    CHECK(annotations[0].text == U"3 lines hidden ⌄");
    CHECK(annotations[0].attributes.foregroundColor == foldChipColors(mc.terminal.colorPalette()).text);
    // Over its band, the text colour at 8 %, across the label's columns.
    REQUIRE(buffer.chipBands.size() == 1);
    CHECK(buffer.chipBands.front().lineOffset == head->lineOffset);
    CHECK(buffer.chipBands.front().columns.fromColumn == ColumnOffset(5));
    // Never a cell: selection, copying and the accessibility bridge walk the cells and never meet it.
    CHECK(std::ranges::none_of(buffer.cells, [](RenderCell const& cell) { return cell.codepoints == U"⋯"; }));

    // With the fold column hidden, the label is the only sign left that output was folded away.
    mc.terminal.settings().gutter.foldMarkers = false;
    CHECK(annotationsOf(renderOf(mc.terminal)).size() == 1);
}

TEST_CASE("Gutter.terminal.aWrappedCommandLineIsLabelledWhereItEnds", "[gutter]")
{
    // A command line wider than the page wraps, and its head row is full to the edge: the label belongs
    // after the text where the prompt line ends, on its last row, as it would on a page wide enough.
    auto mc = MockTerm { PageSize { LineCount(10), ColumnCount(20) } };
    mc.terminal.settings().gutter =
        GutterSettings { .foldMarkers = true, .exitStatus = false, .userMarks = false };
    runCommand(mc, "echo 0123456789abcdefghij", { "x", "y" }); // 27 columns: rows 0 and 1; output on rows 2-3
    REQUIRE(mc.terminal.primaryScreen().grid().lineAt(LineOffset(1)).wrapped());

    collapseOnlyFold(mc);

    auto const buffer = renderOf(mc.terminal);
    auto const annotations = annotationsOf(buffer);
    REQUIRE(annotations.size() == 1);
    CHECK(annotations[0].position.line == LineOffset(1));
    CHECK(annotations[0].position.column == ColumnOffset(8)); // after "ghij" and one blank
    CHECK(annotations[0].text == U"2 lines hidd");            // cut at the page's edge
}

TEST_CASE("Gutter.terminal.aWrappedCommandLineIsLabelledOnce", "[gutter]")
{
    // Blanks at the edge are trimmed, so a head row that wrapped there has room after its text -- but the
    // prompt line goes on below it, and the label belongs only where the line ends.
    auto mc = MockTerm { PageSize { LineCount(10), ColumnCount(20) } };
    mc.terminal.settings().gutter =
        GutterSettings { .foldMarkers = true, .exitStatus = false, .userMarks = false };
    runCommand(mc, "echo 0123456789   x", { "y" }); // "$ echo 0123456789" and three blanks, then "x" on row 1
    REQUIRE(mc.terminal.primaryScreen().grid().lineAt(LineOffset(0)).trimmedColumns() == ColumnCount(17));
    REQUIRE(mc.terminal.primaryScreen().grid().lineAt(LineOffset(1)).wrapped());

    collapseOnlyFold(mc);

    auto const buffer = renderOf(mc.terminal);
    auto const annotations = annotationsOf(buffer);
    REQUIRE(annotations.size() == 1);
    CHECK(annotations[0].position.line == LineOffset(1));
    CHECK(annotations[0].position.column == ColumnOffset(2)); // after "x" and one blank
}

TEST_CASE("Gutter.terminal.aWrappedCommandLineCutByTheViewportIsLabelledOnItsLastDrawnRow", "[gutter]")
{
    // Scrolled so the head row of a wrapped command line is the last row on screen, its continuation is out
    // of sight: the label belongs on the last row of the line that IS drawn, or there is none at all.
    auto mc = MockTerm { PageSize { LineCount(5), ColumnCount(20) }, LineCount(20) };
    mc.terminal.settings().gutter =
        GutterSettings { .foldMarkers = true, .exitStatus = false, .userMarks = false };
    mc.writeToScreen("1\r\n2\r\n3\r\n4\r\n");       // rows for the viewport to show above the command
    runCommand(mc, "echo 0123456789   x", { "y" }); // "$ echo 0123456789" and three blanks, then "x"
    collapseOnlyFold(mc);
    mc.writeToScreen(std::string(10, '\n'));

    auto const& grid = mc.terminal.primaryScreen().grid();
    auto const head = grid.lineOffsetOf(mc.terminal.foldRanges()[0].headStableId);
    REQUIRE(head.has_value());
    auto const bottom = LineOffset(4);
    for ([[maybe_unused]] auto const _: std::views::iota(0, 20))
    {
        if (mc.terminal.viewport().translateScreenToGridLine(bottom) <= *head)
            break;
        REQUIRE(mc.terminal.viewport().scrollUp(LineCount(1)));
    }
    REQUIRE(mc.terminal.viewport().translateScreenToGridLine(bottom) == *head);

    auto const buffer = renderOf(mc.terminal);
    auto const annotations = annotationsOf(buffer);
    REQUIRE(annotations.size() == 1);
    CHECK(annotations[0].position.line == bottom);
    CHECK(annotations[0].position.column == ColumnOffset(18)); // after "0123456789" and one blank
    CHECK(annotations[0].text == U"1 ");                       // cut at the page's edge
}

TEST_CASE("Gutter.terminal.withExitStatusOffAFailedBlockIsDrawnAsAFold", "[gutter]")
{
    // The block column shows an outcome only when asked to: with exit status off, a failed block's fold
    // looks like every other fold.
    auto mc = MockTerm { PageSize { LineCount(10), ColumnCount(20) } };
    mc.terminal.settings().gutter =
        GutterSettings { .foldMarkers = true, .exitStatus = false, .userMarks = false };
    runCommand(mc, "false", { "bad", "worse" }, 1);

    auto const buffer = renderOf(mc.terminal);
    REQUIRE(buffer.gutter.size() == 3);
    for (auto const& cell: buffer.gutter)
    {
        CAPTURE(cell.lineOffset);
        CHECK(cell.attributes.foregroundColor == mc.terminal.colorPalette().foldMarkerColors().foreground);
    }
}

TEST_CASE("Gutter.terminal.rowsBelowTheProjectionStayEmpty", "[gutter]")
{
    // Collapsed folds that hide more rows than the history can backfill leave the page's last rows blank,
    // and a blank row has no line to number: translated anyway, each would repeat the last visible row's.
    auto mc = MockTerm { PageSize { LineCount(10), ColumnCount(20) } };
    mc.terminal.settings().gutter = GutterSettings { .foldMarkers = true,
                                                     .exitStatus = false,
                                                     .userMarks = false,
                                                     .lineNumbers = LineNumberMode::Absolute,
                                                     .lineNumberWidth = 3 };
    runCommand(mc, "a", { "1", "2", "3", "4", "5" });

    collapseOnlyFold(mc);
    auto const projectedRows = static_cast<int>(mc.terminal.foldProjection().size());
    REQUIRE(projectedRows < unbox<int>(mc.terminal.pageSize().lines));

    auto const buffer = renderOf(mc.terminal);
    auto const rows = gutterRows(buffer, 4);
    CHECK(rows.size() == static_cast<size_t>(projectedRows));
    CHECK(rows.lower_bound(projectedRows) == rows.end());

    // And no row carries two block-column glyphs: one extrapolated onto another's line would.
    auto blockColumnRows = std::set<int> {};
    for (auto const& cell: buffer.gutter)
        if (cell.column == ColumnOffset(-1))
            CHECK(blockColumnRows.insert(unbox<int>(cell.lineOffset)).second);
}

TEST_CASE("Gutter.terminal.aPressDoesWhatTheRowShows", "[gutter]")
{
    // The hover's hand, the press and the tooltip's hint ask this one question, so it must say "toggle"
    // only where a fold control is drawn and may toggle, and "menu" only beside a status mark.
    auto mc = MockTerm { PageSize { LineCount(10), ColumnCount(20) } };
    mc.terminal.settings().gutter = GutterSettings { .foldMarkers = true, .exitStatus = true };
    runCommand(mc, "true", {}, 0);      // row 0: a disc, nothing to fold
    runCommand(mc, "make", { "a" }, 2); // rows 1-2: a fold
    mc.writeToScreen(PromptStart);      // row 3: the next prompt

    SECTION("on a fold, its head and its body alike, the fold")
    {
        CHECK(mc.terminal.blockColumnPressAt(LineOffset(1)) == BlockColumnPress::ToggleFold);
        CHECK(mc.terminal.blockColumnPressAt(LineOffset(2)) == BlockColumnPress::ToggleFold);
    }

    SECTION("beside a status mark with nothing to fold, its block's menu")
    {
        CHECK(mc.terminal.blockColumnPressAt(LineOffset(0)) == BlockColumnPress::OpenBlockMenu);
    }

    SECTION("below every block, nothing")
    {
        CHECK(mc.terminal.blockColumnPressAt(LineOffset(8)) == BlockColumnPress::Swallow);
    }

    SECTION("with the fold controls hidden, a fold row is nothing; a status mark still opens its menu")
    {
        mc.terminal.settings().gutter.foldMarkers = false;
        CHECK(mc.terminal.blockColumnPressAt(LineOffset(2)) == BlockColumnPress::Swallow);
        // The failed head now shows its cross, not a fold control.
        CHECK(mc.terminal.blockColumnPressAt(LineOffset(1)) == BlockColumnPress::OpenBlockMenu);
    }

    SECTION("with folding off, the drawn control does nothing")
    {
        mc.terminal.settings().foldingAvailability = FoldingAvailability::Disabled;
        CHECK(mc.terminal.blockColumnPressAt(LineOffset(1)) == BlockColumnPress::Swallow);
        CHECK(mc.terminal.blockColumnPressAt(LineOffset(2)) == BlockColumnPress::Swallow);
    }
}

TEST_CASE("Gutter.terminal.aCollapsedFoldsLabelNamesItsFold", "[gutter]")
{
    // What a click on "N lines hidden ⌄" expands: the label is found where it is drawn.
    auto mc = MockTerm { PageSize { LineCount(10), ColumnCount(30) } };
    mc.terminal.settings().gutter = GutterSettings { .foldMarkers = true };
    runCommand(mc, "make", { "a", "b" }); // "$ make" on row 0, its label from column 7
    CHECK_FALSE(mc.terminal.collapsedFoldLabelAt(
        CellLocation { .line = LineOffset(0), .column = ColumnOffset(8) }, ColumnCount(1)));

    REQUIRE(mc.terminal.toggleFoldContaining(LineOffset(0)));
    auto const at = [&](int column) {
        return mc.terminal.collapsedFoldLabelAt(
            CellLocation { .line = LineOffset(0), .column = ColumnOffset(column) }, ColumnCount(1));
    };
    CHECK(at(7) == LineOffset(0));
    CHECK(at(12) == LineOffset(0));
    CHECK_FALSE(at(3).has_value()); // the command's own text
    CHECK_FALSE(at(6).has_value()); // the blank column before the label
    CHECK_FALSE(mc.terminal.collapsedFoldLabelAt(
        CellLocation { .line = LineOffset(1), .column = ColumnOffset(8) }, ColumnCount(1)));
}

TEST_CASE("Gutter.terminal.aRunningCommandIsNoPrompt", "[gutter]")
{
    auto mc = MockTerm { PageSize { LineCount(10), ColumnCount(20) } };
    CHECK_FALSE(mc.terminal.shellIsPrompting()); // no shell integration, no prompt reported
    startCommand(mc, "sleep 9");
    CHECK_FALSE(mc.terminal.shellIsPrompting());
    mc.writeToScreen(commandEnd(0));
    mc.writeToScreen(PromptStart);
    CHECK(mc.terminal.shellIsPrompting());
}

TEST_CASE("Gutter.terminal.theIndicatorStatusLineSpansTheGutterToo", "[gutter][statusline]")
{
    // #2139: the indicator status line is the terminal's own bar, so the frame names the color it reaches
    // the window's edges in -- over the gutter beside it and the page margins -- and its text is laid out
    // across the gutter's columns and the page's, from the gutter's left edge. The host-writable one is the
    // application's, gets no wider than the main page, and names none.
    auto mc = MockTerm { PageSize { LineCount(5), ColumnCount(40) } };
    mc.terminal.settings().gutter = GutterSettings { .foldMarkers = true, .exitStatus = true };
    auto& palette = mc.terminal.colorPalette();
    palette.indicatorStatusLineInactive.background = 0x111111_rgb;
    palette.indicatorStatusLineInsertMode.background = 0x222222_rgb;

    SECTION("no status line, no bar")
    {
        CHECK_FALSE(renderOf(mc.terminal).statusBar.has_value());
    }

    SECTION("the indicator's bar, on its row below the main page")
    {
        mc.terminal.setStatusDisplay(StatusDisplayType::Indicator);
        auto const buffer = renderOf(mc.terminal);
        REQUIRE(buffer.statusBar.has_value());
        CHECK(buffer.statusBar->lineOffset == LineOffset(4));
        CHECK(buffer.statusBar->lines == LineCount(1));
        // The gutter itself stays beside the main page.
        CHECK(std::ranges::none_of(buffer.gutter,
                                   [](auto const& cell) { return cell.lineOffset >= LineOffset(4); }));
    }

    SECTION("the indicator's bar, on its row above the main page")
    {
        mc.terminal.settings().statusDisplayPosition = StatusDisplayPosition::Top;
        mc.terminal.setStatusDisplay(StatusDisplayType::Indicator);
        auto const buffer = renderOf(mc.terminal);
        REQUIRE(buffer.statusBar.has_value());
        CHECK(buffer.statusBar->lineOffset == LineOffset(0));
    }

    SECTION("its text laid out across the gutter's columns too")
    {
        mc.terminal.setStatusDisplay(StatusDisplayType::Indicator);
        auto const lastTextColumn = [&](RenderBuffer const& buffer) {
            auto last = -1;
            for (auto const& cell: buffer.cells)
                if (cell.position.line == LineOffset(4) && !cell.codepoints.empty()
                    && cell.codepoints != U" ")
                    last = std::max(last, unbox<int>(cell.position.column));
            return last;
        };

        mc.terminal.settings().gutter = GutterSettings {};
        auto const bare = renderOf(mc.terminal);
        REQUIRE(bare.statusBar.has_value());
        CHECK(bare.statusBar->leadingColumns == ColumnCount(0));
        CHECK(mc.terminal.indicatorStatusLinePageSize().columns == ColumnCount(40));

        mc.terminal.settings().gutter = GutterSettings { .foldMarkers = true, .blockColumnWidth = 2 };
        auto const wide = renderOf(mc.terminal);
        REQUIRE(wide.statusBar.has_value());
        CHECK(wide.statusBar->leadingColumns == ColumnCount(2));
        CHECK(mc.terminal.indicatorStatusLinePageSize().columns == ColumnCount(42));
        // The right-hand segment ends at the bar's right edge, two columns further than the page's alone.
        CHECK(lastTextColumn(wide) == lastTextColumn(bare) + 2);
    }

    SECTION("in the colors the indicator wears for the focus")
    {
        mc.terminal.setStatusDisplay(StatusDisplayType::Indicator);
        (void) mc.terminal.sendFocusOutEvent();
        CHECK(renderOf(mc.terminal).statusBar->background == 0x111111_rgb);
        (void) mc.terminal.sendFocusInEvent();
        CHECK(renderOf(mc.terminal).statusBar->background == 0x222222_rgb);
    }

    SECTION("the host-writable status line leaves the strip to the default background")
    {
        mc.terminal.setStatusDisplay(StatusDisplayType::HostWritable);
        auto const buffer = renderOf(mc.terminal);
        CHECK_FALSE(buffer.statusBar.has_value());
        CHECK(std::ranges::none_of(buffer.gutter,
                                   [](auto const& cell) { return cell.lineOffset >= LineOffset(4); }));
    }
}

namespace
{
/// The gutter settings of a native chrome row with exit status on: the block column is the rail's strip.
constexpr auto RailGutter = GutterSettings { .exitStatus = true, .blockColumnForm = BlockColumnForm::Rail };

/// The rail cells of @p buffer, by screen row: the cells that draw a segment or a mark.
[[nodiscard]] std::map<int, RenderGutterCell> railCellsOf(RenderBuffer const& buffer)
{
    auto cells = std::map<int, RenderGutterCell> {};
    for (auto const& cell: buffer.gutter)
        if (cell.rail != RailSegment::None || cell.mark != RailMark::None)
        {
            CHECK(cell.codepoint == 0);
            CHECK(cell.column == ColumnOffset(0));
            CHECK(cells.emplace(unbox<int>(cell.lineOffset), cell).second); // one per row
        }
    return cells;
}

/// The segment drawn beside screen row @p row of @p cells; None where the row has no rail cell.
[[nodiscard]] RailSegment segmentAt(std::map<int, RenderGutterCell> const& cells, int row)
{
    auto const found = cells.find(row);
    return found == cells.end() ? RailSegment::None : found->second.rail;
}

/// The mark drawn beside screen row @p row of @p cells; None where the row has no rail cell.
[[nodiscard]] RailMark markAt(std::map<int, RenderGutterCell> const& cells, int row)
{
    auto const found = cells.find(row);
    return found == cells.end() ? RailMark::None : found->second.mark;
}

/// `ls` printing two rows and succeeding, `false` printing one and failing, then a live prompt: the
/// blocks span rows 0-2 and 3-4, the prompt row 5.
/// @param gutter The gutter to draw with.
void runASuccessAndAFailure(MockTerm<>& mc, GutterSettings const& gutter)
{
    mc.terminal.settings().gutter = gutter;
    runCommand(mc, "ls", { "file1", "file2" }, 0);
    runCommand(mc, "false", { "bad" }, 1);
    mc.writeToScreen(PromptStart);
}
} // namespace

TEST_CASE("Gutter.terminal.aFailedBlockDrawsAHeadAndATail", "[gutter]")
{
    auto mc = MockTerm { PageSize { LineCount(10), ColumnCount(20) } };
    runASuccessAndAFailure(mc, RailGutter);

    auto const cells = railCellsOf(renderOf(mc.terminal));
    auto const segments = std::vector { RailSegment::Head, RailSegment::Body, RailSegment::Tail,
                                        RailSegment::Head, RailSegment::Tail, RailSegment::None };
    auto const marks = std::vector { RailMark::None,  RailMark::None, RailMark::None,
                                     RailMark::Cross, RailMark::None, RailMark::None };
    for (auto const row: std::views::iota(0, static_cast<int>(segments.size())))
    {
        CAPTURE(row);
        CHECK(segmentAt(cells, row) == segments[static_cast<size_t>(row)]);
        CHECK(markAt(cells, row) == marks[static_cast<size_t>(row)]);
    }
    CHECK(cells.size() == 5); // the live prompt and the rows below draw none
    CHECK(cells.at(0).outcome == CommandBlockOutcome::Success);
    CHECK(cells.at(3).outcome == CommandBlockOutcome::Failure);
    CHECK(cells.at(4).railColor == mc.terminal.colorPalette().blockStatusColor(CommandBlockOutcome::Failure));
}

TEST_CASE("Gutter.terminal.hoveringASuccessShowsItsCheck", "[gutter]")
{
    auto mc = MockTerm { PageSize { LineCount(10), ColumnCount(20) } };
    runASuccessAndAFailure(mc, RailGutter);
    auto const* first = mc.terminal.commandBlockAt(LineOffset(0));
    REQUIRE(first != nullptr);
    auto const& palette = mc.terminal.colorPalette();

    mc.terminal.setHoveredCommandBlock(first->id);
    auto const hovered = railCellsOf(renderOf(mc.terminal));
    CHECK(markAt(hovered, 0) == RailMark::Check);
    CHECK(hovered.at(0).railColor == palette.blockStatusHoverColor(CommandBlockOutcome::Success));
    CHECK(hovered.at(2).railColor == palette.blockStatusHoverColor(CommandBlockOutcome::Success));
    // The other block keeps its rest colour.
    CHECK(hovered.at(3).railColor == palette.blockStatusColor(CommandBlockOutcome::Failure));

    mc.terminal.setHoveredCommandBlock(std::nullopt);
    auto const atRest = railCellsOf(renderOf(mc.terminal));
    CHECK(markAt(atRest, 0) == RailMark::None);
    CHECK(atRest.at(0).railColor == palette.blockStatusColor(CommandBlockOutcome::Success));
}

TEST_CASE("Gutter.terminal.aRunningBlockShowsItsTriangle", "[gutter]")
{
    auto mc = MockTerm { PageSize { LineCount(10), ColumnCount(20) } };
    mc.terminal.settings().gutter = RailGutter;
    startCommand(mc, "sleep 9");
    mc.writeToScreen("still going\r\n");

    auto const cells = railCellsOf(renderOf(mc.terminal));
    REQUIRE_FALSE(cells.empty());
    CHECK(segmentAt(cells, 0) == RailSegment::Head);
    CHECK(markAt(cells, 0) == RailMark::Running);
    auto const running = mc.terminal.colorPalette().blockStatusColor(CommandBlockOutcome::Running);
    for (auto const& [row, cell]: cells)
    {
        CAPTURE(row);
        CHECK(cell.outcome == CommandBlockOutcome::Running);
        CHECK(cell.railColor == running);
    }
}

namespace
{
/// A terminal of 10 rows and 80 columns keeping 120 000 rows of history, the rail on, in which `seq` printed
/// @p lines lines and finished, with a live prompt below.
[[nodiscard]] std::unique_ptr<MockTerm<>> tallBlock(int lines)
{
    auto mc = std::make_unique<MockTerm<>>(
        PageSize { LineCount(10), ColumnCount(80) },
        HistoryLimits { .guaranteed = LineCount(120'000), .capacity = LineCount(120'000) });
    mc->terminal.settings().gutter = RailGutter;
    startCommand(*mc, "seq");
    printLines(*mc, "line", lines);
    mc->writeToScreen(commandEnd(0));
    mc->writeToScreen(PromptStart);
    return mc;
}
} // namespace

TEST_CASE("Gutter.terminal.aHundredThousandLineBlockDrawsItsRail", "[gutter]")
{
    // Review Focus 2: a block far taller than the page, scrolled. A frame looks at the drawn rows and one
    // beyond each edge, never at the block's rows.
    auto const mc = tallBlock(100'000);
    auto& terminal = mc->terminal;

    // Beyond the fold scan's reach (ruling C1): it does not fold, and the rail is drawn all the same.
    CHECK_FALSE(terminal.collapseLastFold());

    SECTION("scrolled into its middle, every drawn row is its body")
    {
        REQUIRE(terminal.viewport().scrollToTop());
        REQUIRE(terminal.viewport().scrollDown(LineCount(50'000)));
        auto const cells = railCellsOf(renderOf(terminal));
        REQUIRE(static_cast<int>(cells.size()) == unbox<int>(terminal.pageSize().lines));
        for (auto const& [row, cell]: cells)
        {
            CAPTURE(row);
            CHECK(cell.rail == RailSegment::Body);
            CHECK(cell.mark == RailMark::None);
        }
    }

    SECTION("scrolled to its top, its first row is its head and the row above the page is nobody's")
    {
        REQUIRE(terminal.viewport().scrollToTop());
        auto const cells = railCellsOf(renderOf(terminal));
        CHECK(segmentAt(cells, 0) == RailSegment::Head);
        CHECK(segmentAt(cells, 1) == RailSegment::Body);
    }

    SECTION("at its last row, that row is its tail")
    {
        auto const cells = railCellsOf(renderOf(terminal));
        auto const promptRow = unbox<int>(terminal.primaryScreen().cursor().position.line);
        CHECK(segmentAt(cells, promptRow - 2) == RailSegment::Body);
        CHECK(segmentAt(cells, promptRow - 1) == RailSegment::Tail);
        CHECK(segmentAt(cells, promptRow) == RailSegment::None);
    }
}

TEST_CASE("Gutter.terminal.aCollapsedBlockIsASingleRow", "[gutter]")
{
    // Collapsed, a block's projection is its head alone. A fold is found within MaxFoldScanLines of the page
    // (the scan's bound, Folding.hpp): the largest block it finds prints MaxFoldScanLines - 2 lines, its
    // command row and the live prompt below it taking the scan's other two (ruling C1).
    auto const largest = static_cast<int>(MaxFoldScanLines) - 2;
    auto const mc = tallBlock(largest);
    REQUIRE(mc->terminal.collapseLastFold());
    auto const cells = railCellsOf(renderOf(mc->terminal));
    REQUIRE(cells.size() == 1);
    CHECK(cells.begin()->second.rail == RailSegment::Single);

    // One line more, and the scan no longer reaches its head.
    CHECK_FALSE(tallBlock(largest + 1)->terminal.collapseLastFold());
}

TEST_CASE("Gutter.terminal.aBlockEndingOnThePagesLastRowEndsThere", "[gutter]")
{
    // With a fold collapsed on the page the rows are the projection's, and the row below the last one is
    // found by walking visible lines, which stops at the page's last line: a block reaching it still ends on
    // it rather than running on into itself.
    auto mc = MockTerm { PageSize { LineCount(10), ColumnCount(20) } };
    mc.terminal.settings().gutter = RailGutter;
    runCommand(mc, "ls", { "a", "b", "c" }); // rows 0-3, collapsed to row 0
    REQUIRE(mc.terminal.collapseLastFold());
    startCommand(mc, "make"); // its head on row 4, its output on rows 5-8, the cursor on row 9
    printLines(mc, "x", 4);
    REQUIRE(mc.terminal.primaryScreen().cursor().position.line == LineOffset(9));

    auto const cells = railCellsOf(renderOf(mc.terminal));
    REQUIRE_FALSE(cells.empty());
    auto const lastRow = cells.rbegin()->first;
    REQUIRE(lastRow == 6); // the ls head, then make's six rows
    CHECK(segmentAt(cells, 0) == RailSegment::Single);
    CHECK(segmentAt(cells, 1) == RailSegment::Head);
    CHECK(segmentAt(cells, lastRow - 1) == RailSegment::Body);
    CHECK(segmentAt(cells, lastRow) == RailSegment::Tail);
}

TEST_CASE("Gutter.terminal.aBlockStartingOnTheHistorysFirstRowStartsThere", "[gutter]")
{
    // The row above the page is found by walking visible lines up, which stops at the history's first line:
    // a block starting on it, scrolled to the top under a collapsed fold, still starts there.
    auto mc = MockTerm { PageSize { LineCount(10), ColumnCount(20) },
                         HistoryLimits { .guaranteed = LineCount(100), .capacity = LineCount(100) } };
    mc.terminal.settings().gutter = RailGutter;
    startCommand(mc, "seq");
    printLines(mc, "x", 20);
    mc.writeToScreen(commandEnd(0));
    runCommand(mc, "ls", { "a", "b", "c" });
    mc.writeToScreen(PromptStart);
    REQUIRE(mc.terminal.collapseLastFold());
    REQUIRE(mc.terminal.viewport().scrollToTop());

    auto const cells = railCellsOf(renderOf(mc.terminal));
    CHECK(segmentAt(cells, 0) == RailSegment::Head);
    CHECK(segmentAt(cells, 1) == RailSegment::Body);
}

TEST_CASE("Gutter.terminal.theGlyphFormDrawsTodaysCells", "[gutter]")
{
    // The Terminal row keeps the glyph column: the same session draws today's cells, and no rail.
    auto mc = MockTerm { PageSize { LineCount(10), ColumnCount(20) } };
    runASuccessAndAFailure(mc,
                           GutterSettings { .exitStatus = true, .blockColumnForm = BlockColumnForm::Glyphs });

    auto const buffer = renderOf(mc.terminal);
    CHECK(railCellsOf(buffer).empty());
    auto const rows = gutterRows(buffer, 1);
    CHECK(rows
          == std::map<int, std::u32string> {
              { 0, withGlyph(U"", blockColumnCodepoint(BlockColumnGlyph::Finished, FoldWeight::Light)) },
              { 3, withGlyph(U"", blockColumnCodepoint(BlockColumnGlyph::Failed, FoldWeight::Light)) },
          });
}

TEST_CASE("Gutter.terminal.aUserMarkKeepsItsOwnInk", "[gutter]")
{
    // The ◆ is the user's, as in the glyph column: the full-strength fold colour, whether the row lies in no
    // block or in a block's body -- never black, and never the bar's colour.
    auto mc = MockTerm { PageSize { LineCount(10), ColumnCount(20) } };
    mc.terminal.settings().gutter =
        GutterSettings { .exitStatus = true, .userMarks = true, .blockColumnForm = BlockColumnForm::Rail };
    mc.writeToScreen("loose\r\n");                 // row 0: no block
    runCommand(mc, "false", { "a", "b", "c" }, 1); // rows 1-4
    mc.writeToScreen(PromptStart);
    mc.terminal.primaryScreen().setLogicalLineFlags(LineOffset(0), LineFlag::UserMark, true);
    mc.terminal.primaryScreen().setLogicalLineFlags(LineOffset(3), LineFlag::UserMark, true);

    auto const cells = railCellsOf(renderOf(mc.terminal));
    auto const userMarkInk = mc.terminal.colorPalette().foldMarkerHoverColors().foreground;
    for (auto const row: { 0, 3 })
    {
        CAPTURE(row);
        REQUIRE(cells.contains(row));
        CHECK(cells.at(row).mark == RailMark::UserMark);
        CHECK(cells.at(row).attributes.foregroundColor == userMarkInk);
    }
    CHECK(cells.at(0).rail == RailSegment::None);
    CHECK(cells.at(3).rail == RailSegment::Body);
    CHECK(cells.at(3).attributes.foregroundColor != cells.at(3).railColor);
    // A status mark is drawn in the rail's own ink.
    CHECK(cells.at(1).mark == RailMark::Cross);
    CHECK(cells.at(1).attributes.foregroundColor == cells.at(1).railColor);
}

namespace
{
/// A MockTerm that counts the redraws it is asked for.
struct RedrawCountingTerm final: MockTerm<>
{
    using MockTerm<>::MockTerm;

    int redraws = 0; ///< How often the terminal asked for a frame to be drawn.

    void renderBufferUpdated() override { ++redraws; }
};
} // namespace

TEST_CASE("Gutter.terminal.theRailRedrawsWhenItsHoverChanges", "[gutter]")
{
    auto mc = RedrawCountingTerm { PageSize { LineCount(10), ColumnCount(20) } };
    mc.terminal.settings().gutter = RailGutter;
    runCommand(mc, "true", {}, 0);       // row 0, a block with no output: nothing to fold
    runCommand(mc, "false", { "x" }, 1); // rows 1-2
    mc.writeToScreen(PromptStart);
    auto const* first = mc.terminal.commandBlockAt(LineOffset(0));
    REQUIRE(first != nullptr);

    SECTION("setHoveredCommandBlock redraws on a change, and only then")
    {
        auto const before = mc.redraws;
        mc.terminal.setHoveredCommandBlock(first->id);
        CHECK(mc.redraws == before + 1);
        mc.terminal.setHoveredCommandBlock(first->id);
        CHECK(mc.redraws == before + 1);
        mc.terminal.setHoveredCommandBlock(std::nullopt);
        CHECK(mc.redraws == before + 2);
    }

    SECTION("the pointer on the strip beside a block with nothing to fold lights it, and redraws")
    {
        auto const before = mc.redraws;
        mc.terminal.setGutterHoverLine(LineOffset(0));
        CHECK(mc.redraws == before + 1);
        auto const cells = railCellsOf(renderOf(mc.terminal));
        CHECK(cells.at(0).mark == RailMark::Check); // a success shows its check while hovered
        CHECK(
            cells.at(0).railColor
            == railColor(mc.terminal.colorPalette(), CommandBlockOutcome::Success, BlockAttention::Hovered));

        // To the other block: the fold changes nothing here, the block does.
        mc.terminal.setGutterHoverLine(LineOffset(1));
        CHECK(mc.redraws == before + 2);
        CHECK(markAt(railCellsOf(renderOf(mc.terminal)), 0) == RailMark::None);
    }

    SECTION("under the glyph form the block alone redraws nothing")
    {
        mc.terminal.settings().gutter.blockColumnForm = BlockColumnForm::Glyphs;
        auto const before = mc.redraws;
        mc.terminal.setGutterHoverLine(LineOffset(0)); // no fold on row 0
        CHECK(mc.redraws == before);
    }
}

namespace
{
/// `make` typed at a `~ ❯ ` prompt with an RPROMPT of @p rprompt drawn from column @p rpromptColumn
/// (1-based), printing one line and ending with @p exitCode, then a live prompt. The command row is screen
/// row 0.
[[nodiscard]] std::string commandUnderAnRprompt(std::string_view rprompt, int rpromptColumn, int exitCode)
{
    return std::format(
        "\033]133;A\033\\~ ❯ \033]133;B\033\\make\033[1;{}H{}\033[1;9H\r\n\033]133;C\033\\boom\r\n{}{}",
        rpromptColumn,
        rprompt,
        commandEnd(exitCode),
        PromptStart);
}

/// The text the frame's annotations spell on screen row @p row, and the columns they take.
struct RowAnnotation
{
    std::u32string text;               ///< What the cells spell.
    std::vector<ColumnOffset> columns; ///< Each cell's column.
};

[[nodiscard]] RowAnnotation annotationOnRow(RenderBuffer const& buffer, int row)
{
    auto annotation = RowAnnotation {};
    for (auto const& cell: buffer.annotations)
        if (cell.position.line == LineOffset(row))
        {
            annotation.text += cell.codepoints;
            annotation.columns.push_back(cell.position.column);
        }
    return annotation;
}

/// The pill the frame draws on screen row @p row, if one.
[[nodiscard]] std::optional<RenderChip> chipOnRow(RenderBuffer const& buffer, int row)
{
    auto const chip = std::ranges::find(buffer.chips, LineOffset(row), &RenderChip::lineOffset);
    return chip != buffer.chips.end() ? std::optional { *chip } : std::nullopt;
}

/// Measured faces whose every codepoint advances @p advance pixels in @p cellWidth-pixel cells, each pill
/// @p padding pixels wider than its label.
[[nodiscard]] PillLabelMetrics uniformPillMetrics(int advance, int cellWidth, int padding)
{
    auto metrics = PillLabelMetrics { .cellWidth = cellWidth };
    for (auto& face: metrics.styles)
    {
        face.advances.fill(advance);
        face.widest = advance;
        face.padding = padding;
    }
    return metrics;
}
} // namespace

TEST_CASE("Gutter.terminal.aChipNeverCoversAnRprompt", "[gutter][blockchip]")
{
    // Review Focus 1: the chip lands in the blanks between the command and the RPROMPT, two clear of it.
    auto mc = MockTerm { PageSize { LineCount(10), ColumnCount(80) } };
    mc.terminal.settings().gutter = RailGutter;
    constexpr auto RpromptColumn = 69; // "[main] 14:02" in columns 68..79
    mc.writeToScreen(commandUnderAnRprompt("[main] 14:02", RpromptColumn, 2));

    auto const buffer = renderOf(mc.terminal);
    auto const chip = chipOnRow(buffer, 0);
    REQUIRE(chip.has_value());
    CHECK(chip->style == PillStyle::Failed);
    CHECK(unicode::convert_to<char>(std::u32string_view { chip->text }).starts_with("Failed · exit 2 · "));
    auto const& line = mc.terminal.primaryScreen().grid().lineAt(LineOffset(0));
    for (auto const column:
         std::views::iota(unbox<int>(chip->columns.fromColumn), unbox<int>(chip->columns.toColumn) + 1))
    {
        CAPTURE(column);
        CHECK(line.cellEmptyAt(ColumnOffset(column)));
    }
    CHECK(chip->columns.toColumn + ColumnOffset(2) < ColumnOffset(RpromptColumn - 1));
    // A pill: no cell is laid out for it, and no band is drawn under cells.
    CHECK(buffer.annotations.empty());
    CHECK(buffer.chipBands.empty());
}

TEST_CASE("Gutter.terminal.aCollapsedChipLeavesTheMark", "[gutter][blockchip]")
{
    // The row filled to the edge: no run holds the chip, and the rail's ✗ is the one sign.
    auto mc = MockTerm { PageSize { LineCount(10), ColumnCount(80) } };
    mc.terminal.settings().gutter = RailGutter;
    mc.writeToScreen(commandUnderAnRprompt(std::string(70, '='), 10, 2));

    auto const buffer = renderOf(mc.terminal);
    CHECK_FALSE(chipOnRow(buffer, 0).has_value());
    CHECK(annotationOnRow(buffer, 0).columns.empty());
    CHECK(markAt(railCellsOf(buffer), 0) == RailMark::Cross);
}

TEST_CASE("Gutter.terminal.aRunningChipTicks", "[gutter][blockchip]")
{
    auto mc = MockTerm { PageSize { LineCount(10), ColumnCount(80) } };
    mc.terminal.settings().gutter = RailGutter;
    startCommand(mc, "sleep 9");
    mc.wallClock.advance(std::chrono::seconds(3));

    auto const running = renderOf(mc.terminal);
    REQUIRE(chipOnRow(running, 0).has_value());
    CHECK(chipOnRow(running, 0)->text == U"Running · 3s");
    CHECK(chipOnRow(running, 0)->style == PillStyle::Running);
    CHECK(mc.terminal.hasVisibleRunningStatus());
    CHECK(running.chips.size() == 1);

    mc.writeToScreen(commandEnd(0));
    mc.writeToScreen(PromptStart);
    (void) renderOf(mc.terminal);
    CHECK_FALSE(mc.terminal.hasVisibleRunningStatus());
}

TEST_CASE("Gutter.terminal.theTerminalRowKeepsItsCellChips", "[gutter][blockchip]")
{
    // The Terminal row draws its chips in terminal text over a band: a TUI draws nothing in a second font.
    auto mc = MockTerm { PageSize { LineCount(10), ColumnCount(80) } };
    mc.terminal.settings().gutter = GutterSettings { .exitStatus = true };
    mc.writeToScreen(commandUnderAnRprompt("[main] 14:02", 69, 2));
    // Measured faces change nothing there.
    mc.terminal.setPillLabelMetrics(uniformPillMetrics(3, 10, 0));

    auto const buffer = renderOf(mc.terminal);
    CHECK(buffer.chips.empty());
    auto const chip = annotationOnRow(buffer, 0);
    CHECK(unicode::convert_to<char>(std::u32string_view { chip.text }).starts_with("Failed · exit 2 · "));
    CHECK(chip.columns.size() == chip.text.size());
    CHECK(buffer.chipBands.size() == 1);
}

TEST_CASE("Gutter.terminal.aPillCoversItsMeasuredColumns", "[gutter][blockchip]")
{
    // The pill is placed over the whole columns its measured label and padding take, rounded up -- so it
    // never covers a non-blank cell however its proportional label falls -- and it keeps the right edge.
    auto mc = MockTerm { PageSize { LineCount(10), ColumnCount(80) } };
    mc.terminal.settings().gutter = RailGutter;
    mc.writeToScreen(commandUnderAnRprompt("[main] 14:02", 69, 2));
    auto const unmeasured = chipOnRow(renderOf(mc.terminal), 0);
    REQUIRE(unmeasured.has_value());
    CHECK(unmeasured->columns.length() == ColumnCount::cast_from(unmeasured->text.size()));

    // 5-px advances in 10-px cells with 16 px of padding: 5n + 16 pixels.
    mc.terminal.setPillLabelMetrics(uniformPillMetrics(5, 10, 16));
    auto const measured = chipOnRow(renderOf(mc.terminal), 0);
    REQUIRE(measured.has_value());
    auto const pixels = (5 * static_cast<int>(measured->text.size())) + 16;
    CHECK(measured->columns.length() == ColumnCount((pixels + 9) / 10));
    CHECK(measured->columns.toColumn == unmeasured->columns.toColumn);
}

TEST_CASE("Gutter.terminal.theLargestFoldIsOneChip", "[gutter]")
{
    // Review Focus 2, under ruling C1: the largest block the fold scan finds, collapsed, is one chip on its
    // head row over blank cells, its count grouped; a far longer block does not fold, and draws no chip.
    auto const largest = static_cast<int>(MaxFoldScanLines) - 2;
    auto const mc = tallBlock(largest);
    REQUIRE(mc->terminal.collapseLastFold());

    auto const buffer = renderOf(mc->terminal);
    // The fold chip after the command, and the block's status chip right of it on the same row: apart.
    REQUIRE(buffer.chips.size() == 2);
    auto const& fold = buffer.chips[0];
    auto const& status = buffer.chips[1];
    CHECK(fold.style == PillStyle::Fold);
    CHECK(fold.text == U"4,094 lines hidden ⌄");
    CHECK(status.lineOffset == fold.lineOffset);
    CHECK(status.columns.fromColumn
          >= fold.columns.toColumn + ColumnOffset(1) + boxed_cast<ColumnOffset>(BlockChipGap));
    auto const& line = mc->terminal.primaryScreen().grid().lineAt(
        mc->terminal.viewport().translateScreenToGridLine(fold.lineOffset));
    for (auto const column:
         std::views::iota(unbox<int>(fold.columns.fromColumn), unbox<int>(fold.columns.toColumn) + 1))
        CHECK(line.cellEmptyAt(ColumnOffset(column)));

    // Scrolled to its head, so the row a fold chip would sit on is drawn: its status chip is there -- the
    // frame does place chips on that row -- and no fold chip beside it, nor a click that would open one.
    auto const longer = tallBlock(100'000);
    auto& terminal = longer->terminal;
    CHECK_FALSE(terminal.collapseLastFold());
    REQUIRE(terminal.viewport().scrollToTop());
    auto const frame = renderOf(terminal);
    REQUIRE(frame.chips.size() == 1);
    CHECK(frame.chips[0].lineOffset == LineOffset(0));
    CHECK(frame.chips[0].style == PillStyle::Success);
    for (auto const column: std::views::iota(0, unbox<int>(terminal.pageSize().columns)))
        CHECK_FALSE(
            terminal
                .collapsedFoldLabelAt(CellLocation { .line = LineOffset(0), .column = ColumnOffset(column) },
                                      ColumnCount(1))
                .has_value());
}

TEST_CASE("Gutter.terminal.aStatusChipKeepsClearOfAFoldChip", "[gutter][blockchip]")
{
    // The fold chip is drawn over blanks after the command: the status chip treats those as taken, and on a
    // page with no room left beside it, collapses rather than crowding it.
    auto mc = MockTerm { PageSize { LineCount(10), ColumnCount(26) } };
    mc.terminal.settings().gutter = RailGutter;
    runCommand(mc, "ls", { "a", "b", "c" });
    mc.writeToScreen(PromptStart);
    REQUIRE(mc.terminal.collapseLastFold());

    auto const buffer = renderOf(mc.terminal);
    REQUIRE(buffer.chips.size() == 1);
    CHECK(buffer.chips[0].text == U"3 lines hidden ⌄");
    CHECK(buffer.chips[0].style == PillStyle::Fold);
}

TEST_CASE("Gutter.terminal.aHoveredBlockIsTintedAndRinged", "[gutter][blockhover]")
{
    // The canvas's hover: the hovered block's rows, head to tail, under a tint and a ring of the label.
    auto mc = MockTerm { PageSize { LineCount(10), ColumnCount(20) } };
    runASuccessAndAFailure(mc, RailGutter); // `ls` on rows 0..2, `false` on rows 3..4
    CHECK_FALSE(renderOf(mc.terminal).blockHighlight.has_value());

    auto const* first = mc.terminal.commandBlockAt(LineOffset(0));
    auto const* second = mc.terminal.commandBlockAt(LineOffset(3));
    REQUIRE(first != nullptr);
    REQUIRE(second != nullptr);

    mc.terminal.setHoveredCommandBlock(first->id);
    auto const lit = renderOf(mc.terminal).blockHighlight;
    REQUIRE(lit.has_value());
    CHECK(lit->top == LineOffset(0));
    CHECK(lit->height == LineCount(3));
    auto const inks = blockHoverInks(mc.terminal.colorPalette());
    CHECK(lit->tint == inks.tint);
    CHECK(lit->ring == inks.ring);

    mc.terminal.setHoveredCommandBlock(second->id);
    auto const next = renderOf(mc.terminal).blockHighlight;
    REQUIRE(next.has_value());
    CHECK(next->top == LineOffset(3));
    CHECK(next->height == LineCount(2));

    // The pointer left every block: nothing is tinted.
    mc.terminal.setHoveredCommandBlock(std::nullopt);
    CHECK_FALSE(renderOf(mc.terminal).blockHighlight.has_value());
}

TEST_CASE("Gutter.terminal.theTerminalRowTintsNoBlock", "[gutter][blockhover]")
{
    // The Terminal row draws cells only.
    auto mc = MockTerm { PageSize { LineCount(10), ColumnCount(20) } };
    runASuccessAndAFailure(mc, GutterSettings { .exitStatus = true });
    auto const* first = mc.terminal.commandBlockAt(LineOffset(0));
    REQUIRE(first != nullptr);
    mc.terminal.setHoveredCommandBlock(first->id);
    CHECK_FALSE(renderOf(mc.terminal).blockHighlight.has_value());
}

TEST_CASE("Gutter.terminal.aHoverTintClipsAtThePage", "[gutter][blockhover]")
{
    // A page of four rows shows `file2`, `false`, `bad` and the prompt: of the first block, one row.
    auto mc = MockTerm { PageSize { LineCount(4), ColumnCount(20) }, LineCount(100) };
    runASuccessAndAFailure(mc, RailGutter);
    auto const* first = mc.terminal.commandBlockAt(LineOffset(-2));
    REQUIRE(first != nullptr);
    mc.terminal.setHoveredCommandBlock(first->id);
    auto const lit = renderOf(mc.terminal).blockHighlight;
    REQUIRE(lit.has_value());
    CHECK(lit->top == LineOffset(0));
    CHECK(lit->height == LineCount(1));
}

TEST_CASE("Gutter.terminal.aHoverTintFoldsWithItsBlock", "[gutter][blockhover]")
{
    // A collapsed block is its head row: so is its tint.
    auto mc = MockTerm { PageSize { LineCount(10), ColumnCount(40) } };
    runASuccessAndAFailure(mc, RailGutter);
    auto const* second = mc.terminal.commandBlockAt(LineOffset(3));
    REQUIRE(second != nullptr);
    REQUIRE(mc.terminal.collapseLastFold());
    mc.terminal.setHoveredCommandBlock(second->id);
    auto const lit = renderOf(mc.terminal).blockHighlight;
    REQUIRE(lit.has_value());
    CHECK(lit->top == LineOffset(3));
    CHECK(lit->height == LineCount(1));
}

TEST_CASE("Gutter.terminal.aRunningTimeCountsOnlyWhileOnDisplay", "[gutter][blockchip]")
{
    // I2: what the once-a-second redraw asks is whether a running time is on display -- a chip's, or the
    // sticky header's -- and nothing else.
    auto mc = MockTerm { PageSize { LineCount(5), ColumnCount(80) }, LineCount(100) };
    mc.terminal.settings().gutter = RailGutter;

    SECTION("a running header whose command row is off the page")
    {
        mc.terminal.settings().stickyHeader.mode = StickyHeaderMode::Always;
        startCommand(mc, "make");
        printLines(mc, "line", 20);
        auto const buffer = renderOf(mc.terminal);
        REQUIRE(buffer.stickyHeader.has_value());
        CHECK(buffer.chips.empty());
        CHECK(mc.terminal.hasVisibleRunningStatus());
    }
    SECTION("an off-page command row with no header")
    {
        mc.terminal.settings().stickyHeader.mode = StickyHeaderMode::Never;
        startCommand(mc, "make");
        printLines(mc, "line", 20);
        auto const buffer = renderOf(mc.terminal);
        CHECK_FALSE(buffer.stickyHeader.has_value());
        CHECK_FALSE(mc.terminal.hasVisibleRunningStatus());
    }
    SECTION("the alternate screen")
    {
        startCommand(mc, "vim");
        (void) renderOf(mc.terminal);
        REQUIRE(mc.terminal.hasVisibleRunningStatus());
        mc.writeToScreen("\033[?1049h");
        (void) renderOf(mc.terminal);
        CHECK_FALSE(mc.terminal.hasVisibleRunningStatus());
    }
    SECTION("exit status off")
    {
        mc.terminal.settings().gutter.exitStatus = false;
        mc.terminal.settings().stickyHeader.mode = StickyHeaderMode::Never;
        startCommand(mc, "make");
        auto const buffer = renderOf(mc.terminal);
        CHECK(buffer.chips.empty());
        CHECK_FALSE(mc.terminal.hasVisibleRunningStatus());
    }
}

TEST_CASE("Gutter.terminal.aCrossfadeSnapshotLeavesTheRunningTime", "[gutter][blockchip]")
{
    // m8: whether a running time is on display is a fact of the frames drawn. The crossfade's snapshot of the
    // outgoing page is built, never drawn, and must not change it -- here, though the command's row has
    // scrolled off since the last frame, that frame still shows its chip.
    auto mc = MockTerm { PageSize { LineCount(5), ColumnCount(80) }, LineCount(100) };
    mc.terminal.settings().gutter = RailGutter;
    mc.terminal.settings().screenTransitionStyle = ScreenTransitionStyle::Fade;
    mc.terminal.settings().stickyHeader.mode = StickyHeaderMode::Never;
    mc.writeToScreen("\033[?64h"); // DECPCCM: the page on display follows the cursor's
    startCommand(mc, "make");
    (void) renderOf(mc.terminal);
    REQUIRE(mc.terminal.hasVisibleRunningStatus());

    printLines(mc, "line", 20); // the command row scrolls off; no frame is drawn
    mc.writeToScreen("\033[U"); // NP: the next page, crossfaded from a snapshot of this one
    CHECK(mc.terminal.hasVisibleRunningStatus());
}

TEST_CASE("Gutter.blankRunsOf.onlyTrulyBlankCells", "[gutter][blockchip]")
{
    // m2: a chip goes over cells that show nothing. A wide character's right half and a reverse-video space
    // show something, though neither holds a codepoint of its own that is not blank.
    auto mc = MockTerm { PageSize { LineCount(3), ColumnCount(12) } };
    mc.writeToScreen("ab\xE3\x81\x82 \033[7m \033[m"); // a b あ(2..3) ' '(4) reverse ' '(5)
    auto const& line = mc.terminal.primaryScreen().grid().lineAt(LineOffset(0));
    auto const runs = blankRunsOf(line, LineOffset(0), ColumnCount(12));
    auto const blank = [&](int column) {
        return std::ranges::any_of(runs, [&](ColumnRange const& run) {
            return run.contains(CellLocation { .line = LineOffset(0), .column = ColumnOffset(column) });
        });
    };
    CHECK_FALSE(blank(2));
    CHECK_FALSE(blank(3)); // the wide character's continuation
    CHECK(blank(4));
    CHECK_FALSE(blank(5)); // a reverse-video space is a block of the foreground
    CHECK(blank(6));
    CHECK(blank(11));
}
