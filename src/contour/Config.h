// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <contour/Actions.h>
#include <contour/ConfigDocumentation.h>
#include <contour/display/ShaderConfig.h>

#include <vtbackend/Color.h>
#include <vtbackend/ColorPalette.h>
#include <vtbackend/ControlCode.h>
#include <vtbackend/InputBinding.h>
#include <vtbackend/InputGenerator.h>
#include <vtbackend/MatchModes.h>
#include <vtbackend/Settings.h>
#include <vtbackend/VTType.h>
#include <vtbackend/primitives.h> // CursorDisplay

#include <vtpty/ImageSize.h>
#include <vtpty/PageSize.h>
#include <vtpty/Process.h>
#include <vtpty/SshSession.h>

#include <vtrasterizer/Decorator.h>
#include <vtrasterizer/FontDescriptions.h>

#include <text_shaper/font.h>
#include <text_shaper/mock_font_locator.h>

#include <crispy/StrongLRUHashtable.h>
#include <crispy/assert.h>
#include <crispy/flags.h>
#include <crispy/logstore.h>
#include <crispy/size.h>
#include <crispy/utils.h>

#include <fmt/core.h>

#include <yaml-cpp/emitter.h>
#include <yaml-cpp/node/detail/iterator_fwd.h>
#include <yaml-cpp/ostream_wrapper.h>
#include <yaml-cpp/yaml.h>

#include <range/v3/range/conversion.hpp>
#include <range/v3/view/concat.hpp>
#include <range/v3/view/join.hpp>
#include <range/v3/view/transform.hpp>

#include <chrono>
#include <cstdint>
#include <exception>
#include <filesystem>
#include <limits>
#include <memory>
#include <optional>
#include <regex>
#include <set>
#include <string>
#include <system_error>
#include <type_traits>
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
    std::string colorSchemeLight = "default";
    std::string colorSchemeDark = "default";
    vtbackend::ColorPalette darkMode {};
    vtbackend::ColorPalette lightMode {};
};

struct SimpleColorConfig
{
    std::string colorScheme = "default";
    vtbackend::ColorPalette colors {};
};

using ColorConfig = std::variant<SimpleColorConfig, DualColorConfig>;

enum class RenderingBackend
{
    Default,
    Software,
    OpenGL,
};

struct HorizontalMarginTag
{
};
struct VerticalMarginTag
{
};

using HorizontalMargin = boxed::boxed<unsigned, HorizontalMarginTag>;
using VerticalMargin = boxed::boxed<unsigned, VerticalMarginTag>;

struct WindowMargins
{
    HorizontalMargin horizontal { 0 };
    VerticalMargin vertical { 0 };
};

template <typename T, documentation::StringLiteral doc>
struct ConfigEntry
{
    using value_type = T;

    std::string documentation = doc.value;
    constexpr ConfigEntry(): _value {} {}
    //clang-format off
    constexpr explicit ConfigEntry(T&& in): _value { std::forward<T>(in) } {}

    template <typename F>
    constexpr explicit ConfigEntry(F&& in): _value { std::forward<F>(in) }
    {
    }
    //clang-format on

    [[nodiscard]] constexpr T const& value() const { return _value; }
    [[nodiscard]] constexpr T& value() { return _value; }

    constexpr ConfigEntry& operator=(T const& value)
    {
        _value = value;
        return *this;
    }

    constexpr ConfigEntry& operator=(T&& value) noexcept
    {
        _value = std::move(value);
        return *this;
    }

    constexpr ConfigEntry(ConfigEntry const&) = default;
    constexpr ConfigEntry& operator=(ConfigEntry const&) = default;
    constexpr ConfigEntry(ConfigEntry&&) noexcept = default;
    constexpr ConfigEntry& operator=(ConfigEntry&&) noexcept = default;
    ~ConfigEntry() = default;

  private:
    T _value;
};

template <typename T>
concept ConfigEntryConcept = requires(T t) {
    t.makeDocumentation();
    t._value;
};

struct Bell
{
    std::string sound = "default";
    bool alert = true;
    float volume = 1.0f;
};

const inline vtrasterizer::FontDescriptions defaultFont = vtrasterizer::FontDescriptions {
    .dpiScale = 1.0,
    .dpi = { 0, 0 },
    .size = { 12 },
    .regular = text::font_description { .familyName = { "monospace" },
                                        .weight = text::font_weight::normal,
                                        .slant = text::font_slant::normal,
                                        .spacing = text::font_spacing::proportional,
                                        .strictSpacing = false,
                                        .features = {} },
    .bold = text::font_description { .familyName = { "monospace" },
                                     .weight = text::font_weight::bold,
                                     .slant = text::font_slant::normal,
                                     .spacing = text::font_spacing::proportional,
                                     .strictSpacing = false,
                                     .features = {} },
    .italic = text::font_description { .familyName = { "monospace" },
                                       .weight = text::font_weight::normal,
                                       .slant = text::font_slant::italic,
                                       .spacing = text::font_spacing::proportional,
                                       .strictSpacing = false,
                                       .features = {} },
    .boldItalic = text::font_description { .familyName = { "monospace" },
                                           .weight = text::font_weight::bold,
                                           .slant = text::font_slant::italic,
                                           .spacing = text::font_spacing::proportional,
                                           .strictSpacing = false,
                                           .features = {} },
    .emoji = text::font_description { .familyName = { "emoji" } },
    .renderMode = text::render_mode::gray,
    .textShapingEngine = vtrasterizer::TextShapingEngine::OpenShaper,
    .fontLocator = vtrasterizer::FontLocatorEngine::FontConfig,
    .builtinBoxDrawing = true,
};

