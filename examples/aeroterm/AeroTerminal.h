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

#include <glterminal/FontManager.h>
#include <glterminal/GLLogger.h>
#include <glterminal/GLTerminal.h>
#include "Window.h"

#include <string>

class AeroTerminal {
public:
    AeroTerminal(
        terminal::WindowSize const& _winSize,
        unsigned short _fontSize,
        std::string const& _fontFamily,
        CursorShape _cursorShape,
        glm::vec3 const& _cursorColor,
        glm::vec4 const& _backgroundColor,
        bool _backgroundBlur,
        std::string const& _shell,
        LogMask _logMask);

    ~AeroTerminal();

    int main();

private:
    void render();
    void onResize(unsigned _width, unsigned _height);
    void onKey(int _key, int _scanCode, int _action, int _mods);
    void onChar(char32_t _char);
    void onContentScale(float _xs, float _ys);
    void onScreenUpdate();

private:
    GLLogger logger_;
    FontManager fontManager_;
    Font& regularFont_;
    Window window_;
    GLTerminal terminalView_;
    unsigned int lastCharacter_ = 0;
};
