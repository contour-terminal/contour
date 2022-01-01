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

#include <crispy/assert.h>

#include <immintrin.h>
#include <optional>
#include <stdexcept>
#include <vector>

// #define DEBUG_STRONG_LRU_CACHE 1

namespace crispy
{

struct StrongHash
{
    __m128i value;
};

inline bool operator==(StrongHash a, StrongHash b) noexcept
{
    return _mm_movemask_epi8(_mm_cmpeq_epi32(a.value, b.value)) == 0xFFFF;
}

inline bool operator!=(StrongHash a, StrongHash b) noexcept
{
    return !(a == b);
}

// {{{ details
namespace detail
{
    static constexpr bool isPowerOfTwo(uint32_t value) noexcept
    {
        //.
        return (value & (value - 1)) == 0;
    }
} // namespace detail
// }}}

template <typename T>
struct StrongHasher
{
    StrongHash operator()(T const&) noexcept; // Specialize and implement me.
};

// {{{ some standard hash implementations
namespace detail
{
    template <typename T>
    struct StdHash32
    {
        inline StrongHash operator()(T v) noexcept { return StrongHash { _mm_set_epi32(0, 0, 0, v) }; }
    };
} // namespace detail

// clang-format off
template <> struct StrongHasher<char>: public detail::StdHash32<char> { };
template <> struct StrongHasher<unsigned char>: public detail::StdHash32<char> { };
template <> struct StrongHasher<short>: public detail::StdHash32<short> { };
template <> struct StrongHasher<unsigned short>: public detail::StdHash32<unsigned short> { };
template <> struct StrongHasher<int>: public detail::StdHash32<int> { };
template <> struct StrongHasher<unsigned int>: public detail::StdHash32<int> { };
// clang-format on
// }}}

struct LRUCacheStats
{
    uint32_t hits;
    uint32_t misses;
    uint32_t recycles;
};

struct StrongHashCapacity
{
    uint32_t value;
};

struct StrongCacheCapacity
{
    uint32_t value;
};

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
  private:
    StrongLRUCache(StrongHashCapacity hashCount, StrongCacheCapacity entryCount);

  public:
    ~StrongLRUCache();

    using CachePtr = std::unique_ptr<StrongLRUCache, std::function<void(StrongLRUCache*)>>;

    static constexpr inline size_t requiredMemorySize(StrongHashCapacity hashCount,
                                                      StrongCacheCapacity entryCount);

    template <typename Allocator = std::allocator<unsigned char>>
    static CachePtr create(StrongHashCapacity hashCount, StrongCacheCapacity entryCount);

    /// Returns the actual number of entries currently hold in this cache.
    [[nodiscard]] size_t size() const noexcept;

    /// Returns the maximum number of entries that can be stored in this cache.
    [[nodiscard]] size_t capacity() const noexcept;

    /// Returns the total storage sized used by this object.
    [[nodiscard]] size_t storageSize() const noexcept;

    /// Returns gathered stats and clears the local stats state to start
    /// counting from zero again.
    LRUCacheStats fetchAndClearStats() noexcept;

    /// Clears all entries from the cache.
    void clear();

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

  private:
    // {{{ details
    struct NextWithSameHash
    {
        explicit NextWithSameHash(uint32_t v): value { v } {}
        uint32_t value;
    };

    struct Entry
    {
        Entry(Entry const&) = default;
        Entry(Entry&&) noexcept = default;
        Entry& operator=(Entry const&) = default;
        Entry& operator=(Entry&&) noexcept = default;
        Entry(NextWithSameHash initialNextWithSameHash): nextWithSameHash { initialNextWithSameHash.value } {}

        StrongHash hashValue {};

        uint32_t prevInLRU = 0;
        uint32_t nextInLRU = 0;
        uint32_t nextWithSameHash = 0;

        Key key {};
        std::optional<Value> value = std::nullopt;

#if defined(DEBUG_STRONG_LRU_CACHE)
        uint32_t ordering = 0;
#endif
    };

    // Maps the given hash key to a slot in the hash table.
    uint32_t* hashTableSlot(StrongHash hash);

    // Returns entry index to an unused entry, possibly by evicting
    // the least recently used entry if no free entries are available.
    //
    // This entry is not inserted into the LRU-chain yet.
    uint32_t allocateEntry();

    // Returns the index to the entry associated with the given key.
    // If the key was not found and force is set to true, it'll be created,
    // otherwise 0 is returned.
    uint32_t findEntry(Key key, bool force);