struct TerminalProfile
{
    ConfigEntry<vtpty::Process::ExecInfo, documentation::Shell> shell { {
        .program =
            []() {
                auto const program = vtpty::Process::loginShell(true);
                return program                    // NB: sadly can't be a temporary
                       | ranges::views::join(' ') //
                       | ranges::to<std::string>();
            }(),
        .arguments = {},
        .workingDirectory = "",
        .env = {},
    } };
    ConfigEntry<vtpty::SshHostConfig, documentation::SshHostConfig> ssh {};
    ConfigEntry<bool, documentation::Maximized> maximized { false };
    ConfigEntry<bool, documentation::Fullscreen> fullscreen { false };
    ConfigEntry<bool, documentation::ShowTitleBar> showTitleBar { true };
    ConfigEntry<bool, documentation::ShowIndicatorOnResize> sizeIndicatorOnResize { true };
    ConfigEntry<bool, documentation::MouseHideWhileTyping> mouseHideWhileTyping { true };
    ConfigEntry<vtbackend::LineOffset, documentation::CopyLastMarkRangeOffset> copyLastMarkRangeOffset { 0 };
    ConfigEntry<std::string, documentation::WMClass> wmClass { "contour" };
    ConfigEntry<WindowMargins, documentation::Margins> margins { { HorizontalMargin { 0u },
                                                                   VerticalMargin { 0u } } };
    ConfigEntry<vtbackend::PageSize, documentation::TerminalSize> terminalSize { {
        vtbackend::LineCount(25),
        vtbackend::ColumnCount(80),
    } };
    ConfigEntry<vtbackend::VTType, documentation::TerminalId> terminalId { vtbackend::VTType::VT525 };
    ConfigEntry<vtbackend::MaxHistoryLineCount, documentation::MaxHistoryLineCount> maxHistoryLineCount {
        vtbackend::LineCount(1000)
    };
    ConfigEntry<vtbackend::LineCount, documentation::HistoryScrollMultiplier> historyScrollMultiplier {
        vtbackend::LineCount(3)
    };
    ConfigEntry<ScrollBarPosition, documentation::ScrollbarPosition> scrollbarPosition {
        ScrollBarPosition::Right
    };
    ConfigEntry<vtbackend::StatusDisplayPosition, documentation::StatusDisplayPosition>
        statusDisplayPosition { vtbackend::StatusDisplayPosition::Bottom };
    ConfigEntry<std::string, documentation::IndicatorStatusLineLeft> indicatorStatusLineLeft {
        " {InputMode:Bold,Color=#FFFF00}"
        "{SearchPrompt:Left= │ }"
        "{TraceMode:Bold,Color=#FFFF00,Left= │ }"
        "{ProtectedMode:Bold,Left= │ }"
    };
    ConfigEntry<std::string, documentation::IndicatorStatusLineMiddle> indicatorStatusLineMiddle {
        "{Title:Left= « ,Right= » }"
    };
    ConfigEntry<std::string, documentation::IndicatorStatusLineRight> indicatorStatusLineRight {
        "{HistoryLineCount:Faint,Color=#c0c0c0} │ {Clock:Bold}"
    };
    ConfigEntry<bool, documentation::SyncWindowTitleWithHostWritableStatusDisplay>
        syncWindowTitleWithHostWritableStatusDisplay { false };
    ConfigEntry<bool, documentation::HideScrollbarInAltScreen> hideScrollbarInAltScreen { true };
    ConfigEntry<bool, documentation::Dummy> optionKeyAsAlt { false };
    ConfigEntry<bool, documentation::AutoScrollOnUpdate> autoScrollOnUpdate { true };
    ConfigEntry<vtrasterizer::FontDescriptions, documentation::Fonts> fonts { defaultFont };
    ConfigEntry<Permission, documentation::CaptureBuffer> captureBuffer { Permission::Ask };
    ConfigEntry<Permission, documentation::ChangeFont> changeFont { Permission::Ask };
    ConfigEntry<Permission, documentation::DisplayHostWritableStatusLine> displayHostWritableStatusLine {
        Permission::Ask
    };
    ConfigEntry<bool, documentation::DrawBoldTextWithBrightColors> drawBoldTextWithBrightColors { false };
    ConfigEntry<ColorConfig, documentation::Colors> colors { SimpleColorConfig {} };
    ConfigEntry<vtbackend::LineCount, documentation::ModalCursorScrollOff> modalCursorScrollOff {
        vtbackend::LineCount { 8 }
    };
    ConfigEntry<InputModeConfig, documentation::ModeInsert> modeInsert { CursorConfig {
        vtbackend::CursorShape::Bar, vtbackend::CursorDisplay::Steady, std::chrono::milliseconds { 500 } } };
    ConfigEntry<InputModeConfig, documentation::ModeNormal> modeNormal {
        CursorConfig { vtbackend::CursorShape::Block,
                       vtbackend::CursorDisplay::Steady,
                       std::chrono::milliseconds { 500 } },
    };
    ConfigEntry<InputModeConfig, documentation::ModeVisual> modeVisual {
        CursorConfig { vtbackend::CursorShape::Block,
                       vtbackend::CursorDisplay::Steady,
                       std::chrono::milliseconds { 500 } },
    };
    ConfigEntry<std::chrono::milliseconds, documentation::SmoothLineScrolling> smoothLineScrolling { 100 };
    ConfigEntry<std::chrono::milliseconds, documentation::HighlightTimeout> highlightTimeout { 100 };
    ConfigEntry<bool, documentation::HighlightDoubleClickerWord> highlightDoubleClickedWord { true };
    ConfigEntry<vtbackend::StatusDisplayType, documentation::InitialStatusLine> initialStatusDisplayType {
        vtbackend::StatusDisplayType::Indicator
    };
    ConfigEntry<vtbackend::Opacity, documentation::BackgroundOpacity> backgroundOpacity { vtbackend::Opacity(
        0xFF) };
    ConfigEntry<bool, documentation::BackgroundBlur> backgroundBlur { false };
    ConfigEntry<vtrasterizer::Decorator, "normal: {}\n"> hyperlinkDecorationNormal {
        vtrasterizer::Decorator::DottedUnderline
    };
    ConfigEntry<vtrasterizer::Decorator, "hover: {}\n"> hyperlinkDecorationHover {
        vtrasterizer::Decorator::Underline
    };
    ConfigEntry<Bell, documentation::Bell> bell { { .sound = "default", .alert = true, .volume = 1.0f } };
    ConfigEntry<std::map<vtbackend::DECMode, bool>, documentation::FrozenDecMode> frozenModes {};
};

