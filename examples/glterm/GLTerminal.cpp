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

auto const envvars = terminal::Process::Environment{
    {"TERM", "xterm-256color"},
    {"COLORTERM", "xterm"},
    {"COLORFGBG", "15;0"},
    {"LINES", ""},
    {"COLUMNS", ""},
    {"TERMCAP", ""}
};

GLTerminal::GLTerminal(unsigned _bottomLeft, unsigned _bottomRight, unsigned _width, unsigned _height)
    textShaper_{ GLTERM_FONT_PATH , _fontSize },
    cellBackground_{ textShaper_.maxAdvance(), textShaper_.lineHeight() },
    terminal_{
        computeWindowSize(),
        [this](auto const& msg) { log("terminal: {}", msg); },
        bind(&GLTerminal::onScreenUpdateHook, this, _1),
    },
    process_{ terminal_, _shell, {_shell}, envvars },
    processExitWatcher_{ [this]() { wait(); alive_ = false; }}
{
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

    resize(_width, _height);
}

GLTerminal::~GLTerminal()
{
    wait();
}

void GLTerminal::wait()
{
    using namespace terminal;
    while (true)
        if (visit(overloaded{[&](Process::NormalExit) { return true; },
                             [&](Process::SignalExit) { return true; },
                             [&](Process::Suspend) { return false; },
                             [&](Process::Resume) { return false; },
                  },
                  process_.wait()))
            break;

    terminal_.wait();
    processExitWatcher_.join();
}

void GLTerminal::resize(unsigned _width, unsigned _height)
{
    auto const winSize = computeWindowSize();
    auto const usedHeight = winSize.rows * textShaper_.lineHeight();
    auto const usedWidth = winSize.columns * textShaper_.maxAdvance();
    auto const freeHeight = _height - usedHeight;
    auto const freeWidth = _width - usedWidth;

    logger_ << fmt::format("Resized to {}x{} ({}x{}) (free: {}x{}) (CharBox: {}x{})\n",
        winSize.columns, winSize.rows,
        _width, _height,
        freeWidth, freeHeight,
        textShaper_.maxAdvance(), textShaper_.lineHeight()
    );

    terminal_.resize(winSize);

    cellBackground_.onResize(_width, _height);

    textShaper_.shader().use();
    textShaper_.shader().setMat4(
        "projection",
        glm::ortho(0.0f, static_cast<GLfloat>(_width), 0.0f, static_cast<GLfloat>(_height))
    );

}

void GLTerminal::render()
{
    auto const winSize = computeWindowSize();
    auto const usedHeight = winSize.rows * textShaper_.lineHeight();
    auto const usedWidth = winSize.columns * textShaper_.maxAdvance();
    auto const freeHeight = window_.height() - usedHeight;
    auto const freeWidth = window_.width() - usedWidth;
    auto const bottomMargin = freeHeight / 2;
    auto const leftMargin = freeWidth / 2;

    using namespace terminal;

    auto constexpr defaultForegroundColor = RGBColor{ 255, 255, 255 };
    auto constexpr defaultBackgroundColor = RGBColor{ 0, 32, 32 };

    auto const makeCoords = [&](cursor_pos_t col, cursor_pos_t row) {
        return glm::ivec2{
            leftMargin + (col - 1) * textShaper_.maxAdvance(),
            bottomMargin + (winSize.rows - row) * textShaper_.lineHeight()
        };
    };

    terminal_.render([&](cursor_pos_t row, cursor_pos_t col, Screen::Cell const& cell) {
        cellBackground_.render(
            makeCoords(col, row),
            toRGB(cell.attributes.backgroundColor, defaultBackgroundColor)
        );

        RGBColor const fgColor = toRGB(cell.attributes.foregroundColor, defaultForegroundColor);
        //TODO: other SGRs

        if (cell.character && cell.character != ' ')
        {
            textShaper_.render(
                makeCoords(col, row),
                cell.character,
                fgColor.red / 255.0f,
                fgColor.green / 255.0f,
                fgColor.blue / 255.0f
            );
        }
    });
}

bool GLTerminal::send(char32_t _characterEvent, Modifier _modifier)
{
}

bool GLTerminal::send(Key _key, Modifier _modifier)
{
}

