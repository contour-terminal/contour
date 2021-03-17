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

#include <crispy/point.h>

namespace terminal {

struct [[nodiscard]] Coordinate {
    int row = 0;
    int column = 0;

    constexpr Coordinate& operator+=(Coordinate const& a) noexcept
    {
        row += a.row;
        column += a.column;
        return *this;
    }

    constexpr Coordinate(int _row, int _column) : row{_row}, column{_column} {}
    constexpr explicit Coordinate(crispy::Point p) : row{p.y}, column{p.x} {}
    constexpr Coordinate() = default;
    constexpr Coordinate(Coordinate const&) = default;
    constexpr Coordinate(Coordinate&& v) = default;
    constexpr Coordinate& operator=(Coordinate const&) = default;
    constexpr Coordinate& operator=(Coordinate &&) = default;
};

constexpr void swap(Coordinate& a, Coordinate& b) noexcept
{
    auto c = a;
    a = b;
    b = c;
}

constexpr bool operator==(Coordinate const& a, Coordinate const& b) noexcept { return a.row == b.row && a.column == b.column; }
constexpr bool operator!=(Coordinate const& a, Coordinate const& b) noexcept { return !(a == b); }

constexpr bool operator<(Coordinate const& a, Coordinate const& b) noexcept
{
    if (a.row < b.row)
        return true;

    if (a.row == b.row && a.column < b.column)
        return true;

    return false;
}

constexpr bool operator<=(Coordinate const& a, Coordinate const& b) noexcept
{
    return a < b || a == b;
}

constexpr bool operator>=(Coordinate const& a, Coordinate const& b) noexcept
{
    return !(a < b);
}

constexpr bool operator>(Coordinate const& a, Coordinate const& b) noexcept
{
    return !(a == b || a < b);
}

inline Coordinate operator+(Coordinate const& a, Coordinate const& b) noexcept { return {a.row + b.row, a.column + b.column}; }

constexpr Coordinate operator+(Coordinate const& a, crispy::Point b) noexcept
{
    return {a.row + b.y, a.column + b.x};
}

}

namespace fmt {
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
            return format_to(ctx.out(), "({}, {})", coord.row, coord.column);
        }
    };
}
