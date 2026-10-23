// SPDX-License-Identifier: Apache-2.0
//
// Bitmap-level tests for the underline family of cell decorations.
//
// These drive the real DecorationRenderer headlessly: setTextureAtlas() triggers
// initializeDirectMapping(), which rasterizes every Decorator once and uploads it. The uploads land
// in eachElement<Decorator>() order, so the enumerator is the index, and what the GPU would receive
// is exactly what these tests read back. The dotted underline's further phase tiles follow them.

#include <vtrasterizer/DecorationRenderer.hpp>
#include <vtrasterizer/GridMetrics.hpp>
#include <vtrasterizer/RendererTestHelpers.hpp>
#include <vtrasterizer/TextureAtlas.hpp>
#include <vtrasterizer/UnderlineGeometry.hpp>

#include <core/Utils.hpp>

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include <algorithm>
#include <format>
#include <iterator>
#include <optional>
#include <ranges>
#include <span>
#include <string>
#include <vector>

using namespace vtbackend;
using namespace vtrasterizer;
using core::eachElement;

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
    /// @param gridMetrics The grid the decorations are drawn on.
    /// @param hyperlinkNormal The decorator a link at rest is drawn with.
    /// @param hyperlinkHover The decorator the link under the pointer is drawn with.
    explicit DecorationProbe(GridMetrics const& gridMetrics,
                             Decorator hyperlinkNormal = Decorator::DottedUnderline,
                             Decorator hyperlinkHover = Decorator::Underline):
        _gridMetrics { gridMetrics }, _renderer { _gridMetrics, hyperlinkNormal, hyperlinkHover }
    {
        _renderer.setRenderTarget(_renderTarget, _allocator);
        _atlas = std::make_unique<Renderable::TextureAtlas>(
            _renderTarget.textureScheduler(),
            atlas::AtlasProperties { .format = atlas::Format::RGBA,
                                     .tileSize = _gridMetrics.cellSize,
                                     .hashCount = crispy::StrongHashtableSize { 64 },
                                     .tileCount = crispy::LRUCapacity { 64 },
                                     .directMappingCount = _allocator.currentlyAllocatedCount });
        _renderer.setTextureAtlas(*_atlas);
    }

    /// The tile rasterized for @p decoration, as it was handed to the backend.
    ///
    /// Not const: MockRenderTarget hands out its recorded commands through a non-const accessor.
    [[nodiscard]] atlas::UploadTile const& upload(Decorator decoration)
    {
        auto const& uploads = _renderTarget.getMockBackend().uploadCommands;
        REQUIRE(uploads.size() >= std::numeric_limits<Decorator>::count());
        return uploads[static_cast<size_t>(decoration)];
    }

    /// The dotted underline's tile for @p phase, as it was handed to the backend.
    ///
    /// Phase 0 is the DottedUnderline tile itself; phase `p >= 1` was uploaded after every
    /// Decorator's own tile, at `count + p - 1`.
    ///
    /// @param phase The phase, in `[0, phaseCount)`.
    [[nodiscard]] atlas::UploadTile const& uploadDottedPhase(int phase)
    {
        if (phase == 0)
            return upload(Decorator::DottedUnderline);
        auto const& uploads = _renderTarget.getMockBackend().uploadCommands;
        auto const index = std::numeric_limits<Decorator>::count() + static_cast<size_t>(phase) - 1;
        REQUIRE(uploads.size() > index);
        return uploads[index];
    }

    /// Draws @p cell and names the dotted underline phase tiles that reached the backend for it.
    ///
    /// @param cell The cell to draw.
    /// @param phaseCount How many phase tiles the dotted underline has.
    /// @return One phase per drawn column that sampled a dotted underline tile, in the order drawn.
    [[nodiscard]] std::vector<int> dottedPhasesDrawnFor(vtbackend::RenderCell const& cell, int phaseCount)
    {
        auto const& drawn = _renderTarget.getMockBackend().renderCommands;
        auto const before = drawn.size();
        _renderer.renderCell(cell);

        auto phases = std::vector<int> {};
        for (auto const& command: std::span(drawn).subspan(before))
            for (auto const phase: std::views::iota(0, phaseCount))
            {
                auto const& tile = uploadDottedPhase(phase).location;
                if (tile.x.value == command.tileLocation.x.value
                    && tile.y.value == command.tileLocation.y.value)
                    phases.push_back(phase);
            }
        return phases;
    }

    /// Draws @p cell and names the decorations that reached the backend for it, in the order drawn.
    ///
    /// A decoration is told apart by the atlas tile it samples: every Decorator has a tile of its own, and
    /// upload() has where each one lives.
    ///
    /// @param cell The cell to draw.
    /// @return One entry per decorated column, so a cell of width N draws each of its decorations N times.
    [[nodiscard]] std::vector<Decorator> decorationsDrawnFor(vtbackend::RenderCell const& cell)
    {
        auto const& drawn = _renderTarget.getMockBackend().renderCommands;
        auto const before = drawn.size();
        _renderer.renderCell(cell);

        auto decorations = std::vector<Decorator> {};
        for (auto const& command: std::span(drawn).subspan(before))
            for (auto const decoration: eachElement<Decorator>())
            {
                auto const& tile = upload(decoration).location;
                if (tile.x.value == command.tileLocation.x.value
                    && tile.y.value == command.tileLocation.y.value)
                    decorations.push_back(decoration);
            }
        return decorations;
    }

  private:
    GridMetrics _gridMetrics;
    MockRenderTarget _renderTarget;
    Renderable::DirectMappingAllocator _allocator {};
    DecorationRenderer _renderer;
    std::unique_ptr<Renderable::TextureAtlas> _atlas;
};

