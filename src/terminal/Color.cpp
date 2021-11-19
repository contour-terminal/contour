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
#include <terminal/Color.h>
#include <crispy/overloaded.h>
#include <cstdio>

using namespace std;

namespace terminal {

string to_string(Color _color)
{
    using Type = ColorType;
    switch (_color.type())
    {
        case Type::Indexed:
            return fmt::format("{}", _color.index());
        case Type::Bright:
            switch (_color.index())
            {
                case 0: return "bright-black";
                case 1: return "bright-red";
                case 2: return "bright-green";
                case 3: return "bright-yellow";
                case 4: return "bright-blue";
                case 5: return "bright-magenta";
                case 6: return "bright-cyan";
                case 7: return "bright-white";
                case 8: return "bright-DEFAULT";
            }
            return "?";
        case Type::Default:
            switch (_color.index())
            {
                case 0: return "black";
                case 1: return "red";
                case 2: return "green";
                case 3: return "yellow";
                case 4: return "blue";
                case 5: return "magenta";
                case 6: return "cyan";
                case 7: return "white";
                case 8: return "DEFAULT";
            }
            return "?";
        case Type::RGB:
        {
            char buf[8];
            auto n = snprintf(buf, sizeof(buf), "#%02X%02X%02X", _color.rgb().red, _color.rgb().green, _color.rgb().blue);
            return string(buf, n);
        }
        case Type::Undefined:
            break;
    }
    return "?";
}

string to_string(IndexedColor color)
{
    switch (color)
    {
        case IndexedColor::Black:
            return "black";
        case IndexedColor::Red:
            return "red";
        case IndexedColor::Green:
            return "green";
        case IndexedColor::Yellow:
            return "yellow";
        case IndexedColor::Blue:
            return "blue";
        case IndexedColor::Magenta:
            return "magenta";
        case IndexedColor::Cyan:
            return "cyan";
        case IndexedColor::White:
            return "white";
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
            return "bright-black";
        case BrightColor::Red:
            return "bright-red";
        case BrightColor::Green:
            return "bright-Green";
        case BrightColor::Yellow:
            return "bright-Yellow";
        case BrightColor::Blue:
            return "bright-blue";
        case BrightColor::Magenta:
            return "bright-magenta";
        case BrightColor::Cyan:
            return "bright-cyan";
        case BrightColor::White:
            return "bright-white";
    }
    return fmt::format("BrightColor:{}", static_cast<unsigned>(color));
}

RGBColor::RGBColor(std::string const& _hexCode) : RGBColor()
{
    *this = _hexCode;
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

RGBAColor& RGBAColor::operator=(string const& _hexCode)
{
    if (_hexCode.size() == 9 && _hexCode[0] == '#')
    {
        char* eptr = nullptr;
        uint32_t const value = strtoul(_hexCode.c_str() + 1, &eptr, 16);
        if (eptr && *eptr == '\0')
            *this = RGBAColor{value};
    }
    return *this;
}

string to_string(RGBColor c)
{
    char buf[8];
    auto n = snprintf(buf, sizeof(buf), "#%02X%02X%02X", c.red, c.green, c.blue);
    return string(buf, n);
}

string to_string(RGBAColor c)
{
    char buf[10];
    auto n = snprintf(buf, sizeof(buf), "#%02X%02X%02X%02X", c.red(), c.green(), c.blue(), c.alpha());
    return string(buf, n);
}

RGBColor apply(ColorPalette const& _profile, Color _color, ColorTarget _target, bool _bright) noexcept
{
    switch (_color.type())
    {
        case ColorType::RGB:
            return _color.rgb();
        case ColorType::Indexed:
            {
                auto const index = static_cast<size_t>(_color.index());
                if (_bright && index < 8)
                    return _profile.brightColor(index);
                else
                    return _profile.indexedColor(index);
                break;
            }
        case ColorType::Bright:
            return _profile.brightColor(static_cast<size_t>(_color.index()));
        case ColorType::Undefined:
        case ColorType::Default:
            break;
    }
    return _target == ColorTarget::Foreground ? _profile.defaultForeground : _profile.defaultBackground;
}

}  // namespace terminal
