// SPDX-License-Identifier: Apache-2.0
#include <vtbackend/Folding.hpp>
#include <vtbackend/MockTerm.hpp>
#include <vtbackend/StatusLineBuilder.hpp>
#include <vtbackend/Terminal.hpp>

#include <crispy/Utils.hpp>

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <format>
#include <ranges>
#include <set>
#include <string>
#include <vector>

using namespace vtbackend;
using namespace std::chrono_literals;

namespace chrono = std::chrono;

namespace
{

/// The stable id the fake below gives the line the scan starts at. Deliberately not zero and not round,
/// so a test that accidentally compares against a bare index fails instead of passing by coincidence.
constexpr auto BottomStableId = int64_t { 1000 };

/// A fold line source backed by a plain vector — the whole point of the FoldLineSource seam. Lines are
/// given in the order the scan walks them: the bottom line first, then upwards, one entry per LOGICAL
/// line. Stable ids decrease upwards, exactly as Grid hands them out.
class FakeLines final: public FoldLineSource
{
  public:
    struct Entry
    {
        LineFlags flags;
        /// How many PHYSICAL rows this logical line occupies -- 1 unless a wrap chopped it up.
        int rowCount = 1;
    };

    explicit FakeLines(std::vector<Entry> lines): _lines { std::move(lines) }
    {
        // Ids decrease upwards, and a wrapped line consumes as many of them as it has rows: the head of
        // the line at index n sits that many rows above the head of the line at index n-1.
        auto id = BottomStableId;
        for (auto const& entry: _lines)
        {
            _lastPhysicalIds.push_back(id);
            id -= entry.rowCount;
            _headIds.push_back(id + 1);
        }
    }

    /// Convenience for the common all-unwrapped case.
    explicit FakeLines(std::vector<LineFlags> const& flags): FakeLines { toEntries(flags) } {}

    [[nodiscard]] bool hasLineAt(size_t index) const override { return index < _lines.size(); }
    [[nodiscard]] LineFlags flagsAt(size_t index) const override { return _lines.at(index).flags; }

    [[nodiscard]] int64_t stableIdAt(size_t index) const override { return _headIds.at(index); }

    [[nodiscard]] int64_t lastPhysicalStableIdAt(size_t index) const override
    {
        return _lastPhysicalIds.at(index);
    }

  private:
    [[nodiscard]] static std::vector<Entry> toEntries(std::vector<LineFlags> const& flags)
    {
        auto entries = std::vector<Entry> {};
        entries.reserve(flags.size());
        for (auto const& f: flags)
            entries.push_back(Entry { .flags = f });
        return entries;
    }

    std::vector<Entry> _lines;
    std::vector<int64_t> _headIds;
    std::vector<int64_t> _lastPhysicalIds;
};

/// The stable id of the line @p index lines above the bottom one — the test's own mirror of the fake's
/// mapping, so an expectation reads as "three lines up" rather than as an opaque number.
constexpr int64_t idAt(size_t index) noexcept
{
    return BottomStableId - static_cast<int64_t>(index);
}

} // namespace

// {{{ computeFoldRanges

TEST_CASE("Folding.scan.oneBlock", "[folding]")
{
    // The layout a shell leaves behind for `ls`, walked from the cursor upwards. The bottom line carries
    // BOTH the finished command's CommandEnd and the new prompt's Marked, because precmd emits OSC 133;D
    // and ;A back to back and then prints the prompt onto that line.
    auto const lines = FakeLines { {
        LineFlags { LineFlag::CommandEnd, LineFlag::Marked }, // 0: the NEW prompt
        LineFlags {},                                         // 1: "file2"
        LineFlags { LineFlag::OutputStart },                  // 2: "file1"
        LineFlags { LineFlag::Marked },                       // 3: "$ ls"
    } };

    auto const ranges = computeFoldRanges(lines, MaxFoldScanLines);

    REQUIRE(ranges.size() == 1);
    // The fold hangs off the prompt, and hides exactly the two output lines -- not the line that closed
    // the command, which is the next prompt.
    CHECK(ranges[0].headStableId == idAt(3));
    CHECK(ranges[0].firstStableId == idAt(2));
    CHECK(ranges[0].lastStableId == idAt(1));
}

