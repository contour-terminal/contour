// SPDX-License-Identifier: Apache-2.0
#include <contour/Actions.h>

#include <crispy/utils.h>

#include <array>
#include <string>
#include <string_view>

using namespace std;

using crispy::toLower;

namespace contour::actions
{

namespace
{
    template <typename T>
    inline constexpr auto mapAction(string_view name) noexcept
    {
        return pair { name, Action { T {} } };
    }
} // namespace

optional<Action> fromString(string const& name)
{
    // NB: If we change that variable declaration to `static`,
    // then MSVC will not finish compiling. Yes. That's not a joke.
    auto const mappings = array {
        mapAction<actions::CancelSelection>("CancelSelection"),
        mapAction<actions::ChangeProfile>("ChangeProfile"),
        mapAction<actions::ClearHistoryAndReset>("ClearHistoryAndReset"),
        mapAction<actions::CopyPreviousMarkRange>("CopyPreviousMarkRange"),
        mapAction<actions::CopySelection>("CopySelection"),
        mapAction<actions::CreateDebugDump>("CreateDebugDump"),
        mapAction<actions::CreateSelection>("CreateSelection"),
        mapAction<actions::DecreaseFontSize>("DecreaseFontSize"),
        mapAction<actions::DecreaseOpacity>("DecreaseOpacity"),
        mapAction<actions::FocusNextSearchMatch>("FocusNextSearchMatch"),
        mapAction<actions::FocusPreviousSearchMatch>("FocusPreviousSearchMatch"),
        mapAction<actions::FollowHyperlink>("FollowHyperlink"),
        mapAction<actions::IncreaseFontSize>("IncreaseFontSize"),
        mapAction<actions::IncreaseOpacity>("IncreaseOpacity"),
        mapAction<actions::NewTerminal>("NewTerminal"),
        mapAction<actions::NoSearchHighlight>("NoSearchHighlight"),
        mapAction<actions::OpenConfiguration>("OpenConfiguration"),
        mapAction<actions::OpenFileManager>("OpenFileManager"),
        mapAction<actions::OpenSelection>("OpenSelection"),
        mapAction<actions::PasteClipboard>("PasteClipboard"),
        mapAction<actions::PasteSelection>("PasteSelection"),
        mapAction<actions::Quit>("Quit"),
        mapAction<actions::ReloadConfig>("ReloadConfig"),
        mapAction<actions::ResetConfig>("ResetConfig"),
        mapAction<actions::ResetFontSize>("ResetFontSize"),
        mapAction<actions::ScreenshotVT>("ScreenshotVT"),
        mapAction<actions::SaveScreenshot>("SaveScreenshot"),
        mapAction<actions::CopyScreenshot>("CopyScreenshot"),
        mapAction<actions::ScrollDown>("ScrollDown"),
        mapAction<actions::ScrollMarkDown>("ScrollMarkDown"),
        mapAction<actions::ScrollMarkUp>("ScrollMarkUp"),
        mapAction<actions::ScrollOneDown>("ScrollOneDown"),
        mapAction<actions::ScrollOneUp>("ScrollOneUp"),
        mapAction<actions::ScrollPageDown>("ScrollPageDown"),
        mapAction<actions::ScrollPageUp>("ScrollPageUp"),
        mapAction<actions::ScrollToBottom>("ScrollToBottom"),
        mapAction<actions::ScrollToTop>("ScrollToTop"),
        mapAction<actions::ScrollUp>("ScrollUp"),
        mapAction<actions::SearchReverse>("SearchReverse"),
        mapAction<actions::SendChars>("SendChars"),
        mapAction<actions::ToggleAllKeyMaps>("ToggleAllKeyMaps"),
        mapAction<actions::ToggleFullscreen>("ToggleFullscreen"),
        mapAction<actions::ToggleInputProtection>("ToggleInputProtection"),
        mapAction<actions::ToggleStatusLine>("ToggleStatusLine"),
        mapAction<actions::ToggleTitleBar>("ToggleTitleBar"),
        mapAction<actions::TraceBreakAtEmptyQueue>("TraceBreakAtEmptyQueue"),
        mapAction<actions::TraceEnter>("TraceEnter"),
        mapAction<actions::TraceLeave>("TraceLeave"),
        mapAction<actions::TraceStep>("TraceStep"),
        mapAction<actions::ViNormalMode>("ViNormalMode"),
        mapAction<actions::WriteScreen>("WriteScreen"),
        mapAction<actions::CreateNewTab>("CreateNewTab"),
        mapAction<actions::CloseTab>("CloseTab"),
        mapAction<actions::MoveTabTo>("MoveTabTo"),
        mapAction<actions::MoveTabToLeft>("MoveTabToLeft"),
        mapAction<actions::MoveTabToRight>("MoveTabToRight"),
        mapAction<actions::SwitchToTab>("SwitchToTab"),
        mapAction<actions::SwitchToPreviousTab>("SwitchToPreviousTab"),
        mapAction<actions::SwitchToTabLeft>("SwitchToTabLeft"),
        mapAction<actions::SwitchToTabRight>("SwitchToTabRight"),
        mapAction<actions::SetTabName>("SetTabName"),
    };

    auto const lowerCaseName = toLower(name);
    for (auto const& mapping: mappings)
        if (lowerCaseName == toLower(mapping.first))
            return { mapping.second };

    return nullopt;
}

} // namespace contour::actions
