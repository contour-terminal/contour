// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <vtbackend/Color.hpp>
#include <vtbackend/RenderBuffer.hpp>
#include <vtbackend/Screen.hpp>

#include <vtrasterizer/BoxDrawingRenderer.hpp>
#include <vtrasterizer/FontDescriptions.hpp>
#include <vtrasterizer/GlyphScaling.hpp>
#include <vtrasterizer/GlyphSlicing.hpp>
#include <vtrasterizer/RenderTarget.hpp>
#include <vtrasterizer/TextClusterGrouper.hpp>
#include <vtrasterizer/TextureAtlas.hpp>

#include <text_shaper/Font.hpp>
#include <text_shaper/Shaper.hpp>

#include <crispy/FNV.hpp>
#include <crispy/Point.hpp>
#include <crispy/Size.hpp>
#include <crispy/StrongHash.hpp>
#include <crispy/StrongLRUHashtable.hpp>

#include <libunicode/convert.h>
#include <libunicode/run_segmenter.h>

#include <gsl/pointers>
#include <gsl/span>
#include <gsl/span_ext>

#include <vector>

namespace vtrasterizer
{

text::FontLocator& createFontLocator(FontLocatorEngine engine);

struct FontKeys
{
    text::FontKey regular;
    text::FontKey bold;
    text::FontKey italic;
    text::FontKey boldItalic;
    text::FontKey emoji;
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
                 text::Shaper& textShaper,
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

    /// Rebinds this renderer onto a different text shaper.
    ///
    /// The narrow exception to "configuration at construction time": the owning Renderer replaces its
    /// shaper when the shaping engine changes, and everything cached here -- shaped runs, resolved
    /// FontKeys, rasterized tiles -- names glyphs of the OLD shaper. Rebinding without dropping those
    /// would resolve a stale FontKey inside the new shaper; the caller must therefore reload the font
    /// keys and call updateFontMetrics() (which clears the caches) after this, exactly as
    /// Renderer::applyFontDescriptions() does.
    /// @param textShaper The shaper to shape and rasterize through from now on.
    void setTextShaper(text::Shaper& textShaper) noexcept { _textShaper = &textShaper; }

    void updateFontMetrics();

    /// The shaping-cache key for @p text in @p style under the renderer's CURRENT fonts and size.
    ///
    /// Exposed so the property the key exists for can be asserted directly: a font or size change must
    /// change the key, so entries cached against the previous font become unreachable rather than
    /// resolvable. The cached values are shape_results naming a specific font, and resolving one after
    /// that font has been unloaded throws inside the shaper.
    /// @param text  The codepoints being shaped.
    /// @param style The text style the run is drawn in.
    /// @return The cache key.
    [[nodiscard]] crispy::StrongHash shapingCacheKeyFor(std::u32string_view text,
                                                        TextStyle style) const noexcept;

    void setPressure(bool pressure) noexcept { _pressure = pressure; }

    /// Must be invoked before a new terminal frame is rendered.
    void beginFrame();

    /// Renders a given terminal's grid cell that has been
    /// transformed into a RenderCell.
    void renderCell(vtbackend::RenderCell const& cell);

    void renderLine(vtbackend::RenderLine const& renderLine);

    /// Must be invoked when rendering the terminal's text has finished for this frame.
    void endFrame();

    // TextClusterGrouper::Events overrides -- public, as in the base interface.
    //
    void renderTextGroup(std::u32string_view codepoints,
                         gsl::span<unsigned> clusters,
                         vtbackend::CellLocation initialPenPosition,
                         TextStyle style,
                         vtbackend::RGBColor color,
                         vtbackend::LineFlags flags,
                         vtbackend::GlyphSizing const& sizing) override;

    bool renderBoxDrawingCell(vtbackend::CellLocation position,
                              char32_t codepoint,
                              vtbackend::RGBColor foregroundColor,
                              vtbackend::LineFlags flags) override;

  private:
    void initializeDirectMapping();

    /// Gets the text shaping result of the current text cluster group
    text::ShapeResult const& getOrCreateCachedGlyphPositions(crispy::StrongHash hash,
                                                             std::u32string_view codepoints,
                                                             gsl::span<unsigned> clusters,
                                                             TextStyle style);
    text::ShapeResult createTextShapedGlyphPositions(std::u32string_view codepoints,
                                                     gsl::span<unsigned> clusters,
                                                     TextStyle style);
    text::ShapeResult shapeTextRun(unicode::run_segmenter::range const& run,
                                   std::u32string_view codepoints,
                                   gsl::span<unsigned> clusters,
                                   TextStyle style);

