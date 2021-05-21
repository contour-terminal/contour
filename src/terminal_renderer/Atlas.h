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

#include <crispy/size.h>
#include <crispy/debuglog.h>

#include <fmt/format.h>

#include <algorithm>
#include <array>
#include <cassert>
#include <functional>
#include <list>
#include <map>
#include <optional>
#include <type_traits>
#include <unordered_map>
#include <vector>

namespace terminal::renderer::atlas {

auto const inline AtlasTag = crispy::debugtag::make("renderer.atlas", "Logs details about texture atlas.");

using Buffer = std::vector<uint8_t>;
enum class Format { Red, RGB, RGBA };

constexpr int element_count(Format _format) noexcept
{
    switch (_format)
    {
        case Format::Red: return 1;
        case Format::RGB: return 3;
        case Format::RGBA: return 4;
    }
    return 0;
}

struct AtlasID
{
    int value;

    constexpr bool operator<(AtlasID const& _rhs) const noexcept { return value < _rhs.value; }
    constexpr bool operator==(AtlasID const& _rhs) const noexcept { return value == _rhs.value; }
};

struct CreateAtlas {
    AtlasID atlas;
    crispy::Size size;
    Format format;                // internal texture format (such as GL_R8 or GL_RGBA8 when using OpenGL)
    int user;
};

struct DestroyAtlas {
    // ID of the atlas to release the resources on the GPU for.
    int atlas;
};

struct TextureInfo {
    TextureInfo(TextureInfo &&) = default;
    TextureInfo& operator=(TextureInfo &&) = default;
    TextureInfo(TextureInfo const&) = delete;
    TextureInfo& operator=(TextureInfo const&) = delete;

    AtlasID atlas;                  // for example 0 for GL_TEXTURE0
    std::reference_wrapper<std::string const> atlasName;
    crispy::Point offset;           // Offset into the 2D texture atlas.
    crispy::Size bitmapSize;        // width/height of sub-image in pixels
    crispy::Size targetSize;        // width/height of sub-image when being rendered
    float relativeX;
    float relativeY;
    float relativeWidth;            // width relative to Atlas::width_
    float relativeHeight;           // height relative to Atlas::height_
    int user;                       // some user defined value, in my case, whether or not this texture is colored or monochrome
};

struct UploadTexture {
    std::reference_wrapper<TextureInfo const> texture;  // texture's attributes
    Buffer data;                                        // texture data to be uploaded
    Format format;                                      // internal texture format (such as GL_R8 or GL_RGBA8 when using OpenGL)
};

struct RenderTexture {
    std::reference_wrapper<TextureInfo const> texture;
    int x;                          // window x coordinate to render the texture to
    int y;                          // window y coordinate to render the texture to
    int z;                          // window z coordinate to render the texture to
    std::array<float, 4> color;     // optional; a color being associated with this texture
};

/// Generic listener API to events from an Atlas.
/// AtlasBackend interface, performs the actual atlas operations, such as
/// texture creation, upload, render, and destruction.
///
/// @see OpenGLRenderer
class AtlasBackend {
  public:
    virtual ~AtlasBackend() = default;

    /// Creates a new (3D) texture atlas.
    virtual AtlasID createAtlas(crispy::Size _size, Format _textureFormat, int _user) = 0;

    /// Uploads given texture to the atlas.
    virtual void uploadTexture(UploadTexture _texture) = 0;

    /// Renders given texture from the atlas with the given target position parameters.
    virtual void renderTexture(RenderTexture _texture) = 0;

    /// Destroys the given (3D) texture atlas.
    virtual void destroyAtlas(AtlasID _atlasID) = 0;
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
class TextureAtlasAllocator {
  public:
    struct Cursor {
        AtlasID atlas;
        crispy::Point position;
    };