    // Evicts the least recently used entry in the LRU chain
    // and links it to the unused-entries chain.
    //
    // Requires the cache to be full.
    void recycle();

    int validateChange(int adj);

    Entry& sentinelEntry() noexcept { return _entries[0]; }
    Entry const& sentinelEntry() const noexcept { return _entries[0]; }
    // }}}

    LRUCacheStats _stats;
    uint32_t _hashMask;
    StrongHashCapacity _hashCount;
    uint32_t _size;
    StrongCacheCapacity _capacity;

    // The hash table maps hash codes to indices into the entry table.
    uint32_t* _hashTable;
    Entry* _entries;

#if defined(DEBUG_STRONG_LRU_CACHE)
    int _lastLRUCount = 0;
#endif
};

// {{{ implementation

template <typename Key, typename Value, typename Hasher>
StrongLRUCache<Key, Value, Hasher>::StrongLRUCache(StrongHashCapacity hashCount,
                                                   StrongCacheCapacity entryCount):
    _stats {},
    _hashMask { hashCount.value - 1 },
    _hashCount { hashCount },
    _size { 0 },
    _capacity { entryCount },
    _hashTable { (uint32_t*) (this + 1) },
    _entries { [this]() {
        constexpr uintptr_t Alignment = std::alignment_of_v<Entry>;
        static_assert(detail::isPowerOfTwo(Alignment));
        constexpr uintptr_t AlignMask = Alignment - 1;

        uint32_t* hashTableEnd = _hashTable + _hashCount.value;
        Entry* entryTable = (Entry*) (uintptr_t((char*) hashTableEnd + AlignMask) & ~AlignMask);

        return entryTable;
    }() }
{
    Require(detail::isPowerOfTwo(hashCount.value));
    Require(hashCount.value >= 1);
    Require(entryCount.value >= 2);

    memset(_hashTable, 0, hashCount.value * sizeof(uint32_t));

    for (auto entryIndex = 0; entryIndex < entryCount.value; ++entryIndex)
    {
        Entry* entry = _entries + entryIndex;
        new (entry) Entry(NextWithSameHash(entryIndex + 1));
    }
    new (_entries + entryCount.value) Entry(NextWithSameHash(0));
}

template <typename Key, typename Value, typename Hasher>
StrongLRUCache<Key, Value, Hasher>::~StrongLRUCache()
{
    std::destroy_n(_entries, 1 + _capacity.value);
}

template <typename Key, typename Value, typename Hasher>
constexpr inline size_t StrongLRUCache<Key, Value, Hasher>::requiredMemorySize(StrongHashCapacity hashCount,
                                                                               StrongCacheCapacity entryCount)
{
    Require(detail::isPowerOfTwo(hashCount.value)); // Hash capacity must be power of 2.
    Require(hashCount.value >= 1);
    Require(entryCount.value >= 2);

    auto const hashSize = hashCount.value * sizeof(uint32_t);
    auto const entrySize = (1 + entryCount.value) * sizeof(Entry) + std::alignment_of_v<Entry>;
    auto const totalSize = sizeof(StrongLRUCache) + hashSize + entrySize;
    return totalSize;
}

template <typename Key, typename Value, typename Hasher>
template <typename Allocator>
auto StrongLRUCache<Key, Value, Hasher>::create(StrongHashCapacity hashCount, StrongCacheCapacity entryCount)
    -> CachePtr
{
    // payload memory layout
    // =====================
    //
    // [object  attribs]
    // uint32_t[] hash table
    // Entry[]    entries

    Allocator allocator;
    auto const size = requiredMemorySize(hashCount, entryCount);
    StrongLRUCache* obj = (StrongLRUCache*) allocator.allocate(size);

    // clang-format off
    if (!obj)
        return CachePtr { nullptr, [](auto) {} };
    // clang-format on

    memset(obj, 0, size);
    new (obj) StrongLRUCache(hashCount, entryCount);

    auto deleter = [size, allocator = std::move(allocator)](auto p) mutable {
        std::destroy_n(p, 1);
        allocator.deallocate(reinterpret_cast<typename Allocator::pointer>(p), size);
    };

    return CachePtr(obj, std::move(deleter));
}

template <typename Key, typename Value, typename Hasher>
inline size_t StrongLRUCache<Key, Value, Hasher>::size() const noexcept
{
    return _size;
}

template <typename Key, typename Value, typename Hasher>
inline size_t StrongLRUCache<Key, Value, Hasher>::capacity() const noexcept
{
    return _capacity.value;
}

