// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <iterator>
#include <utility>

namespace crispy
{

template <typename Iter>
// NOLINTNEXTLINE(readability-identifier-naming)
class range
{
  public:
    using iterator = Iter;
    using const_iterator = Iter; // std::add_const_t<Iter>;

    range(Iter begin, Iter end): _begin { begin }, _end { end } {}

    [[nodiscard]] constexpr iterator begin() const { return _begin; }
    [[nodiscard]] constexpr iterator end() const { return _end; }
    [[nodiscard]] constexpr const_iterator cbegin() const { return _begin; }
    [[nodiscard]] constexpr const_iterator cend() const { return _end; }

    [[nodiscard]] constexpr size_t size() const noexcept
    {
        return static_cast<size_t>(std::distance(_begin, _end));
    }

  private:
    Iter _begin;
    Iter _end;
};

template <typename Iter>
range(Iter, Iter) -> range<Iter>;

template <typename Container>
auto reversed(Container&& container)
{
    return range(std::forward<Container>(container).rbegin(), std::forward<Container>(container).rend());
}

} // namespace crispy
