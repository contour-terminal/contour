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

#include <terminal/Color.h>
#include <terminal/Grid.h> // cell attribs
#include <terminal/primitives.h>

#include <terminal_renderer/GridMetrics.h>
#include <terminal_renderer/TextureAtlas.h>
#include <terminal_renderer/shared_defines.h>

#include <crispy/size.h>
#include <crispy/stdfs.h>

#include <array>
#include <memory>
#include <optional>
#include <vector>

namespace terminal
{
struct BackgroundImage;
}

namespace terminal::renderer
{

/**
 * Contains the read-out of the state of an texture atlas.
 */
struct AtlasTextureScreenshot
{
    int atlasInstanceId;
    ImageSize size;
    atlas::Format format;
    atlas::Buffer buffer;
};

/**
 * Defines the attributes of a RenderTile, such as render-offset relative
 * to the render target position.
 *
 * For example the later M may be close to the origin (0,0) (bottom left)
 * and have the extent close to the top right of the grid cell size,
 * whereas the `-` symbol may be offset to the vertical middle and have a
 * vertical extent of just a few pixels.
 *
 * This information is usually font specific and produced by (for example)
 * the text shaping engine and/or the glyph rasterizer.
 *
 * For image fragments x/y will most likely be (0, 0) and
 * width/height span the full grid cell.
 *
 * The bitmap's size is already stored in TextureAtlas::TileCreateData.
 */
struct RenderTileAttributes
{
    // clang-format off
    struct X { int value; };
    struct Y { int value; };
    // clang-format on

    // render x-offset relative to pen position
    X x {};

    // render y-offset relative to pen position
    Y y {};

    // Defines how to interpret the texture data.
    // It could for example be gray-scale antialiased, LCD subpixel antialiased,
    // or a simple RGBA texture.
    // See:
    // - FRAGMENT_SELECTOR_IMAGE_BGRA
    // - FRAGMENT_SELECTOR_GLYPH_ALPHA
    // - FRAGMENT_SELECTOR_GLYPH_LCD
    uint32_t fragmentShaderSelector = FRAGMENT_SELECTOR_IMAGE_BGRA;

    atlas::NormalizedTileLocation normalizedLocation {};

    ImageSize targetSize {};
};

/**
 * Terminal render target interface, for example OpenGL, DirectX, or software-rasterization.
 *
 * @see OpenGLRenderer
 */
class RenderTarget
{
  public:
    using RGBAColor = terminal::RGBAColor;
    using Width = crispy::Width;
    using Height = crispy::Height;
    using TextureAtlas = terminal::renderer::atlas::TextureAtlas<RenderTileAttributes>;

    virtual ~RenderTarget() = default;

    /// Sets the render target's size in pixels.
    /// This is the size that can be rendered to.
    virtual void setRenderSize(ImageSize _size) = 0;

    virtual void setMargin(PageMargin _margin) = 0;

    virtual atlas::AtlasBackend& textureScheduler() = 0;

    virtual void setBackgroundImage(
        std::shared_ptr<terminal::BackgroundImage const> const& _backgroundImage) = 0;

    /// Fills a rectangular area with the given solid color.
    virtual void renderRectangle(int x, int y, Width, Height, RGBAColor color) = 0;

    using ScreenshotCallback =
        std::function<void(std::vector<uint8_t> const& /*_rgbaBuffer*/, ImageSize /*_pixelSize*/)>;

    /// Schedules taking a screenshot of the current scene and forwards it to the given callback.
    virtual void scheduleScreenshot(ScreenshotCallback _callback) = 0;

    /// Clears the target surface with the given fill color.
    virtual void clear(terminal::RGBAColor _fillColor) = 0;

    /// Executes all previously scheduled render commands.
    virtual void execute() = 0;

    /// Clears any existing caches.
    virtual void clearCache() = 0;

    /// Reads out the given texture atlas.
    virtual std::optional<terminal::renderer::AtlasTextureScreenshot> readAtlas() = 0;

    virtual void inspect(std::ostream& output) const = 0;
};

/**
 * Helper-base class for render subsystems, such as
 * text renderer, decoration renderer, image fragment renderer, etc.
 */
class Renderable
{
  public:
    using TextureAtlas = RenderTarget::TextureAtlas;
    using DirectMappingAllocator = atlas::DirectMappingAllocator<RenderTileAttributes>;
    using DirectMapping = atlas::DirectMapping<RenderTileAttributes>;
    using AtlasTileAttributes = atlas::TileAttributes<RenderTileAttributes>;
    using TileSliceIndex = atlas::TileSliceIndex;

