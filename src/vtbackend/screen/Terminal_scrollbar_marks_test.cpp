// SPDX-License-Identifier: Apache-2.0
#include <vtbackend/screen/Terminal.hpp>
#include <vtbackend/shell/CommandBlock.hpp>
#include <vtbackend/shell/ScrollbarMarks.hpp>
#include <vtbackend/shell/UserMarkIndex.hpp>
#include <vtbackend/testing/MockTerm.hpp>
#include <vtbackend/testing/TestHelpers.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <format>
#include <ranges>
#include <set>
#include <span>
#include <string>
#include <utility>
#include <vector>

using namespace vtbackend;

namespace
{

/// Puts @p mc into vi normal mode with its cursor on the page's top row.
void enterNormalModeAtTop(MockTerm<>& mc)
{
    mc.writeToScreen("\033[H");
    mc.terminal.inputHandler().setMode(ViMode::Normal);
}

/// Whether the line @p stableId names on the primary screen carries @p flag.
[[nodiscard]] bool lineHasFlag(Terminal const& terminal, int64_t stableId, LineFlag flag)
{
    auto const& grid = terminal.primaryScreen().grid();
    auto const line = grid.lineOffsetOf(stableId);
    return line.has_value() && grid.lineAt(*line).isFlagEnabled(flag);
}

/// The text of the line @p stableId names on the primary screen, without its trailing blanks.
[[nodiscard]] std::string lineTextOf(Terminal const& terminal, int64_t stableId)
{
    auto const& grid = terminal.primaryScreen().grid();
    auto const line = grid.lineOffsetOf(stableId);
    REQUIRE(line.has_value());
    return grid.lineTextTrimmed(*line);
}

/// A MockTerm that counts the screen updates it is told about -- the signal a frontend reschedules its
/// scrollbar marks on -- and the redraws it is asked for.
struct EventCountingTerm final: MockTerm<>
{
    using MockTerm<>::MockTerm;

    int screenUpdates = 0; ///< How often the terminal said the screen changed.
    int redraws = 0;       ///< How often the terminal asked for a frame to be drawn.

    void screenUpdated() override { ++screenUpdates; }
    void renderBufferUpdated() override { ++redraws; }
};

/// Sets @p terminal gliding, as a flick of the mouse wheel leaves it.
/// @param terminal The terminal, with smooth scrolling turned on here.
void startGlide(Terminal& terminal)
{
    terminal.settings().smoothScrolling = true;
    terminal.setCellPixelSize(ImageSize { vtpty::Width(10), vtpty::Height(20) });
    REQUIRE(terminal.injectWheelMomentum(180.0f, std::chrono::steady_clock::time_point {})
            == SmoothScrollResult::Applied);
    REQUIRE(terminal.isMomentumScrollActive());
}

} // namespace

TEST_CASE("ScrollbarMarks.userMarks.theIndexHearsEveryViMark", "[scrollbar-marks]")
{
    auto mc = MockTerm { PageSize { LineCount(10), ColumnCount(40) }, LineCount(100) };
    mc.writeToScreen("one\r\ntwo\r\nthree\r\n");
    enterNormalModeAtTop(mc);

    mc.sendCharSequence("jmm"); // on "two"
    REQUIRE(mc.terminal.userMarks().ids().size() == 1);
    auto const id = *mc.terminal.userMarks().ids().begin();
    CHECK(lineHasFlag(mc.terminal, id, LineFlag::UserMark));
    CHECK(lineTextOf(mc.terminal, id).starts_with("two"));

    mc.sendCharSequence("mm"); // and off again
    CHECK(mc.terminal.userMarks().ids().empty());
}

TEST_CASE("ScrollbarMarks.userMarks.theAlternateScreenKeepsItsMarksToItself", "[scrollbar-marks]")
{
    // The alternate screen has no scrollback for a tick to point into.
    auto mc = MockTerm { PageSize { LineCount(10), ColumnCount(40) }, LineCount(100) };

    SECTION("a mark there is none here")
    {
        // The index is not even told: an unchanged revision is what the frontend would redraw on.
        mc.writeToScreen("\033[?1049hvim\r\n");
        enterNormalModeAtTop(mc);
        auto const revision = mc.terminal.userMarks().revision();
        mc.sendCharSequence("mm");
        CHECK(mc.terminal.userMarks().revision() == revision);
        CHECK(mc.terminal.userMarks().ids().empty());
    }

    SECTION("nor does taking one off there take the primary screen's")
    {
        // Each grid numbers its rows from its own base, so a row on the alternate screen may well carry the
        // id of the primary screen's marked one: here the bottom row of a page scrolled by one, and the top
        // row of an alternate screen cleared once.
        test::printLines(mc, "line", 10);
        mc.writeToScreen("bottom");
        mc.terminal.inputHandler().setMode(ViMode::Normal); // where the cursor is: on "bottom"
        mc.sendCharSequence("mm");
        mc.terminal.inputHandler().setMode(ViMode::Insert);
        REQUIRE(mc.terminal.userMarks().ids().size() == 1);
        auto const marked = *mc.terminal.userMarks().ids().begin();

        mc.writeToScreen("\033[?1049h\033[Hvim\r\n");
        enterNormalModeAtTop(mc);
        REQUIRE(mc.terminal.currentScreen().grid().stableLineIdOf(LineOffset(0)) == marked);
        mc.sendCharSequence("mmmm");
        mc.terminal.inputHandler().setMode(ViMode::Insert);
        mc.writeToScreen("\033[?1049l");

        CHECK(mc.terminal.userMarks().ids() == std::set { marked });
    }
}

TEST_CASE("ScrollbarMarks.userMarks.aColumnResizeRescansThem", "[scrollbar-marks]")
{
    // A column change reflows the grid and renames every row (Grid::stableIdGeneration), so the ids the
    // index held now name other lines. One scan puts it right: the mark itself rode the reflow on its
    // line's head, as every HeadOnlyLineFlags member does.
    auto mc = MockTerm { PageSize { LineCount(10), ColumnCount(40) }, LineCount(100) };
    mc.writeToScreen("one\r\ntwo\r\nthree\r\n");
    enterNormalModeAtTop(mc);
    mc.sendCharSequence("jmm"); // on "two"
    mc.terminal.inputHandler().setMode(ViMode::Insert);

    auto const& grid = mc.terminal.primaryScreen().grid();
    auto const generation = grid.stableIdGeneration();
    mc.terminal.resizeScreen(PageSize { LineCount(10), ColumnCount(30) });
    REQUIRE(grid.stableIdGeneration() != generation);

    auto const& index = mc.terminal.userMarks();
    CHECK(index.isCurrentFor(grid.stableIdGeneration()));
    REQUIRE(index.ids().size() == 1);
    CHECK(lineTextOf(mc.terminal, *index.ids().begin()).starts_with("two"));
}

