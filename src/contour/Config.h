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
#include <contour/display/ShaderConfig.h>

#include <terminal/Color.h>
#include <terminal/ColorPalette.h>
#include <terminal/InputBinding.h>
#include <terminal/Sequencer.h> // CursorDisplay

#include <vtpty/Process.h>

#include <vtrasterizer/Decorator.h>
#include <vtrasterizer/FontDescriptions.h>

#include <text_shaper/font.h>
#include <text_shaper/mock_font_locator.h>

#include <crispy/size.h>
#include <crispy/stdfs.h>

#include <chrono>
#include <optional>
#include <set>
#include <string>
#include <system_error>
#include <unordered_map>
#include <variant>

namespace contour::config
{

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
                break;
            case MatchModes::Status::Any: break;
        }
        return true;
    }

    inline bool testMatchMode(uint8_t _actualModeFlags, terminal::MatchModes _expected)
    {
        using Flag = terminal::MatchModes::Flag;
        return testMatchMode(_actualModeFlags, _expected, Flag::AlternateScreen)
               && testMatchMode(_actualModeFlags, _expected, Flag::AppCursor)
               && testMatchMode(_actualModeFlags, _expected, Flag::AppKeypad)
               && testMatchMode(_actualModeFlags, _expected, Flag::Select)
               && testMatchMode(_actualModeFlags, _expected, Flag::Insert)
               && testMatchMode(_actualModeFlags, _expected, Flag::Search);
    }
} // namespace helper

template <typename Input>
std::vector<actions::Action> const* apply(
    std::vector<terminal::InputBinding<Input, ActionList>> const& _mappings,
    Input _input,
    terminal::Modifier _modifier,
    uint8_t _actualModeFlags)
{
    for (terminal::InputBinding<Input, ActionList> const& mapping: _mappings)
    {
        if (mapping.modifier == _modifier && mapping.input == _input
            && helper::testMatchMode(_actualModeFlags, mapping.modes))
        {
            return &mapping.binding;
        }
    }
    return nullptr;
}

struct CursorConfig
{
    terminal::CursorShape cursorShape;
    terminal::CursorDisplay cursorDisplay;
    std::chrono::milliseconds cursorBlinkInterval;
};

struct InputModeConfig
{
    CursorConfig cursor;
};

struct TerminalProfile
{
    terminal::Process::ExecInfo shell;
    bool maximized = false;
    bool fullscreen = false;
    bool show_title_bar = true;
    double refreshRate = 0.0; // 0=auto
    terminal::LineOffset copyLastMarkRangeOffset = terminal::LineOffset(0);

    std::string wmClass;

    terminal::PageSize terminalSize = { terminal::LineCount(10), terminal::ColumnCount(40) };
    terminal::VTType terminalId = terminal::VTType::VT525;

    terminal::MaxHistoryLineCount maxHistoryLineCount;
    terminal::LineCount historyScrollMultiplier = terminal::LineCount(3);
    ScrollBarPosition scrollbarPosition = ScrollBarPosition::Right;
    bool hideScrollbarInAltScreen = true;

    bool autoScrollOnUpdate;

    terminal::rasterizer::FontDescriptions fonts;

    struct
    {
        Permission captureBuffer = Permission::Ask;
        Permission changeFont = Permission::Ask;
    } permissions;

    bool drawBoldTextWithBrightColors = false;
    terminal::ColorPalette colors {};

    terminal::LineCount modalCursorScrollOff { 8 };

    struct
    {
        InputModeConfig insert;
        InputModeConfig normal;
        InputModeConfig visual;
    } inputModes;
    std::chrono::milliseconds highlightTimeout { 300 };
    bool highlightDoubleClickedWord = true;
    terminal::StatusDisplayType initialStatusDisplayType = terminal::StatusDisplayType::None;

    terminal::Opacity backgroundOpacity; // value between 0 (fully transparent) and 0xFF (fully visible).
    bool backgroundBlur;                 // On Windows 10, this will enable Acrylic Backdrop.

    std::optional<display::ShaderConfig> backgroundShader;
    std::optional<display::ShaderConfig> backgroundImageShader;
    std::optional<display::ShaderConfig> textShader;

    struct
    {
        terminal::rasterizer::Decorator normal = terminal::rasterizer::Decorator::DottedUnderline;
        terminal::rasterizer::Decorator hover = terminal::rasterizer::Decorator::Underline;
    } hyperlinkDecoration;
};

enum class RenderingBackend
{
    Default,
    Software,
    OpenGL,
};

// NB: All strings in here must be UTF8-encoded.
struct Config
{
    FileSystem::path backingFilePath;

    bool live = false;

    /// Qt platform plugin to be loaded.
    /// This is equivalent to QT_QPA_PLATFORM.
    std::string platformPlugin;

