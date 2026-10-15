// SPDX-License-Identifier: Apache-2.0
#include <vtbackend/core/CommandBlockOutcome.hpp>
#include <vtbackend/render/RenderBuffer.hpp>
#include <vtbackend/screen/Gutter.hpp>
#include <vtbackend/screen/GutterSettings.hpp>
#include <vtbackend/screen/Terminal.hpp>
#include <vtbackend/screen/TerminalClocks.hpp>
#include <vtbackend/shell/Folding.hpp>
#include <vtbackend/testing/MockTerm.hpp>
#include <vtbackend/testing/TestHelpers.hpp>

#include <vtpty/MockPty.hpp>

#include <core/Environment.hpp>
#include <core/platform/Clock.hpp>

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
    CHECK(rows.at(0) == withGlyph(U"", foldMarkerGlyph(FoldMarker::Expanded)));
    CHECK(rows.at(1) == withGlyph(U"", foldMarkerGlyph(FoldMarker::Body)));
    CHECK(rows.at(3) == withGlyph(U"", foldMarkerGlyph(FoldMarker::Body)));
    CHECK(rows.at(4) == withGlyph(U"", foldMarkerGlyph(FoldMarker::BodyEnd)));
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
    auto mc = MockTerm { PageSize { LineCount(10), ColumnCount(20) } };
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
    CHECK(annotations[0].text == U"⋯ 3 lines");
    CHECK(annotations[0].attributes.foregroundColor == mc.terminal.colorPalette().gutterTextColor());
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
    CHECK(annotations[0].text == U"⋯ 2 lines");
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
    CHECK(annotations[0].text == U"⋯ ");                       // cut at the page's edge
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
