// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <crispy/assert.h>

#include <cassert>
#include <list>
#include <stdexcept>
#include <unordered_map>
#include <vector>

namespace crispy
{

/// Implements LRU (Least recently used) cache.
template <typename Key, typename Value>
class lru_cache
{
  public:
    struct item
    {
        Key key;
        Value value;
    };
    using item_list = std::list<item>;

    using iterator = typename item_list::iterator;
    using const_iterator = typename item_list::const_iterator;

    explicit lru_cache(std::size_t capacity): _capacity { capacity } {}

    [[nodiscard]] std::size_t size() const noexcept { return _items.size(); }
    [[nodiscard]] std::size_t capacity() const noexcept { return _capacity; }

    void clear()
    {
        _itemByKeyMapping.clear();
        _items.clear();
    }

    void touch(Key key) noexcept { (void) try_get(key); }

    [[nodiscard]] bool contains(Key key) const noexcept { return _itemByKeyMapping.count(key) != 0; }

    [[nodiscard]] Value* try_get(Key key) const { return const_cast<lru_cache*>(this)->try_get(key); }

    [[nodiscard]] Value* try_get(Key key)
    {
        if (auto i = _itemByKeyMapping.find(key); i != _itemByKeyMapping.end())
        {
            // move it to the front, and return it
            auto i2 = moveItemToFront(i->second);
            _itemByKeyMapping.at(key) = i2;
            return &i2->value;
        }

        return nullptr;
    }

    [[nodiscard]] Value& at(Key key)
    {
        if (Value* p = try_get(key))
            return *p;

        throw std::out_of_range("key");
    }

    [[nodiscard]] Value const& at(Key key) const
    {
        if (Value const* p = try_get(key))
            return *p;

        throw std::out_of_range("key");
    }

    /// Returns the value for the given key, default-constructing it in case
    /// if it wasn't in the cache just yet.
    [[nodiscard]] Value& operator[](Key key)
    {
        if (Value* p = try_get(key))
            return *p;

        if (_items.size() == _capacity)
            return evict_one_and_push_front(key, Value {})->value;

        return emplaceItemToFront(key, Value {})->value;
    }

    /// Conditionally creates a new item to the LRU-Cache iff its key was not present yet.
    ///
    /// @retval true the key did not exist in cache yet, a new value was constructed.
    /// @retval false The key is already in the cache, no entry was constructed.
    template <typename ValueConstructFn>
    [[nodiscard]] bool try_emplace(Key key, ValueConstructFn constructValue)
    {
        if (Value* p = try_get(key))
            return false;

        if (_items.size() == _capacity)
            evict_one_and_push_front(key, constructValue());
        else
            emplaceItemToFront(key, constructValue());
        return true;
    }

    template <typename ValueConstructFn>
    [[nodiscard]] Value& get_or_emplace(Key key, ValueConstructFn constructValue)
    {
        if (Value* p = try_get(key))
            return *p;
        return emplace(key, constructValue());
    }

    Value& emplace(Key key, Value&& value)
    {
        Require(!contains(key));

        if (_items.size() == _capacity)
            return evict_one_and_push_front(key, std::move(value))->value;

        return emplaceItemToFront(key, std::move(value))->value;
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
        for (item const& item: _items)
            result[i++] = item.key;
        return result;
    }

    void erase(iterator iter)
    {
        _items.erase(iter);
        _itemByKeyMapping.erase(iter->key);
    }

    void erase(Key const& key)
    {
        if (auto keyMappingIterator = _itemByKeyMapping.find(key);
            keyMappingIterator != _itemByKeyMapping.end())
            erase(keyMappingIterator->second);
    }

  private:
    /// Evicts least recently used item and prepares (/reuses) its storage for a new item.
    iterator evict_one_and_push_front(Key newKey, Value&& newValue)
    {
        auto const oldKey = _items.back().key;
        auto keyMappingIterator = _itemByKeyMapping.find(oldKey);
        Require(keyMappingIterator != _itemByKeyMapping.end());

        auto newItemIterator = moveItemToFront(keyMappingIterator->second);
        newItemIterator->key = newKey;
        newItemIterator->value = std::move(newValue);
        _itemByKeyMapping.erase(keyMappingIterator);
        _itemByKeyMapping.emplace(newKey, newItemIterator);

        return newItemIterator;
    }

    [[nodiscard]] iterator moveItemToFront(iterator i)
    {
        _items.emplace_front(std::move(*i));
        _items.erase(i);
        return _items.begin();
    }

    iterator emplaceItemToFront(Key key, Value&& value)
    {
        _items.emplace_front(item { key, std::move(value) });
        _itemByKeyMapping.emplace(key, _items.begin());
        return _items.begin();
    }

    // private data
    //
    std::list<item> _items;
    std::unordered_map<Key, iterator> _itemByKeyMapping;
    std::size_t _capacity;
};

} // namespace crispy
