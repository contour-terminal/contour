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

#include <vtbackend/ColorPalette.h>
#include <vtbackend/Image.h>
#include <vtbackend/Terminal.h>

#include <vtrasterizer/BackgroundRenderer.h>
#include <vtrasterizer/CursorRenderer.h>
#include <vtrasterizer/DecorationRenderer.h>
#include <vtrasterizer/Decorator.h>
#include <vtrasterizer/GridMetrics.h>
#include <vtrasterizer/ImageRenderer.h>
#include <vtrasterizer/RenderTarget.h>
#include <vtrasterizer/TextRenderer.h>

#include <crispy/size.h>

#include <fmt/format.h>

#include <chrono>
#include <memory>
#include <utility>
#include <vector>

namespace terminal::rasterizer
{

struct RenderCursor
{
    crispy::Point position;
    CursorShape shape;
    int width;
};

/**
 * Renders a terminal's screen to the current OpenGL context.
 */
class Renderer
{
  public:
    /** Constructs a Renderer instances.
     *
     * @p fonts              Reference to the set of loaded fonts to be used for rendering text.
     * @p colorPalette       User-configurable color profile to use to map terminal colors to.
     * @p projectionMatrix   Projection matrix to apply to the rendered scene when rendering the screen.
     * @p atlasDirectMapping Indicates whether or not direct mapped tiles are allowed.
     * @p atlasTileCount     Number of tiles guaranteed to be available in LRU cache.
     */
    Renderer(PageSize screenSize,
             FontDescriptions fontDescriptions,
             ColorPalette const& colorPalette,
             Opacity backgroundOpacity,
             crispy::StrongHashtableSize atlasHashtableSlotCount,
             crispy::LRUCapacity atlasTileCount,
             bool atlasDirectMapping,
             Decorator hyperlinkNormal,
             Decorator hyperlinkHover);

    [[nodiscard]] ImageSize cellSize() const noexcept { return _gridMetrics.cellSize; }

    /// Initializes the render and all render subsystems with the given RenderTarget
    /// and then informs all renderables about the newly created texture atlas.
    void setRenderTarget(RenderTarget& renderTarget);
    RenderTarget& renderTarget() noexcept { return *_renderTarget; }
    [[nodiscard]] bool hasRenderTarget() const noexcept { return _renderTarget != nullptr; }

    void setBackgroundOpacity(terminal::Opacity _opacity) { _backgroundOpacity = _opacity; }
    [[nodiscard]] terminal::Opacity backgroundOpacity() const noexcept { return _backgroundOpacity; }

    bool setFontSize(text::font_size _fontSize);
    void updateFontMetrics();

    [[nodiscard]] FontDescriptions const& fontDescriptions() const noexcept { return _fontDescriptions; }
    void setFonts(FontDescriptions _fontDescriptions);

    [[nodiscard]] GridMetrics const& gridMetrics() const noexcept { return _gridMetrics; }

    void setHyperlinkDecoration(Decorator normal, Decorator hover)
    {
        _decorationRenderer.setHyperlinkDecoration(normal, hover);
    }

    void setPageSize(PageSize screenSize) noexcept { _gridMetrics.pageSize = screenSize; }

    void setMargin(PageMargin margin) noexcept
    {
        if (_renderTarget)
            _renderTarget->setMargin(margin);
        _gridMetrics.pageMargin = margin;
    }

    /**
     * Renders the given @p _terminal to the current OpenGL context.
     *
     * @p _now The time hint to use when rendering the eventually blinking cursor.
     */
    uint64_t render(Terminal& _terminal, bool _pressure);

    void discardImage(Image const& _image);

    void clearCache();

    void inspect(std::ostream& _textOutput) const;

    std::array<std::reference_wrapper<Renderable>, 5> renderables()
    {
        return std::array<std::reference_wrapper<Renderable>, 5> {
            _backgroundRenderer, _cursorRenderer, _decorationRenderer, _imageRenderer, _textRenderer
        };
    }

    [[nodiscard]] std::array<std::reference_wrapper<Renderable const>, 5> renderables() const
    {
        return std::array<std::reference_wrapper<Renderable const>, 5> {
            _backgroundRenderer, _cursorRenderer, _decorationRenderer, _imageRenderer, _textRenderer
        };
    }

  private:
    void configureTextureAtlas();
    void renderCells(std::vector<RenderCell> const& _renderableCells);
    void renderLines(std::vector<RenderLine> const& renderableLines);
    void executeImageDiscards();

    crispy::StrongHashtableSize _atlasHashtableSlotCount;
    crispy::LRUCapacity _atlasTileCount;
    bool _atlasDirectMapping;

    RenderTarget* _renderTarget;

    Renderable::DirectMappingAllocator _directMappingAllocator;
    std::unique_ptr<Renderable::TextureAtlas> _textureAtlas;

    FontDescriptions _fontDescriptions;
    std::unique_ptr<text::shaper> _textShaper;
    FontKeys _fonts;

    GridMetrics _gridMetrics;

    ColorPalette const& _colorPalette;
    Opacity _backgroundOpacity;

    std::mutex _imageDiscardLock;            //!< Lock guard for accessing _discardImageQueue.
    std::vector<ImageId> _discardImageQueue; //!< List of images to be discarded.

    BackgroundRenderer _backgroundRenderer;
    ImageRenderer _imageRenderer;
    TextRenderer _textRenderer;
    DecorationRenderer _decorationRenderer;
    CursorRenderer _cursorRenderer;
};

} // namespace terminal::rasterizer
