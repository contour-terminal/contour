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
    /// Default ceiling on the GPU memory resident whole-image textures may occupy, in bytes.
    ///
    /// The tile atlas this replaced was a fixed-size allocation, so image memory was bounded by
    /// construction and an image beyond the cap simply got re-uploaded on demand. One texture per
    /// image is bounded by nothing but how many images are alive, and an image stays alive as long as
    /// a grid cell references it -- so a session scrolled through hundreds of sixel frames pinned one
    /// full-resolution texture per frame.
    ///
    /// 256 MB holds roughly eight full-screen 4K images (3840*2160*4 = ~33 MB each), which is far
    /// more than any real working set and still a bound.
    static constexpr size_t DefaultTextureBudgetBytes = 256u * 1024u * 1024u;

    /// @param gridMetrics the grid's metrics.
    /// @param cellSize the size of one cell, in pixels.
    /// @param textureBudgetBytes ceiling on resident image-texture memory; see
    ///        @c DefaultTextureBudgetBytes.
    ImageRenderer(GridMetrics const& gridMetrics,
                  ImageSize cellSize,
                  size_t textureBudgetBytes = DefaultTextureBudgetBytes);

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

    /// Releases least-recently-drawn image textures until the budget is met.
    ///
    /// Only images the current frame did not draw are candidates: this frame's quads are already
    /// scheduled and name their texture by id, so releasing one would drop it from the very frame it
    /// is visible in. A working set that alone exceeds the budget therefore overshoots it rather than
    /// evicting and re-uploading the same textures every frame.
    void evictToBudget();

    std::vector<atlas::RenderImageQuad> _pendingQuadsBelowText;
    std::vector<atlas::RenderImageQuad> _pendingQuadsAboveText;

    // private data
    //
    ImageSize _cellSize;

    /// One image's texture, and what keeping it costs.
    struct ImageTextureEntry
    {
        atlas::ImageTextureId id;
        size_t bytes = 0;           ///< GPU bytes the texture occupies.
        uint64_t lastUsedFrame = 0; ///< The frame this was last drawn in; the LRU key.
    };

    /// vtbackend ImageId -> the texture holding it. Keyed on the image rather than on the
    /// rasterization: policies and cell size change where an image is sampled, not what it contains.
    std::unordered_map<uint32_t, ImageTextureEntry> _imageTextures;
    uint32_t _nextImageTextureId = 1;

    size_t const _textureBudgetBytes;
    size_t _residentBytes = 0;  ///< Sum of every resident entry's bytes.
    uint64_t _frameCounter = 0; ///< Advanced by beginFrame(); stamps entries as they are drawn.
};

} // namespace vtrasterizer