TEST_CASE("ScrollbarMarks.userMarks.noMarkEverPlacedNeedsNoRescan", "[scrollbar-marks]")
{
    // Every mark reaches the index as it is placed, so until one has been, a reflow's rescan has nothing to
    // find and a session that never used `mm` need not walk its scrollback on every column change. Seen
    // here through a flag set behind the index's back, which only a scan could find.
    auto mc = MockTerm { PageSize { LineCount(10), ColumnCount(40) }, LineCount(100) };
    mc.writeToScreen("one\r\ntwo\r\n");
    auto& grid = mc.terminal.primaryScreen().grid();
    grid.lineAt(LineOffset(1)).setFlag(LineFlag::UserMark, true);

    auto const generation = grid.stableIdGeneration();
    mc.terminal.resizeScreen(PageSize { LineCount(10), ColumnCount(30) });
    REQUIRE(grid.stableIdGeneration() != generation);

    auto const& index = mc.terminal.userMarks();
    CHECK(index.isCurrentFor(grid.stableIdGeneration()));
    CHECK(index.ids().empty());
}

TEST_CASE("ScrollbarMarks.userMarks.evictedMarksArePruned", "[scrollbar-marks]")
{
    auto mc = MockTerm { PageSize { LineCount(4), ColumnCount(20) }, LineCount(4) };
    mc.writeToScreen("one\r\ntwo\r\n");
    enterNormalModeAtTop(mc);
    mc.sendCharSequence("mm"); // on "one"
    mc.terminal.inputHandler().setMode(ViMode::Insert);
    REQUIRE(mc.terminal.userMarks().ids().size() == 1);
    auto const marked = *mc.terminal.userMarks().ids().begin();

    mc.writeToScreen("\033[99;1H");
    test::printLines(mc, "filler", 20);

    REQUIRE(marked < mc.terminal.primaryScreen().grid().stableRangeFloor());
    CHECK(mc.terminal.userMarks().ids().empty());
}

TEST_CASE("ScrollbarMarks.userMarks.aMarkFollowsItsLineThroughAScrollInsideMargins", "[scrollbar-marks]")
{
    // A scroll inside margins moves the rows' contents -- flags included -- from id to id, while each id
    // keeps naming its place. The index has to move with the content, or the tick stays on whatever line
    // scrolled into the mark's old place and the marked line gets none.
    auto mc = MockTerm { PageSize { LineCount(10), ColumnCount(40) }, LineCount(100) };
    mc.writeToScreen("l0\r\nl1\r\nl2\r\nl3\r\nl4\r\nl5\r\nl6");
    enterNormalModeAtTop(mc);
    mc.sendCharSequence("jjjmm"); // on "l3"
    mc.terminal.inputHandler().setMode(ViMode::Insert);
    REQUIRE(mc.terminal.userMarks().ids().size() == 1);
    auto const before = *mc.terminal.userMarks().ids().begin();
    auto const revision = mc.terminal.userMarks().revision();

    // Rows 2 to 6 scroll up by one, so "l3" moves to the row "l2" was on.
    mc.writeToScreen("\033[2;6r\033[S");

    auto const& index = mc.terminal.userMarks();
    REQUIRE(index.ids().size() == 1);
    auto const after = *index.ids().begin();
    CHECK(after == before - 1);
    CHECK(lineTextOf(mc.terminal, after).starts_with("l3"));
    CHECK(lineHasFlag(mc.terminal, after, LineFlag::UserMark));
    CHECK(index.revision() != revision);
}

TEST_CASE("ScrollbarMarks.userMarks.aLineFeedFollowsAMarkInsideMarginsAndLeavesItOnAWholePage",
          "[scrollbar-marks]")
{
    // A line feed on the bottom margin scrolls what SU scrolls, by the same path: inside margins the mark
    // moves with its line's content from id to id, while on a whole page every row keeps its id and nothing
    // moves.
    auto mc = MockTerm { PageSize { LineCount(10), ColumnCount(40) }, LineCount(100) };
    mc.writeToScreen("l0\r\nl1\r\nl2\r\nl3\r\nl4\r\nl5\r\nl6\r\nl7\r\nl8\r\nl9");
    enterNormalModeAtTop(mc);
    mc.sendCharSequence("jjjmm"); // on "l3"
    mc.terminal.inputHandler().setMode(ViMode::Insert);
    REQUIRE(mc.terminal.userMarks().ids().size() == 1);
    auto const before = *mc.terminal.userMarks().ids().begin();

    SECTION("inside margins")
    {
        mc.writeToScreen("\033[2;6r\033[6;1H\n"); // rows 2 to 6 scroll up by one: "l3" moves up a row
        REQUIRE(mc.terminal.userMarks().ids().size() == 1);
        auto const after = *mc.terminal.userMarks().ids().begin();
        CHECK(after == before - 1);
        CHECK(lineTextOf(mc.terminal, after).starts_with("l3"));
    }

    SECTION("on the whole page")
    {
        mc.writeToScreen("\033[10;1H\n"); // the page scrolls up by one into the history
        CHECK(mc.terminal.userMarks().ids() == std::set { before });
        CHECK(lineTextOf(mc.terminal, before).starts_with("l3"));
    }
}

