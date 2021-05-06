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

#include <terminal_renderer/TextRenderer.h>
#include <terminal_renderer/GridMetrics.h>

#include <crispy/algorithm.h>
#include <crispy/debuglog.h>
#include <crispy/times.h>
#include <crispy/range.h>

#include <unicode/convert.h>

#include <fmt/format.h>
#include <fmt/ostream.h>

using crispy::copy;
using crispy::times;

using unicode::out;

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

namespace terminal::renderer {

namespace {
    auto const TextRendererTag = crispy::debugtag::make("renderer.text", "Logs details about text rendering.");
}

TextRenderer::TextRenderer(GridMetrics const& _gridMetrics,
                           text::shaper& _textShaper,
                           FontDescriptions& _fontDescriptions,
                           FontKeys const& _fonts) :
    gridMetrics_{ _gridMetrics },
    fontDescriptions_{ _fontDescriptions },
    fonts_{ _fonts },
    textShaper_{ _textShaper }
{
}

void TextRenderer::setRenderTarget(RenderTarget& _renderTarget)
{
    Renderable::setRenderTarget(_renderTarget);
    clearCache();
}

void TextRenderer::clearCache()
{
    monochromeAtlas_ = std::make_unique<TextureAtlas>(renderTarget().monochromeAtlasAllocator());
    colorAtlas_ = std::make_unique<TextureAtlas>(renderTarget().coloredAtlasAllocator());
    lcdAtlas_ = std::make_unique<TextureAtlas>(renderTarget().lcdAtlasAllocator());

    cacheKeyStorage_.clear();
    cache_.clear();

#if !defined(NDEBUG)
    cacheHits_.clear();
#endif
}

void TextRenderer::updateFontMetrics()
{
    clearCache();
}

void TextRenderer::reset(Coordinate const& _pos, CharacterStyleMask const& _styles, RGBColor const& _color)
{
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
        color_
    );
}

text::shape_result const& TextRenderer::cachedGlyphPositions()
{
    auto const codepoints = u32string_view(codepoints_.data(), codepoints_.size());
    if (auto const cached = cache_.find(CacheKey{codepoints, characterStyleMask_}); cached != cache_.end())
    {
#if !defined(NDEBUG)
        cacheHits_[cached->first]++;
#endif
        return cached->second;
    }

    cacheKeyStorage_.emplace_back(u32string{codepoints});
    auto const cacheKeyFromStorage = CacheKey{ cacheKeyStorage_.back(), characterStyleMask_ };

#if !defined(NDEBUG)
    cacheHits_[cacheKeyFromStorage] = 0;
#endif

    return cache_[cacheKeyFromStorage] = requestGlyphPositions();
}

text::shape_result TextRenderer::requestGlyphPositions()
{
    text::shape_result glyphPositions;
    unicode::run_segmenter::range run;
    auto rs = unicode::run_segmenter(codepoints_.data(), codepoints_.size());
    while (rs.consume(out(run)))
        crispy::copy(shapeRun(run), std::back_inserter(glyphPositions));

    return glyphPositions;
}

