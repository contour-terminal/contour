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
    return range(container.rbegin(), container.rend());
}

} // namespace crispy
