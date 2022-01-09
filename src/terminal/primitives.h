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

#include <terminal/defines.h>

#include <crispy/ImageSize.h>
#include <crispy/boxed.h>

#include <cassert>
#include <cstdint>
#include <ostream>
#include <type_traits>

// TODO
// - [ ] rename all History to Scrollback
// - [ ] make sense out of all the semantically different line primitives.

namespace terminal
{

namespace detail::tags // {{{
{
    // column types
    struct ColumnCount
    {
    };
    struct ColumnOffset
    {
    };
    struct ColumnPosition
    {
    };

    // line types
    struct LineCount
    {
    };
    struct LineOffset
    {
    };
    struct ScrollOffset
    {
    };

    // misc.
    struct TabStopCount
    {
    };

    // generic length
    struct Length
    {
    };

    // range
    struct From
    {
    };
    struct To
    {
    };

    // margin
    struct Top
    {
    };
    struct Left
    {
    };
    struct Bottom
    {
    };
    struct Right
    {
    };
} // namespace detail::tags
// }}}

// {{{ Column types

/// ColumnCount simply represents a number of columns.
using ColumnCount = crispy::boxed<int, detail::tags::ColumnCount>;

/// ColumnPosition represents the absolute column on the visibile screen area
/// (usually the main page unless scrolled upwards).
///
/// A column position starts at 1.
using ColumnPosition = crispy::boxed<int, detail::tags::ColumnPosition>;

using ColumnOffset = crispy::boxed<int, detail::tags::ColumnOffset>;

// }}}
// {{{ Line types

/// LineCount represents a number of lines.
using LineCount = crispy::boxed<int, detail::tags::LineCount>;

/// Represents the line offset relative to main-page top.
///
/// *  0  is top-most line on main page
/// *  -1 is the bottom most line in scrollback
using LineOffset = crispy::boxed<int, detail::tags::LineOffset>;

/// Represents the number of lines the viewport has been scrolled up into
/// the scrollback lines history.
///
/// A value of 0 means that it is not scrolled at all (bottom), and
/// a value equal to the number of scrollback lines means it is scrolled
/// to the top.
using ScrollOffset = crispy::boxed<int, detail::tags::ScrollOffset>;

constexpr int operator*(LineCount a, ColumnCount b) noexcept
{
    return a.as<int>() * b.as<int>();
}
constexpr int operator*(ColumnCount a, LineCount b) noexcept
{
    return a.as<int>() * b.as<int>();
}
// }}}

struct MousePixelPosition
{
    // clang-format off
    struct X { int value; };
    struct Y { int value; };
    // clang-format on

    X x {};
    Y y {};
};

struct [[nodiscard]] Coordinate
{
    LineOffset line {};
    ColumnOffset column {};

    constexpr Coordinate& operator+=(Coordinate a) noexcept
    {
        line += a.line;
        column += a.column;
        return *this;
    }

    constexpr Coordinate& operator+=(ColumnOffset x) noexcept
    {
        column += x;
        return *this;
    }
    constexpr Coordinate& operator+=(LineOffset y) noexcept
    {
        line += y;
        return *this;
    }
};

inline std::ostream& operator<<(std::ostream& os, Coordinate coord)
{
    return os << fmt::format("({}, {})", coord.line, coord.column);
}

constexpr bool operator==(Coordinate a, Coordinate b) noexcept
{
    return a.line == b.line && a.column == b.column;
}
constexpr bool operator!=(Coordinate a, Coordinate b) noexcept
{
    return !(a == b);
}

constexpr bool operator<(Coordinate a, Coordinate b) noexcept
{
    if (a.line < b.line)
        return true;

    if (a.line == b.line && a.column < b.column)
        return true;

    return false;
}

constexpr bool operator<=(Coordinate a, Coordinate b) noexcept
{
    return a < b || a == b;
}

constexpr bool operator>=(Coordinate a, Coordinate b) noexcept
{
    return !(a < b);
}

constexpr bool operator>(Coordinate a, Coordinate b) noexcept
{
    return !(a == b || a < b);
}

inline Coordinate operator+(Coordinate a, Coordinate b) noexcept
{
    return { a.line + b.line, a.column + b.column };
}

constexpr Coordinate operator+(Coordinate c, LineOffset y) noexcept
{
    return Coordinate { c.line + y, c.column };
}

constexpr Coordinate operator+(Coordinate c, ColumnOffset x) noexcept
{
    return Coordinate { c.line, c.column + x };
}

// }}}
// {{{ Range

/// Represents the first value of a range.
using From = crispy::boxed<int, detail::tags::From>;

/// Represents the last value of a range (inclusive).
using To = crispy::boxed<int, detail::tags::To>;

// Range (e.g. a range of lines from X to Y).
struct Range
{
    From from;
    To to;

