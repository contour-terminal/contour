// SPDX-License-Identifier: Apache-2.0
#include <vtbackend/shell/BlockPill.hpp>

#include <vtrasterizer/ChipRenderer.hpp>
#include <vtrasterizer/RailGeometry.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <map>
#include <ranges>
#include <utility>

using namespace vtbackend;
using namespace vtrasterizer;

namespace
{
/// @p rects as a vector, to compare with one.
[[nodiscard]] std::vector<vtrasterizer::RailRect> rectsOf(vtrasterizer::RailRects const& rects)
{
    return { rects.all().begin(), rects.all().end() };
}

/// 10x20 cells, no margin, a 22-px strip with a 3-px bar at 8, and the canvas's pills at 1x.
[[nodiscard]] GridMetrics chipMetrics()
{
    auto metrics = GridMetrics { .pageSize = PageSize { LineCount(24), ColumnCount(80) },
                                 .cellSize = ImageSize { Width(10), Height(20) },
                                 .baseline = 15,
                                 .underline = { .position = 17, .thickness = 1 },
                                 .cellMargin = {},
                                 .pageMargin = { .left = 30, .top = 0, .bottom = 0 },
                                 .rail = {} };
    metrics.rail = GutterRailMetrics { .strip = 22, .bar = 3, .barInset = 8, .radius = 2, .endInset = 2 };
    metrics.rail.chips.markEdge = 13;
    metrics.rail.chips.pills[static_cast<std::size_t>(PillStyle::Failed)] =
        PillMetrics { .fontSize = 11, .weight = 600, .padX = 8, .padY = 1, .radius = 9 };
    metrics.rail.chips.pills[static_cast<std::size_t>(PillStyle::Fold)] =
        PillMetrics { .fontSize = 11, .weight = 400, .padX = 10, .height = 18, .radius = 9, .ring = 1 };
    return metrics;
}

/// The pixels @p quads cover, each with the colour last drawn on it.
[[nodiscard]] std::map<std::pair<int, int>, RGBAColor> coverage(std::vector<PillQuad> const& quads)
{
    auto pixels = std::map<std::pair<int, int>, RGBAColor> {};
    for (auto const& quad: quads)
        for (auto const y: std::views::iota(quad.y, quad.y + quad.height))
            for (auto const x: std::views::iota(quad.x, quad.x + quad.width))
                pixels[{ x, y }] = quad.color;
    return pixels;
}

constexpr auto Fill = RGBAColor(0xFF, 0x69, 0x61, 0x29);
constexpr auto Ring = RGBAColor(0xFF, 0xFF, 0xFF, 0x1F);
} // namespace

TEST_CASE("ChipRenderer.aPillIsRoundedAtBothEnds", "[renderer][chip]")
{
    auto const box = RailRect { .x = 100, .y = 40, .width = 60, .height = 18 };
    auto const quads = pillQuads(box, 9, Fill, std::nullopt, 0);
    auto const pixels = coverage(quads);

    // Nothing outside the box, every pixel drawn once in the fill's colour at no more than its alpha.
    for (auto const& quad: quads)
    {
        CHECK(quad.x >= box.x);
        CHECK(quad.y >= box.y);
        CHECK(quad.x + quad.width <= box.x + box.width);
        CHECK(quad.y + quad.height <= box.y + box.height);
        CHECK(quad.color.rgb() == Fill.rgb());
        CHECK(quad.color.alpha() <= Fill.alpha());
    }
    auto area = std::size_t { 0 };
    for (auto const& quad: quads)
        area += static_cast<std::size_t>(quad.width * quad.height);
    CHECK(area == pixels.size());

    // The corners are cut, the middle row runs edge to edge at full strength, and the ends are symmetric.
    CHECK_FALSE(pixels.contains({ 100, 40 }));
    CHECK_FALSE(pixels.contains({ 159, 57 }));
    // (A pill as tall as twice its radius has no straight row: its widest rows' ends are all but full.)
    CHECK(pixels.at({ 100, 49 }).alpha() >= Fill.alpha() - 1);
    CHECK(pixels.at({ 159, 49 }).alpha() >= Fill.alpha() - 1);
    CHECK(pixels.at({ 130, 40 }) == Fill);
    for (auto const y: std::views::iota(40, 58))
        for (auto const x: std::views::iota(100, 130))
        {
            CAPTURE(x, y);
            auto const mirrored = std::pair { 259 - x, y };
            CHECK(pixels.contains({ x, y }) == pixels.contains(mirrored));
        }
    // An edge pixel of a curved row carries the circle's coverage: fainter than the fill.
    auto const edge =
        std::ranges::find_if(quads, [](PillQuad const& quad) { return quad.color.alpha() < Fill.alpha(); });
    CHECK(edge != quads.end());
}

