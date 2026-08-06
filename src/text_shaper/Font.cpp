// SPDX-License-Identifier: Apache-2.0
#include <text_shaper/Font.hpp>

#include <format>

using std::string;
using std::string_view;

namespace text
{

string FontDescription::toPattern() const
{
    string m;
    if (weight != FontWeight::Normal)
        m = std::format(" {}", weight);
    if (slant != FontSlant::Normal)
        m = std::format(" {}", slant);
    return std::format("{}{}", familyName, m);
}

FontDescription FontDescription::parse(string_view pattern)
{
    FontDescription fd {};

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
