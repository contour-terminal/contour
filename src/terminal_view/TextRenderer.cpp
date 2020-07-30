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

#include <crispy/times.h>
#include <crispy/algorithm.h>

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
using crispy::text::GlyphBitmap;
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
                           ColorProfile const& _colorProfile,
                           FontConfig const& _fonts) :
    renderMetrics_{ _renderMetrics },
    screenCoordinates_{ _screenCoordinates },
    colorProfile_{ _colorProfile },
    fonts_{ _fonts },
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
}

void TextRenderer::setCellSize(CellSize const& _cellSize)
{
    cellSize_ = _cellSize;
}

void TextRenderer::setColorProfile(ColorProfile const& _colorProfile)
{
    colorProfile_ = _colorProfile;
}

void TextRenderer::setFont(FontConfig const& _fonts)
{
    fonts_ = _fonts;
    clearCache();
}

void TextRenderer::reset(Coordinate const& _pos, GraphicsAttributes const& _attr)
{
    //std::cout << fmt::format("TextRenderer.reset(): attr:{}\n", _attr.styles);
    row_ = _pos.row;
    startColumn_ = _pos.column;
    attributes_ = _attr;
    codepoints_.clear();
    clusters_.clear();
    clusterOffset_ = 0;
}

void TextRenderer::extend(Cell const& _cell, [[maybe_unused]] cursor_pos_t _column)
{
    for (size_t const i: times(_cell.codepointCount()))
    {
        codepoints_.emplace_back(_cell.codepoint(i));
        clusters_.emplace_back(clusterOffset_);
    }
    ++clusterOffset_;
}

void TextRenderer::schedule(Coordinate const& _pos, Cell const& _cell)
{
    constexpr char32_t SP = 0x20;

    switch (state_)
    {
        case State::Empty:
            if (!_cell.empty() && _cell.codepoint(0) != SP)
            {
                state_ = State::Filling;
                reset(_pos, _cell.attributes());
                extend(_cell, _pos.column);
            }
            break;
        case State::Filling:
            if (!_cell.empty() && row_ == _pos.row && attributes_ == _cell.attributes() && _cell.codepoint(0) != SP)
                extend(_cell, _pos.column);
            else
            {
                flushPendingSegments();
                if (_cell.empty() || _cell.codepoint(0) == SP)
                    state_ = State::Empty;
                else // i.o.w.: cell attributes OR row number changed
                {
                    reset(_pos, _cell.attributes());
                    extend(_cell, _pos.column);
                }
            }
            break;
    }
}

void TextRenderer::flushPendingSegments()
{
    if (codepoints_.empty())
        return;

    auto const [fgColor, bgColor] = attributes_.makeColors(colorProfile_);

    render(
        #if 1
        screenCoordinates_.map(startColumn_, row_),
        #else
        QPoint(startColumn_ - 1, row_ - 1), // `- 1` because it'll be zero-indexed
        #endif
        cachedGlyphPositions(),
        QVector4D(
            static_cast<float>(fgColor.red) / 255.0f,
            static_cast<float>(fgColor.green) / 255.0f,
            static_cast<float>(fgColor.blue) / 255.0f,
            1.0f
        )
    );
}

GlyphPositionList const& TextRenderer::cachedGlyphPositions()
{
    auto const codepoints = u32string_view(codepoints_.data(), codepoints_.size());
    auto const key = CacheKey{codepoints, attributes_.styles};
    if (auto const cached = cache_.find(key); cached != cache_.end())
    {
        METRIC_INCREMENT(cachedText);
        return cached->second;
    }
    else
    {
        cacheKeyStorage_.emplace_back(u32string{codepoints});
        std::cout << fmt::format("TextRenderer.newEntry({}): {}\n",
                cacheKeyStorage_.size(),
                unicode::to_utf8(cacheKeyStorage_.back()).c_str());

        auto const cacheKeyFromStorage = CacheKey{
            cacheKeyStorage_.back(),
            attributes_.styles
        };

        return cache_[cacheKeyFromStorage] = requestGlyphPositions();
    }
}

GlyphPositionList TextRenderer::requestGlyphPositions()
{
    // if (attributes_.styles.mask() != 0)
    //     std::cout << fmt::format("TextRenderer.requestGlyphPositions: styles=({})\n", attributes_.styles);
    GlyphPositionList glyphPositions;
    unicode::run_segmenter::range run;
    auto rs = unicode::run_segmenter(codepoints_.data(), codepoints_.size());
    while (rs.consume(out(run)))
    {
        METRIC_INCREMENT(shapedText);
        crispy::copy(prepareRun(run), std::back_inserter(glyphPositions));
    }

    return glyphPositions;
}

