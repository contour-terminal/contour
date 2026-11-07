// SPDX-License-Identifier: Apache-2.0
#include <vtrasterizer/RunPen.hpp>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <span>
#include <vector>

using namespace vtrasterizer;

namespace
{

/// A shaped glyph, reduced to what the pen reads from it.
struct ShapedGlyph
{
    unsigned cluster;
    int advance;
};

/// Walks @p glyphs through a fresh RunPen and returns the x each one is drawn at.
[[nodiscard]] std::vector<int> penPositions(std::span<ShapedGlyph const> glyphs,
                                            int cellWidth,
                                            int advanceScale = 1)
{
    auto pen = RunPen { cellWidth, advanceScale };
    auto positions = std::vector<int> {};
    for (auto const& glyph: glyphs)
        positions.emplace_back(pen.place(glyph.cluster, glyph.advance));
    return positions;
}

} // namespace

TEST_CASE("RunPen.one_glyph_per_cell_lands_on_each_cell", "[run_pen]")
{
    auto constexpr Glyphs =
        std::array { ShapedGlyph { 0, 10 }, ShapedGlyph { 1, 10 }, ShapedGlyph { 2, 10 } };
    CHECK(penPositions(Glyphs, 10) == std::vector { 0, 10, 20 });
}

TEST_CASE("RunPen.a_glyph_wider_than_its_cell_does_not_push_the_next_cell", "[run_pen]")
{
    // Issue #452: the stopwatch is one cell on the grid, but Segoe UI Emoji advances it by more than
    // two. The glyph after it belongs to the next cell regardless.
    auto constexpr Glyphs =
        std::array { ShapedGlyph { 0, 27 }, ShapedGlyph { 1, 12 }, ShapedGlyph { 2, 12 } };
    CHECK(penPositions(Glyphs, 12) == std::vector { 0, 12, 24 });
}

TEST_CASE("RunPen.a_glyph_narrower_than_its_cell_does_not_pull_the_next_cell", "[run_pen]")
{
    // Issue #1939: a proportional fallback font advances its non-breaking space by 3px in a 10px cell.
    auto constexpr Glyphs = std::array { ShapedGlyph { 0, 3 }, ShapedGlyph { 1, 10 } };
    CHECK(penPositions(Glyphs, 10) == std::vector { 0, 10 });
}

TEST_CASE("RunPen.a_ligature_covering_two_cells_leaves_the_next_glyph_on_the_third", "[run_pen]")
{
    // HarfBuzz merges the clusters of a ligature into its first one, so the glyph for "->" names cell 0,
    // and the glyph after it names cell 2.
    auto constexpr Glyphs = std::array { ShapedGlyph { 0, 20 }, ShapedGlyph { 2, 10 } };
    CHECK(penPositions(Glyphs, 10) == std::vector { 0, 20 });
}

TEST_CASE("RunPen.glyphs_within_one_cluster_follow_the_font_advance", "[run_pen]")
{
    // A base and its combining mark share a cluster. The font positions the mark relative to the pen
    // after the base's advance -- 7px here, not the 10px cell -- so that is where the pen must be when
    // the mark is placed. The next cluster still starts on its own cell.
    auto constexpr Glyphs = std::array { ShapedGlyph { 0, 7 }, ShapedGlyph { 0, 0 }, ShapedGlyph { 1, 7 } };
    CHECK(penPositions(Glyphs, 10) == std::vector { 0, 7, 10 });
}

TEST_CASE("RunPen.a_double_width_line_doubles_the_advances_within_a_cluster", "[run_pen]")
{
    // Clusters are display columns, two per character on a double-width line, so the next character's
    // cluster is 2 and already places it; only the font's advances need stretching to match its glyphs.
    auto constexpr Glyphs = std::array { ShapedGlyph { 0, 7 }, ShapedGlyph { 0, 0 }, ShapedGlyph { 2, 7 } };
    CHECK(penPositions(Glyphs, 10, 2) == std::vector { 0, 14, 20 });
}