TEST_CASE("ChipRenderer.aSquareBoxIsOneRectangle", "[renderer][chip]")
{
    auto const box = RailRect { .x = 5, .y = 6, .width = 30, .height = 12 };
    CHECK(pillQuads(box, 0, Fill, std::nullopt, 0)
          == std::vector { PillQuad { .x = 5, .y = 6, .width = 30, .height = 12, .color = Fill } });
    CHECK(pillQuads(box, 0, std::nullopt, std::nullopt, 0).empty());
    CHECK(pillQuads(RailRect { .x = 0, .y = 0, .width = 0, .height = 12 }, 4, Fill, std::nullopt, 0).empty());
}

TEST_CASE("ChipRenderer.theRingIsInsideTheEdge", "[renderer][chip]")
{
    // The fold chip's hairline: the box less the box a ring's width inside it, drawn after the fill.
    auto const box = RailRect { .x = 0, .y = 0, .width = 80, .height = 20 };
    auto const quads = pillQuads(box, 10, Fill, Ring, 1);
    auto const firstRing =
        std::ranges::find_if(quads, [](PillQuad const& quad) { return quad.color == Ring; });
    REQUIRE(firstRing != quads.end());
    CHECK(
        std::ranges::all_of(firstRing, quads.end(), [](PillQuad const& quad) { return quad.color == Ring; }));

    auto const ring = coverage(std::vector(firstRing, quads.end()));
    CHECK(ring.contains({ 40, 0 }));  // the top edge
    CHECK(ring.contains({ 40, 19 })); // the bottom edge
    CHECK(ring.contains({ 0, 10 }));  // the left end
    CHECK(ring.contains({ 79, 10 })); // the right end
    CHECK_FALSE(ring.contains({ 40, 10 }));
    CHECK_FALSE(ring.contains({ 40, 1 }));
    CHECK_FALSE(ring.contains({ 1, 10 }));
}

TEST_CASE("ChipRenderer.aStatusPillKeepsToItsColumnsRightEdge", "[renderer][chip]")
{
    // Right-aligned in the columns placed for it, as wide as its label and padding, its height fitted to the
    // label's line and padding, centred on the row.
    auto const metrics = chipMetrics();
    auto const chip = RenderChip { .lineOffset = LineOffset(2),
                                   .columns = ColumnRange { .line = LineOffset(2),
                                                            .fromColumn = ColumnOffset(50),
                                                            .toColumn = ColumnOffset(59) },
                                   .style = PillStyle::Failed,
                                   .text = U"Failed",
                                   .ink = {},
                                   .fill = Fill,
                                   .ring = std::nullopt };
    auto const& pill = metrics.rail.chips.pills[static_cast<std::size_t>(PillStyle::Failed)];
    auto const box = pillBoxOf(chip, pill, 40, 14, metrics, 0);
    // Columns 50..59 span x 530..630 (the 30-px margin); 40 + 2 * 8 = 56 wide; 14 + 2 = 16 tall in a 20-px
    // row.
    CHECK(box == RailRect { .x = 630 - 56, .y = 40 + 2, .width = 56, .height = 16 });

    // Never wider than its columns, never taller than the row.
    CHECK(pillBoxOf(chip, pill, 200, 40, metrics, 0)
          == RailRect { .x = 530, .y = 40, .width = 100, .height = 20 });
    // It scrolls with the rows.
    CHECK(pillBoxOf(chip, pill, 40, 14, metrics, 5).y == 47);
}

