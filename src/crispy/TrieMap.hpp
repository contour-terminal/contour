// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <array>
#include <cassert>
#include <cstdint>
#include <memory>
#include <optional>
#include <variant>

namespace crispy
{

// clang-format off
struct no_match {};
struct partial_match {};
template <typename T> struct exact_match { T const& value; };
template <typename T> using trie_match = std::variant<exact_match<T>, partial_match, no_match>;
// clang-format on

namespace detail
{
    template <typename Value>
    struct trie_node
    {
        std::array<std::unique_ptr<trie_node<Value>>, 256> children;
        std::optional<Value> value;
    };
} // namespace detail

/// General purpose Trie map data structure,
///
/// While this is a general purpose Trie data structure,
/// I only implemented as much as was needed to fit the purpose.
template <typename Key, typename Value>
class trie_map
{
  public:
    void insert(Key const& key, Value value);
    void clear();

    [[nodiscard]] size_t size() const noexcept { return _size; }

    [[nodiscard]] trie_match<Value> search(Key const& key, bool allowWhildcardDot = false) const noexcept;
    [[nodiscard]] bool contains(Key const& key) const noexcept;

  private:
    detail::trie_node<Value> _root;
    size_t _size = 0;
};

template <typename Key, typename Value>
void trie_map<Key, Value>::clear()
{
    for (std::unique_ptr<detail::trie_node<Value>>& childNode: _root.children)
        childNode.reset();
    _size = 0;
}

template <typename Key, typename Value>
void trie_map<Key, Value>::insert(Key const& key, Value value)
{
    assert(!key.empty());

    detail::trie_node<Value>* currentNode = &_root;
    for (auto const element: key)
    {
        auto const childIndex = static_cast<uint8_t>(element);
        if (!currentNode->children[childIndex])
            currentNode->children[childIndex] = std::make_unique<detail::trie_node<Value>>();
        currentNode = currentNode->children[childIndex].get();
    }

    assert(!currentNode->value.has_value());

    if (!currentNode->value.has_value())
        ++_size;

    currentNode->value = std::move(value);
}

template <typename Key, typename Value>
trie_match<Value> trie_map<Key, Value>::search(Key const& key, bool allowWhildcardDot) const noexcept
{
    detail::trie_node<Value> const* currentNode = &_root;
    for (auto const element: key)
    {
        auto const childIndex = static_cast<uint8_t>(element);
        if (currentNode->children[childIndex])
            currentNode = currentNode->children[childIndex].get();
        else if (allowWhildcardDot && currentNode->children[static_cast<uint8_t>('.')])
            currentNode = currentNode->children[static_cast<uint8_t>('.')].get();
        else
            return no_match {};
    }

    if (currentNode->value.has_value())
        return exact_match<Value> { currentNode->value.value() };

    return partial_match {};
}

template <typename Key, typename Value>
bool trie_map<Key, Value>::contains(Key const& key) const noexcept
{
    return std::holds_alternative<exact_match<Value>>(search(key));
}

} // namespace crispy
