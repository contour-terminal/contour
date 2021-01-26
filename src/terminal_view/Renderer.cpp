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

#include <array>
#include <functional>

using std::array;
using std::scoped_lock;
using std::chrono::steady_clock;
using std::optional;
using std::tuple;

namespace terminal::view {

void loadGridMetricsFromFont(crispy::text::Font const& _font, GridMetrics& _gm)
{
    _gm.cellSize = Size{ _font.maxAdvance(), _font.lineHeight() };
    _gm.baseline = _font.baseline();
    _gm.ascender = _font.ascender();
    _gm.descender = _font.descender();
    _gm.underline.position = _gm.baseline + _font.underlineOffset();
    _gm.underline.thickness = _font.underlineThickness();
}

GridMetrics loadGridMetrics(crispy::text::Font const& _font, Size _pageSize)
{
    auto gm = GridMetrics{};

    gm.pageSize = _pageSize;
    gm.cellMargin = {0, 0, 0, 0}; // TODO (pass as args, and make use of them)
    gm.pageMargin = {0, 0};       // TODO (fill early)

    loadGridMetricsFromFont(_font, gm);

    return gm;
}

Renderer::Renderer(Size const& _screenSize,
                   FontConfig& _fonts,
                   terminal::ColorProfile _colorProfile,
                   terminal::Opacity _backgroundOpacity,
                   Decorator _hyperlinkNormal,
                   Decorator _hyperlinkHover,
                   ShaderConfig const& _backgroundShaderConfig,
                   ShaderConfig const& _textShaderConfig,
                   QMatrix4x4 const& _projectionMatrix) :
    gridMetrics_{ loadGridMetrics(_fonts.regular.front(), _screenSize) },
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
        gridMetrics_,
        _colorProfile.defaultBackground,
        renderTarget_
    },
    imageRenderer_{
        renderTarget_.textureScheduler(),
        renderTarget_.coloredAtlasAllocator(),
        cellSize()
    },
    textRenderer_{
        metrics_,
        renderTarget_.textureScheduler(),
        renderTarget_.monochromeAtlasAllocator(),
        renderTarget_.coloredAtlasAllocator(),
        renderTarget_.lcdAtlasAllocator(),
        gridMetrics_,
        _fonts
    },
    decorationRenderer_{
        renderTarget_.textureScheduler(),
        renderTarget_.monochromeAtlasAllocator(),
        gridMetrics_,
        _colorProfile,
        _hyperlinkNormal,
        _hyperlinkHover
    },
    cursorRenderer_{
        renderTarget_.textureScheduler(),
        renderTarget_.monochromeAtlasAllocator(),
        gridMetrics_,
        CursorShape::Block, // TODO: should not be hard-coded; actual value be passed via render(terminal, now);
        canonicalColor(_colorProfile.cursor)
    }
{
}

void Renderer::discardImage(Image const& _image)
{
    // Defer rendering into the renderer thread & render stage, as this call might have
    // been coming out of bounds from another thread (e.g. the terminal's screen update thread)
    auto _l = scoped_lock{imageDiscardLock_};
    discardImageQueue_.emplace_back(_image.id());
}

void Renderer::executeImageDiscards()
{
    auto _l = scoped_lock{imageDiscardLock_};

    for (auto const& imageId : discardImageQueue_)
        imageRenderer_.discardImage(imageId);

    discardImageQueue_.clear();
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

bool Renderer::setFontSize(double _fontSize)
{
    if (_fontSize == fonts_.regular.front().fontSize())
        return false;

    auto const updateFontList = [&](crispy::text::FontList& fontList) {
        for (auto& font : fontList)
            if (font.loaded())
                font.setFontSize(_fontSize);
    };

    updateFontList(fonts_.regular);
    updateFontMetrics();

    updateFontList(fonts_.bold);
    updateFontList(fonts_.italic);
    updateFontList(fonts_.boldItalic);
    updateFontList(fonts_.emoji);

    return true;
}

void Renderer::updateFontMetrics()
{
    loadGridMetricsFromFont(fonts_.regular.front(), gridMetrics_);

    textRenderer_.updateFontMetrics();
    imageRenderer_.setCellSize(cellSize());
    decorationRenderer_.clearCache();

    clearCache();
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
    metrics_.clear();

    gridMetrics_.pageSize = _terminal.screenSize();

    executeImageDiscards();

    uint64_t const changes = renderInternalNoFlush(_terminal, _now, _currentMousePosition, _pressure);

    backgroundRenderer_.renderPendingCells();
    backgroundRenderer_.finish();

    textRenderer_.flushPendingSegments();
    textRenderer_.finish();

    renderTarget_.execute();

    return changes;
}

uint64_t Renderer::renderInternalNoFlush(Terminal& _terminal,
                                         steady_clock::time_point _now,
                                         terminal::Coordinate const& _currentMousePosition,
                                         bool _pressure)
{
    auto const pressure = _pressure && _terminal.screen().isPrimaryScreen();
    textRenderer_.setPressure(pressure);

    auto _l = scoped_lock{_terminal};
    auto const reverseVideo = _terminal.screen().isModeEnabled(terminal::DECMode::ReverseVideo);
    auto const baseLine = _terminal.viewport().absoluteScrollOffset().value_or(_terminal.screen().historyLineCount());

    renderCursor(_terminal);

    auto const renderHyperlinks = !pressure && _terminal.screen().contains(_currentMousePosition);

    if (renderHyperlinks)
    {
        auto& cellAtMouse = _terminal.screen().at(_currentMousePosition);
        if (cellAtMouse.hyperlink())
            cellAtMouse.hyperlink()->state = HyperlinkState::Hover; // TODO: Left-Ctrl pressed?
    }

    auto const changes = _terminal.preRender(_now);

    _terminal.screen().render(
        [&](Coordinate const& _pos, Cell const& _cell) {
            auto const absolutePos = Coordinate{baseLine + _pos.row, _pos.column};
            auto const selected = _terminal.isSelectedAbsolute(absolutePos);
            renderCell(_pos, _cell, reverseVideo, selected);
        },
        _terminal.viewport().absoluteScrollOffset()
    );

    if (renderHyperlinks)
    {
        auto& cellAtMouse = _terminal.screen().at(_currentMousePosition);
        if (cellAtMouse.hyperlink())
            cellAtMouse.hyperlink()->state = HyperlinkState::Inactive;
    }

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
            gridMetrics_.map(
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
        imageRenderer_.renderImage(gridMetrics_.map(_pos), fragment.value());
}

void Renderer::dumpState(std::ostream& _textOutput) const
{
    textRenderer_.debugCache(_textOutput);
}

} // end namespace
