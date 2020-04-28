/**
 * This file is part of the "contour" project.
 *   Copyright (c) 2020 Christian Parpart <christian@parpart.family>
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

#include <cassert>
#include <map>
#include <optional>
#include <variant>
#include <vector>

namespace atlas {

using Buffer = std::vector<uint8_t>;

struct CreateAtlas {
    unsigned atlas;
    unsigned width;
    unsigned height;
    unsigned depth;
};

struct DestroyAtlas {
    // ID of the atlas to release the resources on the GPU for.
    unsigned atlas;
};

struct UploadTexture {
    unsigned atlas;                 // for example GL_TEXTURE0
    unsigned x;
    unsigned y;
    unsigned z;
    unsigned width;
    unsigned height;
    Buffer const& data;
};

struct RenderTexture {
    unsigned atlas;
    unsigned x;
    unsigned y;
    unsigned z;
    unsigned width;
    unsigned height;
};

using Command = std::variant<
    CreateAtlas,
    UploadTexture,
    RenderTexture,
    DestroyAtlas
>;

using CommandList = std::vector<Command>;

/**
 * Texture Atlas API.
 *
 * This Texture atlas stores textures with given dimension in a 3 dimensional array of atlases.
 * Thus, you may say a 4D atlas ;-)
 *
 */
template <typename Key>
class TextureAtlas {
  public:
    TextureAtlas(unsigned _instanceLimit,
                 unsigned _atlasDepth,
                 unsigned _atlasWidth,
                 unsigned _atlasHeight)
      : instanceLimit_{ _instanceLimit },
        atlasDepth_{ _atlasDepth },
        atlasWidth_{ _atlasWidth },
        atlasHeight_{ _atlasHeight }
    {
    }

    [[nodiscard]] std::vector<DestroyAtlas> clear()
    {
        std::vector<DestroyAtlas> cleanups;

        for (unsigned i = 0; i < currentAtlasInstance_; ++i)
            cleanups.push_back(DestroyAtlas{i});

        *this = TextureAtlas(instanceLimit_, atlasDepth_, atlasWidth_, atlasHeight_);

        return cleanups;
    }

    /// Tests whether given sub-texture is being present in this texture atlas.
    bool contains(Key const& _id) const
    {
        return allocations_.find(_id) != allocations_.end();
    }

    [[nodiscard]] RenderTexture const* get(Key const& _id) const
    {
        if (auto i = allocations_.find(_id); i != allocations_.end())
            return &i->second;
        else
            return nullptr;
    }

    [[nodiscard]] std::optional<UploadTexture> insert(Key const& _id, unsigned _width, unsigned _height, Buffer const& _data)
    {
        assert(_height <= atlasHeight_);
        assert(_width <= atlasWidth_);

        // ensure we have enough width space in current row
        if (currentWidth_ + _width >= atlasWidth_ && !allocateFreeRow())
            return std::nullopt;

        RenderTexture const& info = allocations_[_id] = RenderTexture{
            currentAtlasInstance_,
            currentWidth_,
            currentHeight_,
            currentDepth_,
            _width,
            _height,
        };

        if (_height > maxTextureHeightInCurrentRow_)
            maxTextureHeightInCurrentRow_ = _height;

        // increment to next free slot
        currentWidth_ += _width;
        if (currentWidth_ >= atlasWidth_ && !allocateFreeRow())
            return std::nullopt;

        return UploadTexture{
            info.atlas,
            info.x,
            info.y,
            info.z,
            info.width,
            info.height,
            _data
        };
    }

  private:
    bool allocateFreeRow()
    {
        currentWidth_ = 0;
        currentHeight_ += maxTextureHeightInCurrentRow_;
        maxTextureHeightInCurrentRow_ = 0;

        if (currentHeight_ >= atlasHeight_) // current depth-level full? -> go one level deeper
        {
            currentHeight_ = 0;
            currentDepth_++;

            if (currentDepth_ >= atlasDepth_) // whole 3D atlas full? -> use next atlas.
            {
                currentDepth_ = 0;
                currentAtlasInstance_++;

                if (currentAtlasInstance_ > instanceLimit_)
                    return false;
            }
        }
        return true;
    }

  private:
    unsigned instanceLimit_;            // maximum number of atlas instances (e.g. maximum number of OpenGL 3D textures)
    unsigned atlasDepth_;               // atlas depth
    unsigned atlasWidth_;
    unsigned atlasHeight_;

    unsigned currentAtlasInstance_ = 0; // (OpenGL) texture count already in use
    unsigned currentDepth_ = 0;         // index to current atlas that is being filled.
    unsigned currentWidth_ = 0;
    unsigned currentHeight_ = 0;
    unsigned maxTextureHeightInCurrentRow_ = 0;

    std::map<Key, RenderTexture> allocations_ = {};
};

} // end namespace
