/**
 * This file is part of the "libterminal" project
 *   Copyright (c) 2019 Christian Parpart <christian@parpart.family>
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

#include <cstddef>
#include <cstdint>

namespace terminal::support {

template <typename I, typename T>
struct _TimesIterator {
    T start;
    I count;
    T step;
    T current;

    constexpr T operator*() noexcept { return current; }
    constexpr T const& operator*() const noexcept { return current; }

    constexpr _TimesIterator<I, T>& operator++() noexcept { current += step; --count; return *this; }
    constexpr _TimesIterator<I, T>& operator++(int) noexcept { return ++*this; }

    constexpr _TimesIterator<I, T>& operator--() noexcept { current -= step; ++count; return *this; }
    constexpr _TimesIterator<I, T>& operator--(int) noexcept { return ++*this; }

    constexpr bool operator==(_TimesIterator<I, T> const& other) const noexcept { return count == other.count; }
    constexpr bool operator!=(_TimesIterator<I, T> const& other) const noexcept { return count != other.count; }
};

template <typename I, typename T>
struct _Times {
    T start;
    I count;
    T step;

    using iterator = _TimesIterator<I, T>;

    constexpr std::size_t size() const noexcept { return count; }
    constexpr T operator[](size_t i) const noexcept { return start + i * step; }

    constexpr iterator begin() const noexcept { return _TimesIterator<I, T>{start, count, step, start}; }

    constexpr iterator end() const noexcept {
        return iterator{
            start,
            I{},
            step,
            static_cast<T>(start + count * step)
        };
    }
};

template <typename I, typename T> _Times(T, I, T) -> _Times<I, T>;

template <typename I, typename T> constexpr auto begin(_Times<I, T> const& _times) noexcept { return _times.begin(); }
template <typename I, typename T> constexpr auto end(_Times<I, T> const& _times) noexcept { return _times.end(); }

template <typename I, typename T> constexpr auto begin(_Times<I, T>& _times) noexcept { return _times.begin(); }
template <typename I, typename T> constexpr auto end(_Times<I, T>& _times) noexcept { return _times.end(); }

// TODO: give random access hints to STL algorithms

template <typename I, typename T>
constexpr inline _Times<I, T> times(T start, I count, T step = 1)
{
    return _Times<I, T>{start, count, step};
}

template <typename T>
constexpr inline _Times<T, T> times(T count)
{
    return _Times<T, T>{0, count, 1};
}

} // end namespace