    RenderingBackend renderingBackend = RenderingBackend::Default;

    /// Enables/disables support for direct mapped texture atlas tiles (e.g. glyphs).
    bool textureAtlasDirectMapping = true;

    // Number of hashtable slots to map to the texture tiles.
    // Larger values may increase performance, but too large may also decrease.
    // This value is rounted up to a value equal to the power of two.
    //
    // Default: 4096
    crispy::StrongHashtableSize textureAtlasHashtableSlots = crispy::StrongHashtableSize { 4096 };

    /// Number of tiles that must fit at lest into the texture atlas,
    /// excluding US-ASCII glyphs, cursor shapes and decorations.
    ///
    /// Value must be at least as large as grid cells available in the current view.
    /// This value is automatically adjusted if too small.
    crispy::LRUCapacity textureAtlasTileCount = crispy::LRUCapacity { 4000 };

    // Configures the size of the PTY read buffer.
    // Changing this value may result in better or worse throughput performance.
    //
    // This value must be integer-devisable by 16.
    size_t ptyReadBufferSize = 16384;

    // Size in bytes per PTY Buffer Object.
    //
    // Defaults to 1 MB, that's roughly 10k lines when column count is 100.
    size_t ptyBufferObjectSize = 1024u * 1024u;

    bool reflowOnResize = true;

    std::unordered_map<std::string, terminal::ColorPalette> colorschemes;
    std::unordered_map<std::string, TerminalProfile> profiles;
    std::string defaultProfileName;

    TerminalProfile* profile(std::string const& _name)
    {
        assert(_name != "");
        if (auto i = profiles.find(_name); i != profiles.end())
            return &i->second;
        assert(false && "Profile not found.");
        return nullptr;
    }

    TerminalProfile const* profile(std::string const& _name) const
    {
        if (auto i = profiles.find(_name); i != profiles.end())
            return &i->second;
        assert(false && "Profile not found.");
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

    bool spawnNewProcess = false;
    std::shared_ptr<logstore::Sink> loggingSink;

    bool sixelScrolling = true;
    terminal::ImageSize maxImageSize = {}; // default to runtime system screen size.
    unsigned maxImageColorRegisters = 4096;

    std::set<std::string> experimentalFeatures;
};

FileSystem::path configHome(std::string const& _programName);

std::optional<std::string> readConfigFile(std::string const& _filename);

void loadConfigFromFile(Config& _config, FileSystem::path const& _fileName);
Config loadConfigFromFile(FileSystem::path const& _fileName);
Config loadConfig();

std::string defaultConfigString();
std::error_code createDefaultConfig(FileSystem::path const& _path);
std::string defaultConfigFilePath();

} // namespace contour::config

namespace fmt // {{{
{
template <>
struct formatter<contour::config::Permission>
{
    template <typename ParseContext>
    constexpr auto parse(ParseContext& ctx)
    {
        return ctx.begin();
    }
    template <typename FormatContext>
    auto format(contour::config::Permission const& _perm, FormatContext& ctx)
    {
        switch (_perm)
        {
            case contour::config::Permission::Allow: return fmt::format_to(ctx.out(), "allow");
            case contour::config::Permission::Deny: return fmt::format_to(ctx.out(), "deny");
            case contour::config::Permission::Ask: return fmt::format_to(ctx.out(), "ask");
        }
        return fmt::format_to(ctx.out(), "({})", unsigned(_perm));
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
            case SelectionAction::CopyToClipboard: return fmt::format_to(ctx.out(), "CopyToClipboard");
            case SelectionAction::CopyToSelectionClipboard:
                return fmt::format_to(ctx.out(), "CopyToSelectionClipboard");
            case SelectionAction::Nothing: return fmt::format_to(ctx.out(), "Waiting");
        }
        return fmt::format_to(ctx.out(), "{}", static_cast<unsigned>(_value));
    }
};

template <>
struct formatter<contour::config::ScrollBarPosition>
{
    template <typename ParseContext>
    constexpr auto parse(ParseContext& ctx)
    {
        return ctx.begin();
    }
    using ScrollBarPosition = contour::config::ScrollBarPosition;
    template <typename FormatContext>
    auto format(ScrollBarPosition _value, FormatContext& ctx)
    {
        switch (_value)
        {
            case ScrollBarPosition::Hidden: return fmt::format_to(ctx.out(), "Hidden");
            case ScrollBarPosition::Left: return fmt::format_to(ctx.out(), "Left");
            case ScrollBarPosition::Right: return fmt::format_to(ctx.out(), "Right");
        }
        return fmt::format_to(ctx.out(), "{}", static_cast<unsigned>(_value));
    }
};

} // namespace fmt
