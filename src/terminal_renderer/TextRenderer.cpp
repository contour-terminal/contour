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

#include <terminal/logging.h>
#include <terminal/primitives.h>

#include <terminal_renderer/BoxDrawingRenderer.h>
#include <terminal_renderer/GridMetrics.h>
#include <terminal_renderer/TextRenderer.h>
#include <terminal_renderer/shared_defines.h>
#include <terminal_renderer/utils.h>

#include <text_shaper/fontconfig_locator.h>
#include <text_shaper/mock_font_locator.h>

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

#include <unicode/convert.h>
#include <unicode/utf8_grapheme_segmenter.h>

#include <fmt/core.h>
#include <fmt/ostream.h>

#include <range/v3/algorithm/copy.hpp>

#include <algorithm>

using crispy::Point;
using crispy::StrongHash;

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

namespace terminal::renderer
{

using text::LocatorLog;

namespace
{
    constexpr auto FirstReservedChar = char32_t { 0x21 };
    constexpr auto LastReservedChar = char32_t { 0x7E };
    constexpr auto DirectMappedCharsCount = LastReservedChar - FirstReservedChar + 1;

    StrongHash hashGlyphKeyAndPresentation(text::glyph_key const& glyphKey,
                                           unicode::PresentationStyle presentation) noexcept
    {
        // return StrongHash::compute(key) * static_cast<uint32_t>(presentation);
        // clang-format off
        return StrongHash::compute(glyphKey.font.value)
               * static_cast<uint32_t>(glyphKey.index.value)
               * StrongHash::compute(glyphKey.size.pt)
               * static_cast<uint32_t>(presentation)
               ;
        // clang-format on
    }

    StrongHash hashTextAndStyle(u32string_view text, TextStyle style) noexcept
    {
        return StrongHash::compute(text) * static_cast<uint32_t>(style);
    }