const InputMappings defaultInputMappings {
    .keyMappings {
        KeyInputMapping { .modes { vtbackend::MatchModes {} },
                          .modifiers { vtbackend::Modifiers { vtbackend::Modifier::Alt } },
                          .input = vtbackend::Key::Enter,
                          .binding = { { actions::ToggleFullscreen {} } } },
        KeyInputMapping { .modes {},
                          .modifiers { vtbackend::Modifiers {} },
                          .input = vtbackend::Key::Escape,
                          .binding = { { actions::CancelSelection {} } } },
        KeyInputMapping { .modes { vtbackend::MatchModes {} },
                          .modifiers { vtbackend::Modifiers { vtbackend::Modifier::Shift } },
                          .input = vtbackend::Key::DownArrow,
                          .binding = { { actions::ScrollOneDown {} } } },
        KeyInputMapping { .modes { vtbackend::MatchModes {} },
                          .modifiers { vtbackend::Modifiers { vtbackend::Modifier::Shift } },
                          .input = vtbackend::Key::End,
                          .binding = { { actions::ScrollToBottom {} } } },
        KeyInputMapping { .modes { vtbackend::MatchModes {} },
                          .modifiers { vtbackend::Modifiers { vtbackend::Modifier::Shift } },
                          .input = vtbackend::Key::Home,
                          .binding = { { actions::ScrollToTop {} } } },
        KeyInputMapping { .modes { vtbackend::MatchModes {} },
                          .modifiers { vtbackend::Modifiers { vtbackend::Modifier::Shift } },
                          .input = vtbackend::Key::PageDown,
                          .binding = { { actions::ScrollPageDown {} } } },
        KeyInputMapping { .modes { vtbackend::MatchModes {} },
                          .modifiers { vtbackend::Modifiers { vtbackend::Modifier::Shift } },
                          .input = vtbackend::Key::PageUp,
                          .binding = { { actions::ScrollPageUp {} } } },
        KeyInputMapping { .modes { vtbackend::MatchModes {} },
                          .modifiers { vtbackend::Modifiers { vtbackend::Modifier::Shift } },
                          .input = vtbackend::Key::UpArrow,
                          .binding = { { actions::ScrollOneUp {} } } },
        KeyInputMapping { .modes { vtbackend::MatchModes {} },
                          .modifiers { vtbackend::Modifiers {} },
                          .input = vtbackend::Key::F3,
                          .binding = { { actions::FocusNextSearchMatch {} } } },
        KeyInputMapping { .modes { vtbackend::MatchModes {} },
                          .modifiers { vtbackend::Modifiers { vtbackend::Modifier::Shift } },
                          .input = vtbackend::Key::F3,
                          .binding = { { actions::FocusPreviousSearchMatch {} } } },
        //     KeyInputMapping { .modes { vtbackend::MatchModes {} },
        //                       .modifiers { vtbackend::Modifiers {  vtbackend::Modifier {
        //                       vtbackend::Modifier::Shift }
        //                                    | vtbackend::Modifiers { vtbackend::Modifier::Control }
        //                                    }
        //                                    },
        //                       .input { vtbackend::Key::Plus },
        //                       .binding = { { actions::IncreaseFontSize {} } } },
    },
    .charMappings {
        CharInputMapping {
            .modes { vtbackend::MatchModes {} },
            .modifiers { vtbackend::Modifiers { vtbackend::Modifiers { vtbackend::Modifier::Shift }
                                                | vtbackend::Modifiers { vtbackend::Modifier::Control } } },
            .input = '-',
            .binding = { { actions::DecreaseFontSize {} } } },
        CharInputMapping {
            .modes { vtbackend::MatchModes {} },
            .modifiers { vtbackend::Modifiers { vtbackend::Modifiers { vtbackend::Modifier::Shift }
                                                | vtbackend::Modifiers { vtbackend::Modifier::Control } } },
            .input = '_',
            .binding = { { actions::DecreaseFontSize {} } } },
        CharInputMapping {
            .modes { vtbackend::MatchModes {} },
            .modifiers { vtbackend::Modifiers { vtbackend::Modifiers { vtbackend::Modifier::Shift }
                                                | vtbackend::Modifiers { vtbackend::Modifier::Control } } },
            .input = 'N',
            .binding = { actions::NewTerminal {} } },
        CharInputMapping {
            .modes { vtbackend::MatchModes {} },
            .modifiers { vtbackend::Modifiers { vtbackend::Modifiers { vtbackend::Modifier::Alt }
                                                | vtbackend::Modifiers { vtbackend::Modifier::Control } } },
            .input = 'V',
            .binding = { { actions::PasteClipboard { .strip = true } } } },
        CharInputMapping {
            .modes { vtbackend::MatchModes {} },
            .modifiers { vtbackend::Modifiers { vtbackend::Modifiers { vtbackend::Modifier::Shift }
                                                | vtbackend::Modifiers { vtbackend::Modifier::Control } } },
            .input = 'V',
            .binding = { { actions::PasteClipboard { .strip = false } } } },
        CharInputMapping {
            .modes { vtbackend::MatchModes {} },
            .modifiers { vtbackend::Modifiers { vtbackend::Modifiers { vtbackend::Modifier::Shift }
                                                | vtbackend::Modifiers { vtbackend::Modifier::Control } } },
            .input = 'V',
            .binding = { { actions::PasteClipboard { .strip = false } } } },
        CharInputMapping { .modes { vtbackend::MatchModes {} },
                           .modifiers { vtbackend::Modifiers { vtbackend::Modifier::Alt }
                                        | vtbackend::Modifiers { vtbackend::Modifier::Control } },
                           .input = 'S',
                           .binding = { { actions::ScreenshotVT {} } } },
        CharInputMapping { .modes { vtbackend::MatchModes {} },
                           .modifiers { vtbackend::Modifiers { vtbackend::Modifier::Control } },
                           .input = '0',
                           .binding = { { actions::ResetFontSize {} } } },
        CharInputMapping { .modes = []() -> vtbackend::MatchModes {
                              auto mods = vtbackend::MatchModes();
                              mods.enable(vtbackend::MatchModes::Select);
                              mods.enable(vtbackend::MatchModes::Insert);
                              return mods;
                          }(),
                           .modifiers { vtbackend::Modifiers { vtbackend::Modifier::Control } },
                           .input = 'C',
                           .binding = { { actions::CopySelection {} } } },
        CharInputMapping { .modes = []() -> vtbackend::MatchModes {
                              auto mods = vtbackend::MatchModes();
                              mods.enable(vtbackend::MatchModes::Select);
                              mods.enable(vtbackend::MatchModes::Insert);
                              return mods;
                          }(),
                           .modifiers { vtbackend::Modifiers { vtbackend::Modifier::Control } },
                           .input = 'C',
                           .binding = { { actions::CancelSelection {} } } },
        CharInputMapping { .modes = []() -> vtbackend::MatchModes {
                              auto mods = vtbackend::MatchModes();
                              mods.enable(vtbackend::MatchModes::Select);
                              mods.enable(vtbackend::MatchModes::Insert);
                              return mods;
                          }(),
                           .modifiers { vtbackend::Modifiers { vtbackend::Modifier::Control } },
                           .input = 'V',
                           .binding = { { actions::PasteClipboard { .strip = false } } } },
        CharInputMapping { .modes = []() -> vtbackend::MatchModes {
                              auto mods = vtbackend::MatchModes();
                              mods.enable(vtbackend::MatchModes::Select);
                              mods.enable(vtbackend::MatchModes::Insert);
                              return mods;
                          }(),
                           .modifiers { vtbackend::Modifiers { vtbackend::Modifier::Control } },
                           .input = 'V',
                           .binding = { { actions::CancelSelection {} } } },
        CharInputMapping { .modes = []() -> vtbackend::MatchModes {
                              auto mods = vtbackend::MatchModes();
                              mods.enable(vtbackend::MatchModes::Insert);
                              return mods;
                          }(),
                           .modifiers { vtbackend::Modifiers { vtbackend::Modifier::Shift }
                                        | vtbackend::Modifiers { vtbackend::Modifier::Control } },
                           .input = ',',
                           .binding = { { actions::OpenConfiguration {} } } },
        CharInputMapping { .modes = []() -> vtbackend::MatchModes {
                              auto mods = vtbackend::MatchModes();
                              mods.enable(vtbackend::MatchModes::Insert);
                              return mods;
                          }(),
                           .modifiers { vtbackend::Modifiers { vtbackend::Modifier::Shift }
                                        | vtbackend::Modifiers { vtbackend::Modifier::Control } },
                           .input = ' ', // SPACE
                           .binding = { { actions::ViNormalMode {} } } },
        CharInputMapping { .modes { vtbackend::MatchModes {} },
                           .modifiers { vtbackend::Modifiers { vtbackend::Modifier::Shift }
                                        | vtbackend::Modifiers { vtbackend::Modifier::Control } },
                           .input = ',',
                           .binding = { { actions::OpenConfiguration {} } } },
        CharInputMapping { .modes { vtbackend::MatchModes {} },
                           .modifiers { vtbackend::Modifiers { vtbackend::Modifier::Shift }
                                        | vtbackend::Modifiers { vtbackend::Modifier::Control } },
                           .input = 'Q',
                           .binding = { { actions::Quit {} } } },
        CharInputMapping { .modes = []() -> vtbackend::MatchModes {
                              auto mods = vtbackend::MatchModes();
                              mods.disable(vtbackend::MatchModes::AlternateScreen);
                              return mods;
                          }(),
                           .modifiers { vtbackend::Modifiers { vtbackend::Modifier::Alt }
                                        | vtbackend::Modifiers { vtbackend::Modifier::Control } },
                           .input = 'K',
                           .binding = { { actions::ScrollMarkUp {} } } },
        CharInputMapping { .modes = []() -> vtbackend::MatchModes {
                              auto mods = vtbackend::MatchModes();
                              mods.disable(vtbackend::MatchModes::AlternateScreen);
                              return mods;
                          }(),
                           .modifiers { vtbackend::Modifiers { vtbackend::Modifier::Alt }
                                        | vtbackend::Modifiers { vtbackend::Modifier::Control } },
                           .input = 'J',
                           .binding = { { actions::ScrollMarkDown {} } } },
        CharInputMapping { .modes { vtbackend::MatchModes {} },
                           .modifiers { vtbackend::Modifiers { vtbackend::Modifier::Alt }
                                        | vtbackend::Modifiers { vtbackend::Modifier::Control } },
                           .input = 'O',
                           .binding = { { actions::OpenFileManager {} } } },
        CharInputMapping { .modes { vtbackend::MatchModes {} },
                           .modifiers { vtbackend::Modifiers { vtbackend::Modifier::Alt }
                                        | vtbackend::Modifiers { vtbackend::Modifier::Control } },
                           .input = '.',
                           .binding = { { actions::ToggleStatusLine {} } } },
        CharInputMapping { .modes { vtbackend::MatchModes {} },
                           .modifiers { vtbackend::Modifiers { vtbackend::Modifier::Shift }
                                        | vtbackend::Modifiers { vtbackend::Modifier::Control } },
                           .input = 'F',
                           .binding = { { actions::SearchReverse {} } } },
        CharInputMapping { .modes { vtbackend::MatchModes {} },
                           .modifiers { vtbackend::Modifiers { vtbackend::Modifier::Shift }
                                        | vtbackend::Modifiers { vtbackend::Modifier::Control } },
                           .input = 'H',
                           .binding = { { actions::NoSearchHighlight {} } } },
    },
    .mouseMappings {
        MouseInputMapping { .modes { vtbackend::MatchModes {} },
                            .modifiers { vtbackend::Modifiers { vtbackend::Modifier::Control } },
                            .input = vtbackend::MouseButton::Left,
                            .binding = { { actions::FollowHyperlink {} } } },
        MouseInputMapping { .modes { vtbackend::MatchModes {} },
                            .modifiers { vtbackend::Modifiers { vtbackend::Modifier::None } },
                            .input = vtbackend::MouseButton::Middle,
                            .binding = { { actions::PasteSelection {} } } },
        MouseInputMapping { .modes { vtbackend::MatchModes {} },
                            .modifiers { vtbackend::Modifiers { vtbackend::Modifier::None } },
                            .input = vtbackend::MouseButton::WheelDown,
                            .binding = { { actions::ScrollDown {} } } },
        MouseInputMapping { .modes { vtbackend::MatchModes {} },
                            .modifiers { vtbackend::Modifiers { vtbackend::Modifier::None } },
                            .input = vtbackend::MouseButton::WheelUp,
                            .binding = { { actions::ScrollUp {} } } },
        MouseInputMapping { .modes { vtbackend::MatchModes {} },
                            .modifiers { vtbackend::Modifiers { vtbackend::Modifier::Alt } },
                            .input = vtbackend::MouseButton::WheelDown,
                            .binding = { { actions::DecreaseOpacity {} } } },
        MouseInputMapping { .modes { vtbackend::MatchModes {} },
                            .modifiers { vtbackend::Modifiers { vtbackend::Modifier::Alt } },
                            .input = vtbackend::MouseButton::WheelUp,
                            .binding = { { actions::IncreaseOpacity {} } } },
        MouseInputMapping { .modes { vtbackend::MatchModes {} },
                            .modifiers { vtbackend::Modifiers { vtbackend::Modifier::Control } },
                            .input = vtbackend::MouseButton::WheelDown,
                            .binding = { { actions::DecreaseFontSize {} } } },
        MouseInputMapping { .modes { vtbackend::MatchModes {} },
                            .modifiers { vtbackend::Modifiers { vtbackend::Modifier::Control } },
                            .input = vtbackend::MouseButton::WheelUp,
                            .binding = { { actions::IncreaseFontSize {} } } },
        MouseInputMapping { .modes { vtbackend::MatchModes {} },
                            .modifiers { vtbackend::Modifiers { vtbackend::Modifier::Shift } },
                            .input = vtbackend::MouseButton::WheelDown,
                            .binding = { { actions::ScrollPageDown {} } } },
        MouseInputMapping { .modes { vtbackend::MatchModes {} },
                            .modifiers { vtbackend::Modifiers { vtbackend::Modifier::Shift } },
                            .input = vtbackend::MouseButton::WheelUp,
                            .binding = { { actions::ScrollPageUp {} } } },
    }
};