TEST_CASE("ScrollbarMarks.userMarks.aMarkAnEraseTookIsDropped", "[scrollbar-marks]")
{
    // ED resets the rows it erases whole, past the mark funnel, so the erase reports the mark it takes
    // itself -- or the scrollbar keeps a tick for a blank row.
    auto mc = MockTerm { PageSize { LineCount(10), ColumnCount(40) }, LineCount(100) };
    mc.writeToScreen("one\r\ntwo\r\nthree\r\n");
    enterNormalModeAtTop(mc);
    mc.sendCharSequence("jjmm"); // on "three"
    mc.terminal.inputHandler().setMode(ViMode::Insert);
    REQUIRE(mc.terminal.userMarks().ids().size() == 1);
    auto const revision = mc.terminal.userMarks().revision();

    SECTION("ED 0 below the cursor")
    {
        mc.writeToScreen("\033[H\033[J");
    }

    SECTION("ED 1 above it")
    {
        mc.writeToScreen("\033[5;1H\033[1J");
    }

    SECTION("DECALN over the whole page")
    {
        mc.writeToScreen("\033#8");
    }

    // The cursor's own row is erased whole too when ED starts or ends at its edge, as DEC STD 070's ED sets
    // that row to single width with every other row it erases whole.
    SECTION("ED 0 from the first column of the mark's row")
    {
        mc.writeToScreen("\033[3;1H\033[J");
    }

    SECTION("ED 1 to the last column of the mark's row")
    {
        mc.writeToScreen("\033[3;40H\033[1J");
    }

    CHECK(mc.terminal.userMarks().ids().empty());
    CHECK(mc.terminal.userMarks().revision() != revision);
}

TEST_CASE("ScrollbarMarks.userMarks.aRowErasedInPartKeepsItsMark", "[scrollbar-marks]")
{
    // EL changes what a line says, not which line it is, and an ED that starts or ends inside the cursor's
    // row erases only part of it: the mark stays with the row in both.
    auto mc = MockTerm { PageSize { LineCount(10), ColumnCount(40) }, LineCount(100) };
    mc.writeToScreen("one\r\ntwo\r\nthree\r\n");
    enterNormalModeAtTop(mc);
    mc.sendCharSequence("jjmm"); // on "three"
    mc.terminal.inputHandler().setMode(ViMode::Insert);
    REQUIRE(mc.terminal.userMarks().ids().size() == 1);
    auto const revision = mc.terminal.userMarks().revision();

    SECTION("EL 2 over the whole row")
    {
        mc.writeToScreen("\033[3;1H\033[2K");
    }

    SECTION("ED 0 from its second column")
    {
        mc.writeToScreen("\033[3;2H\033[J");
    }

    SECTION("ED 1 to its second-to-last column")
    {
        mc.writeToScreen("\033[3;39H\033[1J");
    }

    CHECK(mc.terminal.userMarks().ids().size() == 1);
    CHECK(mc.terminal.userMarks().revision() == revision);
}

TEST_CASE("ScrollbarMarks.userMarks.aMarkAShrinkingPageCutOffIsDropped", "[scrollbar-marks]")
{
    // A page that shrinks below its cursor cuts the rows under it off, and the next rows scrolled in come to
    // name their ids: a mark on one of them must not come back as a tick on a fresh, unmarked row.
    auto mc = MockTerm { PageSize { LineCount(10), ColumnCount(40) }, LineCount(100) };
    mc.writeToScreen("one\r\ntwo\r\nthree");
    enterNormalModeAtTop(mc);
    mc.sendCharSequence("jjjjjjjmm"); // on the empty row 7, below the cursor
    mc.terminal.inputHandler().setMode(ViMode::Insert);
    REQUIRE(mc.terminal.userMarks().ids().size() == 1);

    mc.terminal.resizeScreen(PageSize { LineCount(5), ColumnCount(40) });
    mc.writeToScreen("\r\nfour\r\nfive\r\nsix\r\nseven\r\neight");

    CHECK(mc.terminal.userMarks().ids().empty());
    CHECK(mc.terminal.scrollbarMarkInputs().empty());
}

TEST_CASE("ScrollbarMarks.userMarks.aViMarkSaysTheScreenChanged", "[scrollbar-marks]")
{
    // A mark moves no cell and no cursor, so unless it says so the frontend learns of it -- and redraws
    // the scrollbar's ticks -- only with the next byte of output.
    auto mc = EventCountingTerm { PageSize { LineCount(10), ColumnCount(40) }, LineCount(100) };
    mc.writeToScreen("one\r\n");
    enterNormalModeAtTop(mc);

    auto const before = mc.screenUpdates;
    mc.sendCharSequence("mm");
    CHECK(mc.screenUpdates > before);
}

namespace
{

/// The kinds of @p inputs, in the order the terminal reported them.
[[nodiscard]] std::vector<ScrollbarMarkKind> kindsOf(std::span<ScrollbarMarkInput const> inputs)
{
    return std::ranges::to<std::vector>(inputs | std::views::transform(&ScrollbarMarkInput::kind));
}

/// The scrollbar row of the line @p stableId names while nothing is collapsed, counted as the viewport
/// counts its range: the history rows above the page, plus the line's place on it (negative in the history).
[[nodiscard]] int64_t unfoldedRowOf(Terminal const& terminal, int64_t stableId)
{
    auto const& grid = terminal.primaryScreen().grid();
    auto const line = grid.lineOffsetOf(stableId);
    REQUIRE(line.has_value());
    return unbox<int64_t>(grid.historyLineCount()) + unbox<int64_t>(*line);
}

/// The rows the scrollbar spans: what Viewport::scrollableLineCount() lets it scroll, plus the page.
[[nodiscard]] int64_t scrollbarRowsOf(Terminal const& terminal)
{
    return unbox<int64_t>(terminal.viewport().scrollableLineCount())
           + unbox<int64_t>(terminal.pageSize().lines);
}

} // namespace

TEST_CASE("ScrollbarMarks.terminal.blocksAreMarkedAtTheirHeads", "[scrollbar-marks]")
{
    auto mc = MockTerm { PageSize { LineCount(10), ColumnCount(40) }, LineCount(100) };
    test::runCommand(mc, "ls", { "file1", "file2" }, 0);
    test::runCommand(mc, "false", {}, 1);
    test::startCommand(mc, "sleep 9");

    auto const inputs = mc.terminal.scrollbarMarkInputs();
    REQUIRE(kindsOf(inputs)
            == std::vector {
                ScrollbarMarkKind::Command, ScrollbarMarkKind::Failure, ScrollbarMarkKind::Running });

    // Oldest block first, each on its prompt line: rows 0, 3 and 4 of a page nothing has scrolled yet.
    CHECK(inputs[0].visibleRow == 0);
    CHECK(inputs[1].visibleRow == 3);
    CHECK(inputs[2].visibleRow == 4);
    for (auto const& input: inputs)
    {
        CHECK(lineHasFlag(mc.terminal, input.targetStableId, LineFlag::Marked));
        CHECK(input.visibleRow == unfoldedRowOf(mc.terminal, input.targetStableId));
        CHECK(input.visibleRow < scrollbarRowsOf(mc.terminal));
    }
}

