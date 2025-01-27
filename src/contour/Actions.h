// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <crispy/assert.h>
#include <crispy/utils.h>

#include <format>
#include <optional>
#include <string>
#include <variant>

namespace contour::actions
{

// Defines the format to use when extracting a selection range from the terminal.
enum class CopyFormat : uint8_t
{
    // Copies purely the text (with their whitespaces, and newlines, but no formatting).
    Text,

    // Copies the selection in HTML format.
    HTML,

    // Copies the selection in escaped VT sequence format.
    VT,

    // Copies the selection as PNG image.
    PNG,
};

// clang-format off
struct CancelSelection{};
struct ChangeProfile{ std::string name; };
struct ClearHistoryAndReset{};
struct CopyPreviousMarkRange{};
struct CopySelection{ CopyFormat format = CopyFormat::Text; };
struct CreateDebugDump{};
struct CreateSelection{ std::string delimiters; };
struct DecreaseFontSize{};
struct DecreaseOpacity{};
struct FocusNextSearchMatch{};
struct FocusPreviousSearchMatch{};
struct FollowHyperlink{};
struct IncreaseFontSize{};
struct IncreaseOpacity{};
struct NewTerminal{ std::optional<std::string> profileName; };
struct NoSearchHighlight{};
struct OpenConfiguration{};
struct OpenFileManager{};
struct OpenSelection{};
struct PasteClipboard{ bool strip = false; };
struct PasteSelection{ bool evaluateInShell = false;};
struct Quit{};
struct ReloadConfig{ std::optional<std::string> profileName; };
struct ResetConfig{};
struct ResetFontSize{};
struct ScreenshotVT{};
struct SaveScreenshot{};
struct CopyScreenshot{};
struct ScrollDown{};
struct ScrollMarkDown{};
struct ScrollMarkUp{};
struct ScrollOneDown{};
struct ScrollOneUp{};
struct ScrollPageDown{};
struct ScrollPageUp{};
struct ScrollToBottom{};
struct ScrollToTop{};
struct ScrollUp{};
struct SearchReverse{};
struct SendChars{ std::string chars; };
struct ToggleAllKeyMaps{};
struct ToggleFullscreen{};
struct ToggleInputProtection{};
struct ToggleStatusLine{};
struct ToggleTitleBar{};
struct TraceBreakAtEmptyQueue{};
struct TraceEnter{};
struct TraceLeave{};
struct TraceStep{};
struct ViNormalMode{};
struct WriteScreen{ std::string chars; }; // "\033[2J\033[3J"
struct CreateNewTab{};
struct CloseTab{};
struct MoveTabTo{ int position; };
struct MoveTabToLeft{};
struct MoveTabToRight{};
struct SwitchToTab{ int position; };
struct SwitchToPreviousTab{};
struct SwitchToTabLeft{};
struct SwitchToTabRight{};
// clang-format on

using Action = std::variant<CancelSelection,
                            ChangeProfile,
                            ClearHistoryAndReset,
                            CopyPreviousMarkRange,
                            CopySelection,
                            CreateDebugDump,
                            CreateSelection,
                            DecreaseFontSize,
                            DecreaseOpacity,
                            FocusNextSearchMatch,
                            FocusPreviousSearchMatch,
                            FollowHyperlink,
                            IncreaseFontSize,
                            IncreaseOpacity,
                            NewTerminal,
                            NoSearchHighlight,
                            OpenConfiguration,
                            OpenFileManager,
                            OpenSelection,
                            PasteClipboard,
                            PasteSelection,
                            Quit,
                            ReloadConfig,
                            ResetConfig,
                            ResetFontSize,
                            ScreenshotVT,
                            SaveScreenshot,
                            CopyScreenshot,
                            ScrollDown,
                            ScrollMarkDown,
                            ScrollMarkUp,
                            ScrollOneDown,
                            ScrollOneUp,
                            ScrollPageDown,
                            ScrollPageUp,
                            ScrollToBottom,
                            ScrollToTop,
                            ScrollUp,
                            SearchReverse,
                            SendChars,
                            ToggleAllKeyMaps,
                            ToggleFullscreen,
                            ToggleInputProtection,
                            ToggleStatusLine,
                            ToggleTitleBar,
                            TraceBreakAtEmptyQueue,
                            TraceEnter,
                            TraceLeave,
                            TraceStep,
                            ViNormalMode,
                            WriteScreen,
                            CreateNewTab,
                            CloseTab,
                            MoveTabTo,
                            MoveTabToLeft,
                            MoveTabToRight,
                            SwitchToTab,
                            SwitchToPreviousTab,
                            SwitchToTabLeft,
                            SwitchToTabRight>;

template <typename T>
concept NonRepeatableActionConcept = crispy::one_of<T, CreateNewTab, CloseTab>;

std::optional<Action> fromString(std::string const& name);

namespace documentation
{
    constexpr inline std::string_view CancelSelection { "Cancels currently active selection, if any." };
    constexpr inline std::string_view ChangeProfile { "Changes the profile to the given profile `name`." };
    constexpr inline std::string_view ClearHistoryAndReset {
        "Clears the history, performs a terminal hard reset and attempts to force a redraw of the currently "
        "running application."
    };
    constexpr inline std::string_view CopyPreviousMarkRange {
        "Copies the most recent range that is delimited by vertical line marks into clipboard."
    };
    constexpr inline std::string_view CopySelection {
        "Copies the current selection into the clipboard buffer."
    };
    constexpr inline std::string_view CreateSelection {
        "Creates selection with custom delimiters configured via `delimiters` member."
    };
    constexpr inline std::string_view CreateDebugDump { "Create dump for debug purposes" };
    constexpr inline std::string_view DecreaseFontSize { "Decreases the font size by 1 pixel." };
    constexpr inline std::string_view DecreaseOpacity { "Decreases the default-background opacity by 5%." };
    constexpr inline std::string_view FocusNextSearchMatch { "Focuses the next search match (if any)." };
    constexpr inline std::string_view FocusPreviousSearchMatch {
        "Focuses the next previous match (if any)."
    };
    constexpr inline std::string_view FollowHyperlink {
        "Follows the hyperlink that is exposed via OSC 8 under the current cursor position."
    };
    constexpr inline std::string_view IncreaseFontSize { "Increases the font size by 1 pixel." };
    constexpr inline std::string_view IncreaseOpacity { "Increases the default-background opacity by 5%." };
    constexpr inline std::string_view NewTerminal {
        "Spawns a new terminal at the current terminals current working directory."
    };
    constexpr inline std::string_view NoSearchHighlight {
        "Disables current search highlighting, if anything is still highlighted due to a prior search."
    };
    constexpr inline std::string_view OpenConfiguration { "Opens the configuration file." };
    constexpr inline std::string_view OpenFileManager {
        "Opens the current working directory in a system file manager."
    };
    constexpr inline std::string_view OpenSelection {
        "Open the current terminal selection with the default system application (eg; xdg-open)"
    };
    constexpr inline std::string_view PasteClipboard {
        "Pastes clipboard to standard input. Pass boolean parameter 'strip' to indicate whether or not to "
        "strip repetitive whitespaces down to one and newlines to whitespaces."
    };
    constexpr inline std::string_view PasteSelection { "Pastes current selection to standard input."
                                                       "Option `evaluate_in_shell` specify if pasted text "
                                                       "must be appended with linefeed and used as an input "
                                                       "for the running shell" };
    constexpr inline std::string_view Quit { "Quits the application." };
    constexpr inline std::string_view ReloadConfig { "Forces a configuration reload." };
    constexpr inline std::string_view ResetConfig {
        "Overwrites current configuration with builtin default configuration and loads it. Attention, all "
        "your current configuration will be lost due to overwrite!"
    };
    constexpr inline std::string_view ResetFontSize {
        "Resets font size to what is configured in the config file."
    };
    constexpr inline std::string_view ScreenshotVT { "Takes a screenshot in form of VT escape sequences." };
    constexpr inline std::string_view SaveScreenshot { "Takes a screenshot and saves it into a file." };
    constexpr inline std::string_view CopyScreenshot {
        "takes a screenshot and puts it into the system clipboard"
    };
    constexpr inline std::string_view ScrollDown { "Scrolls down by the multiplier factor." };
    constexpr inline std::string_view ScrollMarkDown {
        "Scrolls one mark down (if none present, bottom of the screen)"
    };
    constexpr inline std::string_view ScrollMarkUp { "Scrolls one mark up" };
    constexpr inline std::string_view ScrollOneDown { "Scrolls down by exactly one line." };
    constexpr inline std::string_view ScrollOneUp { "Scrolls up by exactly one line." };
    constexpr inline std::string_view ScrollPageDown { "Scrolls a page down." };
    constexpr inline std::string_view ScrollPageUp { "Scrolls a page up." };
    constexpr inline std::string_view ScrollToBottom { "Scrolls to the bottom of the screen buffer." };
    constexpr inline std::string_view ScrollToTop { "Scrolls to the top of the screen buffer." };
    constexpr inline std::string_view ScrollUp { "Scrolls up by the multiplier factor." };
    constexpr inline std::string_view SearchReverse {
        "Initiates search mode (starting to search at current cursor position, moving upwards)."
    };
    constexpr inline std::string_view SendChars {
        "Writes given characters in `chars` member to the applications input."
    };
    constexpr inline std::string_view ToggleAllKeyMaps { "Disables/enables responding to all keybinds (this "
                                                         "keybind will be preserved when disabling all "
                                                         "others)." };
    constexpr inline std::string_view ToggleFullscreen { "Enables/disables full screen mode." };
    constexpr inline std::string_view ToggleInputProtection { "Enables/disables terminal input protection." };
    constexpr inline std::string_view ToggleStatusLine {
        "Shows/hides the VT320 compatible Indicator status line."
    };
    constexpr inline std::string_view ToggleTitleBar { "Shows/Hides titlebar" };
    constexpr inline std::string_view TraceBreakAtEmptyQueue {
        "Executes any pending VT sequence from the VT sequence buffer in trace mode, then waits."
    };
    constexpr inline std::string_view TraceEnter {
        "Enables trace mode, suspending execution until explicitly "
        "requested to continue (See TraceLeave and TraceStep)."
    };
    constexpr inline std::string_view TraceLeave { "Disables trace mode. Any pending VT sequence will be "
                                                   "flushed out and normal execution will be resumed." };
    constexpr inline std::string_view TraceStep {
        "Executes a single VT sequence that is to be executed next."
    };
    constexpr inline std::string_view ViNormalMode {
        "Enters/Leaves Vi-like normal mode. The cursor can then be moved via h/j/k/l movements in normal "
        "mode and text can be selected via `v`, yanked via `y`, and clipboard pasted via `p`."
    };
    constexpr inline std::string_view WriteScreen {
        "Writes VT sequence in `chars` member to the screen (bypassing the application)."
    };
    constexpr inline std::string_view CreateNewTab { "Creates a new tab in the terminal emulator." };
    constexpr inline std::string_view CloseTab { "Closes current tab." };
    constexpr inline std::string_view MoveTabTo {
        "Moves current tab to the given position (starting at number 1)."
    };
    constexpr inline std::string_view MoveTabToLeft { "Moves current tab to the left." };
    constexpr inline std::string_view MoveTabToRight { "Moves current tab to the right." };
    constexpr inline std::string_view SwitchToTab {
        "Switch to absolute tab position (starting at number 1)"
    };
    constexpr inline std::string_view SwitchToPreviousTab { "Switch to the previously focused tab" };
    constexpr inline std::string_view SwitchToTabLeft { "Switch to tab to the left" };
    constexpr inline std::string_view SwitchToTabRight { "Switch to tab to the right" };
} // namespace documentation

#if defined(__clang__) && __clang_major__ >= 19
constexpr
#endif
    inline auto
    getDocumentation()
{
    return std::array {
        std::tuple { Action { CancelSelection {} }, documentation::CancelSelection },
        std::tuple { Action { ChangeProfile {} }, documentation::ChangeProfile },
        std::tuple { Action { ClearHistoryAndReset {} }, documentation::ClearHistoryAndReset },
        std::tuple { Action { CopyPreviousMarkRange {} }, documentation::CopyPreviousMarkRange },
        std::tuple { Action { CopySelection {} }, documentation::CopySelection },
        std::tuple { Action { CreateDebugDump {} }, documentation::CreateDebugDump },
        std::tuple { Action { CreateSelection {} }, documentation::CreateSelection },
        std::tuple { Action { DecreaseFontSize {} }, documentation::DecreaseFontSize },
        std::tuple { Action { DecreaseOpacity {} }, documentation::DecreaseOpacity },
        std::tuple { Action { FocusNextSearchMatch {} }, documentation::FocusNextSearchMatch },
        std::tuple { Action { FocusPreviousSearchMatch {} }, documentation::FocusPreviousSearchMatch },
        std::tuple { Action { FollowHyperlink {} }, documentation::FollowHyperlink },
        std::tuple { Action { IncreaseFontSize {} }, documentation::IncreaseFontSize },
        std::tuple { Action { IncreaseOpacity {} }, documentation::IncreaseOpacity },
        std::tuple { Action { NewTerminal {} }, documentation::NewTerminal },
        std::tuple { Action { NoSearchHighlight {} }, documentation::NoSearchHighlight },
        std::tuple { Action { OpenConfiguration {} }, documentation::OpenConfiguration },
        std::tuple { Action { OpenFileManager {} }, documentation::OpenFileManager },
        std::tuple { Action { OpenSelection {} }, documentation::OpenSelection },
        std::tuple { Action { PasteClipboard {} }, documentation::PasteClipboard },
        std::tuple { Action { PasteSelection {} }, documentation::PasteSelection },
        std::tuple { Action { Quit {} }, documentation::Quit },
        std::tuple { Action { ReloadConfig {} }, documentation::ReloadConfig },
        std::tuple { Action { ResetConfig {} }, documentation::ResetConfig },
        std::tuple { Action { ResetFontSize {} }, documentation::ResetFontSize },
        std::tuple { Action { ScreenshotVT {} }, documentation::ScreenshotVT },
        std::tuple { Action { SaveScreenshot {} }, documentation::SaveScreenshot },
        std::tuple { Action { CopyScreenshot {} }, documentation::CopyScreenshot },
        std::tuple { Action { ScrollDown {} }, documentation::ScrollDown },
        std::tuple { Action { ScrollMarkDown {} }, documentation::ScrollMarkDown },
        std::tuple { Action { ScrollMarkUp {} }, documentation::ScrollMarkUp },
        std::tuple { Action { ScrollOneDown {} }, documentation::ScrollOneDown },
        std::tuple { Action { ScrollOneUp {} }, documentation::ScrollOneUp },
        std::tuple { Action { ScrollPageDown {} }, documentation::ScrollPageDown },
        std::tuple { Action { ScrollPageUp {} }, documentation::ScrollPageUp },
        std::tuple { Action { ScrollToBottom {} }, documentation::ScrollToBottom },
        std::tuple { Action { ScrollToTop {} }, documentation::ScrollToTop },
        std::tuple { Action { ScrollUp {} }, documentation::ScrollUp },
        std::tuple { Action { SearchReverse {} }, documentation::SearchReverse },
        std::tuple { Action { SendChars {} }, documentation::SendChars },
        std::tuple { Action { ToggleAllKeyMaps {} }, documentation::ToggleAllKeyMaps },
        std::tuple { Action { ToggleFullscreen {} }, documentation::ToggleFullscreen },
        std::tuple { Action { ToggleInputProtection {} }, documentation::ToggleInputProtection },
        std::tuple { Action { ToggleStatusLine {} }, documentation::ToggleStatusLine },
        std::tuple { Action { ToggleTitleBar {} }, documentation::ToggleTitleBar },
        std::tuple { Action { TraceBreakAtEmptyQueue {} }, documentation::TraceBreakAtEmptyQueue },
        std::tuple { Action { TraceEnter {} }, documentation::TraceEnter },
        std::tuple { Action { TraceLeave {} }, documentation::TraceLeave },
        std::tuple { Action { TraceStep {} }, documentation::TraceStep },
        std::tuple { Action { ViNormalMode {} }, documentation::ViNormalMode },
        std::tuple { Action { WriteScreen {} }, documentation::WriteScreen },
        std::tuple { Action { CreateNewTab {} }, documentation::CreateNewTab },
        std::tuple { Action { CloseTab {} }, documentation::CloseTab },
        std::tuple { Action { MoveTabTo {} }, documentation::MoveTabTo },
        std::tuple { Action { MoveTabToLeft {} }, documentation::MoveTabToLeft },
        std::tuple { Action { MoveTabToRight {} }, documentation::MoveTabToRight },
        std::tuple { Action { SwitchToTab {} }, documentation::SwitchToTab },
        std::tuple { Action { SwitchToPreviousTab {} }, documentation::SwitchToPreviousTab },
        std::tuple { Action { SwitchToTabLeft {} }, documentation::SwitchToTabLeft },
        std::tuple { Action { SwitchToTabRight {} }, documentation::SwitchToTabRight },
    };
}

#if defined(__clang__) && __clang_major__ >= 19
static_assert(getDocumentation().size() == std::variant_size_v<Action>);
#endif

} // namespace contour::actions

