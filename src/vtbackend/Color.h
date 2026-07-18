// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <crispy/defines.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <format>
#include <optional>
#include <ostream>
#include <string>
#include <variant>

namespace vtbackend
{

enum class IndexedColor : uint8_t
{
    Black = 0,
    Red = 1,
    Green = 2,
    Yellow = 3,
    Blue = 4,
    Magenta = 5,
    Cyan = 6,
    White = 7,
    Default = 9,
    // TODO: 10..255
};

//! Bright colors. As introduced by aixterm, bright versions of standard 3bit colors.
enum class BrightColor : uint8_t
{
    Black = 0,
    Red = 1,
    Green = 2,
    Yellow = 3,
    Blue = 4,
    Magenta = 5,
    Cyan = 6,
    White = 7,
};

// {{{ RGBColor
struct RGBColor
{
    uint8_t red { 0 };
    uint8_t green { 0 };
    uint8_t blue { 0 };

    constexpr RGBColor() = default;
    constexpr RGBColor(uint8_t r, uint8_t g, uint8_t b): red { r }, green { g }, blue { b } {}
    constexpr explicit RGBColor(uint32_t rgb):
        red { static_cast<uint8_t>((rgb >> 16) & 0xFF) },
        green { static_cast<uint8_t>((rgb >> 8) & 0xFF) },
        blue { static_cast<uint8_t>(rgb & 0xFF) }
    {
    }

    [[nodiscard]] constexpr uint32_t value() const noexcept
    {
        return static_cast<uint32_t>((red << 16) | (green << 8) | blue);
    }

    [[nodiscard]] constexpr RGBColor inverse() const noexcept
    {
        return RGBColor { uint8_t(255 - red), uint8_t(255 - green), uint8_t(255 - blue) };
    }

    explicit RGBColor(std::string const& hexCode);

    RGBColor& operator=(std::string const& hexCode);

