/**
 * This file is part of the "contour" project.
 *   Copyright (c) 2020 Christian Parpart <christian@parpart.family>
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */
#include <terminal_renderer/DecorationRenderer.h>
#include <terminal_renderer/GridMetrics.h>
#include <terminal_renderer/Pixmap.h>
#include <terminal_renderer/shared_defines.h>

#include <crispy/times.h>
#include <crispy/utils.h>

#include <array>
#include <cmath>
#include <iostream>
#include <optional>
#include <utility>

using crispy::each_element;
using crispy::Size;

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

namespace terminal::renderer
{

namespace
{
    auto constexpr CellFlagDecorationMappings = array {
        pair { CellFlags::Underline, Decorator::Underline },
        pair { CellFlags::DoublyUnderlined, Decorator::DoubleUnderline },
        pair { CellFlags::CurlyUnderlined, Decorator::CurlyUnderline },
        pair { CellFlags::DottedUnderline, Decorator::DottedUnderline },
        pair { CellFlags::DashedUnderline, Decorator::DashedUnderline },
        pair { CellFlags::Overline, Decorator::Overline },
        pair { CellFlags::CrossedOut, Decorator::CrossedOut },
        pair { CellFlags::Framed, Decorator::Framed },
        pair { CellFlags::Encircled, Decorator::Encircle },
    };
}

DecorationRenderer::DecorationRenderer(GridMetrics const& _gridMetrics,
                                       Decorator _hyperlinkNormal,
                                       Decorator _hyperlinkHover):
    Renderable { _gridMetrics }, hyperlinkNormal_ { _hyperlinkNormal }, hyperlinkHover_ { _hyperlinkHover }
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
        _textureAtlas->setDirectMapping(tileIndex, move(tileData));
    }
}

void DecorationRenderer::inspect(std::ostream& /*output*/) const
{
}

void DecorationRenderer::renderLine(RenderLine const& line)
{
    for (auto const& mapping: CellFlagDecorationMappings)
        if (line.flags & mapping.first)
            renderDecoration(mapping.second,
                             _gridMetrics.map(CellLocation { line.lineOffset }),
                             line.usedColumns,
                             line.decorationColor);
}

void DecorationRenderer::renderCell(RenderCell const& _cell)
{
    for (auto const& mapping: CellFlagDecorationMappings)
        if (_cell.flags & mapping.first)
            renderDecoration(
                mapping.second, _gridMetrics.map(_cell.position), ColumnCount(1), _cell.decorationColor);
}

