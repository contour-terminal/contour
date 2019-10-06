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
#include <terminal_view/GLLogger.h>
#include <terminal_view/FontManager.h>

#include <terminal/Util.h>

#include <ground/overloaded.h>

#include <GL/glew.h>
#include <glm/glm.hpp>

#include <array>
#include <iostream>
#include <utility>

using namespace std;
using namespace std::placeholders;
using namespace terminal;

auto const envvars = terminal::Process::Environment{
    {"TERM", "xterm-256color"},
    {"COLORTERM", "xterm"},
    {"COLORFGBG", "15;0"},
    {"LINES", ""},
    {"COLUMNS", ""},
    {"TERMCAP", ""}
};

TerminalView::TerminalView(WindowSize const& _winSize,
                           optional<size_t> _maxHistoryLineCount,
                           Font& _regularFont,
                           CursorShape _cursorShape,
                           glm::vec3 const& _cursorColor,
                           terminal::ColorProfile const& _colorProfile,
                           terminal::Opacity _backgroundOpacity,
                           string const& _shell,
                           glm::mat4 const& _projectionMatrix,
                           function<void()> _onScreenUpdate,
                           function<void()> _onWindowTitleChanged,
                           function<void(unsigned int, unsigned int, bool)> _resizeWindow,
                           GLLogger& _logger) :
    logger_{ _logger },
    updated_{ false },
    colorProfile_{ _colorProfile },
    backgroundOpacity_{ _backgroundOpacity },
    regularFont_{ _regularFont },
    textShaper_{ regularFont_.get(), _projectionMatrix },
    cellBackground_{
        glm::vec2{
            regularFont_.get().maxAdvance(),
            regularFont_.get().lineHeight()
        },
        _projectionMatrix
    },
    cursor_{
        glm::ivec2{
            regularFont_.get().maxAdvance(),
            regularFont_.get().lineHeight(),
        },
        _projectionMatrix,
        _cursorShape,
        _cursorColor
    },
    terminal_{
        _winSize,
        move(_maxHistoryLineCount),
        move(_onWindowTitleChanged),
        move(_resizeWindow),
        [this](terminal::LogEvent const& _event) { logger_(_event); },
        bind(&TerminalView::onScreenUpdateHook, this, _1),
    },
    process_{ terminal_, _shell, {_shell}, envvars },
    processExitWatcher_{ [this]() { wait(); }},
    onScreenUpdate_{ move(_onScreenUpdate) }
{
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
}

TerminalView::~TerminalView()
{
    wait();
    processExitWatcher_.join();
}

bool TerminalView::alive() const
{
    return alive_;
}

void TerminalView::setMaxHistoryLineCount(std::optional<size_t> _maxHistoryLineCount)
{
    terminal_.setMaxHistoryLineCount(_maxHistoryLineCount);
}

bool TerminalView::send(terminal::InputEvent const& _inputEvent)
{
    return visit(overloaded{
        [&](KeyInputEvent const& _key) -> bool {
            logger_.keyPress(_key.key, _key.modifier);
            return terminal_.send(_inputEvent);
        },
        [&](CharInputEvent const& _char) -> bool {
            logger_.keyPress(_char.value, _char.modifier);
            return terminal_.send(_inputEvent);
        },
        [&](MousePressEvent const& _mouse) -> bool {
            // TODO: anything else? logging?
            return terminal_.send(_inputEvent);
        },
    }, _inputEvent);
}


std::string TerminalView::screenshot() const
{
    return terminal_.screenshot();
}

void TerminalView::resize(unsigned _width, unsigned _height)
{
    auto const newSize = terminal::WindowSize{
        static_cast<unsigned short>(_width / regularFont_.get().maxAdvance()),
        static_cast<unsigned short>(_height / regularFont_.get().lineHeight())
    };

    auto const computeMargin = [this](WindowSize const& ws, unsigned _width, unsigned _height)
    {
        auto const usedHeight = ws.rows * regularFont_.get().lineHeight();
        auto const usedWidth = ws.columns * regularFont_.get().maxAdvance();
        auto const freeHeight = _height - usedHeight;
        auto const freeWidth = _width - usedWidth;
        auto const bottomMargin = freeHeight / 2;
        auto const leftMargin = freeWidth / 2;
        return Margin{leftMargin, bottomMargin};
    };

    WindowSize const oldSize = terminal_.size();
    bool const doResize = newSize != oldSize; // terminal_.size();
    if (doResize)
        terminal_.resize(newSize);

    margin_ = computeMargin(newSize, _width, _height);

    if (doResize)
        cout << fmt::format(
            "Resized to {}x{} ({}x{}) (margin: {}x{}) (CharBox: {}x{})\n",
            newSize.columns, newSize.rows,
            _width, _height,
            margin_.left, margin_.bottom,
            regularFont_.get().maxAdvance(), regularFont_.get().lineHeight()
        );
}

