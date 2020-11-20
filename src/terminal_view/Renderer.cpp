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
using std::optional;
using std::tuple;

namespace terminal::view {

Renderer::Renderer(Logger _logger,
                   Size const& _screenSize,
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
        _colorProfile.defaultBackground,
        renderTarget_
    },
    imageRenderer_{
        renderTarget_,
        renderTarget_.coloredAtlasAllocator(),
        cellSize()
    },
    textRenderer_{
        metrics_,
        renderTarget_,
        renderTarget_.monochromeAtlasAllocator(),
        renderTarget_.coloredAtlasAllocator(),
        screenCoordinates_,
        _fonts,
        cellSize()
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
}

void Renderer::discardImage(Image const& _image)
{
    imageRenderer_.discardImage(_image);
}

void Renderer::clearCache()
{
    renderTarget_.clearCache();

    // TODO(?): below functions are actually doing the same again and again and again. delete them (and their functions for that)
    // either that, or only the render target is allowed to clear the actual atlas caches.
    decorationRenderer_.clearCache();
    cursorRenderer_.clearCache();
    textRenderer_.clearCache();
    imageRenderer_.clearCache();
}

void Renderer::setFont(FontConfig const& _fonts)
{
    fonts_ = _fonts;

    screenCoordinates_.cellSize = Size{
        fonts_.regular.first.get().maxAdvance(),
        fonts_.regular.first.get().lineHeight()
    };
    screenCoordinates_.textBaseline = fonts_.regular.first.get().baseline();

    textRenderer_.setFont(_fonts);
    textRenderer_.setCellSize(cellSize());
    imageRenderer_.setCellSize(cellSize());

    clearCache();
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

    screenCoordinates_.cellSize = Size{
        fonts_.regular.first.get().maxAdvance(),
        fonts_.regular.first.get().lineHeight()
    };
    screenCoordinates_.textBaseline = fonts_.regular.first.get().baseline();

    textRenderer_.setCellSize(cellSize());
    imageRenderer_.setCellSize(cellSize());

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
    backgroundRenderer_.setDefaultColor(_colors.defaultBackground);
    decorationRenderer_.setColorProfile(_colors);
    cursorRenderer_.setColor(canonicalColor(colorProfile_.cursor));
}

uint64_t Renderer::render(Terminal& _terminal,
                          steady_clock::time_point _now,
                          terminal::Coordinate const& _currentMousePosition,
                          bool _pressure)
{
    auto const pressure = _pressure && _terminal.screen().isPrimaryScreen();
    metrics_.clear();
    textRenderer_.setPressure(pressure);

    screenCoordinates_.screenSize = _terminal.screenSize();

    uint64_t changes = 0;
    {
        auto _l = scoped_lock{_terminal};
        auto const reverseVideo = _terminal.screen().isModeEnabled(terminal::Mode::ReverseVideo);
        auto const baseLine = _terminal.viewport().absoluteScrollOffset().value_or(_terminal.screen().historyLineCount());

        if (!pressure)
            renderCursor(_terminal);

        if (!pressure && _terminal.screen().contains(_currentMousePosition))
        {
            auto& cellAtMouse = _terminal.screen().at(_currentMousePosition);
            if (cellAtMouse.hyperlink())
                cellAtMouse.hyperlink()->state = HyperlinkState::Hover; // TODO: Left-Ctrl pressed?

            changes = _terminal.preRender(_now);

            _terminal.screen().render(
                [&](Coordinate const& _pos, Cell const& _cell) {
                    auto const absolutePos = Coordinate{baseLine + _pos.row, _pos.column};
                    auto const selected = _terminal.isSelectedAbsolute(absolutePos);
                    renderCell(_pos, _cell, reverseVideo, selected);
                },
                _terminal.viewport().absoluteScrollOffset()
            );

            if (cellAtMouse.hyperlink())
                cellAtMouse.hyperlink()->state = HyperlinkState::Inactive;
        }
        else
        {
            changes = _terminal.preRender(_now);
            _terminal.screen().render(
                [&](Coordinate const& _pos, Cell const& _cell) {
                    auto const absolutePos = Coordinate{baseLine + _pos.row, _pos.column};
                    auto const selected = _terminal.isSelectedAbsolute(absolutePos);
                    renderCell(_pos, _cell, reverseVideo, selected);
                },
                _terminal.viewport().absoluteScrollOffset());
        }
    }

    backgroundRenderer_.renderPendingCells();
    backgroundRenderer_.finish();

    textRenderer_.flushPendingSegments();
    textRenderer_.finish();

    renderTarget_.execute();

    return changes;
}

void Renderer::renderCursor(Terminal const& _terminal)
{
    bool const shouldDisplayCursor = _terminal.screen().cursor().visible
        && (_terminal.cursorDisplay() == CursorDisplay::Steady || _terminal.cursorBlinkActive());

    // TODO: check if CursorStyle has changed, and update render context accordingly.
    if (shouldDisplayCursor && _terminal.viewport().isLineVisible(_terminal.screen().cursor().position.row))
    {
        Cell const& cursorCell = _terminal.screen().at(_terminal.screen().cursor().position);

        auto const cursorShape = _terminal.screen().focused() ? _terminal.cursorShape()
                                                              : CursorShape::Rectangle;

        cursorRenderer_.setShape(cursorShape);

        cursorRenderer_.render(
            screenCoordinates_.map(
                _terminal.screen().cursor().position.column,
                _terminal.screen().cursor().position.row + _terminal.viewport().relativeScrollOffset()
            ),
            cursorCell.width()
        );
    }
}

tuple<RGBColor, RGBColor> makeColors(ColorProfile const& _colorProfile, Cell const& _cell, bool _reverseVideo, bool _selected)
{
    auto const [fg, bg] = _cell.attributes().makeColors(_colorProfile, _reverseVideo);
    if (!_selected)
        return tuple{fg, bg};

    auto const a = _colorProfile.selectionForeground.value_or(bg);
    auto const b = _colorProfile.selectionBackground.value_or(fg);
    return tuple{a, b};
}

void Renderer::renderCell(Coordinate const& _pos, Cell const& _cell, bool _reverseVideo, bool _selected)
{
    auto const [fg, bg] = makeColors(colorProfile_, _cell, _reverseVideo, _selected);

    backgroundRenderer_.renderCell(_pos, bg);
    decorationRenderer_.renderCell(_pos, _cell);
    textRenderer_.schedule(_pos, _cell, fg);
    if (optional<ImageFragment> const& fragment = _cell.imageFragment(); fragment.has_value())
        imageRenderer_.renderImage(screenCoordinates_.map(_pos), fragment.value());
}

void Renderer::dumpState(std::ostream& _textOutput) const
{
    textRenderer_.debugCache(_textOutput);
}

} // end namespace
