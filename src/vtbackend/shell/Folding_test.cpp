// SPDX-License-Identifier: Apache-2.0
#include <vtbackend/MockTerm.hpp>
#include <vtbackend/StatusLineBuilder.hpp>
#include <vtbackend/Terminal.hpp>
#include <vtbackend/shell/Folding.hpp>

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

// }}}
// {{{ Wired to a live terminal

namespace
{

/// Runs one shell command through a terminal exactly as a shell with OSC 133 integration would: prompt
/// start, prompt end, the echoed command line, output start, the output itself, and the command end
/// that a precmd hook emits at the top of the NEXT prompt.
void runCommand(MockTerm<>& mc, std::string const& command, std::vector<std::string> const& output)
{
    mc.writeToScreen("\033]133;A\033\\");
    mc.writeToScreen("$ ");
    mc.writeToScreen("\033]133;B\033\\");
    mc.writeToScreen(command + "\r\n");
    mc.writeToScreen("\033]133;C\033\\");
    for (auto const& line: output)
        mc.writeToScreen(line + "\r\n");
    mc.writeToScreen("\033]133;D;0\033\\");
}

/// The text of each row the viewport currently shows, top first, trailing blanks dropped -- what
/// folding is ultimately about.
std::vector<std::string> visibleRows(Terminal& terminal)
{
    auto rows = std::vector<std::string> {};
    auto const& screen = terminal.primaryScreen();
    for (auto y = 0; y < unbox<int>(terminal.pageSize().lines); ++y)
    {
        auto const line = terminal.viewport().translateScreenToGridCoordinate(
            CellLocation { .line = LineOffset(y), .column = ColumnOffset(0) });
        auto text = screen.grid().lineText(line.line);
        text.resize(crispy::trimRight(text).size());
        rows.push_back(std::move(text));
    }
    return rows;
}

} // namespace

TEST_CASE("Folding.terminal.rangesComeFromTheMarks", "[folding]")
{
    auto mc = MockTerm { PageSize { LineCount(10), ColumnCount(20) } };
    runCommand(mc, "ls", { "file1", "file2" });

    auto const ranges = mc.terminal.foldRanges();
    REQUIRE(ranges.size() == 1);

    // The head is the prompt line, and the hidden rows are the two output lines -- neither the prompt
    // above them nor the line the shell closed the command on.
    auto const& grid = mc.terminal.primaryScreen().grid();
    CHECK(ranges[0].headStableId == grid.stableLineIdOf(LineOffset(0)));
    CHECK(ranges[0].firstStableId == grid.stableLineIdOf(LineOffset(1)));
    CHECK(ranges[0].lastStableId == grid.stableLineIdOf(LineOffset(2)));
}

TEST_CASE("Folding.terminal.nothingCollapsedProjectsNothing", "[folding]")
{
    // The fast path: with no fold collapsed the contiguous page IS the projection, so none is built and
    // every consumer keeps the arithmetic it always had.
    auto mc = MockTerm { PageSize { LineCount(10), ColumnCount(20) } };
    runCommand(mc, "ls", { "file1", "file2" });

    CHECK(mc.terminal.foldProjection().empty());
    CHECK(mc.terminal.foldProjection().empty());
    CHECK(mc.terminal.hiddenLineCount() == LineCount(0));

    // ... and translation is the plain subtraction it has always been.
    CHECK(mc.terminal.viewport().translateScreenToGridCoordinate(
              CellLocation { .line = LineOffset(3), .column = ColumnOffset(1) })
          == CellLocation { .line = LineOffset(3), .column = ColumnOffset(1) });
}

TEST_CASE("Folding.terminal.collapsingHidesTheOutput", "[folding]")
{
    auto mc = MockTerm { PageSize { LineCount(10), ColumnCount(20) } };
    runCommand(mc, "ls", { "file1", "file2" });

    auto const before = visibleRows(mc.terminal);
    REQUIRE(before[0] == "$ ls");
    REQUIRE(before[1] == "file1");
    REQUIRE(before[2] == "file2");

    auto const ranges = mc.terminal.foldRanges();
    REQUIRE(ranges.size() == 1);
    mc.terminal.foldState().collapse(ranges[0].headStableId);

    CHECK(mc.terminal.hiddenLineCount() == LineCount(2));

    auto const after = visibleRows(mc.terminal);
    // The prompt stays; its output is gone and the rows below have moved up.
    CHECK(after[0] == "$ ls");
    CHECK(after[1] != "file1");
    CHECK(after[2] != "file2");
}

TEST_CASE("Folding.terminal.translationRoundTripsUnderFolds", "[folding]")
{
    // The property that matters most: the projection the render pass draws from and the coordinate
    // translation every selection goes through must agree, or a drag selects rows it does not highlight.
    auto mc = MockTerm { PageSize { LineCount(10), ColumnCount(20) } };
    runCommand(mc, "ls", { "file1", "file2" });

    auto const ranges = mc.terminal.foldRanges();
    REQUIRE(ranges.size() == 1);
    mc.terminal.foldState().collapse(ranges[0].headStableId);

    auto const projection = mc.terminal.foldProjection();
    REQUIRE(!projection.empty());

    // The projection is BOTTOM-aligned: its last row is the last row of the page, and when collapsed
    // folds hide more than the history can backfill the rows above it are blank. Screen row and
    // projection index therefore differ by that offset -- which is exactly the thing Grid::render and
    // this translation have to agree about.
    auto const top = unbox<int>(mc.terminal.foldProjectionTopRow());

    for (auto i = size_t { 0 }; i < projection.size(); ++i)
    {
        auto const screenRow = LineOffset(top + static_cast<int>(i));
        CAPTURE(i, top, unbox<int>(screenRow));

        auto const gridLine = mc.terminal.viewport().translateScreenToGridLine(screenRow);
        CHECK(gridLine == projection[i]);
        CHECK(mc.terminal.viewport().translateGridToScreenCoordinate(gridLine) == screenRow);
    }
}

TEST_CASE("Folding.terminal.projectionAndRenderPassAgreeOnRowPlacement", "[folding]")
{
    // The property the whole design rests on, pinned directly: the rows Grid::render draws and the rows
    // the coordinate translation reports must be the same rows, on the same screen lines. They are
    // computed by different code, and a disagreement misplaces a selection rather than crashing.
    auto mc = MockTerm { PageSize { LineCount(10), ColumnCount(20) } };
    runCommand(mc, "ls", { "file1", "file2", "file3" });

    auto const ranges = mc.terminal.foldRanges();
    REQUIRE(ranges.size() == 1);
    mc.terminal.foldState().collapse(ranges[0].headStableId);

    auto const rows = mc.terminal.foldProjection();
    REQUIRE(!rows.empty());

    // Grid::render places the list by min(0, pageLines - size): here the walk ran out of grid at the
    // top, so the rows start at the top and the page is short at the bottom.
    auto const top = std::min(0, unbox<int>(mc.terminal.pageSize().lines) - static_cast<int>(rows.size()));
    CHECK(mc.terminal.foldProjectionTopRow() == LineOffset(top));

    for (auto i = size_t { 0 }; i < rows.size(); ++i)
        CHECK(mc.terminal.viewport().translateScreenToGridLine(LineOffset(top + static_cast<int>(i)))
              == rows[i]);
}

TEST_CASE("Folding.terminal.newMarksInvalidateTheCachedRanges", "[folding]")
{
    // The grid's own identity cannot stand in for a marks change: a second command on a page that has
    // not scrolled moves neither the generation nor the stable base, so a cache keyed on those alone
    // would go on reporting the ranges from before it finished.
    auto mc = MockTerm { PageSize { LineCount(40), ColumnCount(20) } };

    runCommand(mc, "ls", { "file1" });
    REQUIRE(mc.terminal.foldRanges().size() == 1);

    // No scrolling has happened -- the page is far taller than what has been written.
    REQUIRE(mc.terminal.primaryScreen().historyLineCount() == LineCount(0));

    runCommand(mc, "pwd", { "/tmp" });
    CHECK(mc.terminal.foldRanges().size() == 2);
}

