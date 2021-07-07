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
#include <limits>

#include <fmt/format.h>

namespace crispy {

// {{{ forward decls
template <typename T, typename Tag> struct boxed;

namespace helper
{
    template <typename A> struct is_boxed;
    template <typename A, typename B> struct is_boxed<boxed<A, B>>;
}
// }}}

template <typename T>
constexpr bool is_boxed = helper::is_boxed<T>::value;

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
        std::is_enum_v<T> || std::is_integral_v<T> || std::is_floating_point_v<T>,
        "Boxing is only useful on integral & floating point types."
    );

    using inner_type = T;
    using element_type = T;

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

    template <typename To>
    constexpr auto as() const noexcept
    {
        if constexpr (is_boxed<To>)
            return To{static_cast<typename To::element_type>(value)};
        else
            return static_cast<To>(value);
    }

    template <typename Source>
    constexpr static boxed<T, Tag> cast_from(Source _value)
    {
        return boxed<T, Tag>(static_cast<T>(_value));
    }
};

template <typename T, typename U> constexpr T& operator++(boxed<T, U>& a) noexcept { ++a.value; return a; }
template <typename T, typename U> constexpr T& operator++(boxed<T, U>& a, int) noexcept { a.value++; return a; }
template <typename T, typename U> constexpr T& operator--(boxed<T, U>& a) noexcept { --a.value; return a; }
template <typename T, typename U> constexpr T& operator--(boxed<T, U>& a, int) noexcept { a.value--; return a; }
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

template <typename T, typename U> constexpr boxed<T, U> operator+(boxed<T, U> const& a, T b) noexcept { return boxed<T, U>{a.value + b}; }
template <typename T, typename U> constexpr boxed<T, U> operator-(boxed<T, U> const& a, T b) noexcept { return boxed<T, U>{a.value - b}; }
template <typename T, typename U> constexpr boxed<T, U> operator*(boxed<T, U> const& a, T b) noexcept { return boxed<T, U>{a.value * b}; }
template <typename T, typename U> constexpr boxed<T, U> operator/(boxed<T, U> const& a, T b) noexcept { return boxed<T, U>{a.value / b}; }

template <typename T, typename U> constexpr boxed<T, U>& operator+=(boxed<T, U>& a, boxed<T, U> const& b) noexcept { a.value += b.value; return a; }
template <typename T, typename U> constexpr boxed<T, U>& operator-=(boxed<T, U>& a, boxed<T, U> const& b) noexcept { a.value -= b.value; return a; }
template <typename T, typename U> constexpr boxed<T, U>& operator*=(boxed<T, U>& a, boxed<T, U> const& b) noexcept { a.value *= b.value; return a; }
template <typename T, typename U> constexpr boxed<T, U>& operator/=(boxed<T, U>& a, boxed<T, U> const& b) noexcept { a.value /= b.value; return a; }

namespace helper
{
    template <typename A>
    struct is_boxed
    {
        constexpr static bool value = false;
    };

    template <typename A, typename B>
    struct is_boxed<crispy::boxed<A, B>>
    {
        constexpr static bool value = true;
    };
}

} // end namespace crispy

// Casts from one boxed type to another boxed type.
template <typename To, typename From, typename FromTag>
constexpr auto boxed_cast(crispy::boxed<From, FromTag> const& from) noexcept
{
    return To{static_cast<typename To::inner_type>(from.value)};
}

// Casting a boxed type out of the box.
template <typename To, typename From, typename FromTag>
constexpr auto unbox(crispy::boxed<From, FromTag> const& from) noexcept
{
    return static_cast<To>(from.value);
}

namespace std {
    template <typename A, typename B>
    struct numeric_limits<crispy::boxed<A, B>>
    {
        using value_type = A;
        using Boxed = crispy::boxed<A, B>;

        static Boxed min() noexcept { return Boxed{std::numeric_limits<A>::min()}; }
        static Boxed max() noexcept { return Boxed{std::numeric_limits<A>::max()}; }
        static Boxed lowest() noexcept { return Boxed{std::numeric_limits<A>::lowest()}; }
        static Boxed epsilon() noexcept { return Boxed{std::numeric_limits<A>::epsilon()}; }
        static Boxed round_error() noexcept { return Boxed{std::numeric_limits<A>::round_error()}; }
        static Boxed infinity() noexcept { return Boxed{std::numeric_limits<A>::infinity()}; }
        static Boxed quiet_NaN() noexcept { return Boxed{std::numeric_limits<A>::quiet_NaN()}; }
        static Boxed signaling_NaN() noexcept { return Boxed{std::numeric_limits<A>::signaling_NaNinfinity()}; }
        static Boxed denorm_min() noexcept { return Boxed{std::numeric_limits<A>::denorm_min()}; }
    };
}

namespace fmt // {{{
{
    template <typename A, typename B>
    struct formatter<crispy::boxed<A, B>> {
        template <typename ParseContext>
        constexpr auto parse(ParseContext& ctx) { return ctx.begin(); }
        template <typename FormatContext>
        auto format(const crispy::boxed<A, B> _value, FormatContext& ctx)
        {
            return format_to(ctx.out(), "{}", _value.value);
        }
    };
}
// }}}
