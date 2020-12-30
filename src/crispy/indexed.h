#pragma once

#include <utility>
#include <type_traits>

namespace crispy {

namespace impl { // {{{
    template <typename Container, typename Index>
    struct indexed {
        Container& container;
        Index start = 0;

        struct iterator {
            typename Container::iterator iter;
            Index index = 0;

            constexpr iterator& operator++()
            {
                ++iter;
                ++index;
                return *this;
            }

            constexpr iterator& operator++(int)
            {
                ++*this;
                return *this;
            }

            constexpr auto operator*() const { return std::make_pair(index, *iter); }

            constexpr bool operator==(const iterator& rhs) const noexcept { return iter == rhs.iter; }
            constexpr bool operator!=(const iterator& rhs) const noexcept { return iter != rhs.iter; }
        };

        struct const_iterator {
            typename Container::const_iterator iter;
            Index index = 0;

            constexpr const_iterator& operator++()
            {
                ++iter;
                ++index;
                return *this;
            }

            constexpr const_iterator& operator++(int)
            {
                ++*this;
                return *this;
            }

            constexpr auto operator*() const { return std::make_pair(index, *iter); }

            constexpr bool operator==(const const_iterator& rhs) const noexcept { return iter == rhs.iter; }
            constexpr bool operator!=(const const_iterator& rhs) const noexcept { return iter != rhs.iter; }
        };

        constexpr auto begin() const
        {
            if constexpr (std::is_const<Container>::value)
                return const_iterator{container.cbegin(), start};
            else
                return iterator{container.begin(), start};
        }

        constexpr auto end() const
        {
            if constexpr (std::is_const<Container>::value)
                return const_iterator{container.cend()};
            else
                return iterator{container.end()};
        }
    };
} // }}}

template <typename Container, typename Index = std::size_t>
constexpr auto indexed(Container const& c, Index _start = 0)
{
	return typename std::add_const<impl::indexed<const Container, Index>>::type{c, _start};
}

template <typename Container, typename Index = std::size_t>
constexpr auto indexed(Container& c, Index _start = 0)
{
	return impl::indexed<Container, Index>{c, _start};
}

} // end namespace