struct Config
{
    std::filesystem::path configFile {};
    ConfigEntry<bool, documentation::Live> live { false };
    ConfigEntry<std::string, documentation::PlatformPlugin> platformPlugin { "auto" };
    ConfigEntry<RenderingBackend, documentation::RenderingBackend> renderingBackend {
        RenderingBackend::Default
    };
    ConfigEntry<bool, documentation::TextureAtlasDirectMapping> textureAtlasDirectMapping { false };
    ConfigEntry<crispy::strong_hashtable_size, documentation::TextureAtlasHashtableSlots>
        textureAtlasHashtableSlots { 4096u };
    ConfigEntry<crispy::lru_capacity, documentation::TextureAtlasTileCount> textureAtlasTileCount { 4000u };
    ConfigEntry<int, documentation::PTYReadBufferSize> ptyReadBufferSize { 16384 };
    ConfigEntry<int, documentation::PTYBufferObjectSize> ptyBufferObjectSize { 1024 * 1024 };
    ConfigEntry<bool, documentation::ReflowOnResize> reflowOnResize { true };
    ConfigEntry<std::unordered_map<std::string, vtbackend::ColorPalette>, documentation::ColorSchemes>
        colorschemes { { { "default", vtbackend::ColorPalette {} } } };
    ConfigEntry<std::unordered_map<std::string, TerminalProfile>, documentation::Profiles> profiles {
        { { "main", TerminalProfile {} } }
    };
    ConfigEntry<std::string, "default_profile: {}\n"> defaultProfileName { "main" };
    ConfigEntry<std::string, documentation::WordDelimiters> wordDelimiters {
        " /\\()\"'-.,:;<>~!@#$%^&*+=[]{{}}~?|│"
    };
    ConfigEntry<vtbackend::Modifiers, documentation::BypassMouseProtocolModifiers>
        bypassMouseProtocolModifiers { vtbackend::Modifier::Shift };
    ConfigEntry<contour::config::SelectionAction, documentation::OnMouseSelection> onMouseSelection {
        contour::config::SelectionAction::CopyToSelectionClipboard
    };
    ConfigEntry<vtbackend::Modifiers, documentation::MouseBlockSelectionModifiers>
        mouseBlockSelectionModifiers { vtbackend::Modifier::Control };
    ConfigEntry<InputMappings, documentation::InputMappings> inputMappings { defaultInputMappings };
    ConfigEntry<unsigned, documentation::EarlyExitThreshold> earlyExitThreshold {
        documentation::DefaultEarlyExitThreshold
    };
    ConfigEntry<bool, documentation::SpawnNewProcess> spawnNewProcess { false };
    ConfigEntry<bool, documentation::SixelScrolling> sixelScrolling { true };
    ConfigEntry<vtbackend::ImageSize, documentation::MaxImageSize> maxImageSize { { vtpty::Width { 0 },
                                                                                    vtpty::Height { 0 } } };
    ConfigEntry<int, documentation::MaxImageColorRegisters> maxImageColorRegisters { 4096 };
    ConfigEntry<std::set<std::string>, documentation::ExperimentalFeatures> experimentalFeatures {};

