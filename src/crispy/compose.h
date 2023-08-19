// SPDX-License-Identifier: Apache-2.0
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
constexpr auto compose(F fun, Args... args) -> std::pair<F, std::tuple<Args...>>
{
    return std::pair { std::move(fun), std::make_tuple(std::forward<Args>(args)...) };
}

/**
 * Function composition operator, to be used with the compose() function.
 */
template <typename S, typename F, typename... Args>
constexpr auto operator>>(S input, std::pair<F, std::tuple<Args...>> chain)
{
    return std::apply(chain.first,
                      std::tuple_cat(std::move(chain.second), std::tuple<S> { std::move(input) }));
}
