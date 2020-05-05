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

#include <fmt/format.h>
#include <iostream>

#include <cassert>
#include <functional>
#include <iomanip> // setprecision
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
    std::reference_wrapper<std::string const> atlasName;
    unsigned width;
    unsigned height;
    unsigned depth;
    unsigned format;                // internal texture format (such as GL_R8 or GL_RGBA8 when using OpenGL)
};

struct DestroyAtlas {
    // ID of the atlas to release the resources on the GPU for.
    unsigned atlas;
    std::reference_wrapper<std::string const> atlasName;
};

struct TextureInfo {
    unsigned atlas;                 // for example 0 for GL_TEXTURE0
    std::reference_wrapper<std::string const> atlasName;
    unsigned x;                     // target x-coordinate into the 3D texture
    unsigned y;                     // target y-coordinate into the 3D texture
    unsigned z;                     // target y-coordinate into the 3D texture
    unsigned width;                 // width of sub-image in pixels
    unsigned height;                // height of sub-image in pixels
    float relativeX;
    float relativeY;
    float relativeWidth;            // width relative to Atlas::width_
    float relativeHeight;           // height relative to Atlas::height_
    unsigned user;                  // some user defined value, in my case, whether or not this texture is colored or monochrome
};

struct UploadTexture {
    std::reference_wrapper<TextureInfo const> texture;  // texture's attributes
    Buffer data;                                        // texture data to be uploaded
    unsigned format;                                    // internal texture format (such as GL_R8 or GL_RGBA8 when using OpenGL)
};

struct RenderTexture {
    std::reference_wrapper<TextureInfo const> texture;
    unsigned x;           // window x coordinate to render the texture to
    unsigned y;           // window y coordinate to render the texture to
    unsigned z;           // window z coordinate to render the texture to
    QVector4D color;
};

/// Generic listener API to events from an Atlas.
///
/// One prominent user is the scheduler in the Renderer.
class CommandListener {
public:
    virtual ~CommandListener() = default;

    /// Creates a new (3D) texture atlas.
    virtual void createAtlas(CreateAtlas const&) = 0;

    /// Uploads given texture to the atlas.
    virtual void uploadTexture(UploadTexture const&) = 0;

    /// Renders given texture from the atlas with the given target position parameters.
    virtual void renderTexture(RenderTexture const&) = 0;

    /// Destroys the given (3D) texture atlas.
    virtual void destroyAtlas(DestroyAtlas const&) = 0;
};

/**
 * Texture Atlas API.
 *
 * This Texture atlas stores textures with given dimension in a 3 dimensional array of atlases.
 * Thus, you may say a 4D atlas ;-)
 *
 * @param Key a comparable key (such as @c char or @c uint32_t) to use to store and access textures.
 * @param Metadata some optionally accessible metadata that is attached with each texture.
 */
template <typename Key, typename Metadata = int>
class TextureAtlas {
  public:
    /**
     * Constructs a texture atlas with given limits.
     *
     * @param _maxInstances maximum number of OpenGL 3D textures
     * @param _depth    maximum 3D depth (z-value)
     * @param _width    atlas texture width
     * @param _height   atlas texture height
     * @param _format   an arbitrary user defined number that defines the storage format for this texture,
     *                  such as GL_R8 or GL_RBGA8 when using OpenGL
     */
    TextureAtlas(unsigned _maxInstances,
                 unsigned _depth,
                 unsigned _width,
                 unsigned _height,
                 unsigned _format, // such as GL_R8 or GL_RGBA8
                 CommandListener& _listener,
                 std::string _name = {})
      : maxInstances_{ _maxInstances },
        depth_{ _depth },
        width_{ _width },
        height_{ _height },
        format_{ _format },
        name_{ std::move(_name) },
        commandListener_{ _listener }
    {
        commandListener_.createAtlas({
            currentAtlasInstance_,
            name_,
            width_,
            height_,
            depth_,
            format_
        });
    }