// {{{ fmtlib custom formatters
#define DECLARE_ACTION_FMT(T)                                                    \
    template <>                                                                  \
    struct std::formatter<contour::actions::T>: std::formatter<std::string_view> \
    {                                                                            \
        auto format(contour::actions::T const&, auto& ctx) const                 \
        {                                                                        \
            return formatter<string_view>::format(#T, ctx);                      \
        }                                                                        \
    };

// {{{ declare
DECLARE_ACTION_FMT(CancelSelection)
DECLARE_ACTION_FMT(ChangeProfile)
DECLARE_ACTION_FMT(ClearHistoryAndReset)
DECLARE_ACTION_FMT(CopyPreviousMarkRange)
DECLARE_ACTION_FMT(CopySelection)
DECLARE_ACTION_FMT(CreateDebugDump)
DECLARE_ACTION_FMT(CreateSelection)
DECLARE_ACTION_FMT(DecreaseFontSize)
DECLARE_ACTION_FMT(DecreaseOpacity)
DECLARE_ACTION_FMT(FocusNextSearchMatch)
DECLARE_ACTION_FMT(FocusPreviousSearchMatch)
DECLARE_ACTION_FMT(FollowHyperlink)
DECLARE_ACTION_FMT(IncreaseFontSize)
DECLARE_ACTION_FMT(IncreaseOpacity)
DECLARE_ACTION_FMT(NewTerminal)
DECLARE_ACTION_FMT(NoSearchHighlight)
DECLARE_ACTION_FMT(OpenConfiguration)
DECLARE_ACTION_FMT(OpenFileManager)
DECLARE_ACTION_FMT(OpenSelection)
DECLARE_ACTION_FMT(PasteClipboard)
DECLARE_ACTION_FMT(PasteSelection)
DECLARE_ACTION_FMT(Quit)
DECLARE_ACTION_FMT(ReloadConfig)
DECLARE_ACTION_FMT(ResetConfig)
DECLARE_ACTION_FMT(ResetFontSize)
DECLARE_ACTION_FMT(ScreenshotVT)
DECLARE_ACTION_FMT(SaveScreenshot)
DECLARE_ACTION_FMT(CopyScreenshot)
DECLARE_ACTION_FMT(ScrollDown)
DECLARE_ACTION_FMT(ScrollMarkDown)
DECLARE_ACTION_FMT(ScrollMarkUp)
DECLARE_ACTION_FMT(ScrollOneDown)
DECLARE_ACTION_FMT(ScrollOneUp)
DECLARE_ACTION_FMT(ScrollPageDown)
DECLARE_ACTION_FMT(ScrollPageUp)
DECLARE_ACTION_FMT(ScrollToBottom)
DECLARE_ACTION_FMT(ScrollToTop)
DECLARE_ACTION_FMT(ScrollUp)
DECLARE_ACTION_FMT(SearchReverse)
DECLARE_ACTION_FMT(SendChars)
DECLARE_ACTION_FMT(ToggleAllKeyMaps)
DECLARE_ACTION_FMT(ToggleFullscreen)
DECLARE_ACTION_FMT(ToggleInputProtection)
DECLARE_ACTION_FMT(ToggleStatusLine)
DECLARE_ACTION_FMT(ToggleTitleBar)
DECLARE_ACTION_FMT(TraceBreakAtEmptyQueue)
DECLARE_ACTION_FMT(TraceEnter)
DECLARE_ACTION_FMT(TraceLeave)
DECLARE_ACTION_FMT(TraceStep)
DECLARE_ACTION_FMT(ViNormalMode)
DECLARE_ACTION_FMT(WriteScreen)
DECLARE_ACTION_FMT(CreateNewTab)
DECLARE_ACTION_FMT(CloseTab)
DECLARE_ACTION_FMT(MoveTabToLeft)
DECLARE_ACTION_FMT(MoveTabToRight)
DECLARE_ACTION_FMT(SwitchToPreviousTab)
DECLARE_ACTION_FMT(SwitchToTabLeft)
DECLARE_ACTION_FMT(SwitchToTabRight)
// }}}
#undef DECLARE_ACTION_FMT

template <>
struct std::formatter<contour::actions::MoveTabTo>: std::formatter<std::string>
{
    auto format(contour::actions::MoveTabTo const& value, auto& ctx) const
    {
        return formatter<string>::format(std::format("MoveTabTo {{ position: {} }}", value.position), ctx);
    }
};

template <>
struct std::formatter<contour::actions::SwitchToTab>: std::formatter<std::string>
{
    auto format(contour::actions::SwitchToTab const& value, auto& ctx) const
    {
        return formatter<string>::format(std::format("SwitchToTab {{ position: {} }}", value.position), ctx);
    }
};

#define HANDLE_ACTION(T)                                                  \
    if (std::holds_alternative<contour::actions::T>(_action))             \
    {                                                                     \
        name = std::format("{}", std::get<contour::actions::T>(_action)); \
    }

template <>
struct std::formatter<contour::actions::Action>: std::formatter<std::string>
{
    auto format(contour::actions::Action const& _action, auto& ctx) const
    {
        std::string name = "Unknown action";
        // {{{ handle
        HANDLE_ACTION(CancelSelection);
        HANDLE_ACTION(ChangeProfile);
        HANDLE_ACTION(ClearHistoryAndReset);
        HANDLE_ACTION(CopyPreviousMarkRange);
        HANDLE_ACTION(CopySelection);
        HANDLE_ACTION(CreateDebugDump);
        HANDLE_ACTION(DecreaseFontSize);
        HANDLE_ACTION(DecreaseOpacity);
        HANDLE_ACTION(FocusNextSearchMatch);
        HANDLE_ACTION(FocusPreviousSearchMatch);
        HANDLE_ACTION(FollowHyperlink);
        HANDLE_ACTION(IncreaseFontSize);
        HANDLE_ACTION(IncreaseOpacity);
        HANDLE_ACTION(NewTerminal);
        HANDLE_ACTION(NoSearchHighlight);
        HANDLE_ACTION(OpenConfiguration);
        HANDLE_ACTION(OpenFileManager);
        HANDLE_ACTION(OpenSelection);
        HANDLE_ACTION(PasteClipboard);
        HANDLE_ACTION(PasteSelection);
        HANDLE_ACTION(Quit);
        HANDLE_ACTION(ReloadConfig);
        HANDLE_ACTION(ResetConfig);
        HANDLE_ACTION(ResetFontSize);
        HANDLE_ACTION(ScreenshotVT);
        HANDLE_ACTION(CopyScreenshot);
        HANDLE_ACTION(SaveScreenshot);
        HANDLE_ACTION(ScrollDown);
        HANDLE_ACTION(ScrollMarkDown);
        HANDLE_ACTION(ScrollMarkUp);
        HANDLE_ACTION(ScrollOneDown);
        HANDLE_ACTION(ScrollOneUp);
        HANDLE_ACTION(ScrollPageDown);
        HANDLE_ACTION(ScrollPageUp);
        HANDLE_ACTION(ScrollToBottom);
        HANDLE_ACTION(ScrollToTop);
        HANDLE_ACTION(ScrollUp);
        HANDLE_ACTION(SearchReverse);
        HANDLE_ACTION(SendChars);
        HANDLE_ACTION(ToggleAllKeyMaps);
        HANDLE_ACTION(ToggleFullscreen);
        HANDLE_ACTION(ToggleInputProtection);
        HANDLE_ACTION(ToggleStatusLine);
        HANDLE_ACTION(ToggleTitleBar);
        HANDLE_ACTION(TraceBreakAtEmptyQueue);
        HANDLE_ACTION(TraceEnter);
        HANDLE_ACTION(TraceLeave);
        HANDLE_ACTION(TraceStep);
        HANDLE_ACTION(ViNormalMode);
        HANDLE_ACTION(CreateNewTab);
        HANDLE_ACTION(CloseTab);
        HANDLE_ACTION(MoveTabToLeft);
        HANDLE_ACTION(MoveTabToRight);
        HANDLE_ACTION(SwitchToPreviousTab);
        HANDLE_ACTION(SwitchToTabLeft);
        HANDLE_ACTION(SwitchToTabRight);
        if (std::holds_alternative<contour::actions::MoveTabTo>(_action))
        {
            const auto action = std::get<contour::actions::MoveTabTo>(_action);
            name = std::format("MoveTabTo, position: {}", action.position);
        }
        if (std::holds_alternative<contour::actions::SwitchToTab>(_action))
        {
            const auto action = std::get<contour::actions::SwitchToTab>(_action);
            name = std::format("SwitchToTab, position: {}", action.position);
        }
        if (std::holds_alternative<contour::actions::WriteScreen>(_action))
        {
            const auto writeScreenAction = std::get<contour::actions::WriteScreen>(_action);
            name = std::format("{}, chars: '{}'", writeScreenAction, writeScreenAction.chars);
        }
        if (std::holds_alternative<contour::actions::CreateSelection>(_action))
        {
            const auto createSelectionAction = std::get<contour::actions::CreateSelection>(_action);
            name =
                std::format("{}, delimiters: '{}'", createSelectionAction, createSelectionAction.delimiters);
        }
        // }}}
        return formatter<string>::format(name, ctx);
    }
};

template <>
struct std::formatter<contour::actions::CopyFormat>: std::formatter<std::string_view>
{
    auto format(contour::actions::CopyFormat value, auto& ctx) const
    {
        string_view output;
        switch (value)
        {
            case contour::actions::CopyFormat::Text: output = "Text"; break;
            case contour::actions::CopyFormat::HTML: output = "HTML"; break;
            case contour::actions::CopyFormat::PNG: output = "PNG"; break;
            case contour::actions::CopyFormat::VT: output = "VT"; break;
        }
        return formatter<string_view>::format(output, ctx);
    }
};

#undef HANDLE_ACTION
// ]}}
