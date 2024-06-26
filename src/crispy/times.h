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
    struct times_iterator
    {
        T start;
        I count;
        T step;
        T current;

        constexpr T operator*() noexcept { return current; }
        constexpr T const& operator*() const noexcept { return current; }

        constexpr times_iterator<I, T>& operator++() noexcept
        {
            current += step;
            --count;
            return *this;
        }
        constexpr times_iterator<I, T>& operator++(int) noexcept { return ++*this; }

        constexpr times_iterator<I, T>& operator--() noexcept
        {
            current -= step;
            ++count;
            return *this;
        }
        constexpr times_iterator<I, T>& operator--(int) noexcept { return ++*this; }

        constexpr bool operator==(times_iterator<I, T> const& other) const noexcept
        {
            return count == other.count;
        }
        constexpr bool operator!=(times_iterator<I, T> const& other) const noexcept
        {
            return count != other.count;
        }
    };

    template <typename I, typename T>
    struct times
    {
        T start;
        I count;
        T step;

        using iterator = times_iterator<I, T>;

        [[nodiscard]] constexpr std::size_t size() const noexcept { return count; }
        constexpr T operator[](size_t i) const noexcept { return start + i * step; }

        [[nodiscard]] constexpr iterator begin() const noexcept
        {
            return times_iterator<I, T> { start, count, step, start };
        }

        [[nodiscard]] constexpr iterator end() const noexcept
        {
            return iterator { start, I {}, step, static_cast<T>(start + count * step) };
        }
    };

    template <typename I, typename T>
    times(T, I, T) -> times<I, T>;

    template <typename I, typename T>
    constexpr auto begin(times<I, T> const& times) noexcept
    {
        return times.begin();
    }
    template <typename I, typename T>
    constexpr auto end(times<I, T> const& times) noexcept
    {
        return times.end();
    }

    template <typename I, typename T>
    constexpr auto begin(times<I, T>& times) noexcept
    {
        return times.begin();
    }
    template <typename I, typename T>
    constexpr auto end(times<I, T>& times) noexcept
    {
        return times.end();
    }

    template <typename I, typename T1, typename T2>
    struct times_2d_iterator
    {
        using outer = times<I, T1>;
        using inner = times<I, T2>;

        outer first;
        inner second;
        typename outer::iterator outerIt;
        typename inner::iterator innerIt;

        constexpr times_2d_iterator(outer outer, inner inner, bool init) noexcept:
            first { std::move(outer) },
            second { std::move(inner) },
            outerIt { init ? std::begin(first) : std::end(first) },
            innerIt { init ? std::begin(second) : std::end(second) }
        {
        }

        using value_type = std::tuple<T1, T2>;
        constexpr value_type operator*() const noexcept { return { *outerIt, *innerIt }; }

        constexpr times_2d_iterator<I, T1, T2>& operator++() noexcept
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

        constexpr times_2d_iterator<I, T1, T2>& operator++(int) noexcept { return *++this; }

        constexpr bool operator==(times_2d_iterator<I, T1, T2> const& other) const noexcept
        {
            return innerIt == other.innerIt;
            // return outerIt == other.outerIt && innerIt == other.innerIt;
        }

        constexpr bool operator!=(times_2d_iterator<I, T1, T2> const& other) const noexcept
        {
            return !(*this == other);
        }
    };

    template <typename I, typename T1, typename T2>
    struct times_2d
    {
        times<I, T1> first;
        times<I, T2> second;

        using iterator = times_2d_iterator<I, T1, T2>;

        [[nodiscard]] constexpr std::size_t size() const noexcept { return first.size() * second.size(); }
        constexpr auto operator[](std::size_t i) const noexcept { return second[i % second.size()]; }

        [[nodiscard]] constexpr iterator begin() const noexcept { return iterator { first, second, true }; }
        [[nodiscard]] constexpr iterator end() const noexcept { return iterator { first, second, false }; }
    };

    template <typename I, typename T1, typename T2>
    constexpr auto begin(detail::times_2d<I, T1, T2> const& times) noexcept
    {
        return times.begin();
    }

    template <typename I, typename T1, typename T2>
    constexpr auto end(detail::times_2d<I, T1, T2> const& times) noexcept
    {
        return times.end();
    }

    template <typename I, typename T1, typename T2>
    constexpr auto begin(detail::times_2d<I, T1, T2>& times) noexcept
    {
        return times.begin();
    }

    template <typename I, typename T1, typename T2>
    constexpr auto end(detail::times_2d<I, T1, T2>& times) noexcept
    {
        return times.end();
    }

    template <typename I, typename T1, typename T2>
    constexpr inline detail::times_2d<I, T1, T2> operator*(detail::times<I, T1> a, detail::times<I, T2> b)
    {
        return detail::times_2d<I, T1, T2> { std::move(a), std::move(b) };
    }

    template <typename I, typename T, typename Callable>
        requires std::is_invocable_r_v<void, Callable>
    constexpr void operator|(detail::times<I, T> times, Callable callable)
    {
        for ([[maybe_unused]] auto&& i: times)
            callable();
    }

    template <typename I, typename T, typename Callable>
        requires std::is_invocable_r_v<void, Callable, T>
    constexpr void operator|(detail::times<I, T> times, Callable callable)
    {
        for (auto&& i: times)
            callable(i);
    }

    // ---------------------------------------------------------------------------------------------------

    template <typename I, typename T1, typename T2, typename Callable>
        requires std::is_invocable_v<Callable, T1, T2>
    constexpr void operator|(detail::times_2d<I, T1, T2> times, Callable callable)
    {
        for (auto&& [i, j]: times)
            callable(i, j);
    }

} // namespace detail

template <typename I, typename T>
constexpr inline detail::times<I, T> times(T start, I count, T step = T(1))
{
    return detail::times<I, T> { start, count, step };
}

template <typename T>
constexpr inline detail::times<T, T> times(T count)
{
    return detail::times<T, T> { T(0), count, T(1) };
}

template <typename T>
constexpr inline detail::times_2d<T, T, T> times2D(T a, T b)
{
    return detail::times_2d<T, T, T> { std::move(a), std::move(b) };
}

} // namespace crispy
