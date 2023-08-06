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

#include <crispy/defines.h>

#include <fmt/format.h>

#include <algorithm>
#include <array>
#include <cassert>
#include <cmath>
#include <cstdint>
#include <initializer_list>
#include <optional>
#include <ostream>
#include <string>
#include <utility>
#include <variant>

namespace terminal
{

enum class indexed_color : uint8_t
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
enum class bright_color
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
struct rgb_color
{
    uint8_t red { 0 };
    uint8_t green { 0 };
    uint8_t blue { 0 };

    constexpr rgb_color() = default;
    constexpr rgb_color(uint8_t r, uint8_t g, uint8_t b): red { r }, green { g }, blue { b } {}
    constexpr explicit rgb_color(uint32_t rgb):
        red { static_cast<uint8_t>((rgb >> 16) & 0xFF) },
        green { static_cast<uint8_t>((rgb >> 8) & 0xFF) },
        blue { static_cast<uint8_t>(rgb & 0xFF) }
    {
    }

    [[nodiscard]] constexpr uint32_t value() const noexcept
    {
        return static_cast<uint32_t>((red << 16) | (green << 8) | blue);
    }

    [[nodiscard]] constexpr rgb_color inverse() const noexcept
    {
        return rgb_color { uint8_t(255 - red), uint8_t(255 - green), uint8_t(255 - blue) };
    }

    explicit rgb_color(std::string const& hexCode);

    rgb_color& operator=(std::string const& hexCode);
};

constexpr rgb_color operator*(rgb_color c, float s) noexcept
{
    return rgb_color { static_cast<uint8_t>(std::clamp(static_cast<float>(c.red) * s, 0.0f, 255.0f)),
                       static_cast<uint8_t>(std::clamp(static_cast<float>(c.green) * s, 0.0f, 255.0f)),
                       static_cast<uint8_t>(std::clamp(static_cast<float>(c.blue) * s, 0.0f, 255.0f)) };
}

constexpr rgb_color operator+(rgb_color a, rgb_color b) noexcept
{
    return rgb_color { static_cast<uint8_t>(std::clamp<unsigned>(a.red + b.red, 0, 255)),
                       static_cast<uint8_t>(std::clamp<unsigned>(a.green + b.green, 0, 255)),
                       static_cast<uint8_t>(std::clamp<unsigned>(a.blue + b.blue, 0, 255)) };
}

constexpr rgb_color mix(rgb_color a, rgb_color b, float t = 0.5) noexcept
{
    return a * t + b * (1.0f - t);
}

inline double distance(rgb_color e1, rgb_color e2) noexcept
{
    auto const rmean = (uint32_t(e1.red) + uint32_t(e2.red)) / 2;
    auto const r = uint32_t(e1.red) - uint32_t(e2.red);
    auto const g = uint32_t(e1.green) - uint32_t(e2.green);
    auto const b = uint32_t(e1.blue) - uint32_t(e2.blue);
    return sqrt((((512 + rmean) * r * r) >> 8) + 4 * g * g + (((767 - rmean) * b * b) >> 8));
}

constexpr rgb_color operator"" _rgb(unsigned long long value)
{
    return rgb_color { static_cast<uint32_t>(value) };
}

constexpr bool operator==(rgb_color a, rgb_color b) noexcept
{
    return a.red == b.red && a.green == b.green && a.blue == b.blue;
}

constexpr bool operator!=(rgb_color a, rgb_color b) noexcept
{
    return !(a == b);
}

struct rgb_color_pair
{
    rgb_color foreground;
    rgb_color background;

    [[nodiscard]] bool isTooSimilar(double threshold = 0.1) const noexcept
    {
        return distance(foreground, background) <= threshold;
    }

    [[nodiscard]] rgb_color_pair distinct(double threshold = 0.25) const noexcept
    {
        if (isTooSimilar(threshold))
            return { foreground.inverse(), foreground };
        else
            return *this;
    }

    [[nodiscard]] constexpr rgb_color_pair constructDefaulted(std::optional<rgb_color> fgOpt,
                                                              std::optional<rgb_color> bgOpt) const noexcept
    {
        return { fgOpt.value_or(foreground), bgOpt.value_or(background) };
    }

    [[nodiscard]] constexpr rgb_color_pair swapped() const noexcept
    {
        // Swap fg/bg.
        return { background, foreground };
    }

