// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <crispy/defines.h>

#include <fmt/format.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
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
enum class BrightColor
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

constexpr RGBColor mix(RGBColor a, RGBColor b, float t = 0.5) noexcept
{
    return a * t + b * (1.0f - t);
}

inline double distance(RGBColor e1, RGBColor e2) noexcept
{
    auto const rmean = (uint32_t(e1.red) + uint32_t(e2.red)) / 2;
    auto const r = uint32_t(e1.red) - uint32_t(e2.red);
    auto const g = uint32_t(e1.green) - uint32_t(e2.green);
    auto const b = uint32_t(e1.blue) - uint32_t(e2.blue);
    return sqrt((((512 + rmean) * r * r) >> 8) + 4 * g * g + (((767 - rmean) * b * b) >> 8));
}

constexpr RGBColor operator"" _rgb(unsigned long long value)
{
    return RGBColor { static_cast<uint32_t>(value) };
}

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
            return { foreground.inverse(), foreground };
        else
            return *this;
    }

    [[nodiscard]] constexpr RGBColorPair constructDefaulted(std::optional<RGBColor> fgOpt,
                                                            std::optional<RGBColor> bgOpt) const noexcept
    {
        return { fgOpt.value_or(foreground), bgOpt.value_or(background) };
    }

    [[nodiscard]] constexpr RGBColorPair swapped() const noexcept
    {
        // Swap fg/bg.
        return { background, foreground };
    }

    [[nodiscard]] constexpr RGBColorPair allForeground() const noexcept
    {
        // All same color components as foreground.
        return { foreground, foreground };
    }

    [[nodiscard]] constexpr RGBColorPair allBackground() const noexcept
    {
        // All same color components as foreground.
        return { background, background };
    }
};

constexpr RGBColorPair mix(RGBColorPair a, RGBColorPair b, float t = 0.5) noexcept
{
    return RGBColorPair {
        mix(a.foreground, b.foreground, t),
        mix(a.background, b.background, t),
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

} // namespace vtbackend

// {{{ fmtlib custom formatter
template <>
struct fmt::formatter<vtbackend::Color>: fmt::formatter<std::string>
{
    auto format(vtbackend::Color value, format_context& ctx) -> format_context::iterator
    {
        return formatter<std::string>::format(to_string(value), ctx);
    }
};

template <>
struct fmt::formatter<vtbackend::RGBColor>: fmt::formatter<std::string>
{
    auto format(vtbackend::RGBColor value, format_context& ctx) -> format_context::iterator
    {
        return formatter<std::string>::format(to_string(value), ctx);
    }
};

template <>
struct fmt::formatter<vtbackend::RGBAColor>: fmt::formatter<std::string>
{
    template <typename FormatContext>
    auto format(vtbackend::RGBAColor value, format_context& ctx) -> format_context::iterator
    {
        return formatter<std::string>::format(to_string(value), ctx);
    }
};

template <>
struct fmt::formatter<vtbackend::CellRGBColor>: fmt::formatter<std::string>
{
    auto format(vtbackend::CellRGBColor value, format_context& ctx) -> format_context::iterator
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
struct fmt::formatter<vtbackend::RGBColorPair>: fmt::formatter<std::string>
{
    auto format(vtbackend::RGBColorPair value, format_context& ctx) -> format_context::iterator
    {
        return formatter<std::string>::format(fmt::format("{}/{}", value.foreground, value.background), ctx);
    }
};
// }}}
