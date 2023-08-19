// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <crispy/StrongHash.h>
#include <crispy/assert.h>
#include <crispy/utils.h>

#include <optional>
#include <ostream>
#include <stdexcept>
#include <vector>

#include "StrongHash.h"

#define DEBUG_STRONG_LRU_HASHTABLE 1

#if defined(NDEBUG) && defined(DEBUG_STRONG_LRU_HASHTABLE)
    #undef DEBUG_STRONG_LRU_HASHTABLE
#endif

namespace crispy
{

// {{{ details
namespace detail
{
    constexpr bool isPowerOfTwo(uint32_t value) noexcept
    {
        //.
        return (value & (value - 1)) == 0;
    }
} // namespace detail
// }}}

struct lru_hashtable_stats
{
    uint32_t hits;
    uint32_t misses;
    uint32_t recycles;
};
} // namespace crispy

// {{{ fmt
template <>
struct fmt::formatter<crispy::lru_hashtable_stats>
{
    static auto parse(format_parse_context& ctx) -> format_parse_context::iterator { return ctx.begin(); }
    static auto format(crispy::lru_hashtable_stats stats, format_context& ctx) -> format_context::iterator
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
// }}}

namespace crispy
{
// Defines the number of hashes the hashtable can store.
struct strong_hashtable_size
{
    uint32_t value;
};

// Number of entries the LRU strong_lru_hashtable can store at most.
struct lru_capacity
{
    uint32_t value;
};

// LRU hashtable implementation with the goal to minimize runtime allocations
// and maximize speed.
//
// NOTE!
//     Cache locality could be further improved by having one single
//     memory region instead of two.
//     Or even overloading operator new of strong_lru_hashtable
//     and put the dynamic data at the end of the primary data.
template <typename Value>
class strong_lru_hashtable
{
  private:
    strong_lru_hashtable(strong_hashtable_size hashCount, lru_capacity entryCount, std::string name);

  public:
    ~strong_lru_hashtable();

    using ptr = std::unique_ptr<strong_lru_hashtable, std::function<void(strong_lru_hashtable*)>>;

    [[nodiscard]] static constexpr inline size_t requiredMemorySize(strong_hashtable_size hashCount,
                                                                    lru_capacity entryCount);

    template <typename Allocator = std::allocator<unsigned char>>
    [[nodiscard]] static ptr create(strong_hashtable_size hashCount,
                                    lru_capacity entryCount,
                                    std::string name = "");

    /// Returns the actual number of entries currently hold in this hashtable.
    [[nodiscard]] size_t size() const noexcept;

    /// Returns the maximum number of entries that can be stored in this hashtable.
    [[nodiscard]] size_t capacity() const noexcept;

    /// Returns the total storage sized used by this object.
    [[nodiscard]] size_t storageSize() const noexcept;

    /// Returns gathered stats and clears the local stats state to start
    /// counting from zero again.
    lru_hashtable_stats fetchAndClearStats() noexcept;

    /// Clears all entries from the hashtable.
    void clear();

    // Deletes the hash entry and its associated value from the LRU hashtable
    void remove(strong_hash const& hash);

    /// Touches a given hash key, putting it to the front of the LRU chain.
    /// Nothing is done if the hash key was not found.
    void touch(strong_hash const& hash) noexcept;

    /// Returns an ordered list of keys in this hash.
    /// Ordering is from most recent to least recent access.
    [[nodiscard]] std::vector<strong_hash> hashes() const;

    /// Tests for the exitence of the given hash key in this hash table.
    [[nodiscard]] bool contains(strong_hash const& hash) const noexcept;

    /// Returns the value for the given hash key if found, nullptr otherwise.
    [[nodiscard]] Value* try_get(strong_hash const& hash) noexcept;
    [[nodiscard]] Value const* try_get(strong_hash const& hash) const noexcept;

    /// Returns the value for the given hash key,
    /// throwing std::out_of_range if hash key was not found.
    [[nodiscard]] Value& at(strong_hash const& hash);

    /// like at() but does not change LRU order.
    [[nodiscard]] Value& peek(strong_hash const& hash);
    [[nodiscard]] Value const& peek(strong_hash const& hash) const;

    /// Returns the value for the given hash key, default-constructing it in case
    /// if it wasn't in the hashtable just yet.
    [[nodiscard]] Value& operator[](strong_hash const& hash) noexcept;

    /// Assignes the given value to the given hash key.
    /// If the hash key was not found, it is being created,
    /// otherwise the value will be re-assigned with the new value.
    Value& emplace(strong_hash const& hash, Value value) noexcept;