auto DecorationRenderer::createTileData(Decorator decoration, atlas::TileLocation tileLocation)
    -> TextureAtlas::TileCreateData
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
            auto const thickness_half = max(1u, unsigned(ceil(underlineThickness() / 2.0)));
            auto const thickness = thickness_half * 2;
            auto const y0 = max(0u, unsigned(underlinePosition()) - thickness_half);
            auto const height = Height(y0 + thickness);
            auto const imageSize = ImageSize { width, height };
            return create(imageSize, [&]() -> atlas::Buffer {
                auto image = atlas::Buffer(imageSize.area(), 0);
                for (unsigned y = 1; y <= thickness; ++y)
                    for (unsigned x = 0; x < unbox<unsigned>(width); ++x)
                        image[(*height - y0 - y) * *width + x] = 0xFF;
                return image;
            });
        }
        case Decorator::DoubleUnderline: {
            auto const thickness = max(1u, unsigned(ceil(double(underlineThickness()) * 2.0) / 3.0));
            auto const y1 = max(0u, unsigned(underlinePosition()) + thickness);
            auto const y0 = max(0u, y1 - 3 * thickness);
            auto const height = Height(y1 + thickness);
            auto const imageSize = ImageSize { width, height };
            return create(imageSize, [&]() -> atlas::Buffer {
                auto image = atlas::Buffer(imageSize.area(), 0);
                for (unsigned y = 1; y <= thickness; ++y)
                {
                    for (unsigned x = 0; x < unbox<unsigned>(width); ++x)
                    {
                        image[(*height - y1 - y) * *width + x] = 0xFF; // top line
                        image[(*height - y0 - y) * *width + x] = 0xFF; // bottom line
                    }
                }
                return image;
            });
        }
        case Decorator::CurlyUnderline: {
            auto const height = Height::cast_from(_gridMetrics.baseline);
            auto const h2 = max(unbox<int>(height) / 2, 1);
            auto const yScalar = h2 - 1;
            auto const xScalar = 2 * M_PI / *width;
            auto const yBase = h2;
            auto const imageSize = ImageSize { width, height };
            auto block = blockElement(imageSize);
            return create(block.downsampledSize(), [&]() -> atlas::Buffer {
                auto const thickness_half = max(1, int(ceil(underlineThickness() / 2.0)));
                for (int x = 0; x < unbox<int>(width); ++x)
                {
                    // Using Wu's antialiasing algorithm to paint the curved line.
                    // See: https://www-users.mat.umk.pl//~gruby/teaching/lgim/1_wu.pdf
                    auto const y = yScalar * cos(xScalar * x);
                    auto const y1 = static_cast<int>(floor(y));
                    auto const y2 = static_cast<int>(ceil(y));
                    auto const intensity = static_cast<uint8_t>(255 * fabs(y - y1));
                    // block.paintOver(x, yBase + y1, 255 - intensity);
                    // block.paintOver(x, yBase + y2, intensity);
                    block.paintOverThick(x, yBase + y1, uint8_t(255 - intensity), thickness_half, 0);
                    block.paintOverThick(x, yBase + y2, intensity, thickness_half, 0);
                }
                return block.take();
            });
        }
        case Decorator::DottedUnderline: {
            auto const dotHeight = (unsigned) _gridMetrics.underline.thickness;
            auto const dotWidth = dotHeight;
            auto const height = Height::cast_from((unsigned) _gridMetrics.underline.position + dotHeight);
            auto const y0 = (unsigned) _gridMetrics.underline.position - dotHeight;
            auto const x0 = 0u;
            auto const x1 = unbox<unsigned>(width) / 2;
            auto block = blockElement(ImageSize { width, height });
            return create(block.downsampledSize(), [&]() -> atlas::Buffer {
                for (unsigned y = 0; y < dotHeight; ++y)
                {
                    for (unsigned x = 0; x < dotWidth; ++x)
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
            auto const thickness_half = max(1u, unsigned(ceil(underlineThickness() / 2.0)));
            auto const thickness = max(1u, thickness_half * 2);
            auto const y0 = max(0u, unsigned(underlinePosition()) - thickness_half);
            auto const height = Height(y0 + thickness);
            auto const imageSize = ImageSize { width, height };
            return create(imageSize, [&]() -> atlas::Buffer {
                auto image = atlas::Buffer(unbox<size_t>(width) * unbox<size_t>(height), 0);
                for (unsigned y = 1; y <= thickness; ++y)
                    for (unsigned x = 0; x < unbox<unsigned>(width); ++x)
                        if (fabsf(float(x) / float(*width) - 0.5f) >= 0.25f)
                            image[(*height - y0 - y) * *width + x] = 0xFF;
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
                    for (unsigned x = gap; x < unbox<unsigned>(width) - gap; ++x)
                    {
                        image[y * *width + x] = 0xFF;
                        image[(*cellHeight - 1 - y) * *width + x] = 0xFF;
                    }

                // Draws the left and right vertical lines
                for (unsigned y = gap; y < unbox<unsigned>(cellHeight) - gap; y++)
                    for (unsigned x = gap; x < thickness + gap; ++x)
                    {
                        image[y * *width + x] = 0xFF;
                        image[y * *width + (*width - 1 - x)] = 0xFF;
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
                for (unsigned y = 0; y < thickness; ++y)
                    for (unsigned x = 0; x < unbox<unsigned>(width); ++x)
                        image[(*cellHeight - y - 1) * *width + x] = 0xFF;
                return image;
            });
        }
        case Decorator::CrossedOut: {
            auto const height = Height(*_gridMetrics.cellSize.height / 2);
            auto const thickness = (unsigned) underlineThickness();
            auto const imageSize = ImageSize { width, height };
            return create(imageSize, [&]() -> atlas::Buffer {
                auto image = atlas::Buffer(unbox<size_t>(width) * unbox<size_t>(height), 0);
                for (unsigned y = 1; y <= thickness; ++y)
                    for (unsigned x = 0; x < unbox<unsigned>(width); ++x)
                        image[(*height - y) * *width + x] = 0xFF;
                return image;
            });
        }
    }
    Require(false && "Unhandled case.");
    return {};
}

void DecorationRenderer::renderDecoration(Decorator decoration,
                                          crispy::Point pos,
                                          ColumnCount columnCount,
                                          RGBColor const& color)
{
    for (auto i = ColumnCount(0); i < columnCount; ++i)
    {
        auto const tileIndex = _directMapping.toTileIndex(static_cast<uint32_t>(decoration));
        auto const tileLocation = _textureAtlas->tileLocation(tileIndex);
        TextureAtlas::TileCreateData tileData = createTileData(decoration, tileLocation);
        AtlasTileAttributes const& tileAttributes = _textureAtlas->directMapped(tileIndex);
        renderTile({ pos.x + unbox<int>(i) * unbox<int>(_gridMetrics.cellSize.width) },
                   { pos.y },
                   color,
                   tileAttributes);
    }
}

} // namespace terminal::renderer
