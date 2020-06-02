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

#include <optional>
#include <string>
#include <variant>

namespace contour::actions {

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
struct ScrollMarkUp{};
struct ScrollMarkDown{};
struct ScrollToTop{};
struct ScrollToBottom{};
struct PasteClipboard{};
struct CopySelection{};
struct PasteSelection{};
struct ChangeProfile{ std::string name; };
struct NewTerminal{ std::optional<std::string> profileName; };
struct OpenConfiguration{};
struct OpenFileManager{};
struct Quit{};
// CloseTab
// OpenTab
// FocusNextTab
// FocusPreviousTab

using Action = std::variant<
    ToggleFullScreen,
    ScreenshotVT,
    IncreaseFontSize,
    DecreaseFontSize,
    IncreaseOpacity,
    DecreaseOpacity,
    SendChars,
    WriteScreen,
    ScrollOneUp,
    ScrollOneDown,
    ScrollUp,
    ScrollDown,
    ScrollPageUp,
    ScrollPageDown,
    ScrollMarkUp,
    ScrollMarkDown,
    ScrollToTop,
    ScrollToBottom,
    CopySelection,
    PasteSelection,
    PasteClipboard,
    ChangeProfile,
    NewTerminal,
    OpenConfiguration,
    OpenFileManager,
    Quit
>;

std::optional<Action> fromString(std::string const& _name);

} // namespace contour::actions
