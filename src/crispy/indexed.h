#pragma once

#include <utility>
#include <type_traits>

namespace crispy {

namespace impl { // {{{
    template <typename Container>
    struct indexed {
        Container& container;

        struct iterator {
            typename Container::iterator iter;
            std::size_t index = 0;

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
            std::size_t index = 0;

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
                return const_iterator{container.cbegin()};
            else
                return iterator{container.begin()};
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

template <typename Container>
constexpr auto indexed(Container const& c)
{
	return typename std::add_const<impl::indexed<const Container>>::type{c};
}

template <typename Container>
constexpr auto indexed(Container& c)
{
	return impl::indexed<Container>{c};
}

} // end namespace