TEST_CASE("Folding.scan.commandEndLineIsNeverHidden", "[folding]")
{
    // The shell closed the command but has not printed its next prompt yet, so the cursor is standing on
    // the ;D line. Folding it away would hide the row the user is about to type into -- so the range
    // stops above it, at the cost of one visible row.
    auto const lines = FakeLines { {
        LineFlags { LineFlag::CommandEnd },  // 0: where ;D landed, and where the cursor is
        LineFlags {},                        // 1: output
        LineFlags { LineFlag::OutputStart }, // 2: output
        LineFlags { LineFlag::Marked },      // 3: "$ ls"
    } };

    auto const ranges = computeFoldRanges(lines, MaxFoldScanLines);

    REQUIRE(ranges.size() == 1);
    CHECK(ranges[0].headStableId == idAt(3));
    CHECK(ranges[0].firstStableId == idAt(2));
    CHECK(ranges[0].lastStableId == idAt(1));
}

TEST_CASE("Folding.scan.multiLinePrompt", "[folding]")
{
    // A lavish two-line prompt: Marked sits on the FIRST of them, and the fold hangs off that line while
    // the continuation stays visible -- prompt lines are never hidden.
    auto const lines = FakeLines { {
        LineFlags { LineFlag::CommandEnd, LineFlag::Marked }, // 0: the NEW prompt
        LineFlags {},                                         // 1: output
        LineFlags { LineFlag::OutputStart },                  // 2: output
        LineFlags { LineFlag::PromptEnd },                    // 3: "> ls"  (prompt, 2nd line)
        LineFlags { LineFlag::Marked },                       // 4: "~/src" (prompt, 1st line)
    } };

    auto const ranges = computeFoldRanges(lines, MaxFoldScanLines);

    REQUIRE(ranges.size() == 1);
    CHECK(ranges[0].headStableId == idAt(4));
    CHECK(ranges[0].firstStableId == idAt(2));
    CHECK(ranges[0].lastStableId == idAt(1));
}

TEST_CASE("Folding.scan.chainedBlocks", "[folding]")
{
    // Two commands in a row. The line between them carries the older block's CommandEnd AND the newer
    // block's Marked, which the walk has to chain through rather than step over.
    auto const lines = FakeLines { {
        LineFlags { LineFlag::CommandEnd, LineFlag::Marked }, // 0: the NEW prompt
        LineFlags { LineFlag::OutputStart },                  // 1: `pwd` output
        LineFlags { LineFlag::Marked, LineFlag::CommandEnd }, // 2: "$ pwd", closing `ls`
        LineFlags {},                                         // 3: `ls` output
        LineFlags { LineFlag::OutputStart },                  // 4: `ls` output
        LineFlags { LineFlag::Marked },                       // 5: "$ ls"
    } };

    auto const ranges = computeFoldRanges(lines, MaxFoldScanLines);

    REQUIRE(ranges.size() == 2);
    // Most recent first.
    CHECK(ranges[0].headStableId == idAt(2));
    CHECK(ranges[0].firstStableId == idAt(1));
    CHECK(ranges[0].lastStableId == idAt(1));
    CHECK(ranges[1].headStableId == idAt(5));
    CHECK(ranges[1].firstStableId == idAt(4));
    CHECK(ranges[1].lastStableId == idAt(3));
}

