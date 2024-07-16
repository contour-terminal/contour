// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <fmt/format.h>

#include <ostream>

namespace crispy
{

struct [[nodiscard]] point
{
    int x {};
    int y {};
};

template <typename T>
constexpr inline T Zero {};
template <>
constexpr inline point Zero<point> = point { 0, 0 };

constexpr point operator*(point p, double s) noexcept
{
    return point {
        static_cast<int>(static_cast<double>(p.x) * s),
        static_cast<int>(static_cast<double>(p.y) * s),
    };
}

constexpr point operator+(point a, point b) noexcept
{
    return point { a.x + b.x, a.y + b.y };
}

constexpr point& operator+=(point& a, point b) noexcept
{
    a.x += b.x;
    a.y += b.y;
    return a;
}

constexpr void swap(point& a, point& b) noexcept
{
    point const c = a;
    a = b;
    b = c;
}

constexpr inline int compare(point const& a, point const& b) noexcept
{
    if (auto const dr = a.y - b.y; dr != 0)
        return dr;
    else
        return a.x - b.x;
}

constexpr inline bool operator<(point const& a, point const& b) noexcept
{
    return compare(a, b) < 0;
}

constexpr inline bool operator<=(point const& a, point const& b) noexcept
{
    return compare(a, b) <= 0;
}

constexpr inline bool operator>(point const& a, point const& b) noexcept
{
    return compare(a, b) > 0;
}

constexpr inline bool operator>=(point const& a, point const& b) noexcept
{
    return compare(a, b) >= 0;
}

constexpr inline bool operator==(point const& a, point const& b) noexcept
{
    return a.x == b.x && a.y == b.y;
}

constexpr inline bool operator!=(point const& a, point const& b) noexcept
{
    return !(a == b);
}

} // namespace crispy

template <>
struct fmt::formatter<crispy::point>: formatter<std::string>
{
    auto format(crispy::point coord, format_context& ctx) const -> format_context::iterator
    {
        return formatter<std::string>::format(fmt::format("({}, {})", coord.x, coord.y), ctx);
    }
};