/// A decoration as rendered: one '#'/'.' string per row, top-origin.
///
/// Every measurement below reads these rows rather than the bitmap, so the tile is walked once and
/// a failing CHECK can print the decoration instead of a number. @see bitmapRows.
using Rendered = std::vector<std::string>;

/// Cell-bottom offset of the highest inked row, or 0 when the tile is blank.
///
/// A decoration tile's bottom edge IS the cell's bottom edge (DecorationRenderer::renderDecoration
/// draws it at `cellBottom - bitmapHeight`), so top-origin row `r` of a tile `H` tall sits at
/// cell-bottom offset `H - r`. That identity is the whole reason these tests can talk about the
/// baseline at all.
[[nodiscard]] int topmostInkOffset(Rendered const& rows)
{
    auto const height = static_cast<int>(rows.size());
    for (auto const y: std::views::iota(0, height))
        if (rows[static_cast<size_t>(y)].contains('#'))
            return height - y;
    return 0;
}

/// Cell-bottom offset of the lowest inked row, or 0 when the tile is blank.
[[nodiscard]] int bottommostInkOffset(Rendered const& rows)
{
    auto const height = static_cast<int>(rows.size());
    for (auto const y: std::views::iota(0, height) | std::views::reverse)
        if (rows[static_cast<size_t>(y)].contains('#'))
            return height - y;
    return 0;
}

/// How many rows carry any ink.
[[nodiscard]] int inkedRows(Rendered const& rows)
{
    return static_cast<int>(std::ranges::count_if(rows, [](auto const& row) { return row.contains('#'); }));
}

/// Whether any row carries ink in column @p x.
[[nodiscard]] bool columnIsInked(Rendered const& rows, size_t x)
{
    return std::ranges::any_of(rows, [x](auto const& row) { return x < row.size() && row[x] == '#'; });
}

/// How many columns the first contiguous run of inked columns spans -- the width of a dotted
/// underline's first dot.
[[nodiscard]] int firstInkedColumnRun(Rendered const& rows)
{
    if (rows.empty())
        return 0;
    auto const columns = std::views::iota(0uz, rows.front().size());
    auto const isInked = [&rows](size_t x) {
        return columnIsInked(rows, x);
    };
    auto const first = std::ranges::find_if(columns, isInked);
    auto const end = std::ranges::find_if_not(std::ranges::subrange(first, columns.end()), isInked);
    return static_cast<int>(std::ranges::distance(first, end));
}

