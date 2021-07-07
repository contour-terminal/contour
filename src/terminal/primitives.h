#pragma once

#include <crispy/boxed.h>
#include <crispy/ImageSize.h>
#include <cstdint>
#include <type_traits>
#include <cassert>

// TODO
// - [ ] rename all History to Scrollback
// - [ ] make sense out of all the semantically different line primitives.

namespace terminal {

namespace detail::tags // {{{
{
    // column types
    struct ColumnCount{};
    struct Column{};

    // line types
    struct LineCount{};
    struct LinePosition{};
    struct RelativeLinePosition{};
    struct RelativeScrollbackPosition{};
    struct StaticScrollbackPosition{};

    // misc.
    struct TabStopCount{};

    // generic length
    struct Length{};

    // range
    struct From{};
    struct To{};

    // margin
    struct Top{};
    struct Left{};
    struct Bottom{};
    struct Right{};
}
// }}}

// {{{ Column types

/// ColumnCount simply represents a number of columns.
using ColumnCount = crispy::boxed<int, detail::tags::ColumnCount>;

/// ColumnPosition represents the absolute column on the visibile screen area
/// (usually the main page unless scrolled upwards).
///
/// A column position starts at 1.
using ColumnPosition = crispy::boxed<int, detail::tags::Column>;

// }}}
// {{{ Line types

/// LineCount represents a number of lines.
using LineCount = crispy::boxed<int, detail::tags::LineCount>;

/// LinePosition is the 1-based line coordinate of the main-page area (or viewport).
using LinePosition = crispy::boxed<int, detail::tags::LinePosition>;

/// RelativeScrollbackPosition represents scroll offset relative to the main page buffer.
///
/// A value of 0 means bottom most scrollback, one line above main page area.
/// And a value equal to the number of scrollback lines minus one means
/// the top-most scrollback line.
using RelativeScrollbackPosition = crispy::boxed<int, detail::tags::RelativeScrollbackPosition>;

/// RelativeLinePosition combines LinePosition and RelativeScrollbackPosition
/// into one, whereas values from 1 upwards are main page area, and
/// values from 0 downwards represent the scrollback lines.
using RelativeLinePosition = crispy::boxed<int, detail::tags::RelativeLinePosition>;

/// StaticScrollbackPosition represents scroll offset relative to scroll top (0).
///
/// A value of 0 means scroll top, and
/// a value equal to the number of scrollback lines
/// is the scroll bottom (main page area).
using StaticScrollbackPosition = crispy::boxed<int, detail::tags::StaticScrollbackPosition>;

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
    struct ValueTag{};
    using iterator = crispy::boxed<int, ValueTag>;
    iterator begin() const { return iterator{from.value}; }
    auto end() const { return iterator{to.value + 1}; }
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
struct Rect { Top top; Left left; Bottom bottom; Right right; };

// Screen's page margin
//
struct PageMargin { Top top; Left left; Bottom bottom; Right right; };

constexpr Range horizontal(PageMargin m) noexcept { return Range{ From{*m.top}, To{*m.bottom} }; }
constexpr Range vertical(PageMargin m) noexcept { return Range{ From{*m.left}, To{*m.right} }; }

// }}}
// {{{ Length

// Lengths and Ranges
using Length = crispy::boxed<int, detail::tags::Length>;

// }}}
// {{{ PageSize
struct PageSize { LineCount lines; ColumnCount columns; };
constexpr bool operator==(PageSize a, PageSize b) noexcept { return a.lines == b.lines && a.columns == b.columns; }
constexpr bool operator!=(PageSize a, PageSize b) noexcept { return !(a == b); }
// }}}
// {{{ Coordinate types

struct ScreenCoordinate // or name CursorPosition?
{
    LinePosition line;
    ColumnPosition column;
};

// }}}
// {{{ GridSize

struct GridSize
{
    LineCount lines;
    ColumnCount columns;

    struct Offset {
        LineCount lines;
        ColumnCount columns;
    };

    /// This iterator can be used to iterate through each and every point between (0, 0) and (width, height).
    struct iterator {
      public:
        constexpr iterator(ColumnCount _width, int _next) noexcept :
            width{ _width },
            next{ _next },
            offset{ makeOffset(_next) }
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
            return Offset{
                LineCount(offset / *width),
                ColumnCount(offset % *width)
            };
        }
    };

    constexpr iterator begin() const noexcept { return iterator{columns, 0}; }
    constexpr iterator end() const noexcept { return iterator{columns, *columns * *lines}; }

    constexpr iterator begin() noexcept { return iterator{columns, 0}; }
    constexpr iterator end() noexcept { return iterator{columns, *columns * *lines}; }
};

constexpr GridSize::iterator begin(GridSize const& s) noexcept { return s.begin(); }
constexpr GridSize::iterator end(GridSize const& s) noexcept { return s.end(); }
// }}}
// {{{ misc

using TabStopCount = crispy::boxed<int, detail::tags::TabStopCount>;

// }}}
// {{{ convenience methods

constexpr Length length(Range range) noexcept
{
    //assert(range.to.value >= range.from.value);
    return Length::cast_from(*range.to - *range.from) + Length{1};
}

// }}}
// {{{ ImageSize types

using Width = crispy::Width;
using Height = crispy::Height;

using ImageSize = crispy::ImageSize;

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

}

namespace fmt { // {{{
    template <>
    struct formatter<terminal::PageSize> {
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
} // }}}
