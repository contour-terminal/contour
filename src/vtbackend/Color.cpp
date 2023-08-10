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
#include <vtbackend/Color.h>

#include <crispy/overloaded.h>

#include <cstdio>

using namespace std;

namespace terminal
{

string to_string(color color)
{
    using type = color_type;
    switch (color.type())
    {
        case type::Indexed: return fmt::format("{}", color.index());
        case type::Bright:
            switch (color.index())
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
        case type::Default:
            switch (color.index())
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
        case type::RGB:
            return fmt::format("#{:02X}{:02X}{:02X}", color.rgb().red, color.rgb().green, color.rgb().blue);
        case type::Undefined: break;
    }
    return "?";
}

string to_string(indexed_color color)
{
    switch (color)
    {
        case indexed_color::Black: return "black";
        case indexed_color::Red: return "red";
        case indexed_color::Green: return "green";
        case indexed_color::Yellow: return "yellow";
        case indexed_color::Blue: return "blue";
        case indexed_color::Magenta: return "magenta";
        case indexed_color::Cyan: return "cyan";
        case indexed_color::White: return "white";
        case indexed_color::Default: return "DEFAULT";
    }
    return fmt::format("IndexedColor:{}", static_cast<unsigned>(color));
}

string to_string(bright_color color)
{
    switch (color)
    {
        case bright_color::Black: return "bright-black";
        case bright_color::Red: return "bright-red";
        case bright_color::Green: return "bright-Green";
        case bright_color::Yellow: return "bright-Yellow";
        case bright_color::Blue: return "bright-blue";
        case bright_color::Magenta: return "bright-magenta";
        case bright_color::Cyan: return "bright-cyan";
        case bright_color::White: return "bright-white";
    }
    return fmt::format("BrightColor:{}", static_cast<unsigned>(color));
}

rgb_color::rgb_color(std::string const& hexCode): rgb_color()
{
    *this = hexCode;
}

rgb_color& rgb_color::operator=(string const& hexCode)
{
    if (hexCode.size() == 7 && hexCode[0] == '#')
    {
        char* eptr = nullptr;
        auto const value = static_cast<uint32_t>(strtoul(hexCode.c_str() + 1, &eptr, 16));
        if (eptr && *eptr == '\0')
            *this = rgb_color { value };
    }
    if (hexCode.size() >= 3 && hexCode[0] == '0' && hexCode[1] == 'x')
    {
        char* eptr = nullptr;
        auto const value = static_cast<uint32_t>(strtoul(hexCode.c_str() + 2, &eptr, 16));
        if (eptr && *eptr == '\0')
            *this = rgb_color { value };
    }
    return *this;
}

rgba_color& rgba_color::operator=(string const& hexCode)
{
    if (hexCode.size() == 9 && hexCode[0] == '#')
    {
        char* eptr = nullptr;
        auto const value = static_cast<uint32_t>(strtoul(hexCode.c_str() + 1, &eptr, 16));
        if (eptr && *eptr == '\0')
            *this = rgba_color { value };
    }
    return *this;
}

string to_string(rgb_color c)
{
    return fmt::format("#{:02X}{:02X}{:02X}", c.red, c.green, c.blue);
}

string to_string(rgba_color c)
{
    return fmt::format("#{:02X}{:02X}{:02X}{:02X}", c.red(), c.green(), c.blue(), c.alpha());
}

} // namespace terminal