    std::string const& name() const noexcept { return name_; }
    constexpr unsigned maxInstances() const noexcept { return maxInstances_; }
    constexpr unsigned depth() const noexcept { return depth_; }
    constexpr unsigned width() const noexcept { return width_; }
    constexpr unsigned height() const noexcept { return height_; }

    /// @return number of internally used 3D texture atlases.
    constexpr unsigned currentInstance() const noexcept { return currentAtlasInstance_; }

    /// @return number of 2D text atlases in use in current 3D texture atlas.
    constexpr unsigned currentZ() const noexcept { return currentZ_; }

    /// @return current X offset into the current 3D texture atlas.
    constexpr unsigned currentX() const noexcept { return currentX_; }

    /// @return current Y offset into the current 3D texture atlas.
    constexpr unsigned currentY() const noexcept { return currentY_; }

    constexpr unsigned maxTextureHeightInCurrentRow() const noexcept { return maxTextureHeightInCurrentRow_; }

    /// @return number of textures stored in this texture atlas.
    constexpr size_t size() const noexcept { return allocations_.size(); }

    constexpr void clear()
    {
        currentAtlasInstance_ = 0;
        currentZ_ = 0;
        currentX_ = 0;
        currentY_ = 0;
        maxTextureHeightInCurrentRow_ = 0;

        allocations_.clear();
        metadata_.clear();

        for (unsigned i = 0; i < currentAtlasInstance_; ++i)
            commandListener_.destroyAtlas({i, name_});

        commandListener_.createAtlas({
            currentAtlasInstance_,
            name_,
            width_,
            height_,
            depth_,
            format_
        });
    }

    /// Tests whether given sub-texture is being present in this texture atlas.
    constexpr bool contains(Key const& _id) const
    {
        return allocations_.find(_id) != allocations_.end();
    }

    using DataRef = std::tuple<
        std::reference_wrapper<TextureInfo const>,
        std::reference_wrapper<Metadata const>
    >;

    [[nodiscard]] std::optional<DataRef> get(Key const& _id) const
    {
        if (auto const i = allocations_.find(_id); i != allocations_.end())
            return DataRef{std::ref(i->second), std::ref(metadata_.at(_id))};
        else
            return std::nullopt;
    }

    std::optional<DataRef> insert(Key const& _id,
                                  unsigned _width,
                                  unsigned _height,
                                  unsigned _format,
                                  Buffer _data,
                                  unsigned _user,
                                  Metadata&& _metadata = {})
    {
        // fail early if to-be-inserted texture is too large to fit a single page in the whole atlas
        if (_height > height_ || _width > width_)
            return std::nullopt;

        // ensure we have enough width space in current row
        if (currentX_ + _width >= width_ && !advanceY())
            return std::nullopt;

        // ensure we have enoguh height space in current row
        if (currentY_ + _height > height_ && !advanceZ())
            return std::nullopt;

        TextureInfo const& info = allocations_.emplace(std::pair{
            _id,
            TextureInfo{
                currentAtlasInstance_,
                name_,
                currentX_,
                currentY_,
                currentZ_,
                _width,
                _height,
                static_cast<float>(currentX_) / static_cast<float>(width_),
                static_cast<float>(currentY_) / static_cast<float>(height_),
                static_cast<float>(_width) / static_cast<float>(width_),
                static_cast<float>(_height) / static_cast<float>(height_),
                _user
            }
        }).first->second;

        if constexpr (!std::is_same_v<Metadata, void>)
            metadata_.emplace(std::pair{_id, std::move(_metadata)});

        currentX_ += _width;

        if (_height > maxTextureHeightInCurrentRow_)
            maxTextureHeightInCurrentRow_ = _height;

        commandListener_.uploadTexture(UploadTexture{
            std::ref(info),
            std::move(_data),
            _format
        });

        return get(_id);
    }

  private:
    constexpr bool advanceY()
    {
        if (currentY_ + maxTextureHeightInCurrentRow_ <= height_)
        {
            currentY_ += maxTextureHeightInCurrentRow_;
            currentX_ = 0;
            maxTextureHeightInCurrentRow_ = 0;
            return true;
        }
        else
            return advanceZ();
    }

