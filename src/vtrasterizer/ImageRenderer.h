// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <vtbackend/Image.h>

#include <vtrasterizer/RenderTarget.h>
#include <vtrasterizer/TextRenderer.h>

#include <crispy/FNV.h>
#include <crispy/point.h>
#include <crispy/size.h>

#include <unordered_map>
#include <vector>

namespace vtrasterizer
{

/// Image Rendering API.
///
/// Can render any arbitrary RGBA image (for example Sixel Graphics images).
///
/// An image is held as one whole texture and sampled per cell, rather than sliced into one
/// fixed-size atlas tile per cell. The atlas is a glyph cache: a few hundred distinct tiles, each
/// reused constantly. An image inverts every one of those assumptions -- each tile is unique, used
/// once, and a full-screen image needs more of them than the atlas can hold, so it evicted its own
/// tiles while drawing itself and most cells ended up sampling another cell's pixels.
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
    /// The texture holding @p rasterizedImage's pixels, creating and uploading it on first sight.
    atlas::ImageTextureId textureFor(vtbackend::RasterizedImage const& rasterizedImage);

    /// Hands @p quads to the backend and empties them.
    void flushQuads(std::vector<atlas::RenderImageQuad>& quads);

    std::vector<atlas::RenderImageQuad> _pendingQuadsBelowText;
    std::vector<atlas::RenderImageQuad> _pendingQuadsAboveText;

    // private data
    //
    ImageSize _cellSize;

    /// vtbackend ImageId -> the texture holding it. Keyed on the image rather than on the
    /// rasterization: policies and cell size change where an image is sampled, not what it contains.
    std::unordered_map<uint32_t, atlas::ImageTextureId> _imageTextureIds;
    uint32_t _nextImageTextureId = 1;
};

} // namespace vtrasterizer