template <typename Key, typename Value, typename Hasher>
inline size_t StrongLRUCache<Key, Value, Hasher>::storageSize() const noexcept
{
    auto const hashTableSize = _hashCount.value * sizeof(uint32_t);

    // +1 for sentinel entry in the front
    // +1 for alignment
    auto const entryTableSize = (2 + _capacity.value) * sizeof(Entry);

    return sizeof(StrongLRUCache) + hashTableSize + entryTableSize;
}

template <typename Key, typename Value, typename Hasher>
inline uint32_t* StrongLRUCache<Key, Value, Hasher>::hashTableSlot(StrongHash hash)
{
    uint32_t const index = _mm_cvtsi128_si32(hash.value);
    uint32_t const slot = index & _hashMask;
    return _hashTable + slot;
}

template <typename Key, typename Value, typename Hasher>
LRUCacheStats StrongLRUCache<Key, Value, Hasher>::fetchAndClearStats() noexcept
{
    auto st = _stats;
    _stats = LRUCacheStats {};
    return st;
}

template <typename Key, typename Value, typename Hasher>
void StrongLRUCache<Key, Value, Hasher>::clear()
{
    Entry& sentinel = sentinelEntry();
    uint32_t entryIndex = sentinel.nextInLRU;
    while (entryIndex)
    {
        Entry& entry = _entries[entryIndex];
        entry.value.reset();
        entryIndex = entry.nextInLRU;
    }

    memset(_hashTable, 0, _hashCount.value * sizeof(uint32_t));

    while (entryIndex < _capacity.value)
    {
        _entries[entryIndex] = Entry(NextWithSameHash(1 + entryIndex));
        ++entryIndex;
    }
    _entries[_capacity.value] = Entry(NextWithSameHash(0));

    auto const oldSize = static_cast<int>(_size);
    _size = 0;
    validateChange(-oldSize);
}

template <typename Key, typename Value, typename Hasher>
void StrongLRUCache<Key, Value, Hasher>::touch(Key key) noexcept
{
    findEntry(key, false);
}

template <typename Key, typename Value, typename Hasher>
bool StrongLRUCache<Key, Value, Hasher>::contains(Key key) const noexcept
{
    return const_cast<StrongLRUCache*>(this)->findEntry(key, false) != 0;
}

template <typename Key, typename Value, typename Hasher>
Value* StrongLRUCache<Key, Value, Hasher>::try_get(Key key)
{
    uint32_t const entryIndex = findEntry(key, false);
    if (!entryIndex)
        return nullptr;
    return &_entries[entryIndex].value.value();
}

template <typename Key, typename Value, typename Hasher>
Value& StrongLRUCache<Key, Value, Hasher>::at(Key key)
{
    uint32_t const entryIndex = findEntry(key, false);
    if (!entryIndex)
        throw std::out_of_range("key not in cache");
    return *_entries[entryIndex].value;
}

template <typename Key, typename Value, typename Hasher>
Value& StrongLRUCache<Key, Value, Hasher>::operator[](Key key) noexcept
{
    uint32_t const entryIndex = findEntry(key, true);
    return *_entries[entryIndex].value;
}

template <typename Key, typename Value, typename Hasher>
template <typename ValueConstructFn>
bool StrongLRUCache<Key, Value, Hasher>::try_emplace(Key key, ValueConstructFn constructValue)
{
    if (contains(key))
        return false;

    emplace(key, constructValue());
    return true;
}

template <typename Key, typename Value, typename Hasher>
template <typename ValueConstructFn>
Value& StrongLRUCache<Key, Value, Hasher>::get_or_emplace(Key key, ValueConstructFn constructValue)
{
    if (Value* p = try_get(key))
        return *p;
    return emplace(key, constructValue());
}

template <typename Key, typename Value, typename Hasher>
inline Value& StrongLRUCache<Key, Value, Hasher>::emplace(Key key, Value value) noexcept
{
    return (*this)[key] = std::move(value);
}

template <typename Key, typename Value, typename Hasher>
std::vector<Key> StrongLRUCache<Key, Value, Hasher>::keys() const
{
    auto result = std::vector<Key> {};
    Entry const& sentinel = sentinelEntry();
    for (uint32_t entryIndex = sentinel.nextInLRU; entryIndex != 0;)
    {
        Entry const& entry = _entries[entryIndex];
        result.emplace_back(entry.key);
        entryIndex = entry.nextInLRU;
    }
    Guarantee(result.size() == _size);

    return result;
}

