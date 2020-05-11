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

using namespace std;
using namespace std::chrono;
using namespace std::placeholders;

using namespace crispy;
using namespace terminal;
using namespace terminal::view;

GLRenderer::GLRenderer(Logger _logger,
                       text::FontList const& _regularFont,
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
    projectionMatrix_{ _projectionMatrix },
    textShaper_{ _regularFont.first, _regularFont.second },
    textShader_{ createShader(_textShaderConfig) },
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
    textShaper_.setFont(_font, _fallback);
    clearCache();
}

bool GLRenderer::setFontSize(unsigned int _fontSize)
{
    if (_fontSize == regularFont_.first.get().fontSize())
        return false;

    regularFont_.first.get().setFontSize(_fontSize);
    for (auto& fallback : regularFont_.second)
        fallback.get().setFontSize(_fontSize);

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
    pendingDraw_ = {};

    auto const changes = _terminal.render(
        _now,
        bind(&GLRenderer::fillBackgroundGroup, this, _1, _2, _3, _terminal.screenSize()),
        bind(&GLRenderer::fillTextGroup, this, _1, _2, _3, _terminal.screenSize())
    );

    assert(!pendingBackgroundDraw_.empty());
    renderPendingBackgroundCells(_terminal.screenSize());

    renderTextGroup(_terminal.screenSize());

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
    textShader_->setUniformValue(0, projectionMatrix_);
    textRenderer_.execute();

    if (_terminal.isSelectionAvailable())
    {
        auto const color = canonicalColor(colorProfile_.selection, static_cast<Opacity>(colorProfile_.selectionOpacity * 255.0f));
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

void GLRenderer::fillTextGroup(cursor_pos_t _row, cursor_pos_t _col, Screen::Cell const& _cell, WindowSize const& _screenSize)
{
#if 1
    pendingDraw_.state = PendingDraw::State::Filling;
    pendingDraw_.reset(_row, _col, _cell.attributes());
    pendingDraw_.extend(_cell);
    renderTextGroup(_screenSize);
    pendingDraw_.reset(_row, _col, _cell.attributes());
#else
    constexpr uint8_t SP = 0x20;

    switch (pendingDraw_.state)
    {
        case PendingDraw::State::Empty:
            if (_cell.codepoint() > SP)
            {
                pendingDraw_.state = PendingDraw::State::Filling;
                pendingDraw_.reset(_row, _col, _cell.attributes());
                pendingDraw_.extend(_cell);
            }
            break;
        case PendingDraw::State::Filling:
            if (pendingDraw_.lineNumber == _row && pendingDraw_.attributes == _cell.attributes() && _cell.codepoint() > SP)
                pendingDraw_.extend(_cell);
            else
            {
                renderTextGroup(_screenSize);
                if (_cell.codepoint() > SP)
                {
                    pendingDraw_.reset(_row, _col, _cell.attributes());
                    pendingDraw_.extend(_cell);
                }
                else
                {
                    pendingDraw_.codepoints.clear();
                    pendingDraw_.state = PendingDraw::State::Empty;
                }
            }
            break;
    }
#endif
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

void GLRenderer::renderTextGroup(WindowSize const& _screenSize)
{
    if (pendingDraw_.codepoints.empty())
        return;

    ++metrics_.renderTextGroup;

    auto const [fgColor, bgColor] = makeColors(pendingDraw_.attributes);
    auto const textStyle = text::FontStyle::Regular;

    if (pendingDraw_.attributes.styles & CharacterStyleMask::Bold)
    {
        // TODO: switch font
    }

    if (pendingDraw_.attributes.styles & CharacterStyleMask::Italic)
    {
        // TODO: *Maybe* update transformation matrix to have chars italic *OR* change font (depending on bold-state)
    }

    if (pendingDraw_.attributes.styles & CharacterStyleMask::Blinking)
    {
        // TODO: update textshaper's shader to blink
    }

    if (pendingDraw_.attributes.styles & CharacterStyleMask::CrossedOut)
    {
        // TODO: render centered horizontal bar through the cell rectangle (we could reuse the TextShaper and a Unicode character for that, respecting opacity!)
    }

    if (pendingDraw_.attributes.styles & CharacterStyleMask::DoublyUnderlined)
    {
        // TODO: render lower-bound horizontal bar through the cell rectangle (we could reuse the TextShaper and a Unicode character for that, respecting opacity!)
    }
    else if (pendingDraw_.attributes.styles & CharacterStyleMask::Underline)
    {
        // TODO: render lower-bound double-horizontal bar through the cell rectangle (we could reuse the TextShaper and a Unicode character for that, respecting opacity!)
    }

    if (!(pendingDraw_.attributes.styles & CharacterStyleMask::Hidden))
    {
        (void) textStyle;
        text::TextShaper& textShaper = textShaper_; // TODO: selection by textStyle

        if (text::GlyphPositionList const* glyphPositions = textShaper.shape(pendingDraw_.codepoints); glyphPositions)
        {
            textRenderer_.render(
                makeCoords(pendingDraw_.startColumn, pendingDraw_.lineNumber, _screenSize),
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
