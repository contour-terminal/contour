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

#include <terminal_view/TextRenderer.h>
#include <terminal_view/GridMetrics.h>
#include <terminal_view/RenderMetrics.h>

#include <crispy/text/Font.h>
#include <crispy/algorithm.h>
#include <crispy/logger.h>
#include <crispy/times.h>

using std::array;
using std::get;
using std::max;
using std::move;
using std::nullopt;
using std::optional;
using std::pair;
using std::u32string;
using std::u32string_view;
using std::vector;

using crispy::copy;
using crispy::text::Font;
using crispy::text::FontList;
using crispy::text::FontStyle;
using crispy::text::Glyph;
using crispy::text::GlyphMetrics;
using crispy::text::GlyphPositionList;
using crispy::times;

using unicode::out;

namespace atlas = crispy::atlas;

namespace terminal::view {

#if !defined(NDEBUG)
#define METRIC_INCREMENT(name) do { ++renderMetrics_. name ; } while (0)
#else
#define METRIC_INCREMENT(name) do {} while (0)
#endif

TextRenderer::TextRenderer(RenderMetrics& _renderMetrics,
                           crispy::atlas::CommandListener& _commandListener,
                           crispy::atlas::TextureAtlasAllocator& _monochromeAtlasAllocator,
                           crispy::atlas::TextureAtlasAllocator& _colorAtlasAllocator,
                           crispy::atlas::TextureAtlasAllocator& _lcdAtlasAllocator,
                           GridMetrics const& _gridMetrics,
                           FontConfig const& _fonts) :
    renderMetrics_{ _renderMetrics },
    gridMetrics_{ _gridMetrics },
    // Light should be default - but as soon as LCD is functional, that's default
    fonts_{ _fonts },
    textShaper_{},
    commandListener_{ _commandListener },
    monochromeAtlas_{ _monochromeAtlasAllocator },
    colorAtlas_{ _colorAtlasAllocator },
    lcdAtlas_{ _lcdAtlasAllocator }
{
    if (fonts_.emoji.first.get().hasColor())
        fonts_.emoji.first.get().selectSizeForWidth(gridMetrics_.cellSize.width);

    for (Font& fallbackFont : fonts_.emoji.second)
    {
        if (fallbackFont.hasColor())
            fallbackFont.selectSizeForWidth(gridMetrics_.cellSize.width);
    }
}

void TextRenderer::clearCache()
{
    monochromeAtlas_.clear();
    colorAtlas_.clear();
    lcdAtlas_.clear();

    textShaper_.clearCache();

    cacheKeyStorage_.clear();
    cache_.clear();
#if !defined(NDEBUG)
    cacheHits_.clear();
#endif
}

void TextRenderer::setFont(FontConfig const& _fonts)
{
    fonts_ = _fonts;

    fonts_.emoji.first.get().selectSizeForWidth(gridMetrics_.cellSize.width);
    for (Font& fallbackFont : fonts_.emoji.second)
        fallbackFont.selectSizeForWidth(gridMetrics_.cellSize.width);

    clearCache();
}

void TextRenderer::reset(Coordinate const& _pos, CharacterStyleMask const& _styles, RGBColor const& _color)
{
    // debuglog().write("styles:{}, color:{}", _styles, _color);

    row_ = _pos.row;
    startColumn_ = _pos.column;
    characterStyleMask_ = _styles;
    color_ = _color;
    codepoints_.clear();
    clusters_.clear();
    clusterOffset_ = 0;
}

void TextRenderer::extend(Cell const& _cell, [[maybe_unused]] int _column)
{
    for (size_t const i: times(_cell.codepointCount()))
    {
        codepoints_.emplace_back(_cell.codepoint(i));
        clusters_.emplace_back(clusterOffset_);
    }
    ++clusterOffset_;
}

void TextRenderer::schedule(Coordinate const& _pos, Cell const& _cell, RGBColor const& _color)
{
    constexpr char32_t SP = 0x20;

    bool const emptyCell = _cell.empty() || _cell.codepoint(0) == SP;

    switch (state_)
    {
        case State::Empty:
            if (!emptyCell)
            {
                state_ = State::Filling;
                reset(_pos, _cell.attributes().styles, _color);
                extend(_cell, _pos.column);
            }
            break;
        case State::Filling:
        {
            //if (!_cell.empty() && row_ == _pos.row && attributes_ == _cell.attributes() && _cell.codepoint(0) != SP)
            bool const sameLine = _pos.row == row_;
            bool const sameSGR = _cell.attributes().styles == characterStyleMask_ && _color == color_;

            // Do not perform multi-column text shaping when under rendering pressure.
            // This usually only happens in bandwidth heavy commands (such as cat), where
            // ligature rendering isn't that important anyways?
            // Performing multi-column text shaping under pressure would cause the cache to be
            // filled up needlessly with half printed words. We mitigate that by shaping cell-wise
            // in such cases.

            if (!pressure_ && !emptyCell && sameLine && sameSGR)
                extend(_cell, _pos.column);
            else
            {
                flushPendingSegments();
                if (emptyCell)
                    state_ = State::Empty;
                else // i.o.w.: cell attributes OR row number changed
                {
                    reset(_pos, _cell.attributes().styles, _color);
                    extend(_cell, _pos.column);
                }
            }
            break;
        }
    }
}

void TextRenderer::flushPendingSegments()
{
    if (codepoints_.empty())
        return;

    render(
        gridMetrics_.map(startColumn_, row_),
        cachedGlyphPositions(),
        QVector4D(
            static_cast<float>(color_.red) / 255.0f,
            static_cast<float>(color_.green) / 255.0f,
            static_cast<float>(color_.blue) / 255.0f,
            1.0f
        )
    );
}

GlyphPositionList const& TextRenderer::cachedGlyphPositions()
{
    auto const codepoints = u32string_view(codepoints_.data(), codepoints_.size());
    if (auto const cached = cache_.find(CacheKey{codepoints, characterStyleMask_}); cached != cache_.end())
    {
        METRIC_INCREMENT(cachedText);
#if !defined(NDEBUG)
        cacheHits_[cached->first]++;
#endif
        return cached->second;
    }
    else
    {
        cacheKeyStorage_.emplace_back(u32string{codepoints});

        auto const cacheKeyFromStorage = CacheKey{
            cacheKeyStorage_.back(),
            characterStyleMask_
        };

#if !defined(NDEBUG)
        cacheHits_[cacheKeyFromStorage] = 0;
#endif

        return cache_[cacheKeyFromStorage] = requestGlyphPositions();
    }
}

GlyphPositionList TextRenderer::requestGlyphPositions()
{
    // if (characterStyleMask_.mask() != 0)
    //     debuglog().write("TextRenderer.requestGlyphPositions: styles=({})", characterStyleMask_);

    GlyphPositionList glyphPositions;
    unicode::run_segmenter::range run;
    auto rs = unicode::run_segmenter(codepoints_.data(), codepoints_.size());
    while (rs.consume(out(run)))
    {
        METRIC_INCREMENT(shapedText);
        crispy::copy(shapeRun(run), std::back_inserter(glyphPositions));
    }

    return glyphPositions;
}

GlyphPositionList TextRenderer::shapeRun(unicode::run_segmenter::range const& _run)
{
    if ((characterStyleMask_ & CharacterStyleMask::Hidden))
        return {};

    FontStyle const textStyle = [](CharacterStyleMask const& _styles) -> FontStyle {
        auto const bold = _styles & CharacterStyleMask::Bold
            ? FontStyle::Bold
            : FontStyle::Regular;
        auto const italic = _styles & CharacterStyleMask::Italic
            ? FontStyle::Italic
            : FontStyle::Regular;
        return FontStyle::Regular | bold | italic;
    }(characterStyleMask_);

    if (characterStyleMask_ & CharacterStyleMask::Blinking)
    {
        // TODO: update textshaper's shader to blink (requires current clock knowledge)
    }

    bool const isEmojiPresentation = std::get<unicode::PresentationStyle>(_run.properties) == unicode::PresentationStyle::Emoji;

    FontList& font = [](FontConfig& _fonts, FontStyle _style, bool _isEmoji) -> FontList& {
        if (_isEmoji)
            return _fonts.emoji;

        switch (_style)
        {
            case FontStyle::Bold:
                return _fonts.bold;
            case FontStyle::Italic:
                return _fonts.italic;
            case FontStyle::BoldItalic:
                return _fonts.boldItalic;
            case FontStyle::Regular:
                return _fonts.regular;
        }
        return _fonts.regular;
    }(fonts_, textStyle, isEmojiPresentation);

    auto const& regularFont = fonts_.regular.first.get();
    auto const advanceX = regularFont.maxAdvance();
    auto const count = static_cast<int>(_run.end - _run.start);
    auto const codepoints = codepoints_.data() + _run.start;
    auto const clusters = clusters_.data() + _run.start;
    auto const clusterGap = -static_cast<int>(clusters_[0]);

    GlyphPositionList gpos = textShaper_.shape(
        std::get<unicode::Script>(_run.properties),
        font,
        advanceX,
        count,
        codepoints,
        clusters,
        clusterGap
    );

    if (crispy::logging_sink::for_debug().enabled())
    {
        auto msg = debuglog();
        msg.write("Shaping: {}\n", unicode::to_utf8(codepoints, count));
        // A single shape run always uses the same font,
        // so it is sufficient to just print that.
        msg.write("via font: \"{}\"\n", gpos.at(0).font.get().filePath());
        msg.write("with metrics:");
        for (crispy::text::GlyphPosition const& gp : gpos)
        {
            gp.font.get().filePath();
            msg.write(" {}:{},{}",
                      gp.glyphIndex,
                      gp.renderOffset.x,
                      gp.renderOffset.y);
        }

        // msg.write("\n");
        // msg.write("Codepoints:");
        // for (size_t const i : times(count))
        //     msg.write(" U+{:X}", static_cast<uint32_t>(codepoints[i]));
    }

    return gpos;
}

void TextRenderer::finish()
{
    state_ = State::Empty;
    codepoints_.clear();
}

void TextRenderer::render(QPoint _pos,
                          vector<crispy::text::GlyphPosition> const& _glyphPositions,
                          QVector4D const& _color)
{
    for (crispy::text::GlyphPosition const& gpos : _glyphPositions)
        if (optional<DataRef> const ti = getTextureInfo(GlyphId{gpos.font, gpos.glyphIndex}); ti.has_value())
            renderTexture(_pos,
                          _color,
                          get<0>(*ti).get(), // TextureInfo
                          get<1>(*ti).get(), // Metadata
                          gpos);
}

optional<TextRenderer::DataRef> TextRenderer::getTextureInfo(GlyphId const& _id)
{
    auto const colored = _id.font.get().hasColor();
    TextureAtlas& lookupAtlas = [&]() -> TextureAtlas& {
        if (colored)
            return colorAtlas_;
        switch (fonts_.renderMode)
        {
            case crispy::text::RenderMode::LCD:
                // fallthrough; return lcdAtlas_;
                return lcdAtlas_;
            case crispy::text::RenderMode::Color:
                return colorAtlas_;
            case crispy::text::RenderMode::Light:
            case crispy::text::RenderMode::Gray:
            case crispy::text::RenderMode::Bitmap:
                return monochromeAtlas_;
        }
        return monochromeAtlas_;
    }();
    // TODO: what if lookupAtlas != targetAtlas. the lookup should be decoupled

    if (optional<DataRef> const dataRef = lookupAtlas.get(_id); dataRef.has_value())
        return dataRef;

    Font& font = _id.font.get();
    auto theGlyphOpt = font.loadGlyphByIndex(_id.glyphIndex, fonts_.renderMode);
    if (!theGlyphOpt.has_value())
        return nullopt;

    Glyph& glyph = theGlyphOpt.value();
    auto const numCells = colored ? 2 : 1; // is this the only case - with colored := Emoji presentation?
    // FIXME: this `2` is a hack of my bad knowledge. FIXME.
    // As I only know of emojis being colored fonts, and those take up 2 cell with units.

    auto const xMax = glyph.metrics.bearing.x + glyph.metrics.bitmapSize.x;
    if (xMax > gridMetrics_.cellSize.width * numCells)
        debuglog().write("Glyph width {}+{}={} exceeds cell width {}.",
                         glyph.metrics.bearing.x,
                         glyph.metrics.bitmapSize.x,
                         xMax,
                         gridMetrics_.cellSize.width * numCells);

    auto const yMax = gridMetrics_.baseline + glyph.metrics.bearing.y;
    auto const yOverflow = gridMetrics_.cellSize.height - yMax;
    if (yMax > gridMetrics_.cellSize.height)
    {
        assert(yOverflow < 0);
        if (yOverflow > gridMetrics_.descender)
        {
            // shift down
            debuglog().write("Glyph height {}+{}={} exceeds cell height {}. With bitmap dimmension {}, shifting vertically by {}.",
                    gridMetrics_.baseline,
                    glyph.metrics.bearing.y,
                    yMax,
                    gridMetrics_.cellSize.height,
                    glyph.metrics.bitmapSize,
                    yOverflow);
            glyph.metrics.bearing.y += yOverflow;
        }
        else
        {
            debuglog().write("Glyph height {}+{}={} exceeds cell height {}. Bitmap dimmension is {}.",
                    gridMetrics_.baseline,
                    glyph.metrics.bearing.y,
                    yMax,
                    gridMetrics_.cellSize.height,
                    glyph.metrics.bitmapSize);
        }
    }

    auto const [ratioX, ratioY] = [&]() -> pair<double, double> {
        // only for emoji
        if (!colored)
            return {1.0, 1.0};
        else
            return {
                double(gridMetrics_.cellSize.width * numCells) / double(glyph.metrics.bitmapSize.x),
                double(gridMetrics_.cellSize.height) / double(glyph.metrics.bitmapSize.y)
            };
    }();
    auto ratio = max(ratioX, ratioY);

    if (glyph.bitmap.format == crispy::text::BitmapFormat::RGBA)
    {
        auto const cellSize = gridMetrics_.cellSize;

        // std::cout << fmt::format("colored: {}x{} metric.size: {}, cell.size: {}\n",
        //         glyph.bitmap.width, glyph.bitmap.height,
        //         glyph.metrics.bitmapSize, cellSize);
#if 1
        if (numCells > 1 && // XXX for now, only if emoji glyph
                (glyph.metrics.bitmapSize.x > cellSize.width * numCells
              || glyph.metrics.bitmapSize.y > cellSize.height))
        {
            auto [scaled, factor] = scale(glyph.bitmap, cellSize.width * numCells, cellSize.height);
            glyph.metrics.bearing.x /= factor;
            glyph.metrics.bearing.y /= factor;
            glyph.metrics.bitmapSize.x = scaled.width;
            glyph.metrics.bitmapSize.y = scaled.height; // TODO: there shall be only one with'x'height.
            glyph.bitmap = move(scaled);

            int rightEdge = std::numeric_limits<int>::max();
            for (int x = glyph.bitmap.width - 1; x >= 0; --x)
            {
                for (int y = 0; y < glyph.bitmap.height; ++y)
                {
                    auto const& pixel = &glyph.bitmap.data.at(y * glyph.bitmap.width * 4 + x * 4);
                    if (pixel[3] > 20)
                        rightEdge = x;
                }
                if (rightEdge != std::numeric_limits<int>::max())
                    break;
            }
            if (rightEdge != std::numeric_limits<int>::max())
            {
                printf("right edge found. %d < %d.\n", rightEdge+1, glyph.bitmap.width);
            }

            ratio = 1.0;// / factor; // pre-scale
        }
#else
        GlyphMetrics& metadata = glyph.metrics;
        metadata.bearing.x = int(ceil(metadata.bearing.x * ratio));
        metadata.bearing.y = int(ceil(metadata.bearing.y * ratio));
#endif
    }

    auto && [userFormat, targetAtlas] = [&]() -> pair<int, TextureAtlas&> { // {{{
        // this format ID is used by the fragment shader to select the right texture atlas
        if (colored)
            return {1, colorAtlas_};
        switch (glyph.bitmap.format)
        {
            case crispy::text::BitmapFormat::RGBA:
                return {1, colorAtlas_};
            case crispy::text::BitmapFormat::LCD:
                return {2, lcdAtlas_};
            case crispy::text::BitmapFormat::Gray:
                return {0, monochromeAtlas_};
        }
        return {0, monochromeAtlas_};
    }(); // }}}

    if (crispy::logging_sink::for_debug().enabled())
        debuglog().write("insert glyph {}: {}; path:{}",
                         _id.glyphIndex,
                         glyph.metrics,
                         _id.font.get().filePath());

    assert(&lookupAtlas == &targetAtlas);
    return targetAtlas.insert(_id,
                              glyph.metrics.bitmapSize.x,
                              glyph.metrics.bitmapSize.y,
                              unsigned(ceil(double(glyph.metrics.bitmapSize.x) * ratio)),
                              unsigned(ceil(double(glyph.metrics.bitmapSize.y) * ratio)),
                              move(glyph.bitmap.data),
                              userFormat,
                              glyph.metrics);
}

void TextRenderer::renderTexture(QPoint const& _pos,
                                 QVector4D const& _color,
                                 atlas::TextureInfo const& _textureInfo,
                                 GlyphMetrics const& _glyphMetrics,
                                 crispy::text::GlyphPosition const& _glyphPos)
{
    auto const colored = _glyphPos.font.get().hasColor();

    if (colored)
    {
        auto const x = _pos.x()
                     + _glyphMetrics.bearing.x
                     + _glyphPos.renderOffset.x
                     ;

        auto const y = _pos.y();

        renderTexture(QPoint(x, y), _color, _textureInfo);
    }
    else
    {
        auto const x = _pos.x()
                     + _glyphPos.renderOffset.x
                     + _glyphMetrics.bearing.x
                     ;

        // auto const y = _pos.y() + _gpos.y + baseline + _glyph.descender;
        auto const y = _pos.y()                     // bottom left
                     + _glyphPos.renderOffset.y     // -> harfbuzz adjustment
                     + gridMetrics_.baseline        // -> baseline
                     + _glyphMetrics.bearing.y      // -> bitmap top
                     - _glyphMetrics.bitmapSize.y   // -> bitmap height
                     ;

        renderTexture(QPoint(x, y), _color, _textureInfo);
    }

#if 0
    if (crispy::logging_sink::for_debug().enabled())
        debuglog().write("xy={}:{} pos=({}:{}) tex={}x{}, gpos=({}:{}), baseline={}, descender={}",
                         x, y,
                         _pos.x(), _pos.y(),
                         _textureInfo.width, _textureInfo.height,
                         _glyphPos.renderOffset.x, _glyphPos.renderOffset.y,
                         _glyphPos.font.get().baseline(),
                         _glyph.descender);
#endif
}

void TextRenderer::renderTexture(QPoint const& _pos,
                                 QVector4D const& _color,
                                 atlas::TextureInfo const& _textureInfo)
{
    // TODO: actually make x/y/z all signed (for future work, i.e. smooth scrolling!)
    auto const x = _pos.x();
    auto const y = _pos.y();
    auto const z = 0;
    auto const color = array{_color[0], _color[1], _color[2], _color[3]};
    commandListener_.renderTexture({_textureInfo, x, y, z, color});
}

void TextRenderer::debugCache(std::ostream& _textOutput) const
{
    std::map<u32string, CacheKey> orderedKeys;

    for (auto && [key, val] : cache_)
    {
        (void) val;
        orderedKeys[u32string(key.text)] = key;
    }

    _textOutput << fmt::format("TextRenderer: {} cache entries:\n", orderedKeys.size());
    for (auto && [word, key] : orderedKeys)
    {
#if !defined(NDEBUG)
        auto hits = int64_t{};
        if (auto i = cacheHits_.find(key); i != cacheHits_.end())
            hits = i->second;
        _textOutput << fmt::format("{:>5} : {}\n", hits, unicode::to_utf8(word));
#else
        _textOutput << fmt::format("  {}\n", unicode::to_utf8(word));
#endif
    }
}

} // end namespace