    TerminalProfile* profile(std::string const& name) noexcept
    {
        assert(!name.empty());
        if (auto i = profiles.value().find(name); i != profiles.value().end())
            return &i->second;
        assert(false && "Profile not found.");
        return nullptr;
    }

    [[nodiscard]] TerminalProfile const* profile(std::string const& name) const
    {
        assert(!name.empty());
        if (auto i = profiles.value().find(name); i != profiles.value().end())
            return &i->second;
        assert(false && "Profile not found.");
        crispy::unreachable();
    }

    TerminalProfile& profile() noexcept
    {
        if (auto* prof = profile(defaultProfileName.value()); prof)
            return *prof;
        crispy::unreachable();
    }

    [[nodiscard]] TerminalProfile const& profile() const noexcept
    {
        if (auto const* prof = profile(defaultProfileName.value()); prof)
            return *prof;
        crispy::unreachable();
    }
};

struct YAMLConfigReader
{
    std::filesystem::path configFile;
    YAML::Node doc;
    logstore::category const& logger;

    YAMLConfigReader(std::string const& filename, logstore::category const& log):
        configFile(filename), logger { log }
    {
        try
        {
            doc = YAML::LoadFile(configFile.string());
        }
        catch (std::exception const& e)
        {
            errorLog()("Configuration file is corrupted. {}\nDefault config will be loaded.", e.what());
        }
    }

    template <typename T, documentation::StringLiteral D>
    void loadFromEntry(YAML::Node const& node, std::string const& entry, ConfigEntry<T, D>& where)
    {
        logger()("Loading entry: {}", entry);
        try
        {
            loadFromEntry(node, entry, where.value());
        }
        catch (std::exception const& e)
        {
            logger()("Failed, default value will be used");
        }
    }

    template <typename T, documentation::StringLiteral D>
    void loadFromEntry(std::string const& entry, ConfigEntry<T, D>& where)
    {
        loadFromEntry(doc, entry, where.value());
    }

    template <typename T>
        requires std::is_scalar_v<T>
    void loadFromEntry(YAML::Node const& node, std::string const& entry, T& where)
    {
        auto const child = node[entry];
        if (child)
            where = child.as<T>();
        logger()("Loading entry: {}, value {}", entry, where);
    }

    template <typename V, typename T>
    void loadFromEntry(YAML::Node const& node, std::string const& entry, boxed::boxed<V, T>& where)
    {
        auto const child = node[entry];
        if (child)
            where = boxed::boxed<V, T>(child.as<V>());
        logger()("Loading entry: {}, value {}", entry, where.template as<V>());
    }

