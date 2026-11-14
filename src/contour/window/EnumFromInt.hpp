// SPDX-License-Identifier: Apache-2.0
#pragma once

/// @file
/// A checked conversion from the int QML hands over to an enumerator, driven by the table that lists the
/// enumerators: the range is the table's, so a new enumerator is one row and cannot be clamped away.

#include <algorithm>
#include <array>
#include <cstddef>
#include <functional>
#include <optional>
#include <ranges>
#include <type_traits>

namespace contour::window
{

/// @param rows       A table with a row per enumerator, in order of value.
/// @param projection Reads the enumerator a row stands for.
/// @return Whether each row's enumerator has its position as its value, from 0: what makes a table indexable
///         by the enum, and @c enumFromInt right.
template <typename Row, std::size_t Count, typename Projection>
[[nodiscard]] constexpr bool enumValuesAreDense(std::array<Row, Count> const& rows,
                                                Projection projection) noexcept
{
    return std::ranges::all_of(std::views::iota(std::size_t { 0 }, Count), [&](std::size_t position) {
        return static_cast<std::size_t>(std::invoke(projection, rows[position])) == position;
    });
}

/// @param values Every enumerator of the enum, in order of value.
/// @return Whether each one's value is its position, from 0: what makes @c enumFromInt right.
template <typename Enum, std::size_t Count>
[[nodiscard]] constexpr bool enumValuesAreDense(std::array<Enum, Count> const& values) noexcept
{
    return enumValuesAreDense(values, std::identity {});
}

/// @param values Every enumerator of the enum, in order of value (@c enumValuesAreDense).
/// @param value  An enumerator's underlying value, as QML hands it over.
/// @return The enumerator, or nothing for a value that is none of them.
template <typename Enum, std::size_t Count>
[[nodiscard]] constexpr std::optional<Enum> enumFromInt(std::array<Enum, Count> const& values,
                                                        int value) noexcept
{
    if (value < 0 || static_cast<std::size_t>(value) >= Count)
        return std::nullopt;
    return values[static_cast<std::size_t>(value)];
}

} // namespace contour::window