    constexpr auto operator<=>(RGBColor const&) const noexcept = default;
};

constexpr RGBColor operator*(RGBColor c, float s) noexcept
{
    return RGBColor { static_cast<uint8_t>(std::clamp(static_cast<float>(c.red) * s, 0.0f, 255.0f)),
                      static_cast<uint8_t>(std::clamp(static_cast<float>(c.green) * s, 0.0f, 255.0f)),
                      static_cast<uint8_t>(std::clamp(static_cast<float>(c.blue) * s, 0.0f, 255.0f)) };
}

constexpr RGBColor operator+(RGBColor a, RGBColor b) noexcept
{
    return RGBColor { static_cast<uint8_t>(std::clamp<unsigned>(a.red + b.red, 0, 255)),
                      static_cast<uint8_t>(std::clamp<unsigned>(a.green + b.green, 0, 255)),
                      static_cast<uint8_t>(std::clamp<unsigned>(a.blue + b.blue, 0, 255)) };
}

/// Linearly interpolates between two RGB colors.
/// At t=0 returns @p a, at t=1 returns @p b.
constexpr RGBColor mixColor(RGBColor const& a, RGBColor const& b, float t = 0.5) noexcept
{
    auto const lerpChannel = [](uint8_t x, uint8_t y, float f) -> uint8_t {
        return static_cast<uint8_t>(
            std::clamp(std::lerp(static_cast<float>(x), static_cast<float>(y), f), 0.0f, 255.0f));
    };
    return RGBColor { lerpChannel(a.red, b.red, t),
                      lerpChannel(a.green, b.green, t),
                      lerpChannel(a.blue, b.blue, t) };
}

inline double distance(RGBColor e1, RGBColor e2) noexcept
{
    auto const rmean = (uint32_t(e1.red) + uint32_t(e2.red)) / 2;
    auto const r = uint32_t(e1.red) - uint32_t(e2.red);
    auto const g = uint32_t(e1.green) - uint32_t(e2.green);
    auto const b = uint32_t(e1.blue) - uint32_t(e2.blue);
    return sqrt((((512 + rmean) * r * r) >> 8) + (4 * g * g) + (((767 - rmean) * b * b) >> 8));
}

constexpr RGBColor operator""_rgb(unsigned long long value)
{
    return RGBColor { static_cast<uint32_t>(value) };
}

namespace detail
{
    /// HSL hue-to-channel helper; all arguments are normalized to [0, 1].
    /// See http://en.wikipedia.org/wiki/HSL_color_space.
    constexpr double hue2rgb(double p, double q, double t) noexcept
    {
        if (t < 0)
            t += 1;
        if (t > 1)
            t -= 1;
        if (t < 1. / 6)
            return p + ((q - p) * 6 * t);
        if (t < 1. / 2)
            return q;
        if (t < 2. / 3)
            return p + ((q - p) * (2. / 3 - t) * 6);
        return p;
    }
} // namespace detail

/// Converts a normalized HSL colour to RGB.
/// @param h,s,l Hue, saturation and lightness, each normalized to [0, 1].
/// @return The converted RGB colour.
constexpr RGBColor hslToRgb(double h, double s, double l) noexcept
{
    if (s == 0)
    {
        auto const grayscale = static_cast<uint8_t>(l * 255.);
        return RGBColor { grayscale, grayscale, grayscale };
    }
    auto const q = l < 0.5 ? l * (1 + s) : l + s - (l * s);
    auto const p = (2 * l) - q;
    return RGBColor { static_cast<uint8_t>(detail::hue2rgb(p, q, h + (1. / 3)) * 255),
                      static_cast<uint8_t>(detail::hue2rgb(p, q, h) * 255),
                      static_cast<uint8_t>(detail::hue2rgb(p, q, h - (1. / 3)) * 255) };
}

/// Converts a DEC HLS colour to RGB, as used by both the Sixel and ReGIS colour introducers.
///
/// DEC places blue at hue 0 deg; the -120 deg offset rotates that onto the standard HSL wheel so both
/// graphics protocols render identical colours. The parameters arrive off the wire unclamped and are
/// saturated to their defined ranges first (converting an out-of-range double to @c uint8_t is UB).
/// @param hueDeg Hue angle in degrees (0..360).
/// @param lightnessPct Lightness as a percentage (0..100).
/// @param saturationPct Saturation as a percentage (0..100).
/// @return The converted RGB colour.
constexpr RGBColor decHlsToRgb(unsigned hueDeg, unsigned lightnessPct, unsigned saturationPct) noexcept
{
    auto const shifted = static_cast<double>(std::min(hueDeg, 360u)) - 120.0;
    auto const h = (shifted < 0 ? 360.0 + shifted : shifted) / 360.0;
    auto const s = static_cast<double>(std::min(saturationPct, 100u)) / 100.0;
    auto const l = static_cast<double>(std::min(lightnessPct, 100u)) / 100.0;
    return hslToRgb(h, s, l);
}

/// The VT340 default 16-colour palette, shared by the Sixel and ReGIS graphics subsystems.
/// (https://www.vt100.net/docs/vt3xx-gp/chapter2.html#S2.4)
constexpr inline std::array<RGBColor, 16> VT340DefaultColorPalette = {
    RGBColor { 0, 0, 0 },       //  0: black
    RGBColor { 51, 51, 204 },   //  1: blue
    RGBColor { 204, 33, 33 },   //  2: red
    RGBColor { 51, 204, 51 },   //  3: green
    RGBColor { 204, 51, 204 },  //  4: magenta
    RGBColor { 51, 204, 204 },  //  5: cyan
    RGBColor { 204, 204, 51 },  //  6: yellow
    RGBColor { 135, 135, 135 }, //  7: gray 50%
    RGBColor { 66, 66, 66 },    //  8: gray 25%
    RGBColor { 84, 84, 153 },   //  9: less saturated blue
    RGBColor { 153, 66, 66 },   // 10: less saturated red
    RGBColor { 84, 153, 84 },   // 11: less saturated green
    RGBColor { 153, 84, 153 },  // 12: less saturated magenta
    RGBColor { 84, 153, 153 },  // 13: less saturated cyan
    RGBColor { 153, 153, 84 },  // 14: less saturated yellow
    RGBColor { 204, 204, 204 }, // 15: gray 75%
};

struct RGBColorPair
{
    RGBColor foreground;
    RGBColor background;

