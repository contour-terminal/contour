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

#include "CellBackground.h"
#include "TextShaper.h"

/// OpenGL-Terminal Object.
class GLTerminal {
  public:
    GLTerminal(unsigned _bottomLeft, unsigned _bottomRight, unsigned _width, unsigned _height);
    ~GLTerminal();

    bool send(char32_t _characterEvent, Modifier _modifier) { return terminal_.send(_characterEvent, _modifier); }
    bool send(Key _key, Modifier _modifier) { return terminal_.send(_key, _modifier); }
    std::string screenshot() const { return terminal_.screenshot(); }

    //void translate(unsigned _bottomLeft, unsigned _bottomRight);
    void resize(unsigned _width, unsigned _height);
    void render();

    bool alive() const;
    void wait();

  private:
    bool alive_ = true;

    TextShaper textShaper_;
    CellBackground cellBackground_;

    terminal::Terminal terminal_;
    terminal::Process process_;
    std::thread processExitWatcher_;
};
