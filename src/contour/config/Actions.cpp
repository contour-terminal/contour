// SPDX-License-Identifier: Apache-2.0
#include <contour/config/Actions.hpp>

#include <core/Utils.hpp>

#include <algorithm>
#include <optional>
#include <string>

using namespace std;

using core::toLower;

namespace contour::actions
{

optional<Action> fromString(string const& name)
{
    // Case-insensitive, so `input_mapping:` may spell an action however it reads best.
    auto const lowerCaseName = toLower(name);
    auto const& catalog = actionCatalog();
    auto const entry = std::ranges::find_if(
        catalog, [&](auto const& candidate) { return toLower(candidate.name) == lowerCaseName; });

    if (entry == catalog.end())
        return nullopt;

    // The prototype: a ParameterizedActionConcept action still has its argument filled in by the
    // caller (the YAML reader) from the sibling keys of the `action:` entry.
    return entry->prototype;
}

namespace
{
    /// The sibling keys of a block action's own arguments, beside the ones that address its block: none for
    /// most.
    /// @return The keys, ready to append.
    template <CommandBlockActionConcept BlockAction>
    [[nodiscard]] std::string formatOwnArguments(BlockAction const& /*action*/)
    {
        return {};
    }

    /// OpenCommandOutput's own sibling keys: the program only when the binding names one (without, the
    /// configured pager runs), then the placement, then the format only when the binding names one (without,
    /// the program's own format applies).
    /// @param action The action.
    /// @return The keys, ready to append.
    [[nodiscard]] inline std::string formatOwnArguments(OpenCommandOutput const& action)
    {
        auto arguments = action.program == OpenCommandOutput {}.program
                             ? std::string {}
                             : std::format(", program: {}", config::SingleQuoted { action.program });
        arguments += std::format(", placement: {}", config::configEnumToken(action.placement));
        if (action.format)
            arguments += std::format(", format: {}", config::configEnumToken(*action.format));
        return arguments;
    }

    /// The sibling keys of a block action: `, target: T`, then `, part: P` where it has a part, then the
    /// pinned `, block: N`, then the action's own. The target and the part are written even at their
    /// defaults; the block only when one is pinned.
    /// @param action The action.
    /// @return The keys, ready to append.
    template <CommandBlockActionConcept BlockAction>
    [[nodiscard]] std::string formatCommandBlockArguments(BlockAction const& action)
    {
        auto arguments = std::format(", target: {}", config::configEnumToken(action.target));
        if constexpr (requires { action.part; })
            arguments += std::format(", part: {}", config::configEnumToken(action.part));
        if (action.block != BlockAction {}.block)
            arguments += std::format(", block: {}", action.block.value);
        return arguments + formatOwnArguments(action);
    }

