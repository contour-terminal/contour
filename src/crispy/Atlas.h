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

#include <QtGui/QVector4D>

#include <cassert>
#include <functional>
#include <map>
#include <optional>
#include <ostream>
#include <type_traits>
#include <variant>
#include <vector>

namespace crispy::atlas {

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

struct TextureInfo {
    unsigned atlas;
    unsigned x;
    unsigned y;
    unsigned z;
    unsigned width;
    unsigned height;
};

struct RenderTexture {
    std::reference_wrapper<TextureInfo const> texture;
    unsigned x;
    unsigned y;
    unsigned z;
    unsigned width;
    unsigned height;
    QVector4D color;
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
 */
template <typename Key, typename Metadata = int>
class TextureAtlas {
  public:
    /**
     * Constructs a texture atlas with given limits.
     *
     * @param _instanceLimit maximum number of OpenGL 3D textures
     * @param _atlasDepth maximum 3D depth (z-value)
     * @param _atlasWidth atlas texture width
     * @param _atlasHeight atlas texture height
     */
    TextureAtlas(unsigned _instanceLimit,
                 unsigned _atlasDepth,
                 unsigned _atlasWidth,
                 unsigned _atlasHeight,
                 std::string _name = {})
      : instanceLimit_{ _instanceLimit },
        atlasDepth_{ _atlasDepth },
        atlasWidth_{ _atlasWidth },
        atlasHeight_{ _atlasHeight },
        name_{ std::move(_name) },
        commandQueue_{}
    {
        commandQueue_.emplace_back(CreateAtlas{
            currentAtlasInstance_,
            atlasWidth_,
            atlasHeight_,
            atlasDepth_
        });
    }

    std::string const& name() const noexcept { return name_; }

    void clear()
    {
        for (unsigned i = 1; i < currentAtlasInstance_; ++i)
            commandQueue_.emplace_back(DestroyAtlas{i});

        *this = TextureAtlas(instanceLimit_, atlasDepth_, atlasWidth_, atlasHeight_);
    }

    /// Tests whether given sub-texture is being present in this texture atlas.
    bool contains(Key const& _id) const
    {
        return allocations_.find(_id) != allocations_.end();
    }

    [[nodiscard]] TextureInfo const* get(Key const& _id) const
    {
        if (auto i = allocations_.find(_id); i != allocations_.end())
            return &i->second;
        else
            return nullptr;
    }

    [[nodiscard]] Metadata const& metadata(Key const& _id) const
    {
        return metadata_.at(_id);
    }

    CommandList& commandQueue() noexcept { return commandQueue_; }
    CommandList const& commandQueue() const noexcept { return commandQueue_; }

    void swap(CommandList& other) { commandQueue_.swap(other); }

    bool insert(Key const& _id,
                unsigned _width,
                unsigned _height,
                Buffer const& _data,
                Metadata&& _metadata = {})
    {
        assert(_height <= atlasHeight_);
        assert(_width <= atlasWidth_);

        // ensure we have enough width space in current row
        if (currentWidth_ + _width >= atlasWidth_ && !allocateFreeRow())
            return false;

        TextureInfo const& info = allocations_[_id] = TextureInfo{
            currentAtlasInstance_,
            currentWidth_,
            currentHeight_,
            currentDepth_,
            _width,
            _height,
        };

        if constexpr (!std::is_same_v<Metadata, void>)
            metadata_.emplace(std::pair{_id, std::move(_metadata)});

        if (_height > maxTextureHeightInCurrentRow_)
            maxTextureHeightInCurrentRow_ = _height;

        // increment to next free slot
        currentWidth_ += _width;
        if (currentWidth_ >= atlasWidth_ && !allocateFreeRow())
            return false;

        commandQueue_.emplace_back(UploadTexture{
            info.atlas,
            info.x,
            info.y,
            info.z,
            info.width,
            info.height,
            _data
        });

        return true;
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
                commandQueue_.emplace_back(CreateAtlas{
                    currentAtlasInstance_,
                    atlasWidth_,
                    atlasHeight_,
                    atlasDepth_
                });
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

    std::string name_;
    CommandList commandQueue_;
    std::map<Key, TextureInfo> allocations_ = {};

    // conditionally transform void to int as I can't conditionally enable/disable this member var.
    std::map<
        Key,
        std::conditional_t<std::is_same_v<Metadata, void>, int, Metadata>
    > metadata_ = {};
};

} // end namespace

namespace std {
    inline ostream& operator<<(ostream& _os, crispy::atlas::CreateAtlas const& _cmd)
    {
        _os << "CreateAtlas{"
            << _cmd.atlas
            << ", " << _cmd.width << '/' << _cmd.height << '/' << _cmd.depth
            << "}";
        return _os;
    }

    inline ostream& operator<<(ostream& _os, crispy::atlas::UploadTexture const& _cmd)
    {
        _os << "UploadTexture{"
            << _cmd.atlas
            << ", " << _cmd.x << '/' << _cmd.y << '/' << _cmd.z
            << ", " << _cmd.width << 'x' << _cmd.height
            << ", #" << _cmd.data.size()
            << "}";
        return _os;
    }

    inline ostream& operator<<(ostream& _os, crispy::atlas::RenderTexture const& _cmd)
    {
        _os << "RenderTexture{"
            << _cmd.texture.get().atlas
            << ", " << _cmd.x << '/' << _cmd.y << '/' << _cmd.z
            << ", " << _cmd.width << 'x' << _cmd.height
            << "}";
        return _os;
    }

    inline ostream& operator<<(ostream& _os, crispy::atlas::DestroyAtlas const& _cmd)
    {
        _os << "RenderTexture{"
            << _cmd.atlas
            << "}";
        return _os;
    }

    inline ostream& operator<<(ostream& _os, crispy::atlas::Command const& _cmd)
    {
        visit([&](auto const& cmd) { _os << cmd; }, _cmd);
        return _os;
    }
}
