// SPDX-License-Identifier: Apache-2.0
//
// Bitmap-level tests for the underline family of cell decorations.
//
// These drive the real DecorationRenderer headlessly: setTextureAtlas() triggers
// initializeDirectMapping(), which rasterizes every Decorator once and uploads it. The uploads land
// in each_element<Decorator>() order, so the enumerator is the index, and what the GPU would receive
// is exactly what these tests read back.

#include <vtrasterizer/DecorationRenderer.h>
#include <vtrasterizer/GridMetrics.h>
#include <vtrasterizer/RendererTestHelpers.h>
#include <vtrasterizer/TextureAtlas.h>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <format>
#include <ranges>
#include <string>
#include <vector>

using namespace vtbackend;
using namespace vtrasterizer;

namespace
{

/// The grid metrics Contour derives from Nimbus Mono PS at 12pt/96dpi -- the font from issue #1754.
///
/// Reproduced rather than invented: the font declares (upem 1000) hhea ascender 603, descender -397
/// and lineGap 200, so at 16 ppem FreeType reports lineHeight 20 and ascender 10, giving
/// `baseline = lineHeight - ascender = 10` and `underline.position = baseline + (-1) = 9`. The
/// lineGap is what makes this font interesting: 4 of those 10 pixels are leading, not descent.
constexpr auto NimbusCellSize = ImageSize { Width(10), Height(20) };
constexpr auto NimbusBaseline = 10;
constexpr auto NimbusUnderlinePosition = 9;

[[nodiscard]] GridMetrics gridMetricsFor(ImageSize cellSize, int baseline, int position, int thickness)
{
    return GridMetrics { .pageSize = PageSize { LineCount(24), ColumnCount(80) },
                         .cellSize = cellSize,
                         .baseline = baseline,
                         .underline = { .position = position, .thickness = thickness } };
}

/// Holds the renderer and everything it borrows, so a test gets its uploads in one expression.
///
/// The atlas must outlive the renderer's use of it and the render target must outlive the atlas,
/// which is why these are members rather than locals in a helper function.
class DecorationProbe
{
  public:
    explicit DecorationProbe(GridMetrics const& gridMetrics):
        _gridMetrics { gridMetrics },
        _renderer { _gridMetrics, Decorator::DottedUnderline, Decorator::Underline }
    {
        _renderer.setRenderTarget(_renderTarget, _allocator);
        _atlas = std::make_unique<Renderable::TextureAtlas>(
            _renderTarget.textureScheduler(),
            atlas::AtlasProperties { .format = atlas::Format::RGBA,
                                     .tileSize = _gridMetrics.cellSize,
                                     .hashCount = crispy::strong_hashtable_size { 64 },
                                     .tileCount = crispy::lru_capacity { 64 },
                                     .directMappingCount = _allocator.currentlyAllocatedCount });
        _renderer.setTextureAtlas(*_atlas);
    }

    /// The tile rasterized for @p decoration, as it was handed to the backend.
    ///
    /// Not const: MockRenderTarget hands out its recorded commands through a non-const accessor.
    [[nodiscard]] atlas::UploadTile const& upload(Decorator decoration)
    {
        auto const& uploads = _renderTarget.getMockBackend().uploadCommands;
        REQUIRE(uploads.size() == std::numeric_limits<Decorator>::count());
        return uploads[static_cast<size_t>(decoration)];
    }

