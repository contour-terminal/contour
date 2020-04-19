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

#include <algorithm>

namespace terminal::support {

// XXX Some C++20 backports

template <typename Container, typename Fn>
bool any_of(Container && _container, Fn && _fn)
{
    return std::any_of(begin(_container), end(_container), std::forward<Fn>(_fn));
}

template <typename ExecutionPolicy, typename Container, typename Fn>
bool any_of(ExecutionPolicy _ep, Container && _container, Fn && _fn)
{
    return std::any_of(_ep, begin(_container), end(_container), std::forward<Fn>(_fn));
}

template <typename Container, typename Fn>
void for_each(Container && _container, Fn && _fn)
{
    std::for_each(begin(_container), end(_container), std::forward<Fn>(_fn));
}

template <typename ExecutionPolicy, typename Container, typename Fn>
void for_each(ExecutionPolicy _ep, Container && _container, Fn && _fn)
{
    std::for_each(_ep, begin(_container), end(_container), std::forward<Fn>(_fn));
}

} // end namespace
