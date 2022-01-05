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
#include <crispy/assert.h>

#include <immintrin.h>
#include <optional>
#include <ostream>
#include <stdexcept>
#include <vector>

#define DEBUG_STRONG_LRU_HASHTABLE 1

#if defined(NDEBUG) && defined(DEBUG_STRONG_LRU_HASHTABLE)
    #undef DEBUG_STRONG_LRU_HASHTABLE
#endif

namespace crispy
{

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

struct LRUHashtableStats
{
    uint32_t hits;
    uint32_t misses;
    uint32_t recycles;
};

// Defines the number of hashes the hashtable can store.
struct StrongHashtableSize
{
    uint32_t value;
};

// Number of entries the LRU StrongLRUHashtable can store at most.
struct LRUCapacity
{
    uint32_t value;
};

// LRU hashtable implementation with the goal to minimize runtime allocations
// and maximize speed.
//
// NOTE!
//     Cache locality could be further improved by having one single
//     memory region instead of two.
//     Or even overloading operator new of StrongLRUHashtable
//     and put the dynamic data at the end of the primary data.
template <typename Value>
class StrongLRUHashtable
{
  private:
    StrongLRUHashtable(StrongHashtableSize hashCount, LRUCapacity entryCount);

  public:
    ~StrongLRUHashtable();

    using CachePtr = std::unique_ptr<StrongLRUHashtable, std::function<void(StrongLRUHashtable*)>>;

    static constexpr inline size_t requiredMemorySize(StrongHashtableSize hashCount, LRUCapacity entryCount);

    template <typename Allocator = std::allocator<unsigned char>>
    static CachePtr create(StrongHashtableSize hashCount, LRUCapacity entryCount);

    /// Returns the actual number of entries currently hold in this hashtable.
    [[nodiscard]] size_t size() const noexcept;

    /// Returns the maximum number of entries that can be stored in this hashtable.
    [[nodiscard]] size_t capacity() const noexcept;

    /// Returns the total storage sized used by this object.
    [[nodiscard]] size_t storageSize() const noexcept;

    /// Returns gathered stats and clears the local stats state to start
    /// counting from zero again.
    LRUHashtableStats fetchAndClearStats() noexcept;

    /// Clears all entries from the hashtable.
    void clear();

    // Delets the hash entry and its associated value from the LRU hashtable
    void erase(StrongHash const& hash);

    /// Touches a given hash key, putting it to the front of the LRU chain.
    /// Nothing is done if the hash key was not found.
    void touch(StrongHash const& hash) noexcept;

    /// Returns an ordered list of keys in this hash.
    /// Ordering is from most recent to least recent access.
    [[nodiscard]] std::vector<StrongHash> hashes() const;

    /// Tests for the exitence of the given hash key in this hash table.
    [[nodiscard]] bool contains(StrongHash const& hash) const noexcept;

    /// Returns the value for the given hash key if found, nullptr otherwise.
    [[nodiscard]] Value* try_get(StrongHash const& hash);
    [[nodiscard]] Value const* try_get(StrongHash const& hash) const;

    /// Returns the value for the given hash key,
    /// throwing std::out_of_range if hash key was not found.
    [[nodiscard]] Value& at(StrongHash const& hash);

    /// like at() but does not change LRU order.
    [[nodiscard]] Value& peek(StrongHash const& hash);
    [[nodiscard]] Value const& peek(StrongHash const& hash) const;

    /// Returns the value for the given hash key, default-constructing it in case
    /// if it wasn't in the hashtable just yet.
    [[nodiscard]] Value& operator[](StrongHash const& hash) noexcept;

    /// Assignes the given value to the given hash key.
    /// If the hash key was not found, it is being created,
    /// otherwise the value will be re-assigned with the new value.
    Value& emplace(StrongHash const& hash, Value value) noexcept;

    /// Conditionally creates a new item to the LRU-Cache iff its hash key
    /// was not present yet.
    ///
    /// @retval true the hash key did not exist in hashtable yet, a new value was constructed.
    /// @retval false The hash key is already in the hashtable, no entry was constructed.
    template <typename ValueConstructFn>
    [[nodiscard]] bool try_emplace(StrongHash const& hash, ValueConstructFn constructValue);