    // Used for terminal profile and color scheme loading
    template <typename T>
    void loadFromEntry(YAML::Node const& node,
                       std::string const& entry,
                       std::unordered_map<std::string, T>& where)
    {
        if (auto const child = node[entry]; child && child.IsMap())
        {
            for (auto entry: child)
            {
                auto const name = entry.first.as<std::string>();
                logger()("Loading map with entry: {}", name);
                loadFromEntry(child, entry.first.as<std::string>(), where[name]);
            }
        }
    }

    void loadFromEntry(YAML::Node const& node, std::string const& entry, std::chrono::milliseconds& where)
    {
        if (auto const child = node[entry]; child)
            where = std::chrono::milliseconds(child.as<int>());
        logger()("Loading entry: {}, value {}", entry, where.count());
    }

    template <typename Input>
    void appendOrCreateBinding(std::vector<vtbackend::InputBinding<Input, ActionList>>& bindings,
                               vtbackend::MatchModes modes,
                               vtbackend::Modifiers modifier,
                               Input input,
                               actions::Action action)
    {
        for (auto& binding: bindings)
        {
            if (match(binding, modes, modifier, input))
            {
                binding.binding.emplace_back(std::move(action));
                return;
            }
        }

        bindings.emplace_back(vtbackend::InputBinding<Input, ActionList> {
            modes, modifier, input, ActionList { std::move(action) } });
    }

    // clang-format off
    void loadFromEntry(YAML::Node const& node, std::string const& entry, std::filesystem::path& where);
    void loadFromEntry(YAML::Node const& node, std::string const& entry, RenderingBackend& where);
    void loadFromEntry(YAML::Node const& node, std::string const& entry, crispy::strong_hashtable_size& where);
    void loadFromEntry(YAML::Node const& node, std::string const& entry, vtbackend::MaxHistoryLineCount& where);
    void loadFromEntry(YAML::Node const& node, std::string const& entry, crispy::lru_capacity& where);
    void loadFromEntry(YAML::Node const& node, std::string const& entry, vtbackend::CursorDisplay& where);
    void loadFromEntry(YAML::Node const& node, std::string const& entry, vtbackend::Modifiers& where);
    void loadFromEntry(YAML::Node const& node, std::string const& entry, vtbackend::CursorShape& where);
    void loadFromEntry(YAML::Node const& node, std::string const& entry, contour::config::SelectionAction& where);
    void loadFromEntry(YAML::Node const& node, std::string const& entry, InputMappings& where);
    void loadFromEntry(YAML::Node const& node, std::string const& entry, vtbackend::ImageSize& where);
    void loadFromEntry(YAML::Node const& node, std::string const& entry, std::set<std::string>& where);
    void loadFromEntry(YAML::Node const& node, std::string const& entry, std::string& where);
    void loadFromEntry(YAML::Node const& node, std::string const& entry, vtbackend::StatusDisplayPosition& where);
    void loadFromEntry(YAML::Node const& node, std::string const& entry, ScrollBarPosition& where);
    void loadFromEntry(YAML::Node const& node, std::string const& entry, vtrasterizer::FontDescriptions& where);
    void loadFromEntry(YAML::Node const& node, std::string const& entry, text::render_mode& where);
    void loadFromEntry(YAML::Node const& node, std::string const& entry, vtrasterizer::FontLocatorEngine& where);
    void loadFromEntry(YAML::Node const& node, std::string const& entry, vtrasterizer::TextShapingEngine& where);
    void loadFromEntry(YAML::Node const& node, std::string const& entry, ColorConfig& where);
    void loadFromEntry(YAML::Node const& node, std::string const& entry, text::font_description& where);
    void loadFromEntry(YAML::Node const& node, std::string const& entry, std::vector<text::font_feature>& where);
    void loadFromEntry(YAML::Node const& node, std::string const& entry, text::font_weight& where);
    void loadFromEntry(YAML::Node const& node, std::string const& entry, text::font_slant& where);
    void loadFromEntry(YAML::Node const& node, std::string const& entry, text::font_size& where);
    void loadFromEntry(YAML::Node const& node, std::string const& entry, vtbackend::LineCount& where);
    void loadFromEntry(YAML::Node const& node, std::string const& entry, vtbackend::VTType& where);
    void loadFromEntry(YAML::Node const& node, std::string const& entry, vtpty::PageSize& where);
    void loadFromEntry(YAML::Node const& node, std::string const& entry, WindowMargins& where);
    void loadFromEntry(YAML::Node const& node, std::string const& entry, vtbackend::LineOffset& where);
    void loadFromEntry(YAML::Node const& node, std::string const& entry, vtpty::Process::ExecInfo& where);
    void loadFromEntry(YAML::Node const& node, std::string const& entry, vtpty::SshHostConfig& where);
    void loadFromEntry(YAML::Node const& node, std::string const& entry, Bell& where);
    void loadFromEntry(YAML::Node const& node, std::string const& entry, std::map<vtbackend::DECMode, bool>& where);
    void loadFromEntry(YAML::Node const& node, std::string const& entry, vtrasterizer::Decorator& where);
    void loadFromEntry(YAML::Node const& node, std::string const& entry, vtbackend::Opacity& where);
    void loadFromEntry(YAML::Node const& node, std::string const& entry, vtbackend::StatusDisplayType& where);
    void loadFromEntry(YAML::Node const& node, std::string const& entry, Permission& where);
    void loadFromEntry(YAML::Node const& node, std::string const& entry, std::shared_ptr<vtbackend::BackgroundImage>& where);
    void loadFromEntry(YAML::Node const& node, std::string const& entry, vtbackend::CellRGBColor& where);
    void loadFromEntry(YAML::Node const& node, std::string const& entry, vtbackend::CursorColor& where);
    void loadFromEntry(YAML::Node const& node, std::string const& entry, vtbackend::RGBColor& where);
    void loadFromEntry(YAML::Node const& node, std::string const& entry, vtbackend::RGBColorPair& where);
    void loadFromEntry(YAML::Node const& node, std::string const& entry, vtbackend::CellRGBColorAndAlphaPair& where);
    void loadFromEntry(YAML::Node const& node, std::string const& entry, vtbackend::ColorPalette::Palette& colors);
    void loadFromEntry(YAML::Node const& node, std::string const& entry, vtbackend::ColorPalette& where);
    void loadFromEntry(YAML::Node const& node, vtbackend::ColorPalette& where);
    void loadFromEntry(YAML::Node const& node, std::string const& entry, TerminalProfile& where);
    void defaultSettings(vtpty::Process::ExecInfo& shell);
    // clang-format on