    [[nodiscard]] bool isTooSimilar(double threshold = 0.1) const noexcept
    {
        return distance(foreground, background) <= threshold;
    }

    [[nodiscard]] RGBColorPair distinct(double threshold = 0.25) const noexcept
    {
        if (isTooSimilar(threshold))
            return { .foreground = foreground.inverse(), .background = foreground };
        else
            return *this;
    }

    [[nodiscard]] constexpr RGBColorPair constructDefaulted(std::optional<RGBColor> fgOpt,
                                                            std::optional<RGBColor> bgOpt) const noexcept
    {
        return { .foreground = fgOpt.value_or(foreground), .background = bgOpt.value_or(background) };
    }

    [[nodiscard]] constexpr RGBColorPair swapped() const noexcept
    {
        // Swap fg/bg.
        return { .foreground = background, .background = foreground };
    }

    [[nodiscard]] constexpr RGBColorPair allForeground() const noexcept
    {
        // All same color components as foreground.
        return { .foreground = foreground, .background = foreground };
    }

    [[nodiscard]] constexpr RGBColorPair allBackground() const noexcept
    {
        // All same color components as foreground.
        return { .foreground = background, .background = background };
    }
};

/// Linearly interpolates between two RGB color pairs.
/// At t=0 returns @p a, at t=1 returns @p b.
constexpr RGBColorPair mixColor(RGBColorPair const& a, RGBColorPair const& b, float t = 0.5) noexcept
{
    return RGBColorPair {
        .foreground = mixColor(a.foreground, b.foreground, t),
        .background = mixColor(a.background, b.background, t),
    };
}
// }}}

// {{{ RGBAColor
struct RGBAColor
{
    uint32_t value { 0 };

    [[nodiscard]] constexpr uint8_t red() const noexcept
    {
        return static_cast<uint8_t>((value >> 24) & 0xFF);
    }
    [[nodiscard]] constexpr uint8_t green() const noexcept
    {
        return static_cast<uint8_t>((value >> 16) & 0xFF);
    }
    [[nodiscard]] constexpr uint8_t blue() const noexcept
    {
        return static_cast<uint8_t>((value >> 8) & 0xFF);
    }
    [[nodiscard]] constexpr uint8_t alpha() const noexcept { return static_cast<uint8_t>(value & 0xFF); }

    constexpr RGBAColor() noexcept = default;
    constexpr RGBAColor(uint32_t value) noexcept: value { value } {}

    constexpr RGBAColor(uint8_t r, uint8_t g, uint8_t b, uint8_t a) noexcept:
        value { (static_cast<uint32_t>(r) << 24) | (static_cast<uint32_t>(g) << 16)
                | (static_cast<uint32_t>(b) << 8) | (a) }
    {
    }

    constexpr RGBAColor(RGBColor color) noexcept: RGBAColor { color.red, color.green, color.blue, 0xFF } {}

    constexpr RGBAColor(RGBColor color, uint8_t alpha) noexcept:
        RGBAColor { color.red, color.green, color.blue, alpha }
    {
    }

    [[nodiscard]] constexpr RGBColor rgb() const noexcept { return RGBColor(value >> 8); }

    [[nodiscard]] RGBAColor& operator=(std::string const& hexCode);

    // NOLINTNEXTLINE(readability-identifier-naming)
    constexpr static inline auto White = uint32_t(0xFF'FF'FF'FF);

    constexpr auto operator<=>(RGBAColor const&) const noexcept = default;
};
// }}}

// {{{ Color
enum class ColorType : uint8_t
{
    Undefined,
    Default,
    Bright,
    Indexed,
    RGB
};

struct CRISPY_PACKED Color
{
    // Layout:
    //
    // 31                                         0
    //  │uint8        │ uint8   │ uint8  │  uint8 │
    //  ╞═════════════╪═════════╪════════╪════════╡
    //  │type=RGB     │     RED │  GREEN │   BLUE │
    //  │type=Index   │  unused │ unused │  index │
    //  │type=Bright  │  unused │ unused │  index │
    //  │type=Default │  unused │ unused │ unused │
    //
    uint32_t content = 0;