    [[nodiscard]] constexpr rgb_color_pair allForeground() const noexcept
    {
        // All same color components as foreground.
        return { foreground, foreground };
    }

    [[nodiscard]] constexpr rgb_color_pair allBackground() const noexcept
    {
        // All same color components as foreground.
        return { background, background };
    }
};

constexpr rgb_color_pair mix(rgb_color_pair a, rgb_color_pair b, float t = 0.5) noexcept
{
    return rgb_color_pair {
        mix(a.foreground, b.foreground, t),
        mix(a.background, b.background, t),
    };
}
// }}}

// {{{ RGBAColor
struct rgba_color
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

    constexpr rgba_color() noexcept = default;
    constexpr rgba_color(uint32_t value) noexcept: value { value } {}

    constexpr rgba_color(uint8_t r, uint8_t g, uint8_t b, uint8_t a) noexcept:
        value { (static_cast<uint32_t>(r) << 24) | (static_cast<uint32_t>(g) << 16)
                | (static_cast<uint32_t>(b) << 8) | (a) }
    {
    }

    constexpr rgba_color(rgb_color color) noexcept: rgba_color { color.red, color.green, color.blue, 0xFF } {}

    constexpr rgba_color(rgb_color color, uint8_t alpha) noexcept:
        rgba_color { color.red, color.green, color.blue, alpha }
    {
    }

    [[nodiscard]] constexpr rgb_color rgb() const noexcept { return rgb_color(value >> 8); }

    [[nodiscard]] rgba_color& operator=(std::string const& hexCode);

    // NOLINTNEXTLINE(readability-identifier-naming)
    constexpr static inline auto White = uint32_t(0xFF'FF'FF'FF);
};

constexpr bool operator==(rgba_color a, rgba_color b) noexcept
{
    return a.value == b.value;
}

constexpr bool operator!=(rgba_color a, rgba_color b) noexcept
{
    return !(a == b);
}
// }}}

// {{{ Color
enum class color_type : uint8_t
{
    Undefined,
    Default,
    Bright,
    Indexed,
    RGB
};

struct CRISPY_PACKED color
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

    constexpr color() noexcept = default;
    constexpr color(color const&) noexcept = default;
    constexpr color(color&&) noexcept = default;
    constexpr color& operator=(color const&) noexcept = default;
    constexpr color& operator=(color&&) noexcept = default;

    constexpr color(bright_color value) noexcept:
        content { (unsigned(value) & 0xFF) | (unsigned(color_type::Bright) << 24) }
    {
    }
    constexpr color(indexed_color value) noexcept:
        content { (unsigned(value) & 0xFF) | (unsigned(color_type::Indexed) << 24) }
    {
    }
    constexpr color(rgb_color rgb) noexcept: content { rgb.value() | (unsigned(color_type::RGB) << 24) } {}

    [[nodiscard]] constexpr color_type type() const noexcept
    {
        return static_cast<color_type>((content >> 24) & 0xFF);
    }
    [[nodiscard]] constexpr uint8_t index() const noexcept { return content & 0xFF; }
    [[nodiscard]] constexpr rgb_color rgb() const noexcept { return rgb_color(content & 0xFFFFFF); }

    // NOLINTBEGIN(readability-identifier-naming)
    [[nodiscard]] constexpr static color Undefined() noexcept { return color { color_type::Undefined, 0 }; }
    [[nodiscard]] constexpr static color Default() noexcept { return color { color_type::Default, 0 }; }
    [[nodiscard]] constexpr static color Bright(uint8_t index) noexcept
    {
        return color { color_type::Bright, index };
    }
    [[nodiscard]] constexpr static color Indexed(uint8_t index) noexcept
    {
        return color { color_type::Indexed, index };
    }
    [[nodiscard]] constexpr static color Indexed(indexed_color index) noexcept
    {
        return color { color_type::Indexed, (uint8_t) index };
    }
    // NOLINTEND(readability-identifier-naming)

    // TODO: The line below breaks on Windows, most likely because RGB is a PPD, let's find out. ;-)
    // constexpr static Color RGB(RGBColor color) noexcept { return Color{color}; }

  private:
    constexpr color(color_type type, uint8_t value) noexcept:
        content { (static_cast<uint32_t>(type) << 24) | (static_cast<uint32_t>(value) & 0xFF) }
    {
    }
};