// {{{ helpers
template <typename Key, typename Value, typename Hasher>
uint32_t StrongLRUCache<Key, Value, Hasher>::findEntry(Key key, bool force)
{
    StrongHash const hash = Hasher {}(key);

    uint32_t* slot = hashTableSlot(hash);
    uint32_t entryIndex = *slot;
    Entry* result = nullptr;
    while (entryIndex)
    {
        Entry& entry = _entries[entryIndex];
        if (entry.hashValue == hash)
        {
            result = &entry;
            break;
        }
        entryIndex = entry.nextWithSameHash;
    }

    if (result)
    {
        ++_stats.hits;

        Entry& prev = _entries[result->prevInLRU];
        Entry& next = _entries[result->nextInLRU];

        prev.nextInLRU = result->nextInLRU;
        next.prevInLRU = result->prevInLRU;

        validateChange(-1);
    }
    else if (force)
    {
        ++_stats.misses;

        entryIndex = allocateEntry();
        result = &_entries[entryIndex];
        result->nextWithSameHash = *slot;
        result->hashValue = hash;
        result->key = key;
        result->value.emplace(Value {});
        *slot = entryIndex;
    }
    else
    {
        ++_stats.misses;
        return 0;
    }

    Entry& sentinel = sentinelEntry();
    Require(result != &sentinel);
    result->nextInLRU = sentinel.nextInLRU;
    result->prevInLRU = 0;

    Entry& nextEntry = _entries[sentinel.nextInLRU];
    nextEntry.prevInLRU = entryIndex;
    sentinel.nextInLRU = entryIndex;

#if defined(DEBUG_STRONG_LRU_CACHE)
    result->ordering = sentinel.ordering++;
#endif

    Require(validateChange(1) == _size);

    return entryIndex;
}

template <typename Key, typename Value, typename Hasher>
uint32_t StrongLRUCache<Key, Value, Hasher>::allocateEntry()
{
    Entry& sentinel = sentinelEntry();

    if (sentinel.nextWithSameHash == 0)
        recycle();
    else
        ++_size;

    uint32_t poppedEntryIndex = sentinel.nextWithSameHash;
    Require(poppedEntryIndex != 0);

    Entry& poppedEntry = _entries[poppedEntryIndex];
    sentinel.nextWithSameHash = poppedEntry.nextWithSameHash;
    poppedEntry.nextWithSameHash = 0;

    poppedEntry.value.reset();

    return poppedEntryIndex;
}

template <typename Key, typename Value, typename Hasher>
void StrongLRUCache<Key, Value, Hasher>::recycle()
{
    Require(_size == _capacity.value);

    Entry& sentinel = sentinelEntry();
    Require(sentinel.prevInLRU != 0);

    uint32_t const entryIndex = sentinel.prevInLRU;
    Entry& entry = _entries[entryIndex];
    Entry& prev = _entries[entry.prevInLRU];

    prev.nextInLRU = 0;
    sentinel.prevInLRU = entry.prevInLRU;

    validateChange(-1);

    uint32_t* nextIndex = hashTableSlot(entry.hashValue);
    while (*nextIndex != entryIndex)
    {
        Require(*nextIndex != 0);
        nextIndex = &_entries[*nextIndex].nextWithSameHash;
    }

    Guarantee(*nextIndex == entryIndex);
    *nextIndex = entry.nextWithSameHash;
    entry.nextWithSameHash = sentinel.nextWithSameHash;
    sentinel.nextWithSameHash = entryIndex;

    ++_stats.recycles;
}

template <typename Key, typename Value, typename Hasher>
inline int StrongLRUCache<Key, Value, Hasher>::validateChange(int adj)
{
#if defined(DEBUG_STRONG_LRU_CACHE)
    int count = 0;

    Entry& sentinel = sentinelEntry();
    size_t lastOrdering = sentinel.ordering;

    for (uint32_t entryIndex = sentinel.nextInLRU; entryIndex != 0;)
    {
        Entry& entry = _entries[entryIndex];
        Require(entry.ordering < lastOrdering);
        lastOrdering = entry.ordering;
        entryIndex = entry.nextInLRU;
        ++count;
    }
    auto const newLRUCount = _lastLRUCount + adj;
    Require(newLRUCount == count);
    _lastLRUCount = count;
    return newLRUCount;
#else
    return _size;
#endif
}

// }}}
// }}}

} // namespace crispy
