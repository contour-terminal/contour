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
#pragma once

#include <terminal/Process.h>
#include <terminal/Terminal.h>
#include <string>
#include <vector>

#include "CellBackground.h"
#include "FontManager.h"
#include "GLTextShaper.h"
#include "GLLogger.h"

#include <glm/matrix.hpp>

class Font;

/// OpenGL-Terminal Object.
class GLTerminal {
public:
    GLTerminal(
        terminal::WindowSize const& _winSize,
        unsigned _width, unsigned _height,
        Font& _fontFamily,
        std::string const& _shell,
        glm::mat4 const& _projectionMatrix,
        GLLogger& _logger);
    ~GLTerminal();

    bool send(char32_t _characterEvent, terminal::Modifier _modifier);
    bool send(terminal::Key _key, terminal::Modifier _modifier);
    std::string screenshot() const;

    //void translate(unsigned _bottomLeft, unsigned _bottomRight);
    void resize(unsigned _width, unsigned _height);
    void setProjection(glm::mat4 const& _projectionMatrix);
    void render();

    bool alive() const;
    void wait();

private:
    using cursor_pos_t = terminal::cursor_pos_t;
    using RGBColor = terminal::RGBColor;
    using GraphicsAttributes = terminal::Screen::GraphicsAttributes;
    using Cell = terminal::Screen::Cell;

    /// Renders and then clears current cell group if current @p _cell cannot be appended, or appends to current cell group otherwise.
    void fillCellGroup(cursor_pos_t _row, cursor_pos_t _col, Cell const& _cell);
    void renderCellGroup();
    void onScreenUpdateHook(std::vector<terminal::Command> const& _commands);

    glm::ivec2 makeCoords(cursor_pos_t col, cursor_pos_t row) const;
    std::pair<RGBColor, RGBColor> makeColors(GraphicsAttributes const& _attributes) const;
    float makeOpacity(GraphicsAttributes const& _attributes) const noexcept;

private:
    bool alive_ = true;

    struct PendingDraw {
        cursor_pos_t lineNumber{};
        cursor_pos_t startColumn{};
        GraphicsAttributes attributes{};
        std::vector<char32_t> text{};

        void reset(cursor_pos_t _row, cursor_pos_t _col, GraphicsAttributes const& _attributes, char32_t _char)
        {
            lineNumber = _row;
            startColumn = _col;
            attributes = _attributes;
            text.clear();
            text.push_back(_char);
        }
    };
    PendingDraw pendingDraw_;

    unsigned width_;
    unsigned height_;

    struct Margin {
        unsigned left{};
        unsigned bottom{};
    };
    Margin margin_{};

    GLLogger& logger_;

    Font& regularFont_;
    GLTextShaper textShaper_;
    CellBackground cellBackground_;

    terminal::Terminal terminal_;
    terminal::Process process_;
    std::thread processExitWatcher_;
};
