/**
 * This file is part of the "libterminal" project
 *   Copyright (c) 2019-2020 Christian Parpart <christian@parpart.family>
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
#include <vtbackend/Color.h>

#include <vtrasterizer/CursorRenderer.h>

#include <crispy/utils.h>

#include <stdexcept>
#include <vector>

#include "vtbackend/primitives.h"

using crispy::each_element;

using std::array;
using std::get;
using std::max;
using std::move;
using std::nullopt;
using std::optional;
using std::runtime_error;
using std::string;

namespace terminal::rasterizer
{

namespace
{
    // Times 3 because double-width cursor shapes need 2 tiles,
    // plus 1 for narrow-width cursor shapes.
    constexpr uint32_t DirectMappedTilesCount =
        static_cast<uint32_t>(std::numeric_limits<cursor_shape>::count()) * 3;

    constexpr uint32_t toDirectMappingIndex(cursor_shape shape, int width, uint32_t sliceIndex) noexcept
    {
        return static_cast<uint32_t>(shape) + sliceIndex
               + uint32_t(width - 1)
                     * (static_cast<uint32_t>(std::numeric_limits<cursor_shape>::count())
                        + static_cast<uint32_t>(shape));
    }
} // namespace

CursorRenderer::CursorRenderer(GridMetrics const& gridMetrics, cursor_shape shape):
    Renderable { gridMetrics }, _shape { shape }
{
}

void CursorRenderer::setRenderTarget(RenderTarget& renderTarget,
                                     DirectMappingAllocator& directMappingAllocator)
{
    Renderable::setRenderTarget(renderTarget, directMappingAllocator);
    _directMapping = directMappingAllocator.allocate(DirectMappedTilesCount);
}

void CursorRenderer::setTextureAtlas(TextureAtlas& atlas)
{
    Renderable::setTextureAtlas(atlas);
    initializeDirectMapping();
}

void CursorRenderer::setShape(cursor_shape shape)
{
    _shape = shape;
}

void CursorRenderer::clearCache()
{
}

void CursorRenderer::initializeDirectMapping()
{
    Require(_textureAtlas);

    for (int width = 1; width <= 2; ++width)
    {
        for (cursor_shape const shape: each_element<cursor_shape>())
        {
            auto const directMappingIndex = toDirectMappingIndex(shape, width, 0);
            auto const tileIndex = _directMapping.toTileIndex(directMappingIndex);
            auto const tileLocation = _textureAtlas->tileLocation(tileIndex);
            TextureAtlas::TileCreateData tileData = createTileData(shape, width, tileLocation);
            uint32_t const offsetX = 0;
            auto const tileWidth = _gridMetrics.cellSize.width;
            for (TileSliceIndex const slice: atlas::sliced(tileWidth, offsetX, tileData.bitmapSize))
            {
                auto const directMappingIndex = toDirectMappingIndex(shape, width, slice.sliceIndex);
                auto const tileIndex = _directMapping.toTileIndex(directMappingIndex);
                auto const tileLocation = _textureAtlas->tileLocation(tileIndex);
                _textureAtlas->setDirectMapping(tileIndex, sliceTileData(tileData, slice, tileLocation));
            }
        }
    }
}

auto CursorRenderer::createTileData(cursor_shape cursorShape,
                                    int columnWidth,
                                    atlas::TileLocation tileLocation) -> TextureAtlas::TileCreateData
{
    auto const width = width::cast_from(unbox<int>(_gridMetrics.cellSize.width) * columnWidth);
    auto const height = _gridMetrics.cellSize.height;
    auto const defaultBitmapSize = image_size { width, height };
    auto const baseline = _gridMetrics.baseline;
    auto constexpr LineThickness = 1;

    auto const create = [this, tileLocation](image_size bitmapSize,
                                             auto createBitmap) -> TextureAtlas::TileCreateData {
        return createTileData(tileLocation,
                              createBitmap(),
                              atlas::Format::Red,
                              bitmapSize,
                              RenderTileAttributes::X { 0 },
                              RenderTileAttributes::Y { 0 },
                              FRAGMENT_SELECTOR_GLYPH_ALPHA);
    };

    switch (cursorShape)
    {
        case cursor_shape::Block:
            return create(defaultBitmapSize, [&]() {
                return atlas::Buffer(unbox<size_t>(width) * unbox<size_t>(height), 0xFFu);
            });
        case cursor_shape::Underscore:
            return create(defaultBitmapSize, [&]() {
                auto const height = height::cast_from(baseline);
                auto const thickness = (unsigned) max(LineThickness * baseline / 3, 1);
                auto const base_y = max((*height - thickness) / 2, 0u);
                auto image = atlas::Buffer(defaultBitmapSize.area(), 0);

                assert(thickness <= static_cast<size_t>(baseline));
                for (auto y = size_t(0); y <= static_cast<size_t>(thickness); ++y)
                    for (size_t x = 0; x < *width; ++x)
                        image[(defaultBitmapSize.height.as<size_t>() - 1 - base_y - unsigned(y))
                                  * unbox<size_t>(width)
                              + x] = 0xFF;
                return image;
            });
        case cursor_shape::Bar:
            return create(defaultBitmapSize, [&]() {
                auto const thickness = (size_t) max(LineThickness * baseline / 3, 1);
                // auto const base_y = max((height - thickness) / 2, 0);
                auto image = atlas::Buffer(unbox<size_t>(width) * unbox<size_t>(height), 0);

                for (size_t x = 0; x < thickness; ++x)
                    for (size_t y = 0; y < unbox<size_t>(height); ++y)
                        image[y * *width + x] = 0xFF;
                return image;
            });
        case cursor_shape::Rectangle:
            return create(defaultBitmapSize, [&]() {
                auto const height = _gridMetrics.cellSize.height;
                auto image = atlas::Buffer(unbox<size_t>(width) * unbox<size_t>(height), 0xFFu);
                auto const thickness = max(unbox<size_t>(width) / 12, size_t { 1 });

                auto const innerWidth = unbox<size_t>(width) - 2 * thickness;
                auto const innerHeight = unbox<size_t>(height) - 2 * thickness;

                for (size_t y = thickness; y <= innerHeight; ++y)
                    for (size_t x = thickness; x <= innerWidth; ++x)
                        image[y * *width + x] = 0;

                return image;
            });
    }
    Require(false && "Unhandled case.");
    return {};
}

void CursorRenderer::render(crispy::Point pos, int columnWidth, rgb_color color)
{
    for (uint32_t i = 0; i < uint32_t(columnWidth); ++i)
    {
        auto const directMappingIndex = toDirectMappingIndex(_shape, columnWidth, i);
        auto const tileIndex = _directMapping.toTileIndex(directMappingIndex);
        auto const x = pos.x + int(i) * unbox<int>(_gridMetrics.cellSize.width);
        AtlasTileAttributes const& tileAttributes = _textureAtlas->directMapped(tileIndex);
        renderTile({ int(x) }, { pos.y }, color, tileAttributes);
    }
}

void CursorRenderer::inspect(std::ostream& /*output*/) const
{
}

} // namespace terminal::rasterizer
