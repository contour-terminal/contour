#pragma once

#include <crispy/boxed.h>
#include <cstdint>
#include <type_traits>
#include <cassert>

namespace terminal {

namespace tags { struct Column{}; }
/// ColumnPosition represents the absolute column on the visibile screen area
/// (usually the main page unless scrolled upwards).
using ColumnPosition = crispy::boxed<unsigned, tags::Column>;

namespace tags { struct Line{}; struct RelativeLine{}; struct HistoryLine{}; }
using LinePosition = crispy::boxed<unsigned, tags::Line>;
using RelativeLinePosition = crispy::boxed<int, tags::RelativeLine>;
using HistoryLinePosition = crispy::boxed<unsigned, tags::HistoryLine>;

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

namespace tags { struct ScrollOffset{}; }
using ScrollOffset = crispy::boxed<unsigned, tags::ScrollOffset>;

namespace tags { struct Width{}; struct Height{}; }
using Width = crispy::boxed<unsigned, tags::Width>;
using Height = crispy::boxed<unsigned, tags::Height>;

struct PageSize { LinePosition lines; ColumnPosition columns; };
struct HistoryCoordinate { HistoryLinePosition line; ColumnPosition column; };
struct ScreenCoordinate { LinePosition line; ColumnPosition column; }; // or CursorPosition?

// Lengths and Ranges
//
namespace tags { struct Length{}; }
using Length = crispy::boxed<std::size_t, tags::Length>;

// Range (line a range of lines from X to Y)
//
namespace tags { struct From{}; struct To; }
using From = crispy::boxed<uint16_t, tags::From>;
using To = crispy::boxed<uint16_t, tags::To>;
struct Range {
    From from; To to;

    // So you can do: for (auto const v: Range{3, 5}) { ... }
    struct ValueTag{};
    using iterator = crispy::boxed<uint16_t, ValueTag>;
    iterator begin() const { return iterator{from.value}; }
    auto end() const { return Value{static_cast<uint16_t>(to.value + 1)}; }
    // iterator end() const { return crispy::boxed_cast<iterator>(to) + iterator{1}; }
};

constexpr Length length(Range range) noexcept
{
    assert(range.to.value >= range.from.value);
    auto result = static_cast<Length::inner_type>(range.to.value - range.from.value);
    ++result;
    return Length{static_cast<Length::inner_type>(result)};
}

// Rectangular operations
//
namespace tags { struct Top{}; struct Left{}; struct Bottom{}; struct Right{}; }
using Top = crispy::boxed<uint16_t, tags::Top>;
using Left = crispy::boxed<uint16_t, tags::Left>;
using Bottom = crispy::boxed<uint16_t, tags::Bottom>;
using Right = crispy::boxed<uint16_t, tags::Right>;

// Rectangular screen operations
//
struct Rect { Top top; Left left; Bottom bottom; Right right; };

// Screen's page margin
//
struct PageMargin { Top top; Left left; Bottom bottom; Right right; };

constexpr Range horizontal(PageMargin m) noexcept { return Range{ From{*m.top}, To{*m.bottom} }; }
constexpr Range vertical(PageMargin m) noexcept { return Range{ From{*m.left}, To{*m.right} }; }

}
