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

#include <algorithm>

namespace crispy
{

// XXX Some C++20 backports

template <typename Container, typename Pred>
auto find_if(Container&& container, Pred&& pred)
{
    return std::find_if(begin(container), end(container), std::forward<Pred>(pred));
}

template <typename Container, typename Fn>
constexpr bool any_of(Container&& container, Fn&& fn)
{
    return std::any_of(begin(container), end(container), std::forward<Fn>(fn));
}

template <typename Container, typename Fn>
bool none_of(Container&& container, Fn&& fn)
{
    return std::none_of(begin(container), end(container), std::forward<Fn>(fn));
}

template <typename ExecutionPolicy, typename Container, typename Fn>
bool any_of(ExecutionPolicy ep, Container&& container, Fn&& fn)
{
    return std::any_of(ep, begin(container), end(container), std::forward<Fn>(fn));
}

template <typename Container, typename OutputIterator>
void copy(Container&& container, OutputIterator outputIterator)
{
    std::copy(std::begin(container), std::end(container), outputIterator);
}

template <typename Container, typename Fn>
void for_each(Container&& container, Fn&& fn)
{
    std::for_each(begin(container), end(container), std::forward<Fn>(fn));
}

template <typename ExecutionPolicy, typename Container, typename Fn>
void for_each(ExecutionPolicy ep, Container&& container, Fn&& fn)
{
    std::for_each(ep, begin(container), end(container), std::forward<Fn>(fn));
}

template <typename Container, typename T>
auto count(Container&& container, T&& value)
{
    return std::count(begin(container), end(container), std::forward<T>(value));
}

} // namespace crispy
