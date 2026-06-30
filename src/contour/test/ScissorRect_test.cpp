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
