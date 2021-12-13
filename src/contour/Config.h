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

#include <contour/Actions.h>

#include <contour/opengl/ShaderConfig.h>

#include <terminal_renderer/TextRenderer.h>         // FontDescriptions
#include <terminal_renderer/DecorationRenderer.h>   // Decorator

#include <terminal/Color.h>
#include <terminal/InputBinding.h>
#include <terminal/Process.h>
#include <terminal/Sequencer.h>                 // CursorDisplay

#include <text_shaper/font.h>

#include <crispy/size.h>
#include <crispy/stdfs.h>

#include <chrono>
#include <system_error>
#include <optional>
#include <set>
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

enum class Permission
{
    Deny,
    Allow,
    Ask
};

enum class SelectionAction
{
    Nothing,
    CopyToSelectionClipboard,
    CopyToClipboard,
};

using ActionList = std::vector<actions::Action>;
using KeyInputMapping = terminal::InputBinding<terminal::Key, ActionList>;
using CharInputMapping = terminal::InputBinding<char32_t, ActionList>;
using MouseInputMapping = terminal::InputBinding<terminal::MouseButton, ActionList>;

struct InputMappings
{
    std::vector<KeyInputMapping> keyMappings;
    std::vector<CharInputMapping> charMappings;
    std::vector<MouseInputMapping> mouseMappings;
};

namespace helper
{
    inline bool testMatchMode(uint8_t _actualModeFlags,
                       terminal::MatchModes _expected,
                       terminal::MatchModes::Flag _testFlag)
    {
        using MatchModes = terminal::MatchModes;
        switch (_expected.status(_testFlag))
        {
            case MatchModes::Status::Enabled:
                if (!(_actualModeFlags & _testFlag))
                    return false;
                break;
            case MatchModes::Status::Disabled:
                if ((_actualModeFlags & _testFlag))
                    return false;
            case MatchModes::Status::Any:
                break;
        }
        return true;
    }

    inline bool testMatchMode(uint8_t _actualModeFlags,
                       terminal::MatchModes _expected)
    {
        using Flag = terminal::MatchModes::Flag;
        return testMatchMode(_actualModeFlags, _expected, Flag::AlternateScreen)
            && testMatchMode(_actualModeFlags, _expected, Flag::AppCursor)
            && testMatchMode(_actualModeFlags, _expected, Flag::AppKeypad)
            && testMatchMode(_actualModeFlags, _expected, Flag::Select);
    }
}

template <typename Input>
std::vector<actions::Action> const* apply(
    std::vector<terminal::InputBinding<Input, ActionList>> const& _mappings,
    Input _input,
    terminal::Modifier _modifier,
    uint8_t _actualModeFlags
)
{
    for (terminal::InputBinding<Input, ActionList> const& mapping: _mappings)
    {
        if (mapping.modifier == _modifier &&
            mapping.input == _input &&
            helper::testMatchMode(_actualModeFlags, mapping.modes))
        {
            return &mapping.binding;
        }
    }
    return nullptr;
}

struct TerminalProfile {
    terminal::Process::ExecInfo shell;
    bool maximized = false;
    bool fullscreen = false;
    double refreshRate = 0.0; // 0=auto
    terminal::LineOffset copyLastMarkRangeOffset = terminal::LineOffset(0);

    std::string wmClass;

    terminal::PageSize terminalSize = {terminal::LineCount(10), terminal::ColumnCount(40)};
    terminal::VTType terminalId = terminal::VTType::VT525;

    terminal::LineCount maxHistoryLineCount;
    terminal::LineCount historyScrollMultiplier;
    ScrollBarPosition scrollbarPosition = ScrollBarPosition::Right;
    bool hideScrollbarInAltScreen = true;

    bool autoScrollOnUpdate;

    terminal::renderer::FontDescriptions fonts;

    struct {
        Permission captureBuffer = Permission::Ask;
        Permission changeFont = Permission::Ask;
    } permissions;

    terminal::ColorPalette colors{};

    terminal::CursorShape cursorShape;
    terminal::CursorDisplay cursorDisplay;
    std::chrono::milliseconds cursorBlinkInterval;

    terminal::Opacity backgroundOpacity; // value between 0 (fully transparent) and 0xFF (fully visible).
    bool backgroundBlur; // On Windows 10, this will enable Acrylic Backdrop.