TEST_CASE("Folding.terminal.hiddenLinesAreNotVisible", "[folding]")
{
    // isLineVisible cannot be an interval test once folds are in play: a hidden line sits INSIDE the
    // span of drawn rows, so a bounds check would call it visible and the vi cursor would be placed on
    // a row that is not on screen.
    auto mc = MockTerm { PageSize { LineCount(10), ColumnCount(20) } };
    runCommand(mc, "ls", { "file1", "file2" });

    auto const& viewport = mc.terminal.viewport();
    REQUIRE(viewport.isLineVisible(LineOffset(1)));
    REQUIRE(viewport.isLineVisible(LineOffset(2)));

    auto const ranges = mc.terminal.foldRanges();
    REQUIRE(ranges.size() == 1);
    mc.terminal.foldState().collapse(ranges[0].headStableId);

    // The prompt is still drawn; the output between it and the rows below is not.
    CHECK(viewport.isLineVisible(LineOffset(0)));
    CHECK(!viewport.isLineVisible(LineOffset(1)));
    CHECK(!viewport.isLineVisible(LineOffset(2)));
}

TEST_CASE("Folding.terminal.linesAboveTheViewportReportOffScreen", "[folding]")
{
    // A line scrolled out of sight above must translate to a NEGATIVE screen row, not to the top one:
    // cursor-line and search-match highlighting ask this question, and answering "row zero" would paint
    // the highlight on an unrelated line.
    auto mc = MockTerm { PageSize { LineCount(5), ColumnCount(20) }, LineCount(50) };
    for (auto i = 0; i < 20; ++i)
        mc.writeToScreen(std::format("filler {}\r\n", i));
    runCommand(mc, "ls", { "file1", "file2" });

    auto const ranges = mc.terminal.foldRanges();
    REQUIRE(ranges.size() == 1);
    mc.terminal.foldState().collapse(ranges[0].headStableId);

    auto const projection = mc.terminal.foldProjection();
    REQUIRE(!projection.empty());

    // One line above the topmost drawn row is one row above the viewport.
    auto const above = projection.front() - LineOffset(1);
    CHECK(mc.terminal.viewport().translateGridToScreenCoordinate(above) < mc.terminal.foldProjectionTopRow());
    CHECK(!mc.terminal.viewport().isLineVisible(above));
}

TEST_CASE("Folding.terminal.headCarriesTheMarker", "[folding]")
{
    auto mc = MockTerm { PageSize { LineCount(10), ColumnCount(20) } };
    runCommand(mc, "ls", { "file1", "file2" });

    auto const ranges = mc.terminal.foldRanges();
    REQUIRE(ranges.size() == 1);
    mc.terminal.foldState().collapse(ranges[0].headStableId);

    // Exactly one visible row is a collapsed head, and it is the prompt line the fold hangs off.
    auto collapsedHeads = 0;
    for (auto const row: mc.terminal.foldProjection())
        if (mc.terminal.foldMarkerAt(row) == FoldMarker::Collapsed)
            ++collapsedHeads;
    CHECK(collapsedHeads == 1);
}

TEST_CASE("Folding.terminal.gutterDrawsAFoldColumnOverTheWholeBlock", "[folding]")
{
    auto mc = MockTerm { PageSize { LineCount(10), ColumnCount(20) } };
    mc.terminal.settings().foldMarkers = true;
    runCommand(mc, "ls", { "file1", "file2" });

    auto buffer = RenderBuffer {};
    mc.terminal.fillRenderBuffer(buffer, /*includeSelection*/ true);

    // An expanded fold draws a COLUMN, not a lone marker: the head, a bar beside every output row, and
    // a corner closing it. That extent is what tells the user how far the block reaches -- and it is
    // what the mouse can hit, which one cell on the prompt row was not.
    REQUIRE(buffer.gutter.size() == 3);

    // Expanded: the output is showing, and the marker points down at it.
    CHECK(buffer.gutter[0].codepoint == foldMarkerGlyph(FoldMarker::Expanded));
    // On the prompt row, which is the top of the page here.
    CHECK(buffer.gutter[0].lineOffset == LineOffset(0));

    CHECK(buffer.gutter[1].codepoint == foldMarkerGlyph(FoldMarker::Body));
    CHECK(buffer.gutter[1].lineOffset == LineOffset(1));

    CHECK(buffer.gutter[2].codepoint == foldMarkerGlyph(FoldMarker::BodyEnd));
    CHECK(buffer.gutter[2].lineOffset == LineOffset(2));

    auto const ranges = mc.terminal.foldRanges();
    REQUIRE(ranges.size() == 1);
    mc.terminal.foldState().collapse(ranges[0].headStableId);

    buffer.clear();
    mc.terminal.fillRenderBuffer(buffer, /*includeSelection*/ true);

    // Collapsed, the column is one cell: the rows it would have spanned are the rows it now hides.
    REQUIRE(buffer.gutter.size() == 1);
    CHECK(buffer.gutter[0].codepoint == foldMarkerGlyph(FoldMarker::Collapsed));
    CHECK(buffer.gutter[0].lineOffset == LineOffset(0));
}

TEST_CASE("Folding.terminal.gutterSitsBesideTheGridUnderATopStatusLine", "[folding]")
{
    // The gutter is drawn in SCREEN rows, and a status line above the grid pushes the page down. The
    // markers must move with it -- and, more to the point, whatever maps a pixel back has to apply
    // the same shift, which is what Terminal::mainPageTopRow() states once for both directions.
    auto mc = MockTerm { PageSize { LineCount(10), ColumnCount(20) } };
    mc.terminal.settings().foldMarkers = true;
    mc.terminal.settings().statusDisplayPosition = StatusDisplayPosition::Top;
    mc.terminal.setStatusDisplay(StatusDisplayType::Indicator);

    auto const topRow = mc.terminal.mainPageTopRow();
    REQUIRE(topRow == mc.terminal.statusLineHeight().as<LineOffset>());
    REQUIRE(topRow > LineOffset(0));

    runCommand(mc, "ls", { "file1", "file2" });

    auto buffer = RenderBuffer {};
    mc.terminal.fillRenderBuffer(buffer, /*includeSelection*/ true);

    REQUIRE(buffer.gutter.size() == 3);

    // The fold head is on main-page row 0, which is drawn at screen row `topRow`.
    CHECK(buffer.gutter[0].lineOffset == topRow);
    CHECK(buffer.gutter[1].lineOffset == topRow + LineOffset(1));
    CHECK(buffer.gutter[2].lineOffset == topRow + LineOffset(2));
    CHECK(buffer.gutter[0].codepoint == foldMarkerGlyph(FoldMarker::Expanded));
}

TEST_CASE("Folding.terminal.hoveringTheGutterLightsTheWholeRun", "[folding]")
{
    auto mc = MockTerm { PageSize { LineCount(10), ColumnCount(20) } };
    mc.terminal.settings().foldMarkers = true;
    runCommand(mc, "ls", { "file1", "file2" });

    auto const unhovered = [&] {
        auto buffer = RenderBuffer {};
        mc.terminal.fillRenderBuffer(buffer, true);
        return buffer.gutter;
    }();
    REQUIRE(unhovered.size() == 3);

    // Hovering the BODY, not the head: the whole run is one control, so pointing anywhere on it has to
    // light all of it up -- that is what says how much a click is about to act on.
    mc.terminal.setGutterHoverLine(LineOffset(1));

    auto buffer = RenderBuffer {};
    mc.terminal.fillRenderBuffer(buffer, true);
    REQUIRE(buffer.gutter.size() == 3);
    for (size_t i = 0; i < buffer.gutter.size(); ++i)
    {
        CHECK(buffer.gutter[i].codepoint == unhovered[i].codepoint);

        // The glyph is what lights up, and it is the ONLY thing that does: the background stays the
        // page's own in both states, so nothing paints a band down the side of the block.
        CHECK(buffer.gutter[i].attributes.foregroundColor != unhovered[i].attributes.foregroundColor);
        CHECK(buffer.gutter[i].attributes.backgroundColor == unhovered[i].attributes.backgroundColor);

        // Recolorized, not inverted -- what the render path has to carry, as opposed to what the
        // derivation decides (@see ColorPalette.foldMarker.hoverRecolorizesRatherThanInverts). A
        // restored swap puts the page background here, which is exactly the resting background.
        CHECK(buffer.gutter[i].attributes.foregroundColor != unhovered[i].attributes.backgroundColor);
    }

    mc.terminal.setGutterHoverLine(std::nullopt);
    buffer.clear();
    mc.terminal.fillRenderBuffer(buffer, true);
    REQUIRE(buffer.gutter.size() == 3);
    CHECK(buffer.gutter[0].attributes.foregroundColor == unhovered[0].attributes.foregroundColor);
}