  private:
    GridMetrics _gridMetrics;
    MockRenderTarget _renderTarget;
    Renderable::DirectMappingAllocator _allocator {};
    DecorationRenderer _renderer;
    std::unique_ptr<Renderable::TextureAtlas> _atlas;
};

/// Row @p row of @p tile as '#' (ink) and '.' (clear), top-origin, the way the bitmap is stored.
[[nodiscard]] std::string rowOf(atlas::UploadTile const& tile, int row)
{
    auto const width = unbox<size_t>(tile.bitmapSize.width);
    auto text = std::string {};
    for (auto const x: std::views::iota(0uz, width))
        text += tile.bitmap[(static_cast<size_t>(row) * width) + x] > 0 ? '#' : '.';
    return text;
}

/// Every row of @p tile, so a failing CHECK prints the decoration instead of a number.
[[nodiscard]] std::vector<std::string> renderedRows(atlas::UploadTile const& tile)
{
    auto rows = std::vector<std::string> {};
    for (auto const y: std::views::iota(0, unbox<int>(tile.bitmapSize.height)))
        rows.emplace_back(rowOf(tile, y));
    return rows;
}

/// Cell-bottom offset of the highest inked row, or 0 when the tile is blank.
///
/// A decoration tile's bottom edge IS the cell's bottom edge (DecorationRenderer::renderDecoration
/// draws it at `cellBottom - bitmapHeight`), so top-origin row `r` of a tile `H` tall sits at
/// cell-bottom offset `H - r`. That identity is the whole reason these tests can talk about the
/// baseline at all.
[[nodiscard]] int topmostInkOffset(atlas::UploadTile const& tile)
{
    auto const height = unbox<int>(tile.bitmapSize.height);
    for (auto const y: std::views::iota(0, height))
        if (rowOf(tile, y).contains('#'))
            return height - y;
    return 0;
}

/// Cell-bottom offset of the lowest inked row, or 0 when the tile is blank.
[[nodiscard]] int bottommostInkOffset(atlas::UploadTile const& tile)
{
    auto const height = unbox<int>(tile.bitmapSize.height);
    for (auto const y: std::views::iota(0, height) | std::views::reverse)
        if (rowOf(tile, y).contains('#'))
            return height - y;
    return 0;
}

/// How many rows of @p tile carry any ink.
[[nodiscard]] int inkedRows(atlas::UploadTile const& tile)
{
    auto count = 0;
    for (auto const y: std::views::iota(0, unbox<int>(tile.bitmapSize.height)))
        if (rowOf(tile, y).contains('#'))
            ++count;
    return count;
}

/// How many columns of @p tile's LEFT half carry any ink -- the width of a dotted underline's first
/// dot, the second one starting at the half-way mark.
[[nodiscard]] int inkedColumnsInFirstHalf(atlas::UploadTile const& tile)
{
    auto const rows = std::views::iota(0, unbox<int>(tile.bitmapSize.height));
    auto count = 0;
    for (auto const x: std::views::iota(0, unbox<int>(tile.bitmapSize.width) / 2))
        if (std::ranges::any_of(rows, [&](int y) { return rowOf(tile, y)[static_cast<size_t>(x)] == '#'; }))
            ++count;
    return count;
}

[[nodiscard]] size_t litPixels(atlas::UploadTile const& tile)
{
    return static_cast<size_t>(std::ranges::count_if(tile.bitmap, [](uint8_t v) { return v > 0; }));
}

/// The decorations that are supposed to live below the baseline, in the cell's descender region.
constexpr auto UnderlineFamily = std::array {
    Decorator::Underline,       Decorator::DoubleUnderline, Decorator::CurlyUnderline,
    Decorator::DottedUnderline, Decorator::DashedUnderline,
};

} // namespace

TEST_CASE("DecorationRenderer.underlineFamilyStaysBelowTheBaseline", "[decoration]")
{
    // The invariant the whole family shares: a straight underline puts its topmost ink at exactly
    // `underline.position` above the cell bottom, and no sibling may reach higher -- reaching higher
    // means reaching the baseline, which means painting into the glyphs. Asserted in a loop so a
    // sixth decorator is a row in the table above rather than a sixth place to get this wrong.
    auto const gridMetrics =
        gridMetricsFor(NimbusCellSize, NimbusBaseline, NimbusUnderlinePosition, /*thickness*/ 1);
    auto probe = DecorationProbe { gridMetrics };

    for (auto const decoration: UnderlineFamily)
    {
        auto const& tile = probe.upload(decoration);
        INFO(std::format("{} tile {} in cell {}, baseline {}, underline position {}",
                         decoration,
                         tile.bitmapSize,
                         gridMetrics.cellSize,
                         gridMetrics.baseline,
                         gridMetrics.underline.position));
        for (auto const& row: renderedRows(tile))
            UNSCOPED_INFO(row);

        CHECK(litPixels(tile) > 0);
        CHECK(topmostInkOffset(tile) <= gridMetrics.underline.position);
        CHECK(unbox<int>(tile.bitmapSize.height) <= unbox<int>(gridMetrics.cellSize.height));
    }
}

