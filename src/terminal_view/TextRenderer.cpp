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
#include <terminal_view/ScreenCoordinates.h>
#include <terminal_view/RenderMetrics.h>

#include <crispy/algorithm.h>
#include <crispy/logger.h>
#include <crispy/times.h>

using std::get;
using std::nullopt;
using std::optional;
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
                           ScreenCoordinates const& _screenCoordinates,
                           FontConfig const& _fonts,
                           Size const& _cellSize) :
    renderMetrics_{ _renderMetrics },
    screenCoordinates_{ _screenCoordinates },
    fonts_{ _fonts },
    cellSize_{ _cellSize },
    textShaper_{},
    commandListener_{ _commandListener },
    monochromeAtlas_{ _monochromeAtlasAllocator },
    colorAtlas_{ _colorAtlasAllocator }
{
}

void TextRenderer::clearCache()
{
    monochromeAtlas_.clear();
    colorAtlas_.clear();

    textShaper_.clearCache();

    cacheKeyStorage_.clear();
    cache_.clear();
#if !defined(NDEBUG)
    cacheHits_.clear();
#endif
}

void TextRenderer::setCellSize(Size const& _cellSize)
{
    cellSize_ = _cellSize;
}

void TextRenderer::setFont(FontConfig const& _fonts)
{
    fonts_ = _fonts;

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
        #if 1
        screenCoordinates_.map(startColumn_, row_),
        #else
        QPoint(startColumn_ - 1, row_ - 1), // `- 1` because it'll be zero-indexed
        #endif
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
        msg.write("with metrics: ");
        for (crispy::text::GlyphPosition const& gp : gpos)
        {
            gp.font.get().filePath();
            msg.write(" {}:{},{}",
                      gp.glyphIndex,
                      gp.renderOffset.x,
                      gp.renderOffset.y);
        }

        // msg.write("\n");
        // for (size_t const i : times(count))
        //     msg.write(" {:X}", static_cast<uint32_t>(codepoints[i]));
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
    #if 1
    for (crispy::text::GlyphPosition const& gpos : _glyphPositions)
        if (optional<DataRef> const ti = getTextureInfo(GlyphId{gpos.font, gpos.glyphIndex}); ti.has_value())
            renderTexture(_pos,
                          _color,
                          get<0>(*ti).get(), // TextureInfo
                          get<1>(*ti).get(), // Metadata
                          gpos);
    #else
    unsigned offset = 0;
    for (crispy::text::GlyphPosition const& gpos : _glyphPositions)
    {
        if (optional<DataRef> const ti = getTextureInfo(GlyphId{gpos.font, gpos.glyphIndex}); ti.has_value())
            renderTexture(QPoint(_pos.x() + offset, _pos.y()),
                          _color,
                          get<0>(*ti).get(), // TextureInfo
                          get<1>(*ti).get(), // Metadata
                          gpos);
        ++offset;
    }
    #endif
}

optional<TextRenderer::DataRef> TextRenderer::getTextureInfo(GlyphId const& _id)
{
    TextureAtlas& atlas = _id.font.get().hasColor()
        ? colorAtlas_
        : monochromeAtlas_;

    return getTextureInfo(_id, atlas);
}

optional<TextRenderer::DataRef> TextRenderer::getTextureInfo(GlyphId const& _id, TextureAtlas& _atlas)
{
    if (optional<DataRef> const dataRef = _atlas.get(_id); dataRef.has_value())
        return dataRef;

    Font& font = _id.font.get();
    auto theGlyphOpt = font.loadGlyphByIndex(_id.glyphIndex);
    if (!theGlyphOpt.has_value())
        return nullopt;

    auto& theGlyph = theGlyphOpt.value();
    auto const format = _id.font.get().hasColor() ? GL_RGBA : GL_RED;
    auto const colored = _id.font.get().hasColor() ? 1 : 0;

    // FIXME: this `* 2` is a hack of my bad knowledge. FIXME.
    // As I only know of emojis being colored fonts, and those take up 2 cell with units.
    auto const ratioX = colored ? static_cast<double>(cellSize_.width) * 2.0 / static_cast<float>(theGlyph.metrics.bitmapSize.x) : 1.0;
    auto const ratioY = colored ? static_cast<double>(cellSize_.height) / static_cast<float>(theGlyph.metrics.bitmapSize.y) : 1.0;

    GlyphMetrics metadata = theGlyph.metrics;
    if (colored)
    {
        metadata.bearing.x = int(ceil(metadata.bearing.x * ratioX));
        metadata.bearing.y = int(ceil(metadata.bearing.y * ratioY));
    }

    if (crispy::logging_sink::for_debug().enabled())
        //if (_id.font.get().hasColor())
            debuglog().write("insert glyph {}: {}; path:{}",
                             _id.glyphIndex,
                             metadata,
                             _id.font.get().filePath());

    return _atlas.insert(_id, theGlyph.metrics.bitmapSize.x, theGlyph.metrics.bitmapSize.y,
                         unsigned(float(theGlyph.metrics.bitmapSize.x) * ratioX),
                         unsigned(float(theGlyph.metrics.bitmapSize.y) * ratioY),
                         format,
                         move(theGlyph.bitmap),
                         colored,
                         metadata);
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
        auto const baseline = fonts_.regular.first.get().baseline();

        auto const x = _pos.x()
                     + _glyphPos.renderOffset.x
                     + _glyphMetrics.bearing.x
                     ;

        // auto const y = _pos.y() + _gpos.y + baseline + _glyph.descender;
        auto const y = _pos.y()                     // bottom left
                     + _glyphPos.renderOffset.y     // -> harfbuzz adjustment
                     + cellSize_.height             // -> top left
                     - baseline                     // -> base line
                     + _glyphMetrics.bearing.y      // -> bitmap top left
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
    commandListener_.renderTexture({_textureInfo, x, y, z, _color});
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
