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
#include <terminal/Color.h>

#include <terminal_renderer/CursorRenderer.h>

#include <crispy/utils.h>

#include <stdexcept>
#include <vector>

using crispy::each_element;

using std::array;
using std::get;
using std::max;
using std::move;
using std::nullopt;
using std::optional;
using std::runtime_error;
using std::string;

namespace terminal::renderer
{

namespace
{
    // Times 3 because double-width cursor shapes need 2 tiles,
    // plus 1 for narrow-width cursor shapes.
    constexpr uint32_t DirectMappedTilesCount =
        static_cast<uint32_t>(std::numeric_limits<CursorShape>::count()) * 3;

    constexpr uint32_t toDirectMappingIndex(CursorShape shape, int width, uint32_t sliceIndex) noexcept
    {
        return static_cast<unsigned>(shape) + sliceIndex
               + (width - 1)
                     * (static_cast<uint32_t>(std::numeric_limits<CursorShape>::count())
                        + static_cast<uint32_t>(shape));
    }
} // namespace

CursorRenderer::CursorRenderer(GridMetrics const& gridMetrics, CursorShape shape):
    Renderable { gridMetrics }, shape_ { shape }
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

void CursorRenderer::setShape(CursorShape _shape)
{
    shape_ = _shape;
}

void CursorRenderer::clearCache()
{
}

void CursorRenderer::initializeDirectMapping()
{
    Require(_textureAtlas);

    for (int width = 1; width <= 2; ++width)
    {
        for (CursorShape const shape: each_element<CursorShape>())
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

auto CursorRenderer::createTileData(CursorShape cursorShape,
                                    int columnWidth,
                                    atlas::TileLocation tileLocation) -> TextureAtlas::TileCreateData
{
    auto const width = Width(*_gridMetrics.cellSize.width * columnWidth);
    auto const height = _gridMetrics.cellSize.height;
    auto const defaultBitmapSize = ImageSize { width, height };
    auto const baseline = _gridMetrics.baseline;
    auto constexpr LineThickness = 1;

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

    switch (cursorShape)
    {
    case CursorShape::Block:
        return create(defaultBitmapSize,
                      [&]() { return atlas::Buffer(unbox<size_t>(width) * unbox<size_t>(height), 0xFFu); });
    case CursorShape::Underscore:
        return create(ImageSize { width, Height(baseline) }, [&]() {
            auto const height = Height(baseline);
            auto const thickness = max(LineThickness * baseline / 3, 1);
            auto const base_y = max((*height - thickness) / 2, 0u);
            auto image = atlas::Buffer(unbox<size_t>(width) * unbox<size_t>(height), 0);

            for (int y = 1; y <= thickness; ++y)
                for (int x = 0; x < width.as<int>(); ++x)
                    image[(base_y + y) * width.as<int>() + x] = 0xFF;
            return image;
        });
    case CursorShape::Bar:
        return create(defaultBitmapSize, [&]() {
            auto const thickness = max(LineThickness * baseline / 3, 1);
            // auto const base_y = max((height - thickness) / 2, 0);
            auto image = atlas::Buffer(unbox<size_t>(width) * unbox<size_t>(height), 0);

            for (int x = 0; x < thickness; ++x)
                for (int y = 0; y < unbox<int>(height); ++y)
                    image[y * *width + x] = 0xFF;
            return image;
        });
    case CursorShape::Rectangle:
        return create(defaultBitmapSize, [&]() {
            auto const height = _gridMetrics.cellSize.height;
            auto image = atlas::Buffer(unbox<size_t>(width) * unbox<size_t>(height), 0xFFu);
            auto const thickness = max(unbox<int>(width) / 12, 1);

            auto const innerWidth = unbox<int>(width) - 2 * thickness;
            auto const innerHeight = unbox<int>(height) - 2 * thickness;

            for (int y = thickness; y <= innerHeight; ++y)
                for (int x = thickness; x <= innerWidth; ++x)
                    image[y * *width + x] = 0;

            return image;
        });
    }
    Require(false && "Unhandled case.");
    return {};
}

void CursorRenderer::render(crispy::Point _pos, int _columnWidth, RGBColor _color)
{
    for (int i = 0; i < _columnWidth; ++i)
    {
        auto const directMappingIndex = toDirectMappingIndex(shape_, _columnWidth, i);
        auto const tileIndex = _directMapping.toTileIndex(directMappingIndex);
        auto const x = _pos.x + i * unbox<int>(_gridMetrics.cellSize.width);
        AtlasTileAttributes const& tileAttributes = _textureAtlas->directMapped(tileIndex);
        renderTile({ x }, { _pos.y }, _color, tileAttributes);
    }
}

void CursorRenderer::inspect(std::ostream& /*output*/) const
{
}

} // namespace terminal::renderer
