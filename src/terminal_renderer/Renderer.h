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

#include <terminal/ColorPalette.h>
#include <terminal/Image.h>
#include <terminal/Terminal.h>

#include <terminal_renderer/BackgroundRenderer.h>
#include <terminal_renderer/CursorRenderer.h>
#include <terminal_renderer/DecorationRenderer.h>
#include <terminal_renderer/Decorator.h>
#include <terminal_renderer/GridMetrics.h>
#include <terminal_renderer/ImageRenderer.h>
#include <terminal_renderer/RenderTarget.h>
#include <terminal_renderer/TextRenderer.h>

#include <crispy/size.h>

#include <fmt/format.h>

#include <chrono>
#include <memory>
#include <utility>
#include <vector>

namespace terminal::renderer
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

    ImageSize cellSize() const noexcept { return gridMetrics_.cellSize; }

    /// Initializes the render and all render subsystems with the given RenderTarget
    /// and then informs all renderables about the newly created texture atlas.
    void setRenderTarget(RenderTarget& renderTarget);
    RenderTarget& renderTarget() noexcept { return *_renderTarget; }

    void setBackgroundOpacity(terminal::Opacity _opacity) { backgroundOpacity_ = _opacity; }
    terminal::Opacity backgroundOpacity() const noexcept { return backgroundOpacity_; }

    bool setFontSize(text::font_size _fontSize);
    void updateFontMetrics();

    FontDescriptions const& fontDescriptions() const noexcept { return fontDescriptions_; }
    void setFonts(FontDescriptions _fontDescriptions);

    GridMetrics const& gridMetrics() const noexcept { return gridMetrics_; }

    void setHyperlinkDecoration(Decorator _normal, Decorator _hover)
    {
        decorationRenderer_.setHyperlinkDecoration(_normal, _hover);
    }

    void setPageSize(PageSize _screenSize) noexcept { gridMetrics_.pageSize = _screenSize; }

    void setMargin(PageMargin _margin) noexcept
    {
        if (_renderTarget)
            _renderTarget->setMargin(_margin);
        gridMetrics_.pageMargin = _margin;
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
            backgroundRenderer_, cursorRenderer_, decorationRenderer_, imageRenderer_, textRenderer_
        };
    }

    std::array<std::reference_wrapper<Renderable const>, 5> renderables() const
    {
        return std::array<std::reference_wrapper<Renderable const>, 5> {
            backgroundRenderer_, cursorRenderer_, decorationRenderer_, imageRenderer_, textRenderer_
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

    Renderable::DirectMappingAllocator directMappingAllocator_;
    std::unique_ptr<Renderable::TextureAtlas> textureAtlas_;

    FontDescriptions fontDescriptions_;
    std::unique_ptr<text::shaper> textShaper_;
    FontKeys fonts_;

    GridMetrics gridMetrics_;

    ColorPalette const& colorPalette_;
    Opacity backgroundOpacity_;

    std::mutex imageDiscardLock_;            //!< Lock guard for accessing discardImageQueue_.
    std::vector<ImageId> discardImageQueue_; //!< List of images to be discarded.

    BackgroundRenderer backgroundRenderer_;
    ImageRenderer imageRenderer_;
    TextRenderer textRenderer_;
    DecorationRenderer decorationRenderer_;
    CursorRenderer cursorRenderer_;
};

} // namespace terminal::renderer
