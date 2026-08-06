// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <type_traits>

namespace crispy
{

namespace detail
{
    template <typename I, typename T>
    struct TimesIterator
    {
        T start;
        I count;
        T step;
        T current;

        constexpr T operator*() noexcept { return current; }
        constexpr T const& operator*() const noexcept { return current; }

        constexpr TimesIterator<I, T>& operator++() noexcept
        {
            current += step;
            --count;
            return *this;
        }
        constexpr TimesIterator<I, T>& operator++(int) noexcept { return ++*this; }

        constexpr TimesIterator<I, T>& operator--() noexcept
        {
            current -= step;
            ++count;
            return *this;
        }
        constexpr TimesIterator<I, T>& operator--(int) noexcept { return ++*this; }

        constexpr bool operator==(TimesIterator<I, T> const& other) const noexcept
        {
            return count == other.count;
        }
        constexpr bool operator!=(TimesIterator<I, T> const& other) const noexcept
        {
            return count != other.count;
        }
    };

    template <typename I, typename T>
    struct Times
    {
        T start;
        I count;
        T step;

        using iterator = TimesIterator<I, T>;

        [[nodiscard]] constexpr std::size_t size() const noexcept { return count; }
        constexpr T operator[](size_t i) const noexcept { return start + (i * step); }

        [[nodiscard]] constexpr iterator begin() const noexcept
        {
            return TimesIterator<I, T> { start, count, step, start };
        }

        [[nodiscard]] constexpr iterator end() const noexcept
        {
            return iterator { start, I {}, step, static_cast<T>(start + (count * step)) };
        }
    };

    template <typename I, typename T>
    Times(T, I, T) -> Times<I, T>;

    template <typename I, typename T>
    constexpr auto begin(Times<I, T> const& times) noexcept
    {
        return times.begin();
    }
    template <typename I, typename T>
    constexpr auto end(Times<I, T> const& times) noexcept
    {
        return times.end();
    }

    template <typename I, typename T>
    constexpr auto begin(Times<I, T>& times) noexcept
    {
        return times.begin();
    }
    template <typename I, typename T>
    constexpr auto end(Times<I, T>& times) noexcept
    {
        return times.end();
    }

    template <typename I, typename T1, typename T2>
    struct Times2DIterator
    {
        using Outer = Times<I, T1>;
        using Inner = Times<I, T2>;

        Outer first;
        Inner second;
        Outer::iterator outerIt;
        Inner::iterator innerIt;

        constexpr Times2DIterator(Outer outer, Inner inner, bool init) noexcept:
            first { std::move(outer) },
            second { std::move(inner) },
            outerIt { init ? std::begin(first) : std::end(first) },
            innerIt { init ? std::begin(second) : std::end(second) }
        {
        }

        using value_type = std::tuple<T1, T2>;
        constexpr value_type operator*() const noexcept { return { *outerIt, *innerIt }; }

        constexpr Times2DIterator<I, T1, T2>& operator++() noexcept
        {
            ++innerIt;
            if (innerIt == std::end(second))
            {
                ++outerIt;
                if (outerIt != std::end(first))
                    innerIt = std::begin(second);
            }
            return *this;
        }

        constexpr Times2DIterator<I, T1, T2>& operator++(int) noexcept { return ++*this; }

        constexpr bool operator==(Times2DIterator<I, T1, T2> const& other) const noexcept
        {
            return innerIt == other.innerIt;
            // return outerIt == other.outerIt && innerIt == other.innerIt;
        }

        constexpr bool operator!=(Times2DIterator<I, T1, T2> const& other) const noexcept
        {
            return !(*this == other);
        }
    };

    template <typename I, typename T1, typename T2>
    struct Times2D
    {
        Times<I, T1> first;
        Times<I, T2> second;

        using iterator = Times2DIterator<I, T1, T2>;

        [[nodiscard]] constexpr std::size_t size() const noexcept { return first.size() * second.size(); }
        constexpr auto operator[](std::size_t i) const noexcept { return second[i % second.size()]; }

        [[nodiscard]] constexpr iterator begin() const noexcept { return iterator { first, second, true }; }
        [[nodiscard]] constexpr iterator end() const noexcept { return iterator { first, second, false }; }
    };

    template <typename I, typename T1, typename T2>
    constexpr auto begin(detail::Times2D<I, T1, T2> const& times) noexcept
    {
        return times.begin();
    }

    template <typename I, typename T1, typename T2>
    constexpr auto end(detail::Times2D<I, T1, T2> const& times) noexcept
    {
        return times.end();
    }

    template <typename I, typename T1, typename T2>
    constexpr auto begin(detail::Times2D<I, T1, T2>& times) noexcept
    {
        return times.begin();
    }

    template <typename I, typename T1, typename T2>
    constexpr auto end(detail::Times2D<I, T1, T2>& times) noexcept
    {
        return times.end();
    }

    template <typename I, typename T1, typename T2>
    constexpr detail::Times2D<I, T1, T2> operator*(detail::Times<I, T1> a, detail::Times<I, T2> b)
    {
        return detail::Times2D<I, T1, T2> { std::move(a), std::move(b) };
    }

    template <typename I, typename T, typename Callable>
        requires std::is_invocable_r_v<void, Callable>
    constexpr void operator|(detail::Times<I, T> times, Callable callable)
    {
        for ([[maybe_unused]] auto&& i: times)
            callable();
    }

    template <typename I, typename T, typename Callable>
        requires std::is_invocable_r_v<void, Callable, T>
    constexpr void operator|(detail::Times<I, T> times, Callable callable)
    {
        for (auto&& i: times)
            callable(i);
    }

    // ---------------------------------------------------------------------------------------------------

    template <typename I, typename T1, typename T2, typename Callable>
        requires std::is_invocable_v<Callable, T1, T2>
    constexpr void operator|(detail::Times2D<I, T1, T2> times, Callable callable)
    {
        for (auto&& [i, j]: times)
            callable(i, j);
    }

} // namespace detail

template <typename I, typename T>
constexpr detail::Times<I, T> times(T start, I count, T step = T(1))
{
    return detail::Times<I, T> { start, count, step };
}

template <typename T>
constexpr detail::Times<T, T> times(T count)
{
    return detail::Times<T, T> { T(0), count, T(1) };
}

template <typename T>
constexpr detail::Times2D<T, T, T> times2D(T a, T b)
{
    return detail::Times2D<T, T, T> { std::move(a), std::move(b) };
}

} // namespace crispy