TEST_CASE("ScrollbarMarks.terminal.positionsCountFromTheOldestHistoryRow", "[scrollbar-marks]")
{
    // Row 0 is the top of the scrollback, not of the page: the handle measures its range from there.
    auto mc = MockTerm { PageSize { LineCount(5), ColumnCount(40) }, LineCount(100) };
    test::runCommand(mc, "seq", { "1", "2", "3", "4", "5", "6", "7", "8" }, 0);
    test::startCommand(mc, "top");
    REQUIRE(mc.terminal.primaryScreen().grid().historyLineCount() > LineCount(0));

    auto const inputs = mc.terminal.scrollbarMarkInputs();
    REQUIRE(inputs.size() == 2);
    CHECK(inputs[0].visibleRow == 0);
    CHECK(inputs[1].visibleRow == 9); // the prompt, eight lines of output, and then the next prompt
    CHECK(inputs[1].visibleRow < scrollbarRowsOf(mc.terminal));
}

TEST_CASE("ScrollbarMarks.terminal.positionsHoldOnceTheScrollbackEvicts", "[scrollbar-marks]")
{
    // A full scrollback evicts a row for every row it takes in, so its oldest row -- the scrollbar's row 0 --
    // is no longer the first row the terminal ever had.
    //
    // The page is smaller than the Vi scroll-off margin, so any motion would scroll the viewport into the
    // history and pull the Vi cursor along: the line to mark is put on the page's top row instead.
    auto mc = MockTerm { PageSize { LineCount(4), ColumnCount(20) }, LineCount(4) };
    test::printLines(mc, "filler", 20);
    mc.writeToScreen("marked\r\n");
    test::runCommand(mc, "ls", { "file1" }, 0);
    enterNormalModeAtTop(mc);
    mc.sendCharSequence("mm"); // on "marked"
    mc.terminal.inputHandler().setMode(ViMode::Insert);
    REQUIRE(mc.terminal.primaryScreen().grid().stableRangeFloor() > 0);

    auto const inputs = mc.terminal.scrollbarMarkInputs();
    REQUIRE(kindsOf(inputs) == std::vector { ScrollbarMarkKind::Command, ScrollbarMarkKind::UserMark });
    CHECK(lineTextOf(mc.terminal, inputs[0].targetStableId).starts_with("$ ls"));
    CHECK(lineTextOf(mc.terminal, inputs[1].targetStableId).starts_with("marked"));
    for (auto const& input: inputs)
    {
        CHECK(input.visibleRow == unfoldedRowOf(mc.terminal, input.targetStableId));
        CHECK(input.visibleRow < scrollbarRowsOf(mc.terminal));
    }
}

TEST_CASE("ScrollbarMarks.terminal.aFullPageReverseIndexTakesTheTickAlongWithItsLine", "[scrollbar-marks]")
{
    // RI on the top row of a page with history above it moves the page's rows down from id to id: rotating
    // the ring would turn the newest history row into the page's blank top. The mark goes with its line's
    // content, and its tick one row down with it. The line to mark is on the top row, for the reason
    // positionsHoldOnceTheScrollbackEvicts gives.
    auto mc = MockTerm { PageSize { LineCount(4), ColumnCount(20) }, LineCount(4) };
    test::printLines(mc, "filler", 20);
    mc.writeToScreen("one\r\ntwo\r\nthree\r\nfour");
    enterNormalModeAtTop(mc);
    mc.sendCharSequence("mm"); // on "one"
    mc.terminal.inputHandler().setMode(ViMode::Insert);
    REQUIRE(mc.terminal.primaryScreen().grid().historyLineCount() > LineCount(0));
    auto const before = mc.terminal.scrollbarMarkInputs();
    REQUIRE(before.size() == 1);

    mc.writeToScreen("\033[H\033M");

    auto const after = mc.terminal.scrollbarMarkInputs();
    REQUIRE(after.size() == 1);
    CHECK(after[0].targetStableId == before[0].targetStableId + 1);
    CHECK(lineTextOf(mc.terminal, after[0].targetStableId).starts_with("one"));
    CHECK(after[0].visibleRow == before[0].visibleRow + 1);
    CHECK(after[0].visibleRow == unfoldedRowOf(mc.terminal, after[0].targetStableId));
}

TEST_CASE("ScrollbarMarks.terminal.positionsCountVisibleRows", "[scrollbar-marks]")
{
    // The scrollbar's own range counts a collapsed block as its one head row (Viewport::scrollableLineCount),
    // so the ticks must as well -- or every tick below a fold sits as many rows too low as the fold hides.
    auto mc = MockTerm { PageSize { LineCount(10), ColumnCount(40) }, LineCount(100) };
    test::runCommand(mc, "ls", { "file1", "file2" }, 0);
    test::runCommand(mc, "false", {}, 1);
    test::startCommand(mc, "sleep 9");

    test::collapseOnlyFold(mc); // only `ls` printed whole lines of output, so it has the one fold
    REQUIRE(mc.terminal.hiddenLineCount() == LineCount(2));

    auto const inputs = mc.terminal.scrollbarMarkInputs();
    REQUIRE(inputs.size() == 3);
    CHECK(inputs[0].visibleRow == 0);
    CHECK(inputs[1].visibleRow == 1);
    CHECK(inputs[2].visibleRow == 2);
    CHECK(inputs[2].visibleRow < scrollbarRowsOf(mc.terminal));
}

