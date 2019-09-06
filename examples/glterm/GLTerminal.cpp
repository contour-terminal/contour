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

#include <terminal/Util.h>

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

GLTerminal::GLTerminal(unsigned _width,
                       unsigned _height,
                       unsigned _fontSize,
                       string const& _fontFamily,
                       string const& _shell,
                       glm::mat4 const& _projectionMatrix,
                       GLLogger& _logger) :
    width_{ _width },
    height_{ _height },
    logger_{ _logger },
    textShaper_{ _fontFamily, _fontSize, _projectionMatrix },
    cellBackground_{
        textShaper_.maxAdvance(),
        textShaper_.lineHeight(),
        _projectionMatrix
    },
    terminal_{
        terminal::WindowSize{
            static_cast<unsigned short>(width_ / textShaper_.maxAdvance()),
            static_cast<unsigned short>(height_ / textShaper_.lineHeight())
        },
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
        static_cast<unsigned short>(height_ / textShaper_.lineHeight()),
        static_cast<unsigned short>(width_ / textShaper_.maxAdvance())
    };
    auto const usedHeight = winSize.rows * textShaper_.lineHeight();
    auto const usedWidth = winSize.columns * textShaper_.maxAdvance();
    auto const freeHeight = _height - usedHeight;
    auto const freeWidth = _width - usedWidth;

    cout << fmt::format("Resized to {}x{} ({}x{}) (free: {}x{}) (CharBox: {}x{})\n",
        winSize.columns, winSize.rows,
        _width, _height,
        freeWidth, freeHeight,
        textShaper_.maxAdvance(), textShaper_.lineHeight()
    );

    terminal_.resize(winSize);

    margin_ = [winSize, this]() {
        auto const usedHeight = winSize.rows * textShaper_.lineHeight();
        auto const usedWidth = winSize.columns * textShaper_.maxAdvance();
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
    terminal_.render(bind(&GLTerminal::renderCell, this, _1, _2, _3));
}

void GLTerminal::renderCell(terminal::cursor_pos_t row, terminal::cursor_pos_t col, terminal::Screen::Cell const& cell)
{
    auto const makeCoords = [this](cursor_pos_t col, cursor_pos_t row) {
        return glm::ivec2{
            margin_.left + (col - 1) * textShaper_.maxAdvance(),
            margin_.bottom + (terminal_.size().rows - row) * textShaper_.lineHeight()
        };
    };

    auto const [fgColor, bgColor] = [&]() {
        auto constexpr defaultForegroundColor = RGBColor{ 255, 255, 255 };
        auto constexpr defaultBackgroundColor = RGBColor{ 0, 32, 32 };

        return (cell.attributes.styles & CharacterStyleMask::Inverse)
            ? pair{ toRGB(cell.attributes.backgroundColor, defaultBackgroundColor),
                    toRGB(cell.attributes.foregroundColor, defaultForegroundColor) }
            : pair{ toRGB(cell.attributes.foregroundColor, defaultForegroundColor),
                    toRGB(cell.attributes.backgroundColor, defaultBackgroundColor) };
    }();

    float const opacity = [&]() {
        if (cell.attributes.styles & CharacterStyleMask::Hidden)
            return 0.0f;
        else if (cell.attributes.styles & CharacterStyleMask::Faint)
            return 0.5f;
        else
            return 1.0f;
    }();

    if (cell.attributes.styles & CharacterStyleMask::Bold)
    {
        // TODO: switch font
    }

    if (cell.attributes.styles & CharacterStyleMask::Italic)
    {
        // TODO: *Maybe* update transformation matrix to have chars italic *OR* change font (depending on bold-state)
    }

    if (cell.attributes.styles & CharacterStyleMask::Blinking)
    {
        // TODO: update textshaper's shader to blink
    }

    if (cell.attributes.styles & CharacterStyleMask::CrossedOut)
    {
        // TODO: render centered horizontal bar through the cell rectangle (we could reuse the TextShaper and a Unicode character for that, respecting opacity!)
    }

    if (cell.attributes.styles & CharacterStyleMask::DoublyUnderlined)
    {
        // TODO: render lower-bound horizontal bar through the cell rectangle (we could reuse the TextShaper and a Unicode character for that, respecting opacity!)
    }
    else if (cell.attributes.styles & CharacterStyleMask::Underline)
    {
        // TODO: render lower-bound double-horizontal bar through the cell rectangle (we could reuse the TextShaper and a Unicode character for that, respecting opacity!)
    }

    cellBackground_.render(makeCoords(col, row), bgColor);

    if (cell.character && cell.character != ' ')
    {
        textShaper_.render(
            makeCoords(col, row),
            cell.character,
            glm::vec4{
                fgColor.red / 255.0f,
                fgColor.green / 255.0f,
                fgColor.blue / 255.0f,
                opacity
            },
            TextStyle::Regular
        );
    }
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
}
