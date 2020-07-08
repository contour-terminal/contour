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
#include <list>
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
    unsigned targetWidth;           // width of the sub-image when being rendered
    unsigned targetHeight;          // height of the sub-image when being rendered
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
    QVector4D color;      // optional; a color being associated with this texture
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
class TextureAtlas {
  public:
    /**
     * Constructs a texture atlas with given limits.
     *
     * @param _instanceBaseId any arbitrary number that the first instance ID will be assigned.
     *                         This is the base for any further
     * @param _maxInstances maximum number of OpenGL 3D textures
     * @param _depth    maximum 3D depth (z-value)
     * @param _width    atlas texture width
     * @param _height   atlas texture height
     * @param _format   an arbitrary user defined number that defines the storage format for this texture,
     *                  such as GL_R8 or GL_RBGA8 when using OpenGL
     */
    TextureAtlas(unsigned _instanceBaseId,
                 unsigned _maxInstances,
                 unsigned _depth,
                 unsigned _width,
                 unsigned _height,
                 unsigned _format, // such as GL_R8 or GL_RGBA8
                 CommandListener& _listener,
                 std::string _name = {})
      : instanceBaseId_{ _instanceBaseId },
        maxInstances_{ _maxInstances },
        depth_{ _depth },
        width_{ _width },
        height_{ _height },
        format_{ _format },
        name_{ std::move(_name) },
        commandListener_{ _listener },
        currentInstanceId_{ instanceBaseId_ }
    {
        notifyCreateAtlas();
    }

    TextureAtlas(TextureAtlas const&) = delete;
    TextureAtlas& operator=(TextureAtlas const&) = delete;
    TextureAtlas(TextureAtlas&&) = delete; // TODO
    TextureAtlas& operator=(TextureAtlas&&) = delete; // TODO

    ~TextureAtlas()
    {
        for (unsigned id = instanceBaseId_; id <= currentInstanceId_; ++id)
            commandListener_.destroyAtlas(DestroyAtlas{id, name_});
    }

    std::string const& name() const noexcept { return name_; }
    constexpr unsigned maxInstances() const noexcept { return maxInstances_; }
    constexpr unsigned depth() const noexcept { return depth_; }
    constexpr unsigned width() const noexcept { return width_; }
    constexpr unsigned height() const noexcept { return height_; }

    /// @return number of internally used 3D texture atlases.
    constexpr unsigned currentInstance() const noexcept { return currentInstanceId_; }

    /// @return number of 2D text atlases in use in current 3D texture atlas.
    constexpr unsigned currentZ() const noexcept { return currentZ_; }

    /// @return current X offset into the current 3D texture atlas.
    constexpr unsigned currentX() const noexcept { return currentX_; }

    /// @return current Y offset into the current 3D texture atlas.
    constexpr unsigned currentY() const noexcept { return currentY_; }

    constexpr unsigned maxTextureHeightInCurrentRow() const noexcept { return maxTextureHeightInCurrentRow_; }

    void clear()
    {
        currentInstanceId_ = instanceBaseId_;
        currentZ_ = 0;
        currentX_ = 0;
        currentY_ = 0;
        maxTextureHeightInCurrentRow_ = 0;
    }

    TextureInfo& get(size_t _index) { return *std::next(std::begin(textureInfos_), _index); }
    TextureInfo const& get(size_t _index) const { return *std::next(std::begin(textureInfos_), _index); }

    /// Inserts a new texture into the atlas.
    ///
    /// @param _id       a unique identifier used for accessing this texture
    /// @param _width    texture width in pixels
    /// @param _height   texture height in pixels
    /// @param _format   data format
    /// @param _data     raw texture data to be inserted
    /// @param _user     user defined data that is supplied along with TexCoord's 4th component
    ///
    /// @return index to the created TextureInfo or std::nullopt if failed.
    TextureInfo const* insert(unsigned _width,
                              unsigned _height,
                              unsigned _targetWidth,
                              unsigned _targetHeight,
                              unsigned _format,
                              Buffer&& _data,
                              unsigned _user = 0)
    {
        // fail early if to-be-inserted texture is too large to fit a single page in the whole atlas
        if (_height > height_ || _width > width_)
            return nullptr;

        // ensure we have enough width space in current row
        if (currentX_ + _width >= width_ && !advanceY())
            return nullptr;

        // ensure we have enoguh height space in current row
        if (currentY_ + _height > height_ && !advanceZ())
            return nullptr;

        textureInfos_.emplace_back(TextureInfo{
            currentInstanceId_,
            name_,
            currentX_,
            currentY_,
            currentZ_,
            _width,
            _height,
            _targetWidth,
            _targetHeight,
            static_cast<float>(currentX_) / static_cast<float>(width_),
            static_cast<float>(currentY_) / static_cast<float>(height_),
            static_cast<float>(_width) / static_cast<float>(width_),
            static_cast<float>(_height) / static_cast<float>(height_),
            _user
        });

        currentX_ += _width;

        if (_height > maxTextureHeightInCurrentRow_)
            maxTextureHeightInCurrentRow_ = _height;

        TextureInfo const& info = textureInfos_.back();

        commandListener_.uploadTexture(UploadTexture{
            std::ref(info),
            std::move(_data),
            _format
        });

        return &info;
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
        if (currentInstanceId_ < instanceBaseId_ + maxInstances_)
        {
            currentInstanceId_++;
            currentZ_ = 0;
            currentY_ = 0;
            currentX_ = 0;
            maxTextureHeightInCurrentRow_ = 0;

            notifyCreateAtlas();
            return true;
        }
        else
            return false;
    }