TEST_CASE("DecorationRenderer.curlyUnderlineIsNotAsTallAsTheDescender", "[decoration]")
{
    // Regression for #1754. The wave used to be sized from GridMetrics::baseline -- the whole
    // descender PLUS the font's line gap -- so on Nimbus Mono PS it stood 9px tall in a 20px cell
    // (taller than the x-height) with its crest exactly on the baseline. It must now be bounded by
    // the room below the underline position instead.
    auto const gridMetrics =
        gridMetricsFor(NimbusCellSize, NimbusBaseline, NimbusUnderlinePosition, /*thickness*/ 1);
    auto probe = DecorationProbe { gridMetrics };
    auto const& tile = probe.upload(Decorator::CurlyUnderline);

    for (auto const& row: renderedRows(tile))
        UNSCOPED_INFO(row);

    auto const top = topmostInkOffset(tile);
    auto const bottom = bottommostInkOffset(tile);
    auto const bandHeight = top - bottom + 1;

    INFO(std::format("wave occupies cell-bottom offsets {}..{} ({} rows); baseline is at {}",
                     bottom,
                     top,
                     bandHeight,
                     gridMetrics.baseline));

    // It never reaches the baseline, and it sits no higher than a straight underline.
    CHECK(top <= gridMetrics.underline.position);
    CHECK(top < gridMetrics.baseline);

    // Bounded by the room beneath the underline position rather than by the descender: at most half
    // of it, which is the bound kitty settled on ("4 so as to be not too large") and which puts this
    // font's wave at 5 rows rather than 9.
    CHECK(bandHeight <= (gridMetrics.underline.position / 2) + 1);
    CHECK(bottom >= 1);
}

TEST_CASE("DecorationRenderer.curlyUnderlineIsActuallyCurly", "[decoration]")
{
    // A test that only measured the wave's extent would pass just as happily on a straight line, so
    // prove the shape engages: the wave is one cosine cycle per cell with its crest at x = 0, so the
    // topmost inked row must be lit at the cell's edges and the bottom-most at its centre.
    auto const gridMetrics =
        gridMetricsFor(NimbusCellSize, NimbusBaseline, NimbusUnderlinePosition, /*thickness*/ 1);
    auto probe = DecorationProbe { gridMetrics };
    auto const& tile = probe.upload(Decorator::CurlyUnderline);

    for (auto const& row: renderedRows(tile))
        UNSCOPED_INFO(row);

    auto const height = unbox<int>(tile.bitmapSize.height);
    auto const width = unbox<int>(tile.bitmapSize.width);
    auto const crestRow = rowOf(tile, height - topmostInkOffset(tile));
    auto const troughRow = rowOf(tile, height - bottommostInkOffset(tile));

    INFO(std::format("crest '{}' trough '{}'", crestRow, troughRow));

    CHECK(crestRow.front() == '#');
    CHECK(crestRow[static_cast<size_t>(width / 2)] == '.');
    CHECK(troughRow.front() == '.');
    CHECK(troughRow[static_cast<size_t>(width / 2)] == '#');
}

TEST_CASE("DecorationRenderer.curlyUnderlineHonorsUnderlineThickness", "[decoration]")
{
    // The stroke used to be spread along x rather than y, so the font's underline thickness reached
    // the wave in name only. A thicker underline must produce a heavier wave.
    auto thin = DecorationProbe { gridMetricsFor(NimbusCellSize, NimbusBaseline, 9, 1) };
    auto thick = DecorationProbe { gridMetricsFor(NimbusCellSize, NimbusBaseline, 9, 5) };

    CHECK(litPixels(thick.upload(Decorator::CurlyUnderline))
          > litPixels(thin.upload(Decorator::CurlyUnderline)));
}