text::shape_result TextRenderer::shapeRun(unicode::run_segmenter::range const& _run)
{
    if ((characterStyleMask_ & CharacterStyleMask::Hidden))
        return {};

    // auto const weight = characterStyleMask_ & CharacterStyleMask::Bold
    //     ? text::font_weight::bold
    //     : text::font_weight::normal;
    //
    // auto const slant = characterStyleMask_ & CharacterStyleMask::Italic
    //     ? text::font_slant::italic
    //     : text::font_slant::normal;

    if (characterStyleMask_ & CharacterStyleMask::Blinking)
    {
        // TODO: update textshaper's shader to blink (requires current clock knowledge)
    }

    bool const isEmojiPresentation = std::get<unicode::PresentationStyle>(_run.properties) == unicode::PresentationStyle::Emoji;

    auto font = [&]() -> text::font_key {
        if (isEmojiPresentation)
            return fonts_.emoji;
        if (characterStyleMask_ & (CharacterStyleMask::Mask::Bold | CharacterStyleMask::Mask::Italic))
            return fonts_.boldItalic;
        if (characterStyleMask_ & CharacterStyleMask::Mask::Bold)
            return fonts_.bold;
        if (characterStyleMask_ & CharacterStyleMask::Mask::Italic)
            return fonts_.italic;

        return fonts_.regular;
    }();

    // TODO(where to apply cell-advances) auto const advanceX = gridMetrics_.cellSize.width;
    auto const count = static_cast<int>(_run.end - _run.start);
    auto const codepoints = u32string_view(codepoints_.data() + _run.start, count);
    auto const clusters = crispy::span(clusters_.data() + _run.start, count);
    // XXX auto const clusterGap = -static_cast<int>(clusters_[0]);

    text::shape_result gpos;
    textShaper_.shape(
        font,
        codepoints,
        clusters,
        std::get<unicode::Script>(_run.properties),
        gpos
    );

    if (crispy::logging_sink::for_debug().enabled() && !gpos.empty())
    {
        auto msg = debuglog(TextRendererTag);
        msg.write("Shaped codepoints: {}", unicode::convert_to<char>(codepoints));
        msg.write("  (presentation: {}/{})",
                isEmojiPresentation ? "emoji" : "text",
                get<unicode::PresentationStyle>(_run.properties));

        msg.write(" (");
        for (auto const [i, codepoint] : crispy::indexed(codepoints))
        {
            if (i)
                msg.write(" ");
            msg.write("U+{:04X}", unsigned(codepoint));
        }
        msg.write(")\n");

        // A single shape run always uses the same font,
        // so it is sufficient to just print that.
        // auto const& font = gpos.front().glyph.font;
        // msg.write("using font: \"{}\" \"{}\" \"{}\"\n", font.familyName(), font.styleName(), font.filePath());

        msg.write("with metrics:");
        for (text::glyph_position const& gp : gpos)
            msg.write(" {}", gp);
    }

    return gpos;
}

void TextRenderer::finish()
{
    state_ = State::Empty;
    codepoints_.clear();
}

void TextRenderer::render(crispy::Point _pos,
                          text::shape_result const& _glyphPositions,
                          RGBAColor const& _color)
{
    crispy::Point pen = _pos;
    auto const advanceX = gridMetrics_.cellSize.width;

    for (text::glyph_position const& gpos : _glyphPositions)
    {
        if (optional<DataRef> const ti = getTextureInfo(gpos.glyph); ti.has_value())
        {
            renderTexture(pen,
                          _color,
                          get<0>(*ti).get(), // TextureInfo
                          get<1>(*ti).get(), // Metadata
                          gpos);
        }

        if (gpos.advance.x)
        {
            // Only advance horizontally, as we're (guess what) a terminal. :-)
            // Only advance in fixed-width steps.
            // Only advance iff there harfbuzz told us to.
            pen.x += advanceX;
        }
    }
}

TextRenderer::TextureAtlas& TextRenderer::atlasForFont(text::font_key _font)
{
    if (textShaper_.has_color(_font))
        return *colorAtlas_;

    switch (fontDescriptions_.renderMode)
    {
        case text::render_mode::lcd:
            // fallthrough; return lcdAtlas_;
            return *lcdAtlas_;
        case text::render_mode::color:
            return *colorAtlas_;
        case text::render_mode::light:
        case text::render_mode::gray:
        case text::render_mode::bitmap:
            return *monochromeAtlas_;
    }

    return *monochromeAtlas_;
}

