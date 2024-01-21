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

    U+1F600 is the standard smily, a single grapheme cluster.
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

#include <range/v3/view/iota.hpp>

#if defined(_WIN32)
    #include <text_shaper/directwrite_locator.h>
#endif

#if defined(__APPLE__)
    #include <text_shaper/coretext_locator.h>
#endif

#include <crispy/algorithm.h>
#include <crispy/assert.h>
#include <crispy/range.h>

#include <libunicode/convert.h>
#include <libunicode/utf8_grapheme_segmenter.h>

#include <fmt/core.h>
#include <fmt/ostream.h>

#include <range/v3/algorithm/copy.hpp>
#include <range/v3/view/enumerate.hpp>

#include <algorithm>

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
        }

        Require(false && "missing case");
        return atlas::Format::Red; // unreachable();
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
        }
        Require(false && "Glyph format not handled.");
        return 0;
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
        case FontLocatorEngine::DWrite:
#if defined(_WIN32)
            return text::font_locator_provider::get().directwrite();
#else
            locatorLog()("Font locator DirectWrite not supported on this platform.");
#endif
            break;
        case FontLocatorEngine::CoreText:
#if defined(__APPLE__)
            return text::font_locator_provider::get().coretext();
#else
            locatorLog()("Font locator CoreText not supported on this platform.");
#endif
            break;

        case FontLocatorEngine::FontConfig:
            // default case below
            break;
    }

    locatorLog()("Using font locator: fontconfig.");
    return text::font_locator_provider::get().fontconfig();
}

// TODO: What's a good value here? Or do we want to make that configurable,
// or even computed based on memory resources available?
constexpr uint32_t TextShapingCacheSize = 4000;

TextRenderer::TextRenderer(GridMetrics const& gridMetrics,
                           text::shaper& textShaper,
                           FontDescriptions& fontDescriptions,
                           FontKeys const& fontKeys,
                           TextRendererEvents& eventHandler):
    Renderable { gridMetrics },
    _textClusterGrouper { *this },
    _textRendererEvents { eventHandler },
    _fontDescriptions { fontDescriptions },
    _fonts { fontKeys },
    _textShapingCache { ShapingResultCache::create(crispy::strong_hashtable_size { 16384 },
                                                   crispy::lru_capacity { TextShapingCacheSize },
                                                   "Text shaping cache") },
    _textShaper { textShaper },
    _boxDrawingRenderer { gridMetrics }
{
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

    Require(targetPitch < sourcePitch);

    auto slicedBitmap = vector<uint8_t>(targetSize.area() * colorComponentCount);

    if (rasterizerLog)
        rasterizerLog()("Cutting off oversized {} ({}) tile from {} ({}) down to {}.",
                        tileCreateData.bitmapFormat,
                        colorComponentCount,
                        tileCreateData.bitmapSize,
                        tileCreateData.metadata.targetSize,
                        targetSize);

    for (auto const rowIndex: ranges::views::iota(uintptr_t { 0 }, unbox<uintptr_t>(targetSize.height)))
    {
        uint8_t* targetRow = slicedBitmap.data() + rowIndex * targetPitch;
        uint8_t const* sourceRow = tileCreateData.bitmap.data() + rowIndex * sourcePitch;
        Require(sourceRow + targetPitch <= tileCreateData.bitmap.data() + tileCreateData.bitmap.size());
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
    Require(_textureAtlas);
    Require(_directMapping.count == DirectMappedCharsCount);

    _directMappedGlyphKeyToTileIndex.clear();
    _directMappedGlyphKeyToTileIndex.resize(LastReservedChar + 1);

    for (char32_t codepoint = FirstReservedChar; codepoint <= LastReservedChar; ++codepoint)
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
    Require(tileCreateData->bitmapSize.width <= textureAtlas().tileSize().width);

    // fmt::print("Initialize direct mapping {} ({}) for {}; {}; {}\n",
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
                                   makeTextStyle(renderLine.textAttributes.flags));
}

