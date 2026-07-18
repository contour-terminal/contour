// SPDX-License-Identifier: Apache-2.0
//
// Unit tests for the pure raster primitives a text-sizing block is built from: compositing a glyph
// into its block canvas, and cutting that canvas into the cell-sized tiles the atlas can store.

#include <vtrasterizer/GlyphSlicing.h>

#include <catch2/catch_test_macros.hpp>

#include <ranges>
#include <string>

using namespace vtrasterizer;
using vtbackend::Height;
using vtbackend::ImageSize;
using vtbackend::Width;

namespace
{

constexpr auto One = size_t { 1 }; // one byte per pixel: an alpha mask, as text glyphs are

[[nodiscard]] ImageSize sized(int width, int height) noexcept
{
    return ImageSize { Width(width), Height(height) };
}

/// Renders a single-component bitmap as rows of '#' and '.', so a failure shows the picture.
[[nodiscard]] std::vector<std::string> render(std::span<uint8_t const> bitmap, ImageSize size)
{
    auto rows = std::vector<std::string> {};
    for (auto const y: std::views::iota(0, unbox<int>(size.height)))
    {
        auto row = std::string {};
        for (auto const x: std::views::iota(0, unbox<int>(size.width)))
            row += bitmap[(static_cast<size_t>(y) * unbox<size_t>(size.width)) + static_cast<size_t>(x)]
                       ? '#'
                       : '.';
        rows.emplace_back(std::move(row));
    }
    return rows;
}

/// A bitmap whose every pixel is set.
[[nodiscard]] std::vector<uint8_t> filled(ImageSize size)
{
    return std::vector<uint8_t>(size.area(), uint8_t { 0xFF });
}

} // namespace

TEST_CASE("GlyphSlicing.blit.lands_where_it_was_told_to", "[glyphslicing]")
{
    auto target = std::vector<uint8_t>(sized(4, 4).area(), uint8_t { 0 });
    auto const source = filled(sized(2, 2));

    blitClipped(target, sized(4, 4), source, sized(2, 2), 1, 1, One);

    CHECK(render(target, sized(4, 4)) == std::vector<std::string> { "....", ".##.", ".##.", "...." });
}

TEST_CASE("GlyphSlicing.blit.clips_at_every_edge", "[glyphslicing]")
{
    // A glyph sitting on its block's baseline routinely overhangs the block -- a descender below,
    // an accent above. Overhang must be dropped, not wrapped around into the opposite edge and not
    // treated as an error.
    auto const source = filled(sized(2, 2));

    SECTION("off the top-left")
    {
        auto target = std::vector<uint8_t>(sized(3, 3).area(), uint8_t { 0 });
        blitClipped(target, sized(3, 3), source, sized(2, 2), -1, -1, One);
        CHECK(render(target, sized(3, 3)) == std::vector<std::string> { "#..", "...", "..." });
    }

    SECTION("off the bottom-right")
    {
        auto target = std::vector<uint8_t>(sized(3, 3).area(), uint8_t { 0 });
        blitClipped(target, sized(3, 3), source, sized(2, 2), 2, 2, One);
        CHECK(render(target, sized(3, 3)) == std::vector<std::string> { "...", "...", "..#" });
    }

    SECTION("entirely outside leaves the target untouched")
    {
        auto target = std::vector<uint8_t>(sized(3, 3).area(), uint8_t { 0 });
        blitClipped(target, sized(3, 3), source, sized(2, 2), 5, 0, One);
        blitClipped(target, sized(3, 3), source, sized(2, 2), 0, -9, One);
        CHECK(render(target, sized(3, 3)) == std::vector<std::string> { "...", "...", "..." });
    }
}