    constexpr bool advanceZ()
    {
        if (currentZ_ < depth_)
        {
            currentZ_++;
            currentY_ = 0;
            currentX_ = 0;
            maxTextureHeightInCurrentRow_ = 0;
            return true;
        }
        else
            return advanceInstance();
    }

    constexpr bool advanceInstance()
    {
        if (currentAtlasInstance_ < maxInstances_)
        {
            currentAtlasInstance_++;
            currentZ_ = 0;
            currentY_ = 0;
            currentX_ = 0;
            maxTextureHeightInCurrentRow_ = 0;

            commandListener_.createAtlas({
                currentAtlasInstance_,
                name_,
                width_,
                height_,
                depth_,
                format_
            });

            return true;
        }
        else
            return false;
    }

  private:
    unsigned const maxInstances_;       // maximum number of atlas instances (e.g. maximum number of OpenGL 3D textures)
    unsigned const depth_;              // atlas total depth
    unsigned const width_;              // atlas total width
    unsigned const height_;             // atlas total height
    unsigned const format_;             // internal storage format, such as GL_R8 or GL_RGBA8

    std::string const name_;            // atlas human readable name (only for debugging)
    CommandListener& commandListener_;  // atlas event listener (used to perform allocation/modification actions)

    unsigned currentAtlasInstance_ = 0; // (OpenGL) texture count already in use
    unsigned currentZ_ = 0;             // index to current atlas that is being filled
    unsigned currentX_ = 0;             // current X-offset to start drawing to
    unsigned currentY_ = 0;             // current Y-offset to start drawing to
    unsigned maxTextureHeightInCurrentRow_ = 0; // current maximum height in the current row (used to increment currentY_ to get to the next row)

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
        _os << '{'
            << _cmd.atlasName.get() << '/' << _cmd.atlas
            << ", " << _cmd.width << '/' << _cmd.height << '/' << _cmd.depth
            << ", " << _cmd.format
            << '}';
        return _os;
    }

    inline ostream& operator<<(ostream& _os, crispy::atlas::TextureInfo const& _info)
    {
        _os << '{'
            << _info.atlasName.get() << '/' << _info.atlas << '/' << _info.x << '/' << _info.y << '/' << _info.z
            << ", " << _info.width << 'x' << _info.height
            << '}';
        return _os;
    }

    inline ostream& operator<<(ostream& _os, crispy::atlas::UploadTexture const& _cmd)
    {
        _os << '{'
            << _cmd.texture.get()
            << ", len:" << _cmd.data.size()
            << ", format:" << _cmd.format
            << '}';
        return _os;
    }

    inline ostream& operator<<(ostream& _os, crispy::atlas::RenderTexture const& _cmd)
    {
        _os << '{'
            << "AtlasCoord: " << _cmd.texture.get()
            << ", Target: " << _cmd.x << '/' << _cmd.y << '/' << _cmd.z
            << '}';
        return _os;
    }

    inline ostream& operator<<(ostream& _os, crispy::atlas::DestroyAtlas const& _cmd)
    {
        _os << '{'
            << _cmd.atlasName.get() << '/' << _cmd.atlas
            << '}';
        return _os;
    }

    template <typename Key, typename Metadata>
    inline ostream& operator<<(ostream& _os, crispy::atlas::TextureAtlas<Key, Metadata> const& _atlas)
    {
        _os << '{'
            << _atlas.name() << ": "
            << fmt::format("instance: {}/{}", _atlas.currentInstance(), _atlas.maxInstances())
            << fmt::format(", dim: {}x{}x{}", _atlas.width(), _atlas.height(), _atlas.depth())
            << fmt::format(", at: {}x{}x{}", _atlas.currentX(), _atlas.currentY(), _atlas.currentZ())
            << fmt::format(", rowHeight: {}", _atlas.maxTextureHeightInCurrentRow())
            << '}';
        return _os;
    }
}