    // So you can do: for (auto const v: Range{3, 5}) { ... }
    struct ValueTag
    {
    };
    using iterator = crispy::boxed<int, ValueTag>;
    iterator begin() const { return iterator { from.value }; }
    auto end() const { return iterator { to.value + 1 }; }
    // iterator end() const { return crispy::boxed_cast<iterator>(to) + iterator{1}; }
};

// }}}
// {{{ Rect & Margin

// Rectangular operations
//
using Top = crispy::boxed<int, detail::tags::Top>;
using Left = crispy::boxed<int, detail::tags::Left>;
using Bottom = crispy::boxed<int, detail::tags::Bottom>;
using Right = crispy::boxed<int, detail::tags::Right>;

// Rectangular screen operations
//
struct Rect
{
    Top top;
    Left left;
    Bottom bottom;
    Right right;
};

// Screen's page margin
//
struct PageMargin
{
    Top top;
    Left left;
    Bottom bottom;
    Right right;
};

constexpr Range horizontal(PageMargin m) noexcept
{
    return Range { From { *m.top }, To { *m.bottom } };
}
constexpr Range vertical(PageMargin m) noexcept
{
    return Range { From { *m.left }, To { *m.right } };
}

// }}}
// {{{ Length

// Lengths and Ranges
using Length = crispy::boxed<int, detail::tags::Length>;

// }}}
// {{{ PageSize
struct PageSize
{
    LineCount lines;
    ColumnCount columns;
    int area() const noexcept { return *lines * *columns; }
};
constexpr bool operator==(PageSize a, PageSize b) noexcept
{
    return a.lines == b.lines && a.columns == b.columns;
}
constexpr bool operator!=(PageSize a, PageSize b) noexcept
{
    return !(a == b);
}
// }}}
// {{{ Coordinate types

// (0, 0) is home position
struct ScreenPosition
{
    LineOffset line;
    ColumnOffset column;
};

// }}}
// {{{ GridSize

struct GridSize
{
    LineCount lines;
    ColumnCount columns;

    struct Offset
    {
        LineOffset line;
        ColumnOffset column;
    };

    /// This iterator can be used to iterate through each and every point between (0, 0) and (width, height).
    struct iterator
    {
      public:
        constexpr iterator(ColumnCount _width, int _next) noexcept:
            width { _width }, next { _next }, offset { makeOffset(_next) }
        {
        }

        constexpr auto operator*() const noexcept { return offset; }

        constexpr iterator& operator++() noexcept
        {
            offset = makeOffset(++next);
            return *this;
        }

        constexpr iterator& operator++(int) noexcept
        {
            ++*this;
            return *this;
        }

        constexpr bool operator==(iterator const& other) const noexcept { return next == other.next; }
        constexpr bool operator!=(iterator const& other) const noexcept { return next != other.next; }

      private:
        ColumnCount width;
        int next;
        Offset offset;

        constexpr Offset makeOffset(int offset) noexcept
        {
            return Offset { LineOffset(offset / *width), ColumnOffset(offset % *width) };
        }
    };

    constexpr iterator begin() const noexcept { return iterator { columns, 0 }; }
    constexpr iterator end() const noexcept { return iterator { columns, *columns * *lines }; }