TEST_CASE("Folding.terminal.togglingFromAnyLineOfTheBlock", "[folding]")
{
    auto mc = MockTerm { PageSize { LineCount(10), ColumnCount(20) } };
    runCommand(mc, "ls", { "file1", "file2" });

    auto const ranges = mc.terminal.foldRanges();
    REQUIRE(ranges.size() == 1);

    // A user points at the OUTPUT they want gone, not at the prompt line it hangs off -- so a body row
    // toggles the block it belongs to rather than declining because it is not the head.
    CHECK(mc.terminal.foldMarkerAt(LineOffset(2)) == FoldMarker::BodyEnd);
    CHECK(mc.terminal.toggleFoldContaining(LineOffset(2)));
    CHECK(mc.terminal.foldState().isCollapsed(ranges[0].headStableId));

    // And back, from the head this time.
    CHECK(mc.terminal.toggleFoldContaining(LineOffset(0)));
    CHECK(!mc.terminal.foldState().isCollapsed(ranges[0].headStableId));

    // A line no fold reaches toggles nothing.
    CHECK(!mc.terminal.toggleFoldContaining(LineOffset(9)));
}

TEST_CASE("Folding.terminal.noGutterWithoutMarkersOrWithoutFolds", "[folding]")
{
    // Two ways to cost nothing: markers switched off, and a shell that leaves no marks at all -- the
    // overwhelmingly common case, and the one that must not pay for a scan it cannot use.
    SECTION("markers off")
    {
        auto mc = MockTerm { PageSize { LineCount(10), ColumnCount(20) } };
        mc.terminal.settings().foldMarkers = false;
        runCommand(mc, "ls", { "file1", "file2" });

        auto buffer = RenderBuffer {};
        mc.terminal.fillRenderBuffer(buffer, true);
        CHECK(buffer.gutter.empty());
    }

    SECTION("no shell integration")
    {
        auto mc = MockTerm { PageSize { LineCount(10), ColumnCount(20) } };
        mc.terminal.settings().foldMarkers = true;
        mc.writeToScreen("just some output\r\nand more\r\n");

        auto buffer = RenderBuffer {};
        mc.terminal.fillRenderBuffer(buffer, true);
        CHECK(buffer.gutter.empty());
    }
}

TEST_CASE("Folding.terminal.reflowClearsTheFoldState", "[folding]")
{
    // A resize that reflows destroys row identity wholesale, so every collapsed id now names a different
    // row, or none. Guessing would collapse the wrong block.
    auto mc = MockTerm { PageSize { LineCount(10), ColumnCount(20) } };
    runCommand(mc, "ls", { "file1", "file2" });

    auto const ranges = mc.terminal.foldRanges();
    REQUIRE(ranges.size() == 1);
    mc.terminal.foldState().collapse(ranges[0].headStableId);
    REQUIRE(!mc.terminal.foldState().empty());

    mc.terminal.resizeScreen(PageSize { LineCount(10), ColumnCount(10) });
    mc.terminal.refreshFoldState();

    CHECK(mc.terminal.foldState().empty());
    CHECK(mc.terminal.foldProjection().empty());
}

TEST_CASE("Folding.terminal.alternateScreenIsNeverFolded", "[folding]")
{
    // An alt-screen application owns the whole page and paints no prompt into it; the primary screen's
    // marks say nothing about what is on display.
    auto mc = MockTerm { PageSize { LineCount(10), ColumnCount(20) } };
    runCommand(mc, "ls", { "file1", "file2" });

    auto const ranges = mc.terminal.foldRanges();
    REQUIRE(ranges.size() == 1);
    mc.terminal.foldState().collapse(ranges[0].headStableId);
    REQUIRE(!mc.terminal.foldProjection().empty());

    mc.writeToScreen("\033[?1049h");

    CHECK(mc.terminal.foldProjection().empty());
    CHECK(mc.terminal.hiddenLineCount() == LineCount(0));
}

// }}}

// }}}
// {{{ Vi normal mode

namespace
{

/// A terminal in Vi normal mode showing one finished command, with that command's fold collapsed.
///
/// Layout, on a 10-line page: row 0 is the prompt line `$ ls` (the fold head), rows 1 and 2 are its
/// output, and row 3 onwards is where the next prompt would go. Collapsed, rows 1 and 2 are hidden,
/// so the visible rows are 0, 3, 4, ...
[[nodiscard]] auto setupViMock(bool collapse)
{
    // Built as a prvalue with an init callback, exactly as ViCommands_test's setupMockTerminal does:
    // MockTerm owns a Terminal and is neither copyable nor movable, so it can only be RETURNED by
    // being constructed in the return statement.
    return MockTerm<> { PageSize { LineCount(10), ColumnCount(20) },
                        LineCount(0),
                        1024,
                        [collapse](MockTerm<>& mc) {
                            runCommand(mc, "ls", { "file1", "file2" });
                            auto const ranges = mc.terminal.foldRanges();
                            REQUIRE(ranges.size() == 1);
                            if (collapse)
                                mc.terminal.foldState().collapse(ranges[0].headStableId);
                            mc.terminal.inputHandler().setMode(ViMode::Normal);
                        } };
}

/// Puts the Vi cursor on @p line without going through a motion.
void placeViCursor(MockTerm<>& mc, LineOffset line)
{
    mc.terminal.moveNormalModeCursorTo(CellLocation { .line = line, .column = ColumnOffset(0) });
    REQUIRE(mc.terminal.normalModeCursorPosition().line == line);
}

} // namespace

TEST_CASE("Folding.vi.jStepsOverACollapsedBlock", "[folding][vi]")
{
    auto mc = setupViMock(/*collapse*/ true);
    placeViCursor(mc, LineOffset(0));

    // One `j` from the fold head lands on the line the block CONTINUES at, not on the first row it
    // hides -- which is where the cursor used to vanish.
    mc.sendCharSequence("j");
    CHECK(mc.terminal.normalModeCursorPosition().line == LineOffset(3));

    mc.sendCharSequence("j");
    CHECK(mc.terminal.normalModeCursorPosition().line == LineOffset(4));
}

TEST_CASE("Folding.vi.kStepsOverACollapsedBlock", "[folding][vi]")
{
    auto mc = setupViMock(/*collapse*/ true);
    placeViCursor(mc, LineOffset(3));

    mc.sendCharSequence("k");
    CHECK(mc.terminal.normalModeCursorPosition().line == LineOffset(0));
}

TEST_CASE("Folding.vi.countsAreInVisibleLines", "[folding][vi]")
{
    auto mc = setupViMock(/*collapse*/ true);
    placeViCursor(mc, LineOffset(0));

    // `2j` means two rows the user can SEE. Counting grid lines instead would land on row 2, inside
    // the collapsed block.
    mc.sendCharSequence("2j");
    CHECK(mc.terminal.normalModeCursorPosition().line == LineOffset(4));
}

TEST_CASE("Folding.vi.anExpandedBlockIsWalkedNormally", "[folding][vi]")
{
    auto mc = setupViMock(/*collapse*/ false);
    placeViCursor(mc, LineOffset(0));

    // Nothing is collapsed, so every grid line is a visible line and `j` is the step it always was.
    mc.sendCharSequence("j");
    CHECK(mc.terminal.normalModeCursorPosition().line == LineOffset(1));
    mc.sendCharSequence("j");
    CHECK(mc.terminal.normalModeCursorPosition().line == LineOffset(2));
}

TEST_CASE("Folding.vi.theCursorNeverRestsOnAHiddenLine", "[folding][vi]")
{
    auto mc = setupViMock(/*collapse*/ false);

    // Park the cursor inside the block, THEN collapse it underneath -- the one way a fold-aware motion
    // cannot prevent a hidden cursor, and so the case the collapse-time snap exists for.
    placeViCursor(mc, LineOffset(2));
    REQUIRE(mc.terminal.toggleFoldContaining(LineOffset(2)));

    CHECK(mc.terminal.viewport().isLineVisible(mc.terminal.normalModeCursorPosition().line));
    CHECK(mc.terminal.normalModeCursorPosition().line == LineOffset(3));
}

