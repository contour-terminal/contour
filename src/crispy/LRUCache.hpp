// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <core/Assert.hpp>

#include <cassert>
#include <list>
#include <stdexcept>
#include <unordered_map>
#include <utility>
#include <vector>

namespace crispy
{

/// Implements LRU (Least recently used) cache.
template <typename Key, typename Value>
class LRUCache
{
  public:
    struct Item
    {
        Key key;
        Value value;
    };
    using ItemList = std::list<Item>;

    // NOLINTNEXTLINE(readability-identifier-naming): standard iterator/container trait spelling.
    using iterator = ItemList::iterator;
    // NOLINTNEXTLINE(readability-identifier-naming): standard iterator/container trait spelling.
    using const_iterator = ItemList::const_iterator;

    explicit LRUCache(std::size_t capacity): _capacity { capacity } {}

    [[nodiscard]] std::size_t size() const noexcept { return _items.size(); }
    [[nodiscard]] std::size_t capacity() const noexcept { return _capacity; }

    void clear()
    {
        _itemByKeyMapping.clear();
        _items.clear();
    }

    void touch(Key const& key) noexcept { (void) tryGet(key); }

    [[nodiscard]] bool contains(Key const& key) const noexcept { return _itemByKeyMapping.count(key) != 0; }

    [[nodiscard]] Value* tryGet(Key const& key) const { return const_cast<LRUCache*>(this)->tryGet(key); }

    [[nodiscard]] Value* tryGet(Key const& key)
    {
        if (auto i = _itemByKeyMapping.find(key); i != _itemByKeyMapping.end())
        {
            // Move it to the front, and return it. The mapping is updated through its own iterator, not
            // by @p key: a caller may pass the key the item itself holds, which the move leaves empty.
            i->second = moveItemToFront(i->second);
            return &i->second->value;
        }

        return nullptr;
    }

    [[nodiscard]] Value& at(Key const& key)
    {
        if (Value* p = tryGet(key))
            return *p;

        throw std::out_of_range("key");
    }

    [[nodiscard]] Value const& at(Key const& key) const
    {
        if (Value const* p = tryGet(key))
            return *p;

        throw std::out_of_range("key");
    }

    /// Returns the value for the given key, default-constructing it in case
    /// if it wasn't in the cache just yet.
    [[nodiscard]] Value& operator[](Key key)
    {
        if (Value* p = tryGet(key))
            return *p;

        if (_items.size() == _capacity)
            return evictOneAndPushFront(std::move(key), Value {})->value;

        return emplaceItemToFront(std::move(key), Value {})->value;
    }

    /// Conditionally creates a new item to the LRU-Cache iff its key was not present yet.
    ///
    /// @retval true the key did not exist in cache yet, a new value was constructed.
    /// @retval false The key is already in the cache, no entry was constructed.
    template <typename ValueConstructFn>
    [[nodiscard]] bool tryEmplace(Key key, ValueConstructFn constructValue)
    {
        if (Value const* p = tryGet(key))
            return false;

        if (_items.size() == _capacity)
            evictOneAndPushFront(std::move(key), constructValue());
        else
            emplaceItemToFront(std::move(key), constructValue());
        return true;
    }

    template <typename ValueConstructFn>
    [[nodiscard]] Value& getOrEmplace(Key key, ValueConstructFn constructValue)
    {
        if (Value* p = tryGet(key))
            return *p;
        return emplace(std::move(key), constructValue());
    }

    Value& emplace(Key key, Value&& value)
    {
        Require(!contains(key));

        if (_items.size() == _capacity)
            return evictOneAndPushFront(std::move(key), std::move(value))->value;

        return emplaceItemToFront(std::move(key), std::move(value))->value;
    }

    [[nodiscard]] iterator begin() { return _items.begin(); }
    [[nodiscard]] iterator end() { return _items.end(); }

    [[nodiscard]] const_iterator begin() const { return _items.cbegin(); }
    [[nodiscard]] const_iterator end() const { return _items.cend(); }

    [[nodiscard]] const_iterator cbegin() const { return _items.cbegin(); }
    [[nodiscard]] const_iterator cend() const { return _items.cend(); }

    [[nodiscard]] std::vector<Key> keys() const
    {
        std::vector<Key> result;
        result.resize(_items.size());
        size_t i = 0;
        for (Item const& item: _items)
            result[i++] = item.key;
        return result;
    }

    void erase(iterator const& iter)
    {
        // The key is copied out first: it lives in the node that erasing the item frees, and @p iter may be
        // the very iterator the mapping holds, which erasing the mapping destroys.
        auto const key = iter->key;
        _items.erase(iter);
        _itemByKeyMapping.erase(key);
    }

    void erase(Key const& key)
    {
        if (auto keyMappingIterator = _itemByKeyMapping.find(key);
            keyMappingIterator != _itemByKeyMapping.end())
            erase(keyMappingIterator->second);
    }

  private:
    /// Evicts least recently used item and prepares (/reuses) its storage for a new item.
    iterator evictOneAndPushFront(Key newKey, Value&& newValue)
    {
        auto const oldKey = _items.back().key;
        auto keyMappingIterator = _itemByKeyMapping.find(oldKey);
        Require(keyMappingIterator != _itemByKeyMapping.end());

        auto newItemIterator = moveItemToFront(keyMappingIterator->second);
        newItemIterator->key = std::move(newKey);
        newItemIterator->value = std::move(newValue);
        _itemByKeyMapping.erase(keyMappingIterator);
        _itemByKeyMapping.emplace(newItemIterator->key, newItemIterator);

        return newItemIterator;
    }

    [[nodiscard]] iterator moveItemToFront(iterator const& i)
    {
        _items.emplace_front(std::move(*i));
        _items.erase(i);
        return _items.begin();
    }

    iterator emplaceItemToFront(Key key, Value&& value)
    {
        _items.emplace_front(Item { std::move(key), std::move(value) });
        _itemByKeyMapping.emplace(_items.front().key, _items.begin());
        return _items.begin();
    }

    // private data
    //
    std::list<Item> _items;
    std::unordered_map<Key, iterator> _itemByKeyMapping;
    std::size_t _capacity;
};

} // namespace crispy