TEST_CASE("Folding.scan.wrappedOutputHidesAllItsRows", "[folding]")
{
    // A single logical output line the window was too narrow for, chopped into three physical rows. A
    // range that stopped at the head would leave the two continuations on screen -- an orphaned tail
    // below a collapsed block.
    auto const lines = FakeLines { std::vector<FakeLines::Entry> {
        { .flags = LineFlags { LineFlag::CommandEnd, LineFlag::Marked } }, // 0: the NEW prompt, 1 row
        { .flags = LineFlags { LineFlag::OutputStart }, .rowCount = 3 },   // 1: one long output line
        { .flags = LineFlags { LineFlag::Marked } },                       // 2: "$ ls"
    } };

    auto const ranges = computeFoldRanges(lines, MaxFoldScanLines);

    REQUIRE(ranges.size() == 1);
    CHECK(ranges[0].headStableId == BottomStableId - 4);
    // All three physical rows of the wrapped line, head through tail.
    CHECK(ranges[0].firstStableId == BottomStableId - 3);
    CHECK(ranges[0].lastStableId == BottomStableId - 1);
}

TEST_CASE("Folding.scan.commandThatPrintedNothing", "[folding]")
{
    // `cd /tmp` prints nothing: there is no output line to hide, so there is no fold. A marker offering
    // to hide nothing is worse than no marker.
    auto const lines = FakeLines { {
        LineFlags { LineFlag::CommandEnd, LineFlag::Marked }, // 0: the NEW prompt
        LineFlags { LineFlag::Marked },                       // 1: "$ cd /tmp"
    } };

    CHECK(computeFoldRanges(lines, MaxFoldScanLines).empty());
}

TEST_CASE("Folding.scan.outputStartOnTheCommandLine", "[folding]")
{
    // A shell that emits OSC 133;C before the echoed newline lands OutputStart on the command line
    // itself. That line carries Marked, so it must not be hidden -- and with nothing else to hide, the
    // block is not foldable at all.
    auto const lines = FakeLines { {
        LineFlags { LineFlag::CommandEnd, LineFlag::Marked },  // 0: the NEW prompt
        LineFlags { LineFlag::Marked, LineFlag::OutputStart }, // 1: "$ printf hi"
    } };

    CHECK(computeFoldRanges(lines, MaxFoldScanLines).empty());
}

TEST_CASE("Folding.scan.unfinishedBlockIsNotFoldable", "[folding]")
{
    // A command still running: no CommandEnd anywhere, so the walk never opens a block. This is what
    // keeps the cursor from ever standing inside a collapsed range.
    auto const lines = FakeLines { {
        LineFlags {},                        // 0: output still arriving
        LineFlags { LineFlag::OutputStart }, // 1
        LineFlags { LineFlag::Marked },      // 2: "$ tail -f log"
    } };

    CHECK(computeFoldRanges(lines, MaxFoldScanLines).empty());
}

TEST_CASE("Folding.scan.promptScrolledOutIsDropped", "[folding]")
{
    // The output is reachable but the prompt it hangs off is not: there is no row to draw a marker on,
    // so hiding the output would leave the user no way to get it back.
    auto const lines = FakeLines { {
        LineFlags { LineFlag::CommandEnd, LineFlag::Marked }, // 0: the NEW prompt
        LineFlags {},                                         // 1: output
        LineFlags {},                                         // 2: output, and the scrollback ends here
    } };

    CHECK(computeFoldRanges(lines, MaxFoldScanLines).empty());
}

TEST_CASE("Folding.scan.respectsScanBudget", "[folding]")
{
    // The budget cuts the walk off before it reaches the prompt, and a fold with no head is dropped --
    // the walk runs under the terminal lock, so the bound is real rather than defensive.
    auto const lines = FakeLines { {
        LineFlags { LineFlag::CommandEnd, LineFlag::Marked },
        LineFlags {},
        LineFlags { LineFlag::OutputStart },
        LineFlags { LineFlag::Marked },
    } };

    CHECK(computeFoldRanges(lines, 3).empty());
    CHECK(computeFoldRanges(lines, 4).size() == 1);
}

TEST_CASE("Folding.scan.noMarksAtAll", "[folding]")
{
    // A shell with no OSC 133 integration: a flags-only walk that finds nothing and costs nothing.
    auto const lines = FakeLines { std::vector<LineFlags>(64, LineFlags {}) };

    CHECK(computeFoldRanges(lines, MaxFoldScanLines).empty());
}