TEST_CASE("ScrollbarMarks.terminal.blockMarksFollowTheirHeadsThroughAColumnResize", "[scrollbar-marks]")
{
    // The window narrowed while a long command runs. The reflow renames every row; each record's head is
    // re-pointed, and the ticks have to follow it there.
    auto mc = MockTerm { PageSize { LineCount(10), ColumnCount(40) }, LineCount(100) };
    test::runCommand(mc, "ls", { "file1", "file2" }, 0);
    test::startCommand(mc, "make");
    mc.writeToScreen(std::format("{}\r\n{}\r\n", std::string(35, 'x'), std::string(35, 'y')));

    auto const& grid = mc.terminal.primaryScreen().grid();
    auto const generation = grid.stableIdGeneration();
    mc.terminal.resizeScreen(PageSize { LineCount(10), ColumnCount(20) });
    REQUIRE(grid.stableIdGeneration() != generation);

    // Output keeps arriving after the resize, as it does under a running build.
    mc.writeToScreen("zzz\r\n");

    auto const inputs = mc.terminal.scrollbarMarkInputs();
    REQUIRE(kindsOf(inputs) == std::vector { ScrollbarMarkKind::Command, ScrollbarMarkKind::Running });
    // Each tick on its own block's head: neither both on one, nor the two swapped.
    CHECK(lineTextOf(mc.terminal, inputs[0].targetStableId) == "$ ls");
    CHECK(lineTextOf(mc.terminal, inputs[1].targetStableId) == "$ make");
    for (auto const& input: inputs)
    {
        CHECK(lineHasFlag(mc.terminal, input.targetStableId, LineFlag::Marked));
        CHECK(input.visibleRow == unfoldedRowOf(mc.terminal, input.targetStableId));
    }
}

TEST_CASE("ScrollbarMarks.terminal.userMarksAreMarkedWhereTheyStand", "[scrollbar-marks]")
{
    auto mc = MockTerm { PageSize { LineCount(10), ColumnCount(40) }, LineCount(100) };
    mc.writeToScreen("one\r\ntwo\r\nthree\r\nfour\r\nfive\r\n");
    enterNormalModeAtTop(mc);
    mc.sendCharSequence("mm");   // on "one"
    mc.sendCharSequence("jjmm"); // on "three"
    mc.terminal.inputHandler().setMode(ViMode::Insert);

    auto const inputs = mc.terminal.scrollbarMarkInputs();
    REQUIRE(kindsOf(inputs) == std::vector { ScrollbarMarkKind::UserMark, ScrollbarMarkKind::UserMark });
    CHECK(inputs[1].visibleRow - inputs[0].visibleRow == 2);
    for (auto const& input: inputs)
        CHECK(input.visibleRow == unfoldedRowOf(mc.terminal, input.targetStableId));

    SECTION("an erase of its row takes the tick")
    {
        // ED from the start of "two" erases every row below it whole, "three" among them.
        mc.writeToScreen("\033[2;1H\033[J");
        auto const after = mc.terminal.scrollbarMarkInputs();
        REQUIRE(after.size() == 1);
        CHECK(after[0] == inputs[0]);
    }

    SECTION("a scroll inside margins takes the tick along with its line")
    {
        // Rows 2 to 5 scroll up by one: "three" moves to row 1, and "one" stays where it was.
        mc.writeToScreen("\033[2;5r\033[S");
        auto const after = mc.terminal.scrollbarMarkInputs();
        REQUIRE(after.size() == 2);
        CHECK(after[0] == inputs[0]);
        CHECK(after[1].visibleRow == inputs[1].visibleRow - 1);
        CHECK(lineTextOf(mc.terminal, after[1].targetStableId).starts_with("three"));
    }
}

TEST_CASE("ScrollbarMarks.terminal.aMarkInsideACollapsedBlockStandsOnItsHead", "[scrollbar-marks]")
{
    auto mc = MockTerm { PageSize { LineCount(10), ColumnCount(40) }, LineCount(100) };
    test::runCommand(mc, "ls", { "file1", "file2" }, 0);
    test::runCommand(mc, "pwd", { "/tmp" }, 0);
    enterNormalModeAtTop(mc);
    mc.sendCharSequence("jmm"); // on "file1", inside the output of `ls`
    mc.terminal.inputHandler().setMode(ViMode::Insert);

    // The two blocks oldest first, then the mark.
    auto const before = mc.terminal.scrollbarMarkInputs();
    REQUIRE(kindsOf(before)
            == std::vector {
                ScrollbarMarkKind::Command, ScrollbarMarkKind::Command, ScrollbarMarkKind::UserMark });

    mc.terminal.foldState().collapse(before[0].targetStableId);
    auto const after = mc.terminal.scrollbarMarkInputs();
    REQUIRE(after.size() == 3);
    CHECK(after[2].targetStableId == before[2].targetStableId); // a click still goes to the marked line...
    CHECK(after[2].visibleRow == after[0].visibleRow); // ...but the tick stands on the head hiding it
}

TEST_CASE("ScrollbarMarks.terminal.evictedHeadsAreDropped", "[scrollbar-marks]")
{
    // Records outlive their rows; a tick for one whose head is gone would point at nothing.
    auto mc = MockTerm { PageSize { LineCount(4), ColumnCount(20) }, LineCount(4) };
    test::runCommand(mc, "old", { "a" }, 1);
    auto const* old = mc.terminal.commandBlocks().lastFinished();
    REQUIRE(old != nullptr);
    auto const oldHead = old->headStableId;
    REQUIRE(mc.terminal.scrollbarMarkInputs().size() == 1);

    test::printLines(mc, "filler", 20);

    REQUIRE(oldHead < mc.terminal.primaryScreen().grid().stableRangeFloor());
    CHECK(mc.terminal.scrollbarMarkInputs().empty());
}

TEST_CASE("ScrollbarMarks.terminal.aMirroredBlockIsMarkedOnceItsHeadIsPlaced", "[scrollbar-marks]")
{
    // A daemon mirror adopts the host's record before -- or without -- the row that heads it here: its head
    // names no row of this grid until placeCommandBlockHeads() finds one, and a tick before then would point
    // into the host's numbering.
    auto mc = MockTerm { PageSize { LineCount(6), ColumnCount(20) }, LineCount(0) };
    auto record = test::mirroredRecord(5, CommandBlockState::Running);
    record.headIdGeneration = UnknownHeadGeneration;
    mc.terminal.adoptCommandBlock(std::move(record), AdoptMode::Snapshot);
    CHECK(mc.terminal.scrollbarMarkInputs().empty());

    auto& head = mc.terminal.primaryScreen().grid().lineAt(LineOffset(3));
    head.adoptBlock(CommandBlockId { 5 });
    head.setFlag(LineFlag::Marked, true);
    auto block = std::array { CommandBlockId { 5 } };
    mc.terminal.placeCommandBlockHeads(block);

    auto const inputs = mc.terminal.scrollbarMarkInputs();
    REQUIRE(inputs.size() == 1);
    CHECK(inputs[0].kind == ScrollbarMarkKind::Running);
    CHECK(inputs[0].visibleRow == 3);
    CHECK(inputs[0].targetStableId == mc.terminal.primaryScreen().grid().stableLineIdOf(LineOffset(3)));
}