    /// Always returns either the existing item by the given hash key, if found,
    /// or a newly created one by invoking constructValue().
    template <typename ValueConstructFn>
    [[nodiscard]] Value& get_or_emplace(StrongHash const& hash, ValueConstructFn constructValue);

    void inspect(std::ostream& output) const;

    // {{{ public detail
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

        std::optional<Value> value = std::nullopt;

#if defined(DEBUG_STRONG_LRU_HASHTABLE)
        uint32_t ordering = 0;
#endif
    };
    // }}}

  private:
    // {{{ details
    // Maps the given hash hash key to a slot in the hash table.
    uint32_t* hashTableSlot(StrongHash const& hash);

    // Returns entry index to an unused entry, possibly by evicting
    // the least recently used entry if no free entries are available.
    //
    // This entry is not inserted into the LRU-chain yet.
    uint32_t allocateEntry();

    // Returns the index to the entry associated with the given hash key.
    // If the hash key was not found and force is set to true, it'll be created,
    // otherwise 0 is returned.
    uint32_t findEntry(StrongHash const& hash, bool force);

    // Evicts the least recently used entry in the LRU chain
    // and links it to the unused-entries chain.
    //
    // Requires the hashtable to be full.
    void recycle();

    int validateChange(int adj);

    Entry& sentinelEntry() noexcept { return _entries[0]; }
    Entry const& sentinelEntry() const noexcept { return _entries[0]; }
    // }}}

    LRUHashtableStats _stats;
    uint32_t _hashMask;
    StrongHashtableSize _hashCount;
    uint32_t _size;
    LRUCapacity _capacity;

    // The hash table maps hash codes to indices into the entry table.
    uint32_t* _hashTable;
    Entry* _entries;

#if defined(DEBUG_STRONG_LRU_HASHTABLE)
    int _lastLRUCount = 0;
#endif
};

// {{{ implementation

template <typename Value>
StrongLRUHashtable<Value>::StrongLRUHashtable(StrongHashtableSize hashCount, LRUCapacity entryCount):
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

template <typename Value>
StrongLRUHashtable<Value>::~StrongLRUHashtable()
{
    std::destroy_n(_entries, 1 + _capacity.value);
}

template <typename Value>
constexpr inline size_t StrongLRUHashtable<Value>::requiredMemorySize(StrongHashtableSize hashCount,
                                                                      LRUCapacity entryCount)
{
    Require(detail::isPowerOfTwo(hashCount.value)); // Hash capacity must be power of 2.
    Require(hashCount.value >= 1);
    Require(entryCount.value >= 2);

    auto const hashSize = hashCount.value * sizeof(uint32_t);
    auto const entrySize = (1 + entryCount.value) * sizeof(Entry) + std::alignment_of_v<Entry>;
    auto const totalSize = sizeof(StrongLRUHashtable) + hashSize + entrySize;
    return totalSize;
}

template <typename Value>
template <typename Allocator>
auto StrongLRUHashtable<Value>::create(StrongHashtableSize hashCount, LRUCapacity entryCount) -> CachePtr
{
    // payload memory layout
    // =====================
    //
    // [object  attribs]
    // uint32_t[] hash table
    // Entry[]    entries

    Allocator allocator;
    auto const size = requiredMemorySize(hashCount, entryCount);
    StrongLRUHashtable* obj = (StrongLRUHashtable*) allocator.allocate(size);

    // clang-format off
    if (!obj)
        return CachePtr { nullptr, [](auto) {} };
    // clang-format on

    memset(obj, 0, size);
    new (obj) StrongLRUHashtable(hashCount, entryCount);

    auto deleter = [size, allocator = std::move(allocator)](auto p) mutable {
        std::destroy_n(p, 1);
        allocator.deallocate(reinterpret_cast<typename Allocator::pointer>(p), size);
    };

    return CachePtr(obj, std::move(deleter));
}

template <typename Value>
inline size_t StrongLRUHashtable<Value>::size() const noexcept
{
    return _size;
}

template <typename Value>
inline size_t StrongLRUHashtable<Value>::capacity() const noexcept
{
    return _capacity.value;
}