// }}}
// {{{ FoldState

TEST_CASE("Folding.state.toggle", "[folding]")
{
    auto state = FoldState {};
    CHECK(state.empty());

    CHECK(state.toggle(42) == FoldVisibility::Collapsed);
    CHECK(state.isCollapsed(42));
    CHECK(!state.empty());

    CHECK(state.toggle(42) == FoldVisibility::Expanded);
    CHECK(!state.isCollapsed(42));
    CHECK(state.empty());
}

TEST_CASE("Folding.state.collapseAllAndExpandAll", "[folding]")
{
    auto const ranges = std::vector<FoldRange> {
        { .headStableId = 10, .firstStableId = 11, .lastStableId = 12 },
        { .headStableId = 20, .firstStableId = 21, .lastStableId = 22 },
    };

    auto state = FoldState {};
    auto const before = state.revision();
    state.collapseAll(ranges);
    CHECK(state.isCollapsed(10));
    CHECK(state.isCollapsed(20));
    // The revision must move, or every cache keyed on it keeps showing what was there before.
    CHECK(state.revision() != before);

    // A second sweep changes nothing, and says so by leaving the revision alone.
    auto const afterFirst = state.revision();
    state.collapseAll(ranges);
    CHECK(state.revision() == afterFirst);

    state.expandAll();
    CHECK(state.empty());
    CHECK(state.revision() != afterFirst);
}

TEST_CASE("Folding.state.pruneDropsEvictedHeads", "[folding]")
{
    // Without pruning the set grows without bound over a long session, and an id reused by a later
    // generation would come back collapsed.
    auto state = FoldState {};
    state.collapse(10);
    state.collapse(20);
    state.collapse(30);

    state.prune(20);

    CHECK(!state.isCollapsed(10));
    CHECK(state.isCollapsed(20)); // the floor itself is still addressable
    CHECK(state.isCollapsed(30));
}

// }}}
// {{{ hiddenIntervals

TEST_CASE("Folding.hidden.onlyCollapsedRangesHide", "[folding]")
{
    auto const ranges = std::vector<FoldRange> {
        { .headStableId = 10, .firstStableId = 11, .lastStableId = 13 },
        { .headStableId = 20, .firstStableId = 21, .lastStableId = 23 },
    };

    auto state = FoldState {};
    CHECK(hiddenIntervals(ranges, state).empty());

    state.collapse(20);
    auto const hidden = hiddenIntervals(ranges, state);
    REQUIRE(hidden.size() == 1);
    CHECK(hidden[0] == HiddenInterval { .first = 21, .last = 23 });
}

TEST_CASE("Folding.hidden.abuttingRangesMerge", "[folding]")
{
    // Two collapsed blocks with no visible line between them describe one run, so the projection's
    // binary search stays a search.
    auto const ranges = std::vector<FoldRange> {
        { .headStableId = 30, .firstStableId = 31, .lastStableId = 34 },
        { .headStableId = 20, .firstStableId = 25, .lastStableId = 30 },
    };

    auto state = FoldState {};
    state.collapseAll(ranges);

    auto const hidden = hiddenIntervals(ranges, state);
    REQUIRE(hidden.size() == 1);
    CHECK(hidden[0] == HiddenInterval { .first = 25, .last = 34 });
}

// }}}
// {{{ visible-line arithmetic

namespace
{
/// Two collapsed runs with visible ids on either side and between them: 10..12 and 20..24.
///
/// Visible ids around them, ascending: 9, 13, 14, ..., 19, 25, 26, ...
[[nodiscard]] std::vector<HiddenInterval> twoRuns()
{
    return { HiddenInterval { .first = 10, .last = 12 }, HiddenInterval { .first = 20, .last = 24 } };
}
} // namespace

