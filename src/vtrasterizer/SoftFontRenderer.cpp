// SPDX-License-Identifier: Apache-2.0
#include <vtrasterizer/SoftFontRenderer.hpp>

#include <crispy/StrongHash.hpp>

#include <algorithm>
#include <ranges>

using namespace vtbackend;

namespace vtrasterizer
{

namespace
{
    /// The line flags that change what a glyph's tile looks like.
    constexpr auto TileFlagsMask =
        LineFlags { LineFlag::DoubleWidth, LineFlag::DoubleHeightTop, LineFlag::DoubleHeightBottom };

    /// Tags soft glyph keys apart from every other tile keyed in the atlas ('SOFT').
    constexpr auto AtlasKeyTag = uint32_t { 0x534F'4654 };
} // namespace

std::vector<uint8_t> scaleSoftGlyph(SoftGlyph const& glyph, ImageSize target)
{
    auto const targetWidth = unbox<size_t>(target.width);
    auto const targetHeight = unbox<size_t>(target.height);
    auto pixels = std::vector<uint8_t>(targetWidth * targetHeight, uint8_t { 0 });
    if (glyph.width == 0 || glyph.height == 0
        || glyph.coverage.size() < size_t { glyph.width } * glyph.height)
        return pixels;

    for (auto const y: std::views::iota(size_t { 0 }, targetHeight))
    {
        auto const sourceRow = (y * glyph.height / targetHeight) * glyph.width;
        for (auto const x: std::views::iota(size_t { 0 }, targetWidth))
            pixels[(y * targetWidth) + x] = glyph.coverage[sourceRow + (x * glyph.width / targetWidth)] != 0
                                                ? uint8_t { 0xFF }
                                                : uint8_t { 0 };
    }
    return pixels;
}

SoftGlyphDraw SoftFontRenderer::render(CellLocation position,
                                       SoftGlyph const& glyph,
                                       RGBColor color,
                                       LineFlags flags)
{
    auto const tileCount = flags.test(LineFlag::DoubleWidth) ? 2 : 1;
    auto const origin = _gridMetrics.map(position.line, position.column, _smoothScrollYOffset);

    // Look every tile up before scheduling any, so a half-drawn double-width glyph never reaches the screen.
    auto tiles = std::array<AtlasTileAttributes const*, 2> {};
    for (auto const i: std::views::iota(0, tileCount))
    {
        tiles.at(static_cast<size_t>(i)) = getOrCreateCachedTile(glyph, flags, i);
        if (!tiles.at(static_cast<size_t>(i)))
            return SoftGlyphDraw::AtlasFull;
    }

    for (auto const i: std::views::iota(0, tileCount))
    {
        auto const& data = *tiles.at(static_cast<size_t>(i));
        auto renderTile = atlas::RenderTile {};
        renderTile.x = atlas::RenderTile::X { origin.x + (i * unbox<int>(_gridMetrics.cellSize.width)) };
        renderTile.y = atlas::RenderTile::Y { origin.y };
        renderTile.bitmapSize = data.bitmapSize;
        renderTile.color = atlas::normalize(color);
        renderTile.normalizedLocation = data.metadata.normalizedLocation;
        renderTile.tileLocation = data.location;
        textureScheduler().renderTile(renderTile);
    }
    return SoftGlyphDraw::Drawn;
}

Renderable::AtlasTileAttributes const* SoftFontRenderer::getOrCreateCachedTile(SoftGlyph const& glyph,
                                                                               LineFlags flags,
                                                                               int subIndex)
{
    auto const flagBits = static_cast<uint32_t>((flags & TileFlagsMask).value());
    auto const key =
        glyph.id
        * crispy::StrongHash { AtlasKeyTag, 0, 0, (flagBits << 1) | static_cast<uint32_t>(subIndex) };
    return textureAtlas().getOrTryEmplace(
        key, [&](atlas::TileLocation tileLocation) -> std::optional<TextureAtlas::TileCreateData> {
            return createSoftTile(glyph, flags, tileLocation, subIndex);
        });
}

std::optional<Renderable::TextureAtlas::TileCreateData> SoftFontRenderer::createSoftTile(
    SoftGlyph const& glyph, LineFlags flags, atlas::TileLocation tileLocation, int subIndex)
{
    auto const cellSize = _gridMetrics.cellSize;
    if (cellSize.area() == 0)
        return std::nullopt;

    auto const doubleWidth = flags.test(LineFlag::DoubleWidth);
    auto const doubleHeight =
        (flags & LineFlags { LineFlag::DoubleHeightTop, LineFlag::DoubleHeightBottom }).any();

    auto const logicalSize =
        ImageSize { doubleWidth ? Width::cast_from(unbox(cellSize.width) * 2) : cellSize.width,
                    doubleHeight ? Height::cast_from(unbox(cellSize.height) * 2) : cellSize.height };
    auto const scaled = scaleSoftGlyph(glyph, logicalSize);

    // Slice the cell's tile out of the scaled glyph: the subIndex-th cell across, and the top or bottom half
    // down. The rows are kept top first, which is how the atlas takes an alpha bitmap (a FreeType glyph is
    // uploaded the same way).
    auto const rowSize = unbox<size_t>(logicalSize.width);
    auto const tileWidth = unbox<size_t>(cellSize.width);
    auto const tileHeight = unbox<size_t>(cellSize.height);
    auto const sourceX = static_cast<size_t>(subIndex) * tileWidth;
    auto const sourceY = flags.test(LineFlag::DoubleHeightBottom) ? tileHeight : size_t { 0 };

    auto tile = std::vector<uint8_t>(tileWidth * tileHeight);
    for (auto const y: std::views::iota(size_t { 0 }, tileHeight))
        std::copy_n(scaled.begin() + static_cast<std::ptrdiff_t>(((sourceY + y) * rowSize) + sourceX),
                    tileWidth,
                    tile.begin() + static_cast<std::ptrdiff_t>(y * tileWidth));

    return createTileData(tileLocation,
                          std::move(tile),
                          atlas::Format::Red,
                          cellSize,
                          RenderTileAttributes::X { 0 },
                          RenderTileAttributes::Y { 0 },
                          FRAGMENT_SELECTOR_GLYPH_ALPHA);
}

void SoftFontRenderer::inspect(std::ostream& /*output*/) const
{
}

} // namespace vtrasterizer