TEST_CASE("GlyphSlicing.slice.cuts_a_block_into_one_tile_per_cell", "[glyphslicing]")
{
    // The property the atlas depends on: every tile fits one cell. A 2x2-cell block yields four.
    constexpr auto Cell = 3;
    auto const canvasSize = sized(2 * Cell, 2 * Cell);
    auto canvas = std::vector<uint8_t>(canvasSize.area(), uint8_t { 0 });

    // Mark each quadrant's top-left pixel so the tiles can be told apart.
    for (auto const band: std::views::iota(0, 2))
        for (auto const column: std::views::iota(0, 2))
            canvas[(static_cast<size_t>(band * Cell) * unbox<size_t>(canvasSize.width))
                   + static_cast<size_t>(column * Cell)] = 0xFF;

    auto const tiles = sliceIntoTiles(canvas, canvasSize, sized(Cell, Cell), One);

    REQUIRE(tiles.size() == 4);
    for (auto const& tile: tiles)
    {
        INFO("tile (" << tile.column << ", " << tile.band << ")");
        CHECK(tile.size == sized(Cell, Cell));
        // Each quadrant carries exactly its own marker, at its own top-left.
        CHECK(render(tile.bitmap, tile.size) == std::vector<std::string> { "#..", "...", "..." });
    }

    CHECK(tiles[0].column == 0);
    CHECK(tiles[0].band == 0);
    CHECK(tiles[1].column == 1);
    CHECK(tiles[1].band == 0);
    CHECK(tiles[2].column == 0);
    CHECK(tiles[2].band == 1);
    CHECK(tiles[3].column == 1);
    CHECK(tiles[3].band == 1);
}

TEST_CASE("GlyphSlicing.slice.never_exceeds_the_tile_it_was_given", "[glyphslicing]")
{
    // The invariant the whole exercise exists for, over sizes that do NOT divide evenly: a tile
    // larger than the atlas' tile is a write into the neighbouring tile.
    constexpr auto Cell = 4;
    for (auto const width: std::views::iota(1, 14))
        for (auto const height: std::views::iota(1, 14))
        {
            INFO("canvas " << width << "x" << height);
            auto const canvasSize = sized(width, height);
            auto const canvas = filled(canvasSize);
            auto const tiles = sliceIntoTiles(canvas, canvasSize, sized(Cell, Cell), One);

            REQUIRE_FALSE(tiles.empty());

            auto covered = size_t { 0 };
            for (auto const& tile: tiles)
            {
                CHECK(unbox(tile.size.width) <= Cell);
                CHECK(unbox(tile.size.height) <= Cell);
                CHECK(tile.bitmap.size() == tile.size.area());
                covered += tile.size.area();
            }

            // Nothing is dropped and nothing is counted twice.
            CHECK(covered == canvasSize.area());
        }
}

TEST_CASE("GlyphSlicing.slice.an_ordinary_glyph_is_a_single_tile", "[glyphslicing]")
{
    // Unscaled text is the overwhelming majority of what a terminal draws; it must not acquire a
    // grid of one.
    auto const canvasSize = sized(6, 9);
    auto const canvas = filled(canvasSize);
    auto const tiles = sliceIntoTiles(canvas, canvasSize, canvasSize, One);

    REQUIRE(tiles.size() == 1);
    CHECK(tiles[0].column == 0);
    CHECK(tiles[0].band == 0);
    CHECK(tiles[0].size == canvasSize);
}

TEST_CASE("GlyphSlicing.slice.carries_multi_byte_pixels_intact", "[glyphslicing]")
{
    // Emoji arrive as RGBA. Slicing must move whole pixels, not stride over bytes.
    constexpr auto Rgba = size_t { 4 };
    auto const canvasSize = sized(2, 1);
    auto canvas = std::vector<uint8_t> { 1, 2, 3, 4, 5, 6, 7, 8 };

    auto const tiles = sliceIntoTiles(canvas, canvasSize, sized(1, 1), Rgba);

    REQUIRE(tiles.size() == 2);
    CHECK(tiles[0].bitmap == std::vector<uint8_t> { 1, 2, 3, 4 });
    CHECK(tiles[1].bitmap == std::vector<uint8_t> { 5, 6, 7, 8 });
}

TEST_CASE("GlyphSlicing.magnify.reaches_exactly_the_size_asked_for", "[glyphslicing]")
{
    // The block canvas composites pixels, so the raster must already BE the drawn size; a size that
    // came back short or long would misplace every glyph in the block.
    auto const source = filled(sized(3, 5));
    for (auto const factor: { 2, 3, 4 })
    {
        INFO("factor " << factor);
        auto const target = sized(3 * factor, 5 * factor);
        auto const magnified = magnify(source, sized(3, 5), target, One);
        CHECK(magnified.size() == target.area());
    }
}

TEST_CASE("GlyphSlicing.magnify.a_solid_block_stays_solid", "[glyphslicing]")
{
    // Interpolation must not eat the edges: a filled source magnifies to a filled target. Sampling
    // at pixel CENTRES rather than corners is what makes this hold.
    auto const source = filled(sized(2, 2));
    auto const magnified = magnify(source, sized(2, 2), sized(6, 6), One);

    REQUIRE(magnified.size() == sized(6, 6).area());
    for (auto const value: magnified)
        CHECK(value == 0xFF);
}

