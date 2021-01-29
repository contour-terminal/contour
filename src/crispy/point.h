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

#include <ostream>

namespace crispy
{

struct [[nodiscard]] Point // {{{
{
    int x;
    int y;
};

constexpr Point operator+(Point a, Point b) noexcept
{
    return Point{a.x + b.x, a.y + b.y};
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

// Prints Point as human readable text to given stream (used for debugging & unit testing).
inline std::ostream& operator<<(std::ostream& _os, Point const& _coord)
{
    return _os << "{" << _coord.x << ", " << _coord.y << "}";
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
// }}}

struct [[nodiscard]] Size // {{{
{
    int width;
	int height;

    /// This iterator can be used to iterate through each and every point between (0, 0) and (width, height).
    struct iterator {
      private:
        int width;
        int next;
        Point coord{0, 0};

        constexpr Point makePoint(int offset) noexcept
        {
            return Point{
                offset / width,
                offset % width
            };
        }

      public:
        constexpr iterator(int _width, int _next) noexcept :
            width{ _width },
            next{ _next },
            coord{ makePoint(_next) }
        {
        }

        constexpr Point operator*() const noexcept { return coord; }

        constexpr iterator& operator++() noexcept
        {
            coord = makePoint(++next);
            return *this;
        }

        constexpr iterator& operator++(int) noexcept
        {
            ++*this;
            return *this;
        }

        constexpr bool operator==(iterator const& other) const noexcept { return next == other.next; }
        constexpr bool operator!=(iterator const& other) const noexcept { return next != other.next; }
    };

    constexpr iterator begin() const noexcept { return iterator{width, 0}; }
    constexpr iterator end() const noexcept { return iterator{width, width * height}; }

    constexpr iterator begin() noexcept { return iterator{width, 0}; }
    constexpr iterator end() noexcept { return iterator{width, width * height}; }
};
// }}}

}