    /// Renders the arguments @p action carries as YAML sibling keys, e.g. ", position: 3".
    ///
    /// A FLAT sibling-key form, deliberately: a serialized binding must stay a valid YAML flow map
    /// (`{ mods: [...], key: 'T', action: ResizePane, direction: Left, percent: 5 }`), so a nested
    /// `{ ... }` here would break the enclosing map. Empty for the actions that carry no arguments.
    ///
    /// @param action The action whose arguments to render.
    /// @return The arguments, ready to append to the action's name.
    [[nodiscard]] std::string formatActionArguments(Action const& action)
    {
        using namespace contour::actions;
        return std::visit(
            core::Overloaded {
                [](ResizePane const& a) {
                    return std::format(", direction: {}, percent: {}", a.direction, a.percent);
                },
                [](MoveTabTo const& a) { return std::format(", position: {}", a.position); },
                [](ToggleFoldAt const& a) { return std::format(", line: {}", a.line); },
                [](SwitchToTab const& a) { return std::format(", position: {}", a.position); },
                [](WriteScreen const& a) { return std::format(", chars: '{}'", a.chars); },
                [](CreateSelection const& a) { return std::format(", delimiters: '{}'", a.delimiters); },
                [](LaunchLayout const& a) { return std::format(", name: '{}'", a.name); },
                [](AttachToHost const& a) { return std::format(", name: '{}'", a.name); },
                [](SaveLayout const& a) {
                    // A nameless SaveLayout opens the save-as prompt, so — like a colorless SetTabColor below
                    // — it has no argument to write; a named one round-trips its name.
                    return a.name.empty() ? std::string {} : std::format(", name: '{}'", a.name);
                },
                [](CreateNewTab const& a) {
                    // A profile-less CreateNewTab opens the default profile, so it has no argument to write.
                    return a.profileName ? std::format(", profile: '{}'", *a.profileName) : std::string {};
                },
                [](CopySelection const& a) {
                    // Each argument is written only when it is not the default, so a plain CopySelection
                    // stays plain.
                    auto const defaults = CopySelection {};
                    auto arguments = std::string {};
                    if (a.format != defaults.format)
                        arguments += std::format(", format: {}", a.format);
                    if (a.fallback != defaults.fallback)
                        arguments += std::format(", fallback: {}", config::configEnumToken(a.fallback));
                    return arguments;
                },
                // These render the tokens their ConfigEnum rows carry, so what is written back is by
                // construction what the reader accepts.
                [](SetTabBarVisibility const& a) {
                    return std::format(", mode: {}", config::configEnumToken(a.mode));
                },
                [](SetTabBarPosition const& a) {
                    return std::format(", position: {}", config::configEnumToken(a.position));
                },
                [](CommandBlockActionConcept auto const& a) { return formatCommandBlockArguments(a); },
                [](OpenConfiguration const& a) {
                    // The Settings window is the default, so only the editor target has an argument to write.
                    switch (a.target)
                    {
                        case ConfigurationTarget::SettingsWindow: return std::string {};
                        case ConfigurationTarget::Editor: return std::string { ", in_editor: true" };
                    }
                    return std::string {};
                },
                [](SetTabColor const& a) {
                    // A colorless SetTabColor opens the picker, so it has no argument to write. The color
                    // needs no hand-quoting: std::formatter<RGBColor> already renders it as '#RRGGBB', and
                    // unquoted a leading '#' would start a YAML COMMENT and swallow the rest of the line.
                    return a.color ? std::format(", color: {}", *a.color) : std::string {};
                },
                [](auto const&) { return std::string {}; },
            },
            action);
    }

} // namespace

std::string formatAction(Action const& action)
{
    return std::format("{}{}", name(action), formatActionArguments(action));
}

bool isNonRepeatable(Action const& action) noexcept
{
    return std::visit(core::Overloaded {
                          [](NonRepeatableActionConcept auto const&) { return true; },
                          [](auto const&) { return false; },
                      },
                      action);
}

std::vector<Action> filterRepeatableActions(std::vector<Action> const& actions)
{
    std::vector<Action> result;
    result.reserve(actions.size());
    for (auto const& action: actions)
        if (!isNonRepeatable(action))
            result.push_back(action);
    return result;
}

bool isParameterized(Action const& action) noexcept
{
    return std::visit(core::Overloaded {
                          [](ParameterizedActionConcept auto const&) { return true; },
                          [](auto const&) { return false; },
                      },
                      action);
}

span<ActionCatalogEntry const> actionCatalog()
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
        ActionCatalogEntry { "NewWindowTo", Action { NewWindowTo {} }, documentation::NewWindowTo },
        ActionCatalogEntry {
            "NoSearchHighlight", Action { NoSearchHighlight {} }, documentation::NoSearchHighlight },
        ActionCatalogEntry {
            "OpenCommandPalette", Action { OpenCommandPalette {} }, documentation::OpenCommandPalette },
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
        ActionCatalogEntry {
            "CollapseAllFolds", Action { CollapseAllFolds {} }, documentation::CollapseAllFolds },
        ActionCatalogEntry {
            "CollapseLastFold", Action { CollapseLastFold {} }, documentation::CollapseLastFold },
        ActionCatalogEntry { "ExpandAllFolds", Action { ExpandAllFolds {} }, documentation::ExpandAllFolds },
        ActionCatalogEntry { "ScrollDown", Action { ScrollDown {} }, documentation::ScrollDown },
        ActionCatalogEntry { "ScrollMarkDown", Action { ScrollMarkDown {} }, documentation::ScrollMarkDown },
        ActionCatalogEntry { "ScrollMarkUp", Action { ScrollMarkUp {} }, documentation::ScrollMarkUp },
        ActionCatalogEntry { "ScrollOneDown", Action { ScrollOneDown {} }, documentation::ScrollOneDown },
        ActionCatalogEntry { "ScrollOneUp", Action { ScrollOneUp {} }, documentation::ScrollOneUp },
        ActionCatalogEntry { "ScrollPageDown", Action { ScrollPageDown {} }, documentation::ScrollPageDown },
        ActionCatalogEntry { "ScrollPageUp", Action { ScrollPageUp {} }, documentation::ScrollPageUp },
        ActionCatalogEntry { "ScrollToBottom", Action { ScrollToBottom {} }, documentation::ScrollToBottom },
        ActionCatalogEntry { "ScrollToTop", Action { ScrollToTop {} }, documentation::ScrollToTop },
        ActionCatalogEntry { "ToggleFold", Action { ToggleFold {} }, documentation::ToggleFold },
        ActionCatalogEntry { "ToggleFoldAt", Action { ToggleFoldAt {} }, documentation::ToggleFoldAt },
        ActionCatalogEntry { "ToggleLastFold", Action { ToggleLastFold {} }, documentation::ToggleLastFold },
        ActionCatalogEntry {
            "FocusPreviousCommand", Action { FocusPreviousCommand {} }, documentation::FocusPreviousCommand },
        ActionCatalogEntry {
            "FocusNextCommand", Action { FocusNextCommand {} }, documentation::FocusNextCommand },
        ActionCatalogEntry { "SelectPreviousCommandOutput",
                             Action { SelectPreviousCommandOutput {} },
                             documentation::SelectPreviousCommandOutput },
        ActionCatalogEntry { "SelectNextCommandOutput",
                             Action { SelectNextCommandOutput {} },
                             documentation::SelectNextCommandOutput },
        ActionCatalogEntry { "OpenBlockMenu", Action { OpenBlockMenu {} }, documentation::OpenBlockMenu },
        ActionCatalogEntry { "ClearCommandBlockFocus",
                             Action { ClearCommandBlockFocus {} },
                             documentation::ClearCommandBlockFocus },
        ActionCatalogEntry { "ScrollUp", Action { ScrollUp {} }, documentation::ScrollUp },
        ActionCatalogEntry { "SearchReverse", Action { SearchReverse {} }, documentation::SearchReverse },
        ActionCatalogEntry {
            "SearchSelection", Action { SearchSelection {} }, documentation::SearchSelection },
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
        ActionCatalogEntry { "AttachToHost", Action { AttachToHost {} }, documentation::AttachToHost },
        ActionCatalogEntry {
            "OpenContextMenu", Action { OpenContextMenu {} }, documentation::OpenContextMenu },
        ActionCatalogEntry { "SelectAll", Action { SelectAll {} }, documentation::SelectAll },
        ActionCatalogEntry { "SoftReset", Action { SoftReset {} }, documentation::SoftReset },
        ActionCatalogEntry { "CopyLastCommandPrompt",
                             Action { CopyLastCommandPrompt {} },
                             documentation::CopyLastCommandPrompt },
        ActionCatalogEntry { "CopyLastCommandOutput",
                             Action { CopyLastCommandOutput {} },
                             documentation::CopyLastCommandOutput },
        ActionCatalogEntry {
            "CopyLastCommandBlock", Action { CopyLastCommandBlock {} }, documentation::CopyLastCommandBlock },
        ActionCatalogEntry { "CopyHyperlink", Action { CopyHyperlink {} }, documentation::CopyHyperlink },
        ActionCatalogEntry { "SetTabColor", Action { SetTabColor {} }, documentation::SetTabColor },
        ActionCatalogEntry { "ResetTabColor", Action { ResetTabColor {} }, documentation::ResetTabColor },
        ActionCatalogEntry { "CloseAllTabs", Action { CloseAllTabs {} }, documentation::CloseAllTabs },
        ActionCatalogEntry {
            "SetTabBarVisibility", Action { SetTabBarVisibility {} }, documentation::SetTabBarVisibility },
        ActionCatalogEntry {
            "SetTabBarPosition", Action { SetTabBarPosition {} }, documentation::SetTabBarPosition },
        ActionCatalogEntry { "SpeakSelection", Action { SpeakSelection {} }, documentation::SpeakSelection },
        ActionCatalogEntry { "StopSpeaking", Action { StopSpeaking {} }, documentation::StopSpeaking },
        ActionCatalogEntry {
            "SelectCommandBlock", Action { SelectCommandBlock {} }, documentation::SelectCommandBlock },
        ActionCatalogEntry {
            "CopyCommandBlock", Action { CopyCommandBlock {} }, documentation::CopyCommandBlock },
        ActionCatalogEntry {
            "CopyCommandLine", Action { CopyCommandLine {} }, documentation::CopyCommandLine },
        ActionCatalogEntry { "RerunCommand", Action { RerunCommand {} }, documentation::RerunCommand },
        ActionCatalogEntry {
            "ToggleCommandFold", Action { ToggleCommandFold {} }, documentation::ToggleCommandFold },
        ActionCatalogEntry { "ClearToPrompt", Action { ClearToPrompt {} }, documentation::ClearToPrompt },
        ActionCatalogEntry {
            "OpenCommandOutput", Action { OpenCommandOutput {} }, documentation::OpenCommandOutput },
        ActionCatalogEntry { "CloseOtherTabs", Action { CloseOtherTabs {} }, documentation::CloseOtherTabs },
        ActionCatalogEntry {
            "CloseTabsToRight", Action { CloseTabsToRight {} }, documentation::CloseTabsToRight },
        ActionCatalogEntry {
            "MoveTabToNewWindow", Action { MoveTabToNewWindow {} }, documentation::MoveTabToNewWindow },
        ActionCatalogEntry { "ShowAbout", Action { ShowAbout {} }, documentation::ShowAbout },
        ActionCatalogEntry { "MinimizeWindow", Action { MinimizeWindow {} }, documentation::MinimizeWindow },
        ActionCatalogEntry { "ZoomWindow", Action { ZoomWindow {} }, documentation::ZoomWindow },
        ActionCatalogEntry { "FocusNextArea", Action { FocusNextArea {} }, documentation::FocusNextArea },
        ActionCatalogEntry {
            "FocusPreviousArea", Action { FocusPreviousArea {} }, documentation::FocusPreviousArea },
        ActionCatalogEntry { "FocusToolbar", Action { FocusToolbar {} }, documentation::FocusToolbar },
    };
    static_assert(std::tuple_size_v<std::remove_cvref_t<decltype(catalog)>> == std::variant_size_v<Action>,
                  "every Action alternative must have exactly one actionCatalog() row: without it the "
                  "action cannot be named in input_mapping, is missing from the generated docs, and never "
                  "shows up in the command palette");
    return catalog;
}

} // namespace contour::actions