optional<TextRenderer::DataRef> TextRenderer::getTextureInfo(text::glyph_key const& _id)
{
    if (auto i = glyphToTextureMapping_.find(_id); i != glyphToTextureMapping_.end())
        if (TextureAtlas* ta = atlasForBitmapFormat(i->second); ta != nullptr)
            if (optional<DataRef> const dataRef = ta->get(_id); dataRef.has_value())
                return dataRef;

    bool const colored = textShaper_.has_color(_id.font);

    auto theGlyphOpt = textShaper_.rasterize(_id, fontDescriptions_.renderMode);
    if (!theGlyphOpt.has_value())
        return nullopt;

    text::rasterized_glyph& glyph = theGlyphOpt.value();
    auto const numCells = colored ? 2 : 1; // is this the only case - with colored := Emoji presentation?
    // FIXME: this `2` is a hack of my bad knowledge. FIXME.
    // As I only know of emojis being colored fonts, and those take up 2 cell with units.

    debuglog(TextRendererTag).write("Glyph metrics: {}", glyph);
    // auto const xMax = glyph.left + glyph.width;
    // if (xMax > gridMetrics_.cellSize.width * numCells)
    // {
    //     debuglog(TextRendererTag).write("Glyph width {}+{}={} exceeds cell width {}.",
    //                                     glyph.left,
    //                                     glyph.width,
    //                                     xMax,
    //                                     gridMetrics_.cellSize.width * numCells);
    // }

    // {{{ scale bitmap down iff bitmap is emoji and overflowing in diemensions
    if (glyph.format == text::bitmap_format::rgba)
    {
        assert(colored && "RGBA should be only used on colored (i.e. emoji) fonts.");
        assert(numCells >= 2);
        auto const cellSize = gridMetrics_.cellSize;

        if (numCells > 1 && // XXX for now, only if emoji glyph
                (glyph.width > cellSize.width * numCells
              || glyph.height > cellSize.height))
        {
            auto [scaled, factor] = text::scale(glyph, cellSize.width * numCells, cellSize.height);

            glyph.width = scaled.width;
            glyph.height = scaled.height; // TODO: there shall be only one with'x'height.

            // center the image in the middle of the cell
            glyph.top = gridMetrics_.cellSize.height - gridMetrics_.baseline;
            glyph.left = (gridMetrics_.cellSize.width * numCells - glyph.width) / 2;

            // (old way)
            // glyph.metrics.bearing.x /= factor;
            // glyph.metrics.bearing.y /= factor;

            glyph.bitmap = move(scaled.bitmap);

            // XXX currently commented out because it's not used.
            // TODO: But it should be used for cutting the image off the right edge with unnecessary
            // transparent pixels.
            //
            // int const rightEdge = [&]() {
            //     auto rightEdge = std::numeric_limits<int>::max();
            //     for (int x = glyph.bitmap.width - 1; x >= 0; --x) {
            //         for (int y = 0; y < glyph.bitmap.height; ++y)
            //         {
            //             auto const& pixel = &glyph.bitmap.data.at(y * glyph.bitmap.width * 4 + x * 4);
            //             if (pixel[3] > 20)
            //                 rightEdge = x;
            //         }
            //         if (rightEdge != std::numeric_limits<int>::max())
            //             break;
            //     }
            //     return rightEdge;
            // }();
            // if (rightEdge != std::numeric_limits<int>::max())
            //     debuglog(TextRendererTag).write("right edge found. {} < {}.", rightEdge+1, glyph.bitmap.width);
        }
    }
    // }}}

    auto const yMax = gridMetrics_.baseline + glyph.top;
    auto const yMin = yMax - glyph.height;

    auto const ratio = !colored
                     ? 1.0f
                     : max(float(gridMetrics_.cellSize.width * numCells) / float(glyph.width),
                           float(gridMetrics_.cellSize.height) / float(glyph.height));

    auto const yOverflow = gridMetrics_.cellSize.height - yMax;
    if (crispy::logging_sink::for_debug().enabled())
        debuglog(TextRendererTag).write("insert glyph {}: {}; ratio:{}; yOverflow({}, {}); {}",
                                        _id.index,
                                        colored ? "emoji" : "text",
                                        ratio,
                                        yOverflow < 0 ? yOverflow : 0,
                                        yMin < 0 ? yMin : 0,
                                        glyph);

    auto && [userFormat, targetAtlas] = [this](bool _colorFont, text::bitmap_format _glyphFormat) -> pair<int, TextureAtlas&> { // {{{
        // this format ID is used by the fragment shader to select the right texture atlas
        if (_colorFont)
            return {1, *colorAtlas_};
        switch (_glyphFormat)
        {
            case text::bitmap_format::rgba:
                return {1, *colorAtlas_};
            case text::bitmap_format::rgb:
                return {2, *lcdAtlas_};
            case text::bitmap_format::alpha_mask:
                return {0, *monochromeAtlas_};
        }
        return {0, *monochromeAtlas_};
    }(colored, glyph.format); // }}}

    glyphToTextureMapping_[_id] = glyph.format;

    if (yOverflow < 0)
    {
        debuglog(TextRendererTag).write("Cropping {} overflowing bitmap rows.", -yOverflow);
        glyph.height += yOverflow;
        glyph.top += yOverflow;
    }

    if (yMin < 0)
    {
        auto const rowCount = -yMin;
        auto const pixelCount = rowCount * glyph.width * text::pixel_size(glyph.format);
        debuglog(TextRendererTag).write("Cropping {} underflowing bitmap rows.", rowCount);
        glyph.height += yMin;
        auto& data = glyph.bitmap;
        data.erase(begin(data), next(begin(data), pixelCount));
    }

    GlyphMetrics metrics{};
    metrics.bitmapSize.x = glyph.width;
    metrics.bitmapSize.y = glyph.height;
    metrics.bearing.x = glyph.left;
    metrics.bearing.y = glyph.top;

    if (crispy::logging_sink::for_debug().enabled())
        debuglog(TextRendererTag).write("textureAtlas ({}) insert glyph {}: {}; ratio:{}; yOverflow({}, {}); {}",
                                        targetAtlas.allocator().name(),
                                        _id.index,
                                        colored ? "emoji" : "text",
                                        ratio,
                                        yOverflow < 0 ? yOverflow : 0,
                                        yMin < 0 ? yMin : 0,
                                        glyph);

    return targetAtlas.insert(_id,
                              glyph.width,
                              glyph.height,
                              unsigned(ceilf(float(glyph.width) * ratio)),
                              unsigned(ceilf(float(glyph.height) * ratio)),
                              move(glyph.bitmap),
                              userFormat,
                              metrics);
}

