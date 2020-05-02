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

#include "Actions.h"
#include "LoggingSink.h"

#include <terminal/Color.h>
#include <terminal/Commands.h>          // CursorDisplay
#include <terminal/Process.h>
#include <terminal/WindowSize.h>
#include <terminal_view/ShaderConfig.h>

#include <crispy/stdfs.h>

#include <QKeySequence>

#include <chrono>
#include <optional>
#include <string>
#include <variant>
#include <unordered_map>

namespace contour::config {

struct TerminalProfile {
    std::string shell;
    terminal::Process::Environment env;

    terminal::WindowSize terminalSize;

    std::optional<size_t> maxHistoryLineCount;
    size_t historyScrollMultiplier;
    bool autoScrollOnUpdate;

    unsigned short fontSize;
    std::string fontFamily;

    unsigned int tabWidth;

    terminal::ColorProfile colors;

    terminal::CursorShape cursorShape;
    terminal::CursorDisplay cursorDisplay;
    std::chrono::milliseconds cursorBlinkInterval;

    terminal::Opacity backgroundOpacity; // value between 0 (fully transparent) and 0xFF (fully visible).
    bool backgroundBlur; // On Windows 10, this will enable Acrylic Backdrop.
};

using terminal::view::ShaderConfig;
using terminal::view::ShaderClass;

// NB: All strings in here must be UTF8-encoded.
struct Config {
    FileSystem::path backingFilePath;

    std::optional<FileSystem::path> logFilePath;
    LogMask loggingMask;

    bool fullscreen;

    std::unordered_map<std::string, terminal::ColorProfile> colorschemes;
    std::unordered_map<std::string, TerminalProfile> profiles;
    std::string defaultProfileName;

    TerminalProfile* profile(std::string const& _name)
    {
        if (auto i = profiles.find(_name); i != profiles.end())
            return &i->second;
        return nullptr;
    }

    // selection
    std::string wordDelimiters;

    // input mapping
    std::map<QKeySequence, std::vector<actions::Action>> keyMappings;
    std::unordered_map<terminal::MouseEvent, std::vector<actions::Action>> mouseMappings;

    static std::optional<ShaderConfig> loadShaderConfig(ShaderClass _shaderClass);

    ShaderConfig backgroundShader = terminal::view::defaultShaderConfig(ShaderClass::Background);
    ShaderConfig textShader = terminal::view::defaultShaderConfig(ShaderClass::Text);
    ShaderConfig cursorShader = terminal::view::defaultShaderConfig(ShaderClass::Cursor);
};

std::optional<std::string> readConfigFile(std::string const& _filename);

using Logger = std::function<void(std::string const&)>;

void loadConfigFromFile(Config& _config,
                        FileSystem::path const& _fileName,
                        Logger const& _logger);
Config loadConfigFromFile(FileSystem::path const& _fileName,
                          Logger const& _logger);
Config loadConfig(Logger const& _logger);

} // namespace contour::config
