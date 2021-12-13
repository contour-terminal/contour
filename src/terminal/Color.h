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
#pragma once

#include <terminal/defines.h>

#include <fmt/format.h>

#include <cassert>
#include <array>
#include <algorithm>
#include <cstdint>
#include <initializer_list>
#include <optional>
#include <ostream>
#include <string>
#include <variant>
#include <utility>

namespace terminal {

enum class IndexedColor : uint8_t {
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
enum class BrightColor {
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
struct RGBColor {
    uint8_t red;
    uint8_t green;
    uint8_t blue;

    constexpr RGBColor() : red{0}, green{0}, blue{0} {}
    constexpr RGBColor(uint8_t r, uint8_t g, uint8_t b) : red{r}, green{g}, blue{b} {}
    constexpr RGBColor(uint32_t rgb) :
        red{static_cast<uint8_t>((rgb >> 16) & 0xFF)},
        green{static_cast<uint8_t>((rgb >> 8) & 0xFF)},
        blue{static_cast<uint8_t>(rgb & 0xFF)}
    {}

    constexpr uint32_t value() const noexcept
    {
        return (red << 16) | (green << 8) | blue;
    }

    explicit RGBColor(std::string const& _hexCode);

    RGBColor& operator=(std::string const& _hexCode);
};

constexpr RGBColor operator*(RGBColor c, float s) noexcept
{
    return RGBColor{
        static_cast<uint8_t>(std::clamp(static_cast<float>(c.red) * s, 0.0f, 255.0f)),
        static_cast<uint8_t>(std::clamp(static_cast<float>(c.green) * s, 0.0f, 255.0f)),
        static_cast<uint8_t>(std::clamp(static_cast<float>(c.blue) * s, 0.0f, 255.0f))
    };
}

constexpr RGBColor operator "" _rgb(unsigned long long _value)
{
    return RGBColor{static_cast<uint32_t>(_value)};
}

constexpr bool operator==(RGBColor a, RGBColor b) noexcept
{
    return a.red == b.red && a.green == b.green && a.blue == b.blue;
}

constexpr bool operator!=(RGBColor a, RGBColor b) noexcept
{
    return !(a == b);
}
// }}}

// {{{ RGBAColor
struct RGBAColor {
    uint32_t value;

    constexpr uint8_t red() const noexcept   { return static_cast<uint8_t>((value >> 24) & 0xFF); }
    constexpr uint8_t green() const noexcept { return static_cast<uint8_t>((value >> 16) & 0xFF); }
    constexpr uint8_t blue() const noexcept  { return static_cast<uint8_t>((value >> 8) & 0xFF); }
    constexpr uint8_t alpha() const noexcept { return static_cast<uint8_t>(value & 0xFF); }

    constexpr RGBAColor() : value{0} {}
    constexpr RGBAColor(uint32_t _value) : value{_value} {}

    constexpr RGBAColor(uint8_t r, uint8_t g, uint8_t b, uint8_t a) :
        value{ (static_cast<uint32_t>(r) << 24) |
               (static_cast<uint32_t>(g) << 16) |
               (static_cast<uint32_t>(b) << 8)  |
               (a) } {}

    constexpr RGBAColor(RGBColor _color) : RGBAColor{_color.red, _color.green, _color.blue, 0xFF} {}

    constexpr RGBAColor(RGBColor _color, uint8_t _alpha) :
        RGBAColor{_color.red, _color.green, _color.blue, _alpha} {}

    constexpr RGBColor rgb() const noexcept { return RGBColor(value >> 8); }

    RGBAColor& operator=(std::string const& _hexCode);
};

constexpr bool operator==(RGBAColor a, RGBAColor b) noexcept
{
    return a.value == b.value;
}

constexpr bool operator!=(RGBAColor a, RGBAColor b) noexcept
{
    return !(a == b);
}
// }}}

// {{{ Color
enum class ColorType : uint8_t { Undefined, Default, Bright, Indexed, RGB };

struct CONTOUR_PACKED Color
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

