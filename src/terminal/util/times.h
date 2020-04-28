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
#include <functional>
#include <type_traits>

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

template<
    typename I,
    typename T,
    typename Callable,
    typename std::enable_if_t<std::is_invocable_r_v<void, Callable, T>, int> = 0
>
constexpr void operator|(_Times<I, T> _times, Callable _callable)
{
    for (auto && i : _times)
        _callable(i);
}

template<
    typename I,
    typename T,
    typename Callable,
    typename std::enable_if_t<std::is_invocable_r_v<void, Callable>, int> = 0
>
constexpr void operator|(_Times<I, T> _times, Callable _callable)
{
    for ([[maybe_unused]] auto && i : _times)
        _callable();
}

// ---------------------------------------------------------------------------------------------------

template <typename I, typename T1, typename T2>
struct _Times2DIterator {
    using Outer = _Times<I, T1>;
    using Inner = _Times<I, T2>;

    Outer first;
    Inner second;
    typename Outer::iterator outerIt;
    typename Inner::iterator innerIt;

    constexpr _Times2DIterator(Outer _outer, Inner _inner, bool _init) noexcept :
        first{ std::move(_outer) },
        second{ std::move(_inner) },
        outerIt{ _init ? std::begin(first) : std::end(first) },
        innerIt{ _init ? std::begin(second) : std::end(second) }
    {}

    using value_type = std::tuple<T1, T2>;
    constexpr value_type operator*() const noexcept { return {*outerIt, *innerIt}; }

    constexpr _Times2DIterator<I, T1, T2>& operator++() noexcept {
        ++innerIt;
        if (innerIt == std::end(second)) {
            ++outerIt;
            if (outerIt != std::end(first))
                innerIt = std::begin(second);
        }
        return *this;
    }

    constexpr _Times2DIterator<I, T1, T2>& operator++(int) noexcept { return *++this; }

    constexpr bool operator==(_Times2DIterator<I, T1, T2> const& other) const noexcept {
        return innerIt == other.innerIt;
        //return outerIt == other.outerIt && innerIt == other.innerIt;
    }

    constexpr bool operator!=(_Times2DIterator<I, T1, T2> const& other) const noexcept {
        return !(*this == other);
    }
};

template <typename I, typename T1, typename T2>
struct _Times2D
{
    _Times<I, T1> first;
    _Times<I, T2> second;

    using iterator = _Times2DIterator<I, T1, T2>;

    constexpr std::size_t size() const noexcept { return first.size() * second.size(); }
    constexpr auto operator[](std::size_t i) const noexcept { return second[i % second.size()]; }

    constexpr iterator begin() const noexcept { return iterator{first, second, true}; }
    constexpr iterator end() const noexcept { return iterator{first, second, false}; }
};

template <typename I, typename T1, typename T2>
constexpr auto begin(_Times2D<I, T1, T2> const& _times) noexcept { return _times.begin(); }

template <typename I, typename T1, typename T2>
constexpr auto end(_Times2D<I, T1, T2> const& _times) noexcept { return _times.end(); }

template <typename I, typename T1, typename T2>
constexpr auto begin(_Times2D<I, T1, T2>& _times) noexcept { return _times.begin(); }

template <typename I, typename T1, typename T2>
constexpr auto end(_Times2D<I, T1, T2>& _times) noexcept { return _times.end(); }

template <typename I, typename T1, typename T2>
constexpr inline _Times2D<I, T1, T2> operator*(_Times<I, T1> a, _Times<I, T2> b)
{
    return _Times2D<I, T1, T2>{std::move(a), std::move(b)};
}

template<
    typename I,
    typename T1,
    typename T2,
    typename Callable,
              typename std::enable_if_t<
                  std::is_invocable_r_v<void, Callable, T1, T2>,
                  int> = 0
>
constexpr void operator|(_Times2D<I, T1, T2> _times, Callable _callable)
{
    for (auto && [i, j] : _times)
        _callable(i, j);
}

} // end namespace
