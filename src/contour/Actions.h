// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <vtbackend/HintModeHandler.h>

#include <crispy/assert.h>
#include <crispy/utils.h>

#include <array>
#include <format>
#include <optional>
#include <string>
#include <tuple>
#include <type_traits>
#include <variant>
#include <vector>

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

/// A cardinal direction for directional pane actions (resize).
///
/// Kept local to the action layer (rather than reusing vtmux::FocusDirection) so this layer stays
/// transport-agnostic — it describes *what the user asked for*, and the dispatch handler translates it
/// to the model's vtmux::FocusDirection. Mirrors how CopyFormat is a local enum.
enum class Direction : uint8_t
{
    Left,
    Right,
    Up,
    Down,
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
struct HintMode{ std::string patterns; vtbackend::HintAction hintAction = vtbackend::HintAction::Copy; };
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
struct ToggleInputMethodHandling {};
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
struct SetTabTitle{};
struct SplitVertical{};    // split the active pane left/right
struct SplitHorizontal{};  // split the active pane top/bottom
struct ClosePane{};
struct FocusPaneLeft{};
struct FocusPaneRight{};
struct FocusPaneUp{};
struct FocusPaneDown{};
struct SwapPaneLeft{};      // swap the active pane with its left neighbor
struct SwapPaneRight{};     // swap the active pane with its right neighbor
struct SwapPaneUp{};        // swap the active pane with its upper neighbor
struct SwapPaneDown{};      // swap the active pane with its lower neighbor
struct MovePaneLeft{};      // re-parent the active pane past its left neighbor
struct MovePaneRight{};     // re-parent the active pane past its right neighbor
struct MovePaneUp{};        // re-parent the active pane past its upper neighbor
struct MovePaneDown{};      // re-parent the active pane past its lower neighbor
struct ToggleSplitOrientation{}; // flip the active pane's split axis (H<->V)
struct TogglePaneZoom{};    // give the active pane the whole tab area (hiding its siblings), and back
struct ResizePane{ Direction direction; int percent = 5; }; // grow/shrink the active pane
struct LaunchLayout{ std::string name; }; // open the named layout's tabs in the current window
struct SaveLayout{ std::string name; };   // save the current window's tabs/panes as the named layout
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
                            HintMode,
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
                            ToggleInputMethodHandling,
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
                            SwitchToTabRight,
                            SetTabTitle,
                            SplitVertical,
                            SplitHorizontal,
                            ClosePane,
                            FocusPaneLeft,
                            FocusPaneRight,
                            FocusPaneUp,
                            FocusPaneDown,
                            SwapPaneLeft,
                            SwapPaneRight,
                            SwapPaneUp,
                            SwapPaneDown,
                            MovePaneLeft,
                            MovePaneRight,
                            MovePaneUp,
                            MovePaneDown,
                            ToggleSplitOrientation,
                            TogglePaneZoom,
                            ResizePane,
                            LaunchLayout,
                            SaveLayout>;

/// Actions that must fire exactly once per physical keypress and be dropped on key auto-repeat.
///
/// These are structural/destructive actions where a held key must not be amplified into a burst:
/// creating or closing a tab, closing a pane, and splitting a pane. Splitting is included because
/// each fire forks a new shell process and shrinks the layout, so a held split key would spawn a
/// burst of panes until they collapse — the same amplification the tab/pane actions guard against.
/// The toggles belong here for the mirror-image reason: a held key would flip them back and forth
/// once per repeat, so where the key lands would be decided by the repeat count rather than the user.
/// The keyboard dispatch (handleAction) consults this concept to filter such actions out of
/// KeyboardEventType::Repeat events.
template <typename T>
concept NonRepeatableActionConcept = crispy::one_of<T,
                                                    CreateNewTab,
                                                    CloseTab,
                                                    ClosePane,
                                                    SplitVertical,
                                                    SplitHorizontal,
                                                    SwapPaneLeft,
                                                    SwapPaneRight,
                                                    SwapPaneUp,
                                                    SwapPaneDown,
                                                    MovePaneLeft,
                                                    MovePaneRight,
                                                    MovePaneUp,
                                                    MovePaneDown,
                                                    ToggleSplitOrientation,
                                                    TogglePaneZoom,
                                                    LaunchLayout,
                                                    SaveLayout>;

/// @returns true if @p action must be dropped on keyboard auto-repeat (a NonRepeatableActionConcept
/// member), false otherwise.
[[nodiscard]] inline bool isNonRepeatable(Action const& action) noexcept
{
    return std::visit(crispy::overloaded {
                          [](NonRepeatableActionConcept auto const&) { return true; },
                          [](auto const&) { return false; },
                      },
                      action);
}

/// Filters @p actions down to those that may fire on a keyboard auto-repeat event, dropping every
/// NonRepeatableActionConcept member (so a held key cannot be amplified into a burst of, e.g.,
/// tab/pane closes).
/// @param actions The actions bound to the key being repeated.
/// @return A copy containing only the repeatable actions, preserving order.
[[nodiscard]] inline std::vector<Action> filterRepeatableActions(std::vector<Action> const& actions)
{
    std::vector<Action> result;
    result.reserve(actions.size());
    for (auto const& action: actions)
        if (!isNonRepeatable(action))
            result.push_back(action);
    return result;
}

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
    constexpr inline std::string_view HintMode {
        "Activates hint mode: scans the visible terminal for regex-matched patterns (URLs, file paths, "
        "git hashes, etc.), renders short alphabetic labels on each match, and lets the user type a label "
        "to act on that match (copy, open, paste). Parameter `patterns` selects which pattern set to use, "
        "and `hint_action` specifies the action (Copy, Open, Paste, CopyAndPaste, Select)."
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
    constexpr inline std::string_view ToggleInputMethodHandling {
        "Enables/disables IME (input method editor) handling."
    };
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
    constexpr inline std::string_view SetTabTitle {
        "Rename the current tab inline (opens the tab-title editor)"
    };
    constexpr inline std::string_view SplitVertical {
        "Splits the active pane into two side-by-side panes (a vertical divider)."
    };
    constexpr inline std::string_view SplitHorizontal {
        "Splits the active pane into two stacked panes (a horizontal divider)."
    };
    constexpr inline std::string_view ClosePane { "Closes the active pane (or the tab if it is the last "
                                                  "pane)." };
    constexpr inline std::string_view FocusPaneLeft { "Moves pane focus to the left." };
    constexpr inline std::string_view FocusPaneRight { "Moves pane focus to the right." };
    constexpr inline std::string_view FocusPaneUp { "Moves pane focus up." };
    constexpr inline std::string_view FocusPaneDown { "Moves pane focus down." };
    constexpr inline std::string_view SwapPaneLeft { "Swaps the active pane with its left neighbor." };
    constexpr inline std::string_view SwapPaneRight { "Swaps the active pane with its right neighbor." };
    constexpr inline std::string_view SwapPaneUp { "Swaps the active pane with its upper neighbor." };
    constexpr inline std::string_view SwapPaneDown { "Swaps the active pane with its lower neighbor." };
    constexpr inline std::string_view MovePaneLeft { "Moves the active pane past its left neighbor." };
    constexpr inline std::string_view MovePaneRight { "Moves the active pane past its right neighbor." };
    constexpr inline std::string_view MovePaneUp { "Moves the active pane past its upper neighbor." };
    constexpr inline std::string_view MovePaneDown { "Moves the active pane past its lower neighbor." };
    constexpr inline std::string_view ToggleSplitOrientation {
        "Flips the orientation of the active pane's split (horizontal <-> vertical)."
    };
    constexpr inline std::string_view TogglePaneZoom {
        "Zooms the active pane so it alone fills the tab, hiding its siblings; toggles back to the "
        "tiled layout. While zoomed, moving pane focus keeps the zoom and shows the newly focused pane."
    };
    constexpr inline std::string_view ResizePane {
        "Grows or shrinks the active pane in the given direction (by an optional percent)."
    };
    constexpr inline std::string_view LaunchLayout {
        "Opens a named layout, appending its tabs to the current window."
    };
    constexpr inline std::string_view SaveLayout {
        "Saves the current window's tabs as a named layout in layouts.yml."
    };
} // namespace documentation

/// One row of the action catalog: everything that is generically known about an action.
///
/// This is the single source of truth tying an action's three facets together — the name it is
/// written as in `input_mapping:`, an instance to construct from that name, and what it does.
/// Deriving all three from one table is what keeps them from drifting apart: adding an action is
/// adding a row here, not editing three lists that a reader has to diff by eye.
struct ActionCatalogEntry
{
    std::string_view name; ///< Canonical, YAML-facing name, e.g. "SplitVertical".
    Action prototype;      ///< An instance; one carrying arguments still needs them filled in.
    std::string_view documentation; ///< Human-readable description, for the docs and the command palette.
};

/// Every action, in Action's own alternative order.
///
/// The static_assert below pins the table to the variant, so a new alternative cannot be added
/// without a row (which would otherwise silently make the action unnameable in the config, absent
/// from the generated docs, and invisible in the command palette).
///
/// A function over a build-once static, rather than a `constexpr` variable: several actions carry a
/// std::string, and a string cannot survive constant evaluation into a constexpr object (MSVC rejects
/// it outright — "points to memory which was heap allocated during constant evaluation"). Built on
/// first use and handed out by reference, so a lookup costs a scan and never an allocation.
[[nodiscard]] inline auto const& actionCatalog()
{
    static auto const catalog = std::array {
        ActionCatalogEntry {
            "CancelSelection", Action { CancelSelection {} }, documentation::CancelSelection },
        ActionCatalogEntry { "ChangeProfile", Action { ChangeProfile {} }, documentation::ChangeProfile },
        ActionCatalogEntry {
            "ClearHistoryAndReset", Action { ClearHistoryAndReset {} }, documentation::ClearHistoryAndReset },
        ActionCatalogEntry { "CopyPreviousMarkRange",
                             Action { CopyPreviousMarkRange {} },
                             documentation::CopyPreviousMarkRange },
        ActionCatalogEntry { "CopySelection", Action { CopySelection {} }, documentation::CopySelection },
        ActionCatalogEntry {
            "CreateDebugDump", Action { CreateDebugDump {} }, documentation::CreateDebugDump },
        ActionCatalogEntry {
            "CreateSelection", Action { CreateSelection {} }, documentation::CreateSelection },
        ActionCatalogEntry {
            "DecreaseFontSize", Action { DecreaseFontSize {} }, documentation::DecreaseFontSize },
        ActionCatalogEntry {
            "DecreaseOpacity", Action { DecreaseOpacity {} }, documentation::DecreaseOpacity },
        ActionCatalogEntry {
            "FocusNextSearchMatch", Action { FocusNextSearchMatch {} }, documentation::FocusNextSearchMatch },
        ActionCatalogEntry { "FocusPreviousSearchMatch",
                             Action { FocusPreviousSearchMatch {} },
                             documentation::FocusPreviousSearchMatch },
        ActionCatalogEntry {
            "FollowHyperlink", Action { FollowHyperlink {} }, documentation::FollowHyperlink },
        ActionCatalogEntry { "HintMode", Action { HintMode {} }, documentation::HintMode },
        ActionCatalogEntry {
            "IncreaseFontSize", Action { IncreaseFontSize {} }, documentation::IncreaseFontSize },
        ActionCatalogEntry {
            "IncreaseOpacity", Action { IncreaseOpacity {} }, documentation::IncreaseOpacity },
        ActionCatalogEntry { "NewTerminal", Action { NewTerminal {} }, documentation::NewTerminal },
        ActionCatalogEntry {
            "NoSearchHighlight", Action { NoSearchHighlight {} }, documentation::NoSearchHighlight },
            ActionCatalogEntry {
            "OpenConfiguration", Action { OpenConfiguration {} }, documentation::OpenConfiguration },
        ActionCatalogEntry {
            "OpenFileManager", Action { OpenFileManager {} }, documentation::OpenFileManager },
        ActionCatalogEntry { "OpenSelection", Action { OpenSelection {} }, documentation::OpenSelection },
        ActionCatalogEntry { "PasteClipboard", Action { PasteClipboard {} }, documentation::PasteClipboard },
        ActionCatalogEntry { "PasteSelection", Action { PasteSelection {} }, documentation::PasteSelection },
        ActionCatalogEntry { "Quit", Action { Quit {} }, documentation::Quit },
        ActionCatalogEntry { "ReloadConfig", Action { ReloadConfig {} }, documentation::ReloadConfig },
        ActionCatalogEntry { "ResetConfig", Action { ResetConfig {} }, documentation::ResetConfig },
        ActionCatalogEntry { "ResetFontSize", Action { ResetFontSize {} }, documentation::ResetFontSize },
        ActionCatalogEntry { "ScreenshotVT", Action { ScreenshotVT {} }, documentation::ScreenshotVT },
        ActionCatalogEntry { "SaveScreenshot", Action { SaveScreenshot {} }, documentation::SaveScreenshot },
        ActionCatalogEntry { "CopyScreenshot", Action { CopyScreenshot {} }, documentation::CopyScreenshot },
        ActionCatalogEntry { "ScrollDown", Action { ScrollDown {} }, documentation::ScrollDown },
        ActionCatalogEntry { "ScrollMarkDown", Action { ScrollMarkDown {} }, documentation::ScrollMarkDown },
        ActionCatalogEntry { "ScrollMarkUp", Action { ScrollMarkUp {} }, documentation::ScrollMarkUp },
        ActionCatalogEntry { "ScrollOneDown", Action { ScrollOneDown {} }, documentation::ScrollOneDown },
        ActionCatalogEntry { "ScrollOneUp", Action { ScrollOneUp {} }, documentation::ScrollOneUp },
        ActionCatalogEntry { "ScrollPageDown", Action { ScrollPageDown {} }, documentation::ScrollPageDown },
        ActionCatalogEntry { "ScrollPageUp", Action { ScrollPageUp {} }, documentation::ScrollPageUp },
        ActionCatalogEntry { "ScrollToBottom", Action { ScrollToBottom {} }, documentation::ScrollToBottom },
        ActionCatalogEntry { "ScrollToTop", Action { ScrollToTop {} }, documentation::ScrollToTop },
        ActionCatalogEntry { "ScrollUp", Action { ScrollUp {} }, documentation::ScrollUp },
        ActionCatalogEntry { "SearchReverse", Action { SearchReverse {} }, documentation::SearchReverse },
        ActionCatalogEntry { "SendChars", Action { SendChars {} }, documentation::SendChars },
        ActionCatalogEntry {
            "ToggleAllKeyMaps", Action { ToggleAllKeyMaps {} }, documentation::ToggleAllKeyMaps },
        ActionCatalogEntry {
            "ToggleFullscreen", Action { ToggleFullscreen {} }, documentation::ToggleFullscreen },
        ActionCatalogEntry { "ToggleInputMethodHandling",
                             Action { ToggleInputMethodHandling {} },
                             documentation::ToggleInputMethodHandling },
        ActionCatalogEntry { "ToggleInputProtection",
                             Action { ToggleInputProtection {} },
                             documentation::ToggleInputProtection },
        ActionCatalogEntry {
            "ToggleStatusLine", Action { ToggleStatusLine {} }, documentation::ToggleStatusLine },
        ActionCatalogEntry { "ToggleTitleBar", Action { ToggleTitleBar {} }, documentation::ToggleTitleBar },
        ActionCatalogEntry { "TraceBreakAtEmptyQueue",
                             Action { TraceBreakAtEmptyQueue {} },
                             documentation::TraceBreakAtEmptyQueue },
        ActionCatalogEntry { "TraceEnter", Action { TraceEnter {} }, documentation::TraceEnter },
        ActionCatalogEntry { "TraceLeave", Action { TraceLeave {} }, documentation::TraceLeave },
        ActionCatalogEntry { "TraceStep", Action { TraceStep {} }, documentation::TraceStep },
        ActionCatalogEntry { "ViNormalMode", Action { ViNormalMode {} }, documentation::ViNormalMode },
        ActionCatalogEntry { "WriteScreen", Action { WriteScreen {} }, documentation::WriteScreen },
        ActionCatalogEntry { "CreateNewTab", Action { CreateNewTab {} }, documentation::CreateNewTab },
        ActionCatalogEntry { "CloseTab", Action { CloseTab {} }, documentation::CloseTab },
        ActionCatalogEntry { "MoveTabTo", Action { MoveTabTo {} }, documentation::MoveTabTo },
        ActionCatalogEntry { "MoveTabToLeft", Action { MoveTabToLeft {} }, documentation::MoveTabToLeft },
        ActionCatalogEntry { "MoveTabToRight", Action { MoveTabToRight {} }, documentation::MoveTabToRight },
        ActionCatalogEntry { "SwitchToTab", Action { SwitchToTab {} }, documentation::SwitchToTab },
        ActionCatalogEntry {
            "SwitchToPreviousTab", Action { SwitchToPreviousTab {} }, documentation::SwitchToPreviousTab },
        ActionCatalogEntry {
            "SwitchToTabLeft", Action { SwitchToTabLeft {} }, documentation::SwitchToTabLeft },
        ActionCatalogEntry {
            "SwitchToTabRight", Action { SwitchToTabRight {} }, documentation::SwitchToTabRight },
        ActionCatalogEntry { "SetTabTitle", Action { SetTabTitle {} }, documentation::SetTabTitle },
        ActionCatalogEntry { "SplitVertical", Action { SplitVertical {} }, documentation::SplitVertical },
        ActionCatalogEntry {
            "SplitHorizontal", Action { SplitHorizontal {} }, documentation::SplitHorizontal },
        ActionCatalogEntry { "ClosePane", Action { ClosePane {} }, documentation::ClosePane },
        ActionCatalogEntry { "FocusPaneLeft", Action { FocusPaneLeft {} }, documentation::FocusPaneLeft },
        ActionCatalogEntry { "FocusPaneRight", Action { FocusPaneRight {} }, documentation::FocusPaneRight },
        ActionCatalogEntry { "FocusPaneUp", Action { FocusPaneUp {} }, documentation::FocusPaneUp },
        ActionCatalogEntry { "FocusPaneDown", Action { FocusPaneDown {} }, documentation::FocusPaneDown },
        ActionCatalogEntry { "SwapPaneLeft", Action { SwapPaneLeft {} }, documentation::SwapPaneLeft },
        ActionCatalogEntry { "SwapPaneRight", Action { SwapPaneRight {} }, documentation::SwapPaneRight },
        ActionCatalogEntry { "SwapPaneUp", Action { SwapPaneUp {} }, documentation::SwapPaneUp },
        ActionCatalogEntry { "SwapPaneDown", Action { SwapPaneDown {} }, documentation::SwapPaneDown },
        ActionCatalogEntry { "MovePaneLeft", Action { MovePaneLeft {} }, documentation::MovePaneLeft },
        ActionCatalogEntry { "MovePaneRight", Action { MovePaneRight {} }, documentation::MovePaneRight },
        ActionCatalogEntry { "MovePaneUp", Action { MovePaneUp {} }, documentation::MovePaneUp },
        ActionCatalogEntry { "MovePaneDown", Action { MovePaneDown {} }, documentation::MovePaneDown },
        ActionCatalogEntry { "ToggleSplitOrientation",
                             Action { ToggleSplitOrientation {} },
                             documentation::ToggleSplitOrientation },
        ActionCatalogEntry { "TogglePaneZoom", Action { TogglePaneZoom {} }, documentation::TogglePaneZoom },
        ActionCatalogEntry {
            "ResizePane", Action { ResizePane { Direction::Right } }, documentation::ResizePane },
        ActionCatalogEntry { "LaunchLayout", Action { LaunchLayout {} }, documentation::LaunchLayout },
        ActionCatalogEntry { "SaveLayout", Action { SaveLayout {} }, documentation::SaveLayout },
    };
    return catalog;
}

static_assert(std::tuple_size_v<std::remove_cvref_t<decltype(actionCatalog())>>
                  == std::variant_size_v<Action>,
              "every Action alternative must have exactly one actionCatalog() row: without it the action "
              "cannot be named in input_mapping, is missing from the generated docs, and never shows up "
              "in the command palette");

/// The canonical name of @p action, as written in `input_mapping:` — e.g. "SplitVertical".
///
/// Note this is the name of the action's KIND: any argument it carries is not part of it. Use it
/// where the action must be identified rather than described (std::format("{}", action) renders the
/// arguments too, which is what a debug log wants and an identity does not).
/// @param action The action to name.
/// @return Its catalog name; empty only if the catalog and the variant have drifted (which the
///         static_assert above makes impossible).
/// The catalog row describing @p action.
///
/// A direct index, not a search: the table is written in Action's own alternative order, so an
/// action's variant index IS its row. The static_assert above pins the table's SIZE to the variant;
/// Actions_test pins the ORDER (every row's prototype sits at its own index), which together make
/// this lookup total.
/// @param action The action to look up.
/// @return Its row.
[[nodiscard]] inline ActionCatalogEntry const& catalogEntry(Action const& action) noexcept
{
    return actionCatalog()[action.index()];
}

[[nodiscard]] inline std::string_view name(Action const& action) noexcept
{
    return catalogEntry(action).name;
}

/// The documentation of @p action: what it does, in one or two sentences.
/// @param action The action to describe.
/// @return Its catalog description.
[[nodiscard]] inline std::string_view describe(Action const& action) noexcept
{
    return catalogEntry(action).documentation;
}

} // namespace contour::actions