    explicit Renderable(GridMetrics const& gridMetrics);
    virtual ~Renderable() = default;

    virtual void clearCache() {}

    virtual void setRenderTarget(RenderTarget& renderTarget, DirectMappingAllocator& directMappingAllocator);
    virtual void setTextureAtlas(TextureAtlas& atlas) { _textureAtlas = &atlas; }

    TextureAtlas::TileCreateData createTileData(atlas::TileLocation tileLocation,
                                                std::vector<uint8_t> bitmap,
                                                atlas::Format bitmapFormat,
                                                ImageSize bitmapSize,
                                                RenderTileAttributes::X x,
                                                RenderTileAttributes::Y y,
                                                uint32_t fragmentShaderSelector);

    TextureAtlas::TileCreateData createTileData(atlas::TileLocation tileLocation,
                                                std::vector<uint8_t> bitmap,
                                                atlas::Format bitmapFormat,
                                                ImageSize bitmapSize,
                                                ImageSize renderBitmapSize,
                                                RenderTileAttributes::X x,
                                                RenderTileAttributes::Y y,
                                                uint32_t fragmentShaderSelector);

    Renderable::TextureAtlas::TileCreateData sliceTileData(
        Renderable::TextureAtlas::TileCreateData const& createData,
        TileSliceIndex sliceIndex,
        atlas::TileLocation tileLocation);

    atlas::RenderTile createRenderTile(atlas::RenderTile::X x,
                                       atlas::RenderTile::Y y,
                                       RGBAColor color,
                                       Renderable::AtlasTileAttributes const& attributes);

    void renderTile(atlas::RenderTile::X x,
                    atlas::RenderTile::Y y,
                    RGBAColor color,
                    Renderable::AtlasTileAttributes const& attributes);

    constexpr bool renderTargetAvailable() const noexcept { return _renderTarget; }

    RenderTarget& renderTarget() noexcept
    {
        assert(_renderTarget);
        return *_renderTarget;
    }

    TextureAtlas& textureAtlas() noexcept
    {
        assert(_textureAtlas);
        return *_textureAtlas;
    }

    atlas::AtlasBackend& textureScheduler() noexcept { return *_textureScheduler; }

    virtual void inspect(std::ostream& output) const = 0;

  protected:
    GridMetrics const& _gridMetrics;
    RenderTarget* _renderTarget = nullptr;
    TextureAtlas* _textureAtlas = nullptr;
    atlas::DirectMappingAllocator<RenderTileAttributes>* _directMappingAllocator = nullptr;
    atlas::AtlasBackend* _textureScheduler = nullptr;
};

inline Renderable::TextureAtlas::TileCreateData Renderable::createTileData(atlas::TileLocation tileLocation,
                                                                           std::vector<uint8_t> bitmap,
                                                                           atlas::Format bitmapFormat,
                                                                           ImageSize bitmapSize,
                                                                           RenderTileAttributes::X x,
                                                                           RenderTileAttributes::Y y,
                                                                           uint32_t fragmentShaderSelector)
{
    return createTileData(
        tileLocation, std::move(bitmap), bitmapFormat, bitmapSize, bitmapSize, x, y, fragmentShaderSelector);
}

} // namespace terminal::renderer

// {{{ fmt
namespace fmt
{
template <>
struct formatter<terminal::renderer::RenderTileAttributes>
{
    template <typename ParseContext>
    constexpr auto parse(ParseContext& ctx)
    {
        return ctx.begin();
    }
    template <typename FormatContext>
    auto format(terminal::renderer::RenderTileAttributes value, FormatContext& ctx)
    {
        return fmt::format_to(ctx.out(), "tile +{}x +{}y", value.x.value, value.y.value);
    }
};

template <>
struct formatter<terminal::renderer::atlas::TileAttributes<terminal::renderer::RenderTileAttributes>>
{
    template <typename ParseContext>
    constexpr auto parse(ParseContext& ctx)
    {
        return ctx.begin();
    }
    template <typename FormatContext>
    auto format(
        terminal::renderer::atlas::TileAttributes<terminal::renderer::RenderTileAttributes> const& value,
        FormatContext& ctx)
    {
        return fmt::format_to(
            ctx.out(), "(location {}; bitmap {}; {})", value.location, value.bitmapSize, value.metadata);
    }
};

} // namespace fmt
// }}}
