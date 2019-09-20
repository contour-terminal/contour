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

#include "Config.h"
#include "Window.h"
#include "FileChangeWatcher.h"

#include <glterminal/FontManager.h>
#include <glterminal/GLLogger.h>
#include <glterminal/GLTerminal.h>

#include <filesystem>
#include <string>

class AeroTerminal {
public:
    explicit AeroTerminal(Config const& _config);
    ~AeroTerminal();

    int main();

private:
    void render();
    void onResize();
    void onKey(int _key, int _scanCode, int _action, int _mods);
    void onChar(char32_t _char);
    void onContentScale(float _xs, float _ys);
    void onScreenUpdate();
    void onConfigReload(FileChangeWatcher::Event _event);
    void loadConfigValues();

private:
    Config config_;
    GLLogger logger_;
    FontManager fontManager_;
    Font& regularFont_;
    Window window_;
    GLTerminal terminalView_;
    bool keyHandled_ = false;
    bool configReloadPending_ = false;
    FileChangeWatcher configFileChangeWatcher_;
};