// {{{ fmtlib custom formatters

template <>
struct std::formatter<contour::actions::Direction>: std::formatter<std::string_view>
{
    auto format(contour::actions::Direction value, auto& ctx) const
    {
        using contour::actions::Direction;
        auto const name = [value]() -> string_view {
            switch (value)
            {
                case Direction::Left: return "Left";
                case Direction::Right: return "Right";
                case Direction::Up: return "Up";
                case Direction::Down: return "Down";
            }
            return "Right"; // unreachable: the switch is exhaustive over the enum
        }();
        return formatter<string_view>::format(name, ctx);
    }
};

/// Renders the arguments @p action carries as YAML sibling keys, e.g. ", position: 3".
///
/// A FLAT sibling-key form, deliberately: a serialized binding must stay a valid YAML flow map
/// (`{ mods: [...], key: 'T', action: ResizePane, direction: Left, percent: 5 }`), so a nested
/// `{ ... }` here would break the enclosing map. Empty for the actions that carry no arguments.
///
/// @param action The action whose arguments to render.
/// @return The arguments, ready to append to the action's name.
[[nodiscard]] inline std::string formatActionArguments(contour::actions::Action const& action)
{
    using namespace contour::actions;
    return std::visit(
        crispy::overloaded {
            [](ResizePane const& a) {
                return std::format(", direction: {}, percent: {}", a.direction, a.percent);
            },
            [](MoveTabTo const& a) { return std::format(", position: {}", a.position); },
            [](SwitchToTab const& a) { return std::format(", position: {}", a.position); },
            [](WriteScreen const& a) { return std::format(", chars: '{}'", a.chars); },
            [](CreateSelection const& a) { return std::format(", delimiters: '{}'", a.delimiters); },
            [](LaunchLayout const& a) { return std::format(", name: '{}'", a.name); },
            [](SaveLayout const& a) { return std::format(", name: '{}'", a.name); },
            [](auto const&) { return std::string {}; },
        },
        action);
}

