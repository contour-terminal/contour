// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <vtbackend/TextScale.h>
#include <vtbackend/primitives.h>

#include <crispy/StrongHash.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <span>
#include <vector>

namespace vtrasterizer
{

/// One cell-sized cut of a block's raster, and which cell of the block it came from.
///
/// A text-sizing block (`OSC 66` `s=`) is @c scale cells tall and drawn across as many screen rows,
/// but the texture atlas stores tiles of exactly one cell: TextureAtlas spaces tile origins
/// @c tileSize apart, so anything larger overwrites its neighbour. The block is therefore
/// rasterized once and then cut into these.
///
/// kitty carries the same idea as `extract_cell_region` (fonts.c), which likewise never hands the
/// GPU a sprite larger than one unscaled cell.
struct GlyphTile
{
    /// Cell column within the block; 0 is the leftmost.
    uint32_t column = 0;

    /// Cell row within the block; 0 is the head row.
    uint32_t band = 0;

    /// Always within the tile size the cut was asked for.
    vtbackend::ImageSize size {};

    std::vector<uint8_t> bitmap {};
};

/// Copies @p source into @p target at (@p originX, @p originY), clipping at every edge.
///
/// The origin may be negative or beyond the target: a glyph positioned on its block's baseline
/// routinely overhangs, and a descender or an accent that falls outside the block is simply not
/// drawn rather than being an error.
///
/// @param components bytes per pixel; both images must share it.
inline void blitClipped(std::span<uint8_t> target,
                        vtbackend::ImageSize targetSize,
                        std::span<uint8_t const> source,
                        vtbackend::ImageSize sourceSize,
                        int originX,
                        int originY,
                        size_t components) noexcept
{
    auto const targetWidth = unbox<int>(targetSize.width);
    auto const targetHeight = unbox<int>(targetSize.height);
    auto const sourceWidth = unbox<int>(sourceSize.width);
    auto const sourceHeight = unbox<int>(sourceSize.height);

    // The overlapping band, in source coordinates.
    auto const firstRow = std::max(0, -originY);
    auto const lastRow = std::min(sourceHeight, targetHeight - originY);
    auto const firstColumn = std::max(0, -originX);
    auto const lastColumn = std::min(sourceWidth, targetWidth - originX);
    if (firstRow >= lastRow || firstColumn >= lastColumn)
        return;

    auto const runLength = static_cast<size_t>(lastColumn - firstColumn) * components;

    for (auto sourceRow = firstRow; sourceRow < lastRow; ++sourceRow)
    {
        auto const targetRow = sourceRow + originY;
        auto const sourceOffset = ((static_cast<size_t>(sourceRow) * static_cast<size_t>(sourceWidth))
                                   + static_cast<size_t>(firstColumn))
                                  * components;
        auto const targetOffset = ((static_cast<size_t>(targetRow) * static_cast<size_t>(targetWidth))
                                   + static_cast<size_t>(firstColumn + originX))
                                  * components;

        if (sourceOffset + runLength > source.size() || targetOffset + runLength > target.size())
            return;

        std::copy_n(source.begin() + static_cast<ptrdiff_t>(sourceOffset),
                    runLength,
                    target.begin() + static_cast<ptrdiff_t>(targetOffset));
    }
}

/// Cuts @p canvas into a grid of tiles of at most @p tileSize each.
///
/// Tiles at the right and bottom edges are smaller when the canvas is not a whole number of tiles;
/// they are never padded, so a caller may rely on @c GlyphTile::size being the real extent.
///
/// @param components bytes per pixel.
[[nodiscard]] inline std::vector<GlyphTile> sliceIntoTiles(std::span<uint8_t const> canvas,
                                                           vtbackend::ImageSize canvasSize,
                                                           vtbackend::ImageSize tileSize,
                                                           size_t components)
{
    auto tiles = std::vector<GlyphTile> {};
    if (unbox(tileSize.width) == 0 || unbox(tileSize.height) == 0)
        return tiles;

    auto const canvasWidth = unbox<uint32_t>(canvasSize.width);
    auto const canvasHeight = unbox<uint32_t>(canvasSize.height);
    auto const tileWidth = unbox<uint32_t>(tileSize.width);
    auto const tileHeight = unbox<uint32_t>(tileSize.height);

    for (uint32_t top = 0, band = 0; top < canvasHeight; top += tileHeight, ++band)
    {
        auto const rows = std::min(tileHeight, canvasHeight - top);
        for (uint32_t left = 0, column = 0; left < canvasWidth; left += tileWidth, ++column)
        {
            auto const columns = std::min(tileWidth, canvasWidth - left);

            auto tile = GlyphTile { .column = column,
                                    .band = band,
                                    .size = vtbackend::ImageSize { vtbackend::Width::cast_from(columns),
                                                                   vtbackend::Height::cast_from(rows) } };
            tile.bitmap.resize(static_cast<size_t>(columns) * rows * components);

            auto const runLength = static_cast<size_t>(columns) * components;
            for (uint32_t row = 0; row < rows; ++row)
            {
                auto const sourceOffset =
                    ((static_cast<size_t>(top + row) * canvasWidth) + left) * components;
                if (sourceOffset + runLength > canvas.size())
                    break;
                std::copy_n(canvas.begin() + static_cast<ptrdiff_t>(sourceOffset),
                            runLength,
                            tile.bitmap.begin() + static_cast<ptrdiff_t>(row * runLength));
            }

            tiles.emplace_back(std::move(tile));
        }
    }

    return tiles;
}

/// Magnifies @p source to @p targetSize by bilinear interpolation.
///
/// The Stretch scaling strategy magnifies an ordinary-size raster rather than re-hinting the
/// outline, and the block canvas composites pixels -- so unlike the draw-time stretching this
/// replaced, the enlargement has to happen here. text::scale only ever scales DOWN (it asserts so),
/// which is why this is not that.
///
/// Bilinear rather than nearest-neighbour because the draw-time stretching it replaces was the GPU's
/// bilinear sampling; nearest would have made Stretch visibly blockier than before.
///
/// @param components bytes per pixel; each is interpolated independently.
[[nodiscard]] inline std::vector<uint8_t> magnify(std::span<uint8_t const> source,
                                                  vtbackend::ImageSize sourceSize,
                                                  vtbackend::ImageSize targetSize,
                                                  size_t components)
{
    auto const sourceWidth = unbox<size_t>(sourceSize.width);
    auto const sourceHeight = unbox<size_t>(sourceSize.height);
    auto const targetWidth = unbox<size_t>(targetSize.width);
    auto const targetHeight = unbox<size_t>(targetSize.height);

    auto target = std::vector<uint8_t>(targetWidth * targetHeight * components, uint8_t { 0 });
    if (sourceWidth == 0 || sourceHeight == 0 || targetWidth == 0 || targetHeight == 0)
        return target;
    if (source.size() < sourceWidth * sourceHeight * components)
        return target;

    // Map target pixel centres back into source space, so the edges do not drift by half a pixel.
    auto const ratioX = static_cast<double>(sourceWidth) / static_cast<double>(targetWidth);
    auto const ratioY = static_cast<double>(sourceHeight) / static_cast<double>(targetHeight);

    for (size_t y = 0; y < targetHeight; ++y)
    {
        auto const sourceY = ((static_cast<double>(y) + 0.5) * ratioY) - 0.5;
        auto const y0 = static_cast<size_t>(std::max(0.0, std::floor(sourceY)));
        auto const y1 = std::min(y0 + 1, sourceHeight - 1);
        auto const wy = std::clamp(sourceY - static_cast<double>(y0), 0.0, 1.0);

        for (size_t x = 0; x < targetWidth; ++x)
        {
            auto const sourceX = ((static_cast<double>(x) + 0.5) * ratioX) - 0.5;
            auto const x0 = static_cast<size_t>(std::max(0.0, std::floor(sourceX)));
            auto const x1 = std::min(x0 + 1, sourceWidth - 1);
            auto const wx = std::clamp(sourceX - static_cast<double>(x0), 0.0, 1.0);

            for (size_t component = 0; component < components; ++component)
            {
                auto const at = [&](size_t sx, size_t sy) {
                    return static_cast<double>(source[(((sy * sourceWidth) + sx) * components) + component]);
                };
                auto const top = (at(x0, y0) * (1.0 - wx)) + (at(x1, y0) * wx);
                auto const bottom = (at(x0, y1) * (1.0 - wx)) + (at(x1, y1) * wx);
                auto const value = (top * (1.0 - wy)) + (bottom * wy);

                target[(((y * targetWidth) + x) * components) + component] =
                    static_cast<uint8_t>(std::lround(std::clamp(value, 0.0, 255.0)));
            }
        }
    }

    return target;
}

/// The atlas sub-key under which the tile at (@p column, @p band) of a block is cached.
///
/// The block's head tile -- (0, 0) -- is stored under the glyph's own hash, so this never needs to
/// answer for it; starting at 1 keeps the multiplication from collapsing a key to zero.
[[nodiscard]] constexpr uint32_t glyphTileSubKey(uint32_t column, uint32_t band) noexcept
{
    return 1 + column + (band * 256);
}

/// The part of a block's atlas identity that does not come from its glyphs.
///
/// Everything that changes the PIXELS the block's tiles are cut from has to be in here, or two
/// blocks share one atlas entry and whichever rasterizes first wins. That is easy to get wrong for
/// @p cellsAtOneX in particular, because it is derived rather than given: it comes from the `w=` the
/// application asked for, and buildBlockCanvas lays the glyph out against it -- the horizontal slack
/// blockPlacementFor distributes is measured in those cells. Two blocks of the same cluster at the
/// same scale but different `w=` therefore produce different canvases.
///
/// @param scale        the block's full sizing: its scale plus the fraction and alignment, all of
///                     which change the pixels rather than merely the extent.
/// @param cellsAtOneX  how many cells the block's content occupies at scale 1.
[[nodiscard]] inline crispy::strong_hash blockCanvasHash(vtbackend::CellScale const& scale,
                                                         uint32_t cellsAtOneX) noexcept
{
    // The `+ 1` guards keep a zero factor from collapsing the whole product.
    return crispy::strong_hash::compute(static_cast<uint32_t>(scale.scale))
           * (vtbackend::packTextScaleExtras(scale) + 1u) * (cellsAtOneX + 1u);
}

} // namespace vtrasterizer
