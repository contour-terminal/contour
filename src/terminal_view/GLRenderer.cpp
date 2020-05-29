/**
 * This file is part of the "libterminal" project
 *   Copyright (c) 2019 Christian Parpart <christian@parpart.family>
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
#include <terminal_view/GLRenderer.h>
#include <crispy/times.h>
#include <unicode/ucd.h>
#include <unicode/ucd_ostream.h>
#include <unicode/run_segmenter.h>

using namespace std;
using namespace std::chrono;
using namespace std::placeholders;
using namespace crispy;

namespace terminal::view {

class GLRenderer::TextScheduler { // {{{
  public:
    using Flusher = std::function<void(TextScheduler const&)>;

    explicit TextScheduler(Flusher _flusher);

    constexpr cursor_pos_t row() const noexcept { return row_; }
    constexpr cursor_pos_t startColumn() const noexcept { return startColumn_; }
    constexpr ScreenBuffer::GraphicsAttributes attributes() const noexcept { return attributes_; }

    std::vector<char32_t> const& codepoints() const noexcept { return codepoints_; }
    std::vector<unsigned> const& clusters() const noexcept { return clusters_; }

    unicode::run_segmenter::range const& run() const noexcept { return run_; }

    void reset();
    void reset(cursor_pos_t _row, cursor_pos_t _col, ScreenBuffer::GraphicsAttributes const& _attr);
    void schedule(cursor_pos_t _row, cursor_pos_t _col, Screen::Cell const& _cell);
    void flush();

  private:
    void extend(ScreenBuffer::Cell const&);

  private:
    enum class State { Empty, Filling };

    State state_ = State::Empty;
    cursor_pos_t row_ = 1;
    cursor_pos_t startColumn_ = 1;
    ScreenBuffer::GraphicsAttributes attributes_ = {};
    std::vector<char32_t> codepoints_{};
    std::vector<unsigned> clusters_{};

    unicode::run_segmenter::range run_{};

    Flusher flusher_;
};

GLRenderer::TextScheduler::TextScheduler(Flusher _flusher)
    : flusher_{ move(_flusher) }
{
}

void GLRenderer::TextScheduler::reset()
{
    state_ = State::Empty;

    row_ = 1;
    startColumn_ = 1;
    attributes_ = {};
    codepoints_.clear();
    clusters_.clear();
}

void GLRenderer::TextScheduler::reset(cursor_pos_t _row, cursor_pos_t _col, ScreenBuffer::GraphicsAttributes const& _attr)
{
    state_ = State::Filling;
    row_ = _row;
    startColumn_ = _col;
    attributes_ = _attr;
    codepoints_.clear();
    clusters_.clear();
}

void GLRenderer::TextScheduler::extend(ScreenBuffer::Cell const& _cell)
{
    auto const cluster = codepoints_.size();
    for (size_t const i: crispy::times(_cell.codepointCount()))
    {
        codepoints_.emplace_back(_cell.codepoint(i));
        clusters_.emplace_back(cluster);
    }
}

void GLRenderer::TextScheduler::schedule(cursor_pos_t _row, cursor_pos_t _col, Screen::Cell const& _cell)
{
    // TODO: new scheduling algo with given procedure
    //
    // 1) fill up one line (& split words by spaces, right here?)
    //      case (State.Space, Char.Space) -> skip
    //      case (State.Space, Char.NoSpace) -> state <- Fill
    // 2) segment line into runs
    // 3) render each run
    // 4) start next line

    constexpr char32_t SP = 0x20;

    switch (state_)
    {
        case State::Empty:
            if (_cell.codepoint() != SP)
            {
                reset(_row, _col, _cell.attributes());
                state_ = State::Filling;
                extend(_cell);
            }
            break;
        case State::Filling:
            if (row_ == _row && attributes_ == _cell.attributes() && _cell.codepoint() != SP)
                extend(_cell);
            else
            {
                flush();
                if (_cell.codepoint() == SP)
                {
                    state_ = State::Empty;
                    reset();
                }
                else // i.o.w.: cell attributes OR row number changed
                {
                    reset(_row, _col, _cell.attributes());
                    extend(_cell);
                }
            }
            break;
    }
}

void GLRenderer::TextScheduler::flush()
{
    using unicode::out;

    if (codepoints_.size() != 0)
    {
        u32string codepoints;
        for (auto const& cp : codepoints_)
            codepoints.push_back(cp);

        auto rs = unicode::run_segmenter(codepoints.data(), codepoints.size());

        // cout << fmt::format("flush {} codepoints; \"", codepoints.size());
        // for (size_t i = 0; i < codepoints_.size(); ++i)
        //     cout << unicode::to_utf8(codepoints_[i]);
        // cout << '"' << endl;

        while (rs.consume(out(run_)))
            flusher_(*this);
    }
}
// }}}

GLRenderer::GLRenderer(Logger _logger,
                       text::FontList const& _regularFont,
                       text::FontList const& _emojiFont,
                       terminal::ColorProfile _colorProfile,
                       terminal::Opacity _backgroundOpacity,
                       ShaderConfig const& _backgroundShaderConfig,
                       ShaderConfig const& _textShaderConfig,
                       ShaderConfig const& _cursorShaderConfig,
                       QMatrix4x4 const& _projectionMatrix) :
    logger_{ move(_logger) },
    leftMargin_{ 0 },
    bottomMargin_{ 0 },
    colorProfile_{ _colorProfile },
    backgroundOpacity_{ _backgroundOpacity },
    regularFont_{ _regularFont },
    emojiFont_{ _emojiFont },
    projectionMatrix_{ _projectionMatrix },
    textShaper_{},
    textShader_{ createShader(_textShaderConfig) },
    textProjectionLocation_{ textShader_->uniformLocation("vs_projection") },
    textRenderer_{},
    cellBackground_{
        QSize(
            static_cast<int>(regularFont_.first.get().maxAdvance()),
            static_cast<int>(regularFont_.first.get().lineHeight())
        ),
        _projectionMatrix,
        _backgroundShaderConfig
    },
    cursor_{
        QSize(
            static_cast<int>(regularFont_.first.get().maxAdvance()),
            static_cast<int>(regularFont_.first.get().lineHeight())
        ),
        _projectionMatrix,
        CursorShape::Block, // TODO: should not be hard-coded; actual value be passed via render(terminal, now);
        canonicalColor(colorProfile_.cursor),
        _cursorShaderConfig
    }
{
    initializeOpenGLFunctions();

    glEnable(GL_BLEND);
    glBlendFuncSeparate(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA, GL_ONE, GL_ONE);
    //glBlendFunc(GL_SRC1_COLOR, GL_ONE_MINUS_SRC1_COLOR);

    textShader_->bind();
    textShader_->setUniformValue("fs_monochromeTextures", 0);
    textShader_->setUniformValue("fs_colorTextures", 1);
}

void GLRenderer::clearCache()
{
    textRenderer_.clearCache();
    textShaper_.clearCache();
}

void GLRenderer::setFont(crispy::text::Font& _font, crispy::text::FontFallbackList const& _fallback)
{
    regularFont_.first = _font;
    regularFont_.second = _fallback;
    clearCache();
}

bool GLRenderer::setFontSize(unsigned int _fontSize)
{
    if (_fontSize == regularFont_.first.get().fontSize())
        return false;

    for (auto& font: {regularFont_, emojiFont_})
    {
        font.first.get().setFontSize(_fontSize);
        for (auto& fallback : font.second)
            fallback.get().setFontSize(_fontSize);
    }

    // TODO: other font styles

    auto const cellWidth = regularFont_.first.get().maxAdvance();
    auto const cellHeight = regularFont_.first.get().lineHeight();
    auto const cellSize = QSize{static_cast<int>(cellWidth),
                                static_cast<int>(cellHeight)};

    clearCache();
    cellBackground_.resize(cellSize);
    cursor_.resize(cellSize);

    return true;
}

void GLRenderer::setProjection(QMatrix4x4 const& _projectionMatrix)
{
    projectionMatrix_ = _projectionMatrix;

    cellBackground_.setProjection(_projectionMatrix);
    textRenderer_.setProjection(_projectionMatrix);
    cursor_.setProjection(_projectionMatrix);
}

void GLRenderer::setBackgroundOpacity(terminal::Opacity _opacity)
{
    backgroundOpacity_ = _opacity;
}

void GLRenderer::setColorProfile(terminal::ColorProfile const& _colors)
{
    colorProfile_ = _colors;
    cursor_.setColor(canonicalColor(colorProfile_.cursor));
}

uint64_t GLRenderer::render(Terminal const& _terminal, steady_clock::time_point _now)
{
    metrics_.clear();
    pendingBackgroundDraw_ = {};

    auto ts = TextScheduler{
        [&](TextScheduler const& _textScheduler) {
            crispy::CodepointSequence codepoints{};

            // TODO: heavily poor performance. Make me more performant by reusing existing buffers
            // with zero copy (just indexing).
            for (size_t i = _textScheduler.run().start; i < _textScheduler.run().end; ++i)
                codepoints.push_back({_textScheduler.codepoints().at(i),
                                      _textScheduler.clusters().at(i)});

            // cout << "  run: " << run_ << "; ";
            // for (size_t i = 0; i < codepoints.size(); ++i)
            //     cout << fmt::format(" {}:{}", (unsigned) codepoints[i].value, codepoints_[run_.start + i].cluster);
            // cout << endl;

            renderText(
                _terminal.screenSize(),
                _textScheduler.row(),
                _textScheduler.startColumn() + _textScheduler.run().start,
                _textScheduler.attributes(),
                codepoints,
                _textScheduler.run().presentationStyle);
        }
    };

    auto const changes = _terminal.render(
        _now,
        bind(&GLRenderer::fillBackgroundGroup, this, _1, _2, _3, _terminal.screenSize()),
        bind(&TextScheduler::schedule, &ts, _1, _2, _3)
    );

    assert(!pendingBackgroundDraw_.empty());
    renderPendingBackgroundCells(_terminal.screenSize());

    ts.flush();

    // TODO: check if CursorStyle has changed, and update render context accordingly.
    if (_terminal.shouldDisplayCursor() && _terminal.scrollOffset() + _terminal.cursor().row <= _terminal.screenSize().rows)
    {
        Screen::Cell const& cursorCell = _terminal.absoluteAt(_terminal.cursor());
        cursor_.setShape(_terminal.cursorShape());
        cursor_.render(
            makeCoords(_terminal.cursor().column, _terminal.cursor().row + static_cast<cursor_pos_t>(_terminal.scrollOffset()), _terminal.screenSize()),
            cursorCell.width()
        );
    }

    textShader_->bind();
    textShader_->setUniformValue(textProjectionLocation_, projectionMatrix_);
    textRenderer_.execute();

    if (_terminal.isSelectionAvailable())
    {
        auto const color = canonicalColor(colorProfile_.selection, Opacity((int)(colorProfile_.selectionOpacity * 255.0f)));
        for (Selector::Range const& range : _terminal.selection())
        {
            if (_terminal.isAbsoluteLineVisible(range.line))
            {
                cursor_pos_t const row = range.line - static_cast<cursor_pos_t>(_terminal.historyLineCount() - _terminal.scrollOffset());

                ++metrics_.cellBackgroundRenderCount;
                cellBackground_.render(
                    makeCoords(range.fromColumn, row, _terminal.screenSize()),
                    color,
                    1 + range.toColumn - range.fromColumn);
            }
        }
    }
    return changes;
}

void GLRenderer::fillBackgroundGroup(cursor_pos_t _row, cursor_pos_t _col, ScreenBuffer::Cell const& _cell, WindowSize const& _screenSize)
{
    auto const bgColor = makeColors(_cell.attributes()).second;

    if (pendingBackgroundDraw_.lineNumber == _row && pendingBackgroundDraw_.color == bgColor)
        pendingBackgroundDraw_.endColumn++;
    else
    {
        if (!pendingBackgroundDraw_.empty())
            renderPendingBackgroundCells(_screenSize);

        pendingBackgroundDraw_.reset(bgColor, _row, _col);
    }
}

void GLRenderer::renderPendingBackgroundCells(WindowSize const& _screenSize)
{
    if (pendingBackgroundDraw_.color == colorProfile_.defaultBackground)
        return;

    ++metrics_.cellBackgroundRenderCount;

    // printf("GLRenderer.renderPendingBackgroundCells(#%u: %d, %d-%d) #%02x%02x%02x\n",
    //     metrics_.cellBackgroundRenderCount,
    //     pendingBackgroundDraw_.lineNumber,
    //     pendingBackgroundDraw_.startColumn,
    //     pendingBackgroundDraw_.endColumn,
    //     int(pendingBackgroundDraw_.color[0] * 0xFF) & 0xFF,
    //     int(pendingBackgroundDraw_.color[1] * 0xFF) & 0xFF,
    //     int(pendingBackgroundDraw_.color[2] * 0xFF) & 0xFF
    // );

    cellBackground_.render(
        makeCoords(pendingBackgroundDraw_.startColumn, pendingBackgroundDraw_.lineNumber, _screenSize),
        QVector4D(
            static_cast<float>(pendingBackgroundDraw_.color.red) / 255.0f,
            static_cast<float>(pendingBackgroundDraw_.color.green) / 255.0f,
            static_cast<float>(pendingBackgroundDraw_.color.blue) / 255.0f,
            1.0f
        ),
        1 + pendingBackgroundDraw_.endColumn - pendingBackgroundDraw_.startColumn
    );
}

void GLRenderer::renderText(WindowSize const& _screenSize,
                            cursor_pos_t _lineNumber,
                            cursor_pos_t _startColumn,
                            ScreenBuffer::GraphicsAttributes const& _attributes,
                            crispy::CodepointSequence const& _codepoints,
                            unicode::PresentationStyle _presentationStyle)
{
    if (_codepoints.empty())
        return;

    ++metrics_.renderTextGroup;

    auto const [fgColor, bgColor] = makeColors(_attributes);
    auto const textStyle = text::FontStyle::Regular;

    if (_attributes.styles & CharacterStyleMask::Bold)
    {
        // TODO: switch font
    }

    if (_attributes.styles & CharacterStyleMask::Italic)
    {
        // TODO: *Maybe* update transformation matrix to have chars italic *OR* change font (depending on bold-state)
    }

    if (_attributes.styles & CharacterStyleMask::Blinking)
    {
        // TODO: update textshaper's shader to blink
    }

    if (_attributes.styles & CharacterStyleMask::CrossedOut)
    {
        // TODO: render centered horizontal bar through the cell rectangle (we could reuse the TextShaper and a Unicode character for that, respecting opacity!)
    }

    if (_attributes.styles & CharacterStyleMask::DoublyUnderlined)
    {
        // TODO: render lower-bound horizontal bar through the cell rectangle (we could reuse the TextShaper and a Unicode character for that, respecting opacity!)
    }
    else if (_attributes.styles & CharacterStyleMask::Underline)
    {
        // TODO: render lower-bound double-horizontal bar through the cell rectangle (we could reuse the TextShaper and a Unicode character for that, respecting opacity!)
    }

    if (!(_attributes.styles & CharacterStyleMask::Hidden))
    {
        (void) textStyle;// TODO: selection by textStyle
        bool const isEmojiPresentation = _presentationStyle == unicode::PresentationStyle::Emoji;
        // unicode::emoji(_codepoints.front().value)
        //                                 && !unicode::emoji_component(_codepoints.front().value);
        text::FontList& font = isEmojiPresentation ? emojiFont_
                                                   : regularFont_;

        if (text::GlyphPositionList const* glyphPositions = textShaper_.shape(font, _codepoints); glyphPositions)
        {
            textRenderer_.render(
                makeCoords(_startColumn, _lineNumber, _screenSize),
                *glyphPositions,
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
    }
}

QPoint GLRenderer::makeCoords(cursor_pos_t col, cursor_pos_t row, WindowSize const& _screenSize) const
{
    return QPoint{
        static_cast<int>(leftMargin_ + (col - 1) * regularFont_.first.get().maxAdvance()),
        static_cast<int>(bottomMargin_ + (_screenSize.rows - row) * regularFont_.first.get().lineHeight())
    };
}

std::pair<RGBColor, RGBColor> GLRenderer::makeColors(ScreenBuffer::GraphicsAttributes const& _attributes) const
{
    float const opacity = [=]() {
        if (_attributes.styles & CharacterStyleMask::Faint)
            return 0.5f;
        else
            return 1.0f;
    }();

    bool const bright = (_attributes.styles & CharacterStyleMask::Bold) != 0;

    return (_attributes.styles & CharacterStyleMask::Inverse)
        ? pair{ apply(colorProfile_, _attributes.backgroundColor, ColorTarget::Background, bright) * opacity,
                apply(colorProfile_, _attributes.foregroundColor, ColorTarget::Foreground, bright) }
        : pair{ apply(colorProfile_, _attributes.foregroundColor, ColorTarget::Foreground, bright) * opacity,
                apply(colorProfile_, _attributes.backgroundColor, ColorTarget::Background, bright) };
}

} // end namespace
