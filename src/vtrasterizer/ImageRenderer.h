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
#pragma once

#include <terminal/Image.h>

#include <vtrasterizer/RenderTarget.h>
#include <vtrasterizer/TextRenderer.h>

#include <crispy/FNV.h>
#include <crispy/point.h>
#include <crispy/size.h>

#include <memory>
#include <unordered_map>
#include <vector>

namespace terminal::rasterizer
{

// NB: Ensure this struct does NOT contain padding (or adapt strong hash creation).
struct ImageFragmentKey
{
    ImageId const imageId;
    CellLocation const offset;
    ImageSize const size;

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
    void setCellSize(ImageSize _cellSize);

    void renderImage(crispy::Point _pos, ImageFragment const& fragment);

    /// notify underlying cache that this fragment is not going to be rendered anymore, maybe freeing up some
    /// GPU caches.
    void discardImage(ImageId _imageId);

    void inspect(std::ostream& output) const override;

    void beginFrame();
    void endFrame();

    void onBeforeRenderingText() override;
    void onAfterRenderingText() override;

  private:
    AtlasTileAttributes const* getOrCreateCachedTileAttributes(ImageFragment const& fragment);
    std::vector<atlas::RenderTile> pendingRenderTilesAboveText_;

    // private data
    //
    ImageSize cellSize_;
};

} // namespace terminal::rasterizer
