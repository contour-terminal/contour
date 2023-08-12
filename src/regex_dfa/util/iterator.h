// This file is part of the "klex" project, http://github.com/christianparpart/klex>
//	 (c) 2018 Christian Parpart <christian@parpart.family>
//
// Licensed under the MIT License (the "License"); you may not use this
// file except in compliance with the License. You may obtain a copy of
// the License at: http://opensource.org/licenses/MIT

#pragma once

#include <regex_dfa/util/iterator-detail.h>

#include <algorithm>
#include <cstdint>
#include <sstream>
#include <type_traits>
#include <utility>
#include <vector>

namespace regex_dfa::util
{

template <typename Container>
inline auto reversed(Container&& c)
{
    if constexpr (std::is_reference<Container>::value)
        return detail::reversed<Container&> { std::forward<Container>(c) };
    else
        return detail::reversed<Container> { std::forward<Container>(c) };
}

template <typename Container>
inline auto indexed(const Container& c)
{
    return typename std::add_const<detail::indexed<const Container>>::type { c };
}

template <typename Container>
inline auto indexed(Container& c)
{
    return detail::indexed<Container> { c };
}

template <typename Container, typename Lambda>
inline auto translate(const Container& container, Lambda mapfn)
{
    using namespace std;
    using T = decltype(mapfn(*begin(container)));

    vector<T> out;
    out.reserve(distance(begin(container), end(container)));
    transform(begin(container), end(container), back_inserter(out), std::move(mapfn));

    return out;
}

template <typename Container>
inline std::string join(const Container& container, const std::string& separator = ", ")
{
    std::stringstream out;

    for (const auto&& [i, v]: indexed(container))
        if (i)
            out << separator << v;
        else
            out << v;

    return out.str();
}

template <typename T, typename Lambda>
inline auto filter(std::initializer_list<T>&& c, Lambda proc)
{
    return typename std::add_const<detail::filter<const std::initializer_list<T>, Lambda>>::type { c, proc };
}

template <typename Container, typename Lambda>
inline auto filter(const Container& c, Lambda proc)
{
    return typename std::add_const<detail::filter<const Container, Lambda>>::type { c, proc };
}

template <typename Container, typename Lambda>
inline auto filter(Container& c, Lambda proc)
{
    return detail::filter<Container, Lambda> { c, proc };
}

/**
 * Finds the last occurence of a given element satisfying @p test.
 *
 * @returns the iterator representing the last item satisfying @p test or @p end if none found.
 */
template <typename Container, typename Test>
auto find_last(const Container& container, Test test) -> decltype(std::cbegin(container))
{
    auto begin = std::cbegin(container);
    auto end = std::cend(container);

    for (auto i = std::prev(end); i != begin; --i)
        if (test(*i))
            return i;

    if (test(*begin))
        return begin;
    else
        return end;
}

} // namespace regex_dfa::util