TEST_CASE("Folding.vi.aResizeDoesNotStrandTheCursorInAFold", "[folding][vi]")
{
    auto mc = setupViMock(/*collapse*/ false);
    placeViCursor(mc, LineOffset(6));

    auto const ranges = mc.terminal.foldRanges();
    REQUIRE(ranges.size() == 1);
    mc.terminal.foldState().collapse(ranges[0].headStableId);
    REQUIRE(!mc.terminal.foldRanges().empty());

    // SHRINKING the page shifts the Vi cursor UP by the line delta, and that shift is fold-blind:
    // here it lands the cursor on grid line 1, inside the collapsed block, where it renders as
    // nothing at all. Columns are unchanged so nothing reflows and the fold survives the resize.
    mc.terminal.resizeScreen(PageSize { LineCount(5), ColumnCount(20) });
    REQUIRE(!mc.terminal.foldRanges().empty());

    CHECK(mc.terminal.viewport().isLineVisible(mc.terminal.normalModeCursorPosition().line));
}

TEST_CASE("Folding.vi.jumpIntoAFoldExpandsOrSkips", "[folding][vi]")
{
    SECTION("expand")
    {
        auto mc = setupViMock(/*collapse*/ true);
        mc.terminal.settings().foldJumpBehavior = FoldJumpBehavior::Expand;
        placeViCursor(mc, LineOffset(0));

        // A jump that NAMES a hidden line opens the block, so the cursor lands on what it was aiming
        // at rather than somewhere near it.
        mc.terminal.moveNormalModeCursorTo(CellLocation { .line = LineOffset(2), .column = ColumnOffset(0) });

        CHECK(mc.terminal.normalModeCursorPosition().line == LineOffset(2));
        CHECK(!mc.terminal.foldState().isCollapsed(mc.terminal.foldRanges()[0].headStableId));
    }

    SECTION("skip")
    {
        auto mc = setupViMock(/*collapse*/ true);
        mc.terminal.settings().foldJumpBehavior = FoldJumpBehavior::Skip;
        auto const headStableId = mc.terminal.foldRanges()[0].headStableId;
        placeViCursor(mc, LineOffset(0));

        mc.terminal.moveNormalModeCursorTo(CellLocation { .line = LineOffset(2), .column = ColumnOffset(0) });

        // The block stays shut, and the cursor stops at the line it continues at.
        CHECK(mc.terminal.foldState().isCollapsed(headStableId));
        CHECK(mc.terminal.normalModeCursorPosition().line == LineOffset(3));
    }
}

namespace
{

/// A terminal in Vi normal mode with every block of a deep scrollback collapsed.
///
/// Thirty blocks of four lines each, of which three are hidden once collapsed: a hundred and twenty
/// grid lines, thirty of them visible, on a ten-line page -- so there are twenty visible rows to scroll
/// through and roughly ninety grid lines hidden among them. That gap between the two counts is the
/// whole point of the fixture; a viewport where they agree cannot show the bug.
///
/// `scrolloff` is dialled down to 2, because the default 8 on a ten-line page leaves a safe area with
/// no interior and every motion would scroll.
[[nodiscard]] auto setupViScrollMock()
{
    return MockTerm<> {
        PageSize { LineCount(10), ColumnCount(20) },
        LineCount(200),
        1024,
        [](MockTerm<>& mc) {
            for (auto const i: std::views::iota(0, 30))
                runCommand(mc,
                           std::format("cmd{}", i),
                           { std::format("out{}a", i), std::format("out{}b", i), std::format("out{}c", i) });
            mc.terminal.viewport().setScrollOff(LineCount(2));
            REQUIRE(mc.terminal.collapseAllFolds());
            REQUIRE(mc.terminal.viewport().scrollableLineCount() > LineCount(0));
            REQUIRE(mc.terminal.hiddenLineCount() > mc.terminal.viewport().scrollableLineCount());
            mc.terminal.inputHandler().setMode(ViMode::Normal);
        }
    };
}

} // namespace

TEST_CASE("Folding.vi.kWalksUpThroughFoldedHistoryWithoutFallingBack", "[folding][vi]")
{
    // The reported symptom, end to end: fold everything, then hold `k`. Each press must take the cursor
    // one visible row UP and leave it there.
    //
    // What it used to do instead: the press that finally scrolls runs Terminal::onViewportChanged(),
    // whose clamp bounded the cursor by -scrollOffset -- a count of VISIBLE rows read as a grid line,
    // which with ninety hidden lines in between names a row well below the page. So every scrolling `k`
    // threw the cursor back down towards the bottom, and the next one climbed out of the same hole.
    auto mc = setupViScrollMock();
    auto& terminal = mc.terminal;

    placeViCursor(mc,
                  terminal.viewport().translateScreenToGridLine(
                      boxed_cast<LineOffset>(terminal.pageSize().lines) - LineOffset(1)));

    auto previous = terminal.normalModeCursorPosition().line;
    for (auto const press: std::views::iota(0, 25))
    {
        CAPTURE(press, previous);
        mc.sendCharSequence("k");

        auto const current = terminal.normalModeCursorPosition().line;
        REQUIRE(current < previous);
        REQUIRE(terminal.viewport().isLineVisible(current));
        previous = current;
    }
}

TEST_CASE("Folding.vi.jWalksDownThroughFoldedHistoryWithoutFallingBack", "[folding][vi]")
{
    // The mirror, from the top: `j` scrolls the viewport down and the same clamp fired there too.
    auto mc = setupViScrollMock();
    auto& terminal = mc.terminal;

    REQUIRE(terminal.viewport().scrollToTop());
    placeViCursor(mc, terminal.foldProjection().front());

    auto previous = terminal.normalModeCursorPosition().line;
    for (auto const press: std::views::iota(0, 25))
    {
        CAPTURE(press, previous);
        mc.sendCharSequence("j");

        auto const current = terminal.normalModeCursorPosition().line;
        REQUIRE(current > previous);
        REQUIRE(terminal.viewport().isLineVisible(current));
        previous = current;
    }
}

TEST_CASE("Folding.vi.aPlainMotionNeverOpensAFold", "[folding][vi]")
{
    // A motion is computed in visible lines and so never lands inside a fold -- but it lands off SCREEN
    // all the time, on any step the viewport has yet to scroll to. Asking "is it on screen" instead of
    // "does a fold hide it" sent those down the jump-target path, which under the default
    // FoldJumpBehavior::Expand opened whichever block the cursor stepped onto.
    auto const behavior = GENERATE(vtbackend::FoldJumpBehavior::Expand, vtbackend::FoldJumpBehavior::Skip);
    CAPTURE(behavior);

    auto mc = setupViScrollMock();
    auto& terminal = mc.terminal;
    terminal.settings().foldJumpBehavior = behavior;

    auto const hiddenBefore = terminal.hiddenLineCount();
    auto const revisionBefore = terminal.foldState().revision();

    SECTION("a counted motion reaching past the top of the viewport")
    {
        placeViCursor(mc, terminal.foldProjection().back());
        mc.sendCharSequence("15k");
    }

    SECTION("stepping at the very top of the scrollback, where the viewport can scroll no further")
    {
        REQUIRE(terminal.viewport().scrollToTop());
        placeViCursor(mc, terminal.foldProjection().front());
        for ([[maybe_unused]] auto const press: std::views::iota(0, 5))
            mc.sendCharSequence("k");
    }

    CHECK(terminal.hiddenLineCount() == hiddenBefore);
    CHECK(terminal.foldState().revision() == revisionBefore);
    CHECK(terminal.viewport().isLineVisible(terminal.normalModeCursorPosition().line));
}

TEST_CASE("Folding.vi.aJumpAboveTheViewportScrollsByVisibleRows", "[folding][vi]")
{
    // A named jump -- a search hit, a mark -- to a line a few visible rows above the viewport. The
    // scroll it asks for is measured in screen rows, so reporting the distance in GRID lines asked for
    // one ninety rows long, which scrollUp() clamps to the top: the jump landed at the top of the
    // scrollback rather than on its target.
    auto mc = setupViScrollMock();
    auto& terminal = mc.terminal;

    placeViCursor(mc, terminal.foldProjection().front());
    auto const target =
        terminal.advanceVisibleLines(terminal.foldProjection().front(), 4, VerticalDirection::Up);

    terminal.moveNormalModeCursorTo(CellLocation { .line = target, .column = ColumnOffset(0) });

    CHECK(terminal.normalModeCursorPosition().line == target);
    CHECK(terminal.viewport().isLineVisible(target));

    // Landed inside the safe area, not slammed against the top of the scrollable range.
    CHECK(terminal.viewport().translateGridToScreenCoordinate(target)
          == boxed_cast<LineOffset>(terminal.viewport().scrollOff()));
    CHECK(terminal.viewport().scrollOffset()
          < boxed_cast<ScrollOffset>(terminal.viewport().scrollableLineCount()));
}

