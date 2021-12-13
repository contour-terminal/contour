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
#include <tuple>
#include <utility>

/**
 * Provides a facility for function composition.
 *
 * Suppose you write code like this:
 * <code>
 *   auto result = h(g(f(x)), x);
 * </code>
 *
 * What you can do, and what is much more readable:
 * <code>
 *   auto result = f(x) >> compose(g) >> compose(h, x);
 * </code>
 *
 * @param _fun the function to be composed with.
 * @param _args zero or more function parameters to be curried with the given function.
 *              The input resulting value of the left-hand-side of the function composition
 *              will be the last parameter being passed to _fun.
 *
 * @return the result of applying all parameters to the composed function.
 */
template <typename F, typename... Args>
constexpr auto compose(F _fun, Args... _args) -> std::pair<F, std::tuple<Args...>>
{
    return std::pair { std::move(_fun), std::make_tuple(std::forward<Args>(_args)...) };
}

/**
 * Function composition operator, to be used with the compose() function.
 */
template <typename S, typename F, typename... Args>
constexpr auto operator>>(S _input, std::pair<F, std::tuple<Args...>> _chain)
{
    return std::apply(_chain.first,
                      std::tuple_cat(std::move(_chain.second), std::tuple<S> { std::move(_input) }));
}
