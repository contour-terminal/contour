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
#include <terminal_view/Renderer.h>
#include <terminal_view/TextRenderer.h>

#include <functional>

using namespace std;
using namespace std::chrono;
using namespace std::placeholders;
using namespace crispy;

namespace terminal::view {

Renderer::Renderer(Logger _logger,
                   WindowSize const& _screenSize,
                   FontConfig const& _fonts,
                   terminal::ColorProfile _colorProfile,
                   terminal::Opacity _backgroundOpacity,
                   Decorator _hyperlinkNormal,
                   Decorator _hyperlinkHover,
                   ShaderConfig const& _backgroundShaderConfig,
                   ShaderConfig const& _textShaderConfig,
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
    renderTarget_{
        _textShaderConfig,
        _backgroundShaderConfig,
        _projectionMatrix,
        0, // TODO left margin
        0, // TODO bottom margin
        {}, // TODO _cellSize?
    },
    backgroundRenderer_{
        screenCoordinates_,
        _colorProfile,
        renderTarget_
    },
    textRenderer_{
        metrics_,
        renderTarget_,
        renderTarget_.monochromeAtlasAllocator(),
        renderTarget_.coloredAtlasAllocator(),
        screenCoordinates_,
        _colorProfile,
        _fonts
    },
    decorationRenderer_{
        renderTarget_,
        renderTarget_.monochromeAtlasAllocator(),
        screenCoordinates_,
        _colorProfile,
        _hyperlinkNormal,
        _hyperlinkHover,
        1,      // line thickness (TODO: configurable)
        0.75f,  // curly amplitude
        1.0f    // curly frequency
    },
    cursorRenderer_{
        renderTarget_,
        renderTarget_.monochromeAtlasAllocator(),
        screenCoordinates_,
        CursorShape::Block, // TODO: should not be hard-coded; actual value be passed via render(terminal, now);
        canonicalColor(_colorProfile.cursor)
    }
{
    textRenderer_.setCellSize(cellSize());
}

void Renderer::clearCache()
{
    renderTarget_.clearCache();
    decorationRenderer_.clearCache();
    cursorRenderer_.clearCache();
    textRenderer_.clearCache();
}

void Renderer::setFont(FontConfig const& _fonts)
{
    textRenderer_.setFont(_fonts);
}

bool Renderer::setFontSize(unsigned int _fontSize)
{
    if (_fontSize == fonts_.regular.first.get().fontSize())
        return false;

    for (auto& font: {fonts_.regular, fonts_.bold, fonts_.italic, fonts_.boldItalic, fonts_.emoji})
    {
        font.first.get().setFontSize(_fontSize);
        for (auto& fallback : font.second)
            fallback.get().setFontSize(_fontSize);
    }

    screenCoordinates_.cellWidth = cellWidth();
    screenCoordinates_.cellHeight = cellHeight();
    screenCoordinates_.textBaseline = fonts_.regular.first.get().baseline();

    textRenderer_.setCellSize(cellSize());

    clearCache();

    return true;
}

void Renderer::setProjection(QMatrix4x4 const& _projectionMatrix)
{
    renderTarget_.setProjection(_projectionMatrix);
}

void Renderer::setBackgroundOpacity(terminal::Opacity _opacity)
{
    backgroundOpacity_ = _opacity;
}

void Renderer::setColorProfile(terminal::ColorProfile const& _colors)
{
    colorProfile_ = _colors;
    textRenderer_.setColorProfile(_colors);
    decorationRenderer_.setColorProfile(_colors);
    cursorRenderer_.setColor(canonicalColor(colorProfile_.cursor));
}

uint64_t Renderer::render(Terminal& _terminal,
                            steady_clock::time_point _now,
                            terminal::Coordinate const& _currentMousePosition)
{
    metrics_.clear();

    screenCoordinates_.screenSize = _terminal.screenSize();

    uint64_t changes = 0;
    {
        auto _l = scoped_lock{_terminal};
        if (_terminal.screen().contains(_currentMousePosition))
        {
            auto& cellAtMouse = _terminal.screen().at(_currentMousePosition);
            if (cellAtMouse.hyperlink())
            {
                cellAtMouse.hyperlink()->state = HyperlinkState::Hover; // TODO: Left-Ctrl pressed?
            }

            changes = _terminal.preRender(_now);
            _terminal.screen().render(bind(&Renderer::renderCell, this, _1, _2, _3),
                                      _terminal.screen().scrollOffset());

            if (cellAtMouse.hyperlink())
                cellAtMouse.hyperlink()->state = HyperlinkState::Inactive;
        }
        else
        {
            changes = _terminal.preRender(_now);
            _terminal.screen().render(bind(&Renderer::renderCell, this, _1, _2, _3),
                                      _terminal.screen().scrollOffset());
        }
    }

    backgroundRenderer_.renderPendingCells();
    backgroundRenderer_.finish();

    renderSelection(_terminal);

    renderCursor(_terminal);

    textRenderer_.flushPendingSegments();
    textRenderer_.finish();

    renderTarget_.execute();

    return changes;
}

void Renderer::renderCursor(Terminal const& _terminal)
{
    // TODO: check if CursorStyle has changed, and update render context accordingly.
    if (_terminal.shouldDisplayCursor() && _terminal.scrollOffset() + _terminal.cursor().row <= _terminal.screenSize().rows)
    {
        Screen::Cell const& cursorCell = _terminal.absoluteAt(_terminal.cursor());

        auto const cursorShape = _terminal.screen().focused() ? _terminal.cursorShape()
                                                              : CursorShape::Rectangle;

        cursorRenderer_.setShape(cursorShape);

        cursorRenderer_.render(
            screenCoordinates_.map(_terminal.cursor().column, _terminal.cursor().row + static_cast<cursor_pos_t>(_terminal.scrollOffset())),
            cursorCell.width()
        );
    }
}

void Renderer::renderSelection(Terminal const& _terminal)
{
    if (_terminal.isSelectionAvailable())
    {
        // TODO: don't abouse BackgroundRenderer here, maybe invent RectRenderer?
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
        backgroundRenderer_.renderPendingCells();
        backgroundRenderer_.setOpacity(1.0f);
    }
}

void Renderer::renderCell(cursor_pos_t _row, cursor_pos_t _col, ScreenBuffer::Cell const& _cell)
{
    backgroundRenderer_.renderCell(_row, _col, _cell);
    decorationRenderer_.renderCell(_row, _col, _cell);
    textRenderer_.schedule(_row, _col, _cell);
}

} // end namespace