    /**
     * Constructs a texture atlas with given limits.
     *
     * @param _maxInstances maximum number of OpenGL 3D textures
     * @param _width        atlas texture width
     * @param _height       atlas texture height
     * @param _format       an arbitrary user defined number that defines the storage format for this texture,
     *                      such as GL_R8 or GL_RBGA8 when using OpenGL
     */
    TextureAtlasAllocator(AtlasBackend& _backend,
                          crispy::Size _atlasTextureSize,
                          int _maxInstances,
                          Format _format, // such as GL_R8 or GL_RGBA8
                          int _user,
                          std::string _name);

    TextureAtlasAllocator(TextureAtlasAllocator const&) = delete;
    TextureAtlasAllocator& operator=(TextureAtlasAllocator const&) = delete;
    TextureAtlasAllocator(TextureAtlasAllocator&&) = delete; // TODO
    TextureAtlasAllocator& operator=(TextureAtlasAllocator&&) = delete; // TODO

    ~TextureAtlasAllocator();

    constexpr int user() const noexcept { return user_; }
    std::string const& name() const noexcept { return name_; }
    constexpr int maxInstances() const noexcept { return maxInstances_; }
    constexpr crispy::Size size() const noexcept { return size_; }
    constexpr Format format() const noexcept { return format_; }

    std::vector<AtlasID> const& activeAtlasTextures() const noexcept { return atlasIDs_; }

    /// @return number of internally used 3D texture atlases.
    constexpr AtlasID currentInstance() const noexcept { return cursor_.atlas; }

    /// @return current X offset into the current 3D texture atlas.
    constexpr int currentX() const noexcept { return cursor_.position.x; }

    /// @return current Y offset into the current 3D texture atlas.
    constexpr int currentY() const noexcept { return cursor_.position.y; }

    constexpr int maxTextureHeightInCurrentRow() const noexcept { return maxTextureHeightInCurrentRow_; }

    void clear();

    TextureInfo const& get(size_t _index) const { return *std::next(std::begin(textureInfos_), _index); }

    // Configure some enforced horizontal/vertical gap between the subtextures.
    auto inline static constexpr HorizontalGap = 0;
    auto inline static constexpr VerticalGap = 0;

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
    TextureInfo const* insert(crispy::Size _bitmapSize,
                              crispy::Size _targetSize,
                              Format _format,
                              Buffer _data,
                              int _user = 0);

    /// Releases a given texture area the atlas for future reallocations.
    void release(TextureInfo const& _info);

    constexpr Cursor cursor() const noexcept { return cursor_; }

  private:
    std::optional<Cursor> getOffsetAndAdvance(crispy::Size _bitmapSize);

    void getOrCreateNewAtlas()
    {
        if (unusedAtlasIDs_.empty())
        {
            cursor_.atlas = atlasBackend_.createAtlas(size_, format_, user_);
        }
        else
        {
            cursor_.atlas = unusedAtlasIDs_.back();
            unusedAtlasIDs_.pop_back();
        }
        atlasIDs_.push_back(cursor_.atlas);
        cursor_.position.x = 0;
        cursor_.position.y = 0;
    }

    TextureInfo const& appendTextureInfo(crispy::Size _bitmapSize,
                                         crispy::Size _targetSize,
                                         Cursor _offset,
                                         int _user);


    // private data fields
    //
    AtlasBackend& atlasBackend_;   // atlas event listener (used to perform allocation/modification actions)
    size_t const maxInstances_;    // maximum number of atlas instances (e.g. maximum number of OpenGL 3D textures)
    crispy::Size size_;            // total atlas texture size in pixels
    Format const format_;          // internal storage format, such as GL_R8 or GL_RGBA8

    int const user_;               // user-defined arbitrary data that relates to this atlas.
    std::string const name_;       // atlas human readable name (only for debugging)

    Cursor cursor_;                // current texture ID and cursor for the next sub texture
    int maxTextureHeightInCurrentRow_ = 0; // current maximum height in the current row (used to increment currentY_ to get to the next row)

