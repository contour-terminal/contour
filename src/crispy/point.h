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

#include <ostream>

namespace crispy
{

struct [[nodiscard]] Point
{
    int x;
    int y;
};

template <typename T>
constexpr inline T Zero {};
template <>
constexpr inline Point Zero<Point> = Point { 0, 0 };

constexpr Point operator*(Point a, double s) noexcept
{
    return Point { static_cast<int>(a.x * s), static_cast<int>(a.y * s) };
}

constexpr Point operator+(Point a, Point b) noexcept
{
    return Point { a.x + b.x, a.y + b.y };
}

constexpr Point& operator+=(Point& a, Point b) noexcept
{
    a.x += b.x;
    a.y += b.y;
    return a;
}

constexpr void swap(Point& a, Point& b) noexcept
{
    Point const c = a;
    a = b;
    b = c;
}

constexpr inline int compare(Point const& a, Point const& b) noexcept
{
    if (auto const dr = a.y - b.y; dr != 0)
        return dr;
    else
        return a.x - b.x;
}

constexpr inline bool operator<(Point const& a, Point const& b) noexcept
{
    return compare(a, b) < 0;
}

constexpr inline bool operator<=(Point const& a, Point const& b) noexcept
{
    return compare(a, b) <= 0;
}

constexpr inline bool operator>(Point const& a, Point const& b) noexcept
{
    return compare(a, b) > 0;
}

constexpr inline bool operator>=(Point const& a, Point const& b) noexcept
{
    return compare(a, b) >= 0;
}

constexpr inline bool operator==(Point const& a, Point const& b) noexcept
{
    return a.x == b.x && a.y == b.y;
}

constexpr inline bool operator!=(Point const& a, Point const& b) noexcept
{
    return !(a == b);
}

} // namespace crispy

namespace fmt
{
template <>
struct formatter<crispy::Point>
{
    template <typename ParseContext>
    constexpr auto parse(ParseContext& ctx)
    {
        return ctx.begin();
    }
    template <typename FormatContext>
    auto format(crispy::Point coord, FormatContext& ctx)
    {
        return format_to(ctx.out(), "({}, {})", coord.x, coord.y);
    }
};
} // namespace fmt