TEST_CASE("Folding.viewport.clampsToTheRowsTheProjectionDraws", "[folding]")
{
    auto mc = setupViScrollMock();
    auto& terminal = mc.terminal;
    auto& viewport = terminal.viewport();

    REQUIRE(viewport.scrollTo(boxed_cast<ScrollOffset>(viewport.scrollableLineCount()) / 2));
    auto const projection = terminal.foldProjection();
    REQUIRE(projection.size() > 1);

    auto const clampLine = [&](LineOffset line) {
        return viewport.clampCellLocation(CellLocation { .line = line, .column = ColumnOffset(0) }).line;
    };

    // The bounds are the projection's own ends. -scrollOffset is a count of visible rows and names a
    // grid line BELOW the bottom of the page here, which is what dragged the Vi cursor down.
    REQUIRE(-boxed_cast<LineOffset>(viewport.scrollOffset()) > projection.back());

    CHECK(clampLine(projection.front() - LineOffset(50)) == projection.front());
    CHECK(clampLine(projection.back() + LineOffset(50)) == projection.back());
    CHECK(clampLine(projection[projection.size() / 2]) == projection[projection.size() / 2]);
}

TEST_CASE("Folding.viewport.rowsOffTheProjectionAreVisibleLinesToo", "[folding]")
{
    // The rows above and below what is drawn are what a selection dragged past the edge reaches, and
    // what a jump measures its scroll against. They continue the projection, so they continue it in
    // VISIBLE lines: naming whichever grid line happens to sit there would hand a selection rows a
    // collapsed block hides, and would misreport the distance by however many of them there are.
    auto mc = setupViScrollMock();
    auto& terminal = mc.terminal;
    auto& viewport = terminal.viewport();

    REQUIRE(viewport.scrollTo(boxed_cast<ScrollOffset>(viewport.scrollableLineCount()) / 2));

    auto const projection = terminal.foldProjection();
    REQUIRE(projection.size() == unbox<size_t>(terminal.pageSize().lines)); // so screen row == index

    for (auto const distance: std::views::iota(1, 4))
    {
        CAPTURE(distance);

        auto const aboveRow = LineOffset(-distance);
        auto const above = viewport.translateScreenToGridLine(aboveRow);
        CHECK(above == terminal.advanceVisibleLines(projection.front(), distance, VerticalDirection::Up));
        CHECK_FALSE(terminal.isLineHiddenByFold(above));
        CHECK(viewport.translateGridToScreenCoordinate(above) == aboveRow);

        auto const belowRow = boxed_cast<LineOffset>(terminal.pageSize().lines) + LineOffset(distance - 1);
        auto const below = viewport.translateScreenToGridLine(belowRow);
        CHECK(below == terminal.advanceVisibleLines(projection.back(), distance, VerticalDirection::Down));
        CHECK_FALSE(terminal.isLineHiddenByFold(below));
        CHECK(viewport.translateGridToScreenCoordinate(below) == belowRow);
    }
}

TEST_CASE("Folding.viewport.clampIsThePlainIntervalWithNothingCollapsed", "[folding]")
{
    // The unfolded fast path is the arithmetic it always was, and stays it.
    auto mc = MockTerm { PageSize { LineCount(5), ColumnCount(20) }, LineCount(50) };
    for (auto const i: std::views::iota(0, 20))
        mc.writeToScreen(std::format("history{}\r\n", i));

    auto& viewport = mc.terminal.viewport();
    REQUIRE(viewport.scrollTo(ScrollOffset(3)));
    REQUIRE(mc.terminal.foldProjection().empty());

    auto const clampLine = [&](LineOffset line) {
        return viewport.clampCellLocation(CellLocation { .line = line, .column = ColumnOffset(0) }).line;
    };

    CHECK(clampLine(LineOffset(-20)) == LineOffset(-3));
    CHECK(clampLine(LineOffset(20)) == LineOffset(1)); // 5 - 1 - 3
    CHECK(clampLine(LineOffset(-1)) == LineOffset(-1));
}

// }}}

// {{{ Scroll bounds under folds

namespace
{

/// A terminal with enough collapsible blocks scrolled off into history that folding every one of them
/// still leaves rows to scroll through, and a cell size set so the smooth-scroll accumulator has
/// something to work with.
///
/// Each block contributes four lines and hides three, so the scrollable range after a fold-all is the
/// block count less the page -- which is why there are this many of them.
MockTerm<> setupScrollMock()
{
    return MockTerm<> { PageSize { LineCount(5), ColumnCount(20) }, LineCount(100), 1024, [](auto& mock) {
                           for (auto const i: std::views::iota(0, 20))
                               runCommand(mock,
                                          std::format("cmd{}", i),
                                          { std::format("out{}a", i),
                                            std::format("out{}b", i),
                                            std::format("out{}c", i) });
                           mock.terminal.setCellPixelSize(
                               vtbackend::ImageSize { vtpty::Width(10), vtpty::Height(20) });
                       } };
}

} // namespace

TEST_CASE("Folding.scroll.collapsingReclampsAViewportScrolledPastTheNewTop", "[folding][scroll]")
{
    auto mc = setupScrollMock();
    auto& terminal = mc.terminal;

    REQUIRE(terminal.viewport().scrollToTop());
    auto const unfoldedTop = terminal.viewport().scrollOffset();
    REQUIRE(unfoldedTop > ScrollOffset(0));

    // Collapsing takes rows out of the scrollable range underneath a viewport already sitting at the
    // old top. Nothing else corrects that: scrollTo() REJECTS an out-of-range request rather than
    // repairing an offset already stored, so an unclamped viewport would be parked where every
    // subsequent scroll is refused -- and the smooth-scroll remainder would go on cycling there.
    REQUIRE(terminal.collapseAllFolds());
    REQUIRE(terminal.hiddenLineCount() > LineCount(0));

    CHECK(terminal.viewport().scrollOffset()
          == boxed_cast<ScrollOffset>(terminal.viewport().scrollableLineCount()));
    CHECK(terminal.viewport().scrollOffset() < unfoldedTop);
    CHECK(terminal.smoothScrollPixelOffset() == 0.0f);
}

TEST_CASE("Folding.scroll.smoothScrollSettlesAtTheFoldAwareTop", "[folding][scroll]")
{
    auto mc = setupScrollMock();
    auto& terminal = mc.terminal;

    REQUIRE(terminal.collapseAllFolds());
    auto const top = boxed_cast<ScrollOffset>(terminal.viewport().scrollableLineCount());
    REQUIRE(top > ScrollOffset(0));
    REQUIRE(terminal.viewport().scrollableLineCount()
            < terminal.primaryScreen().historyLineCount()); // else the bound would not differ

    // Push past the top a third of a cell at a time, and pin the position on EVERY step. Bounded by the
    // raw history instead of the scrollable count, the accumulator goes on handing scrollTo() offsets it
    // rejects while still storing a sub-cell remainder that wraps through a whole cell height as it
    // goes -- so the viewport stands still while the page slides up and snaps back, which is the
    // flicker. The end state alone would not catch it: the remainder is zero again by the time the
    // count has run past the raw history depth.
    auto reachedTop = false;
    for (auto const step: std::views::iota(0, 300))
    {
        CAPTURE(step);
        REQUIRE(terminal.applySmoothScrollPixelDelta(7.0f) == SmoothScrollResult::Applied);
        REQUIRE(terminal.viewport().scrollOffset() <= top);

        reachedTop = reachedTop || terminal.viewport().scrollOffset() == top;
        if (reachedTop)
        {
            REQUIRE(terminal.viewport().scrollOffset() == top);
            REQUIRE(terminal.smoothScrollPixelOffset() == 0.0f);
        }
    }

    CHECK(reachedTop);
}

