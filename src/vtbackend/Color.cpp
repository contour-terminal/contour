// SPDX-License-Identifier: Apache-2.0
#include <vtbackend/Color.h>

#include <crispy/overloaded.h>
#include <crispy/utils.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <ranges>
#include <vector>

using namespace std;
using namespace std::string_view_literals;

namespace vtbackend
{

string to_string(Color color)
{
    using Type = ColorType;
    switch (color.type())
    {
        case Type::Indexed: return std::format("{}", color.index());
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
        case Type::RGB: return std::format("'{}'", formatColor(color.rgb()));
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
    return std::format("IndexedColor:{}", static_cast<unsigned>(color));
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
    return std::format("BrightColor:{}", static_cast<unsigned>(color));
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

string formatColor(RGBColor color)
{
    return std::format("#{:02X}{:02X}{:02X}", color.red, color.green, color.blue);
}

string to_string(RGBColor c)
{
    // The quoted spelling of formatColor(), not a second copy of it: this is what the config emitter and
    // the logs print, and parseColor() is documented as the inverse of formatColor() — a hex format that
    // lived in two places would let those two drift apart silently.
    return std::format("'{}'", formatColor(c));
}

string to_string(RGBAColor c)
{
    return std::format("'#{:02X}{:02X}{:02X}{:02X}'", c.red(), c.green(), c.blue(), c.alpha());
}

namespace
{
    /// Number of channels in a colour specification: red, green and blue.
    constexpr auto ColorChannelCount = size_t { 3 };

    /// Splits @p text at '/' and parses each of the three resulting channels with @p parseChannel.
    ///
    /// @param text          The channel list, e.g. "f0f0/f0f0/f0f0" (the prefix already stripped).
    /// @param parseChannel  Maps one channel's text to its 8-bit value, or std::nullopt if malformed.
    /// @return The colour, or std::nullopt unless exactly three well-formed channels were given.
    template <typename ParseChannel>
    [[nodiscard]] optional<RGBColor> parseChannelTriple(string_view text, ParseChannel parseChannel)
    {
        auto const channels = std::ranges::to<std::vector>(
            text | std::views::split('/')
            | std::views::transform([](auto&& chars) { return string_view { chars }; }));
        if (channels.size() != ColorChannelCount)
            return std::nullopt;

        auto const values = std::ranges::to<std::vector>(channels | std::views::transform(parseChannel));
        if (!std::ranges::all_of(values, [](auto const& value) { return value.has_value(); }))
            return std::nullopt;

        return RGBColor { *values[0], *values[1], *values[2] };
    }

    /// Parses one channel of an @c rgb: specification: one to four hexadecimal digits.
    ///
    /// X11 reads the digit count as the channel's precision — @c h is scaled in 4 bits, @c hh in 8,
    /// @c hhh in 12 and @c hhhh in 16 — so widening to a full 16-bit channel means
    /// @c value*0xFFFF/(16^digits - 1). We keep the high byte of that, which is both Contour's channel
    /// width and what xterm reports back on a query.
    [[nodiscard]] constexpr optional<uint8_t> parseHexChannel(string_view text) noexcept
    {
        if (text.empty() || text.size() > 4)
            return std::nullopt;

        auto const fullScale = (uint32_t { 1 } << (4 * text.size())) - 1;
        return crispy::to_integer<16, uint32_t>(text).transform([fullScale](uint32_t value) {
            return static_cast<uint8_t>(((value * 0xFFFFu) / fullScale) >> 8);
        });
    }

    /// Number of fractional digits an @c rgbi: intensity is read to; further digits cannot move an
    /// 8-bit channel and would only risk overflowing the accumulator.
    constexpr auto MaxIntensityFractionDigits = size_t { 9 };

    /// Parses one channel of an @c rgbi: specification: a decimal intensity in [0.0, 1.0].
    ///
    /// Hand-rolled rather than delegated to strtod(): that would honour the C locale's decimal point,
    /// making a terminal under a German locale reject the very same escape sequence it accepts under
    /// the C locale.
    [[nodiscard]] optional<uint8_t> parseIntensityChannel(string_view text)
    {
        auto const separator = text.find('.');
        auto const integerText = text.substr(0, separator);
        auto const fractionText = separator == string_view::npos
                                      ? string_view {}
                                      : text.substr(separator + 1).substr(0, MaxIntensityFractionDigits);

        if (integerText.empty() && fractionText.empty())
            return std::nullopt;

        auto intensity = 0.0;

        if (!integerText.empty())
        {
            auto const digits = crispy::to_integer<10, uint32_t>(integerText);
            if (!digits.has_value())
                return std::nullopt;
            intensity = static_cast<double>(*digits);
        }

        if (!fractionText.empty())
        {
            auto const digits = crispy::to_integer<10, uint32_t>(fractionText);
            if (!digits.has_value())
                return std::nullopt;
            intensity +=
                static_cast<double>(*digits) / std::pow(10.0, static_cast<double>(fractionText.size()));
        }

        if (!(0.0 <= intensity && intensity <= 1.0))
            return std::nullopt;

        return static_cast<uint8_t>(std::lround(intensity * 255.0));
    }

    /// One accepted width of the "old style" @c \# colour specification.
    struct OldStyleFormat
    {
        size_t digitCount;       ///< Total number of hexadecimal digits following the '#'.
        size_t digitsPerChannel; ///< Digits each of the three channels is given with.
    };

    /// The four widths XParseColor(3) accepts for the "old style" syntax.
    constexpr auto OldStyleFormats = std::array {
        OldStyleFormat { .digitCount = 3, .digitsPerChannel = 1 },
        OldStyleFormat { .digitCount = 6, .digitsPerChannel = 2 },
        OldStyleFormat { .digitCount = 9, .digitsPerChannel = 3 },
        OldStyleFormat { .digitCount = 12, .digitsPerChannel = 4 },
    };

    /// Parses the "old style" @c \# specification, whose digits are given @p text with the '#' stripped.
    ///
    /// Unlike @c rgb:, these digits are left-justified in a 16-bit channel and zero-filled rather than
    /// rescaled, which is why @c \#fff is 0xF000 (not white) while @c rgb:f/f/f is 0xFFFF (white).
    [[nodiscard]] optional<RGBColor> parseOldStyleColor(string_view text)
    {
        // `auto const`, not `auto const*`: libstdc++'s array iterator is a raw pointer but MSVC's is a
        // class type. @see setDynamicColorCommand in vtbackend/primitives.h.
        auto const* const format =
            std::ranges::find(OldStyleFormats, text.size(), &OldStyleFormat::digitCount);
        if (format == OldStyleFormats.end())
            return std::nullopt;

        auto const width = format->digitsPerChannel;
        auto const parseChannel = [text, width](size_t channel) {
            return crispy::to_integer<16, uint32_t>(text.substr(channel * width, width))
                .transform([width](uint32_t value) {
                    return static_cast<uint8_t>((value << (16 - (4 * width))) >> 8);
                });
        };

        auto const values = std::ranges::to<std::vector>(std::views::iota(size_t { 0 }, ColorChannelCount)
                                                         | std::views::transform(parseChannel));
        if (!std::ranges::all_of(values, [](auto const& value) { return value.has_value(); }))
            return std::nullopt;

        return RGBColor { *values[0], *values[1], *values[2] };
    }
} // namespace

optional<RGBColor> parseColor(string_view const& value)
{
    if (value.starts_with("rgb:"sv))
        return parseChannelTriple(value.substr(4), parseHexChannel);

    if (value.starts_with("rgbi:"sv))
        return parseChannelTriple(value.substr(5), parseIntensityChannel);

    if (value.starts_with("#"sv))
        return parseOldStyleColor(value.substr(1));

    return std::nullopt;
}
} // namespace vtbackend
