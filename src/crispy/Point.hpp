// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <format>

namespace crispy
{

struct [[nodiscard]] Point
{
    int x {};
    int y {};

    constexpr bool operator!() const noexcept { return x == 0 && y == 0; }
    constexpr operator bool() const noexcept { return x != 0 || y != 0; }
};

template <typename T>
constexpr inline T Zero {};
template <>
constexpr inline Point Zero<Point> = Point { .x = 0, .y = 0 };

constexpr Point operator*(Point p, double s) noexcept
{
    return Point {
        .x = static_cast<int>(static_cast<double>(p.x) * s),
        .y = static_cast<int>(static_cast<double>(p.y) * s),
    };
}

constexpr Point operator/(Point p, double s) noexcept
{
    return Point {
        .x = static_cast<int>(static_cast<double>(p.x) / s),
        .y = static_cast<int>(static_cast<double>(p.y) / s),
    };
}

constexpr Point operator*(Point a, Point b) noexcept
{
    return Point {
        .x = a.x * b.x,
        .y = a.y * b.y,
    };
}

constexpr Point operator/(Point a, Point b) noexcept
{
    return Point {
        .x = a.x / b.x,
        .y = a.y / b.y,
    };
}

constexpr Point operator+(Point a, Point b) noexcept
{
    return Point { .x = a.x + b.x, .y = a.y + b.y };
}

constexpr Point& operator+=(Point& a, Point b) noexcept
{
    a.x += b.x;
    a.y += b.y;
    return a;
}

constexpr Point& operator-=(Point& a, Point b) noexcept
{
    a.x -= b.x;
    a.y -= b.y;
    return a;
}

constexpr void swap(Point& a, Point& b) noexcept
{
    Point const c = a;
    a = b;
    b = c;
}

constexpr int compare(Point const& a, Point const& b) noexcept
{
    if (auto const dr = a.y - b.y; dr != 0)
        return dr;
    else
        return a.x - b.x;
}

constexpr bool operator<(Point const& a, Point const& b) noexcept
{
    return compare(a, b) < 0;
}

constexpr bool operator<=(Point const& a, Point const& b) noexcept
{
    return compare(a, b) <= 0;
}

constexpr bool operator>(Point const& a, Point const& b) noexcept
{
    return compare(a, b) > 0;
}

constexpr bool operator>=(Point const& a, Point const& b) noexcept
{
    return compare(a, b) >= 0;
}

constexpr bool operator==(Point const& a, Point const& b) noexcept
{
    return a.x == b.x && a.y == b.y;
}

constexpr bool operator!=(Point const& a, Point const& b) noexcept
{
    return !(a == b);
}

} // namespace crispy

template <>
struct std::formatter<crispy::Point>: formatter<std::string>
{
    auto format(crispy::Point coord, auto& ctx) const
    {
        return formatter<std::string>::format(std::format("({}, {})", coord.x, coord.y), ctx);
    }
};
