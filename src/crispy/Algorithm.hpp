// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <algorithm>
// std::begin/std::end, and the unqualified begin()/end() below: a C array has no associated
// namespace for ADL to find them in, so <iterator> has to be here rather than arriving by luck
// through whatever a consumer happened to include first.
#include <iterator>

namespace crispy
{

// XXX Some C++20 backports

template <typename Container, typename Pred>
// NOLINTNEXTLINE(readability-identifier-naming): mirrors the std:: spelling on purpose.
auto find_if(Container const& container, Pred&& pred)
{
    return std::find_if(begin(container), end(container), std::forward<Pred>(pred));
}

template <typename Container, typename Fn>
// NOLINTNEXTLINE(readability-identifier-naming): mirrors the std:: spelling on purpose.
constexpr bool any_of(Container const& container, Fn&& fn)
{
    return std::any_of(begin(container), end(container), std::forward<Fn>(fn));
}

template <typename Container, typename Fn>
// NOLINTNEXTLINE(readability-identifier-naming): mirrors the std:: spelling on purpose.
bool none_of(Container const& container, Fn&& fn)
{
    return std::none_of(begin(container), end(container), std::forward<Fn>(fn));
}

template <typename ExecutionPolicy, typename Container, typename Fn>
// NOLINTNEXTLINE(readability-identifier-naming): mirrors the std:: spelling on purpose.
bool any_of(ExecutionPolicy ep, Container&& container, Fn&& fn)
{
    return std::any_of(ep,
                       begin(std::forward<Container>(container)),
                       end(std::forward<Container>(container)),
                       std::forward<Fn>(fn));
}

template <typename Container, typename OutputIterator>
// NOLINTNEXTLINE(readability-identifier-naming): mirrors the std:: spelling on purpose.
void copy(Container const& container, OutputIterator outputIterator)
{
    std::copy(std::begin(container), std::end(container), outputIterator);
}

template <typename Container, typename Fn>
// NOLINTNEXTLINE(readability-identifier-naming): mirrors the std:: spelling on purpose.
void for_each(Container&& container, Fn fn)
{
    // Bound to a named reference so the range is expanded once: forwarding into both begin() and
    // end() would read `container` after moving from it. std::ranges::for_each is not used because
    // it rejects a forwarded rvalue that is not a borrowed_range.
    auto&& range = std::forward<Container>(container);
    std::for_each(begin(range), end(range), fn);
}

template <typename ExecutionPolicy, typename Container, typename Fn>
// NOLINTNEXTLINE(readability-identifier-naming): mirrors the std:: spelling on purpose.
void for_each(ExecutionPolicy ep, Container&& container, Fn&& fn)
{
    // Note: no ranges overload takes an execution policy, so the range is expanded here. It is
    // expanded only once — forwarding twice would read `container` after moving from it.
    auto&& range = std::forward<Container>(container);
    std::for_each(ep, begin(range), end(range), std::forward<Fn>(fn));
}

template <typename Container, typename T>
auto count(Container&& container, T&& value)
{
    return std::count(begin(std::forward<Container>(container)),
                      end(std::forward<Container>(container)),
                      std::forward<T>(value));
}

} // namespace crispy
