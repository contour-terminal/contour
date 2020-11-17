/**
 * This file is part of the "libterminal" project
 *   Copyright (c) 2019-2020 Christian Parpart <christian@parpart.family>
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
#include <terminal/Process.h>
#include <terminal/Sequencer.h>               // CursorDisplay
#include <terminal/Size.h>
#include <terminal_view/ShaderConfig.h>
#include <terminal_view/DecorationRenderer.h> // Decorator

#include <crispy/stdfs.h>

#include <QKeySequence>

#include <chrono>
#include <system_error>
#include <optional>
#include <string>
#include <variant>
#include <unordered_map>

namespace contour::config {

enum class ScrollBarPosition
{
    Hidden,
    Left,
    Right
};

struct FontSpec
{
    std::string pattern;
    std::vector<std::string> features = {};
};

inline bool operator==(FontSpec const& a, FontSpec const& b) noexcept
{
    return a.pattern == b.pattern && a.features == b.features;
}

inline bool operator!=(FontSpec const& a, FontSpec const& b) noexcept
{
    return !(a == b);
}

struct FontSpecList
{
    FontSpec regular;
    FontSpec bold;
    FontSpec italic;
    FontSpec boldItalic;
    FontSpec emoji = {"emoji"};
};

inline bool operator==(FontSpecList const& a, FontSpecList const& b) noexcept
{
    return a.regular == b.regular
        && a.bold == b.bold
        && a.italic == b.italic
        && a.boldItalic == b.boldItalic
        && a.emoji == b.emoji;
}

inline bool operator!=(FontSpecList const& a, FontSpecList const& b) noexcept
{
    return !(a == b);
}

struct TerminalProfile {
    terminal::Process::ExecInfo shell;

    terminal::Size terminalSize;

    std::optional<int> maxHistoryLineCount;
    int historyScrollMultiplier;
    bool autoScrollOnUpdate;

    short fontSize;
    FontSpecList fonts;

    int tabWidth;

    terminal::ColorProfile colors;

    terminal::CursorShape cursorShape;
    terminal::CursorDisplay cursorDisplay;
    std::chrono::milliseconds cursorBlinkInterval;

    terminal::Opacity backgroundOpacity; // value between 0 (fully transparent) and 0xFF (fully visible).
    bool backgroundBlur; // On Windows 10, this will enable Acrylic Backdrop.

    struct {
        terminal::view::Decorator normal = terminal::view::Decorator::DottedUnderline;
        terminal::view::Decorator hover = terminal::view::Decorator::Underline;
    } hyperlinkDecoration;
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

    bool sixelScrolling = false;
    bool sixelCursorConformance = true;
    terminal::Size maxImageSize = {2000, 2000};
    int maxImageColorRegisters = 256;

    ScrollBarPosition scrollbarPosition = ScrollBarPosition::Right;
    bool hideScrollbarInAltScreen = true;
};

std::optional<std::string> readConfigFile(std::string const& _filename);

using Logger = std::function<void(std::string const&)>;

void loadConfigFromFile(Config& _config,
                        FileSystem::path const& _fileName,
                        Logger const& _logger);
Config loadConfigFromFile(FileSystem::path const& _fileName,
                          Logger const& _logger);
Config loadConfig(Logger const& _logger);

std::error_code createDefaultConfig(FileSystem::path const& _path);

} // namespace contour::config
