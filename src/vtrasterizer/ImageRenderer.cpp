// SPDX-License-Identifier: Apache-2.0
#include <vtrasterizer/ImageRenderer.h>

#include <crispy/StrongHash.h>
#include <crispy/algorithm.h>
#include <crispy/times.h>

#include <array>

using crispy::times;

using std::array;
using std::nullopt;
using std::optional;

namespace vtrasterizer
{

ImageRenderer::ImageRenderer(GridMetrics const& gridMetrics, ImageSize cellSize):
    Renderable { gridMetrics }, _cellSize { cellSize }
{
}

void ImageRenderer::setRenderTarget(RenderTarget& renderTarget,
                                    DirectMappingAllocator& directMappingAllocator)
{
    Renderable::setRenderTarget(renderTarget, directMappingAllocator);
    clearCache();
}

void ImageRenderer::setCellSize(ImageSize cellSize)
{
    _cellSize = cellSize;
    // TODO: recompute rasterized images slices here?
}

void ImageRenderer::renderImage(crispy::point pos, vtbackend::ImageFragment const& fragment)
{
    // std::cout << fmt::format("ImageRenderer.renderImage: {}\n", fragment);

    AtlasTileAttributes const* tileAttributes = getOrCreateCachedTileAttributes(fragment);
    if (!tileAttributes)
        return;

    // clang-format off
    _pendingRenderTilesAboveText.emplace_back(createRenderTile(atlas::RenderTile::X { pos.x },
                                                               atlas::RenderTile::Y { pos.y },
                                                               vtbackend::RGBAColor::White, *tileAttributes));
    // clang-format on
}

void ImageRenderer::onBeforeRenderingText()
{
    // We could render here the images that should go below text.
}

void ImageRenderer::onAfterRenderingText()
{
    // We render here the images that should go above text.

    for (auto const& tile: _pendingRenderTilesAboveText)
        textureScheduler().renderTile(tile);

    _pendingRenderTilesAboveText.clear();
}

void ImageRenderer::beginFrame()
{
    assert(_pendingRenderTilesAboveText.empty());
}

void ImageRenderer::endFrame()
{
    if (!_pendingRenderTilesAboveText.empty())
    {
        // In case some image tiles are still pending but no text had to be rendered.

        for (auto& tile: _pendingRenderTilesAboveText)
            textureScheduler().renderTile(tile);
        _pendingRenderTilesAboveText.clear();
    }
}

Renderable::AtlasTileAttributes const* ImageRenderer::getOrCreateCachedTileAttributes(
    vtbackend::ImageFragment const& fragment)
{
    // using crispy::StrongHash;
    // auto const hash = StrongHash::compute(fragment.rasterizedImage().image().id().value)
    //                   * fragment.offset().column.value * fragment.offset().line.value
    //                   * fragment.rasterizedImage().cellSize().width.value
    //                   * fragment.rasterizedImage().cellSize().height.value;
    auto const key = ImageFragmentKey { fragment.rasterizedImage().image().id(),
                                        fragment.offset(),
                                        fragment.rasterizedImage().cellSize() };
    auto const hash = crispy::strong_hash::compute(key);

    return textureAtlas().get_or_try_emplace(
        hash, [&](atlas::TileLocation tileLocation) -> optional<TextureAtlas::TileCreateData> {
            return createTileData(tileLocation,
                                  fragment.data(),
                                  atlas::Format::RGBA,
                                  fragment.rasterizedImage().cellSize(),
                                  _cellSize,
                                  RenderTileAttributes::X { 0 },
                                  RenderTileAttributes::Y { 0 },
                                  FRAGMENT_SELECTOR_IMAGE_BGRA);
        });
}

void ImageRenderer::discardImage(vtbackend::ImageId /*imageId*/)
{
    // We currently don't really discard.
    // Because the GPU texture atlas is resource-guarded by an LRU hashtable.
}

void ImageRenderer::clearCache()
{
    // We currently don't really clean up anything.
    // Because the GPU texture atlas is resource-guarded by an LRU hashtable.
}

void ImageRenderer::inspect(std::ostream& /*output*/) const
{
}

} // namespace vtrasterizer
