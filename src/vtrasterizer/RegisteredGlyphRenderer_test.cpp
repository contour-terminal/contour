// SPDX-License-Identifier: Apache-2.0
#include <vtbackend/vt/GlyphGlossary.hpp>

#include <vtrasterizer/GridMetrics.hpp>
#include <vtrasterizer/RegisteredGlyphRenderer.hpp>
#include <vtrasterizer/RendererTestHelpers.hpp>
#include <vtrasterizer/TextureAtlas.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <format>
#include <memory>
#include <ranges>
#include <string>
#include <string_view>
#include <vector>

using namespace vtbackend;
using namespace vtrasterizer;

namespace
{

constexpr auto CellSize = ImageSize { Width(8), Height(16) };
constexpr auto Red = RGBColor { 0xFF, 0, 0 };
constexpr auto Green = RGBColor { 0, 0xFF, 0 };
constexpr auto Blue = RGBColor { 0, 0, 0xFF };
constexpr auto Where = CellLocation { .line = LineOffset(1), .column = ColumnOffset(2) };

/// Where a test glyph goes in the glossary that makes it; any slot does.
constexpr auto Slot = char32_t { 0x100000 };

/// @return The glyph an application registers with @p payload in a viewBox of the cell's own size (8 x 16
///         units, so one unit is one pixel when it is stretched over a cell) and the keys given. Made by a
///         glossary, so its id is the hash a real one has.
[[nodiscard]] std::shared_ptr<RegisteredGlyph const> glyphOf(std::string_view payload,
                                                             std::string_view fit = "stretch",
                                                             std::string_view width = "1",
                                                             std::string_view viewBox = "0,0,8,16")
{
    auto glossary = GlyphGlossary {};
    glossary.registerGlyph(
        Slot,
        parseGlyphDefinition(
            GlyphStatement { .viewBox = viewBox, .fit = fit, .width = width, .payload = payload })
            .value());
    return glossary.find(Slot);
}

/// @return A glyph filling the rectangle from (@p left, @p top) to (@p right, @p bottom) with the
///         foreground.
[[nodiscard]] std::shared_ptr<RegisteredGlyph const> rectangleGlyph(int left, int top, int right, int bottom)
{
    return glyphOf(std::format("fill=nonzero,fg:M{} {}H{}V{}H{}Z", left, top, right, bottom, left));
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
    GlyphProbe():
        _gridMetrics { GridMetrics { .pageSize = PageSize { LineCount(24), ColumnCount(80) },
                                     .cellSize = CellSize,
                                     .baseline = 0,
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

    /// Draws @p glyph at Where, in @p color, and expects it drawn.
    void draw(RegisteredGlyph const& glyph,
              GlyphRenderSpan span = GlyphRenderSpan::OneCell,
              LineFlags flags = {},
              RGBColor color = Red)
    {
        REQUIRE(_renderer.render(Where, glyph, span, color, flags) == RegisteredGlyphDraw::Drawn);
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
    auto const glyph = rectangleGlyph(0, 0, 8, 16);
    probe.draw(*glyph);
    probe.draw(*glyph);

    // Rasterized once, drawn twice.
    REQUIRE(probe.backend().uploadCommands.size() == 1);
    REQUIRE(probe.backend().renderCommands.size() == 2);
    auto const& upload = probe.backend().uploadCommands.front();
    CHECK(upload.bitmapSize == CellSize);
    // Coverage, which the text shader tints.
    CHECK(upload.bitmapFormat == atlas::Format::Red);
    CHECK(allOf(upload.bitmap, 0xFF));
    CHECK(probe.backend().renderCommands.front().color == atlas::normalize(Red));
}

TEST_CASE("RegisteredGlyphRenderer: a foreground-only glyph has one tile for every foreground",
          "[RegisteredGlyphRenderer]")
{
    auto probe = GlyphProbe {};
    auto const glyph = rectangleGlyph(0, 0, 8, 16);
    probe.draw(*glyph, GlyphRenderSpan::OneCell, {}, Red);
    probe.draw(*glyph, GlyphRenderSpan::OneCell, {}, Green);
    CHECK(probe.backend().uploadCommands.size() == 1);
    REQUIRE(probe.backend().renderCommands.size() == 2);
    CHECK(probe.backend().renderCommands[1].color == atlas::normalize(Green));
}

TEST_CASE("RegisteredGlyphRenderer: a glyph with colours of its own is a colour tile",
          "[RegisteredGlyphRenderer]")
{
    auto probe = GlyphProbe {};

    SECTION("fixed colours only: one tile, whatever the foreground")
    {
        auto const glyph = glyphOf("fill=nonzero,#00ff00:M0 0H8V16H0Z");
        probe.draw(*glyph, GlyphRenderSpan::OneCell, {}, Red);
        probe.draw(*glyph, GlyphRenderSpan::OneCell, {}, Green);
        REQUIRE(probe.backend().uploadCommands.size() == 1);
        auto const& upload = probe.backend().uploadCommands.front();
        CHECK(upload.bitmapFormat == atlas::Format::RGBA);
        REQUIRE(upload.bitmap.size() == CellSize.area() * 4);
        CHECK(std::vector(upload.bitmap.begin(), upload.bitmap.begin() + 4)
              == std::vector<uint8_t> { 0, 0xFF, 0, 0xFF });
    }

    SECTION("the foreground among them: a tile for each foreground it is shown in")
    {
        auto const glyph = glyphOf("fill=nonzero,fg:M0 0H8V8H0Z|fill=nonzero,#00ff00:M0 8H8V16H0Z");
        probe.draw(*glyph, GlyphRenderSpan::OneCell, {}, Red);
        probe.draw(*glyph, GlyphRenderSpan::OneCell, {}, Blue);
        probe.draw(*glyph, GlyphRenderSpan::OneCell, {}, Red); // and that one again: no third tile
        REQUIRE(probe.backend().uploadCommands.size() == 2);
        // The foreground is in the pixels: the top-left one is the foreground's.
        auto const firstPixel = [&](size_t upload) {
            auto const& bitmap = probe.backend().uploadCommands[upload].bitmap;
            return std::vector(bitmap.begin(), bitmap.begin() + 4);
        };
        CHECK(firstPixel(0) == std::vector<uint8_t> { 0xFF, 0, 0, 0xFF });
        CHECK(firstPixel(1) == std::vector<uint8_t> { 0, 0, 0xFF, 0xFF });
    }
}

TEST_CASE("RegisteredGlyphRenderer: a registration with other content has tiles of its own",
          "[RegisteredGlyphRenderer]")
{
    // What keeps a re-registered codepoint from showing its old glyph.
    auto probe = GlyphProbe {};
    probe.draw(*rectangleGlyph(0, 0, 8, 16));
    probe.draw(*rectangleGlyph(0, 0, 4, 16));
    REQUIRE(probe.backend().uploadCommands.size() == 2);
    CHECK(rowsOf(probe.backend().uploadCommands[1].bitmap, 8).front() == "####....");
}

TEST_CASE("RegisteredGlyphRenderer: a two-cell span is two tiles side by side", "[RegisteredGlyphRenderer]")
{
    auto probe = GlyphProbe {};
    // The left three quarters of the viewBox, stretched over two cells: 12 of the 16 pixels.
    auto const glyph = glyphOf("fill=nonzero,fg:M0 0H6V16H0Z", "stretch", "2");
    probe.draw(*glyph, GlyphRenderSpan::TwoCells);

    REQUIRE(probe.backend().uploadCommands.size() == 2);
    REQUIRE(probe.backend().renderCommands.size() == 2);
    CHECK(probe.backend().renderCommands[1].x.value - probe.backend().renderCommands[0].x.value == 8);
    CHECK(probe.backend().renderCommands[1].y.value == probe.backend().renderCommands[0].y.value);
    CHECK(allOf(probe.backend().uploadCommands[0].bitmap, 0xFF));
    CHECK(rowsOf(probe.backend().uploadCommands[1].bitmap, 8).front() == "####....");

    // With no room to overflow it is drawn in one cell, from a tile that is not either of those.
    probe.draw(*glyph, GlyphRenderSpan::OneCell);
    REQUIRE(probe.backend().uploadCommands.size() == 3);
    REQUIRE(probe.backend().renderCommands.size() == 3);
    CHECK(rowsOf(probe.backend().uploadCommands[2].bitmap, 8).front() == "######..");
}

TEST_CASE("RegisteredGlyphRenderer: a glyph is clipped to its cell", "[RegisteredGlyphRenderer]")
{
    // Covering, a square viewBox is 16 pixels square in a cell 8 wide: only the cell's share is drawn, and
    // only one tile is made for it.
    auto probe = GlyphProbe {};
    probe.draw(*glyphOf("fill=nonzero,fg:M0 0H16V16H0Z", "cover", "1", "0,0,16,16"));
    REQUIRE(probe.backend().uploadCommands.size() == 1);
    REQUIRE(probe.backend().renderCommands.size() == 1);
    CHECK(allOf(probe.backend().uploadCommands.front().bitmap, 0xFF));
}

TEST_CASE("RegisteredGlyphRenderer: contained, a glyph is centered in its cell", "[RegisteredGlyphRenderer]")
{
    // A square viewBox in a cell 8 x 16 is 8 pixels square, in rows 4 to 11.
    auto probe = GlyphProbe {};
    probe.draw(*glyphOf("fill=nonzero,fg:M0 0H16V16H0Z", "contain", "1", "0,0,16,16"));
    auto const rows = rowsOf(probe.backend().uploadCommands.front().bitmap, 8);
    for (auto const row: std::views::iota(size_t { 0 }, rows.size()))
    {
        INFO("row " << row);
        CHECK(rows[row] == (row >= 4 && row < 12 ? "########" : "........"));
    }
}

TEST_CASE("RegisteredGlyphRenderer: a double-width row stretches the glyph across two tiles",
          "[RegisteredGlyphRenderer]")
{
    // Contained, the square is rows 4 to 11 of the cell. On a double-width row it is drawn twice as wide
    // and no taller, as the row's text is.
    auto probe = GlyphProbe {};
    probe.draw(*glyphOf("fill=nonzero,fg:M0 0H16V16H0Z", "contain", "1", "0,0,16,16"),
               GlyphRenderSpan::OneCell,
               LineFlags { LineFlag::DoubleWidth });

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
    // The upper half of the viewBox, stretched over the cell: on a double-height row that is the whole of
    // the top half's tiles and none of the bottom half's.
    auto probe = GlyphProbe {};
    auto const glyph = rectangleGlyph(0, 0, 8, 8);
    probe.draw(
        *glyph, GlyphRenderSpan::OneCell, LineFlags { LineFlag::DoubleWidth, LineFlag::DoubleHeightTop });
    probe.draw(
        *glyph, GlyphRenderSpan::OneCell, LineFlags { LineFlag::DoubleWidth, LineFlag::DoubleHeightBottom });

    REQUIRE(probe.backend().uploadCommands.size() == 4);
    CHECK(allOf(probe.backend().uploadCommands[0].bitmap, 0xFF));
    CHECK(allOf(probe.backend().uploadCommands[1].bitmap, 0xFF));
    CHECK(allOf(probe.backend().uploadCommands[2].bitmap, 0));
    CHECK(allOf(probe.backend().uploadCommands[3].bitmap, 0));
}

TEST_CASE("RegisteredGlyphRenderer: a glyph that draws nothing is a clear tile", "[RegisteredGlyphRenderer]")
{
    auto probe = GlyphProbe {};
    auto const blank = glyphOf("fill=nonzero,fg:M4 4");
    probe.draw(*blank);
    probe.draw(*blank);
    REQUIRE(probe.backend().uploadCommands.size() == 1); // not drawn again every frame
    CHECK(allOf(probe.backend().uploadCommands.front().bitmap, 0));
}
