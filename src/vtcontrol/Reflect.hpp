// SPDX-License-Identifier: Apache-2.0
#pragma once

/// @file
/// The fields of an API record, by reflection: how many there are, their types, their names, and a reference
/// to each. Like Json.hpp it names glaze, so include it only from `.cpp` files and from headers private to a
/// module that links glaze itself -- never from a header that the GUI or a public interface sees.
///
/// Glaze reflects an aggregate through `glz::to_tie` and counts its fields with `glz::detail::count_members`,
/// which its `detail` namespace documents as unstable; this header is the one place outside Json.hpp that
/// depends on them, so a glaze bump re-checks here.

#include <cstddef>
#include <optional>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

#include <glaze/core/reflect.hpp>

namespace vtcontrol::reflect
{

/// The number of fields of the aggregate `T`.
template <typename T>
inline constexpr std::size_t FieldCount = glz::detail::count_members<T>;

/// The type of field @p I of the aggregate `T`, without reference or cv-qualifiers.
template <typename T, std::size_t I>
using FieldType = std::remove_cvref_t<decltype(glz::get<I>(glz::to_tie(std::declval<T&>())))>;

/// The name of field @p I of the aggregate `T`, spelled as in the source.
template <typename T, std::size_t I>
inline constexpr std::string_view FieldName = glz::member_names<T>[I];

/// @param value An aggregate.
/// @return Field @p I of @p value.
template <std::size_t I, typename T>
[[nodiscard]] constexpr FieldType<T, I>& field(T& value) noexcept
{
    auto tied = glz::to_tie(value);
    return glz::get<I>(tied);
}

/// @param value An aggregate.
/// @return Field @p I of @p value.
template <std::size_t I, typename T>
[[nodiscard]] constexpr FieldType<T, I> const& field(T const& value) noexcept
{
    auto tied = glz::to_tie(value);
    return glz::get<I>(tied);
}

/// Whether `T` is a `std::optional`.
template <typename>
inline constexpr bool IsOptional = false;
template <typename T>
inline constexpr bool IsOptional<std::optional<T>> = true;

/// Whether `T` is a `std::vector`.
template <typename>
inline constexpr bool IsVector = false;
template <typename T>
inline constexpr bool IsVector<std::vector<T>> = true;

/// The unqualified name of the type `T`, as the compiler spells it (`SplitRequest`).
template <typename T>
inline constexpr std::string_view TypeName = [] {
    constexpr auto Qualified = std::string_view { glz::type_name<T> };
    constexpr auto Colon = Qualified.rfind(':');
    return Colon == std::string_view::npos ? Qualified : Qualified.substr(Colon + 1);
}();

} // namespace vtcontrol::reflect