void TextRenderer::renderTexture(crispy::Point const& _pos,
                                 RGBAColor const& _color,
                                 atlas::TextureInfo const& _textureInfo,
                                 GlyphMetrics const& _glyphMetrics,
                                 text::glyph_position const& _glyphPos)
{
    auto const colored = textShaper_.has_color(_glyphPos.glyph.font);

    if (colored)
    {
        auto const x = _pos.x
                     + _glyphMetrics.bearing.x
                     + _glyphPos.offset.x
                     ;

        auto const y = _pos.y;

        renderTexture(crispy::Point{x, y}, _color, _textureInfo);
    }
    else
    {
        auto const x = _pos.x
                     + _glyphMetrics.bearing.x
                     + _glyphPos.offset.x
                     ;

        // auto const y = _pos.y() + _gpos.y + baseline + _glyph.descender;
        auto const y = _pos.y                       // bottom left
                     + _glyphPos.offset.y           // -> harfbuzz adjustment
                     + gridMetrics_.baseline        // -> baseline
                     + _glyphMetrics.bearing.y      // -> bitmap top
                     - _glyphMetrics.bitmapSize.y   // -> bitmap height
                     ;

        renderTexture(crispy::Point{x, y}, _color, _textureInfo);
    }

#if 0
    if (crispy::logging_sink::for_debug().enabled())
        debuglog(TextRendererTag).write("xy={}:{} pos=({}:{}) tex={}x{}, gpos=({}:{}), baseline={}, descender={}",
                                        x, y,
                                        _pos.x(), _pos.y(),
                                        _textureInfo.width, _textureInfo.height,
                                        _glyphPos.offset.x, _glyphPos.offset.y,
                                        textShaper_.metrics(_glyphPos.glyph.font).baseline(),
                                        _glyph.descender);
#endif
}

void TextRenderer::renderTexture(crispy::Point const& _pos,
                                 RGBAColor const& _color,
                                 atlas::TextureInfo const& _textureInfo)
{
    // TODO: actually make x/y/z all signed (for future work, i.e. smooth scrolling!)
    auto const x = _pos.x;
    auto const y = _pos.y;
    auto const z = 0;
    auto const color = array{
        float(_color.red()) / 255.0f,
        float(_color.green()) / 255.0f,
        float(_color.blue()) / 255.0f,
        float(_color.alpha()) / 255.0f,
    };
    textureScheduler().renderTexture({_textureInfo, x, y, z, color});
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
        auto const vword = u32string_view(word);
#if !defined(NDEBUG)
        auto hits = int64_t{};
        if (auto i = cacheHits_.find(key); i != cacheHits_.end())
            hits = i->second;
        _textOutput << fmt::format("{:>5} : {}\n", hits, unicode::convert_to<char>(vword));
#else
        _textOutput << fmt::format("  {}\n", unicode::convert_to<char>(vword));
#endif
    }
}

} // end namespace