    void notifyCreateAtlas()
    {
        commandListener_.createAtlas({
            currentInstanceId_,
            name_,
            width_,
            height_,
            depth_,
            format_
        });
    }

  private:
    unsigned const instanceBaseId_;    // default value to assign to first instance, and incrementing from that point for further instances.
    unsigned const maxInstances_;       // maximum number of atlas instances (e.g. maximum number of OpenGL 3D textures)
    unsigned const depth_;              // atlas total depth
    unsigned const width_;              // atlas total width
    unsigned const height_;             // atlas total height
    unsigned const format_;             // internal storage format, such as GL_R8 or GL_RGBA8

    std::string const name_;            // atlas human readable name (only for debugging)
    CommandListener& commandListener_;  // atlas event listener (used to perform allocation/modification actions)

    unsigned currentInstanceId_;        // (OpenGL) texture count already in use
    unsigned currentZ_ = 0;             // index to current atlas that is being filled
    unsigned currentX_ = 0;             // current X-offset to start drawing to
    unsigned currentY_ = 0;             // current Y-offset to start drawing to
    unsigned maxTextureHeightInCurrentRow_ = 0; // current maximum height in the current row (used to increment currentY_ to get to the next row)

    std::list<TextureInfo> textureInfos_;
};

template <typename Key, typename Metadata = int>
class MetadataTextureAtlas {
  public:
    MetadataTextureAtlas(unsigned _instanceBaseId,
                         unsigned _maxInstances,
                         unsigned _depth,
                         unsigned _width,
                         unsigned _height,
                         unsigned _format, // such as GL_R8 or GL_RGBA8
                         CommandListener& _listener,
                         std::string _name = {}) :
        atlas_{
            _instanceBaseId,
            _maxInstances,
            _depth,
            _width,
            _height,
            _format,
            _listener,
            std::move(_name)
        }
    {
    }

    MetadataTextureAtlas(MetadataTextureAtlas const&) = delete;
    MetadataTextureAtlas& operator=(MetadataTextureAtlas const&) = delete;
    MetadataTextureAtlas(MetadataTextureAtlas&&) = delete; // TODO
    MetadataTextureAtlas& operator=(MetadataTextureAtlas&&) = delete; // TODO

    //std::string const& name() const noexcept { return name_; }
    constexpr unsigned maxInstances() const noexcept { return atlas_.maxInstances(); }
    constexpr unsigned depth() const noexcept { return atlas_.depth(); }
    constexpr unsigned width() const noexcept { return atlas_.width(); }
    constexpr unsigned height() const noexcept { return atlas_.height(); }

    /// @return number of textures stored in this texture atlas.
    constexpr size_t size() const noexcept { return allocations_.size(); }

    /// @return boolean indicating whether or not this atlas is empty (has no textures present).
    constexpr bool empty() const noexcept { return allocations_.size() == 0; }

