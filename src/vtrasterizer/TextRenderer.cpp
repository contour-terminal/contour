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

#include "crispy/StrongHash.h"
#include "crispy/StrongLRUHashtable.h"
#include "crispy/point.h"

#if defined(_WIN32)
    #include <text_shaper/directwrite_locator.h>
#endif

#if defined(__APPLE__)
    #include <text_shaper/coretext_locator.h>
#endif

#include <crispy/algorithm.h>
#include <crispy/assert.h>
#include <crispy/indexed.h>
#include <crispy/range.h>

#include <fmt/core.h>
#include <fmt/ostream.h>

#include <range/v3/algorithm/copy.hpp>

#include <algorithm>

#include <libunicode/convert.h>
#include <libunicode/utf8_grapheme_segmenter.h>

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

namespace terminal::rasterizer
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

    constexpr TextStyle makeTextStyle(CellFlags mask)
    {
        if (contains_all(mask, CellFlags::Bold | CellFlags::Italic))
            return TextStyle::BoldItalic;
        if (mask & CellFlags::Bold)
            return TextStyle::Bold;
        if (mask & CellFlags::Italic)
            return TextStyle::Italic;
        return TextStyle::Regular;
    }

    uint8_t graphemeClusterWidth(std::u32string_view text) noexcept
    {
        assert(!text.empty());
        auto const baseWidth = static_cast<uint8_t>(unicode::width(text[0]));
        for (size_t i = 1; i < text.size(); ++i)
            if (text[i] == 0xFE0F)
                return 2;
        return baseWidth;
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

    auto const bitmapFormat = tileCreateData.bitmapFormat;
    auto const colorComponentCount = atlas::element_count(bitmapFormat);

    auto const subWidth = _textureAtlas->tileSize().width;
    auto const subSize = ImageSize { subWidth, tileCreateData.bitmapSize.height };
    auto const subPitch = unbox<uintptr_t>(subSize.width) * colorComponentCount;
    auto const xOffset = 0;
    auto const sourcePitch = unbox<uintptr_t>(tileCreateData.bitmapSize.width) * colorComponentCount;

    auto slicedBitmap = vector<uint8_t>(subSize.area() * colorComponentCount);

    if (rasterizerLog)
        rasterizerLog()("Cutting off oversized {} tile from {} down to {}.",
                        tileCreateData.bitmapFormat,
                        tileCreateData.bitmapSize,
                        _textureAtlas->tileSize());

    for (uintptr_t rowIndex = 0; rowIndex < unbox<uintptr_t>(subSize.height); ++rowIndex)
    {
        uint8_t* targetRow = slicedBitmap.data() + rowIndex * subPitch;
        uint8_t const* sourceRow =
            tileCreateData.bitmap.data() + rowIndex * sourcePitch + uintptr_t(xOffset) * colorComponentCount;
        Require(sourceRow + subPitch <= tileCreateData.bitmap.data() + tileCreateData.bitmap.size());
        std::memcpy(targetRow, sourceRow, subPitch);
    }

    tileCreateData.bitmapSize = subSize;
    tileCreateData.bitmap = std::move(slicedBitmap);
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

    // Require(tileCreateData->bitmapSize.width <= textureAtlas().tileSize().width);
    restrictToTileSize(*tileCreateData);

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
    // fmt::print("beginFrame: {} / {}\n", _codepoints.size(), _clusters.size());
    Require(_textClusterGroup.codepoints.empty());
    Require(_textClusterGroup.clusters.empty());

    auto constexpr DefaultColor = RGBColor {};
    _textClusterGroup.style = TextStyle::Invalid;
    _textClusterGroup.color = DefaultColor;
}

