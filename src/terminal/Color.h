// This file is part of the "libterminal" project, http://github.com/christianparpart/libterminal>
//   (c) 2018 Christian Parpart <christian@parpart.family>
//
// Licensed under the MIT License (the "License"); you may not use this
// file except in compliance with the License. You may obtain a copy of
// the License at: http://opensource.org/licenses/MIT

#pragma once

#include <fmt/format.h>

#include <cstdint>
#include <cstdio>  // snprintf
#include <string>
#include <variant>

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
};

constexpr bool operator==(RGBColor const& a, RGBColor const& b) noexcept
{
    return a.red == b.red && a.green == b.green && a.blue == b.blue;
}

constexpr bool operator!=(RGBColor const& a, RGBColor const& b) noexcept
{
    return !(a == b);
}

using Color = std::variant<UndefinedColor, DefaultColor, IndexedColor, BrightColor, RGBColor>;

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

}  // namespace terminal
