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

#include <terminal/Image.h>
#include <terminal/Size.h>
#include <QtCore/QPoint>

#include <vector>

namespace terminal::renderer {

/// Image Rendering API.
///
/// Can render any arbitrary RGBA image (for example Sixel Graphics images).
class ImageRenderer
{
  public:
    ImageRenderer(
         atlas::CommandListener& _commandListener,
         atlas::TextureAtlasAllocator& _colorAtlasAllocator,
         Size const& _cellSize
    );

    /// Reconfigures the slicing properties of existing images.
    void setCellSize(Size const& _cellSize);

    void renderImage(QPoint _pos, ImageFragment const& _fragment);

    /// notify underlying cache that this fragment is not going to be rendered anymore, maybe freeing up some GPU caches.
    void discardImage(Image::Id _imageId);

    struct ImageFragmentKey {
        Image::Id const imageId;
        Coordinate const offset;
        Size const size;

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

    struct Metadata {
        // TODO: do we want/need anything here?
    };

    using TextureAtlas = atlas::MetadataTextureAtlas<ImageFragmentKey, Metadata>;
    using DataRef = TextureAtlas::DataRef;

    void clearCache();

  private:
    std::optional<DataRef> getTextureInfo(ImageFragment const& _fragment);

  private:
    ImagePool imagePool_;
    std::map<Image::Id, std::vector<ImageFragmentKey>> imageFragmentsInUse_; // remember each fragment key per image for proper GPU texture GC.
    Size cellSize_;
    atlas::CommandListener& commandListener_;
    TextureAtlas atlas_;
};

}