    // TODO: make this an unordered_map
    std::map<crispy::Size, std::vector<Cursor>> discarded_; // map of texture size to list of atlas texture offsets of regions that have been discarded and are available for reuse.
    std::vector<AtlasID> atlasIDs_;
    std::vector<AtlasID> unusedAtlasIDs_;

    std::list<TextureInfo> textureInfos_;
};

template <typename Key, typename Metadata = int>
class MetadataTextureAtlas {
  public:
    explicit MetadataTextureAtlas(TextureAtlasAllocator& _allocator) :
        atlas_{ _allocator }
    {
    }

    MetadataTextureAtlas(MetadataTextureAtlas const&) = delete;
    MetadataTextureAtlas& operator=(MetadataTextureAtlas const&) = delete;
    MetadataTextureAtlas(MetadataTextureAtlas&&) = delete; // TODO
    MetadataTextureAtlas& operator=(MetadataTextureAtlas&&) = delete; // TODO

    //std::string const& name() const noexcept { return name_; }
    constexpr int maxInstances() const noexcept { return atlas_.maxInstances(); }
    constexpr crispy::Size size() const noexcept { return atlas_.size(); }

    /// @return number of textures stored in this texture atlas.
    //TODO: (do we need you?) constexpr size_t allocationCount() const noexcept { return allocations_.size(); }

    /// @return boolean indicating whether or not this atlas is empty (has no textures present).
    constexpr bool empty() const noexcept { return allocations_.size() == 0; }

    TextureAtlasAllocator& allocator() noexcept { return atlas_; }
    TextureAtlasAllocator const& allocator() const noexcept { return atlas_; }

    /// Clears userdata, if the TextureAtlasAllocator has to be cleared too, that has to be done
    /// explicitly.
    void clear()
    {
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
    /// @param _data     raw texture data to be inserted
    /// @param _user     user defined data that is supplied along with TexCoord's 4th component
    /// @param _metadata user defined metadata for the host
    ///
    /// @return index to the corresponding DataRef or std::nullopt if failed.
    std::optional<DataRef> insert(Key const& _id,
                                  crispy::Size _bitmapSize,
                                  crispy::Size _targetSize,
                                  Buffer&& _data,
                                  int _user = 0,
                                  Metadata _metadata = {})
    {
        assert(allocations_.find(_id) == allocations_.end());

        TextureInfo const* textureInfo = atlas_.insert(_bitmapSize,
                                                       _targetSize,
                                                       atlas_.format(),
                                                       std::move(_data),
                                                       _user);
        if (!textureInfo)
            return std::nullopt;

        allocations_.emplace(_id, textureInfo);

        if constexpr (!std::is_same_v<Metadata, void>)
            metadata_.emplace(std::pair{_id, std::move(_metadata)});

        return get(_id);
    }

    /// Retrieves TextureInfo and Metadata tuple if available, std::nullopt otherwise.
    [[nodiscard]] std::optional<DataRef> get(Key const& _id) const
    {
        if (auto const i = allocations_.find(_id); i != allocations_.end())
            return DataRef{*i->second, metadata_.at(_id)};
        else
            return std::nullopt;
    }

    void release(Key const& _id)
    {
        if (auto k = metadata_.find(_id); k != metadata_.end())
            metadata_.erase(k);

        if (auto const i = allocations_.find(_id); i != allocations_.end())
        {
            TextureInfo const& ti = *i->second;
            atlas_.release(ti);

            allocations_.erase(i);
        }
    }

  private:
    TextureAtlasAllocator& atlas_;

    std::unordered_map<Key, TextureInfo const*> allocations_ = {};

    // conditionally transform void to int as I can't conditionally enable/disable this member var.
    std::unordered_map<
        Key,
        std::conditional_t<std::is_same_v<Metadata, void>, int, Metadata>
    > metadata_ = {};
};

} // end namespace

namespace std
{
    template<>
    struct hash<terminal::renderer::atlas::AtlasID>
    {
        constexpr size_t operator()(terminal::renderer::atlas::AtlasID _atlasID) const noexcept
        {
            return static_cast<size_t>(_atlasID.value);
        }
    };
}

namespace fmt { // {{{
    template <>
    struct formatter<terminal::renderer::atlas::Format> {
        template <typename ParseContext>
        constexpr auto parse(ParseContext& ctx) { return ctx.begin(); }
        template <typename FormatContext>
        auto format(terminal::renderer::atlas::Format _format, FormatContext& ctx)
        {
            switch (_format)
            {
                case terminal::renderer::atlas::Format::RGBA: return format_to(ctx.out(), "RGBA");
                case terminal::renderer::atlas::Format::RGB: return format_to(ctx.out(), "RGB");
                case terminal::renderer::atlas::Format::Red: return format_to(ctx.out(), "Alpha");
            }
            return format_to(ctx.out(), "unknown");
        }
    };