TEST_CASE("Folding.visible.snapLeavesAVisibleIdAlone", "[folding]")
{
    auto const hidden = twoRuns();
    CHECK(snapToVisibleId(hidden, 9, VerticalDirection::Up) == 9);
    CHECK(snapToVisibleId(hidden, 9, VerticalDirection::Down) == 9);
    CHECK(snapToVisibleId(hidden, 13, VerticalDirection::Up) == 13);
    CHECK(snapToVisibleId({}, 42, VerticalDirection::Down) == 42);
}

TEST_CASE("Folding.visible.snapLeavesARunByTheNearestEdge", "[folding]")
{
    auto const hidden = twoRuns();

    // Out through the top of the run, and out through the bottom -- one step either way, because
    // hiddenIntervals() has already merged anything that touches.
    CHECK(snapToVisibleId(hidden, 11, VerticalDirection::Up) == 9);
    CHECK(snapToVisibleId(hidden, 11, VerticalDirection::Down) == 13);
    CHECK(snapToVisibleId(hidden, 22, VerticalDirection::Up) == 19);
    CHECK(snapToVisibleId(hidden, 22, VerticalDirection::Down) == 25);
}

TEST_CASE("Folding.visible.advanceCountsVisibleRowsOnly", "[folding]")
{
    auto const hidden = twoRuns();
    auto constexpr Floor = int64_t { 0 };
    auto constexpr Ceil = int64_t { 100 };

    // From 9 downwards the visible ids are 13, 14, 15, ... -- the run 10..12 is not among them, so one
    // step lands past it rather than inside it. This is the whole point: `3j` means three rows the user
    // can SEE.
    CHECK(advanceVisibleId(hidden, 9, 1, VerticalDirection::Down, Floor, Ceil) == 13);
    CHECK(advanceVisibleId(hidden, 9, 2, VerticalDirection::Down, Floor, Ceil) == 14);
    CHECK(advanceVisibleId(hidden, 9, 7, VerticalDirection::Down, Floor, Ceil) == 19);
    // ...and the next step crosses the second run whole.
    CHECK(advanceVisibleId(hidden, 9, 8, VerticalDirection::Down, Floor, Ceil) == 25);
    CHECK(advanceVisibleId(hidden, 9, 9, VerticalDirection::Down, Floor, Ceil) == 26);

    // Upwards, the mirror image.
    CHECK(advanceVisibleId(hidden, 26, 1, VerticalDirection::Up, Floor, Ceil) == 25);
    CHECK(advanceVisibleId(hidden, 26, 2, VerticalDirection::Up, Floor, Ceil) == 19);
    CHECK(advanceVisibleId(hidden, 26, 8, VerticalDirection::Up, Floor, Ceil) == 13);
    CHECK(advanceVisibleId(hidden, 26, 9, VerticalDirection::Up, Floor, Ceil) == 9);
}

TEST_CASE("Folding.visible.advanceWithNothingHiddenIsPlainArithmetic", "[folding]")
{
    CHECK(advanceVisibleId({}, 5, 3, VerticalDirection::Down, 0, 100) == 8);
    CHECK(advanceVisibleId({}, 5, 3, VerticalDirection::Up, 0, 100) == 2);
    CHECK(advanceVisibleId({}, 5, 0, VerticalDirection::Down, 0, 100) == 5);
}

TEST_CASE("Folding.visible.advanceClampsToTheAddressableGrid", "[folding]")
{
    auto const hidden = twoRuns();

    // A motion that runs off either end stops there rather than naming a row the grid no longer holds.
    CHECK(advanceVisibleId(hidden, 26, 1000, VerticalDirection::Down, 0, 30) == 30);
    CHECK(advanceVisibleId(hidden, 9, 1000, VerticalDirection::Up, 5, 30) == 5);

    // A bound that itself sits inside a collapsed run is not where the walk stops: 22 is hidden, and
    // stopping on it would put the cursor on a row drawn nowhere. It stops on the last VISIBLE id
    // before the bound instead, on both ends.
    CHECK(advanceVisibleId(hidden, 9, 1000, VerticalDirection::Down, 0, 22) == 19);
    CHECK(advanceVisibleId(hidden, 26, 1000, VerticalDirection::Up, 11, 30) == 13);
}