    void clear()
    {
        atlas_.clear();
        allocations_.clear();
        metadata_.clear();
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

    /// Inserts a new texture into the atlas.
    ///
    /// @param _id       a unique identifier used for accessing this texture
    /// @param _width    texture width in pixels
    /// @param _height   texture height in pixels
    /// @param _format   data format
    /// @param _data     raw texture data to be inserted
    /// @param _user     user defined data that is supplied along with TexCoord's 4th component
    /// @param _metadata user defined metadata for the host
    ///
    /// @return index to the corresponding DataRef or std::nullopt if failed.
    std::optional<DataRef> insert(Key const& _id,
                                  unsigned _width,
                                  unsigned _height,
                                  unsigned _targetWidth,
                                  unsigned _targetHeight,
                                  unsigned _format,
                                  Buffer&& _data,
                                  unsigned _user = 0,
                                  Metadata _metadata = {})
    {
        assert(allocations_.find(_id) == allocations_.end());

        TextureInfo const* textureInfo = atlas_.insert(_width, _height, _targetWidth, _targetHeight, _format, std::move(_data), _user);
        if (!textureInfo)
            return std::nullopt;

        allocations_.emplace(std::pair{_id, std::ref(*textureInfo)});

        if constexpr (!std::is_same_v<Metadata, void>)
            metadata_.emplace(std::pair{_id, std::move(_metadata)});

        return get(_id);
    }

    /// Retrieves TextureInfo and Metadata tuple if available, std::nullopt otherwise.
    [[nodiscard]] std::optional<DataRef> get(Key const& _id) const
    {
        if (auto const i = allocations_.find(_id); i != allocations_.end())
            return DataRef{std::ref(i->second), std::ref(metadata_.at(_id))};
        else
            return std::nullopt;
    }

  private:
    TextureAtlas atlas_;

    std::map<Key, TextureInfo> allocations_ = {};

    // conditionally transform void to int as I can't conditionally enable/disable this member var.
    std::map<
        Key,
        std::conditional_t<std::is_same_v<Metadata, void>, int, Metadata>
    > metadata_ = {};
};

} // end namespace

namespace fmt { // {{{
    template <>
    struct formatter<crispy::atlas::CreateAtlas> {
        template <typename ParseContext>
        constexpr auto parse(ParseContext& ctx) { return ctx.begin(); }
        template <typename FormatContext>
        auto format(crispy::atlas::CreateAtlas const& _cmd, FormatContext& ctx)
        {
            return format_to(ctx.out(), "<atlas:{}, dim:{}x{}, depth:{}, format:{}>",
                _cmd.atlasName.get(),
                _cmd.width,
                _cmd.height,
                _cmd.depth,
                _cmd.format
            );
        }
    };

    template <>
    struct formatter<crispy::atlas::TextureInfo> {
        template <typename ParseContext>
        constexpr auto parse(ParseContext& ctx) { return ctx.begin(); }
        template <typename FormatContext>
        auto format(crispy::atlas::TextureInfo const& info, FormatContext& ctx)
        {
            return format_to(ctx.out(), "<{}; {}x{}/{}x{}; {}/{}/{}>",
                info.atlasName.get(),
                info.width,
                info.height,
                info.targetWidth,
                info.targetHeight,
                info.x,
                info.y,
                info.z
            );
        }
    };

    template <>
    struct formatter<crispy::atlas::UploadTexture> {
        template <typename ParseContext>
        constexpr auto parse(ParseContext& ctx) { return ctx.begin(); }
        template <typename FormatContext>
        auto format(crispy::atlas::UploadTexture const& _cmd, FormatContext& ctx)
        {
            return format_to(ctx.out(), "<texture:{}, len:{}, format:{}>",
                _cmd.texture.get(),
                _cmd.data.size(),
                _cmd.format
            );
        }
    };

    template <>
    struct formatter<crispy::atlas::RenderTexture> {
        template <typename ParseContext>
        constexpr auto parse(ParseContext& ctx) { return ctx.begin(); }
        template <typename FormatContext>
        auto format(crispy::atlas::RenderTexture const& _cmd, FormatContext& ctx)
        {
            return format_to(ctx.out(), "<AtlasCoord:{}, target: {}:{}:{}>",
                _cmd.texture.get(),
                _cmd.x,
                _cmd.y,
                _cmd.z
            );
        }
    };

    template <>
    struct formatter<crispy::atlas::DestroyAtlas> {
        template <typename ParseContext>
        constexpr auto parse(ParseContext& ctx) { return ctx.begin(); }
        template <typename FormatContext>
        auto format(crispy::atlas::DestroyAtlas const& _cmd, FormatContext& ctx)
        {
            return format_to(ctx.out(), "<atlas: {}, id:{}>",
                _cmd.atlasName.get(),
                _cmd.atlas
            );
        }
    };

    template <>
    struct formatter<crispy::atlas::TextureAtlas> {
        template <typename ParseContext>
        constexpr auto parse(ParseContext& ctx) { return ctx.begin(); }
        template <typename FormatContext>
        auto format(crispy::atlas::TextureAtlas const& _atlas, FormatContext& ctx)
        {
            return format_to(ctx.out(), "<instance: {}/{}, dim: {}x{}x{}, at: {}x{}x{}, rowHeight:{}>",
                _atlas.currentInstance(), _atlas.maxInstances(),
                _atlas.width(), _atlas.height(), _atlas.depth(),
                _atlas.currentX(), _atlas.currentY(), _atlas.currentZ(),
                _atlas.maxTextureHeightInCurrentRow()
            );
        }
    };
} // }}}