/// The columns at which a run of ink starts in @p row.
[[nodiscard]] std::vector<int> inkRunStarts(std::string const& row)
{
    auto starts = std::vector<int> {};
    for (auto const x: std::views::iota(0uz, row.size()))
        if (row[x] == '#' && (x == 0 || row[x - 1] != '#'))
            starts.push_back(static_cast<int>(x));
    return starts;
}

/// The rows of several tiles of equal height, laid side by side.
[[nodiscard]] Rendered joinedSideBySide(std::span<Rendered const> tiles)
{
    auto joined = Rendered(tiles.empty() ? 0 : tiles.front().size());
    for (auto const& tile: tiles)
        for (auto const y: std::views::iota(0uz, std::min(joined.size(), tile.size())))
            joined[y] += tile[y];
    return joined;
}

[[nodiscard]] int litPixels(Rendered const& rows)
{
    auto count = 0;
    for (auto const& row: rows)
        count += static_cast<int>(std::ranges::count(row, '#'));
    return count;
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
        auto const rows = bitmapRows(tile);
        INFO(std::format("{} tile {} in cell {}, baseline {}, underline position {}",
                         decoration,
                         tile.bitmapSize,
                         gridMetrics.cellSize,
                         gridMetrics.baseline,
                         gridMetrics.underline.position));
        for (auto const& row: rows)
            UNSCOPED_INFO(row);

        CHECK(litPixels(rows) > 0);
        CHECK(topmostInkOffset(rows) <= gridMetrics.underline.position);
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
    auto const rows = bitmapRows(probe.upload(Decorator::CurlyUnderline));

    for (auto const& row: rows)
        UNSCOPED_INFO(row);

    auto const top = topmostInkOffset(rows);
    auto const bottom = bottommostInkOffset(rows);
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
    auto const rows = bitmapRows(probe.upload(Decorator::CurlyUnderline));

    for (auto const& row: rows)
        UNSCOPED_INFO(row);

    auto const height = static_cast<int>(rows.size());
    auto const width = static_cast<int>(rows.front().size());
    auto const& crestRow = rows[static_cast<size_t>(height - topmostInkOffset(rows))];
    auto const& troughRow = rows[static_cast<size_t>(height - bottommostInkOffset(rows))];

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

    CHECK(litPixels(bitmapRows(thick.upload(Decorator::CurlyUnderline)))
          > litPixels(bitmapRows(thin.upload(Decorator::CurlyUnderline))));
}

TEST_CASE("DecorationRenderer.dottedUnderlineSurvivesAShallowUnderlinePosition", "[decoration]")
{
    // The dot origin was computed as `(unsigned) position - thickness`, which wraps whenever a font
    // puts its underline CLOSER to the cell bottom than one thickness. Most of the dot's rows then
    // addressed a wrapped-around y that Pixmap::paint dropped, leaving a dot that was still as WIDE
    // as the thickness asked but only as tall as whatever survived -- a dash, not a dot. At
    // position 0 nothing survived at all.
    //
    // Roundness -- as tall as it is wide -- is therefore the assertion, not mere presence: a test
    // that only counted lit pixels would pass on either side of the fix.
    SECTION("underline closer to the cell bottom than one thickness")
    {
        auto const gridMetrics = gridMetricsFor(
            ImageSize { Width(10), Height(16) }, /*baseline*/ 2, /*position*/ 1, /*thickness*/ 3);
        auto probe = DecorationProbe { gridMetrics };
        auto const rows = bitmapRows(probe.upload(Decorator::DottedUnderline));

        for (auto const& row: rows)
            UNSCOPED_INFO(row);

        CHECK(litPixels(rows) > 0);
        CHECK(inkedRows(rows) == firstInkedColumnRun(rows)); // the first dot is as tall as it is wide
        // The dot shrinks to the room it has rather than overhanging it, so the anchor still holds.
        CHECK(topmostInkOffset(rows) == gridMetrics.underline.position);
    }

    SECTION("underline flush with the cell bottom")
    {
        auto const gridMetrics = gridMetricsFor(
            ImageSize { Width(10), Height(16) }, /*baseline*/ 1, /*position*/ 0, /*thickness*/ 1);
        auto probe = DecorationProbe { gridMetrics };
        auto const rows = bitmapRows(probe.upload(Decorator::DottedUnderline));

        for (auto const& row: rows)
            UNSCOPED_INFO(row);

        CHECK(litPixels(rows) > 0); // drew nothing whatsoever before the fix
    }
}

