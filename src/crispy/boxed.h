/**
 * This file is part of the "contour" project.
 *   Copyright (c) 2020 Christian Parpart <christian@parpart.family>
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

namespace crispy
{

/// boxed<T, Tag> represents a tagged and boxed type T.
///
/// Example could be: `using Column = boxed<int, 1>`
/// as well as: `using Row = boxed<int, 2>`.
///
/// So you have two integer but different boxed types that can not accidentally mixed up
/// when being used together.
template <typename T, int Tag = 0>
struct boxed
{
    T value;

    constexpr T& operator*() noexcept { return value; }
    constexpr T const& operator*() const noexcept { return value; }
};

// relation
template <typename T, int G> constexpr bool operator==(boxed<T, G> a, boxed<T, G> b) { return a.value == b.value; }
template <typename T, int G> constexpr bool operator!=(boxed<T, G> a, boxed<T, G> b) { return a.value != b.value; }
template <typename T, int G> constexpr bool operator<=(boxed<T, G> a, boxed<T, G> b) { return a.value <= b.value; }
template <typename T, int G> constexpr bool operator>=(boxed<T, G> a, boxed<T, G> b) { return a.value >= b.value; }
template <typename T, int G> constexpr bool operator<(boxed<T, G> a, boxed<T, G> b) { return a.value < b.value; }
template <typename T, int G> constexpr bool operator>(boxed<T, G> a, boxed<T, G> b) { return a.value > b.value; }

// algebraic operation
template <typename T, int G> constexpr boxed<T, G> operator+(boxed<T, G> a, boxed<T, G> b) { return boxed<T, G>{a.value + b.value}; }
template <typename T, int G> constexpr boxed<T, G> operator-(boxed<T, G> a, boxed<T, G> b) { return boxed<T, G>{a.value - b.value}; }
template <typename T, int G> constexpr boxed<T, G> operator-(boxed<T, G> a) { return boxed<T, G>{-a.value}; }
template <typename T, int G> constexpr boxed<T, G> operator*(boxed<T, G> a, boxed<T, G> b) { return boxed<T, G>{a.value * b.value}; }
template <typename T, int G> constexpr boxed<T, G> operator/(boxed<T, G> a, boxed<T, G> b) { return boxed<T, G>{a.value / b.value}; }

}
