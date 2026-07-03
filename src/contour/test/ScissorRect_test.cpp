// SPDX-License-Identifier: Apache-2.0
//
// Unit tests for ScissorRect::intersect, the pure clip geometry the GL renderer uses to keep a
// transient inner scissor (e.g. smooth scroll) inside the outer scissor that clips the terminal
// below the custom title bar. Coordinates are OpenGL scissor space: bottom-left origin, device px.

#include <contour/display/ScissorRect.h>

#include <catch2/catch_test_macros.hpp>

using contour::display::ScissorRect;

TEST_CASE("ScissorRect::intersect clips an inner rect to the outer one", "[scissor]")
{
    // Outer scissor: the terminal area, excluding a 40px title bar at the top of a 1000px-tall
    // window -> bottom-left origin means the terminal occupies y in [0, 960).
    auto const outer = ScissorRect { .x = 0, .y = 0, .width = 1920, .height = 960 };

    SECTION("an inner rect extending above the terminal is clipped down to the terminal top")
    {
        // Smooth-scroll region that (wrongly) reaches y in [800, 1000) — its top 40px overlap the
        // title-bar strip. The intersection must cap height so it never paints above y=960.
        auto const inner = ScissorRect { .x = 100, .y = 800, .width = 200, .height = 200 };
        auto const clipped = inner.intersect(outer);
        CHECK(clipped.x == 100);
        CHECK(clipped.y == 800);
        CHECK(clipped.width == 200);
        CHECK(clipped.height == 160); // 960 - 800, not the requested 200
        CHECK_FALSE(clipped.empty());
    }

    SECTION("an inner rect fully inside the outer one is unchanged")
    {
        auto const inner = ScissorRect { .x = 10, .y = 10, .width = 100, .height = 100 };
        auto const clipped = inner.intersect(outer);
        CHECK(clipped.x == 10);
        CHECK(clipped.y == 10);
        CHECK(clipped.width == 100);
        CHECK(clipped.height == 100);
    }

    SECTION("an inner rect entirely inside the title bar yields an empty (clip-everything) rect")
    {
        // Entirely above the terminal (y >= 960) -> no overlap at all.
        auto const inner = ScissorRect { .x = 0, .y = 970, .width = 100, .height = 20 };
        auto const clipped = inner.intersect(outer);
        CHECK(clipped.empty());
        CHECK(clipped.height == 0);
    }
}

TEST_CASE("ScissorRect::intersect is commutative in covered area and never enlarges", "[scissor]")
{
    auto const a = ScissorRect { .x = 0, .y = 0, .width = 100, .height = 100 };
    auto const b = ScissorRect { .x = 50, .y = 50, .width = 100, .height = 100 };

    auto const ab = a.intersect(b);
    auto const ba = b.intersect(a);
    CHECK(ab.x == ba.x);
    CHECK(ab.y == ba.y);
    CHECK(ab.width == ba.width);
    CHECK(ab.height == ba.height);

    // Overlap is the box [50,100) x [50,100) -> 50x50, smaller than either input.
    CHECK(ab.x == 50);
    CHECK(ab.y == 50);
    CHECK(ab.width == 50);
    CHECK(ab.height == 50);
}

TEST_CASE("ScissorRect::empty flags zero-area rects", "[scissor]")
{
    CHECK(ScissorRect {}.empty());
    CHECK(ScissorRect { .x = 0, .y = 0, .width = 0, .height = 10 }.empty());
    CHECK(ScissorRect { .x = 0, .y = 0, .width = 10, .height = 0 }.empty());
    CHECK_FALSE(ScissorRect { .x = 0, .y = 0, .width = 1, .height = 1 }.empty());
}

TEST_CASE("itemScissorToTarget maps an item-relative scissor into render-target coordinates", "[scissor]")
{
    using contour::display::itemScissorToTarget;

    SECTION("the TOP pane of a horizontal split lands in the target's upper region")
    {
        // 1000px-tall window, top pane occupies y in [34, 517) top-left (below a 34px tab strip),
        // i.e. item height 483. A full-pane inner scissor (0,0,800,483 item-relative, bottom-left
        // origin) must land at the pane's bottom edge in window space: 1000 - (34 + 483) = 483.
        auto const inner = ScissorRect { .x = 0, .y = 0, .width = 800, .height = 483 };
        auto const mapped = itemScissorToTarget(inner,
                                                /*itemLeftDevice=*/0,
                                                /*itemTopDevice=*/34,
                                                /*itemHeightDevice=*/483,
                                                /*targetHeightDevice=*/1000);
        CHECK(mapped.x == 0);
        CHECK(mapped.y == 483);
        CHECK(mapped.width == 800);
        CHECK(mapped.height == 483);
    }

    SECTION("a full-window item is the identity mapping (the pre-split coincidence)")
    {
        // Item == window: offsets are zero and the bottom edges coincide, which is exactly why the
        // untranslated scissor happened to work for the single full-window pane.
        auto const inner = ScissorRect { .x = 10, .y = 20, .width = 300, .height = 400 };
        auto const mapped = itemScissorToTarget(inner, 0, 0, 1000, 1000);
        CHECK(mapped.x == 10);
        CHECK(mapped.y == 20);
        CHECK(mapped.width == 300);
        CHECK(mapped.height == 400);
    }

    SECTION("a RIGHT pane of a vertical split shifts by its left offset")
    {
        auto const inner = ScissorRect { .x = 0, .y = 100, .width = 400, .height = 200 };
        auto const mapped = itemScissorToTarget(inner,
                                                /*itemLeftDevice=*/960,
                                                /*itemTopDevice=*/34,
                                                /*itemHeightDevice=*/966,
                                                /*targetHeightDevice=*/1000);
        CHECK(mapped.x == 960);
        CHECK(mapped.y == 100); // bottom-aligned pane: 1000 - (34 + 966) = 0 offset
        CHECK(mapped.width == 400);
        CHECK(mapped.height == 200);
    }
}
