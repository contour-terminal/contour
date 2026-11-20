// SPDX-License-Identifier: Apache-2.0
#include <vtrasterizer/RegisteredGlyphRenderer.hpp>

#include <vtrasterizer/RegisteredGlyphPlacement.hpp>
#include <vtrasterizer/VectorGlyphRasterizer.hpp>

#include <crispy/StrongHash.hpp>

#include <array>
#include <ranges>

using namespace vtbackend;

namespace vtrasterizer
{

namespace
{
    /// The line flags that change what a glyph's tile looks like.
    constexpr auto TileFlagsMask =
        LineFlags { LineFlag::DoubleWidth, LineFlag::DoubleHeightTop, LineFlag::DoubleHeightBottom };

    /// The most tiles one glyph takes: two cells, each two columns wide on a DECDWL row.
    constexpr auto MaxTiles = size_t { 4 };
} // namespace

RegisteredGlyphDraw RegisteredGlyphRenderer::render(CellLocation position,
                                                    RegisteredGlyph const& glyph,
                                                    GlyphRenderSpan span,
                                                    RGBColor color,
                                                    LineFlags flags)
{
    auto const tileCount = static_cast<int>(span) * (flags.test(LineFlag::DoubleWidth) ? 2 : 1);
    auto const origin = _gridMetrics.map(position.line, position.column, _smoothScrollYOffset);

    // Look every tile up before scheduling any, so a half-drawn glyph never reaches the screen.
    auto tiles = std::array<AtlasTileAttributes const*, MaxTiles> {};
    for (auto const index: std::views::iota(0, tileCount))
    {
        auto const* const tile = getOrCreateCachedTile(Tile {
            .glyph = glyph, .flags = flags, .foreground = color, .tileCount = tileCount, .index = index });
        if (!tile)
            return RegisteredGlyphDraw::AtlasFull;
        tiles.at(static_cast<size_t>(index)) = tile;
    }

    for (auto const index: std::views::iota(0, tileCount))
        renderTile(atlas::RenderTile::X { origin.x + (index * unbox<int>(_gridMetrics.cellSize.width)) },
                   atlas::RenderTile::Y { origin.y },
                   color,
                   *tiles.at(static_cast<size_t>(index)));
    return RegisteredGlyphDraw::Drawn;
}

Renderable::AtlasTileAttributes const* RegisteredGlyphRenderer::getOrCreateCachedTile(Tile const& tile)
{
    // The glyph's id is a hash of its content and already tagged apart from other renderers' keys. Only a
    // glyph that mixes the foreground with colours of its own looks different from one foreground to the
    // next: coverage is tinted when drawn, and fixed colours do not change.
    auto const flagBits = static_cast<uint32_t>((tile.flags & TileFlagsMask).value());
    auto const foreground = tile.glyph.definition.glyph.palette() == VectorPalette::Mixed
                                ? (uint32_t { 1 } << 24) | (uint32_t { tile.foreground.red } << 16)
                                      | (uint32_t { tile.foreground.green } << 8)
                                      | uint32_t { tile.foreground.blue }
                                : uint32_t { 0 };
    auto const key = tile.glyph.id
                     * crispy::StrongHash { flagBits,
                                            static_cast<uint32_t>(tile.tileCount),
                                            static_cast<uint32_t>(tile.index),
                                            foreground };
    return textureAtlas().getOrTryEmplace(
        key, [&](atlas::TileLocation tileLocation) -> std::optional<TextureAtlas::TileCreateData> {
            return createTile(tile, tileLocation);
        });
}

std::optional<Renderable::TextureAtlas::TileCreateData> RegisteredGlyphRenderer::createTile(
    Tile const& tile, atlas::TileLocation tileLocation)
{
    auto const cellSize = _gridMetrics.cellSize;
    if (cellSize.area() == 0)
        return std::nullopt;

    auto const cellWidth = unbox<int>(cellSize.width);
    auto const cellHeight = unbox<int>(cellSize.height);
    auto const doubleWidth = tile.flags.test(LineFlag::DoubleWidth);
    auto const doubleHeight =
        (tile.flags & LineFlags { LineFlag::DoubleHeightTop, LineFlag::DoubleHeightBottom }).any();

    // Placed as on an ordinary row, then stretched the way the row stretches its text: twice as wide on a
    // double-width line, twice as tall on a double-height one.
    auto const stretchX = doubleWidth ? 2 : 1;
    auto const stretchY = doubleHeight ? 2 : 1;
    auto const& definition = tile.glyph.definition;
    auto const span = GlyphSpanGeometry { .width = static_cast<float>(cellWidth * tile.tileCount / stretchX),
                                          .height = static_cast<float>(cellHeight) };
    auto const placement =
        placeRegisteredGlyph(definition.layout, definition.glyph.viewBox, span)
            .transform([&](GlyphSpanTransform placed) {
                return GlyphSpanTransform { .sx = placed.sx * static_cast<float>(stretchX),
                                            .sy = placed.sy * static_cast<float>(stretchY),
                                            .tx = placed.tx * static_cast<float>(stretchX),
                                            .ty = placed.ty * static_cast<float>(stretchY) };
            });
    if (!placement)
        return std::nullopt;

    auto drawn = rasterizeVectorGlyph(
        definition.glyph,
        *placement,
        cellSize,
        GlyphWindowOrigin { .x = tile.index * cellWidth,
                            .y = tile.flags.test(LineFlag::DoubleHeightBottom) ? cellHeight : 0 },
        tile.foreground);

    // Coverage is tinted by the shader that draws text; colour is drawn as it is, as an emoji's tile is.
    auto const inColor = drawn.format == GlyphPixels::Color;
    return createTileData(tileLocation,
                          std::move(drawn.pixels),
                          inColor ? atlas::Format::RGBA : atlas::Format::Red,
                          cellSize,
                          RenderTileAttributes::X { 0 },
                          RenderTileAttributes::Y { 0 },
                          inColor ? FRAGMENT_SELECTOR_IMAGE_BGRA : FRAGMENT_SELECTOR_GLYPH_ALPHA);
}

void RegisteredGlyphRenderer::inspect(std::ostream& /*output*/) const
{
}

} // namespace vtrasterizer