TEST_CASE("ChipRenderer.aFoldPillFollowsTheCommand", "[renderer][chip]")
{
    // Left-aligned in its columns, its height the token's (size.foldChip), clamped to the row.
    auto metrics = chipMetrics();
    auto const chip = RenderChip { .lineOffset = LineOffset(0),
                                   .columns = ColumnRange { .line = LineOffset(0),
                                                            .fromColumn = ColumnOffset(10),
                                                            .toColumn = ColumnOffset(29) },
                                   .style = PillStyle::Fold,
                                   .text = U"80 lines hidden ⌄",
                                   .ink = {},
                                   .fill = Fill,
                                   .ring = Ring };
    auto pill = metrics.rail.chips.pills[static_cast<std::size_t>(PillStyle::Fold)];
    CHECK(pillBoxOf(chip, pill, 90, 14, metrics, 0)
          == RailRect { .x = 130, .y = 1, .width = 110, .height = 18 });
    pill.height = 40;
    CHECK(pillBoxOf(chip, pill, 90, 14, metrics, 0).height == 20);
}

TEST_CASE("ChipRenderer.markFacesAreTheCanvas", "[renderer][chip]")
{
    CHECK(railMarkFace(RailMark::Cross).glyph == U'✗');
    CHECK(railMarkFace(RailMark::Cross).shape == MarkShape::Disc);
    CHECK(railMarkFace(RailMark::Running).glyph == U'▸');
    CHECK(railMarkFace(RailMark::Running).shape == MarkShape::Glyph);
    CHECK(railMarkFace(RailMark::Check).glyph == U'✓');
    CHECK(railMarkFace(RailMark::UserMark).shape == MarkShape::Cells);
    for (auto const index: std::views::iota(std::size_t { 0 }, RailMarkFaces.size()))
        CHECK(static_cast<std::size_t>(RailMarkFaces[index].mark) == index);

    // A 13-px disc centred on the bar (8 + 1 into the strip that ends at 30) and on its 20-px row.
    auto const metrics = chipMetrics();
    CHECK(railMarkDiscBox(metrics, 40) == RailRect { .x = 17 - 6, .y = 43, .width = 13, .height = 13 });
    CHECK(drawsMarkInUiFont(RailMark::Cross, metrics));
    CHECK_FALSE(drawsMarkInUiFont(RailMark::UserMark, metrics));
    // Without the chip metrics -- no pills on the row -- every mark keeps its cells.
    CHECK_FALSE(drawsMarkInUiFont(RailMark::Cross, GridMetrics {}));
}

TEST_CASE("ChipRenderer.theBarResumesBelowAMark", "[renderer][chip][rail]")
{
    // m3: a mark on the head row stands in for the bar's top end only; the bar runs on below its disc.
    auto const metrics = chipMetrics();
    auto cell = RenderGutterCell {};
    cell.lineOffset = LineOffset(2); // y 40..59, the disc 43..55
    cell.mark = RailMark::Cross;

    cell.rail = RailSegment::Head;
    CHECK(rectsOf(railRects(cell, metrics, 0))
          == std::vector { RailRect { .x = 16, .y = 56, .width = 3, .height = 4 } });

    cell.rail = RailSegment::Body;
    CHECK(rectsOf(railRects(cell, metrics, 0))
          == std::vector { RailRect { .x = 16, .y = 40, .width = 3, .height = 3 },
                           RailRect { .x = 16, .y = 56, .width = 3, .height = 4 } });

    cell.rail = RailSegment::Single;
    CHECK(rectsOf(railRects(cell, metrics, 0)).empty());

    // A user mark is two cells over the whole row: no bar beside it.
    cell.rail = RailSegment::Body;
    cell.mark = RailMark::UserMark;
    CHECK(rectsOf(railRects(cell, metrics, 0)).empty());
}

TEST_CASE("ChipRenderer.cssWeightsAreFontWeights", "[renderer][chip]")
{
    CHECK(fontWeightOf(400) == text::FontWeight::Normal);
    CHECK(fontWeightOf(600) == text::FontWeight::DemiBold);
    CHECK(fontWeightOf(700) == text::FontWeight::Bold);
    CHECK(fontWeightOf(640) == text::FontWeight::DemiBold);
    CHECK(fontWeightOf(0) == text::FontWeight::Thin);
}
