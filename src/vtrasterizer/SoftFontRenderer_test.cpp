// SPDX-License-Identifier: Apache-2.0
#include <vtbackend/vt/SoftFont.hpp>

#include <vtrasterizer/GridMetrics.hpp>
#include <vtrasterizer/RendererTestHelpers.hpp>
#include <vtrasterizer/SoftFontRenderer.hpp>
#include <vtrasterizer/TextureAtlas.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <memory>
#include <ranges>
#include <string>
#include <vector>

using namespace vtbackend;
using namespace vtrasterizer;

namespace
{

/// A glyph of @p width x @p height from @p rows, '#' being ink; @p serial makes its id unique.
[[nodiscard]] SoftGlyph makeGlyph(uint32_t serial, std::vector<std::string> const& rows)
{
    auto glyph = SoftGlyph { .id = crispy::StrongHash { 0x47, 0x4C, 0x59, serial },
                             .width = static_cast<uint8_t>(rows.front().size()),
                             .height = static_cast<uint8_t>(rows.size()),
                             .coverage = {} };
    for (auto const& row: rows)
        for (auto const c: row)
            glyph.coverage.push_back(c == '#' ? uint8_t { 1 } : uint8_t { 0 });
    return glyph;
}

/// Renders @p pixels as '#'/'.' rows of @p width, top row first.
[[nodiscard]] std::vector<std::string> rowsOf(std::vector<uint8_t> const& pixels, size_t width)
{
    auto rows = std::vector<std::string> {};
    for (auto const y: std::views::iota(size_t { 0 }, pixels.size() / width))
        rows.emplace_back(
            std::ranges::to<std::string>(pixels | std::views::drop(y * width) | std::views::take(width)
                                         | std::views::transform([](uint8_t v) { return v ? '#' : '.'; })));
    return rows;
}

[[nodiscard]] GridMetrics gridMetricsFor(ImageSize cellSize)
{
    return GridMetrics { .pageSize = PageSize { LineCount(24), ColumnCount(80) },
                         .cellSize = cellSize,
                         .baseline = 0,
                         .underline = { .position = 1, .thickness = 1 } };
}

/// Holds the renderer and everything it borrows, so a test gets its commands in one expression.
class SoftFontProbe
{
  public:
    explicit SoftFontProbe(ImageSize cellSize, uint32_t tileCount = 64):
        _gridMetrics { gridMetricsFor(cellSize) }, _renderer { _gridMetrics }, _tileCount { tileCount }
    {
        _renderer.setRenderTarget(_renderTarget, _allocator);
        configureAtlas();
    }

    /// Reconfigures the atlas for @p cellSize, as a font or DPI change does: its tiles are all gone.
    void resizeCells(ImageSize cellSize)
    {
        _gridMetrics.cellSize = cellSize;
        configureAtlas();
    }

    [[nodiscard]] SoftFontRenderer& renderer() noexcept { return _renderer; }
    [[nodiscard]] MockAtlasBackend& backend() { return _renderTarget.getMockBackend(); }

  private:
    void configureAtlas()
    {
        _atlas = std::make_unique<Renderable::TextureAtlas>(
            _renderTarget.textureScheduler(),
            atlas::AtlasProperties { .format = atlas::Format::Red,
                                     .tileSize = _gridMetrics.cellSize,
                                     .hashCount = crispy::StrongHashtableSize { 64 },
                                     .tileCount = crispy::LRUCapacity { _tileCount },
                                     .directMappingCount = _allocator.currentlyAllocatedCount });
        _renderer.setTextureAtlas(*_atlas);
    }

    GridMetrics _gridMetrics;
    SoftFontRenderer _renderer;
    uint32_t _tileCount;
    MockRenderTarget _renderTarget;
    Renderable::DirectMappingAllocator _allocator {};
    std::unique_ptr<Renderable::TextureAtlas> _atlas;
};

constexpr auto CellSize = ImageSize { Width(9), Height(18) };
constexpr auto Black = RGBColor { 0, 0, 0 };

} // namespace