TEST_CASE("ScrollbarMarks.terminal.theRevisionMovesWithEveryInputOfTheTicks", "[scrollbar-marks]")
{
    // A frontend keys its ticks on the revision, so whatever can move a tick moves it; and a read that finds
    // nothing new leaves it, or every frame would copy the marks out again.
    auto mc = MockTerm { PageSize { LineCount(6), ColumnCount(20) }, LineCount(30) };
    auto& terminal = mc.terminal;
    auto revision = terminal.scrollbarMarksRevision();
    auto const moved = [&] {
        auto const now = terminal.scrollbarMarksRevision();
        return std::exchange(revision, now) != now;
    };

    test::runCommand(mc, "ls", { "file1", "file2" }, 0);
    CHECK(moved()); // a block
    CHECK_FALSE(moved());

    mc.writeToScreen("plain output\r\n");
    CHECK_FALSE(moved()); // nothing a tick stands on

    enterNormalModeAtTop(mc);
    mc.sendCharSequence("jmm");
    terminal.inputHandler().setMode(ViMode::Insert);
    CHECK(moved()); // a vi mark
    CHECK_FALSE(moved());

    auto const head = terminal.scrollbarMarkInputs().front().targetStableId;
    terminal.foldState().collapse(head);
    CHECK(moved()); // a fold
    CHECK_FALSE(moved());

    mc.writeToScreen("\033[?1049h");
    CHECK(moved()); // the alternate screen: no tick can be revealed
    mc.writeToScreen("\033[?1049l");
    CHECK(moved());

    terminal.resizeScreen(PageSize { LineCount(6), ColumnCount(25) });
    CHECK(moved()); // every row renamed
    CHECK_FALSE(moved());

    // A row evicted from the history moves every tick up a row, though nothing a tick stands on changed.
    auto full = MockTerm { PageSize { LineCount(6), ColumnCount(20) }, LineCount(10) };
    test::printLines(full, "filler", 20);
    test::runCommand(full, "ls", { "file1" }, 0);
    auto const before = full.terminal.scrollbarMarksRevision();
    full.writeToScreen("more\r\n");
    CHECK(full.terminal.scrollbarMarksRevision() != before);
}

TEST_CASE("ScrollbarMarks.terminal.theAlternateScreenHasNone", "[scrollbar-marks]")
{
    // A full-screen program over existing blocks shows no ticks, and leaving it brings them all back.
    auto mc = MockTerm { PageSize { LineCount(10), ColumnCount(40) }, LineCount(100) };
    test::runCommand(mc, "ls", { "file1" }, 0);
    test::startCommand(mc, "vim");

    mc.writeToScreen("\033[?1049h");
    CHECK(mc.terminal.scrollbarMarkInputs().empty());

    mc.writeToScreen("\033[?1049l");
    CHECK(kindsOf(mc.terminal.scrollbarMarkInputs())
          == std::vector { ScrollbarMarkKind::Command, ScrollbarMarkKind::Running });
}

TEST_CASE("ScrollbarMarks.terminal.noShellIntegrationNoMarks", "[scrollbar-marks]")
{
    auto mc = MockTerm { PageSize { LineCount(5), ColumnCount(40) }, LineCount(100) };

    SECTION("plain output")
    {
        test::printLines(mc, "plain", 50);
        CHECK(mc.terminal.commandBlocks().size() == 0);
        CHECK(mc.terminal.scrollbarMarkInputs().empty());
    }

    SECTION("a prompt nobody has run anything from")
    {
        mc.writeToScreen(test::PromptStart);
        CHECK(mc.terminal.scrollbarMarkInputs().empty());
    }
}

