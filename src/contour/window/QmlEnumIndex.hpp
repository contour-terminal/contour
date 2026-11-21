// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <cstddef>
#include <utility>

namespace contour::window
{

/// Whether @p value, as QML passes an enum (its underlying value, an int), names one of an enum's
/// @p count enumerators counted from 0, and so indexes a table of @p count rows.
/// @param value The int QML handed over.
/// @param count How many enumerators the enum has.
/// @return Whether @p value is one of them.
[[nodiscard]] constexpr bool inRange(int value, std::size_t count) noexcept
{
    return value >= 0 && std::cmp_less(value, count);
}

static_assert(inRange(0, 1));
static_assert(!inRange(1, 1));
static_assert(!inRange(-1, 1));
static_assert(!inRange(0, 0));

} // namespace contour::window