TEST_CASE("Folding.visible.advanceSnapsAHiddenStartingPointFirst", "[folding]")
{
    auto const hidden = twoRuns();

    // A cursor the user collapsed a block underneath starts inside a run. It leaves in the direction of
    // travel, and only then counts -- so the first step is never spent climbing back out.
    CHECK(advanceVisibleId(hidden, 11, 0, VerticalDirection::Down, 0, 100) == 13);
    CHECK(advanceVisibleId(hidden, 11, 0, VerticalDirection::Up, 0, 100) == 9);
    CHECK(advanceVisibleId(hidden, 11, 1, VerticalDirection::Down, 0, 100) == 14);
    CHECK(advanceVisibleId(hidden, 11, 1, VerticalDirection::Up, 0, 100) == 8);
}

// }}}
// {{{ projectRows

TEST_CASE("Folding.project.topRowPlacesTheListTheWayGridRenderDraws", "[folding]")
{
    // The rule Grid::render and Viewport's coordinate translation BOTH read: those two disagreeing is
    // what misplaces a selection, so it is stated once and tested here rather than at either of them.

    // A page-full starts at the top, and so does a short one -- the walk ran out of grid above it, and
    // the page is short at the BOTTOM instead.
    CHECK(foldedRowsTopRow(LineCount(10), 10) == LineOffset(0));
    CHECK(foldedRowsTopRow(LineCount(10), 3) == LineOffset(0));
    CHECK(foldedRowsTopRow(LineCount(10), 0) == LineOffset(0));

    // Rows BEYOND the page are the smooth-scrolling ones and belong ABOVE it, so the last row still
    // lands on the bottom of the page however many extra ones precede it.
    CHECK(foldedRowsTopRow(LineCount(10), 11) == LineOffset(-1));
    CHECK(foldedRowsTopRow(LineCount(10), 14) == LineOffset(-4));
}

TEST_CASE("Folding.visible.distanceCountsOnlyVisibleIds", "[folding]")
{
    auto const hidden = twoRuns();

    // Nothing hidden in between, so the plain difference.
    CHECK(visibleDistance({}, 5, 17) == 12);
    CHECK(visibleDistance(hidden, 5, 9) == 4);

    // [9, 13) spans the whole of the run 10..12, so only id 9 is visible in it.
    CHECK(visibleDistance(hidden, 9, 13) == 1);

    // Both runs crossed: 26 - 9 = 17 ids, of which 3 + 5 are hidden.
    CHECK(visibleDistance(hidden, 9, 26) == 9);

    // Partial overlaps, from inside a run and to inside one.
    CHECK(visibleDistance(hidden, 11, 14) == 1); // 11, 12 hidden; 13 visible
    CHECK(visibleDistance(hidden, 18, 22) == 2); // 18, 19 visible; 20, 21 hidden

    // Empty and reversed ranges. Symmetric so no caller has to order its arguments.
    CHECK(visibleDistance(hidden, 14, 14) == 0);
    CHECK(visibleDistance(hidden, 26, 9) == -9);
}

TEST_CASE("Folding.project.unfoldedIsTheLinearWalk", "[folding]")
{
    // Nothing collapsed: the projection must be exactly what an unfolded viewport already draws.
    auto const rows = projectRows({}, /*stableBase*/ 0, LineOffset(4), LineCount(5), LineOffset(0));

    REQUIRE(rows.size() == 5);
    for (auto i = 0; i < 5; ++i)
        CHECK(rows[static_cast<size_t>(i)] == LineOffset(i));
}

