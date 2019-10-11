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

#include <terminal_view/GLCursor.h>
#include <terminal_view/GLLogger.h>
#include <terminal/Color.h>
#include <terminal/WindowSize.h>
#include <terminal/Process.h>
#include <ground/stdfs.h>
#include <optional>
#include <string>
#include <variant>
#include <unordered_map>

namespace actions {
    struct ToggleFullScreen{};
    struct ScreenshotVT{};
    struct IncreaseFontSize{};
    struct DecreaseFontSize{};
    struct IncreaseOpacity{};
    struct DecreaseOpacity{};
    struct SendChars{ std::string chars; };
    struct WriteScreen{ std::string chars; }; // "\033[2J\033[3J"
    struct ScrollOneUp{};
    struct ScrollOneDown{};
    struct ScrollUp{};
    struct ScrollDown{};
    struct ScrollPageUp{};
    struct ScrollPageDown{};
    struct ScrollToTop{};
    struct ScrollToBottom{};
	struct PasteClipboard{};
	struct CopySelection{};
	struct PasteSelection{};
	// Quit
	// CloseTab
	// OpenTab
	// FocusNextTab
	// FocusPreviousTab
}

using Action = std::variant<
    actions::ToggleFullScreen,
    actions::ScreenshotVT,
    actions::IncreaseFontSize,
    actions::DecreaseFontSize,
    actions::IncreaseOpacity,
    actions::DecreaseOpacity,
    actions::SendChars,
    actions::WriteScreen,
    actions::ScrollOneUp,
    actions::ScrollOneDown,
    actions::ScrollUp,
    actions::ScrollDown,
    actions::ScrollPageUp,
    actions::ScrollPageDown,
    actions::ScrollToTop,
    actions::ScrollToBottom,
    actions::CopySelection,
    actions::PasteSelection,
    actions::PasteClipboard
>;

struct InputMapping {
    terminal::InputEvent input;
    Action action;
};

struct Config {
    FileSystem::path backingFilePath;
    std::optional<FileSystem::path> logFilePath;

    std::string shell = terminal::Process::loginShell();
    terminal::WindowSize terminalSize = {80, 25};
    std::optional<size_t> maxHistoryLineCount = {8000};
    size_t historyScrollMultiplier = 3;
    bool autoScrollOnUpdate = true;
    bool fullscreen = false;
    unsigned short fontSize = 12;
    std::string fontFamily = "Fira Code, Ubuntu Mono, Consolas, monospace";
    CursorShape cursorShape = CursorShape::Block;
    bool cursorBlinking = true;
    unsigned int tabWidth = 8;
    terminal::Opacity backgroundOpacity = terminal::Opacity::Opaque; // value between 0 (fully transparent) and 0xFF (fully visible).
    bool backgroundBlur = false; // On Windows 10, this will enable Acrylic Backdrop.
    LogMask loggingMask = LogMask::ParserError | LogMask::InvalidOutput | LogMask::UnsupportedOutput;

    std::string wordDelimiters = " /\\()\"'-.,:;<>~!@#$%^&*|+=[]{}~?\xE2\x94\x82";

    terminal::ColorProfile colorProfile{};
    std::vector<InputMapping> inputMappings;

    static std::vector<InputMapping> defaultInputMappings()
    {
        return {
            {terminal::KeyInputEvent{terminal::Key::Enter, terminal::Modifier::Alt}, actions::ToggleFullScreen{}},
            {terminal::CharInputEvent{'=', terminal::Modifier::Control + terminal::Modifier::Shift}, actions::IncreaseFontSize{}},
            {terminal::CharInputEvent{'-', terminal::Modifier::Control + terminal::Modifier::Shift}, actions::DecreaseFontSize{}},
            {terminal::MousePressEvent{terminal::MouseButton::WheelUp, terminal::Modifier::Control}, actions::IncreaseFontSize{}},
            {terminal::MousePressEvent{terminal::MouseButton::WheelDown, terminal::Modifier::Control}, actions::DecreaseFontSize{}},
            {terminal::MousePressEvent{terminal::MouseButton::WheelUp, terminal::Modifier::Alt}, actions::IncreaseOpacity{}},
            {terminal::MousePressEvent{terminal::MouseButton::WheelDown, terminal::Modifier::Alt}, actions::DecreaseOpacity{}},
            {terminal::KeyInputEvent{terminal::Key::UpArrow, terminal::Modifier::Shift}, actions::ScrollOneUp{}},
            {terminal::KeyInputEvent{terminal::Key::DownArrow, terminal::Modifier::Shift}, actions::ScrollOneDown{}},
            {terminal::MousePressEvent{terminal::MouseButton::WheelUp, terminal::Modifier::None}, actions::ScrollUp{}},
            {terminal::MousePressEvent{terminal::MouseButton::WheelDown, terminal::Modifier::None}, actions::ScrollDown{}},
            {terminal::MousePressEvent{terminal::MouseButton::WheelUp, terminal::Modifier::Shift}, actions::ScrollPageUp{}},
            {terminal::MousePressEvent{terminal::MouseButton::WheelDown, terminal::Modifier::Shift}, actions::ScrollPageDown{}},
            {terminal::KeyInputEvent{terminal::Key::PageUp, terminal::Modifier::Shift}, actions::ScrollPageUp{}},
            {terminal::KeyInputEvent{terminal::Key::PageDown, terminal::Modifier::Shift}, actions::ScrollPageDown{}},
            {terminal::KeyInputEvent{terminal::Key::Home, terminal::Modifier::Control}, actions::ScrollToTop{}},
            {terminal::KeyInputEvent{terminal::Key::End, terminal::Modifier::Control}, actions::ScrollToBottom{}},
            {terminal::CharInputEvent{'c', terminal::Modifier::Control + terminal::Modifier::Shift}, actions::CopySelection{}},
            {terminal::CharInputEvent{'v', terminal::Modifier::Control + terminal::Modifier::Shift}, actions::PasteClipboard{}},
            {terminal::MousePressEvent{terminal::MouseButton::Middle, terminal::Modifier::None}, actions::PasteSelection{}},
        };
    }
};

std::optional<int> loadConfigFromCLI(Config& _config, int argc, char const* argv[]);
void loadConfigFromFile(Config& _config, std::string const& _fileName);

std::string serializeYaml(Config const& _config);
void saveConfigToFile(Config const& _config, std::string const& _fileName);
