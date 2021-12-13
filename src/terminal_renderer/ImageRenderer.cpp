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
#include <terminal_renderer/ImageRenderer.h>

#include <crispy/times.h>
#include <crispy/algorithm.h>

#include <array>

using crispy::times;

using std::array;
using std::array;
using std::nullopt;
using std::optional;

namespace terminal::renderer {

ImageRenderer::ImageRenderer(ImageSize _cellSize) :
    imagePool_{},
    cellSize_{ _cellSize }
{
}

void ImageRenderer::setRenderTarget(RenderTarget& _renderTarget)
{
    Renderable::setRenderTarget(_renderTarget);
    clearCache();
}

void ImageRenderer::setCellSize(ImageSize _cellSize)
{
    cellSize_ = _cellSize;
    // TODO: recompute slices here?
}

void ImageRenderer::renderImage(crispy::Point _pos, ImageFragment const& _fragment)
{
    if (optional<DataRef> const dataRef = getTextureInfo(_fragment); dataRef.has_value())
    {
        //std::cout << fmt::format("ImageRenderer.renderImage: {}\n", _fragment);

        auto const color = array{1.0f, 0.0f, 0.0f, 1.0f}; // not used
        atlas::TextureInfo const& textureInfo = std::get<0>(*dataRef).get();

        // TODO: actually make x/y/z all signed (for future work, i.e. smooth scrolling!)
        auto const x = _pos.x;
        auto const y = _pos.y;
        auto const z = 0;
        textureScheduler().renderTexture({textureInfo, x, y, z, color});
    }
}

optional<ImageRenderer::DataRef> ImageRenderer::getTextureInfo(ImageFragment const& _fragment)
{
    auto const key = ImageFragmentKey{
        _fragment.rasterizedImage().image().id(),
        _fragment.offset(),
        _fragment.rasterizedImage().cellSize()
    };

    if (optional<DataRef> const info = atlas_->get(key); info.has_value())
        return info;

    auto metadata = Metadata{}; // TODO: do we want/need to fill this?

    auto constexpr colored = true;

    // FIXME: remember if insertion failed already, don't repeat then? or how to deal with GPU atlas/GPU exhaustion?

    auto handle = atlas_->insert(key,
                                 _fragment.rasterizedImage().cellSize(),
                                 cellSize_,
                                 _fragment.data(),
                                 colored,
                                 metadata);

    // remember image fragment key so we can later on release the GPU memory when not needed anymore.
    if (handle)
        imageFragmentsInUse_[_fragment.rasterizedImage().image().id()].emplace_back(key);

    return handle;
}

void ImageRenderer::discardImage(ImageId _imageId)
{
    auto const fragmentsIterator = imageFragmentsInUse_.find(_imageId);
    if (fragmentsIterator != end(imageFragmentsInUse_))
    {
        auto const& fragments = fragmentsIterator->second;
        for (ImageFragmentKey const& key : fragments)
            atlas_->release(key);

        imageFragmentsInUse_.erase(fragmentsIterator);
    }
}

void ImageRenderer::clearCache()
{
    imageFragmentsInUse_.clear();
    atlas_ = std::make_unique<TextureAtlas>(renderTarget().coloredAtlasAllocator());
}

} // end namespace
