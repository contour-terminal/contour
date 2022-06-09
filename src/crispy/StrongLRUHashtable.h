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
#include <crispy/utils.h>

#include <optional>
#include <ostream>
#include <stdexcept>
#include <vector>

#if defined(__x86_64__)
    #include <immintrin.h>
#elif defined(__aarch64__)
    #include <crispy/sse2neon.h>
#endif

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
        return fmt::format_to(
            ctx.out(),
            "{} hits, {} misses, {} evictions, {:.3}% hit rate",
            stats.hits,
            stats.misses,
            stats.recycles,
            stats.hits + stats.misses != 0
                ? 100.0 * (static_cast<double>(stats.hits) / static_cast<double>(stats.hits + stats.misses))
                : 0.0);
    }
};
} // namespace fmt
// }}}

namespace crispy
{
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
    StrongLRUHashtable(StrongHashtableSize hashCount, LRUCapacity entryCount, std::string name);

  public:
    ~StrongLRUHashtable();

    using Ptr = std::unique_ptr<StrongLRUHashtable, std::function<void(StrongLRUHashtable*)>>;

    static constexpr inline size_t requiredMemorySize(StrongHashtableSize hashCount, LRUCapacity entryCount);

    template <typename Allocator = std::allocator<unsigned char>>
    static Ptr create(StrongHashtableSize hashCount, LRUCapacity entryCount, std::string name = "");

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

    // Deletes the hash entry and its associated value from the LRU hashtable
    void remove(StrongHash const& hash);

    /// Touches a given hash key, putting it to the front of the LRU chain.
    /// Nothing is done if the hash key was not found.
    void touch(StrongHash const& hash) noexcept;

    /// Returns an ordered list of keys in this hash.
    /// Ordering is from most recent to least recent access.
    [[nodiscard]] std::vector<StrongHash> hashes() const;

    /// Tests for the exitence of the given hash key in this hash table.
    [[nodiscard]] bool contains(StrongHash const& hash) const noexcept;

    /// Returns the value for the given hash key if found, nullptr otherwise.
    [[nodiscard]] Value* try_get(StrongHash const& hash) noexcept;
    [[nodiscard]] Value const* try_get(StrongHash const& hash) const noexcept;

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

    template <typename ValueConstructFn>
    Value& emplace(StrongHash const& hash, ValueConstructFn constructValue) noexcept;

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

    /**
     * Like get_or_emplace but allows failure in @p constructValue call to cause the hash entry not
     * to be created.
     *
     * Tries to get an existing entry and returns a reference to it,
     * otherwise tries to create an entry, puts into the hashtable and returns a reference to it.
     * If creation has failed, nullptr is returned instead.
     */
    template <typename ValueConstructFn>
    [[nodiscard]] Value* get_or_try_emplace(StrongHash const& hash, ValueConstructFn constructValue);

    /// Retrieves the value stored at the given entry index.
    [[nodiscard]] Value& valueAtEntryIndex(uint32_t entryIndex) noexcept;

    /// Retrieves the value stored at the given entry index.
    [[nodiscard]] Value const& valueAtEntryIndex(uint32_t entryIndex) const noexcept;

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
    uint32_t* hashTableSlot(StrongHash const& hash) noexcept;

    // Returns entry index to an unused entry, possibly by evicting
    // the least recently used entry if no free entries are available.
    //
    // This entry is not inserted into the LRU-chain yet.
    uint32_t allocateEntry(StrongHash const& hash, uint32_t* slot);

    // Relinks the given entry to the front of the LRU-chain.
    void linkToLRUChainHead(uint32_t entryIndex) noexcept;

    // Unlinks given entry from LRU chain without touching the entry itself.
    void unlinkFromLRUChain(Entry& entry) noexcept;

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

    Entry& sentinelEntry() noexcept
    {
        return _entries[0];
    }
    Entry const& sentinelEntry() const noexcept
    {
        return _entries[0];
    }
    // }}}

    LRUHashtableStats _stats;
    uint32_t _hashMask;
    StrongHashtableSize _hashCount;
    uint32_t _size;
    LRUCapacity _capacity;
    std::string _name;

    // The hash table maps hash codes to indices into the entry table.
    uint32_t* _hashTable;
    Entry* _entries;

#if defined(DEBUG_STRONG_LRU_HASHTABLE)
    int _lastLRUCount = 0;
#endif
};

// {{{ implementation

