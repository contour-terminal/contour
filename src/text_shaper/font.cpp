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
#include <text_shaper/font.h>
#include <fmt/format.h>

using std::string;
using std::string_view;

namespace text {

string font_description::toPattern() const
{
    string m;
    if (weight != font_weight::normal)
        m = fmt::format(" {}", weight);
    if (slant != font_slant::normal)
        m = fmt::format(" {}", slant);
    return fmt::format("{}{}", familyName, m);
}

font_description font_description::parse(string_view _pattern)
{
    font_description fd{};

    // TODO: find proper style suffix
    // auto const i = _pattern.rfind(' ');
    // if (i != _pattern.npos)
    // {
    //     fd.familyName = _pattern.substr(0, i);
    //     fd.styleName = _pattern.substr(i + 1);
    // }
    // else
        fd.familyName = _pattern;

    return fd;
}

} // end namespace
