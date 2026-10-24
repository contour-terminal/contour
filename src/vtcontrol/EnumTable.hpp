// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <array>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <ranges>
#include <string_view>
#include <type_traits>
#include <utility>

namespace vtcontrol
{

/// One enumerator and the spelling it has on the wire.
/// @tparam E The enum the row belongs to.
template <typename E>
struct EnumName
{
    E value;               ///< The enumerator.
    std::string_view name; ///< Its wire spelling (camelBack by convention).
};

/// The wire spelling of an enum, by specialization.
///
/// A specialization carries `static constexpr std::array Rows` of `EnumName<E>`, listed in enumerator
/// order starting at 0, so that the row index is the enumerator's value. Anything that needs the
/// spelling (JSON, the self-description, the CLI) reads this one table. Glaze learns it in Json.hpp, so
/// that this vocabulary, and every header that names it, stays free of glaze.
/// @tparam E The enum.
template <typename E>
struct EnumTable;

/// Whether `E` has an `EnumTable` specialization with rows.
template <typename E>
concept HasEnumTable = std::is_enum_v<E> && requires { EnumTable<E>::Rows; };

/// Whether the rows of `E` are listed in enumerator order from 0 and spelled uniquely.
/// @tparam E The enum.
/// @return True when `Rows[i].value == i` for every row and no two rows share a name.
template <HasEnumTable E>
[[nodiscard]] constexpr bool enumTableIsOrdered() noexcept
{
    constexpr auto const& Rows = EnumTable<E>::Rows;
    for (auto const index: std::views::iota(std::size_t { 0 }, Rows.size()))
    {
        if (static_cast<std::size_t>(Rows[index].value) != index)
            return false;
        for (auto const other: std::views::iota(index + 1, Rows.size()))
            if (Rows[index].name == Rows[other].name)
                return false;
    }
    return true;
}

/// @param value The enumerator.
/// @return Its wire spelling, or an empty view when `value` has no row.
template <HasEnumTable E>
[[nodiscard]] constexpr std::string_view nameOf(E value) noexcept
{
    static_assert(enumTableIsOrdered<E>(), "EnumTable rows must be in enumerator order from 0");
    constexpr auto const& Rows = EnumTable<E>::Rows;
    auto const index = static_cast<std::size_t>(value);
    return index < Rows.size() ? Rows[index].name : std::string_view {};
}

/// @param name A wire spelling.
/// @return The enumerator spelled `name`, or nothing.
template <HasEnumTable E>
[[nodiscard]] constexpr std::optional<E> enumFromName(std::string_view name) noexcept
{
    static_assert(enumTableIsOrdered<E>(), "EnumTable rows must be in enumerator order from 0");
    for (auto const& row: EnumTable<E>::Rows)
        if (row.name == name)
            return row.value;
    return std::nullopt;
}

} // namespace vtcontrol