    template <typename ValueConstructFn>
    Value& emplace(strong_hash const& hash, ValueConstructFn constructValue) noexcept;

    /// Conditionally creates a new item to the LRU-Cache iff its hash key
    /// was not present yet.
    ///
    /// @retval true the hash key did not exist in hashtable yet, a new value was constructed.
    /// @retval false The hash key is already in the hashtable, no entry was constructed.
    template <typename ValueConstructFn>
    [[nodiscard]] bool try_emplace(strong_hash const& hash, ValueConstructFn constructValue);

    /// Always returns either the existing item by the given hash key, if found,
    /// or a newly created one by invoking constructValue().
    template <typename ValueConstructFn>
    [[nodiscard]] Value& get_or_emplace(strong_hash const& hash, ValueConstructFn constructValue);

    /**
     * Like get_or_emplace but allows failure in @p constructValue call to cause the hash entry not
     * to be created.
     *
     * Tries to get an existing entry and returns a reference to it,
     * otherwise tries to create an entry, puts into the hashtable and returns a reference to it.
     * If creation has failed, nullptr is returned instead.
     */
    template <typename ValueConstructFn>
    [[nodiscard]] Value* get_or_try_emplace(strong_hash const& hash, ValueConstructFn constructValue);

    /// Retrieves the value stored at the given entry index.
    [[nodiscard]] Value& valueAtEntryIndex(uint32_t entryIndex) noexcept;

    /// Retrieves the value stored at the given entry index.
    [[nodiscard]] Value const& valueAtEntryIndex(uint32_t entryIndex) const noexcept;

    void inspect(std::ostream& output) const;

    // {{{ public detail
    struct next_with_same_hash
    {
        explicit next_with_same_hash(uint32_t v): value { v } {}
        uint32_t value;
    };

    struct entry
    {
        entry(entry const&) = default;
        entry(entry&&) noexcept = default;
        entry& operator=(entry const&) = default;
        entry& operator=(entry&&) noexcept = default;
        entry(next_with_same_hash initialNextWithSameHash): nextWithSameHash { initialNextWithSameHash.value }
        {
        }

        strong_hash hashValue {};

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
    [[nodiscard]] uint32_t* hashTableSlot(strong_hash const& hash) noexcept;

    // Returns entry index to an unused entry, possibly by evicting
    // the least recently used entry if no free entries are available.
    //
    // This entry is not inserted into the LRU-chain yet.
    [[nodiscard]] uint32_t allocateEntry(strong_hash const& hash, uint32_t* slot);

    // Relinks the given entry to the front of the LRU-chain.
    void linkToLRUChainHead(uint32_t entryIndex) noexcept;

    // Unlinks given entry from LRU chain without touching the entry itself.
    void unlinkFromLRUChain(entry& entry) noexcept;

    // Returns the index to the entry associated with the given hash key.
    // If the hash key was not found and force is set to true, it'll be created,
    // otherwise 0 is returned.
    [[nodiscard]] uint32_t findEntry(strong_hash const& hash, bool force);

    // Evicts the least recently used entry in the LRU chain
    // and links it to the unused-entries chain.
    //
    // Requires the hashtable to be full.
    void recycle();

    int validateChange(int adj);

    [[nodiscard]] entry& sentinelEntry() noexcept { return _entries[0]; }
    [[nodiscard]] entry const& sentinelEntry() const noexcept { return _entries[0]; }
    // }}}

    lru_hashtable_stats _stats;
    uint32_t _hashMask;
    strong_hashtable_size _hashCount;
    uint32_t _size = 0;
    lru_capacity _capacity;
    std::string _name;