template <typename Value>
inline size_t StrongLRUHashtable<Value>::storageSize() const noexcept
{
    auto const hashTableSize = _hashCount.value * sizeof(uint32_t);

    // +1 for sentinel entry in the front
    // +1 for alignment
    auto const entryTableSize = (2 + _capacity.value) * sizeof(Entry);

    return sizeof(StrongLRUHashtable) + hashTableSize + entryTableSize;
}

template <typename Value>
inline uint32_t* StrongLRUHashtable<Value>::hashTableSlot(StrongHash const& hash)
{
    uint32_t const index = _mm_cvtsi128_si32(hash.value);
    uint32_t const slot = index & _hashMask;
    return _hashTable + slot;
}

template <typename Value>
LRUHashtableStats StrongLRUHashtable<Value>::fetchAndClearStats() noexcept
{
    auto st = _stats;
    _stats = LRUHashtableStats {};
    return st;
}

template <typename Value>
void StrongLRUHashtable<Value>::clear()
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

template <typename Value>
void StrongLRUHashtable<Value>::erase(StrongHash const& hash)
{
    uint32_t* slot = hashTableSlot(hash);
    uint32_t entryIndex = *slot;
    uint32_t prevWithSameHash = 0;
    Entry* entry = nullptr;

    while (entryIndex)
    {
        entry = _entries + entryIndex;
        if (entry->hashValue == hash)
            break;
        prevWithSameHash = entryIndex;
        entryIndex = entry->nextWithSameHash;
    }

    if (entryIndex == 0)
        return;

    Entry& prev = _entries[entry->prevInLRU];
    Entry& next = _entries[entry->nextInLRU];

    // unlink from LRU chain
    prev.nextInLRU = entry->nextInLRU;
    next.prevInLRU = entry->prevInLRU;
    if (prevWithSameHash)
    {
        Require(_entries[prevWithSameHash].nextWithSameHash == entryIndex);
        _entries[prevWithSameHash].nextWithSameHash = entry->nextWithSameHash;
    }
    else
        *slot = entry->nextWithSameHash;

    // relink into free-chain
    Entry& sentinel = sentinelEntry();
    entry->prevInLRU = 0;
    entry->nextInLRU = sentinel.nextInLRU;
    entry->nextWithSameHash = sentinel.nextWithSameHash;
    sentinel.nextWithSameHash = entryIndex;

    --_size;

    validateChange(-1);
}

template <typename Value>
inline void StrongLRUHashtable<Value>::touch(StrongHash const& hash) noexcept
{
    findEntry(hash, false);
}

template <typename Value>
inline bool StrongLRUHashtable<Value>::contains(StrongHash const& hash) const noexcept
{
    return const_cast<StrongLRUHashtable*>(this)->findEntry(hash, false) != 0;
}

template <typename Value>
inline Value* StrongLRUHashtable<Value>::try_get(StrongHash const& hash)
{
    uint32_t const entryIndex = findEntry(hash, false);
    if (!entryIndex)
        return nullptr;
    return &_entries[entryIndex].value.value();
}

template <typename Value>
inline Value const* StrongLRUHashtable<Value>::try_get(StrongHash const& hash) const
{
    return const_cast<StrongLRUHashtable*>(this)->try_get(hash);
}

template <typename Value>
inline Value& StrongLRUHashtable<Value>::at(StrongHash const& hash)
{
    uint32_t const entryIndex = findEntry(hash, false);
    if (!entryIndex)
        throw std::out_of_range("hash not in table");
    return *_entries[entryIndex].value;
}

template <typename Value>
inline Value& StrongLRUHashtable<Value>::peek(StrongHash const& hash)
{
    uint32_t* slot = hashTableSlot(hash);
    uint32_t entryIndex = *slot;
    while (entryIndex)
    {
        Entry& entry = _entries[entryIndex];
        if (entry.hashValue == hash)
            return *entry.value;
        entryIndex = entry.nextWithSameHash;
    }

    throw std::out_of_range("hash");
}

template <typename Value>
inline Value const& StrongLRUHashtable<Value>::peek(StrongHash const& hash) const
{
    return const_cast<StrongLRUHashtable*>(this)->peek(hash);
}

