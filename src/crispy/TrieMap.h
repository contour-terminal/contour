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

#include <array>
#include <cassert>
#include <memory>
#include <optional>
#include <variant>

namespace crispy
{

// clang-format off
struct NoMatch {};
struct PartialMatch {};
template <typename T> struct ExactMatch { T const& value; };
template <typename T> using TrieMatch = std::variant<ExactMatch<T>, PartialMatch, NoMatch>;
// clang-format on

namespace detail
{
    template <typename Value>
    struct TrieNode
    {
        std::array<std::unique_ptr<TrieNode<Value>>, 256> children;
        std::optional<Value> value;
    };
} // namespace detail

/// General purpose Trie map data structure,
///
/// While this is a general purpose Trie data structure,
/// I only implemented as much as was needed to fit the purpose.
template <typename Key, typename Value>
class TrieMap
{
  public:
    void insert(Key const& key, Value value);
    void clear();

    [[nodiscard]] size_t size() const noexcept { return _size; }

    [[nodiscard]] TrieMatch<Value> search(Key const& key) const noexcept;
    [[nodiscard]] bool contains(Key const& key) const noexcept;

  private:
    detail::TrieNode<Value> _root;
    size_t _size = 0;
};

template <typename Key, typename Value>
void TrieMap<Key, Value>::clear()
{
    for (std::unique_ptr<detail::TrieNode<Value>>& childNode: _root.children)
        childNode.reset();
    _size = 0;
}

template <typename Key, typename Value>
void TrieMap<Key, Value>::insert(Key const& key, Value value)
{
    detail::TrieNode<Value>* currentNode = &_root;
    for (auto const element: key)
    {
        auto const childIndex = static_cast<uint8_t>(element);
        if (!currentNode->children[childIndex])
            currentNode->children[childIndex] = std::make_unique<detail::TrieNode<Value>>();
        currentNode = currentNode->children[childIndex].get();
    }

    assert(!currentNode->value.has_value());

    if (!currentNode->value.has_value())
        ++_size;

    currentNode->value = std::move(value);
}

template <typename Key, typename Value>
TrieMatch<Value> TrieMap<Key, Value>::search(Key const& key) const noexcept
{
    detail::TrieNode<Value> const* currentNode = &_root;
    for (auto const element: key)
    {
        auto const childIndex = static_cast<uint8_t>(element);
        if (!currentNode->children[childIndex])
            return NoMatch {};
        currentNode = currentNode->children[childIndex].get();
    }

    if (currentNode->value.has_value())
        return ExactMatch<Value> { currentNode->value.value() };

    return PartialMatch {};
}

template <typename Key, typename Value>
bool TrieMap<Key, Value>::contains(Key const& key) const noexcept
{
    return std::holds_alternative<ExactMatch<Value>>(search(key));
}

} // namespace crispy