void TextRenderer::renderLine(RenderLine const& renderLine)
{
    if (renderLine.text.empty())
        return;

    auto const textStyle = makeTextStyle(renderLine.textAttributes.flags);

    auto graphemeClusterSegmenter = unicode::utf8_grapheme_segmenter(renderLine.text);
    auto columnOffset = ColumnOffset(0);

    _textClusterGroup.initialPenPosition =
        _gridMetrics.mapBottomLeft(CellLocation { renderLine.lineOffset, columnOffset });

    for (u32string const& graphemeCluster: graphemeClusterSegmenter)
    {
        auto const gridPosition = CellLocation { renderLine.lineOffset, columnOffset };
        auto const width = graphemeClusterWidth(graphemeCluster);
        renderCell(gridPosition, graphemeCluster, textStyle, renderLine.textAttributes.foregroundColor);

        for (int i = 1; i < width; ++i)
            renderCell(CellLocation { gridPosition.line, columnOffset + i },
                       U" ",
                       textStyle,
                       renderLine.textAttributes.foregroundColor);

        columnOffset += ColumnOffset::cast_from(width);
    }

    if (!_textClusterGroup.codepoints.empty())
        flushTextClusterGroup();
}

void TextRenderer::renderCell(RenderCell const& cell)
{
    if (cell.groupStart)
        _updateInitialPenPosition = true;

    renderCell(cell.position,
               cell.codepoints,
               makeTextStyle(cell.attributes.flags),
               cell.attributes.foregroundColor);

    if (cell.groupEnd)
        flushTextClusterGroup();
}

void TextRenderer::renderCell(CellLocation position,
                              std::u32string_view graphemeCluster,
                              TextStyle textStyle,
                              RGBColor foregroundColor)
{
    if (_updateInitialPenPosition)
    {
        _updateInitialPenPosition = false;
        _textClusterGroup.initialPenPosition = _gridMetrics.mapBottomLeft(position);
    }

    bool const isBoxDrawingCharacter = _fontDescriptions.builtinBoxDrawing && graphemeCluster.size() == 1
                                       && BoxDrawingRenderer::renderable(graphemeCluster[0]);

    if (isBoxDrawingCharacter)
    {
        auto const success =
            _boxDrawingRenderer.render(position.line, position.column, graphemeCluster[0], foregroundColor);
        if (success)
        {
            if (!_updateInitialPenPosition)
                flushTextClusterGroup();
            _updateInitialPenPosition = true;
            return;
        }
    }

    appendCellTextToClusterGroup(graphemeCluster, textStyle, foregroundColor);
}

