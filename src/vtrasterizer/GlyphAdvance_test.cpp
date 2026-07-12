// SPDX-License-Identifier: Apache-2.0
#include <vtrasterizer/GlyphAdvance.h>

#include <catch2/catch_test_macros.hpp>

using namespace vtrasterizer;

// The quantization is a compile-time decision; pin it as one.
static_assert(advanceToCells(0, 15) == 0, "a zero advance must not move the pen");
static_assert(advanceToCells(7, 15) == 1, "a sub-half-cell advance must still move the pen one cell");
static_assert(advanceToCells(30, 15) == 2, "a double-cell advance must move the pen two cells");

TEST_CASE("GlyphAdvance.zero_advance_does_not_move_the_pen", "[glyph_advance]")
{
    // Combining marks and the non-final glyphs of a ligature report a zero advance and must keep it.
    CHECK(advanceToCells(0, 15) == 0);
    CHECK(advanceToCells(0, 1) == 0);
}

TEST_CASE("GlyphAdvance.a_positive_advance_never_collapses_to_zero", "[glyph_advance]")
{
    // Issue #1939: a proportional fallback font reports a 7px advance for the non-breaking space in a
    // 15px cell. Rounding to nearest yields zero, and the rest of the run is drawn one cell too far
    // left. Anything that prints must advance.
    CHECK(advanceToCells(7, 15) == 1);
    CHECK(advanceToCells(1, 15) == 1);
    CHECK(advanceToCells(3, 10) == 1);
}

TEST_CASE("GlyphAdvance.single_cell_advances", "[glyph_advance]")
{
    CHECK(advanceToCells(14, 15) == 1);
    CHECK(advanceToCells(15, 15) == 1);
    CHECK(advanceToCells(22, 15) == 1);
}

TEST_CASE("GlyphAdvance.multi_cell_advances", "[glyph_advance]")
{
    // Ligatures and emoji legitimately span several cells; that must survive the clamp.
    CHECK(advanceToCells(23, 15) == 2);
    CHECK(advanceToCells(30, 15) == 2);
    CHECK(advanceToCells(45, 15) == 3);
    CHECK(advanceToCells(30, 10) == 3);
}

TEST_CASE("GlyphAdvance.exact_half_rounds_up", "[glyph_advance]")
{
    // Where this deliberately parts company with std::rint, whose half-to-even rounding sends a
    // half-cell advance to zero. Rounding half up keeps the floor of one cell intact.
    CHECK(advanceToCells(5, 10) == 1);
    CHECK(advanceToCells(15, 10) == 2);
}

TEST_CASE("GlyphAdvance.degenerate_cell_width", "[glyph_advance]")
{
    CHECK(advanceToCells(10, 0) == 0);
    CHECK(advanceToCells(10, -1) == 0);
}

TEST_CASE("GlyphAdvance.negative_advance_is_symmetric", "[glyph_advance]")
{
    CHECK(advanceToCells(-7, 15) == -1);
    CHECK(advanceToCells(-30, 15) == -2);
}
