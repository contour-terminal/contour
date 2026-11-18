// SPDX-License-Identifier: Apache-2.0
#include <vtrasterizer/RegisteredGlyphRenderer.hpp>

#include <vtrasterizer/OutlineRasterizer.hpp>
#include <vtrasterizer/RegisteredGlyphPlacement.hpp>

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
        auto const* const tile = getOrCreateCachedTile(
            Tile { .glyph = glyph, .flags = flags, .tileCount = tileCount, .index = index });
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
    // The glyph's id is a hash of its content and already tagged apart from other renderers' keys.
    auto const flagBits = static_cast<uint32_t>((tile.flags & TileFlagsMask).value());
    auto const key = tile.glyph.id
                     * crispy::StrongHash { flagBits,
                                            static_cast<uint32_t>(tile.tileCount),
                                            static_cast<uint32_t>(tile.index),
                                            0 };
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
    auto const span =
        GlyphSpanGeometry { .width = static_cast<float>(cellWidth * tile.tileCount / stretchX),
                            .height = static_cast<float>(cellHeight),
                            .baseline = static_cast<float>(cellHeight - _gridMetrics.baseline) };
    auto const transform =
        placeRegisteredGlyph(tile.glyph.layout, tile.glyph.outline.bounds, span)
            .transform([&](GlyphSpanTransform placed) {
                return GlyphSpanTransform { .sx = placed.sx * static_cast<float>(stretchX),
                                            .sy = placed.sy * static_cast<float>(stretchY),
                                            .tx = placed.tx * static_cast<float>(stretchX),
                                            .ty = placed.ty * static_cast<float>(stretchY) };
            });

    // A glyph with nothing to draw still gets its (clear) tile, so it is not placed again every frame.
    auto bitmap =
        transform
            ? rasterizeOutline(
                  tile.glyph.outline,
                  *transform,
                  cellSize,
                  OutlineWindowOrigin { .x = tile.index * cellWidth,
                                        .y = tile.flags.test(LineFlag::DoubleHeightBottom) ? cellHeight : 0 })
            : std::vector<uint8_t>(cellSize.area(), uint8_t { 0 });

    return createTileData(tileLocation,
                          std::move(bitmap),
                          atlas::Format::Red,
                          cellSize,
                          RenderTileAttributes::X { 0 },
                          RenderTileAttributes::Y { 0 },
                          FRAGMENT_SELECTOR_GLYPH_ALPHA);
}

void RegisteredGlyphRenderer::inspect(std::ostream& /*output*/) const
{
}

} // namespace vtrasterizer
