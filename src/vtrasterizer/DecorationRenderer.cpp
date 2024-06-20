// SPDX-License-Identifier: Apache-2.0
#include <vtrasterizer/DecorationRenderer.h>
#include <vtrasterizer/GridMetrics.h>
#include <vtrasterizer/Pixmap.h>
#include <vtrasterizer/shared_defines.h>

#include <crispy/times.h>
#include <crispy/utils.h>

#include <array>
#include <cmath>
#include <iostream>
#include <optional>
#include <utility>

using crispy::each_element;
using crispy::size;

using std::array;
using std::ceil;
using std::clamp;
using std::floor;
using std::get;
using std::max;
using std::min;
using std::move;
using std::nullopt;
using std::optional;
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
    Require(_textureAtlas);

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
    for (auto const& mapping: CellFlagDecorationMappings)
        if (line.textAttributes.flags & mapping.first)
            renderDecoration(mapping.second,
                             _gridMetrics.mapBottomLeft(vtbackend::CellLocation { line.lineOffset }),
                             line.usedColumns,
                             line.textAttributes.decorationColor);
}

void DecorationRenderer::renderCell(vtbackend::RenderCell const& cell)
{
    for (auto const& mapping: CellFlagDecorationMappings)
        if (cell.attributes.flags & mapping.first)
            renderDecoration(mapping.second,
                             _gridMetrics.mapBottomLeft(cell.position),
                             vtbackend::ColumnCount(1),
                             cell.attributes.decorationColor);
}

auto DecorationRenderer::createTileData(Decorator decoration,
                                        atlas::TileLocation tileLocation) -> TextureAtlas::TileCreateData
{
    auto const width = _gridMetrics.cellSize.width;

    auto tileData = TextureAtlas::TileCreateData {};
    // NB: To be filled below: bitmapSize, bitmap.
    tileData.bitmapFormat = atlas::Format::Red;
    tileData.metadata.x = {};
    tileData.metadata.y = {};

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
                        image[(unbox(height) - y0 - y) * unbox(width) + x] = 0xFF;
                return image;
            });
        }
        case Decorator::DoubleUnderline: {
            auto const thickness = max(1u, unsigned(ceil(double(underlineThickness()) * 2.0) / 3.0));
            auto const y1 = max(0u, underlinePosition() + thickness);
            // y1 - 3 thickness can be negative
            auto const y0 = max(0, static_cast<int>(y1) - 3 * static_cast<int>(thickness));
            auto const height = vtbackend::Height(y1 + thickness);
            auto const imageSize = ImageSize { width, height };
            return create(imageSize, [&]() -> atlas::Buffer {
                auto image = atlas::Buffer(imageSize.area(), 0);
                for (unsigned y = 1; y <= thickness; ++y)
                {
                    for (auto x: crispy::times(unbox(width)))
                    {
                        image[(unbox(height) - y1 - y) * unbox(width) + x] = 0xFF; // top line
                        image[(unbox(height) - y0 - y) * unbox(width) + x] = 0xFF; // bottom line
                    }
                }
                return image;
            });
        }
        case Decorator::CurlyUnderline: {
            auto const height = vtbackend::Height::cast_from(_gridMetrics.baseline);
            auto const h2 = max(unbox<int>(height) / 2, 1);
            auto const yScalar = h2 - 1;
            auto const xScalar = 2 * M_PI / *width;
            auto const yBase = h2;
            auto const imageSize = ImageSize { width, height };
            auto block = blockElement(imageSize);
            return create(block.downsampledSize, [&]() -> atlas::Buffer {
                auto const thicknessHalf = max(1, int(ceil(underlineThickness() / 2.0)));
                for (auto x: crispy::times(unbox(width)))
                {
                    // Using Wu's antialiasing algorithm to paint the curved line.
                    // See: https://dl.acm.org/doi/pdf/10.1145/127719.122734
                    auto const y = yScalar * cos(xScalar * x);
                    auto const y1 = static_cast<int>(floor(y));
                    auto const y2 = static_cast<int>(ceil(y));
                    auto const intensity = static_cast<uint8_t>(255 * fabs(y - y1));
                    // block.paintOver(x, yBase + y1, 255 - intensity);
                    // block.paintOver(x, yBase + y2, intensity);
                    block.paintOverThick(x, yBase + y1, uint8_t(255 - intensity), thicknessHalf, 0);
                    block.paintOverThick(x, yBase + y2, intensity, thicknessHalf, 0);
                }
                return block.take();
            });
        }
        case Decorator::DottedUnderline: {
            auto const dotHeight = (unsigned) _gridMetrics.underline.thickness;
            auto const dotWidth = dotHeight;
            auto const height =
                vtbackend::Height::cast_from((unsigned) _gridMetrics.underline.position + dotHeight);
            auto const y0 = (unsigned) _gridMetrics.underline.position - dotHeight;
            auto const x0 = 0u;
            auto const x1 = unbox(width) / 2;
            auto block = blockElement(ImageSize { width, height });
            return create(block.downsampledSize, [&]() -> atlas::Buffer {
                for (auto y: crispy::times(dotHeight))
                {
                    for (auto x: crispy::times(dotWidth))
                    {
                        block.paint(int(x + x0), int(y + y0));
                        block.paint(int(x + x1), int(y + y0));
                    }
                }
                return block.take();
            });
        }
        case Decorator::DashedUnderline: {
            // Devides a grid cell's underline in three sub-ranges and only renders first and third one,
            // whereas the middle one is being skipped.
            auto const thicknessHalf = max(1u, unsigned(ceil(underlineThickness() / 2.0)));
            auto const thickness = max(1u, thicknessHalf * 2);
            auto const y0 = max(0, underlinePosition() - static_cast<int>(thicknessHalf));
            auto const height = vtbackend::Height(y0 + thickness);
            auto const imageSize = ImageSize { width, height };
            return create(imageSize, [&]() -> atlas::Buffer {
                auto image = atlas::Buffer(unbox<size_t>(width) * unbox<size_t>(height), 0);
                for (unsigned y = 1; y <= thickness; ++y)
                    for (auto x: crispy::times(unbox(width)))
                        if (fabsf(float(x) / unbox<float>(width) - 0.5f) >= 0.25f)
                            image[(unbox(height) - y0 - y) * unbox(width) + x] = 0xFF;
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
                        image[y * unbox(width) + x] = 0xFF;
                        image[(unbox(cellHeight) - 1 - y) * unbox(width) + x] = 0xFF;
                    }

                // Draws the left and right vertical lines
                for (unsigned y = gap; y < unbox(cellHeight) - gap; y++)
                    for (unsigned x = gap; x < thickness + gap; ++x)
                    {
                        image[y * unbox(width) + x] = 0xFF;
                        image[y * unbox(width) + (unbox(width) - 1 - x)] = 0xFF;
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
                        image[y * unbox(width) + x] = 0xFF;
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
                        image[y * unbox(width) + x] = 0xFF;
                return image;
            });
        }
    }
    Require(false && "Unhandled case.");
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
        auto const tileLocation = _textureAtlas->tileLocation(tileIndex);
        auto const tileData = createTileData(decoration, tileLocation);
        AtlasTileAttributes const& tileAttributes = _textureAtlas->directMapped(tileIndex);
        renderTile({ pos.x + unbox(i) * unbox<int>(_gridMetrics.cellSize.width) },
                   { pos.y - unbox<int>(tileAttributes.bitmapSize.height) },
                   color,
                   tileAttributes);
    }
}

} // namespace vtrasterizer
