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
#include <terminal_view/GLRenderer.h>
#include <terminal_view/TextRenderer.h>

#include <functional>

using namespace std;
using namespace std::chrono;
using namespace std::placeholders;
using namespace crispy;

namespace terminal::view {

GLRenderer::GLRenderer(Logger _logger,
                       WindowSize const& _screenSize,
                       text::FontList const& _regularFont,
                       text::FontList const& _emojiFont,
                       terminal::ColorProfile _colorProfile,
                       terminal::Opacity _backgroundOpacity,
                       ShaderConfig const& _backgroundShaderConfig,
                       ShaderConfig const& _textShaderConfig,
                       ShaderConfig const& _cursorShaderConfig,
                       QMatrix4x4 const& _projectionMatrix) :
    logger_{ move(_logger) },
    colorProfile_{ _colorProfile },
    screenCoordinates_{
        _screenSize,
        _regularFont.first.get().maxAdvance(), // cell width
        _regularFont.first.get().lineHeight() // cell height
    },
    backgroundOpacity_{ _backgroundOpacity },
    regularFont_{ _regularFont },
    emojiFont_{ _emojiFont },
    projectionMatrix_{ _projectionMatrix },
    textRenderer_{
        screenCoordinates_,
        _colorProfile,
        _regularFont,
        _emojiFont,
        _textShaderConfig
    },
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
        canonicalColor(_colorProfile.cursor),
        _cursorShaderConfig
    }
{
    initializeOpenGLFunctions();

    glEnable(GL_BLEND);
    glBlendFuncSeparate(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA, GL_ONE, GL_ONE);
    //glBlendFunc(GL_SRC1_COLOR, GL_ONE_MINUS_SRC1_COLOR);
}

void GLRenderer::clearCache()
{
    textRenderer_.clearCache();
}

void GLRenderer::setFont(crispy::text::Font& _font, crispy::text::FontFallbackList const& _fallback)
{
    textRenderer_.setFont(_font, _fallback);
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
    textRenderer_.setColorProfile(_colors);
    cursor_.setColor(canonicalColor(colorProfile_.cursor));
}

uint64_t GLRenderer::render(Terminal const& _terminal, steady_clock::time_point _now)
{
    metrics_.clear();
    pendingBackgroundDraw_ = {};

    screenCoordinates_.screenSize = _terminal.screenSize();

    auto const changes = _terminal.render(
        _now,
        bind(&GLRenderer::fillBackgroundGroup, this, _1, _2, _3),
        bind(&TextRenderer::schedule, &textRenderer_, _1, _2, _3)
    );

    assert(!pendingBackgroundDraw_.empty());
    renderPendingBackgroundCells();

    textRenderer_.flushPendingSegments();

    // TODO: check if CursorStyle has changed, and update render context accordingly.
    if (_terminal.shouldDisplayCursor() && _terminal.scrollOffset() + _terminal.cursor().row <= _terminal.screenSize().rows)
    {
        Screen::Cell const& cursorCell = _terminal.absoluteAt(_terminal.cursor());
        cursor_.setShape(_terminal.cursorShape());
        cursor_.render(
            screenCoordinates_.map(_terminal.cursor().column, _terminal.cursor().row + static_cast<cursor_pos_t>(_terminal.scrollOffset())),
            cursorCell.width()
        );
    }

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
                    screenCoordinates_.map(range.fromColumn, row),
                    color,
                    1 + range.toColumn - range.fromColumn);
            }
        }
    }
    return changes;
}

void GLRenderer::fillBackgroundGroup(cursor_pos_t _row, cursor_pos_t _col, ScreenBuffer::Cell const& _cell)
{
    auto const bgColor = _cell.attributes().makeColors(colorProfile_).second;

    if (pendingBackgroundDraw_.lineNumber == _row && pendingBackgroundDraw_.color == bgColor)
        pendingBackgroundDraw_.endColumn++;
    else
    {
        if (!pendingBackgroundDraw_.empty())
            renderPendingBackgroundCells();

        pendingBackgroundDraw_.reset(bgColor, _row, _col);
    }
}

void GLRenderer::renderPendingBackgroundCells()
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
        screenCoordinates_.map(pendingBackgroundDraw_.startColumn, pendingBackgroundDraw_.lineNumber),
        QVector4D(
            static_cast<float>(pendingBackgroundDraw_.color.red) / 255.0f,
            static_cast<float>(pendingBackgroundDraw_.color.green) / 255.0f,
            static_cast<float>(pendingBackgroundDraw_.color.blue) / 255.0f,
            1.0f
        ),
        1 + pendingBackgroundDraw_.endColumn - pendingBackgroundDraw_.startColumn
    );
}

} // end namespace