    constexpr Color() noexcept = default;
    constexpr Color(Color const&) noexcept = default;
    constexpr Color(Color&&) noexcept = default;
    constexpr Color& operator=(Color const&) noexcept = default;
    constexpr Color& operator=(Color&&) noexcept = default;

    constexpr Color(BrightColor value) noexcept:
        content { (unsigned(value) & 0xFF) | (unsigned(ColorType::Bright) << 24) }
    {
    }
    constexpr Color(IndexedColor value) noexcept:
        content { (unsigned(value) & 0xFF) | (unsigned(ColorType::Indexed) << 24) }
    {
    }
    constexpr Color(RGBColor rgb) noexcept: content { rgb.value() | (unsigned(ColorType::RGB) << 24) } {}

    [[nodiscard]] constexpr ColorType type() const noexcept
    {
        return static_cast<ColorType>((content >> 24) & 0xFF);
    }
    [[nodiscard]] constexpr uint8_t index() const noexcept { return content & 0xFF; }
    [[nodiscard]] constexpr RGBColor rgb() const noexcept { return RGBColor(content & 0xFFFFFF); }

    // NOLINTBEGIN(readability-identifier-naming)
    [[nodiscard]] constexpr static Color Undefined() noexcept { return Color { ColorType::Undefined, 0 }; }
    [[nodiscard]] constexpr static Color Default() noexcept { return Color { ColorType::Default, 0 }; }
    [[nodiscard]] constexpr static Color Bright(uint8_t index) noexcept
    {
        return Color { ColorType::Bright, index };
    }
    [[nodiscard]] constexpr static Color Indexed(uint8_t index) noexcept
    {
        return Color { ColorType::Indexed, index };
    }
    [[nodiscard]] constexpr static Color Indexed(IndexedColor index) noexcept
    {
        return Color { ColorType::Indexed, (uint8_t) index };
    }
    // NOLINTEND(readability-identifier-naming)

    // TODO: The line below breaks on Windows, most likely because RGB is a PPD, let's find out. ;-)
    // constexpr static Color RGB(RGBColor color) noexcept { return Color{color}; }

