// SPDX-License-Identifier: Apache-2.0
#include <vtbackend/vt/GlyphGlossary.hpp>

#include <vtrasterizer/GridMetrics.hpp>
#include <vtrasterizer/RegisteredGlyphRenderer.hpp>
#include <vtrasterizer/RendererTestHelpers.hpp>
#include <vtrasterizer/TextureAtlas.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <memory>
#include <ranges>
#include <string>
#include <vector>

using namespace vtbackend;
using namespace vtrasterizer;

namespace
{

constexpr auto CellSize = ImageSize { Width(8), Height(16) };
constexpr auto Red = RGBColor { 0xFF, 0, 0 };
constexpr auto Where = CellLocation { .line = LineOffset(1), .column = ColumnOffset(2) };

/// The extent every test glyph declares: an em square on the baseline.
constexpr auto Em = GlyphLayout { .advanceWidth = 1000, .lineHeight = 1000, .extentBottom = 0 };

/// @return A glyph of one rectangle, from (@p left, @p bottom) to (@p right, @p top) in a 1000-unit em;
///         @p serial makes its id unique, as a content hash would.
[[nodiscard]] RegisteredGlyph rectangleGlyph(
    uint32_t serial, GlyphLayout layout, int left = 0, int bottom = 0, int right = 1000, int top = 1000)
{
    auto const on = [](int x, int y) {
        return GlyphOutlinePoint { .x = x, .y = y, .kind = GlyphPointKind::OnCurve };
    };
    return RegisteredGlyph {
        .id = crispy::StrongHash { 0x47, 0x4C, 0x59, serial },
        .layout = layout,
        .outline =
            GlyphOutline { .points = { on(left, bottom), on(left, top), on(right, top), on(right, bottom) },
                           .contourEnds = { 3 },
                           .bounds = { .xMin = left, .yMin = bottom, .xMax = right, .yMax = top } }
    };
}

/// @return A layout that stretches the em over the whole render span.
[[nodiscard]] constexpr GlyphLayout stretched(GlyphRenderSpan span = GlyphRenderSpan::OneCell) noexcept
{
    auto layout = Em;
    layout.size = GlyphSizeMode::Stretch;
    layout.span = span;
    return layout;
}

/// Renders @p pixels as '#'/'.' rows of @p width, top row first; '#' is full ink.
[[nodiscard]] std::vector<std::string> rowsOf(std::vector<uint8_t> const& pixels, size_t width)
{
    auto rows = std::vector<std::string> {};
    for (auto const y: std::views::iota(size_t { 0 }, pixels.size() / width))
        rows.emplace_back(std::ranges::to<std::string>(
            pixels | std::views::drop(y * width) | std::views::take(width)
            | std::views::transform([](uint8_t v) { return v == 0xFF ? '#' : '.'; })));
    return rows;
}

/// @return Whether every pixel of @p pixels is @p value.
[[nodiscard]] bool allOf(std::vector<uint8_t> const& pixels, uint8_t value)
{
    return std::ranges::all_of(pixels, [value](uint8_t pixel) { return pixel == value; });
}

/// Holds the renderer and everything it borrows, so a test gets its commands in one expression.
class GlyphProbe
{
  public:
    /// @param baseline The text baseline, up from the cell's bottom.
    explicit GlyphProbe(int baseline = 0):
        _gridMetrics { GridMetrics { .pageSize = PageSize { LineCount(24), ColumnCount(80) },
                                     .cellSize = CellSize,
                                     .baseline = baseline,
                                     .underline = { .position = 1, .thickness = 1 } } },
        _renderer { _gridMetrics }
    {
        _renderer.setRenderTarget(_renderTarget, _allocator);
        _atlas = std::make_unique<Renderable::TextureAtlas>(
            _renderTarget.textureScheduler(),
            atlas::AtlasProperties { .format = atlas::Format::Red,
                                     .tileSize = CellSize,
                                     .hashCount = crispy::StrongHashtableSize { 64 },
                                     .tileCount = crispy::LRUCapacity { 64 },
                                     .directMappingCount = _allocator.currentlyAllocatedCount });
        _renderer.setTextureAtlas(*_atlas);
    }

