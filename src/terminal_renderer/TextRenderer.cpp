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
#include <terminal_renderer/BoxDrawingRenderer.h>
#include <terminal_renderer/GridMetrics.h>
#include <terminal_renderer/utils.h>

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
using std::make_unique;
using std::max;
using std::move;
using std::nullopt;
using std::optional;
using std::pair;
using std::u32string;
using std::u32string_view;
using std::vector;

using namespace std::placeholders;

namespace terminal::renderer {

namespace // {{{ helpers
{
    text::font_key getFontForStyle(FontKeys const& _fonts, TextStyle _style)
    {
        switch (_style)
        {
            case TextStyle::Invalid:
                break;
            case TextStyle::Regular:
                return _fonts.regular;
            case TextStyle::Bold:
                return _fonts.bold;
            case TextStyle::Italic:
                return _fonts.italic;
            case TextStyle::BoldItalic:
                return _fonts.boldItalic;
        }
        return _fonts.regular;
    }
} // }}}

TextRenderer::TextRenderer(GridMetrics const& _gridMetrics,
                           text::shaper& _textShaper,
                           FontDescriptions& _fontDescriptions,
                           FontKeys const& _fonts) :
    gridMetrics_{ _gridMetrics },
    fontDescriptions_{ _fontDescriptions },
    fonts_{ _fonts },
    textShaper_{ _textShaper },
    boxDrawingRenderer_{ _gridMetrics }
{
    setTextShapingMethod(fontDescriptions_.textShapingMethod);
}

void TextRenderer::setTextShapingMethod(TextShapingMethod _method)
{
    switch (_method)
    {
        case TextShapingMethod::Complex:
            textRenderingEngine_ = make_unique<ComplexTextShaper>(
                gridMetrics_,
                textShaper_,
                fonts_,
                std::bind(&TextRenderer::renderRun, this, _1, _2, _3)
            );
            return;
        case TextShapingMethod::Simple:
            textRenderingEngine_ = make_unique<SimpleTextShaper>(
                gridMetrics_,
                textShaper_,
                fonts_,
                std::bind(&TextRenderer::renderRun, this, _1, _2, _3)
            );
            return;
    }
}

void TextRenderer::setRenderTarget(RenderTarget& _renderTarget)
{
    Renderable::setRenderTarget(_renderTarget);
    boxDrawingRenderer_.setRenderTarget(_renderTarget);
    clearCache();
}

void TextRenderer::clearCache()
{
    monochromeAtlas_ = make_unique<TextureAtlas>(renderTarget().monochromeAtlasAllocator());
    colorAtlas_ = make_unique<TextureAtlas>(renderTarget().coloredAtlasAllocator());
    lcdAtlas_ = make_unique<TextureAtlas>(renderTarget().lcdAtlasAllocator());

    textRenderingEngine_->clearCache();
    boxDrawingRenderer_.clearCache();
}

void TextRenderer::updateFontMetrics()
{
    setTextShapingMethod(fontDescriptions_.textShapingMethod);

    if (!renderTargetAvailable())
        return;

    clearCache();
}

/// Should box drawing fall back to font based box drawing?
// XXX #define BOXDRAWING_FONT_FALLBACK

void TextRenderer::renderCell(RenderCell const& _cell)
{
    auto const style = [](auto mask) constexpr -> TextStyle {
        if (contains_all(mask, CellFlags::Bold | CellFlags::Italic))
            return TextStyle::BoldItalic;
        if (mask & CellFlags::Bold)
            return TextStyle::Bold;
        if (mask & CellFlags::Italic)
            return TextStyle::Italic;
        return TextStyle::Regular;
    }(_cell.flags);

    auto const codepoints = crispy::span(_cell.codepoints.data(), _cell.codepoints.size());

    bool const isBoxDrawingCharacter =
        fontDescriptions_.builtinBoxDrawing &&
        _cell.codepoints.size() == 1 &&
        crispy::ascending(char32_t{0x2500}, codepoints[0], char32_t{0x257F});

    if (isBoxDrawingCharacter)
    {
        [[maybe_unused]] bool const couldRender = boxDrawingRenderer_.render(
            LinePosition::cast_from(_cell.position.row),
            ColumnPosition::cast_from(_cell.position.column),
            codepoints[0] % 0x2500,
            _cell.foregroundColor
        );
#if defined(BOXDRAWING_FONT_FALLBACK)
        if (couldRender)
#endif
        {
            if (!lastWasBoxDrawing_)
                textRenderingEngine_->endSequence();
            lastWasBoxDrawing_ = true;
            return;
        }
    }

    if (lastWasBoxDrawing_ || (_cell.flags & CellFlags::CellSequenceStart))
    {
        lastWasBoxDrawing_ = false;
        textRenderingEngine_->setTextPosition(gridMetrics_.map(_cell.position));
    }

    textRenderingEngine_->appendCell(codepoints, style, _cell.foregroundColor);

    if (_cell.flags & CellFlags::CellSequenceEnd)
        textRenderingEngine_->endSequence();
}

void TextRenderer::start()
{
    textRenderingEngine_->beginFrame();
}

void TextRenderer::finish()
{
    textRenderingEngine_->endSequence();
}

void TextRenderer::renderRun(crispy::Point _pos,
                             crispy::span<text::glyph_position const> _glyphPositions,
                             RGBColor _color)
{
    crispy::Point pen = _pos;
    auto const advanceX = *gridMetrics_.cellSize.width;

    for (text::glyph_position const& gpos: _glyphPositions)
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
    auto const numCells = colored ? 2u : 1u; // is this the only case - with colored := Emoji presentation?
    // FIXME: this `2` is a hack of my bad knowledge. FIXME.
    // As I only know of emojis being colored fonts, and those take up 2 cell with units.

    debuglog(TextRendererTag).write("Glyph metrics: {}", glyph);

    // {{{ scale bitmap down iff bitmap is emoji and overflowing in diemensions
    if (glyph.format == text::bitmap_format::rgba)
    {
        assert(colored && "RGBA should be only used on colored (i.e. emoji) fonts.");
        assert(numCells >= 2);
        auto const cellSize = gridMetrics_.cellSize;

        if (numCells > 1 && // XXX for now, only if emoji glyph
                (glyph.size.width > (cellSize.width * numCells)
              || glyph.size.height > cellSize.height))
        {
            auto const newSize = ImageSize{Width(*cellSize.width * numCells), cellSize.height};
            auto [scaled, factor] = text::scale(glyph, newSize);

            glyph.size = scaled.size; // TODO: there shall be only one with'x'height.

            // center the image in the middle of the cell
            glyph.position.y = gridMetrics_.cellSize.height.as<int>() - gridMetrics_.baseline;
            glyph.position.x = (gridMetrics_.cellSize.width.as<int>() * numCells - glyph.size.width.as<int>()) / 2;

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

    auto const yMax = gridMetrics_.baseline + glyph.position.y;
    auto const yMin = yMax - glyph.size.height.as<int>();

    auto const ratio = !colored
                     ? 1.0f
                     : max(float(gridMetrics_.cellSize.width.as<int>() * numCells) / float(glyph.size.width.as<int>()),
                           float(gridMetrics_.cellSize.height.as<int>()) / float(glyph.size.height.as<int>()));

    auto const yOverflow = gridMetrics_.cellSize.height.as<int>() - yMax;
    if (crispy::debugtag::enabled(TextRendererTag))
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
        glyph.size.height += Height(yOverflow);
        glyph.position.y += yOverflow;
    }

    if (yMin < 0)
    {
        auto const rowCount = -yMin;
        auto const pixelCount = rowCount * glyph.size.width.as<int>() * text::pixel_size(glyph.format);
        debuglog(TextRendererTag).write("Cropping {} underflowing bitmap rows.", rowCount);
        glyph.size.height += Height(yMin);
        auto& data = glyph.bitmap;
        assert(pixelCount >= 0);
        data.erase(begin(data), next(begin(data), pixelCount)); // XXX asan hit (size = -2)
    }

    GlyphMetrics metrics{};
    metrics.bitmapSize = glyph.size;
    metrics.bearing = glyph.position;

    if (crispy::debugtag::enabled(TextRendererTag))
        debuglog(TextRendererTag).write("textureAtlas ({}) insert glyph {}: {}; ratio:{}; yOverflow({}, {}); {}",
                                        targetAtlas.allocator().name(),
                                        _id.index,
                                        colored ? "emoji" : "text",
                                        ratio,
                                        yOverflow < 0 ? yOverflow : 0,
                                        yMin < 0 ? yMin : 0,
                                        glyph);

    return targetAtlas.insert(_id,
                              glyph.size,
                              glyph.size * ratio,
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

        auto const y = _pos.y                                     // bottom left
                     + _glyphPos.offset.y                         // -> harfbuzz adjustment
                     + gridMetrics_.baseline                      // -> baseline
                     + _glyphMetrics.bearing.y                    // -> bitmap top
                     - _glyphMetrics.bitmapSize.height.as<int>()  // -> bitmap height
                     ;

        renderTexture(crispy::Point{x, y}, _color, _textureInfo);
    }

#if 0
    if (crispy::debugtag::enabled(TextRendererTag))
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

    _textOutput << fmt::format("TextRenderer: {} cache entries:\n", orderedKeys.size());
    for (auto && [word, key] : orderedKeys)
    {
        auto const vword = u32string_view(word);
        _textOutput << fmt::format("  {}\n", unicode::convert_to<char>(vword));
    }
}

// {{{ ComplexTextShaper
ComplexTextShaper::ComplexTextShaper(GridMetrics const& _gridMetrics,
                                     text::shaper& _textShaper,
                                     FontKeys const& _fonts,
                                     RenderGlyphs _renderGlyphs):
    gridMetrics_{ _gridMetrics },
    fonts_{ _fonts },
    textShaper_{ _textShaper },
    renderGlyphs_{ std::move(_renderGlyphs) }
{
}

void ComplexTextShaper::clearCache()
{
    cacheKeyStorage_.clear();
    cache_.clear();
}

void ComplexTextShaper::appendCell(crispy::span<char32_t const> _codepoints,
                                   TextStyle _style,
                                   RGBColor _color)
{
    bool const attribsChanged = _color != color_ || _style != style_;
    bool const hasText = !_codepoints.empty() && _codepoints[0] != 0x20;
    bool const noText = !hasText;
    bool const textStartFound = !textStartFound_ && hasText;
    if (noText)
        textStartFound_ = false;
    if (attribsChanged || textStartFound || noText)
    {
        if (cellCount_)
            endSequence(); // also increments text start position
        color_ = _color;
        style_ = _style;
        textStartFound_ = textStartFound;
    }

    for (char32_t const codepoint: _codepoints)
    {
        codepoints_.emplace_back(codepoint);
        clusters_.emplace_back(cellCount_);
    }
    cellCount_++;
}

void ComplexTextShaper::beginFrame()
{
    assert(codepoints_.empty());
    assert(clusters_.empty());

    auto constexpr DefaultColor = RGBColor{};
    style_ = TextStyle::Invalid;
    color_ = DefaultColor;
}

void ComplexTextShaper::setTextPosition(crispy::Point _position)
{
    textPosition_ = _position;
    // std::cout << fmt::format("ComplexTextShaper.sequenceStart: {}\n", textPosition_);
}

void ComplexTextShaper::endSequence()
{
    // std::cout << fmt::format("ComplexTextShaper.equenceEnd({}): {}+{}\n",
    //         textPosition_.x / gridMetrics_.cellSize.width,
    //         textPosition_, cellCount_);

    if (!codepoints_.empty())
    {
        text::shape_result const& glyphPositions = cachedGlyphPositions();
        renderGlyphs_(textPosition_, crispy::span(glyphPositions.data(), glyphPositions.size()), color_);
    }

    codepoints_.clear();
    clusters_.clear();
    textPosition_.x += static_cast<int>(*gridMetrics_.cellSize.width * cellCount_);
    cellCount_ = 0;
    textStartFound_ = false;
}

text::shape_result const& ComplexTextShaper::cachedGlyphPositions()
{
    auto const codepoints = u32string_view(codepoints_.data(), codepoints_.size());
    if (auto const cached = cache_.find(TextCacheKey{codepoints, style_}); cached != cache_.end())
        return cached->second;

    cacheKeyStorage_.emplace_back(u32string{codepoints});
    auto const cacheKeyFromStorage = TextCacheKey{ cacheKeyStorage_.back(), style_ };

    return cache_[cacheKeyFromStorage] = requestGlyphPositions();
}

text::shape_result ComplexTextShaper::requestGlyphPositions()
{
    text::shape_result glyphPositions;
    unicode::run_segmenter::range run;
    auto rs = unicode::run_segmenter(codepoints_.data(), codepoints_.size());
    while (rs.consume(out(run)))
        crispy::copy(shapeRun(run), std::back_inserter(glyphPositions));

    return glyphPositions;
}

text::shape_result ComplexTextShaper::shapeRun(unicode::run_segmenter::range const& _run)
{
    bool const isEmojiPresentation = std::get<unicode::PresentationStyle>(_run.properties) == unicode::PresentationStyle::Emoji;

    auto const font = isEmojiPresentation ? fonts_.emoji : getFontForStyle(fonts_, style_);

    // TODO(where to apply cell-advances) auto const advanceX = gridMetrics_.cellSize.width;
    auto const count = static_cast<int>(_run.end - _run.start);
    auto const codepoints = u32string_view(codepoints_.data() + _run.start, count);
    auto const clusters = crispy::span(clusters_.data() + _run.start, count);

    text::shape_result gpos;
    textShaper_.shape(
        font,
        codepoints,
        clusters,
        std::get<unicode::Script>(_run.properties),
        gpos
    );

    if (crispy::debugtag::enabled(TextRendererTag) && !gpos.empty())
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
// }}}

// {{{ SimpleTextShaper
SimpleTextShaper::SimpleTextShaper(GridMetrics const& _gridMetrics,
                                   text::shaper& _textShaper,
                                   FontKeys const& _fonts,
                                   RenderGlyphs _renderGlyphs):
    gridMetrics_{ _gridMetrics },
    fonts_{ _fonts },
    textShaper_{ _textShaper },
    renderGlyphs_{ std::move(_renderGlyphs) }
{
}

void SimpleTextShaper::setTextPosition(crispy::Point _position)
{
    textPosition_ = _position;
}

void SimpleTextShaper::appendCell(crispy::span<char32_t const> _codepoints, TextStyle _style, RGBColor _color)
{
    if (color_ != _color)
    {
        flush();
        color_ = _color;
    }

    optional<text::glyph_position> glyphPositionOpt = textShaper_.shape(getFontForStyle(fonts_, _style), _codepoints[0]);
    if (!glyphPositionOpt.has_value())
        return;

    glyphPositions_.emplace_back(glyphPositionOpt.value());
    cellCount_++;
}

text::shape_result SimpleTextShaper::cachedGlyphPositions(crispy::span<char32_t const> _codepoints, TextStyle _style)
{
    auto const codepoints = u32string_view(&_codepoints[0], _codepoints.size());
    if (auto const cached = cache_.find(TextCacheKey{codepoints, _style}); cached != cache_.end())
        return cached->second;

    auto glyphPositionOpt = textShaper_.shape(getFontForStyle(fonts_, _style), _codepoints[0]);
    if (!glyphPositionOpt.has_value())
        return {};

    cacheKeyStorage_.emplace_back(u32string{codepoints});
    auto const cacheKeyFromStorage = TextCacheKey{ cacheKeyStorage_.back(), _style };

    return cache_[cacheKeyFromStorage] = {glyphPositionOpt.value()};
}

void SimpleTextShaper::endSequence()
{
    flush();
}

void SimpleTextShaper::flush()
{
    if (glyphPositions_.empty())
        return;

    text::shape_result const& glyphPositions = glyphPositions_;
    renderGlyphs_(textPosition_, crispy::span(glyphPositions.data(), glyphPositions.size()), color_);
    glyphPositions_.clear();
    textPosition_.x += gridMetrics_.cellSize.width.as<int>() * cellCount_;
    cellCount_ = 0;
}
// }}}

} // end namespace
