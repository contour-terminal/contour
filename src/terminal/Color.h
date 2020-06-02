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

#include <fmt/format.h>

#include <array>
#include <algorithm>
#include <cstdint>
#include <initializer_list>
#include <ostream>
#include <string>
#include <variant>
#include <utility>

namespace terminal {

struct UndefinedColor {};

constexpr bool operator==(UndefinedColor, UndefinedColor) noexcept { return true; }
constexpr bool operator!=(UndefinedColor, UndefinedColor) noexcept { return false; }

struct DefaultColor {};

constexpr bool operator==(DefaultColor, DefaultColor) noexcept { return true; }
constexpr bool operator!=(DefaultColor, DefaultColor) noexcept { return false; }

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

    RGBColor& operator=(std::string const& _hexCode);
};

constexpr RGBColor operator*(RGBColor const& c, float s) noexcept
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

constexpr bool operator==(RGBColor const& a, RGBColor const& b) noexcept
{
    return a.red == b.red && a.green == b.green && a.blue == b.blue;
}

constexpr bool operator!=(RGBColor const& a, RGBColor const& b) noexcept
{
    return !(a == b);
}

using Color = std::variant<UndefinedColor, DefaultColor, IndexedColor, BrightColor, RGBColor>;

struct ColorProfile {
    using Palette = std::array<RGBColor, 256>;

    RGBColor const& normalColor(size_t _index) const noexcept {
        assert(_index < 8);
        return palette.at(_index);
    }

    RGBColor const& brightColor(size_t _index) const noexcept {
        assert(_index < 8);
        return palette.at(_index + 8);
    }

    RGBColor const& dimColor(size_t _index) const {
        assert(_index < 8);
        return palette.at(_index); // TODO
    }

    RGBColor const& indexedColor(size_t _index) const noexcept {
        assert(_index < 256);
        return palette.at(_index);
    }

    RGBColor defaultForeground = 0xD0D0D0;
    RGBColor defaultBackground = 0x000000;
    RGBColor selection = 0x707070;
    float selectionOpacity = 1.0f;
	RGBColor cursor = 0x707020;

    RGBColor mouseForeground = 0x800000;
    RGBColor mouseBackground = 0x808000;

    Palette palette = []() {
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
        colors[8] = 0x808080_rgb;  // bright black (dark gray)
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
        for (uint8_t gray = 0, level = gray * 10 + 8; gray < 24; ++gray, level = gray * 10 + 8)
            colors[232 + gray] = RGBColor{level, level, level};

        return colors;
    }();
};

enum class ColorTarget {
    Foreground,
    Background,
};

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

RGBColor const& apply(ColorProfile const& _colorProfile, Color const& _color, ColorTarget _target, bool _bright) noexcept;

constexpr bool operator==(Color const& a, Color const& b) noexcept
{
    if (a.index() != b.index())
        return false;

    if (std::holds_alternative<IndexedColor>(a))
        return std::get<IndexedColor>(a) == std::get<IndexedColor>(b);

    if (std::holds_alternative<BrightColor>(a))
        return std::get<BrightColor>(a) == std::get<BrightColor>(b);

    if (std::holds_alternative<UndefinedColor>(a))
        return true;

    if (std::holds_alternative<DefaultColor>(a))
        return true;

    /*static_*/assert(std::holds_alternative<RGBColor>(a));
    return std::get<RGBColor>(a) == std::get<RGBColor>(b);
}

std::string to_string(IndexedColor color);
std::string to_string(BrightColor color);
std::string to_string(RGBColor c);
std::string to_string(Color const& c);

constexpr bool isUndefined(Color const& color) noexcept
{
    return std::holds_alternative<UndefinedColor>(color);
}

constexpr bool isDefault(Color const& color) noexcept
{
    return std::holds_alternative<DefaultColor>(color);
}

constexpr bool isIndexed(Color const& color) noexcept
{
    return std::holds_alternative<IndexedColor>(color);
}

constexpr bool isRGB(Color const& color) noexcept
{
    return std::holds_alternative<RGBColor>(color);
}

inline std::ostream& operator<<(std::ostream& os, terminal::IndexedColor value)
{
    return os << to_string(value);
}

inline std::ostream& operator<<(std::ostream& os, terminal::BrightColor value)
{
    return os << to_string(value);
}

inline std::ostream& operator<<(std::ostream& os, terminal::RGBColor value)
{
    return os << to_string(value);
}

inline std::ostream& operator<<(std::ostream& os, terminal::Color value)
{
    return os << to_string(value);
}

}  // namespace terminal

namespace fmt {
    template <>
    struct formatter<terminal::IndexedColor> {
        template <typename ParseContext>
        constexpr auto parse(ParseContext& ctx) { return ctx.begin(); }
        template <typename FormatContext>
        auto format(terminal::IndexedColor value, FormatContext& ctx) { return format_to(ctx.out(), to_string(value)); }
    };

    template <>
    struct formatter<terminal::BrightColor> {
        template <typename ParseContext>
        constexpr auto parse(ParseContext& ctx) { return ctx.begin(); }
        template <typename FormatContext>
        auto format(terminal::BrightColor value, FormatContext& ctx) { return format_to(ctx.out(), to_string(value)); }
    };

    template <>
    struct formatter<terminal::RGBColor> {
        template <typename ParseContext>
        constexpr auto parse(ParseContext& ctx) { return ctx.begin(); }
        template <typename FormatContext>
        auto format(terminal::RGBColor const& value, FormatContext& ctx) { return format_to(ctx.out(), to_string(value)); }
    };

    template <>
    struct formatter<terminal::Color> {
        template <typename ParseContext>
        constexpr auto parse(ParseContext& ctx) { return ctx.begin(); }
        template <typename FormatContext>
        auto format(terminal::Color const& value, FormatContext& ctx) { return format_to(ctx.out(), to_string(value)); }
    };
}
