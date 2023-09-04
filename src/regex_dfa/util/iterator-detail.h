// This file is part of the "klex" project, http://github.com/christianparpart/klex>
//	 (c) 2018 Christian Parpart <christian@parpart.family>
//
// Licensed under the MIT License (the "License"); you may not use this
// file except in compliance with the License. You may obtain a copy of
// the License at: http://opensource.org/licenses/MIT

#include <cstdint>
#include <iterator>
#include <utility>

namespace regex_dfa::util::detail
{

template <typename Container>
struct reversed
{
    const Container container;

    auto begin() { return container.crbegin(); }
    auto end() { return container.crend(); }
};

template <typename Container>
struct indexed
{
    Container& container;

    struct iterator
    {
        typename Container::iterator iter;
        std::size_t index = 0;

        iterator& operator++()
        {
            ++iter;
            ++index;
            return *this;
        }

        iterator& operator++(int)
        {
            ++*this;
            return *this;
        }

        auto operator*() const { return std::make_pair(index, *iter); }

        bool operator==(const iterator& rhs) const noexcept { return iter == rhs.iter; }
        bool operator!=(const iterator& rhs) const noexcept { return iter != rhs.iter; }
    };

    struct const_iterator
    {
        typename Container::const_iterator iter;
        std::size_t index = 0;

        const_iterator& operator++()
        {
            ++iter;
            ++index;
            return *this;
        }

        const_iterator& operator++(int)
        {
            ++*this;
            return *this;
        }

        auto operator*() const { return std::make_pair(index, *iter); }

        bool operator==(const const_iterator& rhs) const noexcept { return iter == rhs.iter; }
        bool operator!=(const const_iterator& rhs) const noexcept { return iter != rhs.iter; }
    };

    auto begin() const
    {
        if constexpr (std::is_const<Container>::value)
            return const_iterator { container.cbegin() };
        else
            return iterator { container.begin() };
    }

    auto end() const
    {
        if constexpr (std::is_const<Container>::value)
            return const_iterator { container.cend() };
        else
            return iterator { container.end() };
    }
};

template <typename Container, typename Lambda>
struct filter
{
    Container& container;
    Lambda proc;

    struct iterator
    {
        using iterator_category = std::forward_iterator_tag;
        using value_type = typename Container::value_type;
        using difference_type = long;
        using pointer = value_type*;
        using reference = value_type&;

        typename Container::iterator i;
        typename Container::iterator e;
        Lambda filter;

        auto operator*() const { return *i; }

        iterator& operator++()
        {
            ++i;
            while (i != e && !filter(*i))
                ++i;
            return *this;
        }

        iterator& operator++(int) { return ++*this; }

        bool operator==(const iterator& rhs) const noexcept { return i == rhs.i; }
        bool operator!=(const iterator& rhs) const noexcept { return !(*this == rhs); }
    };

    struct const_iterator
    {
        typename Container::const_iterator i;
        typename Container::const_iterator e;
        Lambda filter;

        auto operator*() const { return *i; }

        const_iterator& operator++()
        {
            ++i;
            while (i != e && !filter(*i))
                ++i;
            return *this;
        }

        const_iterator& operator++(int) { return ++*this; }

        bool operator==(const const_iterator& rhs) const noexcept { return i == rhs.i; }
        bool operator!=(const const_iterator& rhs) const noexcept { return !(*this == rhs); }
    };

    auto begin() const
    {
        if constexpr (std::is_const<Container>::value)
        {
            auto i = const_iterator { std::cbegin(container), std::cend(container), proc };
            while (i != end() && !proc(*i))
                ++i;
            return i;
        }
        else
        {
            auto i = iterator { std::begin(container), std::end(container), proc };
            while (i != end() && !proc(*i))
                ++i;
            return i;
        }
    }

    auto end() const
    {
        if constexpr (std::is_const<Container>::value)
            return const_iterator { std::cend(container), std::cend(container), proc };
        else
            return iterator { std::end(container), std::end(container), proc };
    }
};

} // namespace regex_dfa::util::detail