void TerminalView::setFont(Font& _font)
{
    auto const fontSize = regularFont_.get().fontSize();
    regularFont_ = _font;
    regularFont_.get().setFontSize(fontSize);
    textShaper_.setFont(regularFont_.get());
}

bool TerminalView::setFontSize(unsigned int _fontSize)
{
    if (_fontSize == regularFont_.get().fontSize())
        return false;

    regularFont_.get().setFontSize(_fontSize);
    // TODO: other font styles
    textShaper_.clearGlyphCache();
    cellBackground_.resize(glm::ivec2{regularFont_.get().maxAdvance(), regularFont_.get().lineHeight()});
    cursor_.resize(glm::ivec2{regularFont_.get().maxAdvance(), regularFont_.get().lineHeight()});
    // TODO update margins?

    return true;
}

bool TerminalView::setTerminalSize(terminal::WindowSize const& _newSize)
{
    if (terminal_.size() == _newSize)
        return false;

    terminal_.resize(_newSize);
    margin_ = {0, 0};
    return true;
}

void TerminalView::setProjection(glm::mat4 const& _projectionMatrix)
{
    cellBackground_.setProjection(_projectionMatrix);
    textShaper_.setProjection(_projectionMatrix);
    cursor_.setProjection(_projectionMatrix);
}

bool TerminalView::shouldRender()
{
    bool current = updated_.load();

    if (!current)
        return false;

    if (!std::atomic_compare_exchange_weak(&updated_, &current, false))
        return false;

    return true;
}

bool TerminalView::scrollUp(size_t _numLines)
{
    if (auto const newOffset = min(scrollOffset_ + _numLines, terminal_.historyLineCount()); newOffset != scrollOffset_)
    {
        scrollOffset_ = newOffset;
        return true;
    }
    else
        return false;
}

bool TerminalView::scrollDown(size_t _numLines)
{
    if (auto const newOffset = scrollOffset_ >= _numLines ? scrollOffset_ - _numLines : 0; newOffset != scrollOffset_)
    {
        scrollOffset_ = newOffset;
        return true;
    }
    else
        return false;
}

bool TerminalView::scrollToTop()
{
    if (auto top = terminal_.historyLineCount(); top != scrollOffset_)
    {
        scrollOffset_ = top;
        return true;
    }
    else
        return false;
}

bool TerminalView::scrollToBottom()
{
    if (scrollOffset_ != 0)
    {
        scrollOffset_ = 0;
        return true;
    }
    else
        return false;
}

void TerminalView::render()
{
    terminal_.render(bind(&TerminalView::fillCellGroup, this, _1, _2, _3), scrollOffset_);
    renderCellGroup();

    if (terminal_.cursor().visible && scrollOffset_ + terminal_.cursor().row <= terminal_.size().rows)
        cursor_.render(makeCoords(terminal_.cursor().column, terminal_.cursor().row + static_cast<cursor_pos_t>(scrollOffset_)));
}

void TerminalView::fillCellGroup(terminal::cursor_pos_t _row, terminal::cursor_pos_t _col, terminal::Screen::Cell const& _cell)
{
    if (pendingDraw_.lineNumber == _row && pendingDraw_.attributes == _cell.attributes)
        pendingDraw_.text.push_back(_cell.character);
    else
    {
        if (!pendingDraw_.text.empty())
            renderCellGroup();

        pendingDraw_.reset(_row, _col, _cell.attributes, _cell.character);
    }
}

