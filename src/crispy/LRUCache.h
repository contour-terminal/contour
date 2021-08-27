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

#include <cassert>
#include <list>
#include <stdexcept>
#include <unordered_map>
#include <vector>

namespace crispy
{

template <typename Key, typename Value>
class LRUCache
{
public:
    using Item = std::pair<Key, Value>;
    using ItemList = std::list<Item>;

    using iterator = typename ItemList::iterator;
    using const_iterator = typename ItemList::const_iterator;

    explicit LRUCache(std::size_t _capacity): capacity_{_capacity} {}

    std::size_t size() const noexcept { return items_.size(); }
    std::size_t capacity() const noexcept { return capacity_; }

    void clear()
    {
        lookupMap_.clear();
        items_.clear();
    }

    void touch(Key _key) noexcept
    {
        (void) try_get(_key);
    }

    [[nodiscard]] bool contains(Key _key) const noexcept
    {
        return try_get(_key) != nullptr;
    }

    [[nodiscard]] Value* try_get(Key _key) const
    {
        return const_cast<LRUCache*>(this)->try_get(_key);
    }

    [[nodiscard]] Value* try_get(Key _key)
    {
        if (auto i = lookupMap_.find(_key); i != lookupMap_.end())
        {
            // value exists already, move it to the front, and return it
            Item item = std::move(*i->second);
            items_.emplace_front(std::move(item));
            items_.erase(i->second);
            lookupMap_.insert_or_assign(_key, items_.begin());
            return &items_.front().second;
        }

        return nullptr;
    }

    [[nodiscard]] Value& at(Key _key)
    {
        if (Value* p = try_get(_key))
            return *p;

        throw std::out_of_range("_key");
    }

    [[nodiscard]] Value const& at(Key _key) const
    {
        if (Value const* p = try_get(_key))
            return *p;

        throw std::out_of_range("_key");
    }

    /// Returns the value for the given key, default-constructing it in case
    /// if it wasn't in the cache just yet.
    [[nodiscard]] Value& operator[](Key _key)
    {
        if (Value* p = try_get(_key))
            return *p;

        if (items_.size() == capacity_)
        {
            auto i = lookupMap_.find(items_.back().first);
            lookupMap_.erase(i);
            items_.pop_back();
        }

        items_.emplace_front(Item{_key, Value{}});
        lookupMap_.emplace(_key, items_.begin());
        return items_.front().second;
    }

    /// Conditionally creates a new item to the LRU-Cache iff its key was not present yet.
    ///
    /// @retval true the key did not exist in cache yet, a new value was constructed.
    /// @retval false The key is already in the cache, no entry was constructed.
    template <typename ValueConstructFn>
    [[nodiscard]] bool try_emplace(Key _key, ValueConstructFn _constructValue)
    {
        if (Value* p = try_get(_key))
            return false;

        if (items_.size() == capacity_)
        {
            auto i = lookupMap_.find(items_.back().first);
            lookupMap_.erase(i);
            items_.pop_back();
        }

        items_.emplace_front(Item{_key, _constructValue()});
        lookupMap_.emplace(_key, items_.begin());
        return true;
    }

    template <typename ValueConstructFn>
    [[nodiscard]] Value& get_or_emplace(Key _key, ValueConstructFn _constructValue)
    {
        if (Value* p = try_get(_key))
            return *p;
        return emplace(_key, _constructValue());
    }

    [[nodiscard]] Value& emplace(Key _key, Value _value)
    {
        assert(!try_get(_key));

        if (items_.size() == capacity_)
        {
            auto i = lookupMap_.find(items_.back().first);
            lookupMap_.erase(i);
            items_.pop_back();
        }

        items_.emplace_front(Item{_key, std::move(_value)});
        lookupMap_.emplace(_key, items_.begin());
        return items_.front().second;
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
            result[i++] = item.first;
        return result;
    }

    void erase(iterator _iter)
    {
        items_.erase(_iter);
        itemByKeyMapping_.erase(_iter->first);
    }

    void erase(Key const& _key)
    {
        if (auto i = itemByKeyMapping_.find(_key); i != itemByKeyMapping_.end())
            erase(i->second);
    }

private:
    std::list<Item> items_;
    std::unordered_map<Key, typename std::list<Item>::iterator> lookupMap_;
    std::size_t capacity_;
};

}