    void load(Config& c);

    std::optional<actions::Action> parseAction(YAML::Node const& node);
    std::optional<vtbackend::Modifiers> parseModifierKey(std::string const& key);
    std::optional<vtbackend::Modifiers> parseModifier(YAML::Node const& nodeYAML);
    static std::optional<vtbackend::MatchModes> parseMatchModes(YAML::Node const& nodeYAML);
    std::optional<vtbackend::Key> parseKey(std::string const& name);
    std::optional<std::variant<vtbackend::Key, char32_t>> parseKeyOrChar(std::string const& name);

    bool tryAddKey(InputMappings& inputMappings,
                   vtbackend::MatchModes modes,
                   vtbackend::Modifiers modifier,
                   YAML::Node const& node,
                   actions::Action action);
    std::optional<vtbackend::MouseButton> parseMouseButton(YAML::Node const& node);
    bool tryAddMouse(std::vector<MouseInputMapping>& bindings,
                     vtbackend::MatchModes modes,
                     vtbackend::Modifiers modifier,
                     YAML::Node const& node,
                     actions::Action action);
};

struct YAMLConfigWriter
{
    std::string static addOffset(std::string const& doc, size_t off)
    {
        auto offset = std::string(off, ' ');
        return std::regex_replace(doc, std::regex(".+\n"), offset + "$&");
    }

    struct Offset
    {
        static inline int levels = 0;
        Offset() { levels++; }
        ~Offset() { --levels; }
    };

    template <typename F>
    void scoped(F lambda)
    {
        auto const _ = Offset {};
        lambda();
    }

    std::string createString(Config const& c);

    template <typename T>
    auto format(T v)
    {
        return fmt::format("{}", v);
    }

    template <typename... T>
    [[nodiscard]] std::string format(std::string_view doc, T... args)
    {
        return fmt::format(fmt::runtime(doc), format(args)..., fmt::arg("comment", "#"));
    }

    [[nodiscard]] std::string format(KeyInputMapping v)
    {
        return format("{:<30},{:<30},{:<30}\n",
                      format("- {{ mods: [{}]", format(v.modifiers)),
                      format(" key: '{}'", v.input),
                      format(" action: {} }}", v.binding[0]));
    }

    [[nodiscard]] std::string format(CharInputMapping v)
    {
        auto actionAndModes = format(" action: {} }}", v.binding[0]);
        if (v.modes.any())
        {
            actionAndModes = format(" action: {}, mode: '{}' }}", v.binding[0], v.modes);
        }
        return format("{:<30},{:<30},{:<30}\n",
                      format("- {{ mods: [{}]", format(v.modifiers)),
                      format(" key: '{}'", static_cast<char>(v.input)),
                      actionAndModes);
    }

    [[nodiscard]] std::string format(MouseInputMapping v)
    {
        auto actionAndModes = format(" action: {} }}", v.binding[0]);
        return format("{:<30},{:<30},{:<30}\n",
                      format("- {{ mods: [{}]", format(v.modifiers)),
                      format(" mouse: {}", v.input),
                      actionAndModes);
    }

    [[nodiscard]] std::string static format(std::vector<text::font_feature> const& v)
    {

        auto result = std::string { "[" };
        result.append(v | ranges::views::transform([](auto f) { return fmt::format("{}", f); })
                      | ranges::views::join(", ") | ranges::to<std::string>);
        result.append("]");
        return result;
    }

    [[nodiscard]] std::string static format(vtbackend::Modifiers const& flags)
    {
        std::string result;
        for (auto i = 0u; i < sizeof(vtbackend::Modifier) * 8; ++i)
        {
            auto const flag = static_cast<vtbackend::Modifier>(1 << i);
            if (!flags.test(flag))
                continue;

            // We assume that only valid enum values resulting into non-empty strings.
            auto const element = fmt::format("{}", flag);
            if (element.empty())
                continue;

            if (!result.empty())
                result += ',';

            result += element;
        }
        return result;
    }

    [[nodiscard]] std::string format(std::string_view doc, vtrasterizer::FontDescriptions const& v)
    {
        return format(doc,
                      v.size.pt,
                      v.fontLocator,
                      v.textShapingEngine,
                      v.builtinBoxDrawing,
                      v.renderMode,
                      "true",
                      v.regular.familyName,
                      v.regular.weight,
                      v.regular.slant,
                      format(v.regular.features),
                      v.emoji.familyName);
    }

    template <typename T, typename R>
    [[nodiscard]] std::string format(std::string_view doc, std::chrono::duration<T, R> v)
    {
        return format(doc, v.count());
    }

    [[nodiscard]] std::string format(std::string_view doc, vtpty::Process::ExecInfo const& v)
    {
        auto args = std::string { "[" };
        args.append(v.arguments | ranges::views::join(", ") | ranges::to<std::string>);
        args.append("]");
        return format(doc, v.program, args);
    }

    [[nodiscard]] std::string format(std::string_view doc, vtpty::SshHostConfig const& v)
    {
        return format(doc, v.hostname);
    }

    [[nodiscard]] std::string static format(vtbackend::CellRGBColor const& v)
    {
        if (std::holds_alternative<vtbackend::RGBColor>(v))
            return fmt::format("'{}'", v);

        return fmt::format("{}", v);
    }

    [[nodiscard]] std::string static format(vtbackend::RGBColor const& v) { return fmt::format("'{}'", v); }

    [[nodiscard]] std::string format(std::string_view doc, vtbackend::MaxHistoryLineCount v)
    {
        if (std::holds_alternative<vtbackend::Infinite>(v))
            return format(doc, -1);
        auto number = unbox(std::get<vtbackend::LineCount>(v));
        return format(doc, number);
    }

    [[nodiscard]] std::string format(std::string_view doc, vtbackend::ImageSize v)
    {
        return format(doc, unbox(v.width), unbox(v.height));
    }

    [[nodiscard]] std::string format(std::string_view doc, vtbackend::PageSize v)
    {
        return format(doc, unbox(v.columns), unbox(v.lines));
    }

    [[nodiscard]] std::string format(std::string_view doc, ColorConfig& v)
    {
        if (auto* simple = get_if<SimpleColorConfig>(&v))
            return format(doc, simple->colorScheme);
        else if (auto* dual = get_if<DualColorConfig>(&v))
        {
            return format(doc,
                          fmt::format(fmt::runtime("\n"
                                                   "    light: {}\n"
                                                   "    dark: {}\n"
                                                   "\n"),
                                      dual->colorSchemeLight,
                                      dual->colorSchemeDark));
        }

        return format(doc, "BAD");
    }

