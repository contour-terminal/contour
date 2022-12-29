/**
 * This file is part of the "libterminal" project
 *   Copyright (c) 2021 Christian Parpart <christian@parpart.family>
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
class LRUCache
{
  public:
    struct Item
    {
        Key key;
        Value value;
    };
    using ItemList = std::list<Item>;

    using iterator = typename ItemList::iterator;
    using const_iterator = typename ItemList::const_iterator;

    explicit LRUCache(std::size_t capacity): capacity_ { capacity } {}

    [[nodiscard]] std::size_t size() const noexcept { return items_.size(); }
    [[nodiscard]] std::size_t capacity() const noexcept { return capacity_; }

    void clear()
    {
        itemByKeyMapping_.clear();
        items_.clear();
    }

    void touch(Key key) noexcept { (void) try_get(key); }

    [[nodiscard]] bool contains(Key key) const noexcept { return itemByKeyMapping_.count(key) != 0; }

    [[nodiscard]] Value* try_get(Key key) const { return const_cast<LRUCache*>(this)->try_get(key); }

    [[nodiscard]] Value* try_get(Key key)
    {
        if (auto i = itemByKeyMapping_.find(key); i != itemByKeyMapping_.end())
        {
            // move it to the front, and return it
            auto i2 = moveItemToFront(i->second);
            itemByKeyMapping_.at(key) = i2;
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

        if (items_.size() == capacity_)
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

        if (items_.size() == capacity_)
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

        if (items_.size() == capacity_)
            return evict_one_and_push_front(key, std::move(value))->value;

        return emplaceItemToFront(key, std::move(value))->value;
    }

    [[nodiscard]] iterator begin() { return items_.begin(); }
    [[nodiscard]] iterator end() { return items_.end(); }

    [[nodiscard]] const_iterator begin() const { return items_.cbegin(); }
    [[nodiscard]] const_iterator end() const { return items_.cend(); }

    [[nodiscard]] const_iterator cbegin() const { return items_.cbegin(); }
    [[nodiscard]] const_iterator cend() const { return items_.cend(); }

    [[nodiscard]] std::vector<Key> keys() const
    {
        std::vector<Key> result;
        result.resize(items_.size());
        size_t i = 0;
        for (Item const& item: items_)
            result[i++] = item.key;
        return result;
    }

    void erase(iterator iter)
    {
        items_.erase(iter);
        itemByKeyMapping_.erase(iter->key);
    }

    void erase(Key const& key)
    {
        if (auto keyMappingIterator = itemByKeyMapping_.find(key);
            keyMappingIterator != itemByKeyMapping_.end())
            erase(keyMappingIterator->second);
    }

  private:
    /// Evicts least recently used item and prepares (/reuses) its storage for a new item.
    iterator evict_one_and_push_front(Key newKey, Value&& newValue)
    {
        auto const oldKey = items_.back().key;
        auto keyMappingIterator = itemByKeyMapping_.find(oldKey);
        Require(keyMappingIterator != itemByKeyMapping_.end());

        auto newItemIterator = moveItemToFront(keyMappingIterator->second);
        newItemIterator->key = newKey;
        newItemIterator->value = std::move(newValue);
        itemByKeyMapping_.erase(keyMappingIterator);
        itemByKeyMapping_.emplace(newKey, newItemIterator);

        return newItemIterator;
    }

    [[nodiscard]] iterator moveItemToFront(iterator i)
    {
        items_.emplace_front(std::move(*i));
        items_.erase(i);
        return items_.begin();
    }

    iterator emplaceItemToFront(Key key, Value&& value)
    {
        items_.emplace_front(Item { key, std::move(value) });
        itemByKeyMapping_.emplace(key, items_.begin());
        return items_.begin();
    }

    // private data
    //
    std::list<Item> items_;
    std::unordered_map<Key, iterator> itemByKeyMapping_;
    std::size_t capacity_;
};

} // namespace crispy