TEST_CASE("DecorationRenderer.dottedUnderlineContinuesAcrossCells", "[decoration]")
{
    // A pitch that does not divide the cell width used to restart at every cell, so the gap at a
    // cell boundary differed from the gap inside it. Laid side by side, column `c` drawing phase
    // `c % phaseCount`, the dots must instead march on one unbroken pitch.
    auto const [width, thickness, expectedPhaseCount] = GENERATE(table<int, int, int>({
        { 10, 1, 2 }, // pitch 4
        { 9, 1, 4 },  // pitch 4
        { 10, 2, 3 }, // pitch 6
        { 9, 2, 2 },  // pitch 6
    }));
    auto const gridMetrics = gridMetricsFor(
        ImageSize { Width(width), Height(20) }, NimbusBaseline, NimbusUnderlinePosition, thickness);
    auto const geometry = dottedUnderlineGeometry(width, NimbusUnderlinePosition, thickness);
    auto probe = DecorationProbe { gridMetrics };

    CHECK(geometry.phaseCount == expectedPhaseCount);

    auto tiles = std::vector<Rendered> {};
    for (auto const column: std::views::iota(0, 8))
        tiles.push_back(bitmapRows(probe.uploadDottedPhase(column % geometry.phaseCount)));
    auto const joined = joinedSideBySide(tiles);

    for (auto const& row: joined)
        UNSCOPED_INFO(row);
    INFO(std::format("cell width {}, pitch {}, {} phases", width, geometry.pitch, geometry.phaseCount));

    REQUIRE_FALSE(joined.empty());
    auto const& darkest = *std::ranges::max_element(
        joined, {}, [](std::string const& row) { return std::ranges::count(row, '#'); });
    auto const starts = inkRunStarts(darkest);
    INFO(std::format("darkest row '{}'", darkest));

    REQUIRE(starts.size() >= 2);
    CHECK(starts.front() == 0);
    for (auto const i: std::views::iota(1uz, starts.size()))
        CHECK(starts[i] - starts[i - 1] == geometry.pitch);
}

TEST_CASE("DecorationRenderer.dottedUnderlineDotsAreAntialiased", "[decoration]")
{
    // Round dots need partial coverage at their rim; a tile of only 0 and 255 is a square stamp.
    auto const gridMetrics =
        gridMetricsFor(NimbusCellSize, NimbusBaseline, NimbusUnderlinePosition, /*thickness*/ 2);
    auto probe = DecorationProbe { gridMetrics };
    auto const& tile = probe.upload(Decorator::DottedUnderline);

    for (auto const& row: bitmapRows(tile))
        UNSCOPED_INFO(row);

    // The raw bitmap, not bitmapRows: the latter folds every non-zero coverage into '#'.
    CHECK(std::ranges::any_of(tile.bitmap, [](auto value) { return value > 0 && value < 255; }));
    CHECK(topmostInkOffset(bitmapRows(tile)) == gridMetrics.underline.position);
}

TEST_CASE("DecorationRenderer.dottedUnderlineContinuesOnADoubleWidthLine", "[decoration]")
{
    // A double-width cell spans two display columns, and the phases must follow those display
    // columns -- neither the grid cell's own index nor that index scaled a second time.
    // RenderBufferBuilder::displayColumn() emits grid cell N of a DECDWL line at display column 2N.
    auto const gridMetrics = gridMetricsFor(
        ImageSize { Width(9), Height(20) }, NimbusBaseline, NimbusUnderlinePosition, /*thickness*/ 1);
    auto const geometry = dottedUnderlineGeometry(9, NimbusUnderlinePosition, 1);
    REQUIRE(geometry.phaseCount == 4);
    auto probe = DecorationProbe { gridMetrics };

    auto phases = std::vector<int> {};
    for (auto const displayColumn: { 0, 2 })
    {
        auto cell = vtbackend::RenderCell {};
        cell.width = 1;
        cell.position.column = ColumnOffset(displayColumn);
        cell.attributes.flags = vtbackend::CellFlags { vtbackend::CellFlag::DottedUnderline };
        cell.attributes.lineFlags = vtbackend::LineFlags { vtbackend::LineFlag::DoubleWidth };
        std::ranges::copy(probe.dottedPhasesDrawnFor(cell, geometry.phaseCount), std::back_inserter(phases));
    }

    CHECK(phases == std::vector { 0, 1, 2, 3 });
}