TEST_CASE("Folding.scroll.momentumStopsAtTheFoldAwareTop", "[folding][scroll]")
{
    auto mc = setupScrollMock();
    auto& terminal = mc.terminal;
    auto constexpr ClockBase = chrono::steady_clock::time_point();

    REQUIRE(terminal.collapseAllFolds());
    auto const top = boxed_cast<ScrollOffset>(terminal.viewport().scrollableLineCount());
    REQUIRE(top > ScrollOffset(0));

    terminal.handleScrollPhase(ScrollPhase::Begin, 0.0f, ClockBase);
    for (auto const i: std::views::iota(1, 4))
        terminal.handleScrollPhase(ScrollPhase::Update, 200.0f, ClockBase + chrono::milliseconds(i * 10));
    terminal.handleScrollPhase(ScrollPhase::End, 0.0f, ClockBase + 40ms);

    // The glide has to END at the top. Its stop rule is "the viewport did not move this frame", which a
    // remainder still wobbling inside a frozen scroll offset would satisfy on no frame at all.
    for (auto const i: std::views::iota(0, 600))
        terminal.tick(ClockBase + 56ms + chrono::milliseconds(i * 16));

    CHECK_FALSE(terminal.isMomentumScrollActive());
    CHECK(terminal.viewport().scrollOffset() == top);
    CHECK(terminal.smoothScrollPixelOffset() == 0.0f);
}

TEST_CASE("Folding.scroll.aGlideToTheTopAndBackNeverJitters", "[folding][scroll]")
{
    // The reported symptom, end to end: fold everything, wheel to the top, wheel back down. The
    // viewport's position is scroll offset AND sub-cell remainder together, and what reads as a
    // flicker is that combined position moving BACKWARDS between frames while the glide continues.
    auto mc = setupScrollMock();
    auto& terminal = mc.terminal;
    auto constexpr ClockBase = chrono::steady_clock::time_point();
    auto constexpr CellHeight = 20.0f;

    REQUIRE(terminal.collapseAllFolds());
    auto const top = boxed_cast<ScrollOffset>(terminal.viewport().scrollableLineCount());
    REQUIRE(top > ScrollOffset(0));

    auto const positionPx = [&] {
        return (static_cast<float>(terminal.viewport().scrollOffset().value) * CellHeight)
               + terminal.smoothScrollPixelOffset();
    };

    auto const glide = [&](float velocityPx, auto&& stillGoing) {
        terminal.handleScrollPhase(ScrollPhase::Begin, 0.0f, ClockBase);
        for (auto const i: std::views::iota(1, 4))
            terminal.handleScrollPhase(
                ScrollPhase::Update, velocityPx, ClockBase + chrono::milliseconds(i * 10));
        terminal.handleScrollPhase(ScrollPhase::End, 0.0f, ClockBase + 40ms);

        auto previous = positionPx();
        for (auto const frame: std::views::iota(0, 600))
        {
            CAPTURE(frame);
            terminal.tick(ClockBase + 56ms + chrono::milliseconds(frame * 16));
            auto const current = positionPx();
            CHECK(stillGoing(previous, current));
            previous = current;
        }
    };

    glide(200.0f, [](float previous, float current) { return current >= previous; });
    CHECK_FALSE(terminal.isMomentumScrollActive());
    CHECK(terminal.viewport().scrollOffset() == top);

    glide(-200.0f, [](float previous, float current) { return current <= previous; });
    CHECK_FALSE(terminal.isMomentumScrollActive());
    CHECK(terminal.viewport().scrollOffset() == ScrollOffset(0));
    CHECK(terminal.smoothScrollPixelOffset() == 0.0f);
}

// }}}

// {{{ Invariants a fold has to keep against the rest of the terminal

TEST_CASE("Folding.terminal.aReflowDropsTheFoldState", "[folding]")
{
    auto mc = MockTerm { PageSize { LineCount(10), ColumnCount(20) } };
    runCommand(mc, "ls", { "file1", "file2" });

    auto const ranges = mc.terminal.foldRanges();
    REQUIRE(ranges.size() == 1);
    mc.terminal.foldState().collapse(ranges[0].headStableId);
    REQUIRE(!mc.terminal.foldState().empty());

    // A COLUMN change reflows, which destroys row identity wholesale: every id the fold state holds
    // now names a different row, or none. Resizing also reads the ranges on its way through (the Vi
    // cursor snap does), so the reconciliation cannot key on the ranges' own cache stamp -- that stamp
    // is already up to date by the time the next frame asks, and the wrong block stays collapsed.
    auto const generationBefore = mc.terminal.primaryScreen().grid().generation();
    mc.terminal.resizeScreen(PageSize { LineCount(10), ColumnCount(40) });
    REQUIRE(mc.terminal.primaryScreen().grid().generation() != generationBefore);

    // The reads that get there first in practice: the resize's own Vi-cursor snap, a mouse move over
    // the gutter mid-drag, a selection translating a coordinate. Any one of them refreshes the RANGES'
    // cache stamp, so the reconciliation must not be keyed on it.
    (void) mc.terminal.foldRanges();

    mc.terminal.refreshFoldState();
    CHECK(mc.terminal.foldState().empty());
}

TEST_CASE("Folding.terminal.aShortProjectionStaysInsideTheGrid", "[folding]")
{
    // Collapse enough that fewer rows are left than the page has. The screen rows below what is drawn
    // still have to map onto lines the grid HAS: Grid::rowAt indexes a ring buffer, so a line past the
    // bottom would read unrelated storage rather than fail.
    auto mc = MockTerm { PageSize { LineCount(10), ColumnCount(20) } };
    runCommand(mc, "seq", { "1", "2", "3", "4", "5", "6", "7" });

    auto const ranges = mc.terminal.foldRanges();
    REQUIRE(ranges.size() == 1);
    mc.terminal.foldState().collapse(ranges[0].headStableId);
    REQUIRE(mc.terminal.foldProjection().size() < unbox<size_t>(mc.terminal.pageSize().lines));

    auto const& grid = mc.terminal.primaryScreen().grid();
    auto const bottom = boxed_cast<LineOffset>(mc.terminal.pageSize().lines) - LineOffset(1);
    for (auto const row: std::views::iota(0, unbox<int>(mc.terminal.pageSize().lines)))
    {
        CAPTURE(row);
        auto const line = mc.terminal.viewport().translateScreenToGridLine(LineOffset(row));
        CHECK(line >= grid.addressableTop());
        CHECK(line <= bottom);
    }
}

TEST_CASE("Folding.terminal.gridToScreenStaysOrderedBelowTheProjection", "[folding]")
{
    // makeVisibleWithinSafeArea measures the distance to scroll in SCREEN rows. Reporting a flat
    // "one past the bottom" for everything under the viewport would make every downward jump advance by
    // the safe-area padding and stop, however far away the target actually is.
    //
    // And the distance has to be counted in VISIBLE rows for the same reason: a scroll travels in rows
    // the user can see, so a difference of grid lines -- most of them hidden here -- names a scroll far
    // longer than the one that would bring the target into view.
    auto mc = setupScrollMock();
    auto const& terminal = mc.terminal;
    REQUIRE(mc.terminal.collapseAllFolds());
    REQUIRE(mc.terminal.viewport().scrollToTop());

    auto const projection = terminal.foldProjection();
    REQUIRE(!projection.empty());

    auto const bottom = projection.back();
    auto const screenRowOf = [&](LineOffset line) {
        return terminal.viewport().translateGridToScreenCoordinate(line);
    };

    auto const near = screenRowOf(bottom + LineOffset(1));
    auto const far = screenRowOf(bottom + LineOffset(20));

    CHECK(near > screenRowOf(bottom));
    CHECK(far > near);
    CHECK(far - near
          == LineOffset(terminal.visibleDistance(bottom + LineOffset(1), bottom + LineOffset(20))));

    // And that really is fewer rows than the twenty grid lines between them -- otherwise the two
    // quantities would coincide and the assertion above would prove nothing.
    CHECK(far - near < LineOffset(19));
}

TEST_CASE("Folding.terminal.theAlternateScreenKeepsTheViCursorOnItsOwnGrid", "[folding][vi]")
{
    // Vi normal mode runs on the alternate screen too, and that grid has no scrollback. Bounds taken
    // from the PRIMARY grid would let `k` walk the cursor to a negative line the alternate screen has
    // no row for -- which Grid::rowAt resolves by wrapping its ring buffer.
    auto mc = MockTerm { PageSize { LineCount(5), ColumnCount(20) }, LineCount(50) };
    for (auto const i: std::views::iota(0, 20))
        mc.writeToScreen(std::format("history{}\r\n", i));
    REQUIRE(mc.terminal.primaryScreen().historyLineCount() > LineCount(0));

    mc.writeToScreen("\033[?1049h"); // to the alternate screen
    REQUIRE(mc.terminal.isAlternateScreen());
    REQUIRE(mc.terminal.currentScreen().historyLineCount() == LineCount(0));

    CHECK(mc.terminal.advanceVisibleLines(LineOffset(0), 3, VerticalDirection::Up) == LineOffset(0));
    CHECK(mc.terminal.snapToVisibleLine(LineOffset(-4), VerticalDirection::Up) == LineOffset(0));
}