void TextRenderer::endFrame()
{
    flushTextClusterGroup();
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
                                         RGBAColor color,
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

void TextRenderer::appendCellTextToClusterGroup(u32string_view codepoints, TextStyle style, RGBColor color)
{
    bool const attribsChanged = color != _textClusterGroup.color || style != _textClusterGroup.style;
    bool const hasText = !codepoints.empty() && codepoints[0] != 0x20;
    bool const noText = !hasText;
    bool const textStartFound = !_textStartFound && hasText;
    if (noText)
        _textStartFound = false;
    if (attribsChanged || textStartFound || noText)
    {
        if (_textClusterGroup.cellCount)
            flushTextClusterGroup(); // also increments text start position
        _textClusterGroup.color = color;
        _textClusterGroup.style = style;
        _textStartFound = textStartFound;
    }

    for (char32_t const codepoint: codepoints)
    {
        _textClusterGroup.codepoints.emplace_back(codepoint);
        _textClusterGroup.clusters.emplace_back(_textClusterGroup.cellCount);
    }
    _textClusterGroup.cellCount++;
}

void TextRenderer::flushTextClusterGroup()
{
    if (!_textClusterGroup.codepoints.empty())
    {
        // fmt::print("TextRenderer.flushTextClusterGroup: textPos={}, cellCount={}, width={}, count={}\n",
        //            _textClusterGroup.initialPenPosition, _textClusterGroup.cellCount,
        //            _gridMetrics.cellSize.width,
        //            _textClusterGroup.codepoints.size());

        _textRendererEvents.onBeforeRenderingText();

        auto hash = hashTextAndStyle(
            u32string_view(_textClusterGroup.codepoints.data(), _textClusterGroup.codepoints.size()),
            _textClusterGroup.style);
        text::shape_result const& glyphPositions = getOrCreateCachedGlyphPositions(hash);
        crispy::point pen = _textClusterGroup.initialPenPosition;
        auto const advanceX = *_gridMetrics.cellSize.width;

        for (text::glyph_position const& glyphPosition: glyphPositions)
        {
            if (AtlasTileAttributes const* attributes = ensureRasterizedIfDirectMapped(glyphPosition.glyph))
            {
                auto const pen1 = applyGlyphPositionToPen(pen, *attributes, glyphPosition);
                renderRasterizedGlyph(pen1, _textClusterGroup.color, *attributes);
                pen.x += static_cast<decltype(pen.x)>(advanceX);
                continue;
            }

            auto const hash = hashGlyphKeyAndPresentation(glyphPosition.glyph, glyphPosition.presentation);

            AtlasTileAttributes const* attributes =
                getOrCreateRasterizedMetadata(hash, glyphPosition.glyph, glyphPosition.presentation);

            if (attributes)
            {
                auto const pen1 = applyGlyphPositionToPen(pen, *attributes, glyphPosition);
                renderRasterizedGlyph(pen1, _textClusterGroup.color, *attributes);

                auto xOffset = unbox(textureAtlas().tileSize().width);
                while (AtlasTileAttributes const* subAttribs = textureAtlas().try_get(hash * xOffset))
                {
                    renderTile(atlas::RenderTile::X { pen1.x + int(xOffset) },
                               atlas::RenderTile::Y { pen1.y },
                               _textClusterGroup.color,
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
        _textRendererEvents.onAfterRenderingText();
    }

    _textClusterGroup.resetAndMovePenForward(_textClusterGroup.cellCount
                                             * unbox<int>(_gridMetrics.cellSize.width));
    _textStartFound = false;
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
                auto const subWidth = Width::cast_from(xNext - xOffset);
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
    auto const emojiBoundingBox =
        ImageSize { Width(_gridMetrics.cellSize.width.value * numCells),
                    Height::cast_from(unbox<int>(_gridMetrics.cellSize.height) - _gridMetrics.baseline) };
    if (glyph.format == text::bitmap_format::rgba)
    {
        if (glyph.bitmapSize.height > Height::cast_from(unbox<double>(emojiBoundingBox.height) * 1.1)
            || glyph.bitmapSize.width > Width::cast_from(unbox<double>(emojiBoundingBox.width) * 1.5))
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
            rasterizerLog()(" ==> scaled: {}/{}, factor {}", scaledGlyph, emojiBoundingBox, scaleFactor);
        }
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
        glyph.bitmapSize.height += Height::cast_from(yMin);
        auto& data = glyph.bitmap;
        data.erase(begin(data), next(begin(data), (int) pixelCount)); // XXX asan hit (size = -2)
        Guarantee(glyph.valid());
    }
    // }}}

    if (rasterizerLog)
    {
        auto const boundingBox =
            ImageSize { Width(_gridMetrics.cellSize.width.value * numCells),
                        Height::cast_from(unbox<int>(_gridMetrics.cellSize.height) - _gridMetrics.baseline) };
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

text::shape_result const& TextRenderer::getOrCreateCachedGlyphPositions(strong_hash hash)
{
    return _textShapingCache->get_or_emplace(hash, [this](auto) { return createTextShapedGlyphPositions(); });
}

text::shape_result TextRenderer::createTextShapedGlyphPositions()
{
    auto glyphPositions = text::shape_result {};

    auto run = unicode::run_segmenter::range {};
    auto rs = unicode::run_segmenter(
        u32string_view(_textClusterGroup.codepoints.data(), _textClusterGroup.codepoints.size()));
    while (rs.consume(out(run)))
        for (text::glyph_position& glyphPosition: shapeTextRun(run))
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
text::shape_result TextRenderer::shapeTextRun(unicode::run_segmenter::range const& run)
{
    // TODO(where to apply cell-advances) auto const advanceX = _gridMetrics.cellSize.width;
    auto const count = static_cast<size_t>(run.end - run.start);
    auto const codepoints = u32string_view(_textClusterGroup.codepoints.data() + run.start, count);
    auto const clusters = gsl::span(_textClusterGroup.clusters.data() + run.start, count);
    auto const script = get<unicode::Script>(run.properties);
    auto const presentationStyle = get<unicode::PresentationStyle>(run.properties);
    auto const isEmojiPresentation = presentationStyle == unicode::PresentationStyle::Emoji;
    auto const font = isEmojiPresentation ? _fonts.emoji : getFontForStyle(_fonts, _textClusterGroup.style);

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
        for (auto const [i, codepoint]: crispy::indexed(codepoints))
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
// }}}

} // namespace terminal::rasterizer
