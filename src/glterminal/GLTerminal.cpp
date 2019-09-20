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
#include <glterminal/GLTerminal.h>
#include <glterminal/GLLogger.h>
#include <glterminal/FontManager.h>

#include <terminal/Util.h>

#include <GL/glew.h>
#include <glm/glm.hpp>

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

GLTerminal::GLTerminal(WindowSize const& _winSize,
                       unsigned _width,
                       unsigned _height,
                       Font& _regularFont,
                       CursorShape _cursorShape,
                       glm::vec3 const& _cursorColor,
                       glm::vec4 const& _backgroundColor,
                       string const& _shell,
                       glm::mat4 const& _projectionMatrix,
                       function<void()> _onScreenUpdate,
                       GLLogger& _logger) :
    logger_{ _logger },
    updated_{ false },
    regularFont_{ _regularFont },
    textShaper_{ _regularFont, _projectionMatrix },
    cellBackground_{
        regularFont_.maxAdvance(),
        regularFont_.lineHeight(),
        _projectionMatrix
    },
    cursor_{
        glm::ivec2{
            regularFont_.maxAdvance(),
            regularFont_.lineHeight(),
        },
        _projectionMatrix,
        _cursorShape,
        _cursorColor
    },
    defaultForegroundColor_{ 0.9, 0.9, 0.9, 1.0 }, // TODO: pass in (ideally via both ColorPalette)
    defaultBackgroundColor_{ _backgroundColor },
    terminal_{
        _winSize,
        [this](terminal::LogEvent const& _event) { logger_(_event); },
        bind(&GLTerminal::onScreenUpdateHook, this, _1),
    },
    process_{ terminal_, _shell, {_shell}, envvars },
    processExitWatcher_{ [this]() { wait(); }},
    onScreenUpdate_{ move(_onScreenUpdate) }
{
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
}

GLTerminal::~GLTerminal()
{
    wait();
    processExitWatcher_.join();
}

bool GLTerminal::alive() const
{
    return alive_;
}

bool GLTerminal::send(char32_t _characterEvent, terminal::Modifier _modifier)
{
    logger_.keyPress(_characterEvent, _modifier);
    return terminal_.send(_characterEvent, _modifier);
}

bool GLTerminal::send(terminal::Key _key, terminal::Modifier _modifier)
{
    logger_.keyPress(_key, _modifier);
    return terminal_.send(_key, _modifier);
}

std::string GLTerminal::screenshot() const
{
    return terminal_.screenshot();
}

