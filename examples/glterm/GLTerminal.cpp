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
#include "GLTerminal.h"
#include "GLLogger.h"
#include "FontManager.h"

#include <terminal/Util.h>

#include <GL/glew.h>
#include <GLFW/glfw3.h>
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
                       string const& _shell,
                       glm::mat4 const& _projectionMatrix,
                       GLLogger& _logger) :
    width_{ _width },
    height_{ _height },
    logger_{ _logger },
    regularFont_{ _regularFont },
    textShaper_{ _regularFont, _projectionMatrix },
    cellBackground_{
        regularFont_.maxAdvance(),
        regularFont_.lineHeight(),
        _projectionMatrix
    },
    terminal_{
        _winSize,
        [this](terminal::LogEvent const& _event) { logger_(_event); },
        bind(&GLTerminal::onScreenUpdateHook, this, _1),
    },
    process_{ terminal_, _shell, {_shell}, envvars },
    processExitWatcher_{ [this]() { wait(); }}
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
    width_ = _width;
    height_ = _height;

    auto const winSize = terminal::WindowSize{
        static_cast<unsigned short>(height_ / regularFont_.lineHeight()),
        static_cast<unsigned short>(width_ / regularFont_.maxAdvance())
    };
    auto const usedHeight = winSize.rows * regularFont_.lineHeight();
    auto const usedWidth = winSize.columns * regularFont_.maxAdvance();
    auto const freeHeight = _height - usedHeight;
    auto const freeWidth = _width - usedWidth;

    cout << fmt::format("Resized to {}x{} ({}x{}) (free: {}x{}) (CharBox: {}x{})\n",
        winSize.columns, winSize.rows,
        _width, _height,
        freeWidth, freeHeight,
        regularFont_.maxAdvance(), regularFont_.lineHeight()
    );

    terminal_.resize(winSize);

    margin_ = [winSize, this]() {
        auto const usedHeight = winSize.rows * regularFont_.lineHeight();
        auto const usedWidth = winSize.columns * regularFont_.maxAdvance();
        auto const freeHeight = height_ - usedHeight;
        auto const freeWidth = width_ - usedWidth;
        auto const bottomMargin = freeHeight / 2;
        auto const leftMargin = freeWidth / 2;
        return Margin{leftMargin, bottomMargin};
    }();
}

void GLTerminal::setProjection(glm::mat4 const& _projectionMatrix)
{
    cellBackground_.setProjection(_projectionMatrix);
    textShaper_.setProjection(_projectionMatrix);
}

void GLTerminal::render()
{
    terminal_.render(bind(&GLTerminal::fillCellGroup, this, _1, _2, _3));
    renderCellGroup();
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
    float const opacity = makeOpacity(pendingDraw_.attributes);
    auto const fg = glm::vec4{ fgColor.red / 255.0f, fgColor.green / 255.0f, fgColor.blue / 255.0f, opacity };
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
        fg,
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

std::pair<terminal::RGBColor, terminal::RGBColor> GLTerminal::makeColors(Screen::GraphicsAttributes const& _attributes) const
{
    auto constexpr defaultForegroundColor = RGBColor{ 255, 255, 255 };
    auto constexpr defaultBackgroundColor = RGBColor{ 0, 32, 32 };

    return (_attributes.styles & CharacterStyleMask::Inverse)
        ? pair{ toRGB(_attributes.backgroundColor, defaultBackgroundColor),
                toRGB(_attributes.foregroundColor, defaultForegroundColor) }
        : pair{ toRGB(_attributes.foregroundColor, defaultForegroundColor),
                toRGB(_attributes.backgroundColor, defaultBackgroundColor) };
}

float GLTerminal::makeOpacity(GraphicsAttributes const& _attributes) const noexcept
{
    if (_attributes.styles & CharacterStyleMask::Hidden)
        return 0.0f;
    else if (_attributes.styles & CharacterStyleMask::Faint)
        return 0.5f;
    else
        return 1.0f;
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

    glfwPostEmptyEvent();
}
