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

#include <terminal_renderer/Atlas.h>
#include <terminal_renderer/RenderTarget.h>

#include <terminal/Image.h>
#include <crispy/FNV.h>
#include <crispy/point.h>
#include <crispy/size.h>

#include <memory>
#include <vector>
#include <unordered_map>

namespace terminal::renderer
{
    struct ImageFragmentKey
    {
        ImageId const imageId;
        Coordinate const offset;
        ImageSize const size;

        bool operator==(ImageFragmentKey const& b) const noexcept
        {
            return imageId == b.imageId
                && offset == b.offset
                && size == b.size;
        }

        bool operator!=(ImageFragmentKey const& b) const noexcept
        {
            return !(*this == b);
        }

        bool operator<(ImageFragmentKey const& b) const noexcept
        {
            return (imageId < b.imageId)
                || (imageId == b.imageId && offset < b.offset);
        }
    };
}

namespace std
{
    template<>
    struct hash<terminal::renderer::ImageFragmentKey>
    {
        constexpr size_t operator()(terminal::renderer::ImageFragmentKey const& _key) const noexcept
        {
            using FNV = crispy::FNV<uint64_t>;
            return FNV{}(FNV{}.basis(),
                         _key.imageId.value,
                         _key.offset.line.as<unsigned>(),
                         _key.offset.column.as<unsigned>(),
                         _key.size.width.as<unsigned>(),
                         _key.size.height.as<unsigned>());
        }
    };
}

namespace terminal::renderer {

/// Image Rendering API.
///
/// Can render any arbitrary RGBA image (for example Sixel Graphics images).
class ImageRenderer : public Renderable
{
  public:
    explicit ImageRenderer(ImageSize _cellSize);

    void setRenderTarget(RenderTarget& _renderTarget) override;
    void clearCache() override;

    /// Reconfigures the slicing properties of existing images.
    void setCellSize(ImageSize _cellSize);

    void renderImage(crispy::Point _pos, ImageFragment const& _fragment);

    /// notify underlying cache that this fragment is not going to be rendered anymore, maybe freeing up some GPU caches.
    void discardImage(ImageId _imageId);

    struct Metadata {}; // TODO: do we want/need anything here?
    using TextureAtlas = atlas::MetadataTextureAtlas<ImageFragmentKey, Metadata>;
    using DataRef = TextureAtlas::DataRef;

  private:
    std::optional<DataRef> getTextureInfo(ImageFragment const& _fragment);

    // private data
    //
    ImagePool imagePool_;
    std::unordered_map<ImageId, std::vector<ImageFragmentKey>> imageFragmentsInUse_; // remember each fragment key per image for proper GPU texture GC.
    ImageSize cellSize_;
    std::unique_ptr<TextureAtlas> atlas_;
};

}