TEST_CASE("GlyphSlicing.magnify.identity_is_a_faithful_copy", "[glyphslicing]")
{
    // Ordinary text never reaches this, but a fraction that lands back on 1.0 does; it must not
    // resample the glyph into a blur.
    auto const source = std::vector<uint8_t> { 0, 64, 128, 255 };
    auto const magnified = magnify(source, sized(2, 2), sized(2, 2), One);
    CHECK(magnified == source);
}

TEST_CASE("GlyphSlicing.magnify.interpolates_between_neighbours", "[glyphslicing]")
{
    // Bilinear, not nearest: the midpoint between a black and a white pixel must land in between,
    // because the draw-time stretching this replaced was the GPU's bilinear sampling.
    auto const source = std::vector<uint8_t> { 0, 255 };
    auto const magnified = magnify(source, sized(2, 1), sized(4, 1), One);

    REQUIRE(magnified.size() == 4);
    CHECK(magnified[0] == 0);
    CHECK(magnified[3] == 255);
    // The interior samples fall strictly between the two source values.
    CHECK(magnified[1] > 0);
    CHECK(magnified[1] < magnified[2]);
    CHECK(magnified[2] < 255);
}

TEST_CASE("GlyphSlicing.magnify.keeps_rgba_channels_apart", "[glyphslicing]")
{
    // Emoji are RGBA; interpolating across channel boundaries would smear colour into alpha.
    constexpr auto Rgba = size_t { 4 };
    auto const source = std::vector<uint8_t> { 10, 20, 30, 40 };
    auto const magnified = magnify(source, sized(1, 1), sized(2, 2), Rgba);

    REQUIRE(magnified.size() == 2 * 2 * Rgba);
    for (auto const pixel: std::views::iota(0zu, 4zu))
    {
        INFO("pixel " << pixel);
        CHECK(magnified[(pixel * Rgba) + 0] == 10);
        CHECK(magnified[(pixel * Rgba) + 1] == 20);
        CHECK(magnified[(pixel * Rgba) + 2] == 30);
        CHECK(magnified[(pixel * Rgba) + 3] == 40);
    }
}

TEST_CASE("GlyphSlicing.blockCanvasHash.separates_blocks_that_rasterize_differently", "[glyphslicing]")
{
    using vtbackend::CellScale;

    // Two blocks whose canvases differ must not share an atlas entry: whichever rasterizes first
    // would win it, and the other would draw its glyph at the wrong offset inside its own block.
    auto const base = CellScale { .scale = 4, .numerator = 1, .denominator = 2, .horizontalAlignment = 1 };

    SECTION("the cell span is part of the identity")
    {
        // Same cluster, same scale, different `w=`: blockPlacementFor takes the horizontal slack
        // from the cell span, so the glyph sits at a different offset on the canvas.
        CHECK(blockCanvasHash(base, 1) != blockCanvasHash(base, 2));
    }

    SECTION("the scale is part of the identity")
    {
        auto other = base;
        other.scale = 2;
        CHECK(blockCanvasHash(base, 1) != blockCanvasHash(other, 1));
    }

    SECTION("the fraction and alignment are part of the identity")
    {
        auto fraction = base;
        fraction.denominator = 3;
        CHECK(blockCanvasHash(base, 1) != blockCanvasHash(fraction, 1));

        auto alignment = base;
        alignment.horizontalAlignment = 2;
        CHECK(blockCanvasHash(base, 1) != blockCanvasHash(alignment, 1));
    }

    SECTION("identical blocks share their entry")
    {
        CHECK(blockCanvasHash(base, 2) == blockCanvasHash(base, 2));
    }
}

TEST_CASE("GlyphSlicing.subKey.is_unique_per_cell_and_never_zero", "[glyphslicing]")
{
    // The sub-key multiplies the glyph's hash, so a zero would collapse every tile onto one entry;
    // a collision would serve one cell of a block from another's pixels.
    auto seen = std::vector<uint32_t> {};
    for (auto const band: std::views::iota(0u, 8u))
        for (auto const column: std::views::iota(0u, 32u))
        {
            auto const key = glyphTileSubKey(column, band);
            INFO("(" << column << ", " << band << ") -> " << key);
            CHECK(key != 0);
            CHECK(std::ranges::find(seen, key) == seen.end());
            seen.emplace_back(key);
        }
}
