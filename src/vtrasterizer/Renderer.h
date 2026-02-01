// SPDX-License-Identifier: Apache-2.0
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

#include <crispy/StrongLRUHashtable.h>
#include <crispy/size.h>

#include <gsl/pointers>

#include <format>
#include <memory>
#include <span>
#include <vector>

namespace vtrasterizer
{

struct RenderCursor
{
    crispy::point position;
    vtbackend::CursorShape shape;
    int width;

    RenderCursor(crispy::point position, vtbackend::CursorShape shape, int width):
        position(position), shape(shape), width(width)
    {
    }
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
    Renderer(vtbackend::PageSize pageSize,
             FontDescriptions fontDescriptions,
             vtbackend::ColorPalette const& colorPalette,
             crispy::strong_hashtable_size atlasHashtableSlotCount,
             crispy::lru_capacity atlasTileCount,
             bool atlasDirectMapping,
             Decorator hyperlinkNormal,
             Decorator hyperlinkHover);

    [[nodiscard]] ImageSize cellSize() const noexcept { return _gridMetrics.cellSize; }

    /// Initializes the render and all render subsystems with the given RenderTarget
    /// and then informs all renderables about the newly created texture atlas.
    void setRenderTarget(RenderTarget& renderTarget);
    RenderTarget& renderTarget() noexcept { return *_renderTarget; }
    [[nodiscard]] bool hasRenderTarget() const noexcept { return _renderTarget != nullptr; }

    bool setFontSize(text::font_size fontSize);
    void updateFontMetrics();

    [[nodiscard]] FontDescriptions const& fontDescriptions() const noexcept { return _fontDescriptions; }
    void setFonts(FontDescriptions fontDescriptions);

    [[nodiscard]] GridMetrics const& gridMetrics() const noexcept { return _gridMetrics; }

    void setHyperlinkDecoration(Decorator normal, Decorator hover)
    {
        _decorationRenderer.setHyperlinkDecoration(normal, hover);
    }

    void setPageSize(vtbackend::PageSize screenSize) noexcept { _gridMetrics.pageSize = screenSize; }

    void setMargin(PageMargin margin) noexcept
    {
        if (_renderTarget)
            _renderTarget->setMargin(margin);
        _gridMetrics.pageMargin = margin;
    }

    /**
     * Renders the given @p terminal to the current OpenGL context.
     *
     * @param terminal       The terminal to render
     * @param pressureHint   Indicates whether or not this render will most likely be
     *                       updated right after again, allowing a few optimizations
     *                       to performa that reduce visual features as they are
     *                       CPU intensive but allow to render fast.
     *                       The user shall not notice that, because this frame
     *                       is known already to be updated right after again.
     */
    void render(vtbackend::Terminal& terminal, bool pressureHint);

    void discardImage(vtbackend::Image const& image);

    void clearCache();

    void inspect(std::ostream& textOutput) const;

    std::array<gsl::not_null<Renderable*>, 5> renderables()
    {
        return std::array<gsl::not_null<Renderable*>, 5> {
            &_backgroundRenderer, &_cursorRenderer, &_decorationRenderer, &_imageRenderer, &_textRenderer
        };
    }

    [[nodiscard]] std::array<gsl::not_null<Renderable const*>, 5> renderables() const
    {
        return std::array<gsl::not_null<Renderable const*>, 5> {
            &_backgroundRenderer, &_cursorRenderer, &_decorationRenderer, &_imageRenderer, &_textRenderer
        };
    }

    /// Returns the index of the first cell whose line offset >= @p statusLineBoundary.
    ///
    /// @param cells                Sorted vector of render cells ordered by line offset.
    /// @param statusLineBoundary   Line count that separates main display from the status line.
    /// @return Index into @p cells at the partition point; equals cells.size() if all cells
    ///         belong to the main display.
    [[nodiscard]] static size_t findCellPartitionPoint(std::vector<vtbackend::RenderCell> const& cells,
                                                       vtbackend::LineCount statusLineBoundary);

    /// Returns the index of the first line whose offset >= @p statusLineBoundary.
    ///
    /// @param lines                Sorted vector of render lines ordered by line offset.
    /// @param statusLineBoundary   Line count that separates main display from the status line.
    /// @return Index into @p lines at the partition point; equals lines.size() if all lines
    ///         belong to the main display.
    [[nodiscard]] static size_t findLinePartitionPoint(std::vector<vtbackend::RenderLine> const& lines,
                                                       vtbackend::LineCount statusLineBoundary);

  private:
    void configureTextureAtlas();

    /// Renders a span of cells to the background, decoration, text, and image renderers.
    ///
    /// @param cells  Contiguous sub-range of RenderCell entries to render.
    void renderCells(std::span<vtbackend::RenderCell const> cells);

    /// Renders a span of lines to the background, decoration, and text renderers.
    ///
    /// @param lines  Contiguous sub-range of RenderLine entries to render.
    void renderLines(std::span<vtbackend::RenderLine const> lines);

    void executeImageDiscards();

    crispy::strong_hashtable_size _atlasHashtableSlotCount;
    crispy::lru_capacity _atlasTileCount;
    bool _atlasDirectMapping;

    RenderTarget* _renderTarget = nullptr;

    Renderable::DirectMappingAllocator _directMappingAllocator;
    std::unique_ptr<Renderable::TextureAtlas> _textureAtlas;

    FontDescriptions _fontDescriptions;
    std::unique_ptr<text::shaper> _textShaper;
    FontKeys _fonts;

    GridMetrics _gridMetrics;

    std::mutex _imageDiscardLock;                       //!< Lock guard for accessing _discardImageQueue.
    std::vector<vtbackend::ImageId> _discardImageQueue; //!< List of images to be discarded.

    BackgroundRenderer _backgroundRenderer;
    ImageRenderer _imageRenderer;
    TextRenderer _textRenderer;
    DecorationRenderer _decorationRenderer;
    CursorRenderer _cursorRenderer;
};

} // namespace vtrasterizer
