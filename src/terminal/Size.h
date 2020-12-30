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

namespace terminal {

/// Screen coordinates between 1..n including.
struct Coordinate {
    int row = 1;
    int column = 1;
};

constexpr Coordinate operator+(Coordinate a, Coordinate b) noexcept
{
    return Coordinate{a.row + b.row, a.column + b.column};
}

constexpr Coordinate& operator+=(Coordinate& a, Coordinate b) noexcept
{
    a.row += b.row;
    a.column += b.column;
    return a;
}

constexpr void swap(Coordinate& a, Coordinate& b) noexcept
{
    Coordinate const c = a;
    a = b;
    b = c;
}

// Prints Coordinate as human readable text to given stream (used for debugging & unit testing).
inline std::ostream& operator<<(std::ostream& _os, Coordinate const& _coord)
{
    return _os << "{" << _coord.row << ", " << _coord.column << "}";
}

constexpr inline int compare(Coordinate const& a, Coordinate const& b) noexcept
{
    if (auto const dr = a.row - b.row; dr != 0)
        return dr;
    else
        return a.column - b.column;
}

constexpr inline bool operator<(Coordinate const& a, Coordinate const& b) noexcept
{
    return compare(a, b) < 0;
}

constexpr inline bool operator<=(Coordinate const& a, Coordinate const& b) noexcept
{
    return compare(a, b) <= 0;
}

constexpr inline bool operator>(Coordinate const& a, Coordinate const& b) noexcept
{
    return compare(a, b) > 0;
}

constexpr inline bool operator>=(Coordinate const& a, Coordinate const& b) noexcept
{
    return compare(a, b) >= 0;
}

constexpr inline bool operator==(Coordinate const& a, Coordinate const& b) noexcept
{
    return a.row == b.row && a.column == b.column;
}

constexpr inline bool operator!=(Coordinate const& a, Coordinate const& b) noexcept
{
    return !(a == b);
}

struct [[nodiscard]] Size {
    int width;
	int height;

    /// This iterator can be used to iterate through each and every point between (0, 0) and (width, height).
    struct iterator {
      private:
        int width;
        int next;
        Coordinate coord{0, 0};

        constexpr Coordinate makeCoordinate(int offset) noexcept
        {
            return Coordinate{
                offset / width,
                offset % width
            };
        }

      public:
        constexpr iterator(int _width, int _next) noexcept :
            width{ _width },
            next{ _next },
            coord{ makeCoordinate(_next) }
        {
        }

        constexpr Coordinate operator*() const noexcept { return coord; }

        constexpr iterator& operator++() noexcept
        {
            coord = makeCoordinate(++next);
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

constexpr Size::iterator begin(Size const& s) noexcept { return s.begin(); }
constexpr Size::iterator end(Size const& s) noexcept { return s.end(); }

constexpr int area(Size size) noexcept
{
    return size.width * size.height;
}

constexpr bool operator<(Size a, Size b) noexcept
{
    return area(a) < area(b);
}

constexpr bool operator==(Size const& _a, Size const& _b) noexcept
{
    return _a.width == _b.width && _a.height == _b.height;
}

constexpr bool operator!=(Size const& _a, Size const& _b) noexcept
{
    return !(_a == _b);
}

constexpr Size operator+(Size _a, Size _b) noexcept
{
    return Size{
        _a.width + _b.width,
        _a.height + _b.height
    };
}

constexpr Size operator-(Size _a, Size _b) noexcept
{
    return Size{
        _a.width - _b.width,
        _a.height - _b.height
    };
}

constexpr Size operator*(Size _a, Size _b) noexcept
{
    return Size{
        _a.width * _b.width,
        _a.height * _b.height
    };
}

constexpr Size operator/(Size _a, Size _b) noexcept
{
    return Size{
        _a.width / _b.width,
        _a.height / _b.height
    };
}

constexpr Coordinate operator+(Coordinate a, Size b) noexcept
{
    return Coordinate{a.row + b.height, a.column + b.width};
}

} // end namespace terminal

namespace fmt {
    template <>
    struct formatter<terminal::Size> {
        template <typename ParseContext>
        constexpr auto parse(ParseContext& ctx)
        {
            return ctx.begin();
        }

        template <typename FormatContext>
        auto format(const terminal::Size& value, FormatContext& ctx)
        {
            return format_to(ctx.out(), "{}x{}", value.width, value.height);
        }
    };

    template <>
    struct formatter<terminal::Coordinate> {
        template <typename ParseContext>
        constexpr auto parse(ParseContext& ctx)
        {
            return ctx.begin();
        }

        template <typename FormatContext>
        auto format(terminal::Coordinate coord, FormatContext& ctx)
        {
            return format_to(ctx.out(), "({}:{})", coord.row, coord.column);
        }
    };

}
