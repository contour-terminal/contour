// SPDX-License-Identifier: Apache-2.0
/*
### abstract control flow of a single frame

    beginFrame
        renderCell...
            appendCellTextToClusterGroup
            flushTextClusterGroup?
                getOrCreateCachedGlyphPositions
                getOrCreateRasterizedMetadata
                    createRasterizedGlyph
                        upload each glyph tile
                renderRasterizedGlyph
                    render each glyph tile
    endFrame
        &flushTextClusterGroup...

### How ligatures are being rendered:

    '<=' takes 2 characters (grapheme clusters) '<' and '='.
    Text shaping yields 2 glyph positions.
    first glyph position just moves the cursor with an empty glyph
    second glyph renders an overlarge glyph and offsets x negatively to the left.

    A ligature of 3 characters, say '==>', 3 grapheme clusters,
    yields 3 glyph positions during text shaping.
    All but the last glyphs will just move the pen and render an empty glyph.
    The last glyph will render a very horizontally large glyph
    with a negative x-offset to walk back before starting to paint.

    A ligature of 4 and more characters are treated analogue.

### How emoji are rendered

    U+1F600 is the standard smiley, a single grapheme cluster.
    It has an east asian width of 2.
    Text shaping yields 1 glyph position with x-advance being twice as large (2 grid cells).
    The glyph renders with overlarge width.

### Dealing with wide glyphs

When calling getOrCreateRasterizedMetadata(), we will know
whether the glyph fits the grid or if we need to start iterating over
N (or N minus 1) following tiles to complete the draw.

How to compute number of required tiles:

    int requiredTileCount(TileAttributes<RenderTileAttributes> const& tile) {
        return floor(double(tile.bitmapSize.width) / double(tileSize.width));
    }

But always doing this computation would be expensive. We can store an additional
small integer in the tile attributes for the sake of host memory resource usage.

    auto const metadata = getOrCreateRasterizedMetadata(); // <- uploads all sub-tiles
    for (int tileIndex = 0; tileIndex < metadata.requiredTiles; ++tileIndex)
        renderRasterizedGlyph(metadata, tileIndex) // <- render each sub-tile

    // renderRasterizedGlyph:
    hash = metadata.hash * tileIndex;
    if (subTileMetadata = textureAtlas.try_get(hash))
        render(*subTileMetadata);

### reserved glyphs handling

99% of the text is US-ASCII. We can reserve some slots in the texture
atlas so that when they're to be rendered, there is no need for the LRU-action.

But in order to not accidentally eliminate programming ligatures

    such as programming ligatures <= == != >= == === !== ...)

we need to add an extra indirection.

Initializing  the reserved glyph slots:

    constexpr auto FirstReservedChar = 0x21;
    constexpr auto LastReservedChar = 0x7E;
    for (char ch = FirstReservedChar; ch <= LastReservedChar; ++ch)
    {
        glyph_key glyphKey = get_glyph_key(ch);
        auto reservedSlotIndex = ch - FirstChar
        reservedGlyphKeyMapping[glyphKey.index.value] = reservedSlotIndex;
        setDirectMapping(reservedSlotIndex, getOrCreateRasterizedMetadata(glyphKey));
    }

Making use of reserved glyph slots

    auto getOrCreateRasterizedMetadata(text::glyph_key glyphKey,
                                       unicode::PresentationStyle presentationStyle)
    {
        if (isReserved(glyphKey))
            return textureAtlas().directMapped(reservedIndex(glyphKey));
        // else: standard implementation
    }

    bool isReserved(glyph_key glyphKey) const noexcept
    {
        // reservedGlyphKeyMapping should therefore be a sorted vector.
        // we could then do binary search OR we lookup O(1) on a large enough vector (space issues?).
        return reservedGlyphKeyMapping.contains_key(glyphKey.index.value);
    }

    uint32_t reservedIndex() const noexcept
    {
        return reservedGlyphKeyMapping.at(glyphKey.index.value);
    }
*/

#include <vtbackend/logging.h>
#include <vtbackend/primitives.h>

#include <vtrasterizer/BoxDrawingRenderer.h>
#include <vtrasterizer/GlyphAdvance.h>
#include <vtrasterizer/GlyphSlicing.h>
#include <vtrasterizer/GridMetrics.h>
#include <vtrasterizer/TextRenderer.h>
#include <vtrasterizer/shared_defines.h>
#include <vtrasterizer/utils.h>

#include <text_shaper/font_locator_provider.h>
#include <text_shaper/fontconfig_locator.h>
#include <text_shaper/mock_font_locator.h>

#include <crispy/StrongHash.h>
#include <crispy/StrongLRUHashtable.h>
#include <crispy/point.h>

#include <algorithm>
#include <ranges>

using crispy::point;
using crispy::strong_hash;

using unicode::out;

using std::array;
using std::get;
using std::make_unique;
using std::max;
using std::min;
using std::move;
using std::nullopt;
using std::optional;
using std::ostream;
using std::pair;
using std::u32string;
using std::u32string_view;
using std::unique_ptr;
using std::vector;

using namespace std::placeholders;
using namespace std::string_view_literals;

namespace vtrasterizer
{

using text::locatorLog;

namespace
{
    constexpr auto FirstReservedChar = char32_t { 0x21 };
    constexpr auto LastReservedChar = char32_t { 0x7E };
    constexpr auto DirectMappedCharsCount = LastReservedChar - FirstReservedChar + 1;

    strong_hash hashGlyphKeyAndPresentation(text::glyph_key const& glyphKey,
                                            unicode::PresentationStyle presentation) noexcept
    {
        // return StrongHash::compute(key) * static_cast<uint32_t>(presentation);
        // clang-format off
        return strong_hash::compute(glyphKey.font.value)
               * static_cast<uint32_t>(glyphKey.index.value)
               * strong_hash::compute(glyphKey.size.pt)
               * static_cast<uint32_t>(presentation)
               ;
        // clang-format on
    }

    strong_hash hashTextAndStyle(u32string_view text, TextStyle style) noexcept
    {
        return strong_hash::compute(text) * static_cast<uint32_t>(style);
    }

    text::font_key getFontForStyle(FontKeys const& fonts, TextStyle style)
    {
        switch (style)
        {
            case TextStyle::Invalid: break;
            case TextStyle::Regular: return fonts.regular;
            case TextStyle::Bold: return fonts.bold;
            case TextStyle::Italic: return fonts.italic;
            case TextStyle::BoldItalic: return fonts.boldItalic;
        }
        return fonts.regular;
    }

    atlas::Format toAtlasFormat(text::bitmap_format format)
    {
        switch (format)
        {
            case text::bitmap_format::alpha_mask: return atlas::Format::Red;
            case text::bitmap_format::rgb: return atlas::Format::RGB;
            case text::bitmap_format::rgba: return atlas::Format::RGBA;
            case text::bitmap_format::outlined: return atlas::Format::RGBA;
        }

        (void) SoftRequire(false);
        return atlas::Format::RGBA;
    }