void GLTerminal::resize(unsigned _width, unsigned _height)
{
    auto const newSize = terminal::WindowSize{
        static_cast<unsigned short>(_width / regularFont_.maxAdvance()),
        static_cast<unsigned short>(_height / regularFont_.lineHeight())
    };

    auto const computeMargin = [this](unsigned _width, unsigned _height, WindowSize const& ws)
    {
        auto const usedHeight = ws.rows * regularFont_.lineHeight();
        auto const usedWidth = ws.columns * regularFont_.maxAdvance();
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

    margin_ = computeMargin(_width, _height, terminal_.size());

    if (doResize)
        cout << fmt::format(
            "Resized to {}x{} ({}x{}) (margin: {}x{}) (CharBox: {}x{})\n",
            newSize.columns, newSize.rows,
            _width, _height,
            margin_.left, margin_.bottom,
            regularFont_.maxAdvance(), regularFont_.lineHeight()
        );
}

void GLTerminal::setTerminalSize(terminal::WindowSize const& _newSize)
{
    if (terminal_.size() != _newSize)
    {
        terminal_.resize(_newSize);
        margin_ = {0, 0};
    }
}

void GLTerminal::setProjection(glm::mat4 const& _projectionMatrix)
{
    cellBackground_.setProjection(_projectionMatrix);
    textShaper_.setProjection(_projectionMatrix);
    cursor_.setProjection(_projectionMatrix);
}

bool GLTerminal::shouldRender()
{
    bool current = updated_.load();

    if (!current)
        return false;

    if (!std::atomic_compare_exchange_weak(&updated_, &current, false))
        return false;

    return true;
}

void GLTerminal::render()
{
    terminal_.render(bind(&GLTerminal::fillCellGroup, this, _1, _2, _3));
    renderCellGroup();

    // TODO: only render when visible
    if (terminal_.cursor().visible)
        cursor_.render(makeCoords(terminal_.cursor().column, terminal_.cursor().row));
}

void GLTerminal::fillCellGroup(terminal::cursor_pos_t _row, terminal::cursor_pos_t _col, terminal::Screen::Cell const& _cell)
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

void GLTerminal::renderCellGroup()
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

glm::ivec2 GLTerminal::makeCoords(cursor_pos_t col, cursor_pos_t row) const
{
    return glm::ivec2{
        margin_.left + (col - 1) * regularFont_.maxAdvance(),
        margin_.bottom + (terminal_.size().rows - row) * regularFont_.lineHeight()
    };
}

glm::vec4 applyColor(Color const& _color, glm::vec4 const& _defaultColor, float _opacity)
{
    using namespace terminal;
    auto const opacity = _defaultColor[3] * _opacity;
    auto const defaultColor = glm::vec4{ _defaultColor.r, _defaultColor.g, _defaultColor.b, _defaultColor.a * _opacity };
    return visit(
        overloaded{
            [=](UndefinedColor) {
                return defaultColor;
            },
            [=](DefaultColor) {
                return defaultColor;
            },
            [=](IndexedColor color) {
                switch (color) {
                    case IndexedColor::Black:
                        return glm::vec4{ 0.0, 0.0, 0.0, opacity };
                    case IndexedColor::Red:
                        return glm::vec4{ 205 / 255.0, 0, 0, opacity };
                    case IndexedColor::Green:
                        return glm::vec4{ 0, 205 / 255.0, 0, opacity };
                    case IndexedColor::Yellow:
                        return glm::vec4{ 205 / 255.0, 205 / 255.0, 0, opacity };
                    case IndexedColor::Blue:
                        return glm::vec4{ 0, 0, 238 / 255.0, opacity };
                    case IndexedColor::Magenta:
                        return glm::vec4{ 205 / 255.0, 0, 205 / 255.0, opacity };
                    case IndexedColor::Cyan:
                        return glm::vec4{ 0, 205 / 255.0, 205 / 255.0, opacity };
                    case IndexedColor::White:
                        return glm::vec4{ 229 / 255.0, 229 / 255.0, 229 / 255.0, opacity };
                    case IndexedColor::Default:
                        return defaultColor;
                }
                return defaultColor;
            },
            [=](BrightColor color) {
                switch (color) {
                    case BrightColor::Black:
                        return glm::vec4{ 0, 0, 0, opacity };
                    case BrightColor::Red:
                        return glm::vec4{ 1, 0, 0, opacity };
                    case BrightColor::Green:
                        return glm::vec4{ 0, 1, 0, opacity };
                    case BrightColor::Yellow:
                        return glm::vec4{ 1, 1, 0, opacity };
                    case BrightColor::Blue:
                        return glm::vec4{ 92 / 255, 92 / 255, 1, opacity };
                    case BrightColor::Magenta:
                        return glm::vec4{ 1, 0, 1, opacity };
                    case BrightColor::Cyan:
                        return glm::vec4{ 0, 1, 1, opacity };
                    case BrightColor::White:
                        return glm::vec4{ 1, 1, 1, opacity };
                }
                return defaultColor;
            },
            [=](RGBColor color) {
                return glm::vec4{ color.red / 255.0, color.green / 255.0, color.blue / 255.0, opacity };
            },
        },
        _color
    );
}

std::pair<glm::vec4, glm::vec4> GLTerminal::makeColors(Screen::GraphicsAttributes const& _attributes) const
{
    float const opacity = [=]() {
        if (_attributes.styles & CharacterStyleMask::Hidden)
            return 0.0f;
        else if (_attributes.styles & CharacterStyleMask::Faint)
            return 0.5f;
        else
            return 1.0f;
    }();

    return (_attributes.styles & CharacterStyleMask::Inverse)
        ? pair{ applyColor(_attributes.backgroundColor, defaultBackgroundColor_, opacity),
                applyColor(_attributes.foregroundColor, defaultForegroundColor_, opacity) }
        : pair{ applyColor(_attributes.foregroundColor, defaultForegroundColor_, opacity),
                applyColor(_attributes.backgroundColor, defaultBackgroundColor_, opacity) };
}

void GLTerminal::wait()
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

void GLTerminal::onScreenUpdateHook(std::vector<terminal::Command> const& _commands)
{
    logger_(TraceOutputEvent{ fmt::format("onScreenUpdate: {} instructions", _commands.size()) });

    for (terminal::Command const& command : _commands)
        logger_(TraceOutputEvent{ to_string(command) });

    updated_.store(true);

    if (onScreenUpdate_)
        onScreenUpdate_();
}