    [[nodiscard]] std::string format(std::string_view doc, Bell& v)
    {
        return format(doc, v.sound, v.volume, v.alert);
    }

    [[nodiscard]] std::string format(std::string_view doc, WindowMargins& v)
    {
        return format(doc, v.horizontal, v.vertical);
    }

    [[nodiscard]] std::string format(std::string_view doc, InputModeConfig v)
    {
        auto const shape = [v]() -> std::string_view {
            switch (v.cursor.cursorShape)
            {
                case vtbackend::CursorShape::Block: return "block";
                case vtbackend::CursorShape::Rectangle: return "rectangle";
                case vtbackend::CursorShape::Underscore: return "underscore";
                case vtbackend::CursorShape::Bar: return "bar";
            };
            return "unknown";
        }();
        auto const blinking = v.cursor.cursorDisplay == vtbackend::CursorDisplay::Blink ? true : false;
        auto const blinkingInterval = v.cursor.cursorBlinkInterval.count();
        return format(doc, shape, blinking, blinkingInterval);
    }
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
struct fmt::formatter<contour::config::Permission>: formatter<std::string_view>
{
    auto format(contour::config::Permission value, format_context& ctx)
    {
        string_view name;
        switch (value)
        {
            case contour::config::Permission::Allow: name = "allow"; break;
            case contour::config::Permission::Deny: name = "deny"; break;
            case contour::config::Permission::Ask: name = "ask"; break;
        }
        return formatter<string_view>::format(name, ctx);
    }
};

template <>
struct fmt::formatter<vtbackend::Opacity>: formatter<float>
{
    auto format(vtbackend::Opacity value, format_context& ctx)
    {
        return formatter<float>::format(static_cast<float>(value) / std::numeric_limits<uint8_t>::max(), ctx);
    }
};

template <>
struct fmt::formatter<crispy::strong_hashtable_size>: formatter<uint>
{
    auto format(crispy::strong_hashtable_size value, format_context& ctx)
    {
        return formatter<uint>::format(value.value, ctx);
    }
};

template <>
struct fmt::formatter<vtbackend::StatusDisplayPosition>: formatter<std::string_view>
{
    auto format(vtbackend::StatusDisplayPosition value, format_context& ctx)
    {
        string_view name;
        switch (value)
        {
            case vtbackend::StatusDisplayPosition::Bottom: name = "Bottom"; break;
            case vtbackend::StatusDisplayPosition::Top: name = "Top"; break;
        }
        return formatter<string_view>::format(name, ctx);
    }
};

template <>
struct fmt::formatter<vtbackend::BackgroundImage>: formatter<std::string_view>
{
    auto format(vtbackend::BackgroundImage value, format_context& ctx)
    {
        if (auto* loc = std::get_if<std::filesystem::path>(&value.location))
            return formatter<string_view>::format(loc->string(), ctx);
        return formatter<string_view>::format("Image", ctx);
    }
};

template <>
struct fmt::formatter<vtbackend::StatusDisplayType>: formatter<std::string_view>
{
    auto format(vtbackend::StatusDisplayType value, format_context& ctx)
    {
        string_view name;
        switch (value)
        {
            case vtbackend::StatusDisplayType::None: name = "none"; break;
            case vtbackend::StatusDisplayType::Indicator: name = "indicator"; break;
            case vtbackend::StatusDisplayType::HostWritable: name = "host writable"; break;
        }
        return formatter<string_view>::format(name, ctx);
    }
};

template <>
struct fmt::formatter<crispy::lru_capacity>: formatter<uint>
{
    auto format(crispy::lru_capacity value, format_context& ctx)
    {
        return formatter<uint>::format(value.value, ctx);
    }
};

template <>
struct fmt::formatter<std::set<std::basic_string<char>>>: formatter<std::string_view>
{
    auto format(std::set<std::basic_string<char>> value, format_context& ctx)
    {
        auto result = std::string {};
        result.append(value | ranges::views::join(", ") | ranges::to<std::string>);
        return formatter<std::string_view>::format(result, ctx);
    }
};

template <>
struct fmt::formatter<contour::config::SelectionAction>: formatter<std::string_view>
{
    auto format(contour::config::SelectionAction value, format_context& ctx)
    {
        std::string_view name;
        switch (value)
        {
            case contour::config::SelectionAction::CopyToClipboard: name = "CopyToClipboard"; break;
            case contour::config::SelectionAction::CopyToSelectionClipboard:
                name = "CopyToSelectionClipboard";
                break;
            case contour::config::SelectionAction::Nothing: name = "Waiting"; break;
        }
        return formatter<std::string_view>::format(name, ctx);
    }
};

template <>
struct fmt::formatter<contour::config::ScrollBarPosition>: formatter<std::string_view>
{
    auto format(contour::config::ScrollBarPosition value, format_context& ctx)
    {
        std::string_view name;
        switch (value)
        {
            case contour::config::ScrollBarPosition::Hidden: name = "Hidden"; break;
            case contour::config::ScrollBarPosition::Left: name = "Left"; break;
            case contour::config::ScrollBarPosition::Right: name = "Right"; break;
        }
        return formatter<std::string_view>::format(name, ctx);
    }
};

template <>
struct fmt::formatter<contour::config::RenderingBackend>: formatter<std::string_view>
{
    auto format(contour::config::RenderingBackend const& val, fmt::format_context& ctx)
    {
        std::string_view name;
        switch (val)
        {
            case contour::config::RenderingBackend::Default: name = "default"; break;
            case contour::config::RenderingBackend::OpenGL: name = "OpenGL"; break;
            case contour::config::RenderingBackend::Software: name = "software"; break;
        }
        return formatter<std::string_view>::format(name, ctx);
    }
};

template <>
struct fmt::formatter<contour::config::WindowMargins>: public fmt::formatter<std::string>
{
    using WindowMargins = contour::config::WindowMargins;
    auto format(WindowMargins margins, format_context& ctx) -> format_context::iterator
    {
        return formatter<std::string>::format(fmt::format("{}x+{}y", margins.horizontal, margins.vertical),
                                              ctx);
    }
};

template <typename T, contour::config::documentation::StringLiteral D>
struct fmt::formatter<contour::config::ConfigEntry<T, D>>
{
    auto format(contour::config::ConfigEntry<T, D> const& c, fmt::format_context& ctx)
    {
        return fmt::format_to(ctx.out(), "{}", c.value());
    }
};
// }}}
