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

#include <type_traits>
#include <utility>

namespace crispy
{

namespace impl
{ // {{{
    template <typename Container, typename Index>
    struct indexed // NOLINT(readability-identifier-naming)
    {
        using container_type = std::remove_const_t<std::remove_reference_t<Container>>;

        Container container;
        Index start;

        struct iterator // NOLINT(readability-identifier-naming)
        {               // {{{
            using inner_type = decltype(std::declval<container_type>().begin());
            typename container_type::iterator iter;
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

            constexpr auto operator*() const { return std::pair { index, *iter }; }

            constexpr bool operator==(const iterator& rhs) const noexcept { return iter == rhs.iter; }
            constexpr bool operator!=(const iterator& rhs) const noexcept { return iter != rhs.iter; }
        };
        // }}}
        struct const_iterator // NOLINT(readability-identifier-naming)
        {                     // {{{
            typename container_type::const_iterator iter;
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

            constexpr auto operator*() const { return std::make_pair(Index { index }, *iter); }

            constexpr bool operator==(const const_iterator& rhs) const noexcept { return iter == rhs.iter; }
            constexpr bool operator!=(const const_iterator& rhs) const noexcept { return iter != rhs.iter; }
        };
        // }}}

        [[nodiscard]] constexpr auto begin() const { return const_iterator { container.cbegin(), start }; }
        [[nodiscard]] constexpr auto end() const { return const_iterator { container.cend() }; }

        [[nodiscard]] constexpr auto begin()
        {
            return iterator { const_cast<container_type&>(container).begin(), start };
        }
        [[nodiscard]] constexpr auto end()
        {
            return iterator { const_cast<container_type&>(container).end() };
        }
    };
} // namespace impl

template <typename Container, typename Index = std::size_t>
constexpr auto indexed(Container&& c, Index start = 0)
{
    if constexpr (std::is_const_v<Container>)
        return std::add_const_t<impl::indexed<Container, Index>> { std::forward<Container>(c), start };
    else
        return impl::indexed<Container, Index> { std::forward<Container>(c), start };
}

} // namespace crispy
