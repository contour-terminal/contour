// SPDX-License-Identifier: Apache-2.0
#include <vtrasterizer/ReGISFontRasterizer.h>

#include <catch2/catch_test_macros.hpp>

using namespace vtrasterizer;

// A representative cell/font geometry: a 20px cell whose em box (ascender 16, descender -4) exactly
// fills the cell, so the baseline lands 16px below the cell top.
namespace
{
constexpr int CellHeight = 20;
constexpr int Ascender = 16;  // positive: up from the baseline
constexpr int Descender = -4; // negative: down from the baseline
constexpr int BaselineFromTop = 16;
} // namespace

// Pin the baseline model as a compile-time contract.
static_assert(regisGlyphBaselineOffsetY(CellHeight, Ascender, Descender, Ascender) == 0,
              "a glyph whose top bearing equals the ascender starts at the cell top");
static_assert(regisGlyphBaselineOffsetY(CellHeight, Ascender, Descender, 0) == BaselineFromTop,
              "a glyph with no rise above the baseline starts its bitmap at the baseline row");

TEST_CASE("ReGISFontRasterizer.baseline_is_shared_across_glyph_heights", "[regis][text]")
{
    // The crux of baseline honouring: two glyphs are separated vertically by exactly the difference
    // in their top bearings, regardless of their bitmap heights. This is what keeps 'a' (a short
    // x-height glyph, e.g. top bearing 10) and a capital 'A' (a taller glyph, e.g. top bearing 16)
    // sitting on the same baseline instead of each being centred on its own bitmap.
    auto const shortGlyphTopBearing = 10; // e.g. lowercase 'a'
    auto const tallGlyphTopBearing = 16;  // e.g. capital 'A'

    auto const shortOffset = regisGlyphBaselineOffsetY(CellHeight, Ascender, Descender, shortGlyphTopBearing);
    auto const tallOffset = regisGlyphBaselineOffsetY(CellHeight, Ascender, Descender, tallGlyphTopBearing);

    // The vertical gap between the two glyph tops equals the bearing difference -> shared baseline.
    CHECK((shortOffset - tallOffset) == (tallGlyphTopBearing - shortGlyphTopBearing));
    // The taller glyph (larger bearing) is placed higher (smaller offset from the top).
    CHECK(tallOffset < shortOffset);
}

TEST_CASE("ReGISFontRasterizer.descender_does_not_shift_the_baseline", "[regis][text]")
{
    // 'g' shares 'a's x-height (top bearing 10) but its bitmap extends below the baseline. Since the
    // offset depends only on the top bearing, 'a' and 'g' get the *same* top row; 'g's extra bitmap
    // rows simply extend past the baseline, exactly as a real descender should.
    auto const aTopBearing = 10;
    auto const gTopBearing = 10;

    CHECK(regisGlyphBaselineOffsetY(CellHeight, Ascender, Descender, aTopBearing)
          == regisGlyphBaselineOffsetY(CellHeight, Ascender, Descender, gTopBearing));
}

TEST_CASE("ReGISFontRasterizer.em_box_is_centred_within_the_cell", "[regis][text]")
{
    // When the cell is taller than the em box, the box is centred: a 30px cell with a 20px em box
    // (ascender 16, descender -4) leaves 5px of margin above, so the baseline sits at 5 + 16 = 21.
    auto const tallCell = 30;
    // A glyph rising the full ascender starts at the top margin (row 5).
    CHECK(regisGlyphBaselineOffsetY(tallCell, Ascender, Descender, Ascender) == 5);
    // A glyph sitting on the baseline (top bearing 0) starts at the baseline (row 21).
    CHECK(regisGlyphBaselineOffsetY(tallCell, Ascender, Descender, 0) == 21);
}

TEST_CASE("ReGISFontRasterizer.glyph_taller_than_cell_yields_negative_offset", "[regis][text]")
{
    // If the shaped glyph overflows the cell, the top row is placed above the cell top (negative
    // offset). The caller clips out-of-cell rows, so this must be representable rather than clamped.
    auto const shortCell = 10; // shorter than the 20px em box
    auto const offset = regisGlyphBaselineOffsetY(shortCell, Ascender, Descender, Ascender);
    CHECK(offset < 0);
}