    uint32_t toFragmentShaderSelector(text::bitmap_format glyphFormat)
    {
        auto const lcdShaderId = FRAGMENT_SELECTOR_GLYPH_LCD;
        // TODO ^^^ configurable vs FRAGMENT_SELECTOR_GLYPH_LCD_SIMPLE
        switch (glyphFormat)
        {
            case text::bitmap_format::alpha_mask: return FRAGMENT_SELECTOR_GLYPH_ALPHA;
            case text::bitmap_format::rgb: return lcdShaderId;
            case text::bitmap_format::rgba: return FRAGMENT_SELECTOR_IMAGE_BGRA;
            case text::bitmap_format::outlined: return FRAGMENT_SELECTOR_GLYPH_OUTLINED;
        }
        (void) SoftRequire(false);
        return FRAGMENT_SELECTOR_IMAGE_BGRA;
    }

    constexpr TextStyle makeTextStyle(vtbackend::CellFlags mask) noexcept
    {
        if (mask == vtbackend::CellFlags { vtbackend::CellFlag::Bold, vtbackend::CellFlag::Italic })
            return TextStyle::BoldItalic;
        if (mask & vtbackend::CellFlag::Bold)
            return TextStyle::Bold;
        if (mask & vtbackend::CellFlag::Italic)
            return TextStyle::Italic;
        return TextStyle::Regular;
    }
} // namespace

text::font_locator& createFontLocator(FontLocatorEngine engine)
{
    switch (engine)
    {
        case FontLocatorEngine::Mock: return text::font_locator_provider::get().mock();
        default: return text::font_locator_provider::get().native();
    }

    crispy::unreachable();
}

// TODO: What's a good value here? Or do we want to make that configurable,
// or even computed based on memory resources available?
constexpr uint32_t TextShapingCacheSize = 4000;

TextRenderer::TextRenderer(GridMetrics const& gridMetrics,
                           text::shaper& textShaper,
                           FontDescriptions& fontDescriptions,
                           FontKeys const& fontKeys,
                           TextRendererEvents& eventHandler,
                           GlyphScaler const& glyphScaler):
    Renderable { gridMetrics },
    _textClusterGrouper { *this },
    _textRendererEvents { eventHandler },
    _fontDescriptions { fontDescriptions },
    _fonts { fontKeys },
    _textShapingCache { ShapingResultCache::create(crispy::strong_hashtable_size { 16384 },
                                                   crispy::lru_capacity { TextShapingCacheSize },
                                                   "Text shaping cache") },
    _textShaper { textShaper },
    _boxDrawingRenderer { gridMetrics },
    _glyphScaler { &glyphScaler }
{
}

GlyphScaler const& TextRenderer::defaultGlyphScaler() noexcept
{
    // Re-rasterizing, matching Config's `text_scaling_method` default: asking the font for the
    // outline at the larger size re-hints it, where stretching magnifies an ordinary-size raster.
    // This became real once shaper::resize_font gave a caller the same face at another size -- a
    // font_key encodes its size, so scaling `glyph_key.size.pt` alone never reached FreeType.
    // @see GlyphScalingMethod.
    static auto const scaler = RerasterizingGlyphScaler {};
    return scaler;
}

void TextRenderer::inspect(ostream& textOutput) const
{
    textOutput << "TextRenderer:\n";
    _textShapingCache->inspect(textOutput);
    _boxDrawingRenderer.inspect(textOutput);
}

void TextRenderer::setRenderTarget(
    RenderTarget& renderTarget, atlas::DirectMappingAllocator<RenderTileAttributes>& directMappingAllocator)
{
    _directMapping = directMappingAllocator.allocate(DirectMappedCharsCount);
    Renderable::setRenderTarget(renderTarget, directMappingAllocator);
    _boxDrawingRenderer.setRenderTarget(renderTarget, directMappingAllocator);
    clearCache();
}

void TextRenderer::setTextureAtlas(TextureAtlas& atlas)
{
    Renderable::setTextureAtlas(atlas);
    _boxDrawingRenderer.setTextureAtlas(atlas);

    if (_directMapping)
        initializeDirectMapping();
}

void TextRenderer::clearCache()
{
    if (_textureAtlas && _directMapping)
        initializeDirectMapping();

    _textShapingCache->clear();

    _boxDrawingRenderer.clearCache();
}

void TextRenderer::restrictToTileSize(TextureAtlas::TileCreateData& tileCreateData)
{
    if (tileCreateData.bitmapSize.width <= _textureAtlas->tileSize().width)
        return;
    // Shrink the image's width by recreating it.
    // TODO: In the longer term it would be nice to simply touch the pitch value in order to shrink.
    //       But this requires extending the data structure to also provide a pitch value.

    auto const colorComponentCount = atlas::element_count(tileCreateData.bitmapFormat);

    auto const targetSize = ImageSize { _textureAtlas->tileSize().width, tileCreateData.bitmapSize.height };
    auto const targetPitch = unbox<uintptr_t>(targetSize.width) * colorComponentCount;
    auto const sourcePitch = unbox<uintptr_t>(tileCreateData.bitmapSize.width) * colorComponentCount;

    if (!SoftRequire(targetPitch < sourcePitch))
        return;

    auto slicedBitmap = vector<uint8_t>(targetSize.area() * colorComponentCount);

    if (rasterizerLog)
        rasterizerLog()("Cutting off oversized {} ({}) tile from {} ({}) down to {}.",
                        tileCreateData.bitmapFormat,
                        colorComponentCount,
                        tileCreateData.bitmapSize,
                        tileCreateData.metadata.targetSize,
                        targetSize);

    for (auto const rowIndex: std::views::iota(uintptr_t { 0 }, unbox<uintptr_t>(targetSize.height)))
    {
        uint8_t* targetRow = slicedBitmap.data() + (rowIndex * targetPitch);
        uint8_t const* sourceRow = tileCreateData.bitmap.data() + (rowIndex * sourcePitch);
        if (!SoftRequire(sourceRow + targetPitch
                         <= tileCreateData.bitmap.data() + tileCreateData.bitmap.size()))
            return;
        std::copy_n(sourceRow, targetPitch, targetRow);
    }

    tileCreateData.metadata.targetSize = {};
    tileCreateData.bitmapSize = targetSize;
    tileCreateData.bitmap = std::move(slicedBitmap);

    // NB: Also adjust the normalized width to not render the empty space.
    auto const atlasSize = _textureScheduler->atlasSize();
    tileCreateData.metadata.normalizedLocation.width =
        unbox<float>(tileCreateData.bitmapSize.width) / unbox<float>(atlasSize.width);
}

void TextRenderer::initializeDirectMapping()
{
    if (!SoftRequire(_textureAtlas) || !SoftRequire(_directMapping.count == DirectMappedCharsCount))
        return;

    _directMappedGlyphKeyToTileIndex.clear();
    _directMappedGlyphKeyToTileIndex.resize(LastReservedChar + 1);

    for (char32_t const codepoint: std::views::iota(FirstReservedChar, LastReservedChar + 1))
    {
        if (optional<text::glyph_position> gposOpt = _textShaper.shape(_fonts.regular, codepoint))
        {
            text::glyph_key const& glyph = gposOpt.value().glyph;
            if (glyph.index.value >= _directMappedGlyphKeyToTileIndex.size())
                _directMappedGlyphKeyToTileIndex.resize(glyph.index.value
                                                        + (LastReservedChar - codepoint + 1));
            _directMappedGlyphKeyToTileIndex[glyph.index.value] =
                _directMapping.toTileIndex(codepoint - FirstReservedChar);
        }
    }
}

Renderable::AtlasTileAttributes const* TextRenderer::ensureRasterizedIfDirectMapped(
    text::glyph_key const& glyph)
{
    if (!isGlyphDirectMapped(glyph))
        return nullptr;

    auto const tileIndex = _directMappedGlyphKeyToTileIndex[glyph.index.value];

    if (_textureAtlas->directMapped(tileIndex).bitmapSize.width.value)
        // TODO: Find a better way to test if the glyph was rasterized&uploaded already.
        // like: if (_textureAtlas->isDirectMappingSet(tileIndex)) ...
        return &_textureAtlas->directMapped(tileIndex);

    auto const tileLocation = _textureAtlas->tileLocation(tileIndex);
    auto tileCreateData = createRasterizedGlyph(tileLocation, glyph, unicode::PresentationStyle::Text);
    if (!tileCreateData)
        return nullptr;

    restrictToTileSize(*tileCreateData);
    if (!SoftRequire(tileCreateData->bitmapSize.width <= textureAtlas().tileSize().width))
        return nullptr;

    // std::cout << std::format("Initialize direct mapping {} ({}) for {}; {}; {}\n",
    //            tileIndex,
    //            tileLocation,
    //            glyph,
    //            tileCreateData->bitmapSize,
    //            tileCreateData->metadata);

    _textureAtlas->setDirectMapping(tileIndex, std::move(*tileCreateData));
    return &_textureAtlas->directMapped(tileIndex);
}

void TextRenderer::updateFontMetrics()
{
    if (!renderTargetAvailable())
        return;

    clearCache();
}

void TextRenderer::beginFrame()
{
    _textClusterGrouper.beginFrame();
}

void TextRenderer::renderLine(vtbackend::RenderLine const& renderLine)
{
    _textClusterGrouper.renderLine(renderLine.text,
                                   renderLine.lineOffset,
                                   renderLine.textAttributes.foregroundColor,
                                   makeTextStyle(renderLine.textAttributes.flags),
                                   renderLine.flags);
}

void TextRenderer::renderCell(vtbackend::RenderCell const& cell)
{
    // std::cout << std::format("renderCell: {} {} {} {} {}\n",
    //            cell.position,
    //            unicode::convert_to<char>(u32string_view(cell.codepoints)),
    //            _forceUpdateInitialPenPosition ? "forcedRestart" : "-",
    //            cell.groupStart ? "groupStart" : "-",
    //            cell.groupEnd ? "groupEnd" : "-");

    if (cell.groupStart)
        _textClusterGrouper.forceGroupStart();

    _textClusterGrouper.renderCell(cell.position,
                                   cell.codepoints,
                                   cell.attributes.foregroundColor,
                                   makeTextStyle(cell.attributes.flags),
                                   cell.attributes.lineFlags,
                                   cell.sizing);

    if (cell.groupEnd)
        _textClusterGrouper.forceGroupEnd();
}

void TextRenderer::endFrame()
{
    _textClusterGrouper.endFrame();
}

point TextRenderer::applyGlyphPositionToPen(point pen,
                                            AtlasTileAttributes const& tileAttributes,
                                            text::glyph_position const& gpos) const noexcept
{
    auto const& glyphMetrics = tileAttributes.metadata;

    auto const x = pen.x + glyphMetrics.x.value + gpos.offset.x;

    // Emoji are simple square bitmap fonts that do not need special positioning.
    auto const y = pen.y                   // -> base pen position
                   - _gridMetrics.baseline // -> text baseline
                   - glyphMetrics.y.value  // -> bitmap top
                   - gpos.offset.y         // -> harfbuzz adjustment
                                           //- tileAttributes.bitmapSize.height.as<int>() // -> bitmap height
        ;

    // std::cout << std::format("pen! {} <- {}, gpos {}, glyph offset {}x+{}y, glyph height {} ({})\n",
    //            Point { x, y },
    //            pen,
    //            gpos,
    //            glyphMetrics.x.value,
    //            glyphMetrics.y.value,
    //            tileAttributes.bitmapSize.height,
    //            gpos.presentation);

    return point { .x = x, .y = y };
}

/**
 * Renders a tile relative to the shape run's base position.
 *
 * @param _pos          offset relative to the glyph run's base position
 * @param _color        text color
 * @param _glyphMetrics bitmap size and glyph bearing (cachable)
 * @param _glyphPos     glyph positioning relative to the pen's baseline pos (cachable)
 *
 */
void TextRenderer::renderRasterizedGlyph(crispy::point pen,
                                         vtbackend::RGBAColor color,
                                         AtlasTileAttributes const& attributes)
{
    // clang-format off
    // RasterizerLog()("render glyph pos {} tile {} offset {}:{}",
    //                 pos,
    //                 glyphLocationInAtlas,
    //                 glyphPos.offset.x,
    //                 glyphPos.offset.y);
    // clang-format on

    renderTile(atlas::RenderTile::X { pen.x }, atlas::RenderTile::Y { pen.y }, color, attributes);

    // clang-format off
    // if (RasterizerLog)
    //     RasterizerLog()("xy={}:{} pos={} tex={}, gpos=({}:{}), baseline={}",
    //                     x, y,
    //                     pos,
    //                     glyphBitmapSize,
    //                     glyphPos.offset.x, glyphPos.offset.y,
    //                     gridMetrics.baseline);
    // clang-format on
}

bool TextRenderer::renderBoxDrawingCell(vtbackend::CellLocation position,
                                        char32_t codepoint,
                                        vtbackend::RGBColor foregroundColor,
                                        vtbackend::LineFlags flags)
{
    if (_fontDescriptions.builtinBoxDrawing)
        return _boxDrawingRenderer.render(position.line, position.column, codepoint, flags, foregroundColor);

    return false;
}

crispy::point adjustPenForLineFlags(vtbackend::LineFlags lineFlags,
                                    GridMetrics const& gridMetrics,
                                    int glyphHeight,
                                    int glyphBearingY,
                                    crispy::point pen) noexcept
{
    using vtbackend::LineFlag;

    if (lineFlags.test(LineFlag::DoubleHeightTop))
    {
        auto const lineHeight = unbox<int>(gridMetrics.cellSize.height);
        pen.y += lineHeight - glyphBearingY;
    }
    else if (lineFlags.test(LineFlag::DoubleHeightBottom))
    {
        pen.y += glyphHeight - glyphBearingY;
    }

    return pen;
}

/// Applies a glyph scale adjustment on top of whatever the line flags already did.
///
/// The two compose: a scaled cell on a double-width line is stretched by both. The mechanism is the
/// same one DECDHL uses -- widen the tile's target rectangle and its x offset, leaving the source
/// texture alone -- which is what makes the Stretch strategy free at rasterization time.
Renderable::AtlasTileAttributes adjustTileAttributesForScale(
    GlyphScaleAdjustment adjustment, Renderable::AtlasTileAttributes const& originalAttributes)
{
    if (adjustment.isIdentity())
        return originalAttributes;

    auto const scaled = [factor = adjustment.factor](auto value) {
        return static_cast<int>(std::lround(static_cast<double>(value) * factor));
    };

    auto attributesCopy = originalAttributes;
    attributesCopy.metadata.targetSize.width =
        vtbackend::Width::cast_from(scaled(unbox(attributesCopy.metadata.targetSize.width)));
    attributesCopy.metadata.targetSize.height =
        vtbackend::Height::cast_from(scaled(unbox(attributesCopy.metadata.targetSize.height)));
    attributesCopy.metadata.x.value = scaled(attributesCopy.metadata.x.value);

    return attributesCopy;
}

Renderable::AtlasTileAttributes adjustTileAttributesForLineFlags(
    vtbackend::LineFlags lineFlags, Renderable::AtlasTileAttributes const& originalAttributes)
{
    using vtbackend::LineFlag;
    using vtbackend::Width;

    auto attributesCopy = originalAttributes;

    auto const currentWidth = unbox(attributesCopy.metadata.targetSize.width);

    if (lineFlags.test(LineFlag::DoubleHeightTop))
    {
        attributesCopy.metadata.targetSize.width = Width::cast_from(currentWidth * 2);
        attributesCopy.metadata.x.value *= 2;
        attributesCopy.metadata.normalizedLocation.height /= 2.0f;
    }
    else if (lineFlags.test(LineFlag::DoubleHeightBottom))
    {
        attributesCopy.metadata.targetSize.width = Width::cast_from(currentWidth * 2);
        attributesCopy.metadata.x.value *= 2;
        attributesCopy.metadata.normalizedLocation.y +=
            attributesCopy.metadata.normalizedLocation.height / 2.0f;
        attributesCopy.metadata.normalizedLocation.height /= 2.0f;
    }
    else if (lineFlags.test(LineFlag::DoubleWidth))
    {
        attributesCopy.metadata.targetSize.width = Width::cast_from(currentWidth * 2);
        attributesCopy.metadata.x.value *= 2;
    }

    return attributesCopy;
}

/// Clips a tile about to be drawn to one screen row's band, in both texture space and height.
///
/// A block `scale` cells tall is one raster drawn across several rows. Each row draws its own band of
/// it, so a block whose head has scrolled above the viewport is CLIPPED rather than lost -- when only
/// the head cell drew, the whole block vanished with it. kitty cuts the same bands in
/// calculate_regions_for_line().
///
/// @param attributes the tile, adjusted in place.
/// @param tileTop    the tile's top in screen pixels; moved down to the visible part.
/// @param bandTop    the row's top edge in screen pixels.
/// @param bandBottom the row's bottom edge in screen pixels.
/// @return false when the tile does not reach this band at all, and must not be drawn.
[[nodiscard]] bool clipTileToBand(Renderable::AtlasTileAttributes& attributes,
                                  int& tileTop,
                                  int bandTop,
                                  int bandBottom)
{
    auto const drawnHeight = unbox<int>(attributes.metadata.targetSize.height);
    if (drawnHeight <= 0)
        return true;

    auto const tileBottom = tileTop + drawnHeight;
    auto const visibleTop = std::max(tileTop, bandTop);
    auto const visibleBottom = std::min(tileBottom, bandBottom);
    if (visibleBottom <= visibleTop)
        return false;
    if (visibleTop == tileTop && visibleBottom == tileBottom)
        return true;

    auto const from = static_cast<float>(visibleTop - tileTop) / static_cast<float>(drawnHeight);
    auto const to = static_cast<float>(visibleBottom - tileTop) / static_cast<float>(drawnHeight);

    auto& location = attributes.metadata.normalizedLocation;
    location.y += location.height * from;
    location.height *= to - from;
    attributes.metadata.targetSize.height = vtbackend::Height::cast_from(visibleBottom - visibleTop);
    tileTop = visibleTop;
    return true;
}

void TextRenderer::renderTextGroup(std::u32string_view codepoints,
                                   gsl::span<unsigned> clusters,
                                   vtbackend::CellLocation initialPenPosition,
                                   TextStyle style,
                                   vtbackend::RGBColor color,
                                   vtbackend::LineFlags lineFlags,
                                   vtbackend::GlyphSizing const& sizing)
{
    if (codepoints.empty())
        return;

    _textRendererEvents.onBeforeRenderingText();
    auto _ = crispy::finally { [&]() noexcept { _textRendererEvents.onAfterRenderingText(); } };

    auto const hash = hashTextAndStyle(codepoints, style);
    text::shape_result const& glyphPositions =
        getOrCreateCachedGlyphPositions(hash, codepoints, clusters, style);
    crispy::point pen = _gridMetrics.mapBottomLeft(initialPenPosition, _smoothScrollYOffset);

    using vtbackend::LineFlag;

    auto const advanceScale = lineFlags.test(LineFlag::DoubleWidth) ? 2 : 1;
    auto const cellWidth = unbox<int>(_gridMetrics.cellSize.width);

    // How this group's glyphs are enlarged. Hoisted out of the loop: it is a property of the group,
    // and both branches below need it.
    auto const& scale = sizing.scale;
    auto const adjustment = _glyphScaler->adjustmentFor(scale);

    // A text-sizing block is drawn from cell-sized tiles cut out of one block-sized raster, which is
    // a different shape of work from the ordinary path below -- and must stay so: the atlas cannot
    // hold a tile larger than one cell, and every row of the block has to be able to draw itself.
    if (!scale.isOrdinary())
    {
        renderBlockGroup(glyphPositions, pen, color, sizing, adjustment);
        return;
    }

    for (auto const& glyphPosition: glyphPositions)
    {
        if (auto const* attributes = ensureRasterizedIfDirectMapped(glyphPosition.glyph))
        {
            auto const attributesCopy = adjustTileAttributesForLineFlags(lineFlags, *attributes);
            auto pen1 = applyGlyphPositionToPen(pen, attributesCopy, glyphPosition);
            pen1 = adjustPenForLineFlags(lineFlags,
                                         _gridMetrics,
                                         unbox<int>(attributes->bitmapSize.height),
                                         attributes->metadata.y.value,
                                         pen1);
            renderRasterizedGlyph(pen1, color, attributesCopy);

            // Direct mapping only ever covers printable US-ASCII of the primary font, which occupies
            // exactly one cell. The advance is known, so the font is not consulted for it.
            pen.x += cellWidth * advanceScale;
            continue;
        }

        auto const glyphHash = hashGlyphKeyAndPresentation(glyphPosition.glyph, glyphPosition.presentation);
        AtlasTileAttributes const* attributes =
            getOrCreateRasterizedMetadata(glyphHash, glyphPosition.glyph, glyphPosition.presentation);

        if (attributes)
        {
            auto const attributesCopy = adjustTileAttributesForLineFlags(lineFlags, *attributes);
            auto pen1 = applyGlyphPositionToPen(pen, attributesCopy, glyphPosition);
            pen1 = adjustPenForLineFlags(lineFlags,
                                         _gridMetrics,
                                         unbox<int>(attributes->bitmapSize.height),
                                         attributes->metadata.y.value,
                                         pen1);
            renderRasterizedGlyph(pen1, color, attributesCopy);

            // A glyph wider than one atlas tile was sliced at rasterization time; the head tile was
            // just drawn, and the remaining slices follow. They are parts of the SAME glyph, so
            // every enlargement that applied to the head applies to them identically.
            //
            // Each slice is placed where the previous one ENDED rather than at a re-derived
            // multiple of the cell width. Stepping by the width actually drawn is what keeps the
            // slices contiguous no matter how many multipliers are in play.
            auto sliceX = pen1.x + unbox<int>(attributesCopy.metadata.targetSize.width);
            auto sliceKey = unbox(textureAtlas().tileSize().width);
            while (AtlasTileAttributes const* subAttribs = textureAtlas().try_get(glyphHash * sliceKey))
            {
                auto const subAttribsCopy = adjustTileAttributesForLineFlags(lineFlags, *subAttribs);
                renderTile(
                    atlas::RenderTile::X { sliceX }, atlas::RenderTile::Y { pen1.y }, color, subAttribsCopy);
                sliceX += unbox<int>(subAttribsCopy.metadata.targetSize.width);
                sliceKey += unbox(textureAtlas().tileSize().width);
            }
        }

        // TODO: The font's advance is a stand-in for the datum the pipeline actually has and then drops:
        // the glyph's cluster, i.e. which cell it belongs to. Carrying the cluster on glyph_position would
        // let the pen step by the exact cell delta -- zero for a combining mark, N for a ligature spanning
        // N cells -- with no rounding at all. That awaits TextClusterGrouper's east-asian-width fixme,
        // since clusters presently count cells appended rather than columns occupied.
        pen.x += advanceToCells(glyphPosition.advance.x, cellWidth) * cellWidth * advanceScale;
    }
}

/// Composites @p glyphPosition's raster into a canvas the size of its text-sizing block.
///
/// The canvas is whole cells on both axes, so cutting it into atlas tiles is exact, and the glyph is
/// placed at its block baseline with `v`/`h` applied -- kitty does the same in render_group() before
/// extract_cell_region() cuts the sprites.
std::optional<TextRenderer::BlockCanvas> TextRenderer::buildBlockCanvas(
    std::span<text::glyph_position const> cluster,
    vtbackend::CellScale const& cellScale,
    GlyphScaleAdjustment adjustment,
    int cellsAtOneX)
{
    if (cluster.empty())
        return std::nullopt;

    auto const base = rasterizeAtBlockSize(cluster.front().glyph, adjustment);
    if (!base.has_value())
        return std::nullopt;

    // Asked with zero bearings, this answers where the block's own box sits: originX is the box's
    // left edge and originY its baseline. Each glyph is then placed against those, so the marks of a
    // cluster share one origin instead of each deriving its own.
    auto const box = blockPlacementFor(cellScale,
                                       unbox<int>(_gridMetrics.cellSize.width),
                                       unbox<int>(_gridMetrics.cellSize.height),
                                       _gridMetrics.baseline,
                                       cellsAtOneX,
                                       0,
                                       0);

    auto const components = static_cast<size_t>(text::pixel_size(base->format));
    auto canvas =
        BlockCanvas { .size = box.canvasSize,
                      .format = toAtlasFormat(base->format),
                      .fragmentShaderSelector = toFragmentShaderSelector(base->format),
                      .components = components,
                      .bitmap = std::vector<uint8_t>(box.canvasSize.area() * components, uint8_t { 0 }) };

    auto const scaled = [&](int value) {
        return static_cast<int>(std::lround(static_cast<double>(value) * adjustment.factor));
    };

    // HarfBuzz's contract, and the reason a cluster is more than one glyph: each glyph is drawn at
    // `pen + offset`, and then the pen moves on by `advance`. Placing every glyph at the block's own
    // origin instead stacks the pieces of a Devanagari conjunct on top of one another, which is what
    // left `क्नि` showing nothing but its matra.
    //
    // These advances are NOT rounded to whole cells the way the ordinary path rounds them: inside a
    // cluster they position glyphs against each other, not against the cell grid.
    struct Placement
    {
        text::rasterized_glyph glyph;
        int x;
        int y;
    };
    auto placements = std::vector<Placement> {};
    placements.reserve(cluster.size());

    auto pen = crispy::point { .x = box.originX, .y = box.originY };

    for (auto const& glyphPosition: cluster)
    {
        auto glyph =
            &glyphPosition == &cluster.front() ? base : rasterizeAtBlockSize(glyphPosition.glyph, adjustment);

        // One canvas holds one pixel format. A glyph whose format differs from the cluster's first
        // (an emoji modifier on a text base, say) cannot be composited into it, and is skipped
        // rather than written with the wrong stride -- but it still moves the pen.
        if (!glyph.has_value())
            rasterizerLog()("Cluster glyph {} did not rasterize.", glyphPosition.glyph.index.value);
        else if (glyph->format != base->format)
            rasterizerLog()(
                "Skipping cluster glyph of format {} on a {} canvas.", glyph->format, base->format);
        else
        {
            auto const x = pen.x + scaled(glyphPosition.offset.x) + glyph->position.x;
            auto const y = pen.y - scaled(glyphPosition.offset.y) - glyph->position.y;
            placements.emplace_back(Placement { .glyph = std::move(*glyph), .x = x, .y = y });
        }

        pen.x += scaled(glyphPosition.advance.x);
        pen.y -= scaled(glyphPosition.advance.y);
    }

    // A cluster can reach LEFT of its own origin: Devanagari reorders the i-matra ahead of its base,
    // and its bearing is negative. On the grid that merely draws into the neighbouring cell, but a
    // canvas has an edge, and the overhang was being cut away. Shift the cluster right so its
    // leftmost ink starts at the canvas origin -- kitty's right_shift_canvas() does the same.
    auto leftmost = 0;
    for (auto const& placement: placements)
        leftmost = std::min(leftmost, placement.x);

    for (auto const& placement: placements)
        // Overhang above or below -- a descender, an accent -- is still clipped rather than being an
        // error: a glyph is placed on its baseline, not fitted to the box.
        blitClipped(canvas.bitmap,
                    canvas.size,
                    placement.glyph.bitmap,
                    placement.glyph.bitmapSize,
                    placement.x - leftmost,
                    placement.y,
                    components);

    return canvas;
}

/// Cuts the single cell at (@p column, @p band) out of @p canvas as an atlas tile.
///
/// Every tile is one cell, which is the whole point: TextureAtlas spaces tile origins one tileSize
/// apart, so a taller tile would overwrite its neighbour.
std::optional<TextRenderer::TextureAtlas::TileCreateData> TextRenderer::createBlockTile(
    BlockCanvas const* canvas, atlas::TileLocation tileLocation, uint32_t column, uint32_t band)
{
    if (canvas == nullptr)
        return std::nullopt;

    auto const tiles =
        sliceIntoTiles(canvas->bitmap, canvas->size, _gridMetrics.cellSize, canvas->components);

    auto const wanted = std::ranges::find_if(
        tiles, [&](GlyphTile const& tile) { return tile.column == column && tile.band == band; });
    if (wanted == tiles.end())
        return std::nullopt;

    if (!SoftRequire(wanted->size.width <= _gridMetrics.cellSize.width
                     && wanted->size.height <= _gridMetrics.cellSize.height))
        return std::nullopt;

    return createTileData(tileLocation,
                          std::move(wanted->bitmap),
                          canvas->format,
                          wanted->size,
                          RenderTileAttributes::X { 0 },
                          RenderTileAttributes::Y { 0 },
                          canvas->fragmentShaderSelector);
}

/// Rasterizes @p glyphKey at the size the block will draw it, magnifying an ordinary raster when the
/// strategy is Stretch.
///
/// Both strategies must hand back a raster at its FINAL size, because the block canvas composites
/// pixels: there is no later opportunity to enlarge. That is what makes the two methods differ only
/// in fidelity (a re-hinted outline versus a magnified bitmap) rather than in code path.
std::optional<text::rasterized_glyph> TextRenderer::rasterizeAtBlockSize(text::glyph_key const& glyphKey,
                                                                         GlyphScaleAdjustment adjustment)
{
    auto key = glyphKey;
    if (adjustment.requiresRerasterization)
    {
        // A font_key already encodes its size, so editing glyph_key::size alone would change nothing
        // FreeType can see. Ask the shaper for the SAME face at the larger size; that re-hints it.
        key.size.pt *= adjustment.factor;
        key.font = _textShaper.resize_font(glyphKey.font, key.size);
    }

    auto glyph =
        _textShaper.rasterize(key, _fontDescriptions.renderMode, _fontDescriptions.textOutline.thickness);
    if (!glyph.has_value())
        return std::nullopt;

    if (!SoftRequire(glyph->bitmap.size()
                     == text::pixel_size(glyph->format) * unbox<size_t>(glyph->bitmapSize.width)
                            * unbox<size_t>(glyph->bitmapSize.height)))
        return std::nullopt;

    if (!adjustment.requiresRerasterization && adjustment.factor != 1.0)
    {
        // Stretch: magnify the ordinary raster to the drawn size. Its bearings scale with it, so the
        // placement arithmetic downstream needs no knowledge of which strategy produced this.
        auto const magnifiedSize =
            ImageSize { vtbackend::Width::cast_from(
                            std::lround(unbox<double>(glyph->bitmapSize.width) * adjustment.factor)),
                        vtbackend::Height::cast_from(
                            std::lround(unbox<double>(glyph->bitmapSize.height) * adjustment.factor)) };

        if (unbox(magnifiedSize.width) != 0 && unbox(magnifiedSize.height) != 0)
        {
            glyph->bitmap = magnify(glyph->bitmap,
                                    glyph->bitmapSize,
                                    magnifiedSize,
                                    static_cast<size_t>(text::pixel_size(glyph->format)));
            glyph->bitmapSize = magnifiedSize;
            glyph->position.x =
                static_cast<int>(std::lround(static_cast<double>(glyph->position.x) * adjustment.factor));
            glyph->position.y =
                static_cast<int>(std::lround(static_cast<double>(glyph->position.y) * adjustment.factor));
        }
    }

    return glyph;
}

void TextRenderer::renderBlockGroup(text::shape_result const& glyphPositions,
                                    crispy::point pen,
                                    vtbackend::RGBColor color,
                                    vtbackend::GlyphSizing const& sizing,
                                    GlyphScaleAdjustment adjustment)
{
    auto const cellWidth = unbox<int>(_gridMetrics.cellSize.width);
    auto const cellHeight = unbox<int>(_gridMetrics.cellSize.height);
    auto const& scale = sizing.scale;
    auto const blockScale = std::max<int>(1, scale.scale);

    // The pen is the bottom of THIS row; a tile is drawn from its top, and every tile is exactly one
    // cell. No band arithmetic survives here: the band chose which tile to fetch, not where to put it.
    auto const rowTop = pen.y - cellHeight;

    // RenderBufferBuilder makes every block its own group, so all of these glyphs belong to ONE
    // block: a single grapheme cluster, however many glyphs shaping turned it into. The column span
    // comes from the backend, which claimed those cells -- deriving it from the shaper's advances
    // cannot work, because a Devanagari conjunct like `क्नि` is several reordered glyphs, each with
    // an advance, inside one cell.
    auto const columns = static_cast<uint32_t>(std::max<uint8_t>(1, sizing.columns));
    auto const cellsAtOneX = std::max(1, static_cast<int>(columns) / blockScale);

    // Everything that shapes the canvas is folded into the key by blockCanvasHash; every glyph
    // contributes on top, or two conjuncts sharing a base glyph would collide on one entry.
    auto blockHash = blockCanvasHash(scale, static_cast<uint32_t>(cellsAtOneX));
    for (auto const& glyphPosition: glyphPositions)
        blockHash = blockHash * hashGlyphKeyAndPresentation(glyphPosition.glyph, glyphPosition.presentation);

    // Built at most once, and only if some tile is actually missing: a block that is already cached
    // costs a hash lookup per cell and no rasterization at all.
    auto canvas = std::optional<BlockCanvas> {};
    auto const canvasFor = [&]() -> BlockCanvas const* {
        if (!canvas.has_value())
            canvas = buildBlockCanvas(glyphPositions, scale, adjustment, cellsAtOneX);
        return canvas.has_value() ? &canvas.value() : nullptr;
    };

    for (auto const column: std::views::iota(0u, columns))
    {
        auto const key = blockHash * glyphTileSubKey(column, sizing.band);
        auto const* attributes =
            textureAtlas().get_or_try_emplace(key, [&](atlas::TileLocation tileLocation) {
                return createBlockTile(canvasFor(), tileLocation, column, sizing.band);
            });

        if (attributes)
            renderTile(atlas::RenderTile::X { pen.x + (static_cast<int>(column) * cellWidth) },
                       atlas::RenderTile::Y { rowTop },
                       color,
                       *attributes);
    }
}

Renderable::AtlasTileAttributes const* TextRenderer::getOrCreateRasterizedMetadata(
    strong_hash const& hash, text::glyph_key const& glyphKey, unicode::PresentationStyle presentationStyle)
{
    // clang-format off
    return textureAtlas().get_or_try_emplace(
        hash,
        [&](atlas::TileLocation tileLocation)
        -> optional<TextureAtlas::TileCreateData>
        {
            return createSlicedRasterizedGlyph(tileLocation, glyphKey, presentationStyle, hash);
        }
    );
    // clang-format on
}

auto TextRenderer::createSlicedRasterizedGlyph(atlas::TileLocation tileLocation,
                                               text::glyph_key const& glyphKey,
                                               unicode::PresentationStyle presentation,
                                               strong_hash const& hash)
    -> optional<TextureAtlas::TileCreateData>
{
    auto result = createRasterizedGlyph(tileLocation, glyphKey, presentation);
    if (!result)
        return result;

    auto& createData = *result;

    if (unbox<int>(createData.bitmapSize.width) <= unbox<int>(textureAtlas().tileSize().width))
        // standard (narrow) rasterization
        return result;

    // Now, slice wide glyph into smaller fitting tiles,
    // upload all but the head-tile explicitly and then return the head-tile
    // to the caller.

    auto const bitmapFormat = createData.bitmapFormat;
    auto const colorComponentCount = atlas::element_count(bitmapFormat);
    auto const pitch = unbox<uintptr_t>(createData.bitmapSize.width) * colorComponentCount;

    auto const tileWidth = unbox<uintptr_t>(_gridMetrics.cellSize.width);
    for (uintptr_t xOffset = tileWidth; xOffset < unbox<uintptr_t>(createData.bitmapSize.width); xOffset +=
                                                                                                 tileWidth)
    {
        textureAtlas().emplace(
            hash * uint32_t(xOffset),
            [this, xOffset, tileWidth, &createData, colorComponentCount, bitmapFormat, pitch](
                atlas::TileLocation tileLocation) {
                auto const xNext = min(xOffset + tileWidth, unbox<uintptr_t>(createData.bitmapSize.width));
                auto const subWidth = vtbackend::Width::cast_from(xNext - xOffset);
                auto const subSize = ImageSize { subWidth, createData.bitmapSize.height };
                auto const subPitch = unbox<uintptr_t>(subWidth) * colorComponentCount;

                auto bitmap = vector<uint8_t>(subSize.area() * colorComponentCount);
                for (uintptr_t rowIndex = 0; rowIndex < unbox<uintptr_t>(subSize.height); ++rowIndex)
                {
                    uint8_t* targetRow = bitmap.data() + (rowIndex * subPitch);
                    uint8_t const* sourceRow = createData.bitmap.data() + (rowIndex * pitch)
                                               + (uintptr_t(xOffset) * colorComponentCount);
                    if (!SoftRequire(sourceRow + subPitch
                                     <= createData.bitmap.data() + createData.bitmap.size()))
                        break;
                    std::memcpy(targetRow, sourceRow, subPitch);
                }

                return createTileData(tileLocation,
                                      std::move(bitmap),
                                      bitmapFormat,
                                      subSize,
                                      RenderTileAttributes::X { 0 },
                                      createData.metadata.y,
                                      createData.metadata.fragmentShaderSelector);
            });
    }

    // Construct head-tile
    // cut off bitmap to first tile
    auto const headWidth = textureAtlas().tileSize().width;
    auto const headSize = ImageSize { headWidth, createData.bitmapSize.height };
    auto const headPitch = unbox<uintptr_t>(headWidth) * colorComponentCount;
    auto headBitmap = vector<uint8_t>(headSize.area() * colorComponentCount);
    for (uintptr_t rowIndex = 0; rowIndex < unbox<uintptr_t>(headSize.height); ++rowIndex)
        std::memcpy(headBitmap.data() + (rowIndex * headPitch),    // target
                    createData.bitmap.data() + (rowIndex * pitch), // source
                    headPitch);                                    // sub-image line length

    return { createTileData(tileLocation,
                            std::move(headBitmap),
                            bitmapFormat,
                            headSize,
                            createData.metadata.x,
                            createData.metadata.y,
                            createData.metadata.fragmentShaderSelector) };
}

auto TextRenderer::createRasterizedGlyph(atlas::TileLocation tileLocation,
                                         text::glyph_key const& glyphKey,
                                         unicode::PresentationStyle presentation)
    -> optional<TextureAtlas::TileCreateData>
{
    auto theGlyphOpt = _textShaper.rasterize(
        glyphKey, _fontDescriptions.renderMode, _fontDescriptions.textOutline.thickness);
    if (!theGlyphOpt.has_value())
        return nullopt;

    text::rasterized_glyph& glyph = theGlyphOpt.value();
    if (!SoftRequire(glyph.bitmap.size()
                     == text::pixel_size(glyph.format) * unbox<size_t>(glyph.bitmapSize.width)
                            * unbox<size_t>(glyph.bitmapSize.height)))
        return nullopt;

    uint32_t const numCells = presentation == unicode::PresentationStyle::Emoji
                                  ? 2
                                  : 1; // is this the only case - with colored := Emoji presentation?
    // TODO: Derive numCells based on grapheme cluster's EA width instead of emoji presentation.
    // FIXME: this `2` is a hack of my bad knowledge. FIXME.
    // As I only know of emoji being colored fonts, and those take up 2 cell with units.

    // Everything a glyph is measured against below is the box it is allowed to occupy: one cell.
    // A text-sizing block does not come through here at all -- it is rasterized into a block canvas
    // and cut into cell-sized tiles. @see buildBlockCanvas.

    // Scale bitmap down overflowing in diemensions
    auto const emojiBoundingBox = ImageSize {
        vtbackend::Width(_gridMetrics.cellSize.width.value * numCells),
        vtbackend::Height::cast_from(unbox<int>(_gridMetrics.cellSize.height) - _gridMetrics.baseline)
    };
    if (glyph.format == text::bitmap_format::rgba)
    {
        if (glyph.bitmapSize.height
                > vtbackend::Height::cast_from(unbox<double>(emojiBoundingBox.height) * 1.1)
            || glyph.bitmapSize.width
                   > vtbackend::Width::cast_from(unbox<double>(emojiBoundingBox.width) * 1.5))
        {
            if (rasterizerLog)
                rasterizerLog()(
                    "Scaling oversized glyph of {}+{} down to bounding box {} (expected cell count {}).",
                    glyph.bitmapSize,
                    glyph.position,
                    emojiBoundingBox,
                    numCells);
            auto [scaledGlyph, scaleFactor] = text::scale(glyph, emojiBoundingBox);

            glyph = std::move(scaledGlyph);

            if (rasterizerLog)
                rasterizerLog()(" ==> scaled: {}/{}, factor {}", glyph, emojiBoundingBox, scaleFactor);
        }

        // Assume colored (RGBA) bitmap glyphs to be emoji.
        // At least on macOS, the emoji font reports bad positioning, so we simply center them ourself.
        // Center horizontally within the emoji bounding box (numCells * cellWidth).
        glyph.position.x = unbox<int>(emojiBoundingBox.width - glyph.bitmapSize.width) / 2;
        // Center vertically within the full box height, accounting for the baseline offset.
        glyph.position.y = (unbox<int>(_gridMetrics.cellSize.height) + glyph.bitmapSize.height.as<int>()) / 2
                           - _gridMetrics.baseline;
    }
    else if (glyph.bitmapSize.height > _gridMetrics.cellSize.height)
    {
        // Scale down vertically oversized non-RGBA glyphs (e.g. Nerd Font icons in alpha_mask format)
        // to fit within the box, preventing invalid cropping math downstream.
        //
        // The bound is the cell EXACTLY. It used to allow a further 10%, which meant a glyph up to
        // 1.1 cells tall passed through unscaled and was uploaded into a tile one cell tall --
        // overwriting the rows of the tile beneath it in the atlas, and so replacing part of an
        // unrelated glyph. Nothing about text sizing is needed to hit that; a tall Nerd Font icon
        // does it.
        //
        // NOTE: Width overflow is intentionally NOT checked here, because horizontally oversized glyphs
        // (e.g. programming ligatures) are kept wide and handled by createSlicedRasterizedGlyph(),
        // which slices them into multiple tiles when they exceed the atlas tile size.
        //
        // Which is why the box may not be the cell on BOTH axes: text::scale() scales by
        // max(ratioX, ratioY), so a one-cell-wide box makes the width ratio win and squashes a
        // three-cell ligature to a third of its width. Bounding the height alone leaves the height
        // ratio to scale both axes and preserve the aspect. @see oversizedGlyphBoundingBox.
        auto const cellBoundingBox = oversizedGlyphBoundingBox(glyph.bitmapSize, _gridMetrics.cellSize);
        auto const originalPosition = glyph.position;
        if (rasterizerLog)
            rasterizerLog()("Scaling oversized non-RGBA glyph of {}+{} down to box {}.",
                            glyph.bitmapSize,
                            glyph.position,
                            cellBoundingBox);
        auto [scaledGlyph, scaleFactor] = text::scale(glyph, cellBoundingBox);
        glyph = std::move(scaledGlyph);
        // Restore baseline-relative positioning instead of emoji-style centering -- on BOTH axes.
        // The bearing is the font's statement of where the ink sits relative to the pen, so scaling
        // it by the same factor as the raster is what keeps the glyph where the font put it.
        // Overwriting x with a centering value instead shifted every glyph coming through here by
        // (cellWidth - inkWidth)/2 minus its true left side bearing, so a column of box-drawing or
        // powerline glyphs stopped lining up with the text beside it. That went unnoticed while the
        // height bound carried a 10% tolerance, because only badly oversized icons reached it;
        // bounding by the cell exactly brings ordinary glyphs down this path too.
        glyph.position.y = static_cast<int>(static_cast<float>(originalPosition.y) / scaleFactor);
        glyph.position.x = static_cast<int>(static_cast<float>(originalPosition.x) / scaleFactor);
        if (rasterizerLog)
            rasterizerLog()(" ==> scaled: {}/{}, factor {}", glyph, cellBoundingBox, scaleFactor);
    }

    // The crop below asks "does this glyph overflow the box it may occupy" -- one cell.
    auto const boxBaseline = _gridMetrics.baseline;
    auto const boxHeight = _gridMetrics.cellSize.height.as<int>();

    // y-position relative to box-bottom of glyphs top.
    auto yMax = boxBaseline + glyph.position.y;

    if (yMax <= 0)
    {
        // Glyph's top is at or below box bottom — not visible.
        if (rasterizerLog)
            rasterizerLog()("Skipping glyph with yMax={} (not visible within box).", yMax);
        return nullopt;
    }

    // y-position relative to box-bottom of the glyphs bottom.
    auto const yMin = yMax - glyph.bitmapSize.height.as<int>();

    // Number of pixel lines this rasterized glyph is overflowing above box-top,
    // or 0 if not overflowing.
    auto const yOverflow = max(0, yMax - boxHeight);

    // {{{ crop underflow if glyph is larger than the box
    if (yMin < 0 && yMax - yMin > boxHeight)
    {
        auto const rowCount = (unsigned) -yMin;
        if (!SoftRequire(rowCount <= unbox(glyph.bitmapSize.height)))
            return nullopt;
        auto const pixelCount =
            rowCount * unbox<size_t>(glyph.bitmapSize.width) * text::pixel_size(glyph.format);
        if (static_cast<size_t>(pixelCount) > glyph.bitmap.size())
        {
            errorLog()("TextRenderer: Glyph bitmap size: {}, dimensions: {}, pixelCount: {}",
                       glyph.bitmap.size(),
                       glyph.bitmapSize,
                       pixelCount);
            return nullopt;
        }
        if (!SoftRequire(0 < pixelCount && static_cast<size_t>(pixelCount) <= glyph.bitmap.size()))
            return nullopt;
        rasterizerLog()("Cropping {} underflowing bitmap rows.", rowCount);
        glyph.bitmapSize.height += vtbackend::Height::cast_from(yMin);
        auto& data = glyph.bitmap;
        data.erase(begin(data), next(begin(data), (int) pixelCount)); // XXX asan hit (size = -2)
        if (!SoftRequire(glyph.valid()))
            return nullopt;
    }
    // }}}

    if (rasterizerLog)
    {
        auto const boundingBox = ImageSize {
            vtbackend::Width(_gridMetrics.cellSize.width.value * numCells),
            vtbackend::Height::cast_from(unbox<int>(_gridMetrics.cellSize.height) - _gridMetrics.baseline)
        };
        // clang-format off
        rasterizerLog()("Inserting {} (bbox {}, numCells {}) id {} render mode {} {} yOverflow {} yMin {}.",
                        glyph,
                        boundingBox,
                        numCells,
                        glyphKey.index,
                        _fontDescriptions.renderMode,
                        [=](){ auto s = std::ostringstream(); s << presentation; return s.str(); }(),
                        yOverflow,
                        yMin);
        // clang-format on
    }

    return { createTileData(tileLocation,
                            std::move(glyph.bitmap),
                            toAtlasFormat(glyph.format),
                            glyph.bitmapSize,
                            RenderTileAttributes::X { glyph.position.x },
                            RenderTileAttributes::Y { glyph.position.y },
                            toFragmentShaderSelector(glyph.format)) };
}

text::shape_result const& TextRenderer::getOrCreateCachedGlyphPositions(strong_hash hash,
                                                                        u32string_view codepoints,
                                                                        gsl::span<unsigned> clusters,
                                                                        TextStyle style)
{
    return _textShapingCache->get_or_emplace(hash, [this, codepoints, clusters, style](auto) {
        return createTextShapedGlyphPositions(codepoints, clusters, style);
    });
}

text::shape_result TextRenderer::createTextShapedGlyphPositions(u32string_view codepoints,
                                                                gsl::span<unsigned> clusters,
                                                                TextStyle style)
{
    auto glyphPositions = text::shape_result {};

    auto run = unicode::run_segmenter::range {};
    auto rs = unicode::run_segmenter(codepoints); // TODO Consider moving run segmentation to text grouper
    while (rs.consume(out(run)))
        for (text::glyph_position const& glyphPosition: shapeTextRun(run, codepoints, clusters, style))
            glyphPositions.emplace_back(glyphPosition);

    return glyphPositions;
}

/**
 * Performs text shaping on a text run, that is, a sequence of codepoints
 * with a uniform set of properties:
 *  - same direction
 *  - same script tag
 *  - same language tag
 *  - same SGR attributes (font style, color)
 */
text::shape_result TextRenderer::shapeTextRun(unicode::run_segmenter::range const& run,
                                              u32string_view totalCodepoints,
                                              gsl::span<unsigned> totalClusters,
                                              TextStyle style)
{
    // TODO(where to apply cell-advances) auto const advanceX = _gridMetrics.cellSize.width;
    auto const count = static_cast<size_t>(run.end - run.start);
    auto const codepoints = u32string_view(totalCodepoints.data() + run.start, count);
    auto const clusters = gsl::span(totalClusters.data() + run.start, count);
    auto const script = get<unicode::Script>(run.properties);
    auto const presentationStyle = get<unicode::PresentationStyle>(run.properties);
    auto const isEmojiPresentation = presentationStyle == unicode::PresentationStyle::Emoji;
    auto const font = isEmojiPresentation ? _fonts.emoji : getFontForStyle(_fonts, style);

    text::shape_result glyphPosition;
    glyphPosition.reserve(clusters.size());
    _textShaper.shape(font,
                      codepoints,
                      clusters,
                      script,            // get<unicode::Script>(run.properties),
                      presentationStyle, // get<unicode::PresentationStyle>(run.properties),
                      glyphPosition);

    if (rasterizerLog && !glyphPosition.empty())
    {
        auto msg = rasterizerLog();
        // clang-format off
        msg.append("Shaped codepoints ({}): {}",
                   [=](){ auto s = std::ostringstream(); s << presentationStyle; return s.str(); }(),
                   unicode::convert_to<char>(codepoints));
        // clang-format on

        msg.append(" (");
        size_t i = 0;
        for (auto const codepoint: codepoints)
        {
            if (i)
                msg.append(" ");
            auto const cluster = clusters[i];
            msg.append("U+{:04X}/{}", unsigned(codepoint), cluster);
            ++i;
        }
        msg.append(")\n");

        // A single shape run always uses the same font,
        // so it is sufficient to just print that.
        // auto const& font = glyphPosition.front().glyph.font;
        // msg.write("using font: \"{}\" \"{}\" \"{}\"\n", font.familyName(), font.styleName(),
        // font.filePath());

        msg.append("with metrics:");
        for (text::glyph_position const& gp: glyphPosition)
            msg.append(" {}", gp);
    }

    return glyphPosition;
}

} // namespace vtrasterizer
