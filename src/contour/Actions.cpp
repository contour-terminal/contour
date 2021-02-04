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

#include <algorithm>
#include <array>
#include <cctype>
#include <iterator>
#include <string>
#include <string_view>

using namespace std;

namespace contour::actions {

namespace {
    template <typename String>
    inline string toLower(String const& _value)
    {
        string result;
        result.reserve(_value.size());
        transform(
            begin(_value),
            end(_value),
            back_inserter(result),
            [](auto ch) { return tolower(ch); }
        );
        return result;
    }

    template <typename T>
    inline auto mapAction(string_view _name)
    {
        return pair{_name, Action{T{}}};
    }
}

optional<Action> fromString(string const& _name)
{
    auto static const mappings = array{
        mapAction<actions::FollowHyperlink>("FollowHyperlink"),
        mapAction<actions::ChangeProfile>("ChangeProfile"),
        mapAction<actions::CopySelection>("CopySelection"),
        mapAction<actions::DecreaseFontSize>("DecreaseFontSize"),
        mapAction<actions::DecreaseOpacity>("DecreaseOpacity"),
        mapAction<actions::IncreaseFontSize>("IncreaseFontSize"),
        mapAction<actions::IncreaseOpacity>("IncreaseOpacity"),
        mapAction<actions::NewTerminal>("NewTerminal"),
        mapAction<actions::OpenConfiguration>("OpenConfiguration"),
        mapAction<actions::OpenFileManager>("OpenFileManager"),
        mapAction<actions::PasteClipboard>("PasteClipboard"),
        mapAction<actions::PasteSelection>("PasteSelection"),
        mapAction<actions::Quit>("Quit"),
        mapAction<actions::ScreenshotVT>("ScreenshotVT"),
        mapAction<actions::ScrollDown>("ScrollDown"),
        mapAction<actions::ScrollOneDown>("ScrollOneDown"),
        mapAction<actions::ScrollOneUp>("ScrollOneUp"),
        mapAction<actions::ScrollPageDown>("ScrollPageDown"),
        mapAction<actions::ScrollPageUp>("ScrollPageUp"),
        mapAction<actions::ScrollMarkUp>("ScrollMarkUp"),
        mapAction<actions::ScrollMarkDown>("ScrollMarkDown"),
        mapAction<actions::ScrollToBottom>("ScrollToBottom"),
        mapAction<actions::ScrollToTop>("ScrollToTop"),
        mapAction<actions::ScrollUp>("ScrollUp"),
        mapAction<actions::SendChars>("SendChars"),
        mapAction<actions::ToggleFullscreen>("ToggleFullscreen"),
        mapAction<actions::WriteScreen>("WriteScreen"),
        mapAction<actions::ResetFontSize>("ResetFontSize"),
        mapAction<actions::ReloadConfig>("ReloadConfig"),
        mapAction<actions::ResetConfig>("ResetConfig"),
        mapAction<actions::CopyPreviousMarkRange>("CopyPreviousMarkRange"),
    };

    auto const name = toLower(_name);
    for (auto const& mapping : mappings)
        if (name == toLower(mapping.first))
            return {mapping.second};

    return nullopt;
}

} // end namespace