template <typename Value>
inline Value& StrongLRUHashtable<Value>::operator[](StrongHash const& hash) noexcept
{
    uint32_t const entryIndex = findEntry(hash, true);
    return *_entries[entryIndex].value;
}

template <typename Value>
template <typename ValueConstructFn>
inline bool StrongLRUHashtable<Value>::try_emplace(StrongHash const& hash, ValueConstructFn constructValue)
{
    if (contains(hash))
        return false;

    uint32_t const entryIndex = findEntry(hash, true);
    *_entries[entryIndex].value = constructValue(entryIndex);
    return true;
}

template <typename Value>
template <typename ValueConstructFn>
inline Value& StrongLRUHashtable<Value>::get_or_emplace(StrongHash const& hash,
                                                        ValueConstructFn constructValue)
{
    if (Value* p = try_get(hash))
        return *p;

    uint32_t const entryIndex = findEntry(hash, true);
    return *_entries[entryIndex].value = constructValue(entryIndex);
}

template <typename Value>
inline Value& StrongLRUHashtable<Value>::emplace(StrongHash const& hash, Value value) noexcept
{
    return (*this)[hash] = std::move(value);
}

template <typename Value>
std::vector<StrongHash> StrongLRUHashtable<Value>::hashes() const
{
    auto result = std::vector<StrongHash> {};
    Entry const& sentinel = sentinelEntry();
    for (uint32_t entryIndex = sentinel.nextInLRU; entryIndex != 0;)
    {
        Entry const& entry = _entries[entryIndex];
        result.emplace_back(entry.hashValue);
        entryIndex = entry.nextInLRU;
    }
    Guarantee(result.size() == _size);

    return result;
}

template <typename Value>
void StrongLRUHashtable<Value>::inspect(std::ostream& output) const
{
    Entry const& sentinel = sentinelEntry();
    uint32_t entryIndex = sentinel.prevInLRU;
    uint32_t hashSlotCollisions = 0;
    while (entryIndex != 0)
    {
        Entry const& entry = _entries[entryIndex];
        output << fmt::format("  entry[{:03}]   : LRU: {} <- -> {}, alt: {}; := {}\n",
                              entryIndex,
                              entry.prevInLRU,
                              entry.nextInLRU,
                              entry.nextWithSameHash,
                              entry.value.has_value() ? fmt::format("{}", entry.value.value()) : "nullopt");
        if (entry.nextWithSameHash)
            ++hashSlotCollisions;
        entryIndex = entry.prevInLRU;
    }

    output << "------------------------\n";
    output << fmt::format("hashslot collisions : {}\n", hashSlotCollisions);
    output << fmt::format("stats               : {}\n", _stats);
    output << fmt::format("hash table mask     : 0x{:04X}\n", _hashMask);
    output << fmt::format("hash table capacity : {}\n", _hashCount.value);
    output << fmt::format("entry count         : {}\n", _size);
    output << fmt::format("entry capacity      : {}\n", _capacity.value);
    output << "------------------------\n";
}

// {{{ helpers
template <typename Value>
uint32_t StrongLRUHashtable<Value>::findEntry(StrongHash const& hash, bool force)
{
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

#if defined(DEBUG_STRONG_LRU_HASHTABLE)
    result->ordering = sentinel.ordering++;
#endif

    Require(validateChange(1) == _size);

    return entryIndex;
}

template <typename Value>
uint32_t StrongLRUHashtable<Value>::allocateEntry()
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

template <typename Value>
void StrongLRUHashtable<Value>::recycle()
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

template <typename Value>
inline int StrongLRUHashtable<Value>::validateChange(int adj)
{
#if defined(DEBUG_STRONG_LRU_HASHTABLE)
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

// {{{ fmt
namespace fmt
{
template <>
struct formatter<crispy::LRUHashtableStats>
{
    template <typename ParseContext>
    constexpr auto parse(ParseContext& ctx)
    {
        return ctx.begin();
    }
    template <typename FormatContext>
    auto format(crispy::LRUHashtableStats stats, FormatContext& ctx)
    {
        return format_to(
            ctx.out(), "{} hits, {} misses, {} evictions", stats.hits, stats.misses, stats.recycles);
    }
};
} // namespace fmt
// }}}
