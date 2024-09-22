// SPDX-License-Identifier: Apache-2.0
#include <text_shaper/font.h>

#include <format>

using std::string;
using std::string_view;

namespace text
{

string font_description::toPattern() const
{
    string m;
    if (weight != font_weight::normal)
        m = std::format(" {}", weight);
    if (slant != font_slant::normal)
        m = std::format(" {}", slant);
    return std::format("{}{}", familyName, m);
}

font_description font_description::parse(string_view pattern)
{
    font_description fd {};

    // TODO: find proper style suffix
    // auto const i = pattern.rfind(' ');
    // if (i != pattern.npos)
    // {
    //     fd.familyName = pattern.substr(0, i);
    //     fd.styleName = pattern.substr(i + 1);
    // }
    // else
    fd.familyName = pattern;

    return fd;
}

} // namespace text
