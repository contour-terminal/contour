/**
 * This file is part of the "libterminal" project
 *   Copyright (c) 2019 Christian Parpart <christian@parpart.family>
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
#include <terminal/Color.h>
#include <terminal/Util.h>
#include <ground/overloaded.h>
#include <cstdio>

using namespace std;

namespace terminal {

string to_string(IndexedColor color)
{
    switch (color)
    {
        case IndexedColor::Black:
            return "Black";
        case IndexedColor::Red:
            return "Red";
        case IndexedColor::Green:
            return "Green";
        case IndexedColor::Yellow:
            return "Yellow";
        case IndexedColor::Blue:
            return "Blue";
        case IndexedColor::Magenta:
            return "Magenta";
        case IndexedColor::Cyan:
            return "Cyan";
        case IndexedColor::White:
            return "White";
        case IndexedColor::Default:
            return "DEFAULT";
    }
    return fmt::format("IndexedColor:{}", static_cast<unsigned>(color));
}

string to_string(BrightColor color)
{
    switch (color)
    {
        case BrightColor::Black:
            return "BrightBlack";
        case BrightColor::Red:
            return "BrightRed";
        case BrightColor::Green:
            return "BrightGreen";
        case BrightColor::Yellow:
            return "BrightYellow";
        case BrightColor::Blue:
            return "BrightBlue";
        case BrightColor::Magenta:
            return "BrightMagenta";
        case BrightColor::Cyan:
            return "BrightCyan";
        case BrightColor::White:
            return "BrightWhite";
    }
    return fmt::format("BrightColor:{}", static_cast<unsigned>(color));
}

RGBColor& RGBColor::operator=(string const& _hexCode)
{
    if (_hexCode.size() == 7 && _hexCode[0] == '#')
    {
        char* eptr = nullptr;
        uint32_t const value = strtoul(_hexCode.c_str() + 1, &eptr, 16);
        if (eptr && *eptr == '\0')
            *this = RGBColor{value};
    }
    if (_hexCode.size() >= 3 && _hexCode[0] == '0' && _hexCode[1] == 'x')
    {
        char* eptr = nullptr;
        uint32_t const value = strtoul(_hexCode.c_str() + 2, &eptr, 16);
        if (eptr && *eptr == '\0')
            *this = RGBColor{value};
    }
    return *this;
}

string to_string(RGBColor const c)
{
    char buf[9];
    auto n = snprintf(buf, sizeof(buf), "0x%02X%02X%02X", c.red, c.green, c.blue);
    return string(buf, n);
}

string to_string(Color const& c)
{
    if (isUndefined(c))
        return "UNDEFINED";

    if (isDefault(c))
        return "DEFAULT";

    if (isIndexed(c))
        return to_string(get<IndexedColor>(c));

    if (holds_alternative<BrightColor>(c))
        return to_string(get<BrightColor>(c));

    if (isRGB(c))
        return to_string(get<RGBColor>(c));

    return "?";
}

RGBColor const& apply(ColorProfile const& _profile, Color const& _color, ColorTarget _target, bool _bright) noexcept
{
    return visit(
        overloaded{
            [&](UndefinedColor) -> RGBColor const& {
                return _target == ColorTarget::Foreground ? _profile.defaultForeground : _profile.defaultBackground;
            },
            [&](DefaultColor) -> RGBColor const& {
                return _target == ColorTarget::Foreground ? _profile.defaultForeground : _profile.defaultBackground;
            },
            [&](IndexedColor color) -> RGBColor const& {
                auto const index = static_cast<size_t>(color);
                if (_bright && index < 8)
                    return _profile.brightColor(index);
                else
                    return _profile.indexedColor(index);
            },
            [&](BrightColor color) -> RGBColor const& {
                return _profile.brightColor(static_cast<size_t>(color));
            },
            [](RGBColor const& color) -> RGBColor const& {
                return color;
            },
        },
        _color
    );
}

}  // namespace terminal
