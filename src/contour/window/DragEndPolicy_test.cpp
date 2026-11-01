// SPDX-License-Identifier: Apache-2.0
#include <contour/window/DragEndPolicy.hpp>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <string_view>

using namespace contour::window;

namespace
{
/// The window every row is seen in: 800 x 600 logical pixels.
constexpr auto Window = DropExtent { .width = 800, .height = 600 };

/// One row of the classifier table: what was tracked, and what it reads as.
struct TrackedDragRow
{
    std::string_view name;
    TrackedDrag drag;
    PointerOverContour expected;
};

/// A drag last seen at @p at, after @p before, that then left the window.
constexpr TrackedDrag left(DropPoint before, DropPoint at)
{
    return TrackedDrag { .presence = DragPresence::Left,
                         .last = DragSighting { .at = at, .before = before, .window = Window } };
}

constexpr auto TrackedDragRows = std::array {
    TrackedDragRow {
        .name = "never seen over a window", .drag = TrackedDrag {}, .expected = PointerOverContour::Yes },
    TrackedDragRow { .name = "still over a window",
                     .drag = TrackedDrag { .presence = DragPresence::Over,
                                           .last = DragSighting { .at = { .x = 400, .y = 300 },
                                                                  .before = { .x = 400, .y = 300 },
                                                                  .window = Window } },
                     .expected = PointerOverContour::Yes },
    TrackedDragRow { .name = "left across the right edge",
                     .drag = left({ .x = 780, .y = 300 }, { .x = 790, .y = 300 }),
                     .expected = PointerOverContour::No },
    TrackedDragRow { .name = "left across the top edge",
                     .drag = left({ .x = 400, .y = 20 }, { .x = 400, .y = 4 }),
                     .expected = PointerOverContour::No },
    TrackedDragRow { .name = "exactly on the band's inner border",
                     .drag = left({ .x = 400, .y = 576 }, { .x = 400, .y = 576 }),
                     .expected = PointerOverContour::No },
    TrackedDragRow { .name = "a flick from deep inside whose next step is past the left edge",
                     .drag = left({ .x = 300, .y = 300 }, { .x = 100, .y = 300 }),
                     .expected = PointerOverContour::No },
    TrackedDragRow { .name = "gone from deep inside: Escape, or a doubt",
                     .drag = left({ .x = 400, .y = 300 }, { .x = 402, .y = 301 }),
                     .expected = PointerOverContour::Yes },
};
} // namespace

TEST_CASE("dragEnd.trackedDragTable", "[tabbar][dragend]")
{
    for (auto const& row: TrackedDragRows)
    {
        INFO(row.name);
        CHECK(classifyTrackedDrag(row.drag) == row.expected);
    }
}
