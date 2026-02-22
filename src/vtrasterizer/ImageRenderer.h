// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <vtbackend/Image.h>

#include <vtrasterizer/RenderTarget.h>
#include <vtrasterizer/TextRenderer.h>

#include <crispy/FNV.h>
#include <crispy/point.h>
#include <crispy/size.h>

#include <vector>

namespace vtrasterizer
{

// NB: Ensure this struct does NOT contain padding (or adapt strong hash creation).
struct ImageFragmentKey
{
    vtbackend::ImageId imageId;
    vtbackend::CellLocation offset;
    ImageSize size;

    bool operator==(ImageFragmentKey const& b) const noexcept
    {
        return imageId == b.imageId && offset == b.offset && size == b.size;
    }

    bool operator!=(ImageFragmentKey const& b) const noexcept { return !(*this == b); }

    bool operator<(ImageFragmentKey const& b) const noexcept
    {
        return (imageId < b.imageId) || (imageId == b.imageId && offset < b.offset);
    }
};

/// Image Rendering API.
///
/// Can render any arbitrary RGBA image (for example Sixel Graphics images).
class ImageRenderer: public Renderable, public TextRendererEvents
{
  public:
    ImageRenderer(GridMetrics const& gridMetrics, ImageSize cellSize);

    void setRenderTarget(RenderTarget& renderTarget, DirectMappingAllocator& directMappingAllocator) override;
    void clearCache() override;

    /// Reconfigures the slicing properties of existing images.
    void setCellSize(ImageSize cellSize);

    void renderImage(crispy::point pos, vtbackend::ImageFragment const& fragment);

    /// notify underlying cache that this fragment is not going to be rendered anymore, maybe freeing up some
    /// GPU caches.
    void discardImage(vtbackend::ImageId imageId);

    void inspect(std::ostream& output) const override;

    void beginFrame();
    void endFrame();

    void onBeforeRenderingText() override;
    void onAfterRenderingText() override;

  private:
    AtlasTileAttributes const* getOrCreateCachedTileAttributes(vtbackend::ImageFragment const& fragment);
    std::vector<atlas::RenderTile> _pendingRenderTilesBelowText;
    std::vector<atlas::RenderTile> _pendingRenderTilesAboveText;

    // private data
    //
    ImageSize _cellSize;
};

} // namespace vtrasterizer