void TextRenderer::renderCell(vtbackend::RenderCell const& cell)
{
    // fmt::print("renderCell: {} {} {} {} {}\n",
    //            cell.position,
    //            unicode::convert_to<char>(u32string_view(cell.codepoints)),
    //            _forceUpdateInitialPenPosition ? "forcedRestart" : "-",
    //            cell.groupStart ? "groupStart" : "-",
    //            cell.groupEnd ? "groupEnd" : "-");

    if (cell.groupStart)
        _textClusterGrouper.forceGroupStart();

    _textClusterGrouper.renderCell(cell.position,
                                   cell.codepoints,
                                   makeTextStyle(cell.attributes.flags),
                                   cell.attributes.foregroundColor);

    if (cell.groupEnd)
        _textClusterGrouper.forceGroupEnd();
}

void TextRenderer::renderCell(vtbackend::CellLocation position,
                              std::u32string_view graphemeCluster,
                              TextStyle textStyle,
                              vtbackend::RGBColor foregroundColor)
{
    _textClusterGrouper.renderCell(position, graphemeCluster, textStyle, foregroundColor);
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

    // fmt::print("pen! {} <- {}, gpos {}, glyph offset {}x+{}y, glyph height {} ({})\n",
    //            Point { x, y },
    //            pen,
    //            gpos,
    //            glyphMetrics.x.value,
    //            glyphMetrics.y.value,
    //            tileAttributes.bitmapSize.height,
    //            gpos.presentation);

    return point { x, y };
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
                                        vtbackend::RGBColor foregroundColor)
{
    if (_fontDescriptions.builtinBoxDrawing)
        return _boxDrawingRenderer.render(position.line, position.column, codepoint, foregroundColor);

    return false;
}