void TerminalView::renderCellGroup()
{
    auto const [fgColor, bgColor] = makeColors(pendingDraw_.attributes);
    auto const textStyle = FontStyle::Regular;

    if (pendingDraw_.attributes.styles & CharacterStyleMask::Bold)
    {
        // TODO: switch font
    }

    if (pendingDraw_.attributes.styles & CharacterStyleMask::Italic)
    {
        // TODO: *Maybe* update transformation matrix to have chars italic *OR* change font (depending on bold-state)
    }

    if (pendingDraw_.attributes.styles & CharacterStyleMask::Blinking)
    {
        // TODO: update textshaper's shader to blink
    }

    if (pendingDraw_.attributes.styles & CharacterStyleMask::CrossedOut)
    {
        // TODO: render centered horizontal bar through the cell rectangle (we could reuse the TextShaper and a Unicode character for that, respecting opacity!)
    }

    if (pendingDraw_.attributes.styles & CharacterStyleMask::DoublyUnderlined)
    {
        // TODO: render lower-bound horizontal bar through the cell rectangle (we could reuse the TextShaper and a Unicode character for that, respecting opacity!)
    }
    else if (pendingDraw_.attributes.styles & CharacterStyleMask::Underline)
    {
        // TODO: render lower-bound double-horizontal bar through the cell rectangle (we could reuse the TextShaper and a Unicode character for that, respecting opacity!)
    }

    // TODO: stretch background to number of characters instead.
    for (cursor_pos_t i = 0; i < pendingDraw_.text.size(); ++i)
        cellBackground_.render(makeCoords(pendingDraw_.startColumn + i, pendingDraw_.lineNumber), bgColor);

    textShaper_.render(
        makeCoords(pendingDraw_.startColumn, pendingDraw_.lineNumber),
        pendingDraw_.text,
        fgColor,
        textStyle
    );
}

glm::ivec2 TerminalView::makeCoords(cursor_pos_t col, cursor_pos_t row) const
{
    return glm::ivec2{
        margin_.left + (col - 1) * regularFont_.get().maxAdvance(),
        margin_.bottom + (terminal_.size().rows - row) * regularFont_.get().lineHeight()
    };
}

std::pair<glm::vec4, glm::vec4> TerminalView::makeColors(ScreenBuffer::GraphicsAttributes const& _attributes) const
{
    float const opacity = [=]() {
        if (_attributes.styles & CharacterStyleMask::Hidden)
            return 0.0f;
        else if (_attributes.styles & CharacterStyleMask::Faint)
            return 0.5f;
        else
            return 1.0f;
    }();

    auto const applyColor = [_attributes, this](Color const& _color, ColorTarget _target, float _opacity) -> glm::vec4
    {
        RGBColor const rgb = apply(colorProfile_, _color, _target, _attributes.styles & CharacterStyleMask::Bold);
        glm::vec4 const rgba{
            rgb.red / 255.0f,
            rgb.green / 255.0f,
            rgb.blue / 255.0f,
            _opacity
        };
        return rgba;
    };

    float const backgroundOpacity = static_cast<float>(backgroundOpacity_) / 255.0f;

    return (_attributes.styles & CharacterStyleMask::Inverse)
        ? pair{ applyColor(_attributes.backgroundColor, ColorTarget::Background, opacity * backgroundOpacity),
                applyColor(_attributes.foregroundColor, ColorTarget::Foreground, opacity) }
        : pair{ applyColor(_attributes.foregroundColor, ColorTarget::Foreground, opacity),
                applyColor(_attributes.backgroundColor, ColorTarget::Background, opacity * backgroundOpacity) };
}

void TerminalView::wait()
{
    if (!alive_)
        return;

    using terminal::Process;

    while (true)
        if (visit(overloaded{[&](Process::NormalExit) { return true; },
                             [&](Process::SignalExit) { return true; },
                             [&](Process::Suspend) { return false; },
                             [&](Process::Resume) { return false; },
                  },
                  process_.wait()))
            break;

    terminal_.close();
    terminal_.wait();
    alive_ = false;
}

void TerminalView::setTabWidth(unsigned int _tabWidth)
{
    terminal_.setTabWidth(_tabWidth);
}

void TerminalView::setBackgroundOpacity(terminal::Opacity _opacity)
{
    backgroundOpacity_ = _opacity;
}

void TerminalView::onScreenUpdateHook(std::vector<terminal::Command> const& _commands)
{
    logger_(TraceOutputEvent{ fmt::format("onScreenUpdate: {} instructions", _commands.size()) });

    auto const mnemonics = to_mnemonic(_commands, true, true);
    for (auto const& mnemonic : mnemonics)
        logger_(TraceOutputEvent{ mnemonic });

    updated_.store(true);

    if (onScreenUpdate_)
        onScreenUpdate_();
}