template <typename Value>
StrongLRUHashtable<Value>::StrongLRUHashtable(StrongHashtableSize hashCount,
                                              LRUCapacity entryCount,
                                              std::string name):
    _stats {},
    _hashMask { hashCount.value - 1 },
    _hashCount { hashCount },
    _size { 0 },
    _capacity { entryCount },
    _name { std::move(name) },
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

    for (uint32_t entryIndex = 0; entryIndex <= entryCount.value; ++entryIndex)
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
auto StrongLRUHashtable<Value>::create(StrongHashtableSize hashCount,
                                       LRUCapacity entryCount,
                                       std::string name) -> Ptr
{
    // payload memory layout
    // =====================
    //
    // [object  attribs]
    // uint32_t[] hash table
    // Entry[]    entries

    if (!detail::isPowerOfTwo(hashCount.value))
        hashCount.value = nextPowerOfTwo(hashCount.value);

    Allocator allocator;
    auto const size = requiredMemorySize(hashCount, entryCount);
    StrongLRUHashtable* obj = (StrongLRUHashtable*) allocator.allocate(size);

    // clang-format off
    if (!obj)
        return Ptr { nullptr, [](auto) {} };
    // clang-format on

    memset((void*) obj, 0, size);
    new (obj) StrongLRUHashtable(hashCount, entryCount, std::move(name));

    auto deleter = [size, allocator = std::move(allocator)](auto p) mutable {
        std::destroy_n(p, 1);
        allocator.deallocate(reinterpret_cast<unsigned char*>(p), size);
    };

    return Ptr(obj, std::move(deleter));
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
    auto const entryTableSize = (1 + _capacity.value) * sizeof(Entry);

    return sizeof(StrongLRUHashtable) + hashTableSize + entryTableSize;
}

template <typename Value>
inline uint32_t* StrongLRUHashtable<Value>::hashTableSlot(StrongHash const& hash) noexcept
{
    uint32_t const index = static_cast<uint32_t>(_mm_cvtsi128_si32(hash.value));
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

    while (entryIndex <= _capacity.value)
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
void StrongLRUHashtable<Value>::remove(StrongHash const& hash)
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
inline Value* StrongLRUHashtable<Value>::try_get(StrongHash const& hash) noexcept
{
    uint32_t const entryIndex = findEntry(hash, false);
    if (!entryIndex)
        return nullptr;
    return &_entries[entryIndex].value.value();
}

template <typename Value>
inline Value const* StrongLRUHashtable<Value>::try_get(StrongHash const& hash) const noexcept
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
    _entries[entryIndex].value.emplace(constructValue(entryIndex));
    return true;
}

template <typename Value>
template <typename ValueConstructFn>
inline Value& StrongLRUHashtable<Value>::get_or_emplace(StrongHash const& hash,
                                                        ValueConstructFn constructValue)
{
    uint32_t* slot = hashTableSlot(hash);

    uint32_t entryIndex = *slot;
    while (entryIndex)
    {
        Entry& candidateEntry = _entries[entryIndex];
        if (candidateEntry.hashValue == hash)
        {
            ++_stats.hits;
            unlinkFromLRUChain(candidateEntry);
            linkToLRUChainHead(entryIndex);
            return *candidateEntry.value;
        }
        entryIndex = candidateEntry.nextWithSameHash;
    }

    Require(!entryIndex);

    ++_stats.misses;

    entryIndex = allocateEntry(hash, slot);
    Entry& result = _entries[entryIndex];
    result.value.emplace(constructValue(entryIndex)); // TODO: not yet exception safe
    return *result.value;
}

template <typename Value>
template <typename ValueConstructFn>
inline Value* StrongLRUHashtable<Value>::get_or_try_emplace(StrongHash const& hash,
                                                            ValueConstructFn constructValue)
{
    uint32_t* slot = hashTableSlot(hash);

    uint32_t entryIndex = *slot;
    while (entryIndex)
    {
        Entry& candidateEntry = _entries[entryIndex];
        if (candidateEntry.hashValue == hash)
        {
            ++_stats.hits;
            unlinkFromLRUChain(candidateEntry);
            linkToLRUChainHead(entryIndex);
            return &candidateEntry.value.value();
        }
        entryIndex = candidateEntry.nextWithSameHash;
    }

    Require(!entryIndex);

    ++_stats.misses;

    entryIndex = allocateEntry(hash, slot);
    Entry& result = _entries[entryIndex];

    Require(1 <= entryIndex && entryIndex <= _capacity.value);
    std::optional<Value> constructedValue = constructValue(entryIndex);
    if (!constructedValue)
    {
        remove(hash);
        return nullptr;
    }

    result.value = std::move(constructedValue);
    return &result.value.value();
}

template <typename Value>
Value& StrongLRUHashtable<Value>::valueAtEntryIndex(uint32_t entryIndex) noexcept
{
    return _entries[entryIndex];
}

template <typename Value>
Value const& StrongLRUHashtable<Value>::valueAtEntryIndex(uint32_t entryIndex) const noexcept
{
    return _entries[entryIndex];
}