TEST_CASE("Folding.project.collapsedRunIsSkippedAndBackfilled", "[folding]")
{
    // stableBase 0, so ids and LineOffsets coincide and the expectation reads directly.
    // Grid lines -3..4; the fold hangs off line 0 and hides lines 1..3.
    auto const ranges = std::vector<FoldRange> {
        { .headStableId = 0, .firstStableId = 1, .lastStableId = 3 },
    };
    auto state = FoldState {};
    state.collapse(0);

    auto const rows = projectRows(
        hiddenIntervals(ranges, state), /*stableBase*/ 0, LineOffset(4), LineCount(5), LineOffset(-3));

    // The three hidden rows are freed and filled from further up the history.
    REQUIRE(rows.size() == 5);
    CHECK(rows[0] == LineOffset(-3));
    CHECK(rows[1] == LineOffset(-2));
    CHECK(rows[2] == LineOffset(-1));
    CHECK(rows[3] == LineOffset(0));
    CHECK(rows[4] == LineOffset(4));
}

TEST_CASE("Folding.project.anExpandedFoldHidesNothing", "[folding]")
{
    // An expanded fold is still a fold, but it takes no rows out of the projection -- which is why
    // the projection alone cannot tell the gutter what to draw, and why Terminal::foldMarkerAt() is
    // the one query that answers for expanded and collapsed blocks alike.
    auto const ranges = std::vector<FoldRange> {
        { .headStableId = 0, .firstStableId = 1, .lastStableId = 3 },
    };

    auto const rows = projectRows(
        hiddenIntervals(ranges, FoldState {}), /*stableBase*/ 0, LineOffset(4), LineCount(5), LineOffset(0));

    REQUIRE(rows.size() == 5);
    for (auto i = 0; i < 5; ++i)
        CHECK(rows[static_cast<size_t>(i)] == LineOffset(i));
}

TEST_CASE("Folding.project.stopsAtTheAddressableTop", "[folding]")
{
    // A short scrollback yields fewer rows than asked for; the caller pads the top exactly as an
    // unfolded viewport already shows blank rows above a short history.
    auto const rows = projectRows({}, /*stableBase*/ 0, LineOffset(2), LineCount(10), LineOffset(0));

    REQUIRE(rows.size() == 3);
    CHECK(rows[0] == LineOffset(0));
    CHECK(rows[2] == LineOffset(2));
}

TEST_CASE("Folding.project.stableBaseOffsetsTheIds", "[folding]")
{
    // The realistic case: the grid has scrolled, so a line's stable id is nothing like its LineOffset.
    // The fold hides ids 101..103, which at base 100 are lines 1..3.
    auto const ranges = std::vector<FoldRange> {
        { .headStableId = 100, .firstStableId = 101, .lastStableId = 103 },
    };
    auto state = FoldState {};
    state.collapse(100);

    auto const rows = projectRows(
        hiddenIntervals(ranges, state), /*stableBase*/ 100, LineOffset(4), LineCount(3), LineOffset(-3));

    REQUIRE(rows.size() == 3);
    CHECK(rows[0] == LineOffset(-1));
    CHECK(rows[1] == LineOffset(0));
    CHECK(rows[2] == LineOffset(4));
}

TEST_CASE("Folding.project.everythingVisibleCollapsed", "[folding]")
{
    // The whole reachable grid is hidden: the projection comes back empty rather than looping or
    // inventing rows.
    auto const ranges = std::vector<FoldRange> {
        { .headStableId = -1, .firstStableId = 0, .lastStableId = 4 },
    };
    auto state = FoldState {};
    state.collapse(-1);

    auto const rows = projectRows(
        hiddenIntervals(ranges, state), /*stableBase*/ 0, LineOffset(4), LineCount(5), LineOffset(0));

    CHECK(rows.empty());
}

TEST_CASE("Folding.project.zeroRowsAndInvertedRange", "[folding]")
{
    CHECK(projectRows({}, 0, LineOffset(4), LineCount(0), LineOffset(0)).empty());
    CHECK(projectRows({}, 0, LineOffset(-1), LineCount(5), LineOffset(0)).empty());
}

// }}}

