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
    return fmt::format("{}", static_cast<unsigned>(color));
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

string to_string(RGBColor const c)
{
    char buf[16];
    auto n = snprintf(buf, sizeof(buf), "%u.%u.%u", c.red, c.green, c.blue);
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

}  // namespace terminal