TEST_CASE("scaleSoftGlyph is nearest-neighbour and fills the target", "[SoftFontRenderer]")
{
    auto const glyph = makeGlyph(1, { "#.", ".#" });
    auto const pixels = scaleSoftGlyph(glyph, ImageSize { Width(4), Height(6) });

    REQUIRE(pixels.size() == 24);
    CHECK(rowsOf(pixels, 4) == std::vector<std::string> { "##..", "##..", "##..", "..##", "..##", "..##" });
    CHECK(std::ranges::all_of(pixels, [](uint8_t v) { return v == 0 || v == 0xFF; }));
}

TEST_CASE("a 15x12 glyph fills a 9x18 cell exactly", "[SoftFontRenderer]")
{
    // Ink in the source's first and last rows and columns only.
    auto rows = std::vector<std::string>(12, std::string(15, '.'));
    for (auto& row: rows)
    {
        row.front() = '#';
        row.back() = '#';
    }
    rows.front() = std::string(15, '#');
    rows.back() = std::string(15, '#');
    auto const glyph = makeGlyph(2, rows);

    auto const pixels = scaleSoftGlyph(glyph, CellSize);
    REQUIRE(pixels.size() == 9uz * 18);
    auto const scaled = rowsOf(pixels, 9);

    CHECK(scaled.front() == std::string(9, '#')); // source row 0
    CHECK(scaled.back() == std::string(9, '#'));  // y = 17 samples source row 11, the last
    CHECK(scaled[3].front() == '#');              // source column 0
    CHECK(scaled[3][1] == '.');
    // x = 8 samples source column 8 * 15 / 9 = 13, which is clear; the formula floors, it never rounds.
    CHECK(scaled[3].back() == '.');

    // Every pixel obeys dst(x, y) = src(x * w / W, y * h / H).
    for (auto const y: std::views::iota(0uz, 18uz))
        for (auto const x: std::views::iota(0uz, 9uz))
        {
            auto const expected =
                glyph.coverage[((y * 12 / 18) * 15) + (x * 15 / 9)] ? uint8_t { 0xFF } : uint8_t { 0 };
            CHECK(pixels[(y * 9) + x] == expected);
        }
}

TEST_CASE("one upload for many cells", "[SoftFontRenderer]")
{
    auto probe = SoftFontProbe { CellSize };
    auto const glyph = makeGlyph(3, { "#.", ".#" });
    auto const colors = std::array { RGBColor { 255, 0, 0 }, RGBColor { 0, 255, 0 }, RGBColor { 0, 0, 255 } };

    for (auto const i: std::views::iota(0, 100))
        CHECK(probe.renderer().render(
                  CellLocation { .line = LineOffset(i / 80), .column = ColumnOffset(i % 80) },
                  glyph,
                  colors[static_cast<size_t>(i) % colors.size()],
                  LineFlags {})
              == SoftGlyphDraw::Drawn);

    REQUIRE(probe.backend().uploadCommands.size() == 1);
    REQUIRE(probe.backend().renderCommands.size() == 100);
    for (auto const i: std::views::iota(0uz, 100uz))
        CHECK(probe.backend().renderCommands[i].color == atlas::normalize(colors[i % colors.size()]));

    auto const& upload = probe.backend().uploadCommands.front();
    CHECK(upload.bitmapFormat == atlas::Format::Red);
    CHECK(upload.bitmapSize == CellSize);
}

TEST_CASE("double-width emits two tiles from a doubled glyph", "[SoftFontRenderer]")
{
    auto probe = SoftFontProbe { ImageSize { Width(4), Height(2) } };
    auto const glyph = makeGlyph(4, { "#...", "...#" });

    REQUIRE(probe.renderer().render(CellLocation { .line = LineOffset(1), .column = ColumnOffset(2) },
                                    glyph,
                                    Black,
                                    LineFlags { LineFlag::DoubleWidth })
            == SoftGlyphDraw::Drawn);

    REQUIRE(probe.backend().renderCommands.size() == 2);
    REQUIRE(probe.backend().uploadCommands.size() == 2);
    CHECK(probe.backend().renderCommands[1].x.value - probe.backend().renderCommands[0].x.value == 4);
    CHECK(probe.backend().renderCommands[1].y.value == probe.backend().renderCommands[0].y.value);

    // The glyph scaled to 8x2 is "##......" / "......##"; each cell takes its half.
    CHECK(rowsOf(probe.backend().uploadCommands[0].bitmap, 4) == std::vector<std::string> { "##..", "...." });
    CHECK(rowsOf(probe.backend().uploadCommands[1].bitmap, 4) == std::vector<std::string> { "....", "..##" });
}

