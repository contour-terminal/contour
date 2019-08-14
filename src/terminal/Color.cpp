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
    ssize_t n = snprintf(buf, sizeof(buf), "%u.%u.%u", c.red, c.green, c.blue);
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
