// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <contour/Actions.h>
#include <contour/display/ShaderConfig.h>

#include <vtbackend/Color.h>
#include <vtbackend/ColorPalette.h>
#include <vtbackend/InputBinding.h>
#include <vtbackend/Settings.h>
#include <vtbackend/primitives.h> // CursorDisplay

#include <vtpty/Process.h>
#include <vtpty/SshSession.h>

#include <vtrasterizer/Decorator.h>
#include <vtrasterizer/FontDescriptions.h>

#include <text_shaper/font.h>
#include <text_shaper/mock_font_locator.h>

#include <crispy/StrongLRUHashtable.h>
#include <crispy/assert.h>
#include <crispy/size.h>

#include <chrono>
#include <filesystem>
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
using KeyInputMapping = vtbackend::InputBinding<vtbackend::Key, ActionList>;
using CharInputMapping = vtbackend::InputBinding<char32_t, ActionList>;
using MouseInputMapping = vtbackend::InputBinding<vtbackend::MouseButton, ActionList>;

struct InputMappings
{
    std::vector<KeyInputMapping> keyMappings;
    std::vector<CharInputMapping> charMappings;
    std::vector<MouseInputMapping> mouseMappings;
};

namespace helper
{
    inline bool testMatchMode(uint8_t actualModeFlags,
                              vtbackend::MatchModes expected,
                              vtbackend::MatchModes::Flag testFlag)
    {
        using MatchModes = vtbackend::MatchModes;
        switch (expected.status(testFlag))
        {
            case MatchModes::Status::Enabled:
                if (!(actualModeFlags & testFlag))
                    return false;
                break;
            case MatchModes::Status::Disabled:
                if ((actualModeFlags & testFlag))
                    return false;
                break;
            case MatchModes::Status::Any: break;
        }
        return true;
    }

    inline bool testMatchMode(uint8_t actualModeFlags, vtbackend::MatchModes expected)
    {
        using Flag = vtbackend::MatchModes::Flag;
        return testMatchMode(actualModeFlags, expected, Flag::AlternateScreen)
               && testMatchMode(actualModeFlags, expected, Flag::AppCursor)
               && testMatchMode(actualModeFlags, expected, Flag::AppKeypad)
               && testMatchMode(actualModeFlags, expected, Flag::Select)
               && testMatchMode(actualModeFlags, expected, Flag::Insert)
               && testMatchMode(actualModeFlags, expected, Flag::Search)
               && testMatchMode(actualModeFlags, expected, Flag::Trace);
    }
} // namespace helper

template <typename Input>
std::vector<actions::Action> const* apply(
    std::vector<vtbackend::InputBinding<Input, ActionList>> const& mappings,
    Input input,
    vtbackend::Modifiers modifiers,
    uint8_t actualModeFlags)
{
    for (vtbackend::InputBinding<Input, ActionList> const& mapping: mappings)
    {
        if (mapping.modifiers == modifiers && mapping.input == input
            && helper::testMatchMode(actualModeFlags, mapping.modes))
        {
            return &mapping.binding;
        }
    }
    return nullptr;
}

struct CursorConfig
{
    vtbackend::CursorShape cursorShape { vtbackend::CursorShape::Block };
    vtbackend::CursorDisplay cursorDisplay { vtbackend::CursorDisplay::Steady };
    std::chrono::milliseconds cursorBlinkInterval;
};

struct InputModeConfig
{
    CursorConfig cursor;
};

struct DualColorConfig
{
    vtbackend::ColorPalette darkMode {};
    vtbackend::ColorPalette lightMode {};
};

struct SimpleColorConfig
{
    vtbackend::ColorPalette colors {};
};

using ColorConfig = std::variant<SimpleColorConfig, DualColorConfig>;

struct TerminalProfile
{
    vtpty::Process::ExecInfo shell;
    vtpty::SshHostConfig ssh;

    bool maximized = false;
    bool fullscreen = false;
    bool showTitleBar = true;
    bool sizeIndicatorOnResize = true;
    bool mouseHideWhileTyping = true;
    vtbackend::RefreshRate refreshRate = { 0.0 }; // 0=auto
    vtbackend::LineOffset copyLastMarkRangeOffset = vtbackend::LineOffset(0);

