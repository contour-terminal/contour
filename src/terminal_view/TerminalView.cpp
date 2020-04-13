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
#include <terminal_view/TerminalView.h>
#include <terminal_view/FontManager.h>

#include <terminal/Logger.h>
#include <terminal/Util.h>
#include <terminal/util/overloaded.h>
#include <terminal/util/UTF8.h>

#include <array>
#include <chrono>
#include <iostream>
#include <utility>

using namespace std;
using namespace std::placeholders;

namespace terminal::view {

inline QVector4D makeColor(terminal::RGBColor const& _rgb, terminal::Opacity _opacity = terminal::Opacity::Opaque)
{
    return QVector4D{
        static_cast<float>(_rgb.red) / 255.0f,
        static_cast<float>(_rgb.green) / 255.0f,
        static_cast<float>(_rgb.blue) / 255.0f,
        static_cast<float>(_opacity) / 255.0f
    };
}

TerminalView::TerminalView(std::chrono::steady_clock::time_point _now,
                           WindowSize const& _winSize,
                           optional<size_t> _maxHistoryLineCount,
                           std::string const& _wordDelimiters,
                           function<void()> _onSelectionComplete,
                           Screen::OnBufferChanged _onScreenBufferChanged,
                           function<void()> _bell,
                           Font& _regularFont,
                           CursorShape _cursorShape, // TODO: remember !
                           CursorDisplay _cursorDisplay,
                           chrono::milliseconds _cursorBlinkInterval,
                           terminal::ColorProfile _colorProfile,
                           terminal::Opacity _backgroundOpacity,
                           string const& _shell,
                           terminal::Process::Environment const& _env,
                           QMatrix4x4 const& _projectionMatrix,
                           function<void()> _onScreenUpdate,
                           function<void()> _onWindowTitleChanged,
                           function<void(unsigned int, unsigned int, bool)> _resizeWindow,
                           function<void()> _onTerminalClosed,
                           ShaderConfig const& _backgroundShaderConfig,
                           ShaderConfig const& _textShaderConfig,
                           ShaderConfig const& _cursorShaderConfig,
                           Logger _logger) :
    logger_{ move(_logger) },
    renderer_{
        logger_,
        _regularFont,
        _colorProfile,
        _backgroundOpacity,
        _backgroundShaderConfig,
        _textShaderConfig,
        _cursorShaderConfig,
        _projectionMatrix
    },
    process_{
        _shell,
        {_shell},
        _env,
        _winSize,
        _maxHistoryLineCount,
        _cursorBlinkInterval,
        move(_onWindowTitleChanged),
        move(_resizeWindow),
        bind(&TerminalView::requestDynamicColor, this, _1),
        bind(&TerminalView::resetDynamicColor, this, _1),
        bind(&TerminalView::setDynamicColor, this, _1, _2),
        _now,
        _wordDelimiters,
        move(_onSelectionComplete),
        move(_onScreenBufferChanged),
        move(_bell),
        _cursorDisplay,
        _cursorShape,
        [_onScreenUpdate](auto const& /*_commands*/) { if (_onScreenUpdate) _onScreenUpdate(); },
        move(_onTerminalClosed),
        [this](terminal::LogEvent const& _event) { logger_(_event); }
    },
    colorProfile_{_colorProfile},
    defaultColorProfile_{_colorProfile}
{
}

RGBColor TerminalView::requestDynamicColor(DynamicColorName _name)
{
    switch (_name)
    {
        case DynamicColorName::DefaultForegroundColor:
            return colorProfile_.defaultForeground;
        case DynamicColorName::DefaultBackgroundColor:
            return colorProfile_.defaultBackground;
        case DynamicColorName::TextCursorColor:
            return colorProfile_.cursor;
        case DynamicColorName::MouseForegroundColor:
            return colorProfile_.mouseForeground;
        case DynamicColorName::MouseBackgroundColor:
            return colorProfile_.mouseBackground;
        case DynamicColorName::HighlightForegroundColor:
            return RGBColor{}; // TODO: implement (or in other words: Do we need this? Is this meaningful nowadays?)
        case DynamicColorName::HighlightBackgroundColor:
            return colorProfile_.selection;
    }
    return RGBColor{}; // should never happen.
}

void TerminalView::setColorProfile(terminal::ColorProfile const& _colors)
{
    colorProfile_ = _colors;
    defaultColorProfile_ = _colors;
    renderer_.setColorProfile(colorProfile_);
}

void TerminalView::resetDynamicColor(DynamicColorName _name)
{
    switch (_name)
    {
        case DynamicColorName::DefaultForegroundColor:
            colorProfile_.defaultForeground = defaultColorProfile_.defaultForeground;
            break;
        case DynamicColorName::DefaultBackgroundColor:
            colorProfile_.defaultBackground = defaultColorProfile_.defaultBackground;
            break;
        case DynamicColorName::TextCursorColor:
            colorProfile_.cursor = defaultColorProfile_.cursor;
            break;
        case DynamicColorName::MouseForegroundColor:
            colorProfile_.mouseForeground = defaultColorProfile_.mouseForeground;
            break;
        case DynamicColorName::MouseBackgroundColor:
            colorProfile_.mouseBackground = defaultColorProfile_.mouseBackground;
            break;
        case DynamicColorName::HighlightForegroundColor:
            // not needed (for now)
            break;
        case DynamicColorName::HighlightBackgroundColor:
            colorProfile_.selection = defaultColorProfile_.selection;
            break;
    }
}

void TerminalView::setDynamicColor(DynamicColorName _name, RGBColor const& _value)
{
    switch (_name)
    {
        case DynamicColorName::DefaultForegroundColor:
            colorProfile_.defaultForeground = _value;
            break;
        case DynamicColorName::DefaultBackgroundColor:
            colorProfile_.defaultBackground = _value;
            break;
        case DynamicColorName::TextCursorColor:
            colorProfile_.cursor = _value;
            break;
        case DynamicColorName::MouseForegroundColor:
            colorProfile_.mouseForeground = _value;
            break;
        case DynamicColorName::MouseBackgroundColor:
            colorProfile_.mouseBackground = _value;
            break;
        case DynamicColorName::HighlightForegroundColor:
            break; // TODO: implement (or in other words: Do we need this? Is this meaningful nowadays?)
        case DynamicColorName::HighlightBackgroundColor:
            colorProfile_.selection = _value;
            break;
    }
}

bool TerminalView::alive() const
{
    return process_.alive();
}

void TerminalView::resize(unsigned _width, unsigned _height)
{
    auto const newSize = terminal::WindowSize{
        static_cast<unsigned short>(_width / renderer_.cellWidth()),
        static_cast<unsigned short>(_height / renderer_.cellHeight())
    };

    bool const doResize = newSize != process_.screenSize();
    if (doResize)
        process_.resizeScreen(newSize);

    if (doResize)
    {
        terminal().clearSelection();

        cout << fmt::format(
            "Resized to {}x{} ({}x{}) (CharBox: {}x{})\n",
            newSize.columns, newSize.rows,
            _width, _height,
            renderer_.cellWidth(), renderer_.cellHeight()
        );
    }
}

void TerminalView::setCursorShape(CursorShape _shape)
{
    terminal().setCursorShape(_shape);
}

bool TerminalView::setTerminalSize(terminal::WindowSize const& _newSize)
{
    if (process_.terminal().screenSize() == _newSize)
        return false;

    process_.terminal().resizeScreen(_newSize);
    return true;
}


uint64_t TerminalView::render(chrono::steady_clock::time_point const& _now)
{
    return renderer_.render(process_.terminal(), _now);
}

void TerminalView::wait()
{
    if (!process_.alive())
        return;

    process_.terminal().device().close();
    (void) process_.wait();
}

} // namespace terminal::view
