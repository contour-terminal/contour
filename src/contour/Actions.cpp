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
#include "Actions.h"

#include <crispy/utils.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <iterator>
#include <string>
#include <string_view>

using namespace std;

using crispy::toLower;

namespace contour::actions
{

namespace
{
    template <typename T>
    inline auto mapAction(string_view _name)
    {
        return pair { _name, Action { T {} } };
    }
} // namespace

optional<Action> fromString(string const& _name)
{
    // NB: If we change that variable declaration to `static`,
    // then MSVC will not finish compiling. Yes. That's not a joke.
    auto const mappings = array {
        mapAction<actions::CancelSelection>("CancelSelection"),
        mapAction<actions::ChangeProfile>("ChangeProfile"),
        mapAction<actions::ClearHistoryAndReset>("ClearHistoryAndReset"),
        mapAction<actions::CopyPreviousMarkRange>("CopyPreviousMarkRange"),
        mapAction<actions::CopySelection>("CopySelection"),
        mapAction<actions::DecreaseFontSize>("DecreaseFontSize"),
        mapAction<actions::DecreaseOpacity>("DecreaseOpacity"),
        mapAction<actions::FollowHyperlink>("FollowHyperlink"),
        mapAction<actions::IncreaseFontSize>("IncreaseFontSize"),
        mapAction<actions::IncreaseOpacity>("IncreaseOpacity"),
        mapAction<actions::NewTerminal>("NewTerminal"),
        mapAction<actions::OpenConfiguration>("OpenConfiguration"),
        mapAction<actions::OpenFileManager>("OpenFileManager"),
        mapAction<actions::PasteClipboard>("PasteClipboard"),
        mapAction<actions::PasteSelection>("PasteSelection"),
        mapAction<actions::Quit>("Quit"),
        mapAction<actions::ReloadConfig>("ReloadConfig"),
        mapAction<actions::ResetConfig>("ResetConfig"),
        mapAction<actions::ResetFontSize>("ResetFontSize"),
        mapAction<actions::ScreenshotVT>("ScreenshotVT"),
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
        mapAction<actions::SendChars>("SendChars"),
        mapAction<actions::ToggleAllKeyMaps>("ToggleAllKeyMaps"),
        mapAction<actions::ToggleFullscreen>("ToggleFullscreen"),
        mapAction<actions::ToggleTitleBar>("ToggleTitleBar"),
        mapAction<actions::ViNormalMode>("ViNormalMode"),
        mapAction<actions::WriteScreen>("WriteScreen"),
    };

    auto const name = toLower(_name);
    for (auto const& mapping: mappings)
        if (name == toLower(mapping.first))
            return { mapping.second };

    return nullopt;
}

} // namespace contour::actions