    constexpr Color(BrightColor _value) noexcept:
        content{unsigned(_value) & 0xFF | (unsigned(ColorType::Bright) << 24)} {}
    constexpr Color(IndexedColor _value) noexcept:
        content{unsigned(_value) & 0xFF | (unsigned(ColorType::Indexed) << 24)} {}
    constexpr Color(RGBColor _rgb) noexcept:
        content{_rgb.value() | (unsigned(ColorType::RGB) << 24)} {}

    constexpr ColorType type() const noexcept { return static_cast<ColorType>((content >> 24) & 0xFF); }
    constexpr uint8_t index() const noexcept { return content & 0xFF; }
    constexpr RGBColor rgb() const noexcept { return RGBColor(content & 0xFFFFFF); }

    constexpr static Color Undefined() noexcept { return Color{ColorType::Undefined, 0}; }
    constexpr static Color Default() noexcept { return Color{ColorType::Default, 0}; }
    constexpr static Color Bright(uint8_t _index) noexcept { return Color{ColorType::Bright, _index}; }
    constexpr static Color Indexed(uint8_t _index) noexcept { return Color{ColorType::Indexed, _index}; }
    constexpr static Color Indexed(IndexedColor _index) noexcept { return Color{ColorType::Indexed, (uint8_t) _index}; }
    // TODO: The line below breaks on Windows, most likely because RGB is a PPD, let's find out. ;-)
    //constexpr static Color RGB(RGBColor _color) noexcept { return Color{_color}; }

private:
    constexpr Color(ColorType _type, uint8_t _value) noexcept:
        content{(static_cast<uint32_t>(_type) << 24)
              | (static_cast<uint32_t>(_value) & 0xFF)} {}
};

constexpr bool operator==(Color a, Color b) noexcept
{
    return a.content == b.content;
}

constexpr bool operator!=(Color a, Color b) noexcept
{
    return !(a == b);
}

constexpr bool isUndefined(Color _color) noexcept { return _color.type() == ColorType::Undefined; }
constexpr bool isDefaultColor(Color _color) noexcept { return _color.type() == ColorType::Default; }

constexpr bool isIndexedColor(Color _color) noexcept { return _color.type() == ColorType::Indexed; }
constexpr bool isBrightColor(Color _color) noexcept { return _color.type() == ColorType::Bright; }
constexpr bool isRGBColor(Color _color) noexcept { return _color.type() == ColorType::RGB; }

constexpr int getIndexedColor(Color _color) noexcept { return _color.index(); }
constexpr int getBrightColor(Color _color) noexcept { return _color.index(); }
constexpr RGBColor getRGBColor(Color _color) noexcept { return _color.rgb(); }

std::string to_string(Color color);
std::string to_string(IndexedColor color);
std::string to_string(BrightColor color);
std::string to_string(RGBColor c);
std::string to_string(RGBAColor c);

inline std::ostream& operator<<(std::ostream& os, terminal::Color value) { return os << to_string(value); }

constexpr Color UndefinedColor() noexcept { return Color::Undefined(); }
constexpr Color DefaultColor() noexcept { return Color::Default(); }
// }}}

struct CellForegroundColor{};
struct CellBackgroundColor{};
using CellRGBColor = std::variant<RGBColor, CellForegroundColor, CellBackgroundColor>;

struct CursorColor
{
    CellRGBColor color = CellForegroundColor{};
    CellRGBColor textOverrideColor = CellBackgroundColor{};
};

// {{{ ColorPalette
struct ColorPalette
{
    using Palette = std::array<RGBColor, 256>;

