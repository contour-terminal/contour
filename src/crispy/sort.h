/**
 * This file is part of the "libterminal" project
 *   Copyright (c) 2019-2020 Christian Parpart <christian@parpart.family>
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

#include <crispy/times.h>
#include <utility>

namespace crispy {

namespace detail
{
    template <typename Container, typename Comp, typename size_type>
    constexpr size_type partition(Container& _container, Comp _compare, size_type _low, size_type _high)
    {
        auto i = static_cast<size_t>(_low) - 1;
        auto& pivot = _container[_high];

        for (auto const j : crispy::times(static_cast<size_t>(_low), static_cast<size_t>(_high - _low)))
        {
            if (_compare(_container[j], pivot) <= 0)
            {
                i++;
                std::swap(_container[i], _container[j]);
            }
        }

        i++;
        std::swap(_container[i], _container[_high]);
        return i;
    }
}

template <typename Container, typename Comp>
constexpr void sort(Container& _container, Comp _compare, size_t _low, size_t _high)
{
    if (_low < _high)
    {
        auto const pi = detail::partition(_container, _compare, _low, _high);
        if (pi > 0)
            sort(_container, _compare, _low, pi - 1);
        sort(_container, _compare, pi + 1, _high);
    }
}

template <typename Container, typename Comp>
constexpr void sort(Container& _container, Comp _compare)
{
    if (auto const count = std::size(_container); count > 1)
        sort(_container, _compare, 0, count - 1);
}

template <typename Container>
constexpr void sort(Container& _container)
{
    if (auto const count = std::size(_container); count > 1)
        sort(_container, [](auto const& a, auto const& b) { return a < b ? -1 : a > b ? +1 : 0; }, 0, count - 1);
}

} // end namespaces