    template <>
    struct formatter<terminal::renderer::atlas::CreateAtlas> {
        template <typename ParseContext>
        constexpr auto parse(ParseContext& ctx) { return ctx.begin(); }
        template <typename FormatContext>
        auto format(terminal::renderer::atlas::CreateAtlas const& _cmd, FormatContext& ctx)
        {
            return format_to(ctx.out(), "<atlas:{}, dim:{}, format:{}>",
                _cmd.atlas,
                _cmd.size,
                _cmd.format
            );
        }
    };

    template <>
    struct formatter<terminal::renderer::atlas::TextureInfo> {
        template <typename ParseContext>
        constexpr auto parse(ParseContext& ctx) { return ctx.begin(); }
        template <typename FormatContext>
        auto format(terminal::renderer::atlas::TextureInfo const& info, FormatContext& ctx)
        {
            return format_to(ctx.out(), "<{}; {}x{}/{}x{}; {}/{}/{}>",
                info.atlasName.get(),
                info.bitmapSize.width,
                info.bitmapSize.height,
                info.targetSize.width,
                info.targetSize.height,
                info.offset.x,
                info.offset.y
            );
        }
    };

    template <>
    struct formatter<terminal::renderer::atlas::UploadTexture> {
        template <typename ParseContext>
        constexpr auto parse(ParseContext& ctx) { return ctx.begin(); }
        template <typename FormatContext>
        auto format(terminal::renderer::atlas::UploadTexture const& _cmd, FormatContext& ctx)
        {
            return format_to(ctx.out(), "<texture:{}, len:{}, format:{}>",
                _cmd.texture.get(),
                _cmd.data.size(),
                _cmd.format
            );
        }
    };

    template <>
    struct formatter<terminal::renderer::atlas::RenderTexture> {
        template <typename ParseContext>
        constexpr auto parse(ParseContext& ctx) { return ctx.begin(); }
        template <typename FormatContext>
        auto format(terminal::renderer::atlas::RenderTexture const& _cmd, FormatContext& ctx)
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
    struct formatter<terminal::renderer::atlas::AtlasID> {
        template <typename ParseContext>
        constexpr auto parse(ParseContext& ctx) { return ctx.begin(); }
        template <typename FormatContext>
        auto format(terminal::renderer::atlas::AtlasID const& _atlasID, FormatContext& ctx)
        {
            return format_to(ctx.out(), "{}", _atlasID.value);
        }
    };

    template <>
    struct formatter<terminal::renderer::atlas::TextureAtlasAllocator> {
        template <typename ParseContext>
        constexpr auto parse(ParseContext& ctx) { return ctx.begin(); }
        template <typename FormatContext>
        auto format(terminal::renderer::atlas::TextureAtlasAllocator const& _atlas, FormatContext& ctx)
        {
            return format_to(ctx.out(), "TextureAtlasAllocator<cursor: {}/{} ({}x{}), rowHeight:{}>",
                _atlas.cursor().atlas,
                _atlas.cursor().position,
                _atlas.size(),
                _atlas.maxInstances(),
                _atlas.maxTextureHeightInCurrentRow()
            );
        }
    };
} // }}}
