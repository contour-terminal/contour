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

namespace terminal::view {

using namespace crispy;
using namespace std;
using unicode::out;

#if !defined(NDEBUG)
#define METRIC_INCREMENT(name) do { ++renderMetrics_. name ; } while (0)
#else
#define METRIC_INCREMENT(name) do {} while (0)
#endif

TextRenderer::TextRenderer(RenderMetrics& _renderMetrics,
                           ScreenCoordinates const& _screenCoordinates,
                           ColorProfile const& _colorProfile,
                           FontConfig const& _fonts,
                           ShaderConfig const& _textShaderConfig) :
    renderMetrics_{ _renderMetrics },
    screenCoordinates_{ _screenCoordinates },
    colorProfile_{ _colorProfile },
    fonts_{ _fonts },
    textShaper_{},
    textShader_{ createShader(_textShaderConfig) },
    textProjectionLocation_{ textShader_->uniformLocation("vs_projection") }
{
    initializeOpenGLFunctions();

    textShader_->bind();
    textShader_->setUniformValue("fs_monochromeTextures", 0);
    textShader_->setUniformValue("fs_colorTextures", 1);
}

void TextRenderer::clearCache()
{
    renderer_.clearCache();
    textShaper_.clearCache();
    cacheKeyStorage_.clear();
    cache_.clear();
}

void TextRenderer::setProjection(QMatrix4x4 const& _projectionMatrix)
{
    projectionMatrix_ = _projectionMatrix;
    renderer_.setProjection(_projectionMatrix);
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

void TextRenderer::reset(cursor_pos_t _row, cursor_pos_t _col, ScreenBuffer::GraphicsAttributes const& _attr)
{
    //std::cout << fmt::format("TextRenderer.reset(): attr:{}\n", _attr.styles);
    row_ = _row;
    startColumn_ = _col;
    attributes_ = _attr;
    codepoints_.clear();
    clusters_.clear();
    clusterOffset_ = 0;
}

void TextRenderer::extend(ScreenBuffer::Cell const& _cell, [[maybe_unused]] cursor_pos_t _column)
{
    for (size_t const i: times(_cell.codepointCount()))
    {
        codepoints_.emplace_back(_cell.codepoint(i));
        clusters_.emplace_back(clusterOffset_);
    }
    ++clusterOffset_;
}

void TextRenderer::schedule(cursor_pos_t _row, cursor_pos_t _col, Screen::Cell const& _cell)
{
    constexpr char32_t SP = 0x20;

    switch (state_)
    {
        case State::Empty:
            if (_cell.codepoint(0) != SP)
            {
                state_ = State::Filling;
                reset(_row, _col, _cell.attributes());
                extend(_cell, _col);
            }
            break;
        case State::Filling:
            if (row_ == _row && attributes_ == _cell.attributes() && _cell.codepoint(0) != SP)
                extend(_cell, _col);
            else
            {
                flushPendingSegments();
                if (_cell.empty() || _cell.codepoint(0) == SP)
                    state_ = State::Empty;
                else // i.o.w.: cell attributes OR row number changed
                {
                    reset(_row, _col, _cell.attributes());
                    extend(_cell, _col);
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
    renderer_.render(
        screenCoordinates_.map(startColumn_, row_),
        cachedGlyphPositions(),
        QVector4D(
            static_cast<float>(fgColor.red) / 255.0f,
            static_cast<float>(fgColor.green) / 255.0f,
            static_cast<float>(fgColor.blue) / 255.0f,
            1.0f
        ),
        QSize{
            static_cast<int>(cellWidth()),
            static_cast<int>(cellHeight())
        }
    );
}

crispy::text::GlyphPositionList const& TextRenderer::cachedGlyphPositions()
{
    auto const codepoints = u32string_view(codepoints_.data(), codepoints_.size());
    auto const key = CacheKey{
        codepoints,
        attributes_.styles
    };
    if (auto const cached = cache_.find(key); cached != cache_.end())
    {
        METRIC_INCREMENT(cachedText);
        return cached->second;
    }
    else
    {
        cacheKeyStorage_[codepoints] = u32string(codepoints);

        auto const cacheKeyFromStorage = CacheKey{
            cacheKeyStorage_[codepoints],
            attributes_.styles
        };

        return cache_[cacheKeyFromStorage] = requestGlyphPositions();
    }
}

crispy::text::GlyphPositionList TextRenderer::requestGlyphPositions()
{
    // if (attributes_.styles.mask() != 0)
    //     std::cout << fmt::format("TextRenderer.requestGlyphPositions: styles=({})\n", attributes_.styles);
    text::GlyphPositionList glyphPositions;
    unicode::run_segmenter::range run;
    auto rs = unicode::run_segmenter(codepoints_.data(), codepoints_.size());
    while (rs.consume(out(run)))
    {
        METRIC_INCREMENT(shapedText);
        copy(prepareRun(run), std::back_inserter(glyphPositions));
    }

    return glyphPositions;
}

text::GlyphPositionList TextRenderer::prepareRun(unicode::run_segmenter::range const& _run)
{
    text::FontStyle textStyle = text::FontStyle::Regular;

    if (attributes_.styles & CharacterStyleMask::Bold)
        textStyle |= text::FontStyle::Bold;

    if (attributes_.styles & CharacterStyleMask::Italic)
        textStyle |= text::FontStyle::Italic;

    if (attributes_.styles & CharacterStyleMask::Blinking)
    {
        // TODO: update textshaper's shader to blink (requires current clock knowledge)
    }

    if ((attributes_.styles & CharacterStyleMask::Hidden))
        return {};

    auto& textFont = [&](text::FontStyle _style) -> text::FontList& {
        switch (_style)
        {
            case text::FontStyle::Bold:
                return fonts_.bold;
            case text::FontStyle::Italic:
                return fonts_.italic;
            case text::FontStyle::BoldItalic:
                return fonts_.boldItalic;
            case text::FontStyle::Regular:
                return fonts_.regular;
        }
        return fonts_.regular;
    }(textStyle);

    bool const isEmojiPresentation = std::get<unicode::PresentationStyle>(_run.properties) == unicode::PresentationStyle::Emoji;
    text::FontList& font = isEmojiPresentation ? fonts_.emoji
                                               : textFont;

    unsigned const advanceX = fonts_.regular.first.get().maxAdvance();

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
        _run.end - _run.start,
        codepoints_.data() + _run.start,
        clusters_.data() + _run.start,
        -clusters_[0]
    );
    return gpos;
}

void TextRenderer::execute()
{
    textShader_->bind();
    textShader_->setUniformValue(textProjectionLocation_, projectionMatrix_);
    renderer_.execute();
    state_ = State::Empty;
}

} // end namespace