    text::font_key getFontForStyle(FontKeys const& _fonts, TextStyle _style)
    {
        switch (_style)
        {
            case TextStyle::Invalid: break;
            case TextStyle::Regular: return _fonts.regular;
            case TextStyle::Bold: return _fonts.bold;
            case TextStyle::Italic: return _fonts.italic;
            case TextStyle::BoldItalic: return _fonts.boldItalic;
        }
        return _fonts.regular;
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
} // namespace

unique_ptr<text::font_locator> createFontLocator(FontLocatorEngine _engine)
{
    switch (_engine)
    {
        case FontLocatorEngine::Mock: return make_unique<text::mock_font_locator>();
        case FontLocatorEngine::DWrite:
#if defined(_WIN32)
            return make_unique<text::directwrite_locator>();
#else
            LocatorLog()("Font locator DirectWrite not supported on this platform.");
#endif
            break;
        case FontLocatorEngine::CoreText:
#if defined(__APPLE__)
            return make_unique<text::coretext_locator>();
#else
            LocatorLog()("Font locator CoreText not supported on this platform.");
#endif
            break;

        case FontLocatorEngine::FontConfig:
            // default case below
            break;
    }

    LocatorLog()("Using font locator: fontconfig.");
    return make_unique<text::fontconfig_locator>();
}

// TODO: What's a good value here? Or do we want to make that configurable,
// or even computed based on memory resources available?
constexpr uint32_t TextShapingCacheSize = 4000;

TextRenderer::TextRenderer(GridMetrics const& gridMetrics,
                           text::shaper& _textShaper,
                           FontDescriptions& _fontDescriptions,
                           FontKeys const& _fonts):
    Renderable { gridMetrics },
    fontDescriptions_ { _fontDescriptions },
    fonts_ { _fonts },
    textShapingCache_ { ShapingResultCache::create(crispy::StrongHashtableSize { 16384 },
                                                   crispy::LRUCapacity { TextShapingCacheSize },
                                                   "Text shaping cache") },
    textShaper_ { _textShaper },
    boxDrawingRenderer_ { _gridMetrics }
{
}

void TextRenderer::inspect(ostream& _textOutput) const
{
    _textOutput << "TextRenderer:\n";
    textShapingCache_->inspect(_textOutput);
    boxDrawingRenderer_.inspect(_textOutput);
}

void TextRenderer::setRenderTarget(
    RenderTarget& renderTarget, atlas::DirectMappingAllocator<RenderTileAttributes>& directMappingAllocator)
{
    _directMapping = directMappingAllocator.allocate(DirectMappedCharsCount);
    Renderable::setRenderTarget(renderTarget, directMappingAllocator);
    boxDrawingRenderer_.setRenderTarget(renderTarget, directMappingAllocator);
    clearCache();
}

void TextRenderer::setTextureAtlas(TextureAtlas& atlas)
{
    Renderable::setTextureAtlas(atlas);
    boxDrawingRenderer_.setTextureAtlas(atlas);

    if (_directMapping)
        initializeDirectMapping();
}

void TextRenderer::clearCache()
{
    if (_textureAtlas && _directMapping)
        initializeDirectMapping();

    textShapingCache_->clear();

    boxDrawingRenderer_.clearCache();
}

void TextRenderer::initializeDirectMapping()
{
    Require(_textureAtlas);
    Require(_directMapping.count == DirectMappedCharsCount);

    auto constexpr presentation = unicode::PresentationStyle::Text;

    _directMappedGlyphKeyToTileIndex.clear();
    _directMappedGlyphKeyToTileIndex.resize(LastReservedChar + 1);

    for (char32_t codepoint = FirstReservedChar; codepoint <= LastReservedChar; ++codepoint)
    {
        optional<text::glyph_position> gposOpt = textShaper_.shape(fonts_.regular, codepoint);
        if (!gposOpt)
            continue;
        text::glyph_position& gpos = *gposOpt;

        if (gpos.glyph.index.value >= _directMappedGlyphKeyToTileIndex.size())
            _directMappedGlyphKeyToTileIndex.resize(gpos.glyph.index.value
                                                    + (LastReservedChar - codepoint + 1));

        auto const tileIndex = _directMapping.toTileIndex(codepoint - FirstReservedChar);
        auto const tileLocation = _textureAtlas->tileLocation(tileIndex);
        auto tileCreateData = createRasterizedGlyph(tileLocation, gpos.glyph, presentation);
        if (!tileCreateData)
            continue;

        // Require(tileCreateData->bitmapSize.width <= textureAtlas().tileSize().width);

        // fmt::print("Initialize direct mapping {} ({}) for {} {}; {}; {}\n",
        //            tileIndex,
        //            tileLocation,
        //            codepoint,
        //            gpos.glyph,
        //            tileCreateData->bitmapSize,
        //            tileCreateData->metadata);

        _textureAtlas->setDirectMapping(tileIndex, move(*tileCreateData));
        _directMappedGlyphKeyToTileIndex[gpos.glyph.index.value] = tileIndex;
    }
}

void TextRenderer::updateFontMetrics()
{
    if (!renderTargetAvailable())
        return;

    clearCache();
}

void TextRenderer::beginFrame()
{
    // fmt::print("beginFrame: {} / {}\n", codepoints_.size(), clusters_.size());
    Require(textClusterGroup_.codepoints.empty());
    Require(textClusterGroup_.clusters.empty());

    auto constexpr DefaultColor = RGBColor {};
    textClusterGroup_.style = TextStyle::Invalid;
    textClusterGroup_.color = DefaultColor;
}

void TextRenderer::renderLine(RenderLine const& renderLine)
{
    if (renderLine.text.empty())
        return;

    auto const textStyle = makeTextStyle(renderLine.flags);

    auto graphemeClusterSegmenter = unicode::utf8_grapheme_segmenter(renderLine.text);
    auto columnOffset = ColumnOffset(0);

    textClusterGroup_.initialPenPosition =
        _gridMetrics.map(CellLocation { renderLine.lineOffset, columnOffset });

    for (u32string const& graphemeCluster: graphemeClusterSegmenter)
    {
        auto const gridPosition = CellLocation { renderLine.lineOffset, columnOffset };
        renderCell(gridPosition, graphemeCluster, textStyle, renderLine.foregroundColor);

        auto const width =
            static_cast<uint8_t>(unicode::width(graphemeCluster.front())); // TODO(pr): respect U+FEOF
        columnOffset += ColumnOffset::cast_from(width);
    }
    flushTextClusterGroup();
}

void TextRenderer::renderCell(RenderCell const& cell)
{
    if (cell.groupStart)
        updateInitialPenPosition_ = true;

    renderCell(cell.position, cell.codepoints, makeTextStyle(cell.flags), cell.foregroundColor);

    if (cell.groupEnd)
        flushTextClusterGroup();
}

void TextRenderer::renderCell(CellLocation position,
                              std::u32string_view codepoints,
                              TextStyle textStyle,
                              RGBColor foregroundColor)
{
    if (updateInitialPenPosition_)
    {
        updateInitialPenPosition_ = false;
        textClusterGroup_.initialPenPosition = _gridMetrics.map(position);
    }

    bool const isBoxDrawingCharacter = fontDescriptions_.builtinBoxDrawing && codepoints.size() == 1
                                       && boxDrawingRenderer_.renderable(codepoints[0]);

    if (isBoxDrawingCharacter)
    {
        auto const success =
            boxDrawingRenderer_.render(position.line, position.column, codepoints[0], foregroundColor);
        if (success)
        {
            if (!updateInitialPenPosition_)
                flushTextClusterGroup();
            updateInitialPenPosition_ = true;
            return;
        }
    }

    appendCellTextToClusterGroup(codepoints, textStyle, foregroundColor);
}

void TextRenderer::endFrame()
{
    flushTextClusterGroup();
}

Point TextRenderer::applyGlyphPositionToPen(Point pen,
                                            AtlasTileAttributes const& tileAttributes,
                                            text::glyph_position const& gpos) const noexcept
{
    auto const& glyphMetrics = tileAttributes.metadata;

    auto const x = pen.x + glyphMetrics.x.value + gpos.offset.x;

    // Emoji are simple square bitmap fonts that do not need special positioning.
    auto const y = pen.y                                        // -> base pen position
                   + _gridMetrics.baseline                      // -> baseline
                   + glyphMetrics.y.value                       // -> bitmap top
                   + gpos.offset.y                              // -> harfbuzz adjustment
                   - tileAttributes.bitmapSize.height.as<int>() // -> bitmap height
        ;

    // fmt::print("pen! {} <- {}, gpos {}, glyph offset {}x+{}y, glyph height {} ({})\n",
    //            Point { x, y },
    //            pen,
    //            gpos,
    //            glyphMetrics.x.value,
    //            glyphMetrics.y.value,
    //            tileAttributes.bitmapSize.height,
    //            gpos.presentation);

    return Point { x, y };
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
void TextRenderer::renderRasterizedGlyph(crispy::Point pen,
                                         RGBAColor _color,
                                         AtlasTileAttributes const& attributes)
{
    // clang-format off
    // RasterizerLog()("render glyph pos {} tile {} offset {}:{}",
    //                 _pos,
    //                 _glyphLocationInAtlas,
    //                 _glyphPos.offset.x,
    //                 _glyphPos.offset.y);
    // clang-format on

    renderTile(atlas::RenderTile::X { pen.x }, atlas::RenderTile::Y { pen.y }, _color, attributes);

    // clang-format off
    // if (RasterizerLog)
    //     RasterizerLog()("xy={}:{} pos={} tex={}, gpos=({}:{}), baseline={}",
    //                     x, y,
    //                     _pos,
    //                     _glyphBitmapSize,
    //                     _glyphPos.offset.x, _glyphPos.offset.y,
    //                     _gridMetrics.baseline);
    // clang-format on
}

void TextRenderer::appendCellTextToClusterGroup(u32string_view _codepoints, TextStyle _style, RGBColor _color)
{
    bool const attribsChanged = _color != textClusterGroup_.color || _style != textClusterGroup_.style;
    bool const hasText = !_codepoints.empty() && _codepoints[0] != 0x20;
    bool const noText = !hasText;
    bool const textStartFound = !textStartFound_ && hasText;
    if (noText)
        textStartFound_ = false;
    if (attribsChanged || textStartFound || noText)
    {
        if (textClusterGroup_.cellCount)
            flushTextClusterGroup(); // also increments text start position
        textClusterGroup_.color = _color;
        textClusterGroup_.style = _style;
        textStartFound_ = textStartFound;
    }

    for (char32_t const codepoint: _codepoints)
    {
        textClusterGroup_.codepoints.emplace_back(codepoint);
        textClusterGroup_.clusters.emplace_back(textClusterGroup_.cellCount);
    }
    textClusterGroup_.cellCount++;
}

void TextRenderer::flushTextClusterGroup()
{
    if (!textClusterGroup_.codepoints.empty())
    {
        // fmt::print("TextRenderer.flushTextClusterGroup: textPos={}, cellCount={}, width={}, count={}\n",
        //            textClusterGroup_.initialPenPosition.x, textClusterGroup_.cellCount,
        //            _gridMetrics.cellSize.width,
        //            textClusterGroup_.codepoints.size());

        auto hash = hashTextAndStyle(
            u32string_view(textClusterGroup_.codepoints.data(), textClusterGroup_.codepoints.size()),
            textClusterGroup_.style);
        text::shape_result const& glyphPositions = getOrCreateCachedGlyphPositions(hash);
        crispy::Point pen = textClusterGroup_.initialPenPosition;
        auto const advanceX = *_gridMetrics.cellSize.width;

        for (text::glyph_position const& glyphPosition: glyphPositions)
        {
            if (isGlyphDirectMapped(glyphPosition.glyph))
            {
                auto const directMappingIndex =
                    _directMappedGlyphKeyToTileIndex[glyphPosition.glyph.index.value];
                AtlasTileAttributes const& attributes = _textureAtlas->directMapped(directMappingIndex);
                auto const pen1 = applyGlyphPositionToPen(pen, attributes, glyphPosition);
                renderRasterizedGlyph(pen1, textClusterGroup_.color, attributes);
                pen.x += static_cast<decltype(pen.x)>(advanceX);
                continue;
            }

            auto const hash = hashGlyphKeyAndPresentation(glyphPosition.glyph, glyphPosition.presentation);

            AtlasTileAttributes const* attributes =
                getOrCreateRasterizedMetadata(hash, glyphPosition.glyph, glyphPosition.presentation);

            if (attributes)
            {
                auto const pen1 = applyGlyphPositionToPen(pen, *attributes, glyphPosition);
                renderRasterizedGlyph(pen1, textClusterGroup_.color, *attributes);

                auto xOffset = unbox<uint32_t>(textureAtlas().tileSize().width);
                while (AtlasTileAttributes const* subAttribs = textureAtlas().try_get(hash * xOffset))
                {
                    renderTile(atlas::RenderTile::X { pen1.x + int(xOffset) },
                               atlas::RenderTile::Y { pen1.y },
                               textClusterGroup_.color,
                               *subAttribs);
                    xOffset += unbox<uint32_t>(textureAtlas().tileSize().width);
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

    textClusterGroup_.resetAndMovePenForward(textClusterGroup_.cellCount
                                             * unbox<int>(_gridMetrics.cellSize.width));
    textStartFound_ = false;
}

Renderable::AtlasTileAttributes const* TextRenderer::getOrCreateRasterizedMetadata(
    StrongHash const& hash, text::glyph_key const& glyphKey, unicode::PresentationStyle presentationStyle)
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
                                               StrongHash const& hash)
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
                                      move(bitmap),
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
                            move(headBitmap),
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
    auto theGlyphOpt = textShaper_.rasterize(glyphKey, fontDescriptions_.renderMode);
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

    // Scale bitmap down iff bitmap is emoji and overflowing in diemensions
    if (presentation == unicode::PresentationStyle::Emoji && glyph.format == text::bitmap_format::rgba)
    {
        auto const emojiBoundingBox =
            ImageSize { Width(_gridMetrics.cellSize.width.value * numCells),
                        Height::cast_from(unbox<int>(_gridMetrics.cellSize.height) - _gridMetrics.baseline) };
        if (glyph.bitmapSize.height > emojiBoundingBox.height)
        {
            auto [scaledGlyph, scaleFactor] = text::scale(glyph, emojiBoundingBox);
            glyph = move(scaledGlyph);
            // glyph.position.y = unbox<int>(glyph.bitmapSize.height) - _gridMetrics.underline.position;
        }
    }

    // y-position relative to cell-bottom of glyphs top.
    auto const yMax = _gridMetrics.baseline + glyph.position.y;

    // y-position relative to cell-bottom of the glyphs bottom.
    auto const yMin = yMax - glyph.bitmapSize.height.as<int>();

    // Number of pixel lines this rasterized glyph is overflowing above cell-top,
    // or 0 if not overflowing.
    auto const yOverflow = max(0, yMax - _gridMetrics.cellSize.height.as<int>());

    // {{{ crop underflow if yMin < 0
    // If the rasterized glyph is underflowing below the grid cell's minimum (0),
    // then cut off at grid cell's bottom.
    if (false) // (yMin < 0)
    {
        auto const rowCount = (unsigned) -yMin;
        Require(rowCount <= unbox<unsigned>(glyph.bitmapSize.height));
        auto const pixelCount =
            rowCount * unbox<unsigned>(glyph.bitmapSize.width) * text::pixel_size(glyph.format);
        Require(0 < pixelCount && static_cast<size_t>(pixelCount) <= glyph.bitmap.size());
        RasterizerLog()("Cropping {} underflowing bitmap rows.", rowCount);
        glyph.bitmapSize.height += Height::cast_from(yMin);
        auto& data = glyph.bitmap;
        data.erase(begin(data), next(begin(data), (int) pixelCount)); // XXX asan hit (size = -2)
        Guarantee(glyph.valid());
    }
    // }}}

    if (RasterizerLog)
        RasterizerLog()("Inserting {} id {} render mode {} {} yOverflow {} yMin {}.",
                        glyph,
                        glyphKey.index,
                        fontDescriptions_.renderMode,
                        presentation,
                        yOverflow,
                        yMin);

    return { createTileData(tileLocation,
                            move(glyph.bitmap),
                            toAtlasFormat(glyph.format),
                            glyph.bitmapSize,
                            RenderTileAttributes::X { glyph.position.x },
                            RenderTileAttributes::Y { glyph.position.y },
                            toFragmentShaderSelector(glyph.format)) };
}

text::shape_result const& TextRenderer::getOrCreateCachedGlyphPositions(StrongHash hash)
{
    return textShapingCache_->get_or_emplace(hash, [this](auto) { return createTextShapedGlyphPositions(); });
}

text::shape_result TextRenderer::createTextShapedGlyphPositions()
{
    auto glyphPositions = text::shape_result {};

    auto run = unicode::run_segmenter::range {};
    auto rs = unicode::run_segmenter(
        u32string_view(textClusterGroup_.codepoints.data(), textClusterGroup_.codepoints.size()));
    while (rs.consume(out(run)))
        for (text::glyph_position const& glyphPosition: shapeTextRun(run))
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
text::shape_result TextRenderer::shapeTextRun(unicode::run_segmenter::range const& _run)
{
    bool const isEmojiPresentation =
        get<unicode::PresentationStyle>(_run.properties) == unicode::PresentationStyle::Emoji;

    auto const font = isEmojiPresentation ? fonts_.emoji : getFontForStyle(fonts_, textClusterGroup_.style);

    // TODO(where to apply cell-advances) auto const advanceX = _gridMetrics.cellSize.width;
    auto const count = static_cast<size_t>(_run.end - _run.start);
    auto const codepoints = u32string_view(textClusterGroup_.codepoints.data() + _run.start, count);
    auto const clusters = gsl::span(textClusterGroup_.clusters.data() + _run.start, count);

    text::shape_result glyphPosition;
    glyphPosition.reserve(clusters.size());
    textShaper_.shape(font,
                      codepoints,
                      clusters,
                      get<unicode::Script>(_run.properties),
                      get<unicode::PresentationStyle>(_run.properties),
                      glyphPosition);

    if (RasterizerLog && !glyphPosition.empty())
    {
        auto msg = RasterizerLog();
        msg.append("Shaped codepoints ({}/{}): {}",
                   isEmojiPresentation ? "emoji" : "text",
                   get<unicode::PresentationStyle>(_run.properties),
                   unicode::convert_to<char>(codepoints));

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

} // namespace terminal::renderer