void TextRenderer::renderTextGroup(std::u32string_view codepoints,
                                   gsl::span<unsigned> clusters,
                                   vtbackend::CellLocation initialPenPosition,
                                   TextStyle style,
                                   vtbackend::RGBColor color)
{
    if (codepoints.empty())
        return;

    _textRendererEvents.onBeforeRenderingText();
    auto _ = crispy::finally { [&]() noexcept {
        _textRendererEvents.onAfterRenderingText();
    } };

    auto const hash = hashTextAndStyle(codepoints, style);
    text::shape_result const& glyphPositions =
        getOrCreateCachedGlyphPositions(hash, codepoints, clusters, style);
    crispy::point pen = _gridMetrics.mapBottomLeft(initialPenPosition);
    auto const advanceX = unbox(_gridMetrics.cellSize.width);

    for (auto const& glyphPosition: glyphPositions)
    {
        if (auto const* attributes = ensureRasterizedIfDirectMapped(glyphPosition.glyph))
        {
            auto const pen1 = applyGlyphPositionToPen(pen, *attributes, glyphPosition);
            renderRasterizedGlyph(pen1, color, *attributes);
            pen.x += static_cast<decltype(pen.x)>(advanceX);
            continue;
        }

        auto const hash = hashGlyphKeyAndPresentation(glyphPosition.glyph, glyphPosition.presentation);

        AtlasTileAttributes const* attributes =
            getOrCreateRasterizedMetadata(hash, glyphPosition.glyph, glyphPosition.presentation);

        if (attributes)
        {
            auto const pen1 = applyGlyphPositionToPen(pen, *attributes, glyphPosition);
            renderRasterizedGlyph(pen1, color, *attributes);

            auto xOffset = unbox(textureAtlas().tileSize().width);
            while (AtlasTileAttributes const* subAttribs = textureAtlas().try_get(hash * xOffset))
            {
                renderTile(atlas::RenderTile::X { pen1.x + int(xOffset) },
                           atlas::RenderTile::Y { pen1.y },
                           color,
                           *subAttribs);
                xOffset += unbox(textureAtlas().tileSize().width);
            }
        }

        if (glyphPosition.advance.x)
        {
            // Only advance horizontally, as we're (guess what) a terminal. :-)
            // Only advance in fixed-width steps.
            // Only advance iff there harfbuzz told us to.
            pen.x += static_cast<decltype(pen.x)>(advanceX);
        }
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
                    uint8_t* targetRow = bitmap.data() + rowIndex * subPitch;
                    uint8_t const* sourceRow = createData.bitmap.data() + rowIndex * pitch
                                               + uintptr_t(xOffset) * colorComponentCount;
                    Require(sourceRow + subPitch <= createData.bitmap.data() + createData.bitmap.size());
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
        std::memcpy(headBitmap.data() + rowIndex * headPitch,    // target
                    createData.bitmap.data() + rowIndex * pitch, // source
                    headPitch);                                  // sub-image line length

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
    auto theGlyphOpt = _textShaper.rasterize(glyphKey, _fontDescriptions.renderMode);
    if (!theGlyphOpt.has_value())
        return nullopt;

    text::rasterized_glyph& glyph = theGlyphOpt.value();
    Require(glyph.bitmap.size()
            == text::pixel_size(glyph.format) * unbox<size_t>(glyph.bitmapSize.width)
                   * unbox<size_t>(glyph.bitmapSize.height));

    uint32_t const numCells = presentation == unicode::PresentationStyle::Emoji
                                  ? 2
                                  : 1; // is this the only case - with colored := Emoji presentation?
    // TODO: Derive numCells based on grapheme cluster's EA width instead of emoji presentation.
    // FIXME: this `2` is a hack of my bad knowledge. FIXME.
    // As I only know of emoji being colored fonts, and those take up 2 cell with units.

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
        // At least on MacOS/X, the emoji font reports bad positioning, so we simply center them ourself.
        glyph.position.x = unbox<int>(emojiBoundingBox.width - glyph.bitmapSize.width) / 2;
        glyph.position.y = unbox<int>(emojiBoundingBox.height)
                           - max(0, unbox<int>(emojiBoundingBox.height - glyph.bitmapSize.height) / 2);
    }

    // y-position relative to cell-bottom of glyphs top.
    auto yMax = _gridMetrics.baseline + glyph.position.y;

    if (yMax < 0)
    {
        rasterizerLog()("Encountered glyph with inverted direction, swaping to normal");
        yMax = std::abs(yMax);
    }

    // y-position relative to cell-bottom of the glyphs bottom.
    auto const yMin = yMax - glyph.bitmapSize.height.as<int>();

    // Number of pixel lines this rasterized glyph is overflowing above cell-top,
    // or 0 if not overflowing.
    auto const yOverflow = max(0, yMax - _gridMetrics.cellSize.height.as<int>());

    // {{{ crop underflow if yMin < 0
    // If the rasterized glyph is underflowing below the grid cell's minimum (0),
    // then cut off at grid cell's bottom.
    if (yMin < 0)
    {
        auto const rowCount = (unsigned) -yMin;
        Require(rowCount <= unbox(glyph.bitmapSize.height));
        auto const pixelCount =
            rowCount * unbox<size_t>(glyph.bitmapSize.width) * text::pixel_size(glyph.format);
        Require(0 < pixelCount && static_cast<size_t>(pixelCount) <= glyph.bitmap.size());
        rasterizerLog()("Cropping {} underflowing bitmap rows.", rowCount);
        glyph.bitmapSize.height += vtbackend::Height::cast_from(yMin);
        auto& data = glyph.bitmap;
        data.erase(begin(data), next(begin(data), (int) pixelCount)); // XXX asan hit (size = -2)
        Guarantee(glyph.valid());
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
        for (text::glyph_position& glyphPosition: shapeTextRun(run, codepoints, clusters, style))
            glyphPositions.emplace_back(std::move(glyphPosition));

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
        for (auto const [i, codepoint]: ranges::views::enumerate(codepoints))
        {
            if (i)
                msg.append(" ");
            auto const cluster = clusters[i];
            msg.append("U+{:04X}/{}", unsigned(codepoint), cluster);
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