    struct {
        terminal::renderer::Decorator normal = terminal::renderer::Decorator::DottedUnderline;
        terminal::renderer::Decorator hover = terminal::renderer::Decorator::Underline;
    } hyperlinkDecoration;
};

using opengl::ShaderConfig;
using opengl::ShaderClass;

// NB: All strings in here must be UTF8-encoded.
struct Config {
    FileSystem::path backingFilePath;

    std::optional<FileSystem::path> logFilePath;

    // Configures the size of the PTY read buffer.
    // Changing this value may result in better or worse throughput performance.
    //
    // This value must be integer-devisable by 16.
    int ptyReadBufferSize = 16384;

    bool reflowOnResize = true;

    std::unordered_map<std::string, terminal::ColorPalette> colorschemes;
    std::unordered_map<std::string, TerminalProfile> profiles;
    std::string defaultProfileName;

    TerminalProfile* profile(std::string const& _name)
    {
        if (auto i = profiles.find(_name); i != profiles.end())
            return &i->second;
        return nullptr;
    }

    TerminalProfile const* profile(std::string const& _name) const
    {
        if (auto i = profiles.find(_name); i != profiles.end())
            return &i->second;
        return nullptr;
    }

    TerminalProfile& profile() noexcept { return *profile(defaultProfileName); }
    TerminalProfile const& profile() const noexcept { return *profile(defaultProfileName); }

    // selection
    std::string wordDelimiters;
    terminal::Modifier bypassMouseProtocolModifier = terminal::Modifier::Shift;
    SelectionAction onMouseSelection = SelectionAction::CopyToSelectionClipboard;
    terminal::Modifier mouseBlockSelectionModifier = terminal::Modifier::Control;

    // input mapping
    InputMappings inputMappings;

    static std::optional<ShaderConfig> loadShaderConfig(ShaderClass _shaderClass);

    ShaderConfig backgroundShader = opengl::defaultShaderConfig(ShaderClass::Background);
    ShaderConfig textShader = opengl::defaultShaderConfig(ShaderClass::Text);

    bool spawnNewProcess = false;

    bool sixelScrolling = true;
    bool sixelCursorConformance = true;
    terminal::ImageSize maxImageSize = {}; // default to runtime system screen size.
    int maxImageColorRegisters = 4096;

    std::set<std::string> experimentalFeatures;
};

std::optional<std::string> readConfigFile(std::string const& _filename);

void loadConfigFromFile(Config& _config, FileSystem::path const& _fileName);
Config loadConfigFromFile(FileSystem::path const& _fileName);
Config loadConfig();

std::string createDefaultConfig();
std::error_code createDefaultConfig(FileSystem::path const& _path);
std::string defaultConfigFilePath();

} // namespace contour::config

namespace fmt // {{{
{
    template <>
    struct formatter<contour::config::Permission> {
        template <typename ParseContext>
        constexpr auto parse(ParseContext& ctx) { return ctx.begin(); }
        template <typename FormatContext>
        auto format(contour::config::Permission const& _perm, FormatContext& ctx)
        {
            switch (_perm)
            {
                case contour::config::Permission::Allow:
                    return format_to(ctx.out(), "allow");
                case contour::config::Permission::Deny:
                    return format_to(ctx.out(), "deny");
                case contour::config::Permission::Ask:
                    return format_to(ctx.out(), "ask");
            }
            return format_to(ctx.out(), "({})", unsigned(_perm));
        }
    };

    template <>
    struct formatter<contour::config::SelectionAction>
    {
        template <typename ParseContext>
        constexpr auto parse(ParseContext& ctx)
        {
            return ctx.begin();
        }
        using SelectionAction = contour::config::SelectionAction;
        template <typename FormatContext>
        auto format(SelectionAction _value, FormatContext& ctx)
        {
            switch (_value)
            {
                case SelectionAction::CopyToClipboard:
                    return format_to(ctx.out(), "CopyToClipboard");
                case SelectionAction::CopyToSelectionClipboard:
                    return format_to(ctx.out(), "CopyToSelectionClipboard");
                case SelectionAction::Nothing:
                    return format_to(ctx.out(), "Waiting");
            }
            return format_to(ctx.out(), "{}", static_cast<unsigned>(_value));
        }
    };

} // }}}
