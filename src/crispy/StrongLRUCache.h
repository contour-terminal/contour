/**
 * This file is part of the "contour" project.
 *   Copyright (c) 2020 Christian Parpart <christian@parpart.family>
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

#include <crispy/StrongHash.h>
#include <crispy/StrongLRUHashtable.h>
#include <crispy/assert.h>

#include <optional>
#include <ostream>
#include <stdexcept>
#include <vector>

#if defined(__x86_64__)
    #include <immintrin.h>
#elif defined(__aarch64__)
    #include <crispy/sse2neon.h>
#endif

#define DEBUG_STRONG_LRU_CACHE 1

#if defined(NDEBUG) && defined(DEBUG_STRONG_LRU_CACHE)
    #undef DEBUG_STRONG_LRU_CACHE
#endif

namespace crispy
{

// {{{ details
namespace detail
{
    template <typename Key, typename Value>
    struct LRUCacheEntry
    {
        Key key {};
        Value value {};
    };
} // namespace detail
// }}}

// LRU cache implementation with the goal to minimize runtime allocations
// and maximize speed.
//
// NOTE!
//     Cache locality could be further improved by having one single
//     memory region instead of two.
//     Or even overloading operator new of StrongLRUCache
//     and put the dynamic data at the end of the primary data.
template <typename Key, typename Value, typename Hasher = StrongHasher<Key>>
class StrongLRUCache
{
  public:
    StrongLRUCache(StrongHashtableSize hashCount, LRUCapacity entryCount, std::string name = "");
    StrongLRUCache(StrongLRUCache&&) noexcept = default;
    StrongLRUCache(StrongLRUCache const&) noexcept = delete;
    StrongLRUCache& operator=(StrongLRUCache&&) noexcept = default;
    StrongLRUCache& operator=(StrongLRUCache const&) noexcept = delete;
    ~StrongLRUCache();

    /// Returns the actual number of entries currently hold in this cache.
    [[nodiscard]] size_t size() const noexcept;

    /// Returns the maximum number of entries that can be stored in this cache.
    [[nodiscard]] size_t capacity() const noexcept;

    /// Returns gathered stats and clears the local stats state to start
    /// counting from zero again.
    LRUHashtableStats fetchAndClearStats() noexcept;

    /// Clears all entries from the cache.
    void clear();

    // Delets the key and its associated value from the LRU cache
    void remove(Key key);

    /// Touches a given key, putting it to the front of the LRU chain.
    /// Nothing is done if the key was not found.
    void touch(Key key) noexcept;

    /// Returns an ordered list of keys in this hash.
    /// Ordering is from most recent to least recent access.
    [[nodiscard]] std::vector<Key> keys() const;

    /// Tests for the exitence of the given key in this cache.
    [[nodiscard]] bool contains(Key key) const noexcept;

    /// Returns the value for the given key if found, nullptr otherwise.
    [[nodiscard]] Value* try_get(Key key);
    [[nodiscard]] Value const* try_get(Key key) const;

    /// Returns the value for the given key,
    /// throwing std::out_of_range if key was not found.
    [[nodiscard]] Value& at(Key key);

    /// Returns the value for the given key, default-constructing it in case
    /// if it wasn't in the cache just yet.
    [[nodiscard]] Value& operator[](Key key) noexcept;

    /// Assignes the given value to the given key.
    /// If the key was not found, it is being created, otherwise the value will
    /// be re-assigned with the new value.
    Value& emplace(Key key, Value value) noexcept;

    /// Conditionally creates a new item to the LRU-Cache iff its key was not present yet.
    ///
    /// @retval true the key did not exist in cache yet, a new value was constructed.
    /// @retval false The key is already in the cache, no entry was constructed.
    template <typename ValueConstructFn>
    [[nodiscard]] bool try_emplace(Key key, ValueConstructFn constructValue);

    /// Always returns either the existing item by the given key, if found,
    /// or a newly created one by invoking constructValue().
    template <typename ValueConstructFn>
    [[nodiscard]] Value& get_or_emplace(Key key, ValueConstructFn constructValue);

    void inspect(std::ostream& output) const;

  private:
    using Entry = detail::LRUCacheEntry<Key, Value>;
    using Hashtable = StrongLRUHashtable<Entry>;
    using HashtablePtr = typename Hashtable::Ptr;

    HashtablePtr _hashtable;
};

// {{{ implementation

template <typename Key, typename Value, typename Hasher>
StrongLRUCache<Key, Value, Hasher>::StrongLRUCache(StrongHashtableSize hashCount,
                                                   LRUCapacity entryCount,
                                                   std::string name):
    _hashtable { Hashtable::create(hashCount, entryCount, std::move(name)) }
{
}

template <typename Key, typename Value, typename Hasher>
StrongLRUCache<Key, Value, Hasher>::~StrongLRUCache()
{
}

template <typename Key, typename Value, typename Hasher>
inline size_t StrongLRUCache<Key, Value, Hasher>::size() const noexcept
{
    return _hashtable->size();
}

template <typename Key, typename Value, typename Hasher>
inline size_t StrongLRUCache<Key, Value, Hasher>::capacity() const noexcept
{
    return _hashtable->capacity();
}

template <typename Key, typename Value, typename Hasher>
LRUHashtableStats StrongLRUCache<Key, Value, Hasher>::fetchAndClearStats() noexcept
{
    return _hashtable->fetchAndClearStats();
}

template <typename Key, typename Value, typename Hasher>
void StrongLRUCache<Key, Value, Hasher>::clear()
{
    _hashtable->clear();
}

template <typename Key, typename Value, typename Hasher>
void StrongLRUCache<Key, Value, Hasher>::remove(Key key)
{
    _hashtable->remove(Hasher {}(key));
}

template <typename Key, typename Value, typename Hasher>
inline void StrongLRUCache<Key, Value, Hasher>::touch(Key key) noexcept
{
    _hashtable->touch(Hasher {}(key));
}

template <typename Key, typename Value, typename Hasher>
inline bool StrongLRUCache<Key, Value, Hasher>::contains(Key key) const noexcept
{
    return _hashtable->contains(Hasher {}(key));
}

template <typename Key, typename Value, typename Hasher>
inline Value* StrongLRUCache<Key, Value, Hasher>::try_get(Key key)
{
    if (Entry* e = _hashtable->try_get(Hasher {}(key)))
        return &e->value;
    return nullptr;
}

template <typename Key, typename Value, typename Hasher>
inline Value const* StrongLRUCache<Key, Value, Hasher>::try_get(Key key) const
{
    if (Entry const* e = _hashtable->try_get(Hasher {}(key)))
        return &e->value;
    return nullptr;
}

template <typename Key, typename Value, typename Hasher>
inline Value& StrongLRUCache<Key, Value, Hasher>::at(Key key)
{
    return _hashtable->at(Hasher {}(key)).value;
}

template <typename Key, typename Value, typename Hasher>
inline Value& StrongLRUCache<Key, Value, Hasher>::operator[](Key key) noexcept
{
    return _hashtable->get_or_emplace(Hasher {}(key), [&](auto) { return Entry { key, Value {} }; }).value;
}

template <typename Key, typename Value, typename Hasher>
template <typename ValueConstructFn>
inline bool StrongLRUCache<Key, Value, Hasher>::try_emplace(Key key, ValueConstructFn constructValue)
{
    return _hashtable->try_emplace(Hasher {}(key), [&](auto v) {
        return Entry { key, constructValue(std::move(v)) };
    });
}

template <typename Key, typename Value, typename Hasher>
template <typename ValueConstructFn>
inline Value& StrongLRUCache<Key, Value, Hasher>::get_or_emplace(Key key, ValueConstructFn constructValue)
{
    return _hashtable->get_or_emplace(Hasher {}(key),
                                      [&](auto v) {
                                          return Entry { key, constructValue(v) };
                                      })
        .value;
}

template <typename Key, typename Value, typename Hasher>
inline Value& StrongLRUCache<Key, Value, Hasher>::emplace(Key key, Value value) noexcept
{
    return _hashtable->emplace(Hasher {}(key), Entry { key, std::move(value) }).value;
}

template <typename Key, typename Value, typename Hasher>
std::vector<Key> StrongLRUCache<Key, Value, Hasher>::keys() const
{
    auto result = std::vector<Key> {};
    for (StrongHash const& hash: _hashtable->hashes())
        result.emplace_back(_hashtable->peek(hash).key);
    return result;
}

template <typename Key, typename Value, typename Hasher>
void StrongLRUCache<Key, Value, Hasher>::inspect(std::ostream& output) const
{
    _hashtable->inspect(output);
}

// }}}

} // namespace crispy

// {{{ fmt
namespace fmt
{
template <typename K, typename V>
struct formatter<crispy::detail::LRUCacheEntry<K, V>>
{
    template <typename ParseContext>
    constexpr auto parse(ParseContext& ctx)
    {
        return ctx.begin();
    }
    template <typename FormatContext>
    auto format(crispy::detail::LRUCacheEntry<K, V> const& entry, FormatContext& ctx)
    {
        return format_to(ctx.out(), "{}: {}", entry.key, entry.value);
    }
};
} // namespace fmt
// }}}