    /// Draws @p glyph at Where and expects it drawn.
    void draw(RegisteredGlyph const& glyph,
              GlyphRenderSpan span = GlyphRenderSpan::OneCell,
              LineFlags flags = {})
    {
        REQUIRE(_renderer.render(Where, glyph, span, Red, flags) == RegisteredGlyphDraw::Drawn);
    }

    [[nodiscard]] MockAtlasBackend& backend() { return _renderTarget.getMockBackend(); }

  private:
    GridMetrics _gridMetrics;
    RegisteredGlyphRenderer _renderer;
    MockRenderTarget _renderTarget;
    Renderable::DirectMappingAllocator _allocator {};
    std::unique_ptr<Renderable::TextureAtlas> _atlas;
};

} // namespace

TEST_CASE("RegisteredGlyphRenderer: one tile, drawn in the cell's foreground", "[RegisteredGlyphRenderer]")
{
    auto probe = GlyphProbe {};
    auto const glyph = rectangleGlyph(1, stretched());
    probe.draw(glyph);
    probe.draw(glyph);

    // Rasterized once, drawn twice.
    REQUIRE(probe.backend().uploadCommands.size() == 1);
    REQUIRE(probe.backend().renderCommands.size() == 2);
    CHECK(probe.backend().uploadCommands.front().bitmapSize == CellSize);
    CHECK(allOf(probe.backend().uploadCommands.front().bitmap, 0xFF));
    CHECK(probe.backend().renderCommands.front().color == atlas::normalize(Red));
}

TEST_CASE("RegisteredGlyphRenderer: a registration with other content has tiles of its own",
          "[RegisteredGlyphRenderer]")
{
    // What keeps a re-registered codepoint from showing its old outline.
    auto probe = GlyphProbe {};
    probe.draw(rectangleGlyph(1, stretched()));
    probe.draw(rectangleGlyph(2, stretched(), 0, 0, 500, 1000));
    REQUIRE(probe.backend().uploadCommands.size() == 2);
    CHECK(rowsOf(probe.backend().uploadCommands[1].bitmap, 8).front() == "####....");
}

TEST_CASE("RegisteredGlyphRenderer: a two-cell span is two tiles side by side", "[RegisteredGlyphRenderer]")
{
    auto probe = GlyphProbe {};
    // The left three quarters of the em, stretched over two cells: 12 of the 16 pixels.
    auto const glyph = rectangleGlyph(1, stretched(GlyphRenderSpan::TwoCells), 0, 0, 750, 1000);
    probe.draw(glyph, GlyphRenderSpan::TwoCells);

    REQUIRE(probe.backend().uploadCommands.size() == 2);
    REQUIRE(probe.backend().renderCommands.size() == 2);
    CHECK(probe.backend().renderCommands[1].x.value - probe.backend().renderCommands[0].x.value == 8);
    CHECK(probe.backend().renderCommands[1].y.value == probe.backend().renderCommands[0].y.value);
    CHECK(allOf(probe.backend().uploadCommands[0].bitmap, 0xFF));
    CHECK(rowsOf(probe.backend().uploadCommands[1].bitmap, 8).front() == "####....");

    // With no room to overflow it is drawn in one cell, from a tile that is not either of those.
    probe.draw(glyph, GlyphRenderSpan::OneCell);
    REQUIRE(probe.backend().uploadCommands.size() == 3);
    REQUIRE(probe.backend().renderCommands.size() == 3);
    CHECK(rowsOf(probe.backend().uploadCommands[2].bitmap, 8).front() == "######..");
}

TEST_CASE("RegisteredGlyphRenderer: a glyph is clipped to its cell", "[RegisteredGlyphRenderer]")
{
    // Sized by height, the em is 16 pixels square in a cell 8 wide: only the cell's share is drawn, and
    // only one tile is made for it.
    auto probe = GlyphProbe {};
    auto layout = Em;
    layout.size = GlyphSizeMode::Height;
    probe.draw(rectangleGlyph(1, layout));
    REQUIRE(probe.backend().uploadCommands.size() == 1);
    REQUIRE(probe.backend().renderCommands.size() == 1);
    CHECK(allOf(probe.backend().uploadCommands.front().bitmap, 0xFF));
}