constexpr bool operator==(color a, color b) noexcept
{
    return a.content == b.content;
}

constexpr bool operator!=(color a, color b) noexcept
{
    return !(a == b);
}

constexpr bool isUndefined(color color) noexcept
{
    return color.type() == color_type::Undefined;
}
constexpr bool isDefaultColor(color color) noexcept
{
    return color.type() == color_type::Default;
}

constexpr bool isIndexedColor(color color) noexcept
{
    return color.type() == color_type::Indexed;
}
constexpr bool isBrightColor(color color) noexcept
{
    return color.type() == color_type::Bright;
}
constexpr bool isRGBColor(color color) noexcept
{
    return color.type() == color_type::RGB;
}

constexpr uint8_t getIndexedColor(color color) noexcept
{
    return color.index();
}
constexpr uint8_t getBrightColor(color color) noexcept
{
    return color.index();
}
constexpr rgb_color getRGBColor(color color) noexcept
{
    return color.rgb();
}

std::string to_string(color color);
std::string to_string(indexed_color color);
std::string to_string(bright_color color);
std::string to_string(rgb_color c);
std::string to_string(rgba_color c);

inline std::ostream& operator<<(std::ostream& os, terminal::color value)
{
    return os << to_string(value);
}

constexpr color UndefinedColor() noexcept
{
    return color::Undefined();
}
constexpr color DefaultColor() noexcept
{
    return color::Default();
}
// }}}

struct cell_foreground_color
{
};
struct cell_background_color
{
};
using cell_rgb_color = std::variant<rgb_color, cell_foreground_color, cell_background_color>;

struct cell_rgb_color_pair
{
    cell_rgb_color foreground;
    cell_rgb_color background;
};

struct cell_rgb_color_and_alpha_pair
{
    cell_rgb_color foreground;
    float foregroundAlpha = 1.0f;
    cell_rgb_color background;
    float backgroundAlpha = 1.0f;
};

struct cursor_color
{
    cell_rgb_color color = cell_foreground_color {};
    cell_rgb_color textOverrideColor = cell_background_color {};
};

// {{{ Opacity
enum class opacity : uint8_t
{
    Transparent = 0x00,
    Opaque = 0xFF
};

constexpr opacity& operator++(opacity& value) noexcept
{
    value = static_cast<opacity>(std::min(static_cast<int>(value) + 15, 0xFF));
    return value;
}

constexpr opacity& operator--(opacity& value) noexcept
{
    value = static_cast<opacity>(std::max(static_cast<int>(value) - 15, 0));
    return value;
}
// }}}

} // namespace terminal

// {{{ fmtlib custom formatter
template <>
struct fmt::formatter<terminal::color>: fmt::formatter<std::string>
{
    auto format(terminal::color value, format_context& ctx) -> format_context::iterator
    {
        return formatter<std::string>::format(to_string(value), ctx);
    }
};

template <>
struct fmt::formatter<terminal::rgb_color>: fmt::formatter<std::string>
{
    auto format(terminal::rgb_color value, format_context& ctx) -> format_context::iterator
    {
        return formatter<std::string>::format(to_string(value), ctx);
    }
};

template <>
struct fmt::formatter<terminal::rgba_color>: fmt::formatter<std::string>
{
    template <typename FormatContext>
    auto format(terminal::rgba_color value, format_context& ctx) -> format_context::iterator
    {
        return formatter<std::string>::format(to_string(value), ctx);
    }
};

template <>
struct fmt::formatter<terminal::cell_rgb_color>: fmt::formatter<std::string>
{
    auto format(terminal::cell_rgb_color value, format_context& ctx) -> format_context::iterator
    {
        if (std::holds_alternative<terminal::cell_foreground_color>(value))
            return formatter<std::string>::format("CellForeground", ctx);
        else if (std::holds_alternative<terminal::cell_background_color>(value))
            return formatter<std::string>::format("CellBackground", ctx);
        else
            return formatter<std::string>::format(to_string(std::get<terminal::rgb_color>(value)), ctx);
    }
};

template <>
struct fmt::formatter<terminal::rgb_color_pair>: fmt::formatter<std::string>
{
    auto format(terminal::rgb_color_pair value, format_context& ctx) -> format_context::iterator
    {
        return formatter<std::string>::format(fmt::format("{}/{}", value.foreground, value.background), ctx);
    }
};
// }}}