TEST_CASE("ScrollbarMarks.terminal.aFloodOfBlocksStaysBounded", "[scrollbar-marks]")
{
    // 3000 OSC 133 cycles leave at most maxRecords marks, every one inside the scrollbar's range, and the
    // ticks at most a track's worth.
    auto mc = MockTerm { PageSize { LineCount(24), ColumnCount(40) }, LineCount(10'000) };
    auto bytes = std::string {};
    for (auto const i: std::views::iota(0, 3000))
        bytes += test::commandRun(std::format("cmd {}", i), { "out" }, i % 2);
    mc.writeToScreen(bytes);

    auto const inputs = mc.terminal.scrollbarMarkInputs();
    CHECK(!inputs.empty());
    CHECK(inputs.size() <= mc.terminal.commandBlocks().limits().maxRecords);

    auto const rows = scrollbarRowsOf(mc.terminal);
    CHECK(std::ranges::all_of(inputs, [&](ScrollbarMarkInput const& input) {
        return input.visibleRow >= 0 && input.visibleRow < rows;
    }));
    CHECK(scrollbarMarks(inputs, rows, 600, AllScrollbarMarkSources).size() <= 600);
}

TEST_CASE("ScrollbarMarks.terminal.everyFoldCollapsedStillCountsVisibleRows", "[scrollbar-marks]")
{
    // A fold per command, every one collapsed, as `zM` leaves a long session: each tick still stands on the
    // visible rows above it, counted here one row at a time against the runs the folds hide.
    auto mc = MockTerm { PageSize { LineCount(10), ColumnCount(40) }, LineCount(1000) };
    for (auto const i: std::views::iota(0, 200))
        test::runCommand(mc, std::format("cmd {}", i), { "out", "more" }, i % 3 == 0 ? 1 : 0);
    mc.terminal.foldState().collapseAll(mc.terminal.foldRanges());

    auto const& grid = mc.terminal.primaryScreen().grid();
    auto const runs = mc.terminal.hiddenIntervals();
    auto const hidden = std::vector<HiddenInterval>(runs.begin(), runs.end());
    REQUIRE(hidden.size() >= 150);

    // Marks on a hidden output row, on the prompt line above it, and on a hidden row further down.
    auto const base = grid.stableLineIdOf(LineOffset(0));
    for (auto const id: { hidden[40].first, hidden[40].first - 1, hidden[120].last })
        mc.terminal.primaryScreen().setLogicalLineFlags(
            LineOffset::cast_from(id - base), LineFlag::UserMark, true);

    auto const floor = grid.stableRangeFloor();
    auto const isHidden = [&](int64_t id) {
        return hidingInterval(hidden, id).has_value();
    };
    auto const expectedRowOf = [&](int64_t id) {
        // The tick stands on the nearest drawn row at or above its target.
        auto const above = std::views::iota(floor, id + 1) | std::views::reverse;
        auto const shown = *std::ranges::find_if(above, [&](int64_t row) { return !isHidden(row); });
        return static_cast<int64_t>(std::ranges::count_if(std::views::iota(floor, shown),
                                                          [&](int64_t row) { return !isHidden(row); }));
    };

    auto const inputs = mc.terminal.scrollbarMarkInputs();
    REQUIRE(std::ranges::count(inputs, ScrollbarMarkKind::UserMark, &ScrollbarMarkInput::kind) == 3);
    REQUIRE(inputs.size() > 150);
    for (auto const& input: inputs)
    {
        INFO(std::format("{} at {}", input.kind, input.targetStableId));
        CHECK(input.visibleRow == expectedRowOf(input.targetStableId));
    }
    CHECK(inputs.back().visibleRow < scrollbarRowsOf(mc.terminal));
}

TEST_CASE("ScrollbarMarks.reveal.putsTheLineOnTopAndOpensItsFold", "[scrollbar-marks]")
{
    auto mc = MockTerm { PageSize { LineCount(5), ColumnCount(20) }, LineCount(100) };
    test::runCommand(mc, "ls", { "file1", "file2", "file3" }, 0);
    test::printLines(mc, "filler", 30);

    auto& terminal = mc.terminal;
    auto const& grid = terminal.primaryScreen().grid();
    auto const* ls = terminal.commandBlocks().lastFinished();
    REQUIRE(ls != nullptr);
    auto const head = ls->headStableId;
    REQUIRE(std::ranges::any_of(terminal.foldRanges(),
                                [&](FoldRange const& range) { return range.headStableId == head; }));

    SECTION("an expanded block")
    {
        CHECK(terminal.revealStableLineAtTop(head, grid.stableIdGeneration()).has_value());
        CHECK(grid.stableLineIdOf(terminal.viewport().topLine()) == head);
    }

    SECTION("a collapsed block is opened first, then brought to the top")
    {
        terminal.foldState().collapse(head);

        CHECK(terminal.revealStableLineAtTop(head, grid.stableIdGeneration()).has_value());
        CHECK_FALSE(terminal.foldState().isCollapsed(head));
        CHECK(grid.stableLineIdOf(terminal.viewport().topLine()) == head);
    }

    SECTION("a line a collapsed block hides: the fold opens, and that line is on top, not the head")
    {
        // A user mark inside a folded command's output: its tick stands on the head, a click goes to the
        // mark.
        auto const file2 = head + 2; // under the prompt row and "file1"
        REQUIRE(lineTextOf(terminal, file2).starts_with("file2"));
        terminal.foldState().collapse(head);
        REQUIRE(terminal.isLineHiddenByFold(*grid.lineOffsetOf(file2)));

        CHECK(terminal.revealStableLineAtTop(file2, grid.stableIdGeneration()).has_value());
        CHECK_FALSE(terminal.foldState().isCollapsed(head));
        CHECK(grid.stableLineIdOf(terminal.viewport().topLine()) == file2);
    }

    SECTION("a part-row smooth-scroll offset is dropped, so the line is not drawn part-way off the top")
    {
        terminal.viewport().setPixelOffset(5.0f);
        REQUIRE(terminal.revealStableLineAtTop(head, grid.stableIdGeneration()).has_value());
        CHECK(terminal.smoothScrollPixelOffset() == 0.0f);
    }

    SECTION("a glide in flight is stopped, so it does not carry the line off the top again")
    {
        startGlide(terminal);
        REQUIRE(terminal.revealStableLineAtTop(head, grid.stableIdGeneration()).has_value());
        CHECK_FALSE(terminal.isMomentumScrollActive());
    }

    SECTION("an id read in another generation is refused, and nothing moves")
    {
        auto const before = terminal.viewport().scrollOffset();
        auto const reveal = terminal.revealStableLineAtTop(head, grid.stableIdGeneration() + 1);
        REQUIRE_FALSE(reveal.has_value());
        CHECK(reveal.error() == RevealError::OtherGeneration);
        CHECK(terminal.viewport().scrollOffset() == before);
    }

    SECTION("the alternate screen cannot scroll: refused, and nothing moves")
    {
        terminal.foldState().collapse(head);
        mc.writeToScreen("\033[?1049h");
        auto const before = terminal.viewport().scrollOffset();

        auto const reveal = terminal.revealStableLineAtTop(head, grid.stableIdGeneration());
        REQUIRE_FALSE(reveal.has_value());
        CHECK(reveal.error() == RevealError::NotScrollable);
        CHECK(terminal.viewport().scrollOffset() == before);
        CHECK(terminal.foldState().isCollapsed(head));
    }
}

TEST_CASE("ScrollbarMarks.reveal.anEvictedLineIsRefused", "[scrollbar-marks]")
{
    auto mc = MockTerm { PageSize { LineCount(5), ColumnCount(20) }, LineCount(5) };
    test::runCommand(mc, "ls", { "file1" }, 0);
    auto const* ls = mc.terminal.commandBlocks().lastFinished();
    REQUIRE(ls != nullptr);
    auto const head = ls->headStableId;

    test::printLines(mc, "filler", 30);

    auto const& grid = mc.terminal.primaryScreen().grid();
    REQUIRE(head < grid.stableRangeFloor());
    auto const before = mc.terminal.viewport().scrollOffset();
    auto const reveal = mc.terminal.revealStableLineAtTop(head, grid.stableIdGeneration());
    REQUIRE_FALSE(reveal.has_value());
    CHECK(reveal.error() == RevealError::Evicted);
    CHECK(mc.terminal.viewport().scrollOffset() == before);
}

TEST_CASE("ScrollbarMarks.reveal.anotherPageOnDisplayIsRefused", "[scrollbar-marks]")
{
    // DECPCCM reset: page 3 is on display while the cursor writes to page 0. The viewport is the window onto
    // page 3, which has no history, so a tick drawn before page 3 came up is refused when clicked -- before
    // it opens the fold it would have revealed.
    auto mc = MockTerm { PageSize { LineCount(5), ColumnCount(30) }, LineCount(100) };
    test::startCommand(mc, "ls");
    test::printLines(mc, "out", 20);
    mc.writeToScreen(test::commandEnd(0));
    test::startCommand(mc, "make");
    test::printLines(mc, "more", 20);
    auto const* ls = mc.terminal.commandBlocks().lastFinished();
    REQUIRE(ls != nullptr);
    auto const head = ls->headStableId;
    REQUIRE(std::ranges::any_of(mc.terminal.foldRanges(),
                                [&](FoldRange const& range) { return range.headStableId == head; }));
    mc.terminal.foldState().collapse(head);
    auto const& grid = mc.terminal.primaryScreen().grid();
    auto const generation = grid.stableIdGeneration();

    test::moveDisplayOffTheCursorsPage(mc);
    auto const topLine = mc.terminal.viewport().topLine();

    auto const reveal = mc.terminal.revealStableLineAtTop(head, generation);
    REQUIRE_FALSE(reveal.has_value());
    CHECK(reveal.error() == RevealError::NotScrollable);
    CHECK(mc.terminal.viewport().topLine() == topLine);
    CHECK(mc.terminal.foldState().isCollapsed(head));
}

TEST_CASE("ScrollbarMarks.reveal.aTickIsOfferedExactlyWhereAClickLands", "[scrollbar-marks]")
{
    // DECPCCM reset lets the page on display and the cursor's page part ways, in either direction. The ticks
    // and the reveal a click on one performs ask the same question, so no tick is offered that a click would
    // refuse, and no click lands where no tick was offered.
    auto mc = MockTerm { PageSize { LineCount(5), ColumnCount(30) }, LineCount(100) };
    test::runCommand(mc, "ls", { "file1", "file2" }, 0);
    test::printLines(mc, "filler", 20);
    auto const* ls = mc.terminal.commandBlocks().lastFinished();
    REQUIRE(ls != nullptr);
    auto const head = ls->headStableId;
    auto const generation = mc.terminal.primaryScreen().grid().stableIdGeneration();

    SECTION("both on page 0: offered, and a click lands")
    {
        CHECK_FALSE(mc.terminal.scrollbarMarkInputs().empty());
        CHECK(mc.terminal.revealStableLineAtTop(head, generation).has_value());
    }

    SECTION("the cursor on another page, page 0 on display: offered, and a click lands")
    {
        // The viewport is the window onto the page on display, wherever the cursor writes.
        test::moveCursorOffTheDisplayedPage(mc);

        CHECK_FALSE(mc.terminal.scrollbarMarkInputs().empty());
        CHECK(mc.terminal.revealStableLineAtTop(head, generation).has_value());
    }

    SECTION("the cursor back on page 0, another page on display: neither")
    {
        test::moveDisplayOffTheCursorsPage(mc);
        auto const before = mc.terminal.viewport().scrollOffset();

        CHECK(mc.terminal.scrollbarMarkInputs().empty());
        auto const reveal = mc.terminal.revealStableLineAtTop(head, generation);
        REQUIRE_FALSE(reveal.has_value());
        CHECK(reveal.error() == RevealError::NotScrollable);
        CHECK(mc.terminal.viewport().scrollOffset() == before);
    }
}

TEST_CASE("ScrollbarMarks.reveal.aRevealThatLeavesTheOffsetAloneStillRedraws", "[scrollbar-marks]")
{
    // The line is on top already, so the reveal moves no scroll offset, and the viewport, which asks for a
    // frame when it moves, asks for none. What the reveal drops shows all the same: a part-row offset the
    // last frame drew, and a glide whose frames stop. Without a frame of its own, an idle terminal went on
    // showing both.
    auto mc = EventCountingTerm { PageSize { LineCount(5), ColumnCount(20) }, LineCount(100) };
    test::runCommand(mc, "ls", { "file1", "file2", "file3" }, 0);
    test::printLines(mc, "filler", 30);
    auto& terminal = mc.terminal;
    auto const* ls = terminal.commandBlocks().lastFinished();
    REQUIRE(ls != nullptr);
    auto const head = ls->headStableId;
    auto const generation = terminal.primaryScreen().grid().stableIdGeneration();
    REQUIRE(terminal.revealStableLineAtTop(head, generation).has_value());

    SECTION("a part-row offset")
    {
        terminal.viewport().setPixelOffset(5.0f);
    }

    SECTION("a glide in flight")
    {
        startGlide(terminal);
    }

    auto const offset = terminal.viewport().scrollOffset();
    auto const redraws = mc.redraws;
    REQUIRE(terminal.revealStableLineAtTop(head, generation).has_value());
    CHECK(terminal.viewport().scrollOffset() == offset);
    CHECK(mc.redraws > redraws);
}

TEST_CASE("ScrollbarMarks.reveal.theStickyHeaderJumpLandsTheSameWay", "[scrollbar-marks]")
{
    // A press on the sticky header and a click on a tick take one reveal path, so the header's jump drops a
    // part-row smooth-scroll offset too, and stops a glide in flight.
    auto mc = MockTerm { PageSize { LineCount(5), ColumnCount(20) }, LineCount(100) };
    test::runCommand(mc, "ls", { "file1", "file2", "file3" }, 0);
    test::printLines(mc, "filler", 30);
    auto const* ls = mc.terminal.commandBlocks().lastFinished();
    REQUIRE(ls != nullptr);

    startGlide(mc.terminal);
    mc.terminal.viewport().setPixelOffset(5.0f);
    REQUIRE(mc.terminal.scrollToCommandBlockHead(ls->id).has_value());
    CHECK(mc.terminal.smoothScrollPixelOffset() == 0.0f);
    CHECK_FALSE(mc.terminal.isMomentumScrollActive());
    CHECK(mc.terminal.primaryScreen().grid().stableLineIdOf(mc.terminal.viewport().topLine())
          == ls->headStableId);
}
