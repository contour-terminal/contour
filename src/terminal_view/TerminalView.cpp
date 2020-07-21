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
#include <terminal_view/TerminalView.h>

#include <crispy/text/Font.h>

#include <terminal/Logger.h>
#include <fmt/ostream.h>

#include <array>
#include <chrono>
#include <iostream>
#include <utility>

using std::chrono::milliseconds;
using std::chrono::steady_clock;
using std::nullopt;
using std::optional;
using std::string;

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

TerminalView::TerminalView(steady_clock::time_point _now,
                           WindowSize const& _winSize,
                           Events& _events,
                           optional<size_t> _maxHistoryLineCount,
                           string const& _wordDelimiters,
                           FontConfig const& _fonts,
                           CursorShape _cursorShape, // TODO: remember !
                           CursorDisplay _cursorDisplay,
                           milliseconds _cursorBlinkInterval,
                           terminal::ColorProfile _colorProfile,
                           terminal::Opacity _backgroundOpacity,
                           Decorator _hyperlinkNormal,
                           Decorator _hyperlinkHover,
                           Process::ExecInfo const& _shell,
                           QMatrix4x4 const& _projectionMatrix,
                           ShaderConfig const& _backgroundShaderConfig,
                           ShaderConfig const& _textShaderConfig,
                           Logger _logger) :
    events_{ _events },
    logger_{ move(_logger) },
    fonts_{ _fonts },
    size_{
        static_cast<int>(_winSize.columns * _fonts.regular.first.get().maxAdvance()),
        static_cast<int>(_winSize.rows * _fonts.regular.first.get().lineHeight())
    },
    renderer_{
        logger_,
        _winSize,
        _fonts,
        _colorProfile,
        _backgroundOpacity,
        _hyperlinkNormal,
        _hyperlinkHover,
        _backgroundShaderConfig,
        _textShaderConfig,
        _projectionMatrix
    },
    process_{
        _shell,
        _winSize,
        *this,
        _maxHistoryLineCount,
        _cursorBlinkInterval,
        _now,
        _wordDelimiters,
        _cursorDisplay,
        _cursorShape,
        [this](terminal::LogEvent const& _event) { logger_(_event); }
    },
    colorProfile_{_colorProfile},
    defaultColorProfile_{_colorProfile}
{
}

optional<RGBColor> TerminalView::requestDynamicColor(DynamicColorName _name)
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
    return nullopt; // should never happen
}

void TerminalView::setColorProfile(terminal::ColorProfile const& _colors)
{
    colorProfile_ = _colors;
    defaultColorProfile_ = _colors;
    renderer_.setColorProfile(colorProfile_);
}

bool TerminalView::alive() const
{
    return process_.alive();
}

void TerminalView::setFont(FontConfig const& _fonts)
{
    fonts_ = _fonts;
    renderer_.setFont(_fonts);

    auto const newScreenSize = terminal::WindowSize{
        static_cast<unsigned short>(size_.width() / renderer_.cellWidth()),
        static_cast<unsigned short>(size_.height() / renderer_.cellHeight())
    };

    auto const newMargin = computeMargin(newScreenSize, size_.width(), size_.height());

#if 0 // !defined(NDEBUG)
    cout << fmt::format(
        "TerminalView.setFont(size={}): adjusting margin from {}x{} to {}x{}\n",
        _fonts.regular.first.get().fontSize(),
        windowMargin_.left, windowMargin_.bottom,
        newMargin.left, newMargin.bottom);
#endif

    windowMargin_ = newMargin;
    renderer_.setMargin(windowMargin_.left, windowMargin_.bottom);

    // resize terminalView (same pixels, but adjusted terminal rows/columns and margin)
    resize(size_.width(), size_.height());
}

bool TerminalView::setFontSize(unsigned int _fontSize)
{
    if (!renderer_.setFontSize(_fontSize))
        return false;

    // resize terminalView (same pixels, but adjusted terminal rows/columns and margin)
    resize(size_.width(), size_.height());
    return true;
}

TerminalView::WindowMargin TerminalView::computeMargin(WindowSize const& ws,
                                                       [[maybe_unused]] unsigned _width,
                                                       unsigned _height) const noexcept
{
    auto const usedHeight = static_cast<int>(ws.rows * fonts_.regular.first.get().lineHeight());
    auto const freeHeight = static_cast<int>(_height - usedHeight);
    auto const bottomMargin = freeHeight;

    //auto const usedWidth = ws.columns * regularFont_.get().maxAdvance();
    //auto const freeWidth = _width - usedWidth;
    auto constexpr leftMargin = 0;

    return {leftMargin, bottomMargin};
};

void TerminalView::resize(unsigned _width, unsigned _height)
{
    size_ = QSize{
        static_cast<int>(_width),
        static_cast<int>(_height)
    };

    auto const newScreenSize = terminal::WindowSize{
        static_cast<unsigned short>(_width / renderer_.cellWidth()),
        static_cast<unsigned short>(_height / renderer_.cellHeight())
    };

    windowMargin_ = computeMargin(newScreenSize, _width, _height);

    renderer_.setScreenSize(newScreenSize);
    renderer_.setMargin(windowMargin_.left, windowMargin_.bottom);
    //renderer_.clearCache();

    if (newScreenSize != process_.screenSize())
    {
        process_.resizeScreen(newScreenSize);

        terminal().clearSelection();
    }

#if !defined(NDEBUG)
    std::cout << fmt::format(
        "Resized to pixelSize: {}x{}, screenSize: {}x{}, margin: {}x{}, cellSize: {}x{}\n",
        _width, _height,
        newScreenSize.columns, newScreenSize.rows,
        windowMargin_.left, windowMargin_.bottom,
        renderer_.cellWidth(), renderer_.cellHeight()
    );
#endif
}

void TerminalView::setCursorShape(CursorShape _shape)
{
    terminal().setCursorShape(_shape);
}

bool TerminalView::setTerminalSize(terminal::WindowSize const& _newSize)
{
    if (process_.terminal().screenSize() == _newSize)
        return false;

#if !defined(NDEBUG)
    std::cout << fmt::format("Setting terminal size from {} to {}\n", process_.terminal().screenSize(), _newSize);
#endif

    renderer_.setScreenSize(_newSize);
    process_.terminal().resizeScreen(_newSize);

    return true;
}

uint64_t TerminalView::render(steady_clock::time_point const& _now)
{
    return renderer_.render(process_.terminal(), _now, terminal().currentMousePosition());
}

void TerminalView::wait()
{
    if (!process_.alive())
        return;

    process_.terminal().device().close();
    (void) process_.wait();
}


void TerminalView::bell()
{
    events_.bell();
}

void TerminalView::bufferChanged(ScreenBuffer::Type _type)
{
    events_.bufferChanged(_type);
}

void TerminalView::commands(CommandList const& _commands)
{
    events_.commands(_commands);
}

void TerminalView::copyToClipboard(std::string_view const& _data)
{
    events_.copyToClipboard(_data);
}

void TerminalView::notify(std::string_view const& _title, std::string_view const& _body)
{
    events_.notify(_title, _body);
}

void TerminalView::onClosed()
{
    events_.onClosed();
}

void TerminalView::onSelectionComplete()
{
    events_.onSelectionComplete();
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

void TerminalView::resizeWindow(unsigned _width, unsigned _height, bool _unitInPixels)
{
    events_.resizeWindow(_width, _height, _unitInPixels);
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

void TerminalView::setWindowTitle(std::string_view const& _title)
{
    events_.setWindowTitle(_title);
}

} // namespace terminal::view