TEST_CASE("RegisteredGlyphRenderer: baseline alignment sits on the grid's baseline",
          "[RegisteredGlyphRenderer]")
{
    // The baseline is 4 pixels up from the cell's bottom, so 12 down from its top. Sized by its advance the
    // em is 8 pixels square; the glyph is its lower half, which stands on the baseline: rows 8 to 11.
    auto probe = GlyphProbe { 4 };
    auto layout = Em;
    layout.size = GlyphSizeMode::Advance;
    layout.vertical = GlyphVerticalAlignment::Baseline;
    probe.draw(rectangleGlyph(1, layout, 0, 0, 1000, 500));

    auto const rows = rowsOf(probe.backend().uploadCommands.front().bitmap, 8);
    for (auto const row: std::views::iota(size_t { 0 }, rows.size()))
    {
        INFO("row " << row);
        CHECK(rows[row] == (row >= 8 && row < 12 ? "########" : "........"));
    }
}

TEST_CASE("RegisteredGlyphRenderer: a double-width row stretches the glyph across two tiles",
          "[RegisteredGlyphRenderer]")
{
    // Contained, the em is 8 pixels square, centered in the cell: rows 4 to 11. On a double-width row it is
    // drawn twice as wide and no taller, as the row's text is.
    auto probe = GlyphProbe {};
    probe.draw(rectangleGlyph(1, Em), GlyphRenderSpan::OneCell, LineFlags { LineFlag::DoubleWidth });

    REQUIRE(probe.backend().uploadCommands.size() == 2);
    REQUIRE(probe.backend().renderCommands.size() == 2);
    CHECK(probe.backend().renderCommands[1].x.value - probe.backend().renderCommands[0].x.value == 8);
    for (auto const& upload: probe.backend().uploadCommands)
    {
        auto const rows = rowsOf(upload.bitmap, 8);
        for (auto const row: std::views::iota(size_t { 0 }, rows.size()))
        {
            INFO("row " << row);
            CHECK(rows[row] == (row >= 4 && row < 12 ? "########" : "........"));
        }
    }
}

TEST_CASE("RegisteredGlyphRenderer: each half of a double-height row draws its half of the glyph",
          "[RegisteredGlyphRenderer]")
{
    // The upper half of the em, stretched over the cell: on a double-height row that is the whole of the
    // top half's tiles and none of the bottom half's.
    auto probe = GlyphProbe {};
    auto const glyph = rectangleGlyph(1, stretched(), 0, 500, 1000, 1000);
    probe.draw(
        glyph, GlyphRenderSpan::OneCell, LineFlags { LineFlag::DoubleWidth, LineFlag::DoubleHeightTop });
    probe.draw(
        glyph, GlyphRenderSpan::OneCell, LineFlags { LineFlag::DoubleWidth, LineFlag::DoubleHeightBottom });

    REQUIRE(probe.backend().uploadCommands.size() == 4);
    CHECK(allOf(probe.backend().uploadCommands[0].bitmap, 0xFF));
    CHECK(allOf(probe.backend().uploadCommands[1].bitmap, 0xFF));
    CHECK(allOf(probe.backend().uploadCommands[2].bitmap, 0));
    CHECK(allOf(probe.backend().uploadCommands[3].bitmap, 0));
}

TEST_CASE("RegisteredGlyphRenderer: an outline with nothing to draw is a clear tile",
          "[RegisteredGlyphRenderer]")
{
    auto probe = GlyphProbe {};
    // No declared extent and no points: there is no extent to place.
    auto const blank = RegisteredGlyph { .id = crispy::StrongHash { 0x47, 0x4C, 0x59, 9 } };
    probe.draw(blank);
    probe.draw(blank);
    REQUIRE(probe.backend().uploadCommands.size() == 1); // not placed again every frame
    CHECK(allOf(probe.backend().uploadCommands.front().bitmap, 0));
}
