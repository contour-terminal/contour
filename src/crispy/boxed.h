/**
 * This file is part of the "libterminal" project
 *   Copyright (c) 2020-2021 Christian Parpart <christian@parpart.family>
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

#include <cstdint>
#include <type_traits>
#include <cassert>

namespace crispy {

/**
 * Wrapper to provide strong typing on primitive types.
 *
 * You must provide a unique tag (an empty struct) to each boxed
 * type to ensure uniqueness of this type.
 *
 * @code
 * namespace tags { struct Length{}; }
 * using Length = boxed<std::size_t, tags::Length>;
 * @endcode
 */
template <typename T, typename Tag> struct boxed
{
    static_assert(
        std::is_integral_v<T> || std::is_floating_point_v<T>,
        "Boxing is only useful on integral & floating point types."
    );

    using inner_type = T;

    constexpr boxed(): value{} {}
    constexpr explicit boxed(T _value) noexcept: value{_value} {}
    constexpr boxed(boxed const&) = default;
    constexpr boxed& operator=(boxed const&) = default;
    constexpr boxed(boxed&&) noexcept = default;
    constexpr boxed& operator=(boxed &&) noexcept = default;
    ~boxed() = default;

    T value;

    constexpr T& get() noexcept { return value; }
    constexpr T const& get() const noexcept { return value; }
};

template <typename T, typename U> constexpr T& operator++(boxed<T, U>& a) noexcept { ++a; return a; }
template <typename T, typename U> constexpr T& operator++(boxed<T, U>& a, int) noexcept { a++; return a; }
template <typename T, typename U> constexpr T const& operator*(boxed<T, U> const& a) noexcept { return a.value; }
template <typename T, typename U> constexpr bool operator<(boxed<T, U> const& a, boxed<T, U> const& b) noexcept { return a.value < b.value; }
template <typename T, typename U> constexpr bool operator>(boxed<T, U> const& a, boxed<T, U> const& b) noexcept { return a.value > b.value; }
template <typename T, typename U> constexpr bool operator<=(boxed<T, U> const& a, boxed<T, U> const& b) noexcept { return a.value <= b.value; }
template <typename T, typename U> constexpr bool operator>=(boxed<T, U> const& a, boxed<T, U> const& b) noexcept { return a.value >= b.value; }
template <typename T, typename U> constexpr bool operator==(boxed<T, U> const& a, boxed<T, U> const& b) noexcept { return a.value == b.value; }
template <typename T, typename U> constexpr bool operator!=(boxed<T, U> const& a, boxed<T, U> const& b) noexcept { return a.value != b.value; }
template <typename T, typename U> constexpr bool operator!(boxed<T, U> const& a) noexcept { return !a.value; }
template <typename T, typename U> constexpr boxed<T, U> operator+(boxed<T, U> const& a, boxed<T, U> const& b) noexcept { return boxed<T, U>{a.value + b.value}; }
template <typename T, typename U> constexpr boxed<T, U> operator-(boxed<T, U> const& a, boxed<T, U> const& b) noexcept { return boxed<T, U>{a.value - b.value}; }
template <typename T, typename U> constexpr boxed<T, U> operator*(boxed<T, U> const& a, boxed<T, U> const& b) noexcept { return boxed<T, U>{a.value * b.value}; }
template <typename T, typename U> constexpr boxed<T, U> operator/(boxed<T, U> const& a, boxed<T, U> const& b) noexcept { return boxed<T, U>{a.value / b.value}; }

template <typename From, typename FromTag, typename To, typename ToTag>
constexpr auto boxed_cast(boxed<From, FromTag> const& from) noexcept
{
    return boxed<To, ToTag>{from.value};
}

}
