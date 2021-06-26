#pragma once

#include <crispy/boxed.h>
#include <cstdint>
#include <type_traits>
#include <cassert>

namespace terminal {

namespace tags { struct Column{}; }
using ColumnPosition = crispy::boxed<unsigned, tags::Column>;

namespace tags { struct Line{}; struct RelativeLine{}; struct HistoryLine{}; }
using LinePosition = crispy::boxed<unsigned, tags::Line>;
using RelativeLinePosition = crispy::boxed<int, tags::RelativeLine>;
using HistoryLinePosition = crispy::boxed<unsigned, tags::HistoryLine>;

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
struct Range { From from; To to; };

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