TEST_CASE("DecorationRenderer.doubleUnderlineIsTwoSeparatedStrokes", "[decoration]")
{
    // Both strokes must sit below the underline position -- the upper one used to be placed at
    // `position + 2 * thickness`, i.e. inside the glyph body on a font like Nimbus Mono PS -- and
    // there must still be a visible gap between them, or it is not a double underline.
    auto const gridMetrics =
        gridMetricsFor(NimbusCellSize, NimbusBaseline, NimbusUnderlinePosition, /*thickness*/ 1);
    auto probe = DecorationProbe { gridMetrics };
    auto const rows = bitmapRows(probe.upload(Decorator::DoubleUnderline));
    for (auto const& row: rows)
        UNSCOPED_INFO(row);

    CHECK(topmostInkOffset(rows) <= gridMetrics.underline.position);

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
    auto const rows = bitmapRows(probe.upload(Decorator::DoubleUnderline));

    for (auto const& row: rows)
        UNSCOPED_INFO(row);

    CHECK(inkedRows(rows) == 4); // two strokes of ceil(2 * 2/3) == 2 rows each
}

TEST_CASE("DecorationRenderer.linkRoleUsesTheConfiguredDecorators", "[decoration][links]")
{
    using vtbackend::LinkDecorationRole;
    STATIC_REQUIRE(decoratorFor(LinkDecorationRole::None, Decorator::Framed, Decorator::Overline)
                   == std::nullopt);
    STATIC_REQUIRE(decoratorFor(LinkDecorationRole::Normal, Decorator::Framed, Decorator::Overline)
                   == Decorator::Framed);
    STATIC_REQUIRE(decoratorFor(LinkDecorationRole::Hover, Decorator::Framed, Decorator::Overline)
                   == Decorator::Overline);
}

TEST_CASE("DecorationRenderer.aLinkCellIsDrawnWithTheProfilesDecorator", "[decoration][links]")
{
    using vtbackend::LinkDecorationRole;

    // Decorators the cell's own flags never ask for, so what is drawn can only be the profile's choice.
    auto probe = DecorationProbe { gridMetricsFor(NimbusCellSize, NimbusBaseline, NimbusUnderlinePosition, 1),
                                   Decorator::Framed,
                                   Decorator::Overline };
    auto cell = vtbackend::RenderCell {};
    cell.width = 1;

    SECTION("a cell outside any link is drawn without")
    {
        cell.linkRole = LinkDecorationRole::None;
        CHECK(probe.decorationsDrawnFor(cell).empty());
    }

    SECTION("a link at rest takes the normal decorator")
    {
        cell.linkRole = LinkDecorationRole::Normal;
        CHECK(probe.decorationsDrawnFor(cell) == std::vector { Decorator::Framed });
    }

    SECTION("the hovered link takes the hover decorator")
    {
        cell.linkRole = LinkDecorationRole::Hover;
        CHECK(probe.decorationsDrawnFor(cell) == std::vector { Decorator::Overline });
    }

    SECTION("it adds to what the cell's own flags draw, and spans a wide cell")
    {
        cell.linkRole = LinkDecorationRole::Normal;
        cell.width = 2;
        cell.attributes.flags = vtbackend::CellFlags { vtbackend::CellFlag::CrossedOut };
        CHECK(probe.decorationsDrawnFor(cell)
              == std::vector {
                  Decorator::CrossedOut, Decorator::CrossedOut, Decorator::Framed, Decorator::Framed });
    }
}