/// Renders an action the way `input_mapping:` spells it: its name, plus its arguments as sibling keys.
///
/// The name comes from the SAME catalog row that fromString() parses with, so the config Contour
/// writes is by construction the config Contour can read back. It used to come from a second,
/// independently-maintained table (a `#T` stringification per action): the two could drift, and a
/// drift would have silently emitted a binding that no longer parses.
template <>
struct std::formatter<contour::actions::Action>: std::formatter<std::string>
{
    auto format(contour::actions::Action const& action, auto& ctx) const
    {
        return formatter<string>::format(
            std::format("{}{}", contour::actions::name(action), formatActionArguments(action)), ctx);
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

template <>
struct std::formatter<vtbackend::HintAction>: std::formatter<std::string_view>
{
    auto format(vtbackend::HintAction value, auto& ctx) const
    {
        string_view output;
        switch (value)
        {
            case vtbackend::HintAction::Copy: output = "Copy"; break;
            case vtbackend::HintAction::Open: output = "Open"; break;
            case vtbackend::HintAction::Paste: output = "Paste"; break;
            case vtbackend::HintAction::CopyAndPaste: output = "CopyAndPaste"; break;
            case vtbackend::HintAction::Select: output = "Select"; break;
        }
        return formatter<string_view>::format(output, ctx);
    }
};

// }}}