    std::string wmClass;

    vtbackend::PageSize terminalSize = { vtbackend::LineCount(10), vtbackend::ColumnCount(40) };
    vtbackend::VTType terminalId = vtbackend::VTType::VT525;

    vtbackend::MaxHistoryLineCount maxHistoryLineCount;
    vtbackend::LineCount historyScrollMultiplier = vtbackend::LineCount(3);
    ScrollBarPosition scrollbarPosition = ScrollBarPosition::Right;
    vtbackend::StatusDisplayPosition statusDisplayPosition = vtbackend::StatusDisplayPosition::Bottom;
    bool syncWindowTitleWithHostWritableStatusDisplay = false;
    bool hideScrollbarInAltScreen = true;
    bool optionKeyAsAlt = false;

    bool autoScrollOnUpdate;

    vtrasterizer::FontDescriptions fonts;

    struct
    {
        Permission captureBuffer = Permission::Ask;
        Permission changeFont = Permission::Ask;
        Permission displayHostWritableStatusLine = Permission::Ask;
    } permissions;

    bool drawBoldTextWithBrightColors = false;
    ColorConfig colors = SimpleColorConfig {};

    vtbackend::LineCount modalCursorScrollOff { 8 };

    struct
    {
        InputModeConfig insert;
        InputModeConfig normal;
        InputModeConfig visual;
    } inputModes;
    std::chrono::milliseconds smoothLineScrolling { 100 };
    std::chrono::milliseconds highlightTimeout { 300 };
    bool highlightDoubleClickedWord = true;
    vtbackend::StatusDisplayType initialStatusDisplayType = vtbackend::StatusDisplayType::None;

    vtbackend::Opacity backgroundOpacity; // value between 0 (fully transparent) and 0xFF (fully visible).
    bool backgroundBlur;                  // On Windows 10, this will enable Acrylic Backdrop.

    std::optional<display::ShaderConfig> backgroundShader;
    std::optional<display::ShaderConfig> textShader;

    struct
    {
        vtrasterizer::Decorator normal = vtrasterizer::Decorator::DottedUnderline;
        vtrasterizer::Decorator hover = vtrasterizer::Decorator::Underline;
    } hyperlinkDecoration;

    struct
    {
        std::string sound = "default";
        bool alert = true;
        float volume = 1.0f;
    } bell;

    // Set of DEC modes that are frozen and cannot be changed by the application.
    std::map<vtbackend::DECMode, bool> frozenModes;
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
    std::filesystem::path backingFilePath;

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
    crispy::strong_hashtable_size textureAtlasHashtableSlots = crispy::strong_hashtable_size { 4096 };

    /// Number of tiles that must fit at lest into the texture atlas,
    /// excluding US-ASCII glyphs, cursor shapes and decorations.
    ///
    /// Value must be at least as large as grid cells available in the current view.
    /// This value is automatically adjusted if too small.
    crispy::lru_capacity textureAtlasTileCount = crispy::lru_capacity { 4000 };

    // Configures the size of the PTY read buffer.
    // Changing this value may result in better or worse throughput performance.
    //
    // This value must be integer-devisable by 16.
    size_t ptyReadBufferSize = 16384;

    // Size in bytes per PTY Buffer Object.
    //
    // Defaults to 1 MB, that's roughly 10k lines when column count is 100.
    size_t ptyBufferObjectSize = 1024lu * 1024lu;

    bool reflowOnResize = true;

    std::unordered_map<std::string, vtbackend::ColorPalette> colorschemes;
    std::unordered_map<std::string, TerminalProfile> profiles;
    std::string defaultProfileName;

    TerminalProfile* profile(std::string const& name)
    {
        assert(!name.empty());
        if (auto i = profiles.find(name); i != profiles.end())
            return &i->second;
        assert(false && "Profile not found.");
        return nullptr;
    }