    constexpr iterator begin() noexcept { return iterator { columns, 0 }; }
    constexpr iterator end() noexcept { return iterator { columns, *columns * *lines }; }
};

constexpr Coordinate operator+(Coordinate a, GridSize::Offset b) noexcept
{
    return Coordinate { a.line + b.line, a.column + b.column };
}

constexpr GridSize::iterator begin(GridSize const& s) noexcept
{
    return s.begin();
}
constexpr GridSize::iterator end(GridSize const& s) noexcept
{
    return s.end();
}
// }}}
// {{{ misc

using TabStopCount = crispy::boxed<int, detail::tags::TabStopCount>;

// }}}
// {{{ convenience methods

constexpr Length length(Range range) noexcept
{
    // assert(range.to.value >= range.from.value);
    return Length::cast_from(*range.to - *range.from) + Length { 1 };
}

// }}}
// {{{ ImageSize types

using Width = crispy::Width;
using Height = crispy::Height;

using ImageSize = crispy::ImageSize;

constexpr ImageSize operator*(ImageSize a, PageSize b) noexcept
{
    return ImageSize { a.width * boxed_cast<Width>(b.columns), a.height * boxed_cast<Height>(b.lines) };
}
// }}}
// {{{ Mixed boxed types operator overloads
constexpr LineCount operator+(LineCount a, LineOffset b) noexcept
{
    return a + b.value;
}
constexpr LineCount operator-(LineCount a, LineOffset b) noexcept
{
    return a - b.value;
}
constexpr LineOffset& operator+=(LineOffset& a, LineCount b) noexcept
{
    a.value += b.value;
    return a;
}
constexpr LineOffset& operator-=(LineOffset& a, LineCount b) noexcept
{
    a.value -= b.value;
    return a;
}

constexpr ColumnCount operator+(ColumnCount a, ColumnOffset b) noexcept
{
    return a + b.value;
}
constexpr ColumnCount operator-(ColumnCount a, ColumnOffset b) noexcept
{
    return a - b.value;
}
constexpr ColumnOffset& operator+=(ColumnOffset& a, ColumnCount b) noexcept
{
    a.value += b.value;
    return a;
}
constexpr ColumnOffset& operator-=(ColumnOffset& a, ColumnCount b) noexcept
{
    a.value -= b.value;
    return a;
}
// }}}

// TODO: Maybe make boxed.h into its own C++ github repo?
// TODO: Differenciate Line/Column types for DECOM enabled/disabled coordinates?
//
// Line, Column                 : respects DECOM if enabled (a.k.a. logical column)
// PhysicalLine, PhysicalColumn : always relative to origin (top left)
// ScrollbackLine               : line number relative to top-most line in scrollback buffer.
//
// Respectively for Coordinates:
// - Coordinate
// - PhysicalCoordinate
// - ScrollbackCoordinate

enum class CursorDisplay
{
    Steady,
    Blink
};

enum class CursorShape
{
    Block,
    Rectangle,
    Underscore,
    Bar,
};

} // namespace terminal

namespace fmt
{ // {{{
template <>
struct formatter<terminal::Coordinate>
{
    template <typename ParseContext>
    constexpr auto parse(ParseContext& ctx)
    {
        return ctx.begin();
    }
    template <typename FormatContext>
    auto format(terminal::Coordinate coord, FormatContext& ctx)
    {
        return format_to(ctx.out(), "({}, {})", coord.line, coord.column);
    }
};

template <>
struct formatter<terminal::PageSize>
{
    template <typename ParseContext>
    constexpr auto parse(ParseContext& ctx)
    {
        return ctx.begin();
    }
    template <typename FormatContext>
    auto format(terminal::PageSize value, FormatContext& ctx)
    {
        return format_to(ctx.out(), "{}x{}", value.columns, value.lines);
    }
};

template <>
struct formatter<terminal::GridSize>
{
    template <typename ParseContext>
    constexpr auto parse(ParseContext& ctx)
    {
        return ctx.begin();
    }
    template <typename FormatContext>
    auto format(terminal::GridSize value, FormatContext& ctx)
    {
        return format_to(ctx.out(), "{}x{}", value.columns, value.lines);
    }
};
} // namespace fmt