GlyphPositionList TextRenderer::prepareRun(unicode::run_segmenter::range const& _run)
{
    FontStyle textStyle = FontStyle::Regular;

    if (attributes_.styles & CharacterStyleMask::Bold)
        textStyle |= FontStyle::Bold;

    if (attributes_.styles & CharacterStyleMask::Italic)
        textStyle |= FontStyle::Italic;

    if (attributes_.styles & CharacterStyleMask::Blinking)
    {
        // TODO: update textshaper's shader to blink (requires current clock knowledge)
    }

    if ((attributes_.styles & CharacterStyleMask::Hidden))
        return {};

    auto& textFont = [&](FontStyle _style) -> FontList& {
        switch (_style)
        {
            case FontStyle::Bold:
                return fonts_.bold;
            case FontStyle::Italic:
                return fonts_.italic;
            case FontStyle::BoldItalic:
                return fonts_.boldItalic;
            case FontStyle::Regular:
                return fonts_.regular;
        }
        return fonts_.regular;
    }(textStyle);

    bool const isEmojiPresentation = std::get<unicode::PresentationStyle>(_run.properties) == unicode::PresentationStyle::Emoji;
    FontList& font = isEmojiPresentation ? fonts_.emoji
                                         : textFont;

    auto const advanceX = fonts_.regular.first.get().maxAdvance();

#if 0 // {{{ debug print
    cout << fmt::format("GLRenderer.renderText({}:{}={}) [{}..{}) {}",
                        _lineNumber, _startColumn,
                        _startColumn + _clusters[_offset],
                        _offset, _offsetEnd,
                        isEmojiPresentation ? "E" : "T");
    for (size_t i = _offset; i < _offsetEnd; ++i)
        cout << fmt::format(" {}:{}", (unsigned) _codepoints[i], _clusters[i]);
    cout << endl;
#endif // }}}

    auto gpos = textShaper_.shape(
        std::get<unicode::Script>(_run.properties),
        font,
        advanceX,
        static_cast<int>(_run.end - _run.start),
        codepoints_.data() + _run.start,
        clusters_.data() + _run.start,
        -static_cast<int>(clusters_[0])
    );
    return gpos;
}

void TextRenderer::finish()
{
    state_ = State::Empty;
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
    optional<GlyphBitmap> bitmap = font.loadGlyphByIndex(_id.glyphIndex);
    if (!bitmap.has_value())
        return nullopt;

    auto const format = _id.font.get().hasColor() ? GL_RGBA : GL_RED;
    auto const colored = _id.font.get().hasColor() ? 1 : 0;

    //auto const cw = _id.font.get()->glyph->advance.x >> 6;
    // FIXME: this `* 2` is a hack of my bad knowledge. FIXME.
    // As I only know of emojis being colored fonts, and those take up 2 cell with units.
    auto const ratioX = colored ? static_cast<float>(cellSize_.width) * 2.0f / static_cast<float>(_id.font.get().bitmapWidth()) : 1.0f;
    auto const ratioY = colored ? static_cast<float>(cellSize_.height) / static_cast<float>(_id.font.get().bitmapHeight()) : 1.0f;

    auto metadata = Glyph{};
    metadata.advance = _id.font.get()->glyph->advance.x >> 6;
    metadata.bearing = QPoint(font->glyph->bitmap_left * ratioX, font->glyph->bitmap_top * ratioY);
    metadata.descender = (font->glyph->metrics.height >> 6) - font->glyph->bitmap_top;
    metadata.height = static_cast<unsigned>(font->height) >> 6;
    metadata.size = QPoint(static_cast<int>(font->glyph->bitmap.width), static_cast<int>(font->glyph->bitmap.rows));

#if 0
    if (_id.font.get().hasColor())
    {
        cout << "TextRenderer.insert: colored glyph "
             << _id.glyphIndex
             << ", advance:" << metadata.advance
             << ", descender:" << metadata.descender
             << ", height:" << metadata.height
             << " @ " << _id.font.get().filePath() << endl;
    }
#endif

    auto& bmp = bitmap.value();
    return _atlas.insert(_id, bmp.width, bmp.height,
                         static_cast<unsigned>(static_cast<float>(bmp.width) * ratioX),
                         static_cast<unsigned>(static_cast<float>(bmp.height) * ratioY),
                         format,
                         move(bmp.buffer),
                         colored,
                         metadata);
}

void TextRenderer::renderTexture(QPoint const& _pos,
                                 QVector4D const& _color,
                                 atlas::TextureInfo const& _textureInfo,
                                 Glyph const& _glyph,
                                 crispy::text::GlyphPosition const& _gpos)
{
#if defined(LIBTERMINAL_VIEW_NATURAL_COORDS) && LIBTERMINAL_VIEW_NATURAL_COORDS
    auto const x = _pos.x() + _gpos.x + _glyph.bearing.x();
    auto const y = _pos.y() + _gpos.y + _gpos.font.get().baseline() - _glyph.descender;
#else
    auto const x = _pos.x()
                 + _gpos.x
                 + _glyph.bearing.x();

    auto const y = _pos.y()
                 + _gpos.font.get().bitmapHeight()
                 + _gpos.y
                 ;
#endif

    // cout << fmt::format(
    //     "Text.render: xy={}:{} pos=({}:{}) gpos=({}:{}), baseline={}, lineHeight={}/{}, descender={}\n",
    //     x, y,
    //     _pos.x(), _pos.y(),
    //     _gpos.x, _gpos.y,
    //     _gpos.font.get().baseline(),
    //     _gpos.font.get().lineHeight(),
    //     _gpos.font.get().bitmapHeight(),
    //     _glyph.descender
    // );

    renderTexture(QPoint(x, y), _color, _textureInfo);

    //auto const z = 0u;
    //renderer_.scheduler().renderTexture({_textureInfo, x, y, z, _color});
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

} // end namespace
