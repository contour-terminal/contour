// SPDX-License-Identifier: Apache-2.0
#include <vtbackend/Color.h>

#include <crispy/overloaded.h>
#include <crispy/utils.h>

using namespace std;

namespace vtbackend
{

string to_string(Color color)
{
    using Type = ColorType;
    switch (color.type())
    {
        case Type::Indexed: return fmt::format("{}", color.index());
        case Type::Bright:
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
                default: return "?";
            }
        case Type::Default:
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
                default: return "?";
            }
        case Type::RGB:
            return fmt::format("#{:02X}{:02X}{:02X}", color.rgb().red, color.rgb().green, color.rgb().blue);
        case Type::Undefined: break;
    }
    return "?";
}

string to_string(IndexedColor color)
{
    switch (color)
    {
        case IndexedColor::Black: return "black";
        case IndexedColor::Red: return "red";
        case IndexedColor::Green: return "green";
        case IndexedColor::Yellow: return "yellow";
        case IndexedColor::Blue: return "blue";
        case IndexedColor::Magenta: return "magenta";
        case IndexedColor::Cyan: return "cyan";
        case IndexedColor::White: return "white";
        case IndexedColor::Default: return "DEFAULT";
    }
    return fmt::format("IndexedColor:{}", static_cast<unsigned>(color));
}

string to_string(BrightColor color)
{
    switch (color)
    {
        case BrightColor::Black: return "bright-black";
        case BrightColor::Red: return "bright-red";
        case BrightColor::Green: return "bright-Green";
        case BrightColor::Yellow: return "bright-Yellow";
        case BrightColor::Blue: return "bright-blue";
        case BrightColor::Magenta: return "bright-magenta";
        case BrightColor::Cyan: return "bright-cyan";
        case BrightColor::White: return "bright-white";
    }
    return fmt::format("BrightColor:{}", static_cast<unsigned>(color));
}

RGBColor::RGBColor(std::string const& hexCode): RGBColor()
{
    *this = hexCode;
}

RGBColor& RGBColor::operator=(string const& hexCode)
{
    if (hexCode.size() == 7 && hexCode[0] == '#')
    {
        char* eptr = nullptr;
        auto const value = static_cast<uint32_t>(strtoul(hexCode.c_str() + 1, &eptr, 16));
        if (eptr && *eptr == '\0')
            *this = RGBColor { value };
    }
    if (hexCode.size() >= 3 && hexCode[0] == '0' && hexCode[1] == 'x')
    {
        char* eptr = nullptr;
        auto const value = static_cast<uint32_t>(strtoul(hexCode.c_str() + 2, &eptr, 16));
        if (eptr && *eptr == '\0')
            *this = RGBColor { value };
    }
    return *this;
}

RGBAColor& RGBAColor::operator=(string const& hexCode)
{
    if (hexCode.size() == 9 && hexCode[0] == '#')
    {
        char* eptr = nullptr;
        auto const value = static_cast<uint32_t>(strtoul(hexCode.c_str() + 1, &eptr, 16));
        if (eptr && *eptr == '\0')
            *this = RGBAColor { value };
    }
    return *this;
}

string to_string(RGBColor c)
{
    return fmt::format("#{:02X}{:02X}{:02X}", c.red, c.green, c.blue);
}

string to_string(RGBAColor c)
{
    return fmt::format("#{:02X}{:02X}{:02X}{:02X}", c.red(), c.green(), c.blue(), c.alpha());
}

optional<RGBColor> parseColor(string_view const& value)
{
    try
    {
        // "rgb:RR/GG/BB"
        //  0123456789a
        if (value.size() == 12 && value.substr(0, 4) == "rgb:" && value[6] == '/' && value[9] == '/')
        {
            auto const r = crispy::to_integer<16, uint8_t>(value.substr(4, 2));
            auto const g = crispy::to_integer<16, uint8_t>(value.substr(7, 2));
            auto const b = crispy::to_integer<16, uint8_t>(value.substr(10, 2));
            return RGBColor { r.value(), g.value(), b.value() };
        }

        // "#RRGGBB"
        if (value.size() == 7 && value[0] == '#')
        {
            auto const r = crispy::to_integer<16, uint8_t>(value.substr(1, 2));
            auto const g = crispy::to_integer<16, uint8_t>(value.substr(3, 2));
            auto const b = crispy::to_integer<16, uint8_t>(value.substr(5, 2));
            return RGBColor { r.value(), g.value(), b.value() };
        }

        // "#RGB"
        if (value.size() == 4 && value[0] == '#')
        {
            auto const r = crispy::to_integer<16, uint8_t>(value.substr(1, 1));
            auto const g = crispy::to_integer<16, uint8_t>(value.substr(2, 1));
            auto const b = crispy::to_integer<16, uint8_t>(value.substr(3, 1));
            auto const rr = static_cast<uint8_t>(r.value() << 4);
            auto const gg = static_cast<uint8_t>(g.value() << 4);
            auto const bb = static_cast<uint8_t>(b.value() << 4);
            return RGBColor { rr, gg, bb };
        }

        return std::nullopt;
    }
    catch (...)
    {
        // that will be a formatting error in stoul() then.
        return std::nullopt;
    }
}
} // namespace vtbackend
