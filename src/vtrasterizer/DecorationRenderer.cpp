// SPDX-License-Identifier: Apache-2.0
#include <vtrasterizer/DecorationRenderer.h>

#include <vtrasterizer/GridMetrics.h>
#include <vtrasterizer/Pixmap.h>
#include <vtrasterizer/UnderlineGeometry.h>
#include <vtrasterizer/shared_defines.h>

#include <crispy/times.h>
#include <crispy/utils.h>

#include <array>
#include <cmath>
#include <iostream>
#include <numbers>
#include <optional>
#include <ranges>
#include <utility>

using crispy::each_element;

using std::array;
using std::ceil;
using std::floor;
using std::max;
using std::pair;
using std::string;

namespace vtrasterizer
{

namespace
{
    auto constexpr CellFlagDecorationMappings = array {
        pair { vtbackend::CellFlag::Underline, Decorator::Underline },
        pair { vtbackend::CellFlag::DoublyUnderlined, Decorator::DoubleUnderline },
        pair { vtbackend::CellFlag::CurlyUnderlined, Decorator::CurlyUnderline },
        pair { vtbackend::CellFlag::DottedUnderline, Decorator::DottedUnderline },
        pair { vtbackend::CellFlag::DashedUnderline, Decorator::DashedUnderline },
        pair { vtbackend::CellFlag::Overline, Decorator::Overline },
        pair { vtbackend::CellFlag::CrossedOut, Decorator::CrossedOut },
        pair { vtbackend::CellFlag::Framed, Decorator::Framed },
        pair { vtbackend::CellFlag::Encircled, Decorator::Encircle },
    };
}

DecorationRenderer::DecorationRenderer(GridMetrics const& gridMetrics,
                                       Decorator hyperlinkNormal,
                                       Decorator hyperlinkHover):
    Renderable { gridMetrics }, _hyperlinkNormal { hyperlinkNormal }, _hyperlinkHover { hyperlinkHover }
{
}

constexpr inline uint32_t DirectMappedDecorationCount = std::numeric_limits<Decorator>::count();

void DecorationRenderer::setRenderTarget(RenderTarget& renderTarget,
                                         DirectMappingAllocator& directMappingAllocator)
{
    Renderable::setRenderTarget(renderTarget, directMappingAllocator);
    _directMapping = directMappingAllocator.allocate(DirectMappedDecorationCount);
    clearCache();
}

void DecorationRenderer::setTextureAtlas(TextureAtlas& atlas)
{
    Renderable::setTextureAtlas(atlas);
    initializeDirectMapping();
}

void DecorationRenderer::clearCache()
{
}

void DecorationRenderer::initializeDirectMapping()
{
    if (!SoftRequire(_textureAtlas))
        return;

    for (Decorator const decoration: each_element<Decorator>())
    {
        auto const tileIndex = _directMapping.toTileIndex(static_cast<uint32_t>(decoration));
        auto const tileLocation = _textureAtlas->tileLocation(tileIndex);
        TextureAtlas::TileCreateData tileData = createTileData(decoration, tileLocation);
        _textureAtlas->setDirectMapping(tileIndex, std::move(tileData));
    }
}

void DecorationRenderer::inspect(std::ostream& /*output*/) const
{
}

void DecorationRenderer::renderLine(vtbackend::RenderLine const& line)
{
    auto const scale = line.flags.test(vtbackend::LineFlag::DoubleWidth) ? 2 : 1;
    for (auto const& mapping: CellFlagDecorationMappings)
        if (line.textAttributes.flags & mapping.first)
            renderDecoration(mapping.second,
                             _gridMetrics.mapBottomLeft(vtbackend::CellLocation { .line = line.lineOffset },
                                                        _smoothScrollYOffset),
                             line.usedColumns * scale,
                             line.textAttributes.decorationColor);
}

void DecorationRenderer::renderCell(vtbackend::RenderCell const& cell)
{
    auto const scale = cell.attributes.lineFlags.test(vtbackend::LineFlag::DoubleWidth) ? 2 : 1;
    for (auto const& mapping: CellFlagDecorationMappings)
        if (cell.attributes.flags & mapping.first)
            renderDecoration(mapping.second,
                             _gridMetrics.mapBottomLeft(cell.position, _smoothScrollYOffset),
                             vtbackend::ColumnCount(cell.width * scale),
                             cell.attributes.decorationColor);
}

// NOLINTNEXTLINE(readability-function-cognitive-complexity)
auto DecorationRenderer::createTileData(Decorator decoration, atlas::TileLocation tileLocation)
    -> TextureAtlas::TileCreateData
{
    auto const width = _gridMetrics.cellSize.width;

    auto tileData = TextureAtlas::TileCreateData {};
    // NB: To be filled below: bitmapSize, bitmap.
    tileData.bitmapFormat = atlas::Format::Red;
    tileData.metadata.x.value = 0;
    tileData.metadata.y.value = 0;

    auto const create = [this, tileLocation](ImageSize bitmapSize,
                                             auto createBitmap) -> TextureAtlas::TileCreateData {
        return createTileData(tileLocation,
                              createBitmap(),
                              atlas::Format::Red,
                              bitmapSize,
                              RenderTileAttributes::X { 0 },
                              RenderTileAttributes::Y { 0 },
                              FRAGMENT_SELECTOR_GLYPH_ALPHA);
    };

    switch (decoration)
    {
        case Decorator::Encircle:
            // TODO (default to Underline for now)
            [[fallthrough]];
        case Decorator::Underline: {
            auto const thickness = max(1u, unsigned(ceil(underlineThickness() / 2.0)));
            auto const y0 = max(0, (underlinePosition() - static_cast<int>(thickness)));
            auto const height = vtbackend::Height(y0 + thickness);
            auto const imageSize = ImageSize { width, height };
            return create(imageSize, [&]() -> atlas::Buffer {
                auto image = atlas::Buffer(imageSize.area(), 0);
                for (unsigned y = 1; y <= thickness; ++y)
                    for (auto x: crispy::times(unbox(width)))
                        image[((unbox(height) - y0 - y) * unbox(width)) + x] = 0xFF;
                return image;
            });
        }
        case Decorator::DoubleUnderline: {
            // Two thirds of the underline's thickness per stroke. The parentheses used to close
            // after ceil(), so this truncated instead of rounding up and an even thickness lost a
            // pixel: at thickness 2 it yielded 1.
            auto const thickness = max(1u, unsigned(ceil(double(underlineThickness()) * 2.0 / 3.0)));
            // The upper stroke's top is the underline position, as it is for every other
            // decoration. It used to be `position + thickness`, which placed the stroke's top two
            // thicknesses higher -- above the baseline, and so inside the glyphs.
            // Clamped before the cast to unsigned, never after: an unsigned subtraction wraps to a
            // huge positive, which max() then happily keeps.
            auto const y1 = static_cast<unsigned>(max(0, underlinePosition() - static_cast<int>(thickness)));
            // y1 - 3 thickness can be negative
            auto const y0 = max(0, static_cast<int>(y1) - (3 * static_cast<int>(thickness)));
            auto const height = vtbackend::Height(y1 + thickness);
            auto const imageSize = ImageSize { width, height };
            return create(imageSize, [&]() -> atlas::Buffer {
                auto image = atlas::Buffer(imageSize.area(), 0);
                for (unsigned y = 1; y <= thickness; ++y)
                {
                    for (auto x: crispy::times(unbox(width)))
                    {
                        image[((unbox(height) - y1 - y) * unbox(width)) + x] = 0xFF; // top line
                        image[((unbox(height) - y0 - y) * unbox(width)) + x] = 0xFF; // bottom line
                    }
                }
                return image;
            });
        }
        case Decorator::CurlyUnderline: {
            // The wave is sized from the underline position rather than from the baseline: the
            // latter is `lineHeight - ascender` and so folds in the font's whole line gap, which
            // grew the curl to the height of a lowercase letter on a font with leading (#1754).
            auto const geometry = curlyUnderlineGeometry(
                underlinePosition(), underlineThickness(), unbox<int>(_gridMetrics.cellSize.height));
            auto const height = vtbackend::Height::cast_from(geometry.tileHeight);
            // One full cycle per cell, so adjacent cells join crest to crest.
            auto const xScalar = 2 * std::numbers::pi / unbox<double>(width);
            auto const imageSize = ImageSize { width, height };
            auto block = blockElement(imageSize);
            return create(block.downsampledSize, [&]() -> atlas::Buffer {
                for (auto x: crispy::times(unbox(width)))
                {
                    // Using Wu's antialiasing algorithm to paint the curved line.
                    // See: https://dl.acm.org/doi/pdf/10.1145/127719.122734
                    auto const y = geometry.amplitude * cos(xScalar * static_cast<double>(x));
                    auto const y1 = static_cast<int>(floor(y));
                    auto const y2 = static_cast<int>(ceil(y));
                    auto const intensity = static_cast<uint8_t>(255 * fabs(y - y1));
                    // The stroke thickens along y, not x: a near-horizontal cosine widened
                    // sideways gains no weight and leaves holes where the curve is steepest.
                    block.paintOverThick(static_cast<int>(x),
                                         geometry.centerY + y1,
                                         uint8_t(255 - intensity),
                                         0,
                                         geometry.strokeRadius);
                    block.paintOverThick(
                        static_cast<int>(x), geometry.centerY + y2, intensity, 0, geometry.strokeRadius);
                }
                return block.take();
            });
        }
        case Decorator::DottedUnderline: {
            // Signed throughout, and the dot shrinks to the room it has: computing the origin as
            // `(unsigned) position - thickness` wrapped whenever a font put its underline within
            // one thickness of the cell bottom, and Pixmap::paint then clipped every dot away --
            // the decoration vanished silently rather than merely looking cramped.
            auto const dotSize = std::clamp(underlineThickness(), 1, max(1, underlinePosition()));
            auto const y0 = max(0, underlinePosition() - dotSize);
            auto const height = vtbackend::Height::cast_from(y0 + dotSize);
            // Two dots per cell: one flush with the left edge, one at the half-way mark.
            auto const x1 = unbox<int>(width) / 2;
            auto block = blockElement(ImageSize { width, height });
            return create(block.downsampledSize, [&]() -> atlas::Buffer {
                for (auto const y: std::views::iota(0, dotSize))
                {
                    for (auto const x: std::views::iota(0, dotSize))
                    {
                        block.paint(x, y + y0);
                        block.paint(x + x1, y + y0);
                    }
                }
                return block.take();
            });
        }
        case Decorator::DashedUnderline: {
            // Divides a grid cell's underline in three sub-ranges and only renders first and third one,
            // whereas the middle one is being skipped.
            auto const thicknessHalf = max(1u, unsigned(ceil(underlineThickness() / 2.0)));
            auto const thickness = thicknessHalf * 2; // at least 2, thicknessHalf being at least 1
            // Subtracting the whole thickness, not half of it: the dashes' top is the underline
            // position, as it is for every other decoration. Half left them one row higher, on the
            // baseline itself.
            auto const y0 = max(0, underlinePosition() - static_cast<int>(thickness));
            auto const height = vtbackend::Height(y0 + thickness);
            auto const imageSize = ImageSize { width, height };
            return create(imageSize, [&]() -> atlas::Buffer {
                auto image = atlas::Buffer(unbox<size_t>(width) * unbox<size_t>(height), 0);
                for (unsigned y = 1; y <= thickness; ++y)
                    for (auto x: crispy::times(unbox(width)))
                        if (fabsf((float(x) / unbox<float>(width)) - 0.5f) >= 0.25f)
                            image[((unbox(height) - y0 - y) * unbox(width)) + x] = 0xFF;
                return image;
            });
        }
        case Decorator::Framed: {
            auto const cellHeight = _gridMetrics.cellSize.height;
            auto const thickness = max(1u, unsigned(underlineThickness()) / 2);
            auto const imageSize = ImageSize { width, cellHeight };
            return create(imageSize, [&]() -> atlas::Buffer {
                auto image = atlas::Buffer(unbox<size_t>(width) * unbox<size_t>(cellHeight), 0);
                auto const gap = 0; // thickness;
                // Draws the top and bottom horizontal lines
                for (unsigned y = gap; y < thickness + gap; ++y)
                    for (unsigned x = gap; x < unbox(width) - gap; ++x)
                    {
                        image[(y * unbox(width)) + x] = 0xFF;
                        image[((unbox(cellHeight) - 1 - y) * unbox(width)) + x] = 0xFF;
                    }

                // Draws the left and right vertical lines
                for (unsigned y = gap; y < unbox(cellHeight) - gap; y++)
                    for (unsigned x = gap; x < thickness + gap; ++x)
                    {
                        image[(y * unbox(width)) + x] = 0xFF;
                        image[(y * unbox(width)) + (unbox(width) - 1 - x)] = 0xFF;
                    }
                return image;
            });
        }
        case Decorator::Overline: {
            auto const cellHeight = _gridMetrics.cellSize.height;
            auto const thickness = (unsigned) underlineThickness();
            auto const imageSize = ImageSize { width, cellHeight };
            return create(imageSize, [&]() -> atlas::Buffer {
                auto image = atlas::Buffer(unbox<size_t>(width) * unbox<size_t>(cellHeight), 0);
                for (auto y: crispy::times(thickness))
                    for (auto x: crispy::times(unbox(width)))
                        image[(y * unbox(width)) + x] = 0xFF;
                return image;
            });
        }
        case Decorator::CrossedOut: {
            auto const height = vtbackend::Height(*_gridMetrics.cellSize.height / 2);
            auto const thickness = (unsigned) underlineThickness();
            auto const imageSize = ImageSize { width, height };
            return create(imageSize, [&]() -> atlas::Buffer {
                auto image = atlas::Buffer(unbox<size_t>(width) * unbox<size_t>(height), 0);
                for (unsigned y = 1; y <= thickness; ++y)
                    for (auto x: crispy::times(unbox(width)))
                        image[(y * unbox(width)) + x] = 0xFF;
                return image;
            });
        }
    }
    (void) SoftRequire(false);
    return {};
}

void DecorationRenderer::renderDecoration(Decorator decoration,
                                          crispy::point pos,
                                          vtbackend::ColumnCount columnCount,
                                          vtbackend::RGBColor const& color)
{
    for (auto i = vtbackend::ColumnCount(0); i < columnCount; ++i)
    {
        auto const tileIndex = _directMapping.toTileIndex(static_cast<uint32_t>(decoration));
        // auto const tileLocation = _textureAtlas->tileLocation(tileIndex); // unused
        // auto const tileData = createTileData(decoration, tileLocation); // unused?
        AtlasTileAttributes const& tileAttributes = _textureAtlas->directMapped(tileIndex);
        auto tileAttributesCopy = tileAttributes;

        renderTile({ pos.x + (unbox(i) * unbox<int>(_gridMetrics.cellSize.width)) },
                   { pos.y - unbox<int>(tileAttributes.bitmapSize.height) },
                   color,
                   tileAttributesCopy);
    }
}

} // namespace vtrasterizer