TEST_CASE("double-height top and bottom take the right halves", "[SoftFontRenderer]")
{
    auto probe = SoftFontProbe { ImageSize { Width(2), Height(2) } };
    auto const glyph = makeGlyph(5, { "#.", "..", "..", ".#" });
    auto const where = CellLocation { .line = LineOffset(0), .column = ColumnOffset(0) };

    REQUIRE(probe.renderer().render(where, glyph, Black, LineFlags { LineFlag::DoubleHeightTop })
            == SoftGlyphDraw::Drawn);
    REQUIRE(probe.renderer().render(where, glyph, Black, LineFlags { LineFlag::DoubleHeightBottom })
            == SoftGlyphDraw::Drawn);

    REQUIRE(probe.backend().uploadCommands.size() == 2);
    // Scaled to 2x4 the glyph is the four rows of its source; top takes rows 0-1, bottom rows 2-3.
    CHECK(rowsOf(probe.backend().uploadCommands[0].bitmap, 2) == std::vector<std::string> { "#.", ".." });
    CHECK(rowsOf(probe.backend().uploadCommands[1].bitmap, 2) == std::vector<std::string> { "..", ".#" });
}

TEST_CASE("cell size change regenerates the tile", "[SoftFontRenderer]")
{
    auto probe = SoftFontProbe { CellSize };
    auto const glyph = makeGlyph(6, { "#.", ".#" });
    auto const where = CellLocation { .line = LineOffset(0), .column = ColumnOffset(0) };

    REQUIRE(probe.renderer().render(where, glyph, Black, LineFlags {}) == SoftGlyphDraw::Drawn);
    REQUIRE(probe.backend().uploadCommands.size() == 1);
    CHECK(probe.backend().uploadCommands.back().bitmapSize == CellSize);

    auto const bigger = ImageSize { Width(12), Height(24) };
    probe.resizeCells(bigger);
    REQUIRE(probe.renderer().render(where, glyph, Black, LineFlags {}) == SoftGlyphDraw::Drawn);
    REQUIRE(probe.backend().uploadCommands.size() == 2);
    CHECK(probe.backend().uploadCommands.back().bitmapSize == bigger);
    CHECK(probe.backend().uploadCommands.back().bitmap.size() == bigger.area());
}

TEST_CASE("SoftGlyphCursor finds each cell's glyph in one forward pass", "[SoftFontRenderer]")
{
    auto const glyph = std::make_shared<SoftGlyph const>(makeGlyph(8, { "#" }));
    auto const at = [](int line, int column) {
        return CellLocation { .line = LineOffset(line), .column = ColumnOffset(column) };
    };
    auto const glyphs = std::vector<RenderSoftGlyph> { { .position = at(0, 3), .glyph = glyph },
                                                       { .position = at(1, 1), .glyph = glyph },
                                                       { .position = at(1, 5), .glyph = glyph } };

    SECTION("walking the cells in order")
    {
        auto cursor = SoftGlyphCursor { glyphs };
        auto found = std::vector<CellLocation> {};
        for (auto const line: std::views::iota(0, 3))
            for (auto const column: std::views::iota(0, 8))
                if (cursor.find(at(line, column)) != nullptr)
                    found.push_back(at(line, column));
        CHECK(found == std::vector { at(0, 3), at(1, 1), at(1, 5) });
    }

    SECTION("a cell before the previous one starts the walk over")
    {
        auto cursor = SoftGlyphCursor { glyphs };
        CHECK(cursor.find(at(1, 5)) == glyph.get());
        CHECK(cursor.find(at(0, 3)) == glyph.get());
        CHECK(cursor.find(at(0, 4)) == nullptr);
    }

    SECTION("no glyphs, no match")
    {
        auto cursor = SoftGlyphCursor { std::span<RenderSoftGlyph const> {} };
        CHECK(cursor.find(at(0, 0)) == nullptr);
    }
}
