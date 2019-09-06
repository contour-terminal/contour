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
#include "TextShaper.h"
#include "GLLogger.h"

#include <glm/matrix.hpp>

/// OpenGL-Terminal Object.
class GLTerminal {
  public:
    GLTerminal(unsigned _width, unsigned _height,
               unsigned _fontSize,
               std::string const& _fontFamily,
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
    void renderCell(terminal::cursor_pos_t row, terminal::cursor_pos_t col, terminal::Screen::Cell const& cell);
    void onScreenUpdateHook(std::vector<terminal::Command> const& _commands);

  private:
    bool alive_ = true;

    unsigned width_;
    unsigned height_;

    struct Margin {
        unsigned left{};
        unsigned bottom{};
    };
    Margin margin_{};

    GLLogger& logger_;

    TextShaper textShaper_;
    CellBackground cellBackground_;

    terminal::Terminal terminal_;
    terminal::Process process_;
    std::thread processExitWatcher_;
};
