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

using std::scoped_lock;
using std::chrono::steady_clock;

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

bool Renderer::setFontSize(int _fontSize)
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
                          terminal::Coordinate const& _currentMousePosition,
                          bool _pressure)
{
    metrics_.clear();
    textRenderer_.setPressure(_pressure);

    screenCoordinates_.screenSize = _terminal.screenSize();

    renderCursor(_terminal);

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
            _terminal.screen().render([this](Coordinate const& _pos, Cell const& _cell) { renderCell(_pos, _cell); },
                                      _terminal.screen().scrollOffset());

            if (cellAtMouse.hyperlink())
                cellAtMouse.hyperlink()->state = HyperlinkState::Inactive;
        }
        else
        {
            changes = _terminal.preRender(_now);
            _terminal.screen().render([this](Coordinate const& pos, Cell const& _cell) { renderCell(pos, _cell); },
                                      _terminal.screen().scrollOffset());
        }
    }

    backgroundRenderer_.renderPendingCells();
    backgroundRenderer_.finish();

    renderSelection(_terminal);

    textRenderer_.flushPendingSegments();
    textRenderer_.finish();

    renderTarget_.execute();

    return changes;
}

void Renderer::renderCursor(Terminal const& _terminal)
{
    // TODO: check if CursorStyle has changed, and update render context accordingly.
    if (_terminal.shouldDisplayCursor() && _terminal.isLineVisible(_terminal.cursor().position.row))
    {
        Cell const& cursorCell = *_terminal.at(_terminal.cursor().position);

        auto const cursorShape = _terminal.screen().focused() ? _terminal.cursorShape()
                                                              : CursorShape::Rectangle;

        cursorRenderer_.setShape(cursorShape);

        cursorRenderer_.render(
            screenCoordinates_.map(_terminal.cursor().position.column, _terminal.cursor().position.row + _terminal.scrollOffset()),
            cursorCell.width()
        );
    }
}

void Renderer::renderSelection(Terminal const& _terminal)
{
    if (_terminal.screen().isSelectionAvailable())
    {
        // TODO: don't abouse BackgroundRenderer here, maybe invent RectRenderer?
        backgroundRenderer_.setOpacity(colorProfile_.selectionOpacity);
        Screen const& screen = _terminal.screen();
        auto const selection = screen.selection();
        for (Selector::Range const& range : selection) // _terminal.screen().selection())
        {
            // TODO: see if we can extract and then unit-test this display rendering of selection
            auto const relativeLineNr = range.line - _terminal.historyLineCount() - _terminal.scrollOffset();
            if (_terminal.isLineVisible(relativeLineNr))
            {
                auto const pos = Coordinate{relativeLineNr, range.fromColumn};
                auto const count = 1 + range.toColumn - range.fromColumn;
                backgroundRenderer_.renderOnce(pos, colorProfile_.selection, count);
                ++metrics_.cellBackgroundRenderCount;
            }
        }
        backgroundRenderer_.renderPendingCells();
        backgroundRenderer_.setOpacity(1.0f);
    }
}

void Renderer::renderCell(Coordinate const& _pos, Cell const& _cell)
{
    backgroundRenderer_.renderCell(_pos, _cell);
    decorationRenderer_.renderCell(_pos, _cell);
    textRenderer_.schedule(_pos, _cell);
}

void Renderer::dumpState(std::ostream& _textOutput) const
{
    textRenderer_.debugCache(_textOutput);
}

} // end namespace
