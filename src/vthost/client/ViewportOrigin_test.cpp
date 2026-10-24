// SPDX-License-Identifier: Apache-2.0
#include <catch2/catch_test_macros.hpp>

#include <vthost/client/ViewportOrigin.hpp>

using vtbackend::ColumnCount;
using vtbackend::LineCount;
using vtbackend::PageSize;
using vthost::client::viewportOrigin;

TEST_CASE("viewportOrigin places a client's view inside a larger shared grid", "[vthost][viewport]")
{
    // A daemon-hosted grid belongs to every attached client, so a client smaller than it shows a
    // WINDOW into it -- and the window has to contain the cursor, or the user types out of sight.
    // The rule is tmux's (tty.c, tty_window_offset1).
    using vtbackend::CellLocation;
    using vtbackend::ColumnOffset;
    using vtbackend::LineOffset;

    auto const at = [](int line, int column) {
        return CellLocation { .line = LineOffset(line), .column = ColumnOffset(column) };
    };
    auto const grid = PageSize { .lines = LineCount(50), .columns = ColumnCount(200) };

    SECTION("a viewport at least as large as the grid never pans")
    {
        // The letterbox case: the grid is drawn at its own size and the surplus is background.
        CHECK(viewportOrigin(grid, grid, at(49, 199)) == vtbackend::CellLocation {});
        auto const bigger = PageSize { .lines = LineCount(80), .columns = ColumnCount(300) };
        CHECK(viewportOrigin(grid, bigger, at(49, 199)) == vtbackend::CellLocation {});
    }

    SECTION("a cursor already inside the first viewport-worth keeps the grid's origin")
    {
        auto const viewport = PageSize { .lines = LineCount(10), .columns = ColumnCount(40) };
        CHECK(viewportOrigin(grid, viewport, at(0, 0)) == vtbackend::CellLocation {});
        // The last cell that still fits without moving: one short of the viewport's own extent.
        CHECK(viewportOrigin(grid, viewport, at(9, 39)) == vtbackend::CellLocation {});
    }

    SECTION("the cursor sits on the last visible row, and centred horizontally")
    {
        auto const viewport = PageSize { .lines = LineCount(10), .columns = ColumnCount(40) };
        auto const origin = viewportOrigin(grid, viewport, at(30, 100));
        // Vertically: row 30 is the bottom row of a 10-row window starting at 21.
        CHECK(origin.line == LineOffset(21));
        // Horizontally: column 100 with 20 columns of context to its left.
        CHECK(origin.column == ColumnOffset(80));
    }

    SECTION("the viewport never runs off the end of the grid")
    {
        auto const viewport = PageSize { .lines = LineCount(10), .columns = ColumnCount(40) };
        auto const origin = viewportOrigin(grid, viewport, at(49, 199));
        // Clamped to the last full window: 50-10 and 200-40. Without the clamp the view would show
        // rows the grid does not have, which is how a panning terminal draws garbage at the edge.
        CHECK(origin.line == LineOffset(40));
        CHECK(origin.column == ColumnOffset(160));
    }

    SECTION("the axes are decided independently")
    {
        // Wide enough, far too short: the column axis must not pan just because the line axis does.
        auto const viewport = PageSize { .lines = LineCount(10), .columns = ColumnCount(200) };
        auto const origin = viewportOrigin(grid, viewport, at(45, 199));
        CHECK(origin.column == ColumnOffset(0));
        // Row 45 is the bottom row of a 10-row window starting at 36 — short of the 40 the clamp
        // would impose, which only binds once the cursor reaches the last row.
        CHECK(origin.line == LineOffset(36));
    }

    SECTION("a degenerate grid smaller than the viewport still yields a non-negative origin")
    {
        auto const tiny = PageSize { .lines = LineCount(1), .columns = ColumnCount(1) };
        auto const viewport = PageSize { .lines = LineCount(10), .columns = ColumnCount(40) };
        CHECK(viewportOrigin(tiny, viewport, at(0, 0)) == vtbackend::CellLocation {});
    }
}