    // The hash table maps hash codes to indices into the entry table.
    uint32_t* _hashTable;
    entry* _entries;

#if defined(DEBUG_STRONG_LRU_HASHTABLE)
    int _lastLRUCount = 0;
#endif
};

// {{{ implementation

template <typename Value>
strong_lru_hashtable<Value>::strong_lru_hashtable(strong_hashtable_size hashCount,
                                                  lru_capacity entryCount,
                                                  std::string name):
    _stats {},
    _hashMask { hashCount.value - 1 },
    _hashCount { hashCount },
    _capacity { entryCount },
    _name { std::move(name) },
    _hashTable { (uint32_t*) (this + 1) },
    _entries { [this]() {
        constexpr uintptr_t Alignment = std::alignment_of_v<entry>;
        static_assert(detail::isPowerOfTwo(Alignment));
        constexpr uintptr_t AlignMask = Alignment - 1;

        auto* hashTableEnd = _hashTable + _hashCount.value;
        auto* entryTable = (entry*) (uintptr_t((char*) hashTableEnd + AlignMask) & ~AlignMask);

        return entryTable;
    }() }
{
    Require(detail::isPowerOfTwo(hashCount.value));
    Require(hashCount.value >= 1);
    Require(entryCount.value >= 2);

    memset(_hashTable, 0, hashCount.value * sizeof(uint32_t));

    for (uint32_t entryIndex = 0; entryIndex <= entryCount.value; ++entryIndex)
    {
        entry* ent = _entries + entryIndex;
        new (ent) entry(next_with_same_hash(entryIndex + 1));
    }
    new (_entries + entryCount.value) entry(next_with_same_hash(0));
}

template <typename Value>
strong_lru_hashtable<Value>::~strong_lru_hashtable()
{
    std::destroy_n(_entries, 1 + _capacity.value);
}

template <typename Value>
constexpr inline size_t strong_lru_hashtable<Value>::requiredMemorySize(strong_hashtable_size hashCount,
                                                                        lru_capacity entryCount)
{
    Require(detail::isPowerOfTwo(hashCount.value)); // Hash capacity must be power of 2.
    Require(hashCount.value >= 1);
    Require(entryCount.value >= 2);

    auto const hashSize = hashCount.value * sizeof(uint32_t);
    auto const entrySize = (1 + entryCount.value) * sizeof(entry) + std::alignment_of_v<entry>;
    auto const totalSize = sizeof(strong_lru_hashtable) + hashSize + entrySize;
    return totalSize;
}

template <typename Value>
template <typename Allocator>
auto strong_lru_hashtable<Value>::create(strong_hashtable_size hashCount,
                                         lru_capacity entryCount,
                                         std::string name) -> ptr
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
    auto* obj = (strong_lru_hashtable*) allocator.allocate(size);

    // clang-format off
    if (!obj)
        return ptr { nullptr, [](auto) {} };
    // clang-format on

    memset((void*) obj, 0, size);
    new (obj) strong_lru_hashtable(hashCount, entryCount, std::move(name));

    auto deleter = [size, allocator = std::move(allocator)](auto p) mutable {
        std::destroy_n(p, 1);
        allocator.deallocate(reinterpret_cast<unsigned char*>(p), size);
    };