    TerminalProfile const* profile(std::string const& name) const
    {
        assert(!name.empty());
        if (auto i = profiles.find(name); i != profiles.end())
            return &i->second;
        assert(false && "Profile not found.");
        return nullptr;
    }

    TerminalProfile& profile() noexcept
    {
        if (auto* prof = profile(defaultProfileName); prof)
            return *prof;
        crispy::unreachable();
    }
    TerminalProfile const& profile() const noexcept
    {
        if (auto const* prof = profile(defaultProfileName); prof)
            return *prof;
        crispy::unreachable();
    }

    // selection
    std::string wordDelimiters;
    vtbackend::Modifiers bypassMouseProtocolModifiers = vtbackend::Modifier::Shift;
    SelectionAction onMouseSelection = SelectionAction::CopyToSelectionClipboard;
    vtbackend::Modifiers mouseBlockSelectionModifiers = vtbackend::Modifier::Control;

    // input mapping
    InputMappings inputMappings;

    bool spawnNewProcess = false;
    std::shared_ptr<logstore::sink> loggingSink;

    bool sixelScrolling = true;
    vtbackend::ImageSize maxImageSize = {}; // default to runtime system screen size.
    unsigned maxImageColorRegisters = 4096;

    std::set<std::string> experimentalFeatures;
};

std::filesystem::path configHome();
std::filesystem::path configHome(std::string const& programName);

std::optional<std::string> readConfigFile(std::string const& filename);

void loadConfigFromFile(Config& config, std::filesystem::path const& fileName);
Config loadConfigFromFile(std::filesystem::path const& fileName);
Config loadConfig();

std::string defaultConfigString();
std::error_code createDefaultConfig(std::filesystem::path const& path);
std::string defaultConfigFilePath();

} // namespace contour::config

// {{{ fmtlib custom formatter support
template <>
struct fmt::formatter<contour::config::Permission>
{
    template <typename ParseContext>
    constexpr auto parse(ParseContext& ctx)
    {
        return ctx.begin();
    }
    template <typename FormatContext>
    auto format(contour::config::Permission const& perm, FormatContext& ctx)
    {
        switch (perm)
        {
            case contour::config::Permission::Allow: return fmt::format_to(ctx.out(), "allow");
            case contour::config::Permission::Deny: return fmt::format_to(ctx.out(), "deny");
            case contour::config::Permission::Ask: return fmt::format_to(ctx.out(), "ask");
        }
        return fmt::format_to(ctx.out(), "({})", unsigned(perm));
    }
};

template <>
struct fmt::formatter<contour::config::SelectionAction>
{
    template <typename ParseContext>
    constexpr auto parse(ParseContext& ctx)
    {
        return ctx.begin();
    }
    using SelectionAction = contour::config::SelectionAction;
    template <typename FormatContext>
    auto format(SelectionAction value, FormatContext& ctx)
    {
        switch (value)
        {
            case SelectionAction::CopyToClipboard: return fmt::format_to(ctx.out(), "CopyToClipboard");
            case SelectionAction::CopyToSelectionClipboard:
                return fmt::format_to(ctx.out(), "CopyToSelectionClipboard");
            case SelectionAction::Nothing: return fmt::format_to(ctx.out(), "Waiting");
        }
        return fmt::format_to(ctx.out(), "{}", static_cast<unsigned>(value));
    }
};

template <>
struct fmt::formatter<contour::config::ScrollBarPosition>
{
    template <typename ParseContext>
    constexpr auto parse(ParseContext& ctx)
    {
        return ctx.begin();
    }
    using ScrollBarPosition = contour::config::ScrollBarPosition;
    template <typename FormatContext>
    auto format(ScrollBarPosition value, FormatContext& ctx)
    {
        switch (value)
        {
            case ScrollBarPosition::Hidden: return fmt::format_to(ctx.out(), "Hidden");
            case ScrollBarPosition::Left: return fmt::format_to(ctx.out(), "Left");
            case ScrollBarPosition::Right: return fmt::format_to(ctx.out(), "Right");
        }
        return fmt::format_to(ctx.out(), "{}", static_cast<unsigned>(value));
    }
};
// }}}