TEST_CASE("DecorationRenderer.dottedUnderlineSurvivesAShallowUnderlinePosition", "[decoration]")
{
    // The dot origin was computed as `(unsigned) position - thickness`, which wraps whenever a font
    // puts its underline CLOSER to the cell bottom than one thickness. Most of the dot's rows then
    // addressed a wrapped-around y that Pixmap::paint dropped, leaving a dot that was still as WIDE
    // as the thickness asked but only as tall as whatever survived -- a dash, not a dot. At
    // position 0 nothing survived at all.
    //
    // Squareness is therefore the assertion, not mere presence: a test that only counted lit pixels
    // would pass on either side of the fix.
    SECTION("underline closer to the cell bottom than one thickness")
    {
        auto const gridMetrics = gridMetricsFor(
            ImageSize { Width(10), Height(16) }, /*baseline*/ 2, /*position*/ 1, /*thickness*/ 3);
        auto probe = DecorationProbe { gridMetrics };
        auto const& tile = probe.upload(Decorator::DottedUnderline);

        for (auto const& row: renderedRows(tile))
            UNSCOPED_INFO(row);

        CHECK(litPixels(tile) > 0);
        CHECK(inkedRows(tile) == inkedColumnsInFirstHalf(tile)); // the dot is square
        // The dot shrinks to the room it has rather than overhanging it, so the anchor still holds.
        CHECK(topmostInkOffset(tile) == gridMetrics.underline.position);
    }

    SECTION("underline flush with the cell bottom")
    {
        auto const gridMetrics = gridMetricsFor(
            ImageSize { Width(10), Height(16) }, /*baseline*/ 1, /*position*/ 0, /*thickness*/ 1);
        auto probe = DecorationProbe { gridMetrics };
        auto const& tile = probe.upload(Decorator::DottedUnderline);

        for (auto const& row: renderedRows(tile))
            UNSCOPED_INFO(row);

        CHECK(litPixels(tile) > 0); // drew nothing whatsoever before the fix
    }
}

TEST_CASE("DecorationRenderer.doubleUnderlineIsTwoSeparatedStrokes", "[decoration]")
{
    // Both strokes must sit below the underline position -- the upper one used to be placed at
    // `position + 2 * thickness`, i.e. inside the glyph body on a font like Nimbus Mono PS -- and
    // there must still be a visible gap between them, or it is not a double underline.
    auto const gridMetrics =
        gridMetricsFor(NimbusCellSize, NimbusBaseline, NimbusUnderlinePosition, /*thickness*/ 1);
    auto probe = DecorationProbe { gridMetrics };
    auto const& tile = probe.upload(Decorator::DoubleUnderline);

    auto const rows = renderedRows(tile);
    for (auto const& row: rows)
        UNSCOPED_INFO(row);

    CHECK(topmostInkOffset(tile) <= gridMetrics.underline.position);

    // Two runs of inked rows, separated by at least one clear row.
    auto inkedRuns = 0;
    auto previousWasInked = false;
    for (auto const& row: rows)
    {
        auto const inked = row.contains('#');
        if (inked && !previousWasInked)
            ++inkedRuns;
        previousWasInked = inked;
    }
    CHECK(inkedRuns == 2);
}

TEST_CASE("DecorationRenderer.doubleUnderlineThicknessFollowsTwoThirds", "[decoration]")
{
    // `unsigned(ceil(t * 2.0) / 3.0)` truncated where `ceil(t * 2.0 / 3.0)` was meant, so an even
    // underline thickness produced a stroke one pixel too thin: at t=2 it yielded 1 instead of 2.
    auto const gridMetrics = gridMetricsFor(
        ImageSize { Width(10), Height(40) }, /*baseline*/ 18, /*position*/ 16, /*thickness*/ 2);
    auto probe = DecorationProbe { gridMetrics };
    auto const& tile = probe.upload(Decorator::DoubleUnderline);

    auto const rows = renderedRows(tile);
    for (auto const& row: rows)
        UNSCOPED_INFO(row);

    auto const inkedRows = std::ranges::count_if(rows, [](auto const& row) { return row.contains('#'); });
    CHECK(inkedRows == 4); // two strokes of ceil(2 * 2/3) == 2 rows each
}