TEST_CASE("Folding.terminal.foldingFollowsTheDISPLAYEDPage", "[folding]")
{
    // fillRenderBufferInternal hands Grid::render a folded row list for the DISPLAYED page 0 and draws
    // every other page contiguously. Everything else that speaks about folds has to key on the same
    // thing -- isAlternateScreen() does not, because it follows the CURSOR page.
    auto mc = MockTerm { PageSize { LineCount(10), ColumnCount(20) } };
    runCommand(mc, "ls", { "file1", "file2" });

    auto const ranges = mc.terminal.foldRanges();
    REQUIRE(ranges.size() == 1);
    mc.terminal.foldState().collapse(ranges[0].headStableId);
    REQUIRE(!mc.terminal.foldProjection().empty());

    SECTION("another page on display folds nothing")
    {
        // DECPCCM is set, so the display follows the cursor onto page 1.
        mc.terminal.setPage(PageIndex(1), /*moveCursorHome*/ true);
        REQUIRE_FALSE(mc.terminal.foldingAppliesToDisplayedPage());
        REQUIRE_FALSE(mc.terminal.foldState().empty()); // still collapsed, just not the page shown

        CHECK(mc.terminal.foldProjection().empty());
        CHECK(mc.terminal.hiddenLineCount() == LineCount(0));
    }

    SECTION("page 0 on display folds, wherever the cursor is")
    {
        // DECPCCM RESET: the cursor moves to another page while page 0 stays on display. This is the
        // case that separates the two predicates -- isAlternateScreen() reports true here, because it
        // asks about the cursor, while the render pass goes on folding the page it is drawing.
        mc.writeToScreen("\033[?64l"); // DECPCCM off
        mc.terminal.setPage(PageIndex(3), /*moveCursorHome*/ true);

        REQUIRE(mc.terminal.isAlternateScreen());
        REQUIRE(mc.terminal.foldingAppliesToDisplayedPage());

        CHECK_FALSE(mc.terminal.foldProjection().empty());
        CHECK(mc.terminal.hiddenLineCount() == LineCount(2));
    }
}

// }}}
// Temporary probe appended to Folding_test.cpp

// {{{ The cursor, under a projection that is not an addition

namespace
{

/// A folded terminal whose visible rows are a NON-contiguous selection of grid rows, so a cursor's
/// screen row genuinely differs from its grid line even at scroll offset zero.
MockTerm<> setupCursorMock()
{
    return MockTerm<> { PageSize { LineCount(10), ColumnCount(20) }, LineCount(400), 1024, [](auto& mock) {
                           for (auto const i: std::views::iota(0, 20))
                               runCommand(mock,
                                          std::format("cmd{}", i),
                                          { std::format("out{}a", i), std::format("out{}b", i) });
                           mock.terminal.setCursorDisplay(CursorDisplay::Steady);
                       } };
}

/// The screen rows the render buffer emitted per-cell data for -- the path that carries a block
/// cursor's cell inversion, which is the whole of how a block cursor is drawn.
std::set<int> perCellRows(Terminal& terminal, chrono::steady_clock::time_point now)
{
    terminal.tick(now);
    terminal.ensureFreshRenderBuffer();
    auto rows = std::set<int> {};
    auto const buffer = terminal.renderBuffer();
    for (auto const& cell: buffer.get().cells)
        rows.insert(unbox<int>(cell.position.line));
    return rows;
}

} // namespace

TEST_CASE("Folding.cursor.theCursorsOwnRowTakesThePerCellPath", "[folding][cursor]")
{
    auto mc = setupCursorMock();
    auto& terminal = mc.terminal;
    auto constexpr ClockBase = chrono::steady_clock::time_point();

    REQUIRE(terminal.collapseAllFolds());

    // Mid-page, where the projection has backfilled history above and the screen row is nothing like
    // the grid line. At the bottom row the two happen to coincide, which is why that is not the case
    // to test.
    mc.writeToScreen("\033[4;1H");
    auto const gridLine = terminal.currentScreen().cursor().position.line;
    auto const screenRow = terminal.viewport().translateGridToScreenCoordinate(gridLine);
    REQUIRE(screenRow != gridLine); // else the test proves nothing

    auto const rows = perCellRows(terminal, ClockBase + 100ms);

    // The row the cursor is DRAWN on gets the per-cell data. Comparing a grid line against a screen
    // row marked whichever row shared the number instead, so the block cursor appeared on an
    // unrelated line and vanished from its own -- and moved as scrolling reshaped the projection.
    CHECK(rows.contains(unbox<int>(screenRow)));
    CHECK_FALSE(rows.contains(unbox<int>(gridLine)));
}

TEST_CASE("Folding.cursor.aHiddenCursorMarksNoRowAtAll", "[folding][cursor]")
{
    auto mc = setupCursorMock();
    auto& terminal = mc.terminal;
    auto constexpr ClockBase = chrono::steady_clock::time_point();

    REQUIRE(terminal.collapseAllFolds());
    mc.writeToScreen("\033[4;1H");
    mc.writeToScreen("\033[?25l"); // DECTCEM off: there is no cursor to draw

    auto const rows = perCellRows(terminal, ClockBase + 100ms);
    auto const gridLine = terminal.currentScreen().cursor().position.line;

    // Neither the row it would be drawn on nor -- the old bug -- the row whose screen index merely
    // happens to equal its grid line.
    CHECK_FALSE(rows.contains(unbox<int>(terminal.viewport().translateGridToScreenCoordinate(gridLine))));
    CHECK_FALSE(rows.contains(unbox<int>(gridLine)));
}

TEST_CASE("Folding.cursor.theMotionAnimationOriginGoesThroughTheProjection", "[folding][cursor]")
{
    auto mc = setupCursorMock();
    auto& terminal = mc.terminal;
    auto constexpr ClockBase = chrono::steady_clock::time_point();
    REQUIRE(terminal.settings().cursorMotionAnimationDuration.count() > 0);

    REQUIRE(terminal.collapseAllFolds());

    // Both endpoints taken from the projection, so both are lines that are actually drawn -- a hidden
    // one carries no cursor at all -- and so the two are separated by a collapsed block, which is what
    // makes their distance in SCREEN rows differ from their distance in grid lines.
    auto pageRows = std::vector<LineOffset> {};
    for (auto const& row: terminal.foldProjection())
        if (row >= LineOffset(0))
            pageRows.push_back(row);
    REQUIRE(pageRows.size() >= 2);

    auto const fromLine = pageRows.front();
    auto const toLine = pageRows.back();
    REQUIRE(terminal.viewport().translateGridToScreenCoordinate(toLine)
                - terminal.viewport().translateGridToScreenCoordinate(fromLine)
            != toLine - fromLine); // else grid->screen is an addition here and the test proves nothing

    mc.writeToScreen(std::format("\033[{};1H", unbox<int>(fromLine) + 1));
    terminal.tick(ClockBase + 100ms);
    terminal.ensureFreshRenderBuffer();
    auto const fromGrid = terminal.currentScreen().cursor().position;
    REQUIRE(fromGrid.line == fromLine);

    mc.writeToScreen(std::format("\033[{};1H", unbox<int>(toLine) + 1));
    terminal.tick(ClockBase + 200ms);
    terminal.ensureFreshRenderBuffer();

    auto const buffer = terminal.renderBuffer();
    auto const& cursor = buffer.get().cursor;
    REQUIRE(cursor.has_value());
    REQUIRE(cursor->animateFrom.has_value());
    REQUIRE(cursor->animationProgress < 1.0f);

    // The animation interpolates between two SCREEN positions, and its origin has to be the row the
    // start line is actually drawn on. Reconstructing the mapping as one offset sampled at the
    // DESTINATION and adding it to the start line assumes grid->screen is an addition; under a fold
    // projection it is a lookup, so the origin landed on the wrong row -- and on a different wrong row
    // each frame while the viewport moved, which is the jitter.
    auto const expected = terminal.viewport().translateGridToScreenCoordinate(fromGrid.line);
    CHECK(cursor->animateFrom->line == expected);
}