template <typename Value>
inline Value& StrongLRUHashtable<Value>::emplace(StrongHash const& hash, Value value) noexcept
{
    uint32_t const entryIndex = findEntry(hash, true);
    return *_entries[entryIndex].value = std::move(value);
}

template <typename Value>
template <typename ValueConstructFn>
Value& StrongLRUHashtable<Value>::emplace(StrongHash const& hash, ValueConstructFn constructValue) noexcept
{
    uint32_t const entryIndex = findEntry(hash, true);
    _entries[entryIndex].value.emplace(constructValue(entryIndex));
    return *_entries[entryIndex].value;
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
        // output << fmt::format("  entry[{:03}]   : LRU: {} <- -> {}, alt: {}; := {}\n",
        //                       entryIndex,
        //                       entry.prevInLRU,
        //                       entry.nextInLRU,
        //                       entry.nextWithSameHash,
        //                       entry.value.has_value() ? fmt::format("{}", entry.value.value()) :
        //                       "nullopt");
        if (entry.nextWithSameHash)
            ++hashSlotCollisions;
        entryIndex = entry.prevInLRU;
    }

    auto const humanReadableUtiliation = [](auto a, auto b) -> std::string {
        auto const da = static_cast<double>(a);
        auto const db = static_cast<double>(b);
        auto const dr = (da / db) * 100.0;
        if (dr >= 99.99)
            return "100%";
        else
            return fmt::format("{:.02}%", dr);
    };

    output << fmt::format("=============================================================\n", _name);
    output << fmt::format("Hashtale: {}\n", _name);
    output << fmt::format("-------------------------------------------------------------\n", _name);
    output << fmt::format("hashslot collisions : {} ({})\n",
                          hashSlotCollisions,
                          humanReadableUtiliation(hashSlotCollisions, _hashCount.value));
    output << fmt::format("stats               : {}\n", _stats);
    output << fmt::format("hash table capacity : {} ({} utilization)\n",
                          _hashCount.value,
                          humanReadableUtiliation(_size, _hashCount.value));
    output << fmt::format("entry count         : {}\n", _size);
    output << fmt::format("entry capacity      : {} ({} utilization)\n",
                          _capacity.value,
                          humanReadableUtiliation(_size, _capacity.value));
    output << fmt::format("-------------------------------------------------------------\n", _name);
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
        unlinkFromLRUChain(*result);
        linkToLRUChainHead(entryIndex);
    }
    else if (force)
    {
        ++_stats.misses;

        entryIndex = allocateEntry(hash, slot);
        result = &_entries[entryIndex];
        result->value.emplace(Value {});
    }
    else
    {
        ++_stats.misses;
        return 0;
    }

    return entryIndex;
}

template <typename Value>
inline void StrongLRUHashtable<Value>::unlinkFromLRUChain(Entry& entry) noexcept
{
    Entry& prev = _entries[entry.prevInLRU];
    Entry& next = _entries[entry.nextInLRU];
    prev.nextInLRU = entry.nextInLRU;
    next.prevInLRU = entry.prevInLRU;

    validateChange(-1);
}

template <typename Value>
inline void StrongLRUHashtable<Value>::linkToLRUChainHead(uint32_t entryIndex) noexcept
{
    // The entry must be already unlinked

    Entry& sentinel = sentinelEntry();
    Entry& oldHead = _entries[sentinel.nextInLRU];
    Entry& newHead = _entries[entryIndex];

    newHead.nextInLRU = sentinel.nextInLRU;
    newHead.prevInLRU = 0;
    oldHead.prevInLRU = entryIndex;
    sentinel.nextInLRU = entryIndex;

#if defined(DEBUG_STRONG_LRU_HASHTABLE)
    newHead.ordering = sentinel.ordering++;
#endif

    Require(validateChange(1) == static_cast<int>(_size));
}

template <typename Value>
uint32_t StrongLRUHashtable<Value>::allocateEntry(StrongHash const& hash, uint32_t* slot)
{
    Entry& sentinel = sentinelEntry();

    if (sentinel.nextWithSameHash == 0)
        recycle();
    else
        ++_size;

    uint32_t poppedEntryIndex = sentinel.nextWithSameHash;
    Require(1 <= poppedEntryIndex && poppedEntryIndex <= _capacity.value);

    Entry& poppedEntry = _entries[poppedEntryIndex];
    sentinel.nextWithSameHash = poppedEntry.nextWithSameHash;
    poppedEntry.nextWithSameHash = 0;

    poppedEntry.value.reset();
    poppedEntry.hashValue = hash;
    poppedEntry.nextWithSameHash = *slot;
    *slot = poppedEntryIndex;

    linkToLRUChainHead(poppedEntryIndex);

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
    (void) adj;
    return int(_size);
#endif
}

// }}}
// }}}

} // namespace crispy