    return ptr(obj, std::move(deleter));
}

template <typename Value>
inline size_t strong_lru_hashtable<Value>::size() const noexcept
{
    return _size;
}

template <typename Value>
inline size_t strong_lru_hashtable<Value>::capacity() const noexcept
{
    return _capacity.value;
}

template <typename Value>
inline size_t strong_lru_hashtable<Value>::storageSize() const noexcept
{
    auto const hashTableSize = _hashCount.value * sizeof(uint32_t);

    // +1 for sentinel entry in the front
    // +1 for alignment
    auto const entryTableSize = (1 + _capacity.value) * sizeof(entry);

    return sizeof(strong_lru_hashtable) + hashTableSize + entryTableSize;
}

template <typename Value>
inline uint32_t* strong_lru_hashtable<Value>::hashTableSlot(strong_hash const& hash) noexcept
{
    auto const index = hash.d();
    auto const slot = index & _hashMask;
    return _hashTable + slot;
}

template <typename Value>
lru_hashtable_stats strong_lru_hashtable<Value>::fetchAndClearStats() noexcept
{
    auto st = _stats;
    _stats = lru_hashtable_stats {};
    return st;
}

template <typename Value>
void strong_lru_hashtable<Value>::clear()
{
    entry& sentinel = sentinelEntry();
    auto entryIndex = sentinel.nextInLRU;
    while (entryIndex)
    {
        entry& entry = _entries[entryIndex];
        entry.value.reset();
        entryIndex = entry.nextInLRU;
    }

    memset(_hashTable, 0, _hashCount.value * sizeof(uint32_t));

    while (entryIndex <= _capacity.value)
    {
        _entries[entryIndex] = entry(next_with_same_hash(1 + entryIndex));
        ++entryIndex;
    }
    _entries[_capacity.value] = entry(next_with_same_hash(0));

    auto const oldSize = static_cast<int>(_size);
    _size = 0;
    validateChange(-oldSize);
}

template <typename Value>
void strong_lru_hashtable<Value>::remove(strong_hash const& hash)
{
    uint32_t* slot = hashTableSlot(hash);
    uint32_t entryIndex = *slot;
    uint32_t prevWithSameHash = 0;
    entry* ent = nullptr;

    while (entryIndex)
    {
        ent = _entries + entryIndex;
        if (ent->hashValue == hash)
            break;
        prevWithSameHash = entryIndex;
        entryIndex = ent->nextWithSameHash;
    }

    if (entryIndex == 0)
        return;

    entry& prev = _entries[ent->prevInLRU];
    entry& next = _entries[ent->nextInLRU];

    // unlink from LRU chain
    prev.nextInLRU = ent->nextInLRU;
    next.prevInLRU = ent->prevInLRU;
    if (prevWithSameHash)
    {
        Require(_entries[prevWithSameHash].nextWithSameHash == entryIndex);
        _entries[prevWithSameHash].nextWithSameHash = ent->nextWithSameHash;
    }
    else
        *slot = ent->nextWithSameHash;

    // relink into free-chain
    entry& sentinel = sentinelEntry();
    ent->prevInLRU = 0;
    ent->nextInLRU = sentinel.nextInLRU;
    ent->nextWithSameHash = sentinel.nextWithSameHash;
    sentinel.nextWithSameHash = entryIndex;

    --_size;

    validateChange(-1);
}

template <typename Value>
inline void strong_lru_hashtable<Value>::touch(strong_hash const& hash) noexcept
{
    (void) findEntry(hash, false);
}

template <typename Value>
inline bool strong_lru_hashtable<Value>::contains(strong_hash const& hash) const noexcept
{
    return const_cast<strong_lru_hashtable*>(this)->findEntry(hash, false) != 0;
}

template <typename Value>
inline Value* strong_lru_hashtable<Value>::try_get(strong_hash const& hash) noexcept
{
    uint32_t const entryIndex = findEntry(hash, false);
    if (!entryIndex)
        return nullptr;
    return &_entries[entryIndex].value.value();
}

template <typename Value>
inline Value const* strong_lru_hashtable<Value>::try_get(strong_hash const& hash) const noexcept
{
    return const_cast<strong_lru_hashtable*>(this)->try_get(hash);
}

template <typename Value>
inline Value& strong_lru_hashtable<Value>::at(strong_hash const& hash)
{
    uint32_t const entryIndex = findEntry(hash, false);
    if (!entryIndex)
        throw std::out_of_range("hash not in table");
    return *_entries[entryIndex].value;
}

template <typename Value>
inline Value& strong_lru_hashtable<Value>::peek(strong_hash const& hash)
{
    uint32_t* slot = hashTableSlot(hash);
    uint32_t entryIndex = *slot;
    while (entryIndex)
    {
        entry& entry = _entries[entryIndex];
        if (entry.hashValue == hash)
            return *entry.value;
        entryIndex = entry.nextWithSameHash;
    }

    throw std::out_of_range("hash");
}

template <typename Value>
inline Value const& strong_lru_hashtable<Value>::peek(strong_hash const& hash) const
{
    return const_cast<strong_lru_hashtable*>(this)->peek(hash);
}

template <typename Value>
inline Value& strong_lru_hashtable<Value>::operator[](strong_hash const& hash) noexcept
{
    uint32_t const entryIndex = findEntry(hash, true);
    return *_entries[entryIndex].value;
}

template <typename Value>
template <typename ValueConstructFn>
inline bool strong_lru_hashtable<Value>::try_emplace(strong_hash const& hash, ValueConstructFn constructValue)
{
    if (contains(hash))
        return false;

    uint32_t const entryIndex = findEntry(hash, true);
    _entries[entryIndex].value.emplace(constructValue(entryIndex));
    return true;
}

template <typename Value>
template <typename ValueConstructFn>
inline Value& strong_lru_hashtable<Value>::get_or_emplace(strong_hash const& hash,
                                                          ValueConstructFn constructValue)
{
    uint32_t* slot = hashTableSlot(hash);

    uint32_t entryIndex = *slot;
    while (entryIndex)
    {
        entry& candidateEntry = _entries[entryIndex];
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
    entry& result = _entries[entryIndex];
    result.value.emplace(constructValue(entryIndex)); // TODO: not yet exception safe
    return *result.value;
}

template <typename Value>
template <typename ValueConstructFn>
inline Value* strong_lru_hashtable<Value>::get_or_try_emplace(strong_hash const& hash,
                                                              ValueConstructFn constructValue)
{
    uint32_t* slot = hashTableSlot(hash);

    uint32_t entryIndex = *slot;
    while (entryIndex)
    {
        entry& candidateEntry = _entries[entryIndex];
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
    entry& result = _entries[entryIndex];

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
Value& strong_lru_hashtable<Value>::valueAtEntryIndex(uint32_t entryIndex) noexcept
{
    return _entries[entryIndex];
}

template <typename Value>
Value const& strong_lru_hashtable<Value>::valueAtEntryIndex(uint32_t entryIndex) const noexcept
{
    return _entries[entryIndex];
}

template <typename Value>
inline Value& strong_lru_hashtable<Value>::emplace(strong_hash const& hash, Value value) noexcept
{
    uint32_t const entryIndex = findEntry(hash, true);
    return *_entries[entryIndex].value = std::move(value);
}

template <typename Value>
template <typename ValueConstructFn>
Value& strong_lru_hashtable<Value>::emplace(strong_hash const& hash, ValueConstructFn constructValue) noexcept
{
    uint32_t const entryIndex = findEntry(hash, true);
    _entries[entryIndex].value.emplace(constructValue(entryIndex));
    return *_entries[entryIndex].value;
}

template <typename Value>
std::vector<strong_hash> strong_lru_hashtable<Value>::hashes() const
{
    auto result = std::vector<strong_hash> {};
    entry const& sentinel = sentinelEntry();
    for (uint32_t entryIndex = sentinel.nextInLRU; entryIndex != 0;)
    {
        entry const& entry = _entries[entryIndex];
        result.emplace_back(entry.hashValue);
        entryIndex = entry.nextInLRU;
    }
    Guarantee(result.size() == _size);

    return result;
}

template <typename Value>
void strong_lru_hashtable<Value>::inspect(std::ostream& output) const
{
    entry const& sentinel = sentinelEntry();
    uint32_t entryIndex = sentinel.prevInLRU;
    uint32_t hashSlotCollisions = 0;
    while (entryIndex != 0)
    {
        entry const& entry = _entries[entryIndex];
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
uint32_t strong_lru_hashtable<Value>::findEntry(strong_hash const& hash, bool force)
{
    uint32_t* slot = hashTableSlot(hash);
    uint32_t entryIndex = *slot;
    entry* result = nullptr;
    while (entryIndex)
    {
        entry& entry = _entries[entryIndex];
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
inline void strong_lru_hashtable<Value>::unlinkFromLRUChain(entry& ent) noexcept
{
    entry& prev = _entries[ent.prevInLRU];
    entry& next = _entries[ent.nextInLRU];
    prev.nextInLRU = ent.nextInLRU;
    next.prevInLRU = ent.prevInLRU;

    validateChange(-1);
}

template <typename Value>
inline void strong_lru_hashtable<Value>::linkToLRUChainHead(uint32_t entryIndex) noexcept
{
    // The entry must be already unlinked

    entry& sentinel = sentinelEntry();
    entry& oldHead = _entries[sentinel.nextInLRU];
    entry& newHead = _entries[entryIndex];

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
uint32_t strong_lru_hashtable<Value>::allocateEntry(strong_hash const& hash, uint32_t* slot)
{
    entry& sentinel = sentinelEntry();

    if (sentinel.nextWithSameHash == 0)
        recycle();
    else
        ++_size;

    uint32_t poppedEntryIndex = sentinel.nextWithSameHash;
    Require(1 <= poppedEntryIndex && poppedEntryIndex <= _capacity.value);

    entry& poppedEntry = _entries[poppedEntryIndex];
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
void strong_lru_hashtable<Value>::recycle()
{
    Require(_size == _capacity.value);

    entry& sentinel = sentinelEntry();
    Require(sentinel.prevInLRU != 0);

    uint32_t const entryIndex = sentinel.prevInLRU;
    entry& ent = _entries[entryIndex];
    entry& prev = _entries[ent.prevInLRU];

    prev.nextInLRU = 0;
    sentinel.prevInLRU = ent.prevInLRU;

    validateChange(-1);

    uint32_t* nextIndex = hashTableSlot(ent.hashValue);
    while (*nextIndex != entryIndex)
    {
        Require(*nextIndex != 0);
        nextIndex = &_entries[*nextIndex].nextWithSameHash;
    }

    Guarantee(*nextIndex == entryIndex);
    *nextIndex = ent.nextWithSameHash;
    ent.nextWithSameHash = sentinel.nextWithSameHash;
    sentinel.nextWithSameHash = entryIndex;

    ++_stats.recycles;
}

template <typename Value>
inline int strong_lru_hashtable<Value>::validateChange(int adj)
{
#if defined(DEBUG_STRONG_LRU_HASHTABLE)
    int count = 0;

    entry& sentinel = sentinelEntry();
    size_t lastOrdering = sentinel.ordering;

    for (uint32_t entryIndex = sentinel.nextInLRU; entryIndex != 0;)
    {
        entry& entry = _entries[entryIndex];
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