    /// One text-sizing block's raster, sized to whole cells so that cutting it into atlas tiles is
    /// exact. @see buildBlockCanvas.
    struct BlockCanvas
    {
        vtbackend::ImageSize size {};
        // NB: atlas::Format has no zero enumerator; Red is the narrowest valid one.
        atlas::Format format = atlas::Format::Red;
        uint32_t fragmentShaderSelector {};
        size_t components {};
        std::vector<uint8_t> bitmap {};
    };

    /// Draws one row of a scaled block, one cell-sized tile per column.
    ///
    /// Separate from the ordinary path because it is a different shape of work, and must stay so:
    /// unscaled text is the overwhelming majority of what a terminal draws and must not pay for any
    /// of this.
    void renderBlockGroup(text::ShapeResult const& glyphPositions,
                          crispy::Point pen,
                          vtbackend::RGBColor color,
                          vtbackend::LineFlags lineFlags,
                          vtbackend::GlyphSizing const& sizing,
                          GlyphScaleAdjustment adjustment);

    /// @param cluster one grapheme cluster: a base glyph followed by its zero-advance marks. They
    ///                share one canvas, or a Devanagari conjunct is torn into its pieces.
    std::optional<BlockCanvas> buildBlockCanvas(std::span<text::GlyphPosition const> cluster,
                                                vtbackend::CellScale const& cellScale,
                                                GlyphScaleAdjustment adjustment,
                                                int cellsAtOneX);

    std::optional<TextureAtlas::TileCreateData> createBlockTile(BlockCanvas const* canvas,
                                                                atlas::TileLocation tileLocation,
                                                                uint32_t column,
                                                                uint32_t band);

    std::optional<text::RasterizedGlyph> rasterizeAtBlockSize(text::GlyphKey const& glyphKey,
                                                              GlyphScaleAdjustment adjustment);

    AtlasTileAttributes const* getOrCreateRasterizedMetadata(crispy::StrongHash const& hash,
                                                             text::GlyphKey const& glyphKey,
                                                             unicode::PresentationStyle presentationStyle);

    /**
     * Creates (and rasterizes) a single glyph and returns its
     * render tile attributes required for the render step.
     */
    std::optional<TextureAtlas::TileCreateData> createSlicedRasterizedGlyph(
        atlas::TileLocation tileLocation,
        text::GlyphKey const& glyphKey,
        unicode::PresentationStyle presentation,
        crispy::StrongHash const& hash);

    std::optional<TextureAtlas::TileCreateData> createRasterizedGlyph(
        atlas::TileLocation tileLocation,
        text::GlyphKey const& glyphKey,
        unicode::PresentationStyle presentation,
        GlyphWidthPolicy widthPolicy = GlyphWidthPolicy::Sliced);

    void restrictToTileSize(TextureAtlas::TileCreateData& tileCreateData);

    crispy::Point applyGlyphPositionToPen(crispy::Point pen,
                                          AtlasTileAttributes const& tileAttributes,
                                          text::GlyphPosition const& gpos) const noexcept;

    void renderRasterizedGlyph(crispy::Point pen,
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

    using ShapingResultCache = crispy::StrongLRUHashtable<text::ShapeResult>;
    using ShapingResultCachePtr = ShapingResultCache::Ptr;

    ShapingResultCachePtr _textShapingCache;
    /// Holds a shaping result the cache refused (an empty result for a non-empty run, i.e. a shaper
    /// failure) for as long as the caller needs it. @see getOrCreateCachedGlyphPositions().
    text::ShapeResult _uncachedShapeResult {};
    // TODO: make unique_ptr, get owned, export cref for other users in Renderer impl.
    // A pointer rather than a reference so setTextShaper() can rebind it; the owner (Renderer)
    // replaces the shaper wholesale when the shaping engine changes, and a reference member would
    // then name a destroyed object for the rest of the renderer's life.
    gsl::not_null<text::Shaper*> _textShaper;

    DirectMapping _directMapping {};

    // Maps from glyph index to tile index.
    std::vector<uint32_t> _directMappedGlyphKeyToTileIndex {};

    [[nodiscard]] bool isGlyphDirectMapped(text::GlyphKey const& glyph) const noexcept
    {
        return _directMapping                  // Is direct mapping enabled?
               && glyph.font == _fonts.regular // Only regular font is direct-mapped for now.
               && glyph.index.value < _directMappedGlyphKeyToTileIndex.size()
               && _directMappedGlyphKeyToTileIndex[glyph.index.value] != 0;
    }

    AtlasTileAttributes const* ensureRasterizedIfDirectMapped(text::GlyphKey const& glyphKey);

    // sub-renderer
    //
    BoxDrawingRenderer _boxDrawingRenderer;

    /// How a glyph is enlarged for a scaled text-sizing block. Injected so that a second strategy --
    /// re-rasterizing at the larger point size for a crisper result -- is a new implementation
    /// rather than an edit here. @see GlyphScaler.
    gsl::not_null<GlyphScaler const*> _glyphScaler;
};

} // namespace vtrasterizer
