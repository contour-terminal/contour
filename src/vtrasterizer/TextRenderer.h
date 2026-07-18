// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <vtbackend/Color.h>
#include <vtbackend/RenderBuffer.h>
#include <vtbackend/Screen.h>

#include <vtrasterizer/BoxDrawingRenderer.h>
#include <vtrasterizer/FontDescriptions.h>
#include <vtrasterizer/GlyphScaling.h>
#include <vtrasterizer/RenderTarget.h>
#include <vtrasterizer/TextClusterGrouper.h>
#include <vtrasterizer/TextureAtlas.h>

#include <text_shaper/font.h>
#include <text_shaper/shaper.h>

#include <crispy/FNV.h>
#include <crispy/StrongHash.h>
#include <crispy/StrongLRUHashtable.h>
#include <crispy/point.h>
#include <crispy/size.h>

#include <libunicode/convert.h>
#include <libunicode/run_segmenter.h>

#include <gsl/pointers>
#include <gsl/span>
#include <gsl/span_ext>

#include <vector>

namespace vtrasterizer
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
class TextRenderer: public Renderable, public TextClusterGrouper::Events
{
    friend class TextRendererTest;

  public:
    TextRenderer(GridMetrics const& gridMetrics,
                 text::shaper& textShaper,
                 FontDescriptions& fontDescriptions,
                 FontKeys const& fontKeys,
                 TextRendererEvents& eventHandler,
                 GlyphScaler const& glyphScaler = defaultGlyphScaler());

    /// The strategy used when no other is injected. @see GlyphScalingMethod.
    [[nodiscard]] static GlyphScaler const& defaultGlyphScaler() noexcept;

    void setRenderTarget(RenderTarget& renderTarget, DirectMappingAllocator& directMappingAllocator) override;
    void setTextureAtlas(TextureAtlas& atlas) override;

    void setSmoothScrollOffset(int offset) noexcept override
    {
        Renderable::setSmoothScrollOffset(offset);
        _boxDrawingRenderer.setSmoothScrollOffset(offset);
    }

    void inspect(std::ostream& textOutput) const override;

    void clearCache() override;

    void updateFontMetrics();

    void setPressure(bool pressure) noexcept { _pressure = pressure; }

    /// Must be invoked before a new terminal frame is rendered.
    void beginFrame();

    /// Renders a given terminal's grid cell that has been
    /// transformed into a RenderCell.
    void renderCell(vtbackend::RenderCell const& cell);

    void renderLine(vtbackend::RenderLine const& renderLine);

    /// Must be invoked when rendering the terminal's text has finished for this frame.
    void endFrame();

  private:
    void initializeDirectMapping();

    void renderTextGroup(std::u32string_view codepoints,
                         gsl::span<unsigned> clusters,
                         vtbackend::CellLocation initialPenPosition,
                         TextStyle style,
                         vtbackend::RGBColor color,
                         vtbackend::LineFlags flags,
                         vtbackend::CellScale const& scale) override;

    bool renderBoxDrawingCell(vtbackend::CellLocation position,
                              char32_t codepoint,
                              vtbackend::RGBColor foregroundColor,
                              vtbackend::LineFlags flags) override;

    /// Gets the text shaping result of the current text cluster group
    text::shape_result const& getOrCreateCachedGlyphPositions(crispy::strong_hash hash,
                                                              std::u32string_view codepoints,
                                                              gsl::span<unsigned> clusters,
                                                              TextStyle style);
    text::shape_result createTextShapedGlyphPositions(std::u32string_view codepoints,
                                                      gsl::span<unsigned> clusters,
                                                      TextStyle style);
    text::shape_result shapeTextRun(unicode::run_segmenter::range const& run,
                                    std::u32string_view codepoints,
                                    gsl::span<unsigned> clusters,
                                    TextStyle style);

    /// @param blockScale How many cells tall the block this glyph belongs to is (`OSC 66` `s=`).
    ///                   Bounds the oversize clamps below, which would otherwise squash a glyph
    ///                   rasterized for a tall block back down to a single cell.
    AtlasTileAttributes const* getOrCreateRasterizedMetadata(crispy::strong_hash const& hash,
                                                             text::glyph_key const& glyphKey,
                                                             unicode::PresentationStyle presentationStyle,
                                                             uint8_t blockScale = 1);

    /**
     * Creates (and rasterizes) a single glyph and returns its
     * render tile attributes required for the render step.
     */
    std::optional<TextureAtlas::TileCreateData> createSlicedRasterizedGlyph(
        atlas::TileLocation tileLocation,
        text::glyph_key const& glyphKey,
        unicode::PresentationStyle presentation,
        crispy::strong_hash const& hash,
        uint8_t blockScale = 1);

    std::optional<TextureAtlas::TileCreateData> createRasterizedGlyph(atlas::TileLocation tileLocation,
                                                                      text::glyph_key const& glyphKey,
                                                                      unicode::PresentationStyle presentation,
                                                                      uint8_t blockScale = 1);

    void restrictToTileSize(TextureAtlas::TileCreateData& tileCreateData);

    crispy::point applyGlyphPositionToPen(crispy::point pen,
                                          AtlasTileAttributes const& tileAttributes,
                                          text::glyph_position const& gpos) const noexcept;

    void renderRasterizedGlyph(crispy::point pen,
                               vtbackend::RGBAColor color,
                               AtlasTileAttributes const& attributes);

    // general properties
    //
    TextClusterGrouper _textClusterGrouper;
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

    /// How a glyph is enlarged for a scaled text-sizing block. Injected so that a second strategy --
    /// re-rasterizing at the larger point size for a crisper result -- is a new implementation
    /// rather than an edit here. @see GlyphScaler.
    gsl::not_null<GlyphScaler const*> _glyphScaler;
};

} // namespace vtrasterizer
