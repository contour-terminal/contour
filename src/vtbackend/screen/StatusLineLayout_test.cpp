// SPDX-License-Identifier: Apache-2.0
#include <vtbackend/screen/StatusLineLayout.hpp>

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include <format>
#include <optional>
#include <ranges>
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

/// A bare literal on @p side, @p width columns wide: a separator such as " │ " or "« ".
[[nodiscard]] constexpr StatusLineItemExtent separator(StatusLineSide side, int width)
{
    return StatusLineItemExtent { .side = side,
                                  .width = ColumnCount(width),
                                  .priority = StatusLinePriority::Normal,
                                  .kind = StatusLineItemKind::Separator };
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

TEST_CASE("collapseStatusLine.dropsOnlyWhatHelps", "[statusline][collapse]")
{
    using enum StatusLinePriority;

    SECTION("a middle too wide on its own loses only its own items")
    {
        // 78 columns cannot be centred on an 80-column line whatever else goes, so the right's Low clock
        // stays.
        auto const items = std::vector {
            item(StatusLineSide::Left, 8, Essential),
            item(StatusLineSide::Middle, 78, Normal),
            item(StatusLineSide::Right, 6, Low),
        };
        CHECK(collapseStatusLine(items, ColumnCount(80)) == std::vector { Shown, Dropped, Shown });
    }

    SECTION("a middle that overlaps the left may lose the left's items, never the right's")
    {
        auto const items = std::vector {
            item(StatusLineSide::Left, 8, Essential),
            item(StatusLineSide::Left, 4, Low),
            item(StatusLineSide::Middle, 66, Normal),
            item(StatusLineSide::Right, 6, Low),
        };
        CHECK(collapseStatusLine(items, ColumnCount(80)) == std::vector { Shown, Dropped, Dropped, Shown });
    }

    SECTION("the right's items never help the middle")
    {
        auto const items = std::vector {
            item(StatusLineSide::Left, 10, Essential),
            item(StatusLineSide::Middle, 4, Normal),
            item(StatusLineSide::Right, 5, Low),
        };
        CHECK(collapseStatusLine(items, ColumnCount(20)) == std::vector { Shown, Dropped, Shown });
    }

    SECTION("a right segment as wide as the line loses only its own items")
    {
        auto const items = std::vector {
            item(StatusLineSide::Left, 5, Low),
            item(StatusLineSide::Right, 15, Normal),
            item(StatusLineSide::Right, 10, Normal),
        };
        CHECK(collapseStatusLine(items, ColumnCount(20)) == std::vector { Shown, Shown, Dropped });
    }

    SECTION("nothing is dropped where nothing helps")
    {
        // Only separators, which are never dropped on their own account: the segment is not shown, as before.
        auto const items = std::vector {
            separator(StatusLineSide::Left, 10),
            separator(StatusLineSide::Middle, 4),
            separator(StatusLineSide::Right, 5),
        };
        CHECK(collapseStatusLine(items, ColumnCount(20)) == std::vector { Shown, Shown, Shown });
    }
}

TEST_CASE("collapseStatusLine.aSeparatorGoesWithItsItem", "[statusline][collapse]")
{
    using enum StatusLinePriority;

    SECTION("« … » losing its title leaves no lone «")
    {
        auto const items = std::vector {
            item(StatusLineSide::Left, 8, Essential), separator(StatusLineSide::Middle, 2),
            item(StatusLineSide::Middle, 70, Normal), separator(StatusLineSide::Middle, 2),
            item(StatusLineSide::Right, 6, Normal),
        };
        CHECK(collapseStatusLine(items, ColumnCount(80))
              == std::vector { Shown, Dropped, Dropped, Dropped, Shown });
    }

    SECTION("between two items, it goes with either")
    {
        auto const items = std::vector {
            item(StatusLineSide::Left, 10, Essential),
            item(StatusLineSide::Right, 4, Normal),
            separator(StatusLineSide::Right, 3),
            item(StatusLineSide::Right, 5, Low),
        };
        CHECK(collapseStatusLine(items, ColumnCount(20)) == std::vector { Shown, Shown, Dropped, Dropped });
    }

    SECTION("a leading one goes with the item after it, a trailing one with the item before it")
    {
        auto const items = std::vector {
            item(StatusLineSide::Left, 10, Essential),
            separator(StatusLineSide::Right, 1),
            item(StatusLineSide::Right, 5, Low),
            separator(StatusLineSide::Right, 1),
        };
        CHECK(collapseStatusLine(items, ColumnCount(14)) == std::vector { Shown, Dropped, Dropped, Dropped });
    }

    SECTION("it stays while both neighbours do")
    {
        auto const items = std::vector {
            separator(StatusLineSide::Left, 1), item(StatusLineSide::Left, 6, Essential),
            separator(StatusLineSide::Left, 3), item(StatusLineSide::Left, 6, Normal),
            separator(StatusLineSide::Left, 1), item(StatusLineSide::Right, 5, Low),
        };
        CHECK(collapseStatusLine(items, ColumnCount(20))
              == std::vector { Shown, Shown, Shown, Shown, Shown, Dropped });
    }
}

TEST_CASE("collapseStatusLine.measuresSegmentsAsItIsTold", "[statusline][collapse]")
{
    // Two items that join into one cluster measure 1 + 0 alone and 2 together: the caller's measure decides.
    auto const items = std::vector {
        item(StatusLineSide::Left, 8, StatusLinePriority::Essential),
        item(StatusLineSide::Right, 1, StatusLinePriority::Low),
        item(StatusLineSide::Right, 0, StatusLinePriority::Low),
    };
    auto const joined = [&](StatusLineSide side, std::span<StatusLineItemFate const> fates) {
        if (side == StatusLineSide::Left)
            return ColumnCount(8);
        if (side == StatusLineSide::Right && fates[1] == Shown)
            return ColumnCount(2);
        return ColumnCount(0);
    };
    CHECK(collapseStatusLine(items, ColumnCount(9)) == std::vector { Shown, Shown, Shown });
    CHECK(collapseStatusLine(items, ColumnCount(9), joined) == std::vector { Shown, Dropped, Shown });
}

TEST_CASE("collapseStatusLine.measuresTheLineAsItWouldBeDrawn", "[statusline][collapse]")
{
    using enum StatusLinePriority;

    // Classic in 14 columns: " NORMAL" | "« title »" | "0 │ 12:04". Once the clock goes, its " │ " goes with
    // it, and the right fits beside the mode; once the title goes, so do « and ». The mode stays: counting
    // the separators that will be dropped made the line look too wide for it.
    auto const items = std::vector {
        separator(StatusLineSide::Left, 1),   item(StatusLineSide::Left, 6, Essential),
        separator(StatusLineSide::Middle, 2), item(StatusLineSide::Middle, 10, Normal),
        separator(StatusLineSide::Middle, 2), item(StatusLineSide::Right, 1, Normal),
        separator(StatusLineSide::Right, 3),  item(StatusLineSide::Right, 5, Low),
    };
    for (auto const columns: std::views::iota(11, 18))
    {
        INFO(columns);
        auto const expected =
            columns < 16 ? std::vector { Shown, Shown, Dropped, Dropped, Dropped, Shown, Dropped, Dropped }
                         : std::vector { Shown, Shown, Dropped, Dropped, Dropped, Shown, Shown, Shown };
        CHECK(collapseStatusLine(items, ColumnCount(columns)) == expected);
    }
}
