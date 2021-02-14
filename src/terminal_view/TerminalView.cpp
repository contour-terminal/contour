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
#include <terminal_renderer/opengl/OpenGLRenderer.h>

#include <crispy/logger.h>

#include <fmt/ostream.h>

#include <array>
#include <chrono>
#include <memory>
#include <utility>

using std::chrono::milliseconds;
using std::chrono::steady_clock;
using std::make_unique;
using std::nullopt;
using std::optional;
using std::string;
using std::tuple;
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
                           int _logicalDpiX,
                           int _logicalDpiY,
                           renderer::FontDescriptions const& _fontDescriptions,
                           CursorShape _cursorShape, // TODO: remember !
                           CursorDisplay _cursorDisplay,
                           milliseconds _cursorBlinkInterval,
                           terminal::ColorProfile _colorProfile,
                           terminal::Opacity _backgroundOpacity,
                           renderer::Decorator _hyperlinkNormal,
                           renderer::Decorator _hyperlinkHover,
                           unique_ptr<Pty> _pty,
                           Process::ExecInfo const& _shell,
                           unique_ptr<renderer::RenderTarget> _renderTarget) :
    events_{ _events },
    renderer_{
        _pty->screenSize(),
        _logicalDpiX,
        _logicalDpiY,
        _fontDescriptions,
        _colorProfile,
        _backgroundOpacity,
        _hyperlinkNormal,
        _hyperlinkHover,
        move(_renderTarget)
    },
    fontSize_{ _fontDescriptions.size },
    size_{
        static_cast<int>(_pty->screenSize().width * gridMetrics().cellSize.width),
        static_cast<int>(_pty->screenSize().height * gridMetrics().cellSize.height)
    },
    terminal_(
        std::move(_pty),
        *this,
        _maxHistoryLineCount,
        _cursorBlinkInterval,
        _now,
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

void TerminalView::updateFontMetrics()
{
    auto const newMargin = computeMargin(screenSize(), size_.width, size_.height);

    debuglog().write("with font size {}, adjusting margin from {}x{} to {}x{}\n",
                     fontSize_,
                     windowMargin_.left, windowMargin_.bottom,
                     newMargin.left, newMargin.bottom);

    windowMargin_ = newMargin;
    renderer_.updateFontMetrics();
    renderer_.setMargin(windowMargin_.left, windowMargin_.bottom);

    // resize terminalView (same pixels, but adjusted terminal rows/columns and margin)
    resize(size_.width, size_.height);
}

bool TerminalView::setFontSize(text::font_size _fontSize)
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
    auto const usedHeight = static_cast<int>(ws.height * gridMetrics().cellSize.height);
    auto const freeHeight = static_cast<int>(_height - usedHeight);
    auto const bottomMargin = freeHeight;

    //auto const usedWidth = ws.columns * regularFont_.maxAdvance();
    //auto const freeWidth = _width - usedWidth;
    auto constexpr leftMargin = 0;

    return {leftMargin, bottomMargin};
};

void TerminalView::resize(int _width, int _height)
{
    size_ = Size{_width, _height};

    auto const newScreenSize = screenSize();

    windowMargin_ = computeMargin(newScreenSize, _width, _height);

    renderer_.setRenderSize(_width, _height);
    renderer_.setScreenSize(newScreenSize);
    renderer_.setMargin(windowMargin_.left, windowMargin_.bottom);
    //renderer_.clearCache();

    if (newScreenSize != terminal_.screenSize())
    {
        terminal_.resizeScreen(newScreenSize, newScreenSize * cellSize());
        terminal_.clearSelection();
    }

    debuglog().write("Resized to pixelSize: {}, screenSize: {}, margin: {}x{}, cellSize: {}",
        size_,
        newScreenSize,
        windowMargin_.left, windowMargin_.bottom,
        renderer_.cellSize()
    );
}

void TerminalView::setCursorShape(CursorShape _shape)
{
    terminal().setCursorShape(_shape);
}

bool TerminalView::setTerminalSize(Size _cells)
{
    debuglog().write("Setting terminal size from {} to {}\n", terminal_.screenSize(), _cells);

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

void TerminalView::screenUpdated()
{
    events_.screenUpdated();
}

FontDef TerminalView::getFontDef()
{
    auto const fontByStyle = [&](text::font_weight _weight, text::font_slant _slant) -> text::font_description const&
    {
        auto const bold = _weight != text::font_weight::normal;
        auto const italic = _slant != text::font_slant::normal;
        if (bold && italic)
            return renderer_.fontDescriptions().boldItalic;
        else if (bold)
            return renderer_.fontDescriptions().bold;
        else if (italic)
            return renderer_.fontDescriptions().italic;
        else
            return renderer_.fontDescriptions().regular;
    };
    auto const nameOfStyledFont = [&](text::font_weight _weight, text::font_slant _slant) -> string
    {
        auto const& regularFont = renderer_.fontDescriptions().regular;
        auto const& styledFont = fontByStyle(_weight, _slant);
        if (styledFont.familyName == regularFont.familyName)
            return "auto";
        else
            return styledFont.toPattern();
    };
    return {
        renderer_.fontDescriptions().size.pt,
        renderer_.fontDescriptions().regular.familyName,
        nameOfStyledFont(text::font_weight::bold, text::font_slant::normal),
        nameOfStyledFont(text::font_weight::normal, text::font_slant::italic),
        nameOfStyledFont(text::font_weight::bold, text::font_slant::italic),
        renderer_.fontDescriptions().emoji.toPattern()
    };
}

void TerminalView::setFontDef(FontDef const& _fontSpec)
{
    events_.setFontDef(_fontSpec);
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

void TerminalView::setTerminalProfile(std::string const& _configProfileName)
{
    events_.setTerminalProfile(_configProfileName);
}

void TerminalView::discardImage(Image const& _image)
{
    renderer_.discardImage(_image);
}

} // namespace terminal::view
