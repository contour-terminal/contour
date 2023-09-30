// SPDX-License-Identifier: Apache-2.0
#include <vtrasterizer/RenderTarget.h>

using namespace crispy;
using namespace std;
using namespace vtrasterizer;
using namespace vtbackend;

Renderable::Renderable(GridMetrics const& gridMetrics): _gridMetrics { gridMetrics }
{
}

void Renderable::setRenderTarget(RenderTarget& renderTarget, DirectMappingAllocator& directMappingAllocator)
{
    _renderTarget = &renderTarget;
    _textureScheduler = &_renderTarget->textureScheduler();
    _directMappingAllocator = &directMappingAllocator;
}

auto Renderable::createTileData(atlas::TileLocation tileLocation,
                                std::vector<uint8_t> bitmap,
                                atlas::Format bitmapFormat,
                                ImageSize bitmapSize,
                                ImageSize renderBitmapSize,
                                RenderTileAttributes::X x,
                                RenderTileAttributes::Y y,
                                uint32_t fragmentShaderSelector) -> TextureAtlas::TileCreateData
{
    // clang-format off
    auto tileData = TextureAtlas::TileCreateData {};
    auto const atlasSize = _textureScheduler->atlasSize();
    Require(!!atlasSize.width);
    Require(!!atlasSize.height);
    Require(bitmap.size() == bitmapSize.area() * atlas::element_count(bitmapFormat));
    tileData.bitmap = std::move(bitmap);
    tileData.bitmapSize = bitmapSize;
    tileData.bitmapFormat = bitmapFormat;
    tileData.metadata.x = x;
    tileData.metadata.y = y;
    tileData.metadata.fragmentShaderSelector = fragmentShaderSelector;
    tileData.metadata.normalizedLocation.x = static_cast<float>(tileLocation.x.value) / unbox<float>(atlasSize.width);
    tileData.metadata.normalizedLocation.y = static_cast<float>(tileLocation.y.value) / unbox<float>(atlasSize.height);
    tileData.metadata.normalizedLocation.width = unbox<float>(tileData.bitmapSize.width) / unbox<float>(atlasSize.width);
    tileData.metadata.normalizedLocation.height = unbox<float>(tileData.bitmapSize.height) / unbox<float>(atlasSize.height);
    tileData.metadata.targetSize = renderBitmapSize;
    return tileData;
    // clang-format on
}

auto Renderable::sliceTileData(Renderable::TextureAtlas::TileCreateData const& createData,
                               TileSliceIndex sliceIndex,
                               atlas::TileLocation tileLocation) -> Renderable::TextureAtlas::TileCreateData
{
    auto const bitmapFormat = createData.bitmapFormat;
    auto const colorComponentCount = element_count(bitmapFormat);
    auto const pitch = unbox<uintptr_t>(createData.bitmapSize.width) * colorComponentCount;

    auto const subWidth = Width(sliceIndex.endX - sliceIndex.beginX);
    auto const subSize = ImageSize { subWidth, createData.bitmapSize.height };
    auto const subPitch = unbox<uintptr_t>(subWidth) * colorComponentCount;
    auto bitmap = vector<uint8_t>();
    bitmap.resize(subSize.area() * colorComponentCount);
    for (uintptr_t rowIndex = 0; rowIndex < unbox<uintptr_t>(subSize.height); ++rowIndex)
    {
        uint8_t* targetRow = bitmap.data() + rowIndex * subPitch;
        uint8_t const* sourceRow =
            createData.bitmap.data() + rowIndex * pitch + uintptr_t(sliceIndex.beginX) * colorComponentCount;
        Require(sourceRow + subPitch <= createData.bitmap.data() + createData.bitmap.size());
        std::copy_n(sourceRow, subPitch, targetRow);
    }

    return createTileData(tileLocation,
                          std::move(bitmap),
                          bitmapFormat,
                          subSize,
                          RenderTileAttributes::X { (int) sliceIndex.beginX },
                          RenderTileAttributes::Y { createData.metadata.y },
                          createData.metadata.fragmentShaderSelector);
}

atlas::RenderTile Renderable::createRenderTile(atlas::RenderTile::X x,
                                               atlas::RenderTile::Y y,
                                               RGBAColor color,
                                               Renderable::AtlasTileAttributes const& attributes)
{
    auto renderTile = atlas::RenderTile {};
    renderTile.x = atlas::RenderTile::X { x };
    renderTile.y = atlas::RenderTile::Y { y };
    renderTile.bitmapSize = attributes.bitmapSize;
    renderTile.fragmentShaderSelector = attributes.metadata.fragmentShaderSelector;
    renderTile.color = atlas::normalize(color);
    renderTile.normalizedLocation = attributes.metadata.normalizedLocation;
    renderTile.targetSize = attributes.metadata.targetSize;
    renderTile.tileLocation = attributes.location;
    return renderTile;
}

void Renderable::renderTile(atlas::RenderTile::X x,
                            atlas::RenderTile::Y y,
                            RGBAColor color,
                            Renderable::AtlasTileAttributes const& attributes)
{
    textureScheduler().renderTile(createRenderTile(x, y, color, attributes));
}