  private:
    constexpr Color(ColorType type, uint8_t value) noexcept:
        content { (static_cast<uint32_t>(type) << 24) | (static_cast<uint32_t>(value) & 0xFF) }
    {
    }
};

constexpr bool operator==(Color a, Color b) noexcept
{
    return a.content == b.content;
}

constexpr bool operator!=(Color a, Color b) noexcept
{
    return !(a == b);
}

constexpr bool isUndefined(Color color) noexcept
{
    return color.type() == ColorType::Undefined;
}
constexpr bool isDefaultColor(Color color) noexcept
{
    return color.type() == ColorType::Default;
}

constexpr bool isIndexedColor(Color color) noexcept
{
    return color.type() == ColorType::Indexed;
}
constexpr bool isBrightColor(Color color) noexcept
{
    return color.type() == ColorType::Bright;
}
constexpr bool isRGBColor(Color color) noexcept
{
    return color.type() == ColorType::RGB;
}

constexpr uint8_t getIndexedColor(Color color) noexcept
{
    return color.index();
}
constexpr uint8_t getBrightColor(Color color) noexcept
{
    return color.index();
}
constexpr RGBColor getRGBColor(Color color) noexcept
{
    return color.rgb();
}

std::string to_string(Color color);
std::string to_string(IndexedColor color);
std::string to_string(BrightColor color);
std::string to_string(RGBColor c);
std::string to_string(RGBAColor c);

/// Renders @p color as a bare hex literal, e.g. "#FF0000" — the exact inverse of parseColor().
///
/// Deliberately NOT to_string(RGBColor), which wraps the same value in single quotes: those are what a
/// YAML emitter wants, and exactly what a value being handed back to parseColor(), or shown to a human
/// as a command-palette row, must not have.
/// @param color The color to render.
/// @return Its "#RRGGBB" form, in upper case.
[[nodiscard]] std::string formatColor(RGBColor color);

inline std::ostream& operator<<(std::ostream& os, vtbackend::Color value)
{
    return os << to_string(value);
}

constexpr Color UndefinedColor() noexcept
{
    return Color::Undefined();
}
constexpr Color DefaultColor() noexcept
{
    return Color::Default();
}
// }}}

struct CellForegroundColor
{
};
struct CellBackgroundColor
{
};
using CellRGBColor = std::variant<RGBColor, CellForegroundColor, CellBackgroundColor>;

struct CellRGBColorPair
{
    CellRGBColor foreground = CellForegroundColor {};
    CellRGBColor background = CellBackgroundColor {};
};

struct CellRGBColorAndAlphaPair
{
    CellRGBColor foreground = CellForegroundColor {};
    float foregroundAlpha = 1.0f;
    CellRGBColor background = CellBackgroundColor {};
    float backgroundAlpha = 1.0f;
};

struct CursorColor
{
    CellRGBColor color = CellForegroundColor {};
    CellRGBColor textOverrideColor = CellBackgroundColor {};
};

// {{{ Opacity
enum class Opacity : uint8_t
{
    Transparent = 0x00,
    Opaque = 0xFF
};

constexpr Opacity& operator++(Opacity& value) noexcept
{
    value = static_cast<Opacity>(std::min(static_cast<int>(value) + 15, 0xFF));
    return value;
}

constexpr Opacity& operator--(Opacity& value) noexcept
{
    value = static_cast<Opacity>(std::max(static_cast<int>(value) - 15, 0));
    return value;
}
// }}}

/// Parses an X11 colour specification, as accepted by OSC 4/5/10..19 and Contour's configuration.
///
/// Three syntaxes are recognised, mirroring XParseColor(3):
///   - @c rgb:<h>/<h>/<h> — one to four hexadecimal digits per channel, the digit count giving the
///     channel's precision (so @c rgb:f/f/f, @c rgb:ff/ff/ff and @c rgb:ffff/ffff/ffff all mean white).
///   - @c rgbi:<f>/<f>/<f> — decimal intensities in [0.0, 1.0].
///   - @c \#rgb, @c \#rrggbb, @c \#rrrgggbbb, @c \#rrrrggggbbbb — the "old style" syntax, whose digits
///     are left-justified and zero-filled rather than rescaled (so @c \#fff is @em not white).
///
/// @param value The specification to parse.
/// @return The colour, or std::nullopt if @p value is not a well-formed specification.
[[nodiscard]] std::optional<RGBColor> parseColor(std::string_view const& value);

} // namespace vtbackend

// {{{ fmtlib custom formatter
template <>
struct std::formatter<vtbackend::Color>: std::formatter<std::string>
{
    auto format(vtbackend::Color value, auto& ctx) const
    {
        return formatter<std::string>::format(to_string(value), ctx);
    }
};

template <>
struct std::formatter<vtbackend::RGBColor>: std::formatter<std::string>
{
    auto format(vtbackend::RGBColor value, auto& ctx) const
    {
        return formatter<std::string>::format(to_string(value), ctx);
    }
};

template <>
struct std::formatter<vtbackend::RGBAColor>: std::formatter<std::string>
{
    auto format(vtbackend::RGBAColor value, auto& ctx) const
    {
        return formatter<std::string>::format(to_string(value), ctx);
    }
};

template <>
struct std::formatter<vtbackend::CellRGBColor>: std::formatter<std::string>
{
    auto format(vtbackend::CellRGBColor value, auto& ctx) const
    {
        if (std::holds_alternative<vtbackend::CellForegroundColor>(value))
            return formatter<std::string>::format("CellForeground", ctx);
        else if (std::holds_alternative<vtbackend::CellBackgroundColor>(value))
            return formatter<std::string>::format("CellBackground", ctx);
        else
            return formatter<std::string>::format(to_string(std::get<vtbackend::RGBColor>(value)), ctx);
    }
};

template <>
struct std::formatter<vtbackend::RGBColorPair>: std::formatter<std::string>
{
    auto format(vtbackend::RGBColorPair value, auto& ctx) const
    {
        return formatter<std::string>::format(std::format("{}/{}", value.foreground, value.background), ctx);
    }
};
// }}}