// }}}

// }}}
// {{{ Scrolling by marks across collapsed folds

TEST_CASE("Folding.viewport.scrollMarkUpReachesMarksBehindCollapsedFolds", "[folding][viewport]")
{
    // A scroll offset counts VISIBLE rows, so the offset that brings a mark to the top is the number of
    // rows the user can SEE above it -- not its grid line negated, which counts the ninety-odd rows the
    // collapsed folds hide as well. That larger number is past scrollableLineCount(), so scrollTo()
    // REFUSED it as out of bounds while the action still reported success: the key was swallowed and
    // the viewport never moved.
    auto mc = setupViScrollMock();
    auto& terminal = mc.terminal;
    auto& viewport = terminal.viewport();

    REQUIRE(terminal.hiddenLineCount() > LineCount(0));
    REQUIRE(viewport.scrollOffset() == ScrollOffset(0));

    auto const maxOffset = boxed_cast<ScrollOffset>(viewport.scrollableLineCount());
    auto previous = viewport.scrollOffset();
    auto steps = 0;

    while (viewport.scrollMarkUp())
    {
        CAPTURE(steps, previous);

        // Every step climbs, stays inside what the viewport admits, and lands on a row that is DRAWN --
        // a mark inside a collapsed block has no offset that would show it, so it must be stepped over
        // rather than stopped on.
        REQUIRE(viewport.scrollOffset() > previous);
        REQUIRE(viewport.scrollOffset() <= maxOffset);
        REQUIRE(viewport.isLineVisible(viewport.translateScreenToGridLine(LineOffset(0))));

        previous = viewport.scrollOffset();
        ++steps;
    }

    // Thirty commands ran, each stamping a prompt mark, and collapsing a block keeps its head visible --
    // so the walk crosses many of them rather than stopping at the first refusal.
    CHECK(steps > 5);
}

TEST_CASE("Folding.viewport.scrollMarkDownReachesMarksBehindCollapsedFolds", "[folding][viewport]")
{
    // The mirror, walking back down from the top.
    auto mc = setupViScrollMock();
    auto& viewport = mc.terminal.viewport();

    REQUIRE(viewport.scrollToTop());

    auto previous = viewport.scrollOffset();
    auto steps = 0;

    while (viewport.scrollMarkDown())
    {
        CAPTURE(steps, previous);
        REQUIRE(viewport.scrollOffset() < previous);
        REQUIRE(viewport.isLineVisible(viewport.translateScreenToGridLine(LineOffset(0))));

        previous = viewport.scrollOffset();
        ++steps;
    }

    CHECK(steps > 5);

    // Having run out of marks below, the last step falls through to the bottom -- which is where the
    // unfolded behaviour ends up too.
    CHECK(viewport.scrollOffset() == ScrollOffset(0));
}

TEST_CASE("Folding.projection.theTopmostOffsetIsUnaffectedBySmoothScrolling", "[folding]")
{
    // The projection asks the walk for a page PLUS the smooth-scrolling row, and used to bound the
    // scroll offset it honoured by that same total -- one row short of what scrollableLineCount()
    // admits. At the very top of a folded scrollback the page was therefore drawn one row further down
    // for as long as the glide lasted, and snapped back the instant the sub-cell offset reached zero.
    auto mc = setupViScrollMock();
    auto& terminal = mc.terminal;
    auto& viewport = terminal.viewport();

    REQUIRE(viewport.scrollToTop());
    REQUIRE(viewport.scrollOffset() == boxed_cast<ScrollOffset>(viewport.scrollableLineCount()));

    auto const settled = viewport.translateScreenToGridLine(LineOffset(0));

    // Mid-glide: a non-zero sub-cell offset asks the projection for one row above the page.
    viewport.setPixelOffset(3.0f);
    REQUIRE(terminal.smoothScrollExtraLines() == LineCount(1));
    CHECK(viewport.translateScreenToGridLine(LineOffset(0)) == settled);

    viewport.resetPixelOffset();
    CHECK(viewport.translateScreenToGridLine(LineOffset(0)) == settled);
}

TEST_CASE("Folding.statusLine.theScrollPercentageCountsWhatCanBeReached", "[folding][statusline]")
{
    // The offset is bounded by the scrollable count, so dividing by the raw history depth yielded an
    // indicator that could never reach 100% and quoted a depth the user cannot travel to.
    auto mc = setupViScrollMock();
    auto& terminal = mc.terminal;

    REQUIRE(terminal.viewport().scrollToTop());

    auto const scrollable = terminal.viewport().scrollableLineCount();
    REQUIRE(scrollable > LineCount(0));
    REQUIRE(terminal.primaryScreen().historyLineCount() > scrollable);

    auto const text =
        serializeToVT(terminal, parseStatusLineSegment("{HistoryLineCount}"), StatusLineStyling::Disabled);
    CAPTURE(text);

    // Scrolled as far as the folds leave reachable, which is the whole of it.
    CHECK(text.contains("100%"));
}

// }}}
// {{{ Fold state across resizes

TEST_CASE("Folding.terminal.aLineCountResizeKeepsTheFoldState", "[folding]")
{
    // The other half of aReflowDropsTheFoldState. growLines/shrinkLines rotate the ring through the
    // primitives that carry the stable-id accounting, so every row keeps the id it had -- and yet the
    // generation bumps all the same, because a mirror still has to resync its geometry. Reconciling on
    // THAT counter threw away everything the user had collapsed the moment a status line appeared or
    // the window grew a row taller; stableIdGeneration() is the narrower question, and the right one.
    auto mc = MockTerm { PageSize { LineCount(10), ColumnCount(20) } };
    runCommand(mc, "ls", { "file1", "file2" });

    auto const ranges = mc.terminal.foldRanges();
    REQUIRE(ranges.size() == 1);
    mc.terminal.foldState().collapse(ranges[0].headStableId);
    REQUIRE(mc.terminal.hiddenLineCount() > LineCount(0));

    auto const& grid = mc.terminal.primaryScreen().grid();
    auto const generationBefore = grid.generation();
    auto const stableIdGenerationBefore = grid.stableIdGeneration();

    mc.terminal.resizeScreen(PageSize { LineCount(9), ColumnCount(20) });

    // The mirror-facing counter moved; the id-facing one did not.
    CHECK(grid.generation() != generationBefore);
    CHECK(grid.stableIdGeneration() == stableIdGenerationBefore);

    mc.terminal.refreshFoldState();
    CHECK(!mc.terminal.foldState().empty());
    CHECK(mc.terminal.hiddenLineCount() > LineCount(0));
}

TEST_CASE("Folding.terminal.theGutterStopsWhereTheProjectionDoes", "[folding]")
{
    // Collapse enough that the projection is shorter than the page: with no scrollback to backfill
    // from, the rows below the content are blank and Grid::render draws nothing on them. The gutter
    // walked the whole page regardless, and translateScreenToGridLine() answers for a row past the
    // projection with the LAST visible line -- so each blank row was handed that line's marker, a
    // column of duplicates hanging below the content while the grid beside it stayed empty.
    auto mc = MockTerm { PageSize { LineCount(10), ColumnCount(20) } };
    mc.terminal.settings().foldMarkers = true;
    runCommand(mc, "a", { "1", "2", "3", "4", "5" });
    runCommand(mc, "b", { "6" });

    auto const ranges = mc.terminal.foldRanges();
    REQUIRE(ranges.size() == 2);

    // The OLDER block, so the newer one stays expanded and its body reaches the bottom of the content:
    // that is the range a blank row extrapolated onto, and so the marker that used to be repeated.
    mc.terminal.foldState().collapse(ranges.back().headStableId);

    auto const projectedRows = mc.terminal.foldProjection().size();
    REQUIRE(projectedRows > 0);
    REQUIRE(projectedRows < unbox<size_t>(mc.terminal.pageSize().lines));

    auto buffer = RenderBuffer {};
    mc.terminal.fillRenderBuffer(buffer, true);

    // Nothing is drawn beside a row the grid leaves blank.
    for (auto const& cell: buffer.gutter)
    {
        CAPTURE(cell.lineOffset);
        CHECK(cell.lineOffset < LineOffset::cast_from(projectedRows));
    }

    // And no row carries two markers.
    auto rows = std::set<int> {};
    for (auto const& cell: buffer.gutter)
        CHECK(rows.insert(unbox<int>(cell.lineOffset)).second);
}

// }}}
