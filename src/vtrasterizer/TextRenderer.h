// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <vtbackend/Color.h>
#include <vtbackend/RenderBuffer.h>
#include <vtbackend/Screen.h>

#include <vtrasterizer/BoxDrawingRenderer.h>
#include <vtrasterizer/FontDescriptions.h>
#include <vtrasterizer/RenderTarget.h>
#include <vtrasterizer/TextureAtlas.h>

#include <text_shaper/font.h>
#include <text_shaper/shaper.h>

#include <crispy/FNV.h>
#include <crispy/point.h>
#include <crispy/size.h>

#include <gsl/span>
#include <gsl/span_ext>

#include <functional>
#include <list>
#include <memory>
#include <unordered_map>
#include <vector>

#include "crispy/StrongHash.h"
#include "crispy/StrongLRUHashtable.h"
#include <libunicode/convert.h>
#include <libunicode/run_segmenter.h>

namespace terminal::rasterizer
{

text::font_locator& createFontLocator(FontLocatorEngine engine);

struct FontKeys
{
    text::font_key regular;
    text::font_key bold;
    text::font_key italic;
    text::font_key boldItalic;
    text::font_key emoji;
};

struct TextRendererEvents
{
    virtual ~TextRendererEvents() = default;

    virtual void onBeforeRenderingText() = 0;
    virtual void onAfterRenderingText() = 0;
};

/// Text Rendering Pipeline
class TextRenderer: public Renderable
{
  public:
    TextRenderer(GridMetrics const& gridMetrics,
                 text::shaper& textShaper,
                 FontDescriptions& fontDescriptions,
                 FontKeys const& fontKeys,
                 TextRendererEvents& eventHandler);

    void setRenderTarget(RenderTarget& renderTarget, DirectMappingAllocator& directMappingAllocator) override;
    void setTextureAtlas(TextureAtlas& atlas) override;

    void inspect(std::ostream& textOutput) const override;

    void clearCache() override;

    void updateFontMetrics();

    void setPressure(bool pressure) noexcept { _pressure = pressure; }

    /// Must be invoked before a new terminal frame is rendered.
    void beginFrame();

    /// Renders a given terminal's grid cell that has been
    /// transformed into a RenderCell.
    void renderCell(RenderCell const& cell);

    void renderCell(CellLocation position,
                    std::u32string_view graphemeCluster,
                    TextStyle textStyle,
                    RGBColor foregroundColor);

    void renderLine(RenderLine const& renderLine);

    /// Must be invoked when rendering the terminal's text has finished for this frame.
    void endFrame();

  private:
    void initializeDirectMapping();

    /// Puts a sequence of codepoints that belong to the same grid cell at @p _pos
    /// at the end of the currently filled line.
    void appendCellTextToClusterGroup(std::u32string_view codepoints, TextStyle style, RGBColor color);

    /// Gets the text shaping result of the current text cluster group
    text::shape_result const& getOrCreateCachedGlyphPositions(crispy::strong_hash hash);
    text::shape_result createTextShapedGlyphPositions();
    text::shape_result shapeTextRun(unicode::run_segmenter::range const& run);
    void flushTextClusterGroup();

    AtlasTileAttributes const* getOrCreateRasterizedMetadata(crispy::strong_hash const& hash,
                                                             text::glyph_key const& glyphKey,
                                                             unicode::PresentationStyle presentationStyle);

    /**
     * Creates (and rasterizes) a single glyph and returns its
     * render tile attributes required for the render step.
     */
    std::optional<TextureAtlas::TileCreateData> createSlicedRasterizedGlyph(
        atlas::TileLocation tileLocation,
        text::glyph_key const& glyphKey,
        unicode::PresentationStyle presentation,
        crispy::strong_hash const& hash);

    std::optional<TextureAtlas::TileCreateData> createRasterizedGlyph(
        atlas::TileLocation tileLocation,
        text::glyph_key const& glyphKey,
        unicode::PresentationStyle presentation);

    void restrictToTileSize(TextureAtlas::TileCreateData& tileCreateData);

    crispy::point applyGlyphPositionToPen(crispy::point pen,
                                          AtlasTileAttributes const& tileAttributes,
                                          text::glyph_position const& gpos) const noexcept;

    void renderRasterizedGlyph(crispy::point pen, RGBAColor color, AtlasTileAttributes const& attributes);

    // general properties
    //
    TextRendererEvents& _textRendererEvents;
    FontDescriptions& _fontDescriptions;
    FontKeys const& _fonts;

    // performance optimizations
    //
    bool _pressure = false;

    using ShapingResultCache = crispy::strong_lru_hashtable<text::shape_result>;
    using ShapingResultCachePtr = ShapingResultCache::ptr;

    ShapingResultCachePtr _textShapingCache;
    // TODO: make unique_ptr, get owned, export cref for other users in Renderer impl.
    text::shaper& _textShaper;

    DirectMapping _directMapping {};

    // Maps from glyph index to tile index.
    std::vector<uint32_t> _directMappedGlyphKeyToTileIndex {};

    [[nodiscard]] bool isGlyphDirectMapped(text::glyph_key const& glyph) const noexcept
    {
        return _directMapping                  // Is direct mapping enabled?
               && glyph.font == _fonts.regular // Only regular font is direct-mapped for now.
               && glyph.index.value < _directMappedGlyphKeyToTileIndex.size()
               && _directMappedGlyphKeyToTileIndex[glyph.index.value] != 0;
    }

    AtlasTileAttributes const* ensureRasterizedIfDirectMapped(text::glyph_key const& glyphKey);

    // sub-renderer
    //
    BoxDrawingRenderer _boxDrawingRenderer;

    // work-data for the current text cluster group
    struct TextClusterGroup
    {
        // pen-start position of this text group
        crispy::point initialPenPosition {};

        // uniform text style for this text group
        TextStyle style = TextStyle::Invalid;

        // uniform text color for this text group
        RGBColor color {};

        // codepoints within this text group with
        // uniform unicode properties (script, language, direction).
        std::vector<char32_t> codepoints;

        // cluster indices for each codepoint
        std::vector<unsigned> clusters;

        // number of grid cells processed
        int cellCount = 0; // FIXME: EA width vs actual cells

        void resetAndMovePenForward(int penIncrementInX)
        {
            codepoints.clear();
            clusters.clear();
            cellCount = 0;
            initialPenPosition.x += penIncrementInX;
        }
    };
    TextClusterGroup _textClusterGroup {};

    bool _textStartFound = false;
    bool _updateInitialPenPosition = false;
};

} // namespace terminal::rasterizer
