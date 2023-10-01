// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <vtbackend/CellFlags.h>
#include <vtbackend/Color.h>

#include <type_traits>
#include <utility>

namespace vtbackend
{

/// Character graphics rendition information.
struct GraphicsAttributes
{
    Color foregroundColor { DefaultColor() };
    Color backgroundColor { DefaultColor() };
    Color underlineColor { DefaultColor() };
    CellFlags flags {};
};

static_assert(std::is_trivially_copy_assignable_v<GraphicsAttributes>);

constexpr bool operator==(GraphicsAttributes const& a, GraphicsAttributes const& b) noexcept
{
    return a.backgroundColor == b.backgroundColor && a.foregroundColor == b.foregroundColor
           && a.flags == b.flags && a.underlineColor == b.underlineColor;
}

constexpr bool operator!=(GraphicsAttributes const& a, GraphicsAttributes const& b) noexcept
{
    return !(a == b);
}

} // namespace vtbackend
