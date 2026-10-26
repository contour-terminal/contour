// SPDX-License-Identifier: Apache-2.0
#include <vtbackend/screen/StatusLineLayout.hpp>

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include <format>
#include <optional>
#include <vector>

using namespace vtbackend;

namespace
{
constexpr auto Shown = StatusLineItemFate::Shown;
constexpr auto Dropped = StatusLineItemFate::Dropped;

/// An item on @p side, @p width columns wide, of @p priority.
[[nodiscard]] constexpr StatusLineItemExtent item(StatusLineSide side, int width, StatusLinePriority priority)
{
    return StatusLineItemExtent { .side = side, .width = ColumnCount(width), .priority = priority };
}

/// Where a segment of the placement starts, as a plain number, or -1 when it is not placed.
[[nodiscard]] int startOf(std::optional<ColumnOffset> start)
{
    return start ? unbox<int>(*start) : -1;
}
} // namespace

TEST_CASE("placeStatusLineSegments.isTodaysRule", "[statusline][collapse]")
{
    // The middle is centred, or not shown; the right ends in the last column, or is not shown.
    auto const [left, middle, right, middleStart, rightStart] = GENERATE(table<int, int, int, int, int>({
        { 0, 0, 3, -1, 17 },
        { 0, 8, 0, 6, -1 },
        { 6, 8, 0, 6, -1 },
        { 7, 8, 0, -1, -1 },
        { 0, 0, 20, -1, -1 },
        { 12, 0, 8, -1, 12 },
        { 0, 10, 6, 5, -1 }, // the right one would overlap the middle one, which ends at column 15
        { 0, 9, 6, 5, 14 },  // it ends where the right one starts
    }));
    INFO(std::format("left {}, middle {}, right {}", left, middle, right));
    auto const placement = placeStatusLineSegments(StatusLineSegmentWidths { .left = ColumnCount(left),
                                                                             .middle = ColumnCount(middle),
                                                                             .right = ColumnCount(right) },
                                                   ColumnCount(20));
    CHECK(startOf(placement.middleStart) == middleStart);
    CHECK(startOf(placement.rightStart) == rightStart);
}

TEST_CASE("collapseStatusLine.keepsWhatFits", "[statusline][collapse]")
{
    auto const items = std::vector {
        item(StatusLineSide::Left, 8, StatusLinePriority::Essential),
        item(StatusLineSide::Left, 6, StatusLinePriority::Low),
        item(StatusLineSide::Middle, 10, StatusLinePriority::Normal),
        item(StatusLineSide::Right, 6, StatusLinePriority::Low),
    };
    CHECK(collapseStatusLine(items, ColumnCount(60)) == std::vector { Shown, Shown, Shown, Shown });
    CHECK(collapseStatusLine({}, ColumnCount(20)).empty());
}

TEST_CASE("collapseStatusLine.dropsByPriorityRightToLeft", "[statusline][collapse]")
{
    using enum StatusLinePriority;

    SECTION("the canvas's narrow pane")
    {
        // [Left: Essential 8, High 15, Normal 24][Middle: High 22][Right: High 17, Low 6] in 40 columns. The
        // Low goes, then the Normal, and of the three equal Highs the middle's: its segment alone is still
        // not placed, while the left and the right one now fit.
        auto const items = std::vector {
            item(StatusLineSide::Left, 8, Essential), item(StatusLineSide::Left, 15, High),
            item(StatusLineSide::Left, 24, Normal),   item(StatusLineSide::Middle, 22, High),
            item(StatusLineSide::Right, 17, High),    item(StatusLineSide::Right, 6, Low),
        };
        CHECK(collapseStatusLine(items, ColumnCount(40))
              == std::vector { Shown, Shown, Dropped, Dropped, Shown, Dropped });
    }

    SECTION("of two equal Normals on the left and the right, the right one goes first")
    {
        auto const items = std::vector {
            item(StatusLineSide::Left, 8, Essential),
            item(StatusLineSide::Left, 6, Normal),
            item(StatusLineSide::Right, 8, Normal),
        };
        CHECK(collapseStatusLine(items, ColumnCount(20)) == std::vector { Shown, Shown, Dropped });
    }

    SECTION("with every segment unplaced, ties go right to left")
    {
        // The left segment is wider than the line, so the right one cannot be placed either: the right's item
        // goes first, then the left's last.
        auto const items = std::vector {
            item(StatusLineSide::Left, 15, Normal),
            item(StatusLineSide::Left, 10, Normal),
            item(StatusLineSide::Right, 5, Normal),
        };
        CHECK(collapseStatusLine(items, ColumnCount(20)) == std::vector { Shown, Dropped, Dropped });
    }

    SECTION("a segment that is placed keeps its item over an equal one that is not")
    {
        // The middle cannot be placed beside the left, the right can: the middle's Normal goes, though it is
        // left of the right's.
        auto const items = std::vector {
            item(StatusLineSide::Left, 10, Normal),
            item(StatusLineSide::Middle, 4, Normal),
            item(StatusLineSide::Right, 5, Normal),
        };
        CHECK(collapseStatusLine(items, ColumnCount(20)) == std::vector { Shown, Dropped, Shown });
    }

    SECTION("an overflowing left segment drops by priority")
    {
        auto const items = std::vector {
            item(StatusLineSide::Left, 8, Essential),
            item(StatusLineSide::Left, 20, High),
            item(StatusLineSide::Left, 10, Normal),
        };
        CHECK(collapseStatusLine(items, ColumnCount(30)) == std::vector { Shown, Shown, Dropped });
    }

    SECTION("an item of no width is never dropped and never counts")
    {
        auto const items = std::vector {
            item(StatusLineSide::Left, 0, Low),
            item(StatusLineSide::Left, 15, Essential),
            item(StatusLineSide::Right, 0, Low),
            item(StatusLineSide::Right, 10, Normal),
        };
        CHECK(collapseStatusLine(items, ColumnCount(20)) == std::vector { Shown, Shown, Shown, Dropped });
    }
}

TEST_CASE("collapseStatusLine.neverDropsTheLastItem", "[statusline][collapse]")
{
    using enum StatusLinePriority;

    SECTION("one item wider than the line is shown, to be cut")
    {
        auto const items = std::vector { item(StatusLineSide::Left, 30, Low) };
        CHECK(collapseStatusLine(items, ColumnCount(20)) == std::vector { Shown });
    }

    SECTION("nor is the last of several")
    {
        auto const items = std::vector {
            item(StatusLineSide::Left, 30, Essential),
            item(StatusLineSide::Middle, 4, High),
            item(StatusLineSide::Right, 4, Low),
        };
        CHECK(collapseStatusLine(items, ColumnCount(20)) == std::vector { Shown, Dropped, Dropped });
    }

    SECTION("Essentials go only after every other item")
    {
        auto const items = std::vector {
            item(StatusLineSide::Left, 12, Essential), item(StatusLineSide::Left, 12, Essential),
            item(StatusLineSide::Right, 3, Low),       item(StatusLineSide::Right, 3, Normal),
            item(StatusLineSide::Right, 3, High),
        };
        CHECK(collapseStatusLine(items, ColumnCount(20))
              == std::vector { Shown, Dropped, Dropped, Dropped, Dropped });
    }
}
