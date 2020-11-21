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
using std::unique_ptr;

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
                           unique_ptr<Pty> _pty,
                           Process::ExecInfo const& _shell,
                           QMatrix4x4 const& _projectionMatrix,
                           ShaderConfig const& _backgroundShaderConfig,
                           ShaderConfig const& _textShaderConfig,
                           Logger _logger) :
    events_{ _events },
    logger_{ move(_logger) },
    fonts_{ _fonts },
    size_{
        static_cast<int>(_pty->screenSize().width * _fonts.regular.first.get().maxAdvance()),
        static_cast<int>(_pty->screenSize().height * _fonts.regular.first.get().lineHeight())
    },
    renderer_{
        logger_,
        _pty->screenSize(),
        _fonts,
        _colorProfile,
        _backgroundOpacity,
        _hyperlinkNormal,
        _hyperlinkHover,
        _backgroundShaderConfig,
        _textShaderConfig,
        _projectionMatrix
    },
    terminal_(
        std::move(_pty),
        *this,
        _maxHistoryLineCount,
        _cursorBlinkInterval,
        _now,
        logger_,
        _wordDelimiters
    ),
    process_{ _shell, terminal_.device() },
    processExitWatcher_{ [this]() {
        (void) process_.wait();
        terminal_.device().close();
    } },
    colorProfile_{_colorProfile},
    defaultColorProfile_{_colorProfile}
{
    terminal_.setCursorDisplay(_cursorDisplay);
    terminal_.setCursorShape(_cursorShape);
    terminal_.screen().setCellPixelSize(renderer_.cellSize());
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
            if (colorProfile_.selectionForeground.has_value())
                return colorProfile_.selectionForeground.value();
            else
                return nullopt;
        case DynamicColorName::HighlightBackgroundColor:
            if (colorProfile_.selectionBackground.has_value())
                return colorProfile_.selectionBackground.value();
            else
                return nullopt;
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
    auto const newMargin = computeMargin(screenSize(), size_.width, size_.height);

#if 0 // !defined(NDEBUG)
    std::cout << fmt::format(
        "TerminalView.setFont(size={}): adjusting margin from {}x{} to {}x{}\n",
        _fonts.regular.first.get().fontSize(),
        windowMargin_.left, windowMargin_.bottom,
        newMargin.left, newMargin.bottom);
#endif

    windowMargin_ = newMargin;
    renderer_.setFont(_fonts);
    renderer_.setMargin(windowMargin_.left, windowMargin_.bottom);

    // resize terminalView (same pixels, but adjusted terminal rows/columns and margin)
    resize(size_.width, size_.height);
}

bool TerminalView::setFontSize(int _fontSize)
{
    if (!renderer_.setFontSize(_fontSize))
        return false;

    auto const newMargin = computeMargin(screenSize(), size_.width, size_.height);
    windowMargin_ = newMargin;
    renderer_.setMargin(windowMargin_.left, windowMargin_.bottom);

    // resize terminalView (same pixels, but adjusted terminal rows/columns and margin)
    resize(size_.width, size_.height);
    return true;
}

TerminalView::WindowMargin TerminalView::computeMargin(Size const& ws,
                                                       [[maybe_unused]] unsigned _width,
                                                       unsigned _height) const noexcept
{
    auto const usedHeight = static_cast<int>(ws.height * fonts_.regular.first.get().lineHeight());
    auto const freeHeight = static_cast<int>(_height - usedHeight);
    auto const bottomMargin = freeHeight;

    //auto const usedWidth = ws.columns * regularFont_.get().maxAdvance();
    //auto const freeWidth = _width - usedWidth;
    auto constexpr leftMargin = 0;

    return {leftMargin, bottomMargin};
};

void TerminalView::resize(int _width, int _height)
{
    size_ = Size{_width, _height};

    auto const newScreenSize = screenSize();

    windowMargin_ = computeMargin(newScreenSize, _width, _height);

    renderer_.setScreenSize(newScreenSize);
    renderer_.setMargin(windowMargin_.left, windowMargin_.bottom);
    //renderer_.clearCache();

    if (newScreenSize != terminal_.screenSize())
    {
        terminal_.resizeScreen(newScreenSize, newScreenSize * cellSize());
        terminal_.clearSelection();
    }

#if !defined(NDEBUG)
    std::cout << fmt::format(
        "Resized to pixelSize: {}, screenSize: {}, margin: {}x{}, cellSize: {}\n",
        size_,
        newScreenSize,
        windowMargin_.left, windowMargin_.bottom,
        renderer_.cellSize()
    );
#endif
}

void TerminalView::setCursorShape(CursorShape _shape)
{
    terminal().setCursorShape(_shape);
}

bool TerminalView::setTerminalSize(Size _cells)
{
#if !defined(NDEBUG)
    std::cout << fmt::format("Setting terminal size from {} to {}\n", terminal_.screenSize(), _cells);
#endif

    if (terminal_.screenSize() == _cells)
        return false;

    renderer_.setScreenSize(_cells);
    terminal_.resizeScreen(_cells, _cells * cellSize());

    return true;
}

uint64_t TerminalView::render(steady_clock::time_point const& _now, bool _pressure)
{
    return renderer_.render(terminal_, _now, terminal().currentMousePosition(), _pressure);
}

Process::ExitStatus TerminalView::waitForProcessExit()
{
    processExitWatcher_.join();
    return process_.checkStatus().value();
}

void TerminalView::bell()
{
    events_.bell();
}

void TerminalView::bufferChanged(ScreenType _type)
{
    events_.bufferChanged(_type);
}

void TerminalView::commands()
{
    events_.commands();
}

void TerminalView::copyToClipboard(std::string_view const& _data)
{
    events_.copyToClipboard(_data);
}

void TerminalView::dumpState()
{
    events_.dumpState();
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
            colorProfile_.selectionForeground = defaultColorProfile_.selectionForeground;
            break;
        case DynamicColorName::HighlightBackgroundColor:
            colorProfile_.selectionBackground = defaultColorProfile_.selectionBackground;
            break;
    }
}

void TerminalView::resizeWindow(int _width, int _height, bool _unitInPixels)
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
            colorProfile_.selectionForeground = _value;
            break;
        case DynamicColorName::HighlightBackgroundColor:
            colorProfile_.selectionBackground = _value;
            break;
    }
}

void TerminalView::setWindowTitle(std::string_view const& _title)
{
    events_.setWindowTitle(_title);
}

void TerminalView::discardImage(Image const& _image)
{
    renderer_.discardImage(_image);
}

} // namespace terminal::view
