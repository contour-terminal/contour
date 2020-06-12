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
                       FontConfig const& _fonts,
                       terminal::ColorProfile _colorProfile,
                       terminal::Opacity _backgroundOpacity,
                       ShaderConfig const& _backgroundShaderConfig,
                       ShaderConfig const& _textShaderConfig,
                       ShaderConfig const& _decoratorShaderConfig,
                       ShaderConfig const& _cursorShaderConfig,
                       QMatrix4x4 const& _projectionMatrix) :
    screenCoordinates_{
        _screenSize,
        _fonts.regular.first.get().maxAdvance(), // cell width
        _fonts.regular.first.get().lineHeight(), // cell height
        _fonts.regular.first.get().baseline()
    },
    logger_{ move(_logger) },
    colorProfile_{ _colorProfile },
    backgroundOpacity_{ _backgroundOpacity },
    fonts_{ _fonts },
    projectionMatrix_{ _projectionMatrix },
    backgroundRenderer_{
        screenCoordinates_,
        _colorProfile,
        _projectionMatrix,
        _backgroundShaderConfig
    },
    textRenderer_{
        metrics_,
        screenCoordinates_,
        _colorProfile,
        _fonts,
        _textShaderConfig
    },
    decorationRenderer_{
        screenCoordinates_,
        _projectionMatrix,
        _decoratorShaderConfig,
        1,      // line thickness (TODO: configurable)
        0.75f,  // curly amplitude
        1.0f    // curly frequency
    },
    cursor_{
        QSize(
            static_cast<int>(fonts_.regular.first.get().maxAdvance()),
            static_cast<int>(fonts_.regular.first.get().lineHeight())
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
    decorationRenderer_.clearCache();
}

void GLRenderer::setFont(FontConfig const& _fonts)
{
    textRenderer_.setFont(_fonts);
}

bool GLRenderer::setFontSize(unsigned int _fontSize)
{
    if (_fontSize == fonts_.regular.first.get().fontSize())
        return false;

    for (auto& font: {fonts_.regular, fonts_.bold, fonts_.italic, fonts_.boldItalic, fonts_.emoji})
    {
        font.first.get().setFontSize(_fontSize);
        for (auto& fallback : font.second)
            fallback.get().setFontSize(_fontSize);
    }

    auto const cellWidth = fonts_.regular.first.get().maxAdvance();
    auto const cellHeight = fonts_.regular.first.get().lineHeight();
    auto const cellSize = QSize{static_cast<int>(cellWidth),
                                static_cast<int>(cellHeight)};

    screenCoordinates_.cellWidth = cellWidth;
    screenCoordinates_.cellHeight = cellHeight;
    screenCoordinates_.textBaseline = fonts_.regular.first.get().baseline();

    cursor_.resize(cellSize);
    clearCache();

    return true;
}

void GLRenderer::setProjection(QMatrix4x4 const& _projectionMatrix)
{
    projectionMatrix_ = _projectionMatrix;

    backgroundRenderer_.setProjection(_projectionMatrix);
    decorationRenderer_.setProjection(_projectionMatrix);
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

    screenCoordinates_.screenSize = _terminal.screenSize();

    backgroundRenderer_.setOpacity(static_cast<float>(backgroundOpacity_) / 255.0f);
    auto const changes = _terminal.render(_now, bind(&GLRenderer::renderCell, this, _1, _2, _3));

    textRenderer_.flushPendingSegments();

    backgroundRenderer_.execute();
    renderCursor(_terminal);
    textRenderer_.execute();
    decorationRenderer_.execute();
    renderSelection(_terminal);

    return changes;
}

void GLRenderer::renderCursor(Terminal const& _terminal)
{
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
}

void GLRenderer::renderSelection(Terminal const& _terminal)
{
    if (_terminal.isSelectionAvailable())
    {
        backgroundRenderer_.setOpacity(colorProfile_.selectionOpacity);
        for (Selector::Range const& range : _terminal.selection())
        {
            if (_terminal.isAbsoluteLineVisible(range.line))
            {
                cursor_pos_t const row = range.line - static_cast<cursor_pos_t>(_terminal.historyLineCount() - _terminal.scrollOffset());
                ++metrics_.cellBackgroundRenderCount;
                backgroundRenderer_.renderOnce(
                    row,
                    range.fromColumn,
                    colorProfile_.selection,
                    1 + range.toColumn - range.fromColumn
                );
            }
        }
        backgroundRenderer_.execute();
    }
}

void GLRenderer::renderCell(cursor_pos_t _row, cursor_pos_t _col, ScreenBuffer::Cell const& _cell)
{
    backgroundRenderer_.renderCell(_row, _col, _cell);
    decorationRenderer_.renderCell(_row, _col, _cell);
    textRenderer_.schedule(_row, _col, _cell);
}

} // end namespace