    Palette palette = []() constexpr {
        Palette colors;

        // normal colors
        colors[0] = 0x000000_rgb; // black
        colors[1] = 0x800000_rgb; // red
        colors[2] = 0x008000_rgb; // green
        colors[3] = 0x808000_rgb; // yellow
        colors[4] = 0x000080_rgb; // blue
        colors[5] = 0x800080_rgb; // magenta
        colors[6] = 0x008080_rgb; // cyan
        colors[7] = 0xc0c0c0_rgb; // white

        // bright colors
        colors[8] = 0x707070_rgb;  // bright black (dark gray)
        colors[9] = 0xff0000_rgb;  // bright red
        colors[10] = 0x00ff00_rgb; // bright green
        colors[11] = 0xffff00_rgb; // bright yellow
        colors[12] = 0x0000ff_rgb; // bright blue
        colors[13] = 0xff00ff_rgb; // bright magenta
        colors[14] = 0x00ffff_rgb; // bright blue
        colors[15] = 0xffffff_rgb; // bright white

        // colors 16-231 are a 6x6x6 color cube
        for (unsigned red = 0; red < 6; ++red)
            for (unsigned green = 0; green < 6; ++green)
                for (unsigned blue = 0; blue < 6; ++blue)
                    colors[16 + (red * 36) + (green * 6) + blue] = RGBColor{
                        static_cast<uint8_t>(red   ? (red   * 40 + 55) : 0),
                        static_cast<uint8_t>(green ? (green * 40 + 55) : 0),
                        static_cast<uint8_t>(blue  ? (blue  * 40 + 55) : 0)
                    };

        // colors 232-255 are a grayscale ramp, intentionally leaving out black and white
        for (uint8_t gray = 0, level = uint8_t(gray * 10 + 8); gray < 24; ++gray, level = uint8_t(gray * 10 + 8))
            colors[232 + gray] = RGBColor{level, level, level};

        return colors;
    }();

    RGBColor normalColor(size_t _index) const noexcept {
        assert(_index < 8);
        return palette.at(_index);
    }

    RGBColor brightColor(size_t _index) const noexcept {
        assert(_index < 8);
        return palette.at(_index + 8);
    }

    RGBColor dimColor(size_t _index) const {
        assert(_index < 8);
        return palette.at(_index); // TODO
    }

    RGBColor indexedColor(size_t _index) const noexcept {
        assert(_index < 256);
        return palette.at(_index);
    }

    RGBColor defaultForeground = 0xD0D0D0;
    RGBColor defaultBackground = 0x000000;
    std::optional<RGBColor> selectionForeground = std::nullopt;
    std::optional<RGBColor> selectionBackground = std::nullopt;

    CursorColor cursor;

    RGBColor mouseForeground = 0x800000;
    RGBColor mouseBackground = 0x808000;

    struct {
        RGBColor normal = 0x0070F0;
        RGBColor hover = 0xFF0000;
    } hyperlinkDecoration;
};

enum class ColorTarget {
    Foreground,
    Background,
};

RGBColor apply(ColorPalette const& _profile, Color _color, ColorTarget _target, bool _bright) noexcept;

// }}}

// {{{ Opacity
enum class Opacity : uint8_t {
    Transparent = 0x00,
    Opaque = 0xFF
};

constexpr Opacity& operator++(Opacity& _value) noexcept {
    _value = static_cast<Opacity>(std::min(static_cast<int>(_value) + 15, 0xFF));
    return _value;
}

constexpr Opacity& operator--(Opacity& _value) noexcept {
    _value = static_cast<Opacity>(std::max(static_cast<int>(_value) - 15, 0));
    return _value;
}
// }}}

}  // namespace terminal

namespace fmt // {{{
{
    template <>
    struct formatter<terminal::Color> {
        template <typename ParseContext>
        constexpr auto parse(ParseContext& ctx) { return ctx.begin(); }
        template <typename FormatContext>
        auto format(terminal::Color value, FormatContext& ctx) { return format_to(ctx.out(), to_string(value)); }
    };

    template <>
    struct formatter<terminal::RGBColor> {
        template <typename ParseContext>
        constexpr auto parse(ParseContext& ctx) { return ctx.begin(); }
        template <typename FormatContext>
        auto format(terminal::RGBColor value, FormatContext& ctx) { return format_to(ctx.out(), to_string(value)); }
    };

    template <>
    struct formatter<terminal::RGBAColor> {
        template <typename ParseContext>
        constexpr auto parse(ParseContext& ctx) { return ctx.begin(); }
        template <typename FormatContext>
        auto format(terminal::RGBAColor value, FormatContext& ctx) { return format_to(ctx.out(), to_string(value)); }
    };

}
// }}}
