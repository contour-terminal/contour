// SPDX-License-Identifier: Apache-2.0
#include <crispy/StrongHash.h>
#include <crispy/StrongLRUHashtable.h>
#include <crispy/utils.h>

#include <catch2/catch_test_macros.hpp>

#include <iostream>
#include <string_view>

using namespace crispy;
using namespace std;
using namespace std::string_view_literals;

namespace
{
// simple 32-bit hash
template <typename T>
inline strong_hash h(T v)
{
    return strong_hash(0, 0, 0, static_cast<uint32_t>(v));
}

template <typename T>
inline string sh(T value)
{
    return to_structured_string(h(value));
}

template <typename Value, typename... Values>
inline string sh(Value first, Value second, Values... remaining)
{
    auto left = to_structured_string(h(first));
    auto right = sh(second, remaining...); // NOLINT(readability-suspicious-call-argument)
    return left + ", " + right;
}

template <typename T>
strong_hash collidingHash(T v) noexcept
{
    return strong_hash(0, 0, static_cast<uint32_t>(v), 0);
}

template <typename T>
inline string ch(T value)
{
    return to_structured_string(collidingHash(value));
}

template <typename Value, typename... Values>
inline string ch(Value first, Value second, Values... remaining)
{
    auto left = to_structured_string(collidingHash(first));
    auto right = ch(second, remaining...); // NOLINT(readability-suspicious-call-argument)
    return left + ", " + right;
}
} // namespace

TEST_CASE("strong_hash", "")
{
    auto empty = strong_hash::compute("");
    auto a = strong_hash::compute("A");
    auto b = strong_hash::compute("AB");
    auto c = strong_hash::compute("ABC");
    auto d = strong_hash::compute("ABCD");
    auto e = strong_hash::compute("ABCDE");
    auto f = strong_hash::compute("ABCDEF");

    REQUIRE(a != empty);
    REQUIRE(a != b);
    REQUIRE(a != c);
    REQUIRE(a != d);
    REQUIRE(a != e);
    REQUIRE(a != f);
}

TEST_CASE("strong_lru_hashtable.operator_index", "")
{
    auto cachePtr = strong_lru_hashtable<int>::create(strong_hashtable_size { 8 }, lru_capacity { 4 });
    auto& cache = *cachePtr;

    cache[h(1)] = 2;
    REQUIRE(cache[h(1)] == 2);
    REQUIRE(joinHumanReadable(cache.hashes()) == sh(1));

    cache[h(2)] = 4;
    REQUIRE(cache[h(2)] == 4);
    REQUIRE(joinHumanReadable(cache.hashes()) == sh(2, 1));

    cache[h(3)] = 6;
    REQUIRE(cache[h(3)] == 6);
    REQUIRE(joinHumanReadable(cache.hashes()) == sh(3, 2, 1));

    cache[h(4)] = 8;
    REQUIRE(cache[h(4)] == 8);
    REQUIRE(joinHumanReadable(cache.hashes()) == sh(4, 3, 2, 1));

    cache[h(5)] = 10;
    REQUIRE(cache[h(5)] == 10);
    REQUIRE(joinHumanReadable(cache.hashes()) == sh(5, 4, 3, 2));

    cache[h(6)] = 12;
    REQUIRE(cache[h(6)] == 12);
    REQUIRE(joinHumanReadable(cache.hashes()) == sh(6, 5, 4, 3));
}

TEST_CASE("strong_lru_hashtable.at", "")
{
    auto cachePtr = strong_lru_hashtable<int>::create(strong_hashtable_size { 8 }, lru_capacity { 4 });
    auto& cache = *cachePtr;
    for (int i = 1; i <= 4; ++i)
        cache[h(i)] = 2 * i;
    REQUIRE(joinHumanReadable(cache.hashes()) == sh(4, 3, 2, 1));

    CHECK_THROWS_AS(cache.at(h(-1)), std::out_of_range);
    CHECK_NOTHROW(cache.at(h(1)));
}

TEST_CASE("strong_lru_hashtable.clear", "[lrucache]")
{
    auto cachePtr = strong_lru_hashtable<int>::create(strong_hashtable_size { 8 }, lru_capacity { 4 });
    auto& cache = *cachePtr;
    for (int i = 1; i <= 4; ++i)
        cache[h(i)] = 2 * i;
    REQUIRE(joinHumanReadable(cache.hashes()) == sh(4, 3, 2, 1));

    CHECK(cache.size() == 4);
    cache.clear();
    CHECK(cache.size() == 0);
}

TEST_CASE("strong_lru_hashtable.touch", "")
{
    auto cachePtr = strong_lru_hashtable<int>::create(strong_hashtable_size { 8 }, lru_capacity { 4 });
    auto& cache = *cachePtr;
    for (int i = 1; i <= 4; ++i)
        cache[h(i)] = 2 * i;
    REQUIRE(joinHumanReadable(cache.hashes()) == sh(4, 3, 2, 1));

    // no-op (not found)
    cache.touch(h(-1));
    REQUIRE(joinHumanReadable(cache.hashes()) == sh(4, 3, 2, 1));

    // no-op (found)
    cache.touch(h(4));
    REQUIRE(joinHumanReadable(cache.hashes()) == sh(4, 3, 2, 1));

    // middle to front
    cache.touch(h(3));
    REQUIRE(joinHumanReadable(cache.hashes()) == sh(3, 4, 2, 1));

    // back to front
    cache.touch(h(1));
    REQUIRE(joinHumanReadable(cache.hashes()) == sh(1, 3, 4, 2));
}

TEST_CASE("strong_lru_hashtable.contains", "")
{
    auto cachePtr = strong_lru_hashtable<int>::create(strong_hashtable_size { 8 }, lru_capacity { 4 });
    auto& cache = *cachePtr;
    for (int i = 1; i <= 4; ++i)
        cache[h(i)] = i;
    REQUIRE(joinHumanReadable(cache.hashes()) == sh(4, 3, 2, 1));

    // not found: no-op
    REQUIRE(!cache.contains(h(-1)));
    REQUIRE(joinHumanReadable(cache.hashes()) == sh(4, 3, 2, 1));

    // found: front is no-op
    REQUIRE(cache.contains(h(4)));
    REQUIRE(joinHumanReadable(cache.hashes()) == sh(4, 3, 2, 1));

    // found: middle to front
    REQUIRE(cache.contains(h(3)));
    REQUIRE(joinHumanReadable(cache.hashes()) == sh(3, 4, 2, 1));

    // found: back to front
    REQUIRE(cache.contains(h(1)));
    REQUIRE(joinHumanReadable(cache.hashes()) == sh(1, 3, 4, 2));
}

TEST_CASE("strong_lru_hashtable.try_emplace", "")
{
    auto cachePtr = strong_lru_hashtable<int>::create(strong_hashtable_size { 4 }, lru_capacity { 2 });
    auto& cache = *cachePtr;

    auto rv = cache.try_emplace(h(2), [](auto) { return 4; });
    CHECK(rv);
    CHECK(joinHumanReadable(cache.hashes()) == sh(2));
    CHECK(cache.at(h(2)) == 4);

    rv = cache.try_emplace(h(3), [](auto) { return 6; });
    CHECK(rv);
    CHECK(joinHumanReadable(cache.hashes()) == sh(3, 2));
    CHECK(cache.at(h(2)) == 4);
    CHECK(cache.at(h(3)) == 6);

    rv = cache.try_emplace(h(2), [](auto) { return -1; });
    CHECK_FALSE(rv);
    CHECK(joinHumanReadable(cache.hashes()) == sh(2, 3));
    CHECK(cache.at(h(2)) == 4);
    CHECK(cache.at(h(3)) == 6);
}

TEST_CASE("strong_lru_hashtable.try_get", "")
{
    auto cachePtr = strong_lru_hashtable<int>::create(strong_hashtable_size { 8 }, lru_capacity { 4 });
    auto& cache = *cachePtr;
    for (int i = 1; i <= 4; ++i)
        cache[h(i)] = 2 * i;
    REQUIRE(joinHumanReadable(cache.hashes()) == sh(4, 3, 2, 1));

    // no-op (not found)
    REQUIRE(cache.try_get(h(-1)) == nullptr);
    REQUIRE(joinHumanReadable(cache.hashes()) == sh(4, 3, 2, 1));

    // no-op (found)
    auto* const p1 = cache.try_get(h(4));
    REQUIRE(p1 != nullptr);
    REQUIRE(*p1 == 8);
    REQUIRE(joinHumanReadable(cache.hashes()) == sh(4, 3, 2, 1));

    // middle to front
    auto* const p2 = cache.try_get(h(3));
    REQUIRE(p2 != nullptr);
    REQUIRE(*p2 == 6);
    REQUIRE(joinHumanReadable(cache.hashes()) == sh(3, 4, 2, 1));

    // back to front
    auto* const p3 = cache.try_get(h(1));
    REQUIRE(p3 != nullptr);
    REQUIRE(*p3 == 2);
    REQUIRE(joinHumanReadable(cache.hashes()) == sh(1, 3, 4, 2));
}

TEST_CASE("strong_lru_hashtable.get_or_try_emplace.recursive", "[lrucache]")
{
    auto cachePtr = strong_lru_hashtable<int>::create(strong_hashtable_size { 4 }, lru_capacity { 2 });
    auto& cache = *cachePtr;

    int* b = nullptr;
    int* a = cache.get_or_try_emplace(h(1), [&b, &cache](auto) -> optional<int> {
        b = cache.get_or_try_emplace(h(2), [&](auto) -> optional<int> { return { -2 }; });
        return { -1 };
    });

    REQUIRE(a != nullptr);
    CHECK(*a == -1);

    REQUIRE(b != nullptr);
    CHECK(*b == -2);
}

TEST_CASE("strong_lru_hashtable.get_or_try_emplace", "[lrucache]")
{
    auto cachePtr = strong_lru_hashtable<int>::create(strong_hashtable_size { 4 }, lru_capacity { 2 });
    auto& cache = *cachePtr;

    int* a = nullptr;

    a = cache.get_or_try_emplace(h(1), [](auto) -> optional<int> { return nullopt; });
    REQUIRE(!a);
    a = cache.get_or_try_emplace(h(1), [](auto i) -> optional<int> { return i; });
    REQUIRE(a);
    REQUIRE(*a == 1);
    CHECK(joinHumanReadable(cache.hashes()) == sh(1));

    a = cache.get_or_try_emplace(h(2), [](auto) -> optional<int> { return nullopt; });
    REQUIRE(!a);
    a = cache.get_or_try_emplace(h(2), [](auto i) -> optional<int> { return i; });
    REQUIRE(a);
    REQUIRE(*a == 2);
    CHECK(joinHumanReadable(cache.hashes()) == sh(2, 1));

    a = cache.get_or_try_emplace(h(3), [](auto) -> optional<int> { return nullopt; });
    REQUIRE(!a);
    a = cache.get_or_try_emplace(h(3), [](auto i) -> optional<int> { return i; });
    REQUIRE(a);
    REQUIRE_FALSE(cache.contains(h(1)));
    REQUIRE(*a == 1);
    CHECK(joinHumanReadable(cache.hashes()) == sh(3, 2));

    a = cache.get_or_try_emplace(h(4), [](auto) -> optional<int> { return nullopt; });
    REQUIRE(!a);
    a = cache.get_or_try_emplace(h(4), [](auto i) -> optional<int> { return i; });
    REQUIRE(a);
    REQUIRE_FALSE(cache.contains(h(2)));
    REQUIRE(*a == 2);
    CHECK(joinHumanReadable(cache.hashes()) == sh(4, 3));
}

TEST_CASE("strong_lru_hashtable.get_or_emplace", "[lrucache]")
{
    auto cachePtr = strong_lru_hashtable<int>::create(strong_hashtable_size { 4 }, lru_capacity { 2 });
    auto& cache = *cachePtr;

    int const& a = cache.get_or_emplace(h(2), [](auto) { return 4; });
    CHECK(a == 4);
    CHECK(cache.at(h(2)) == 4);
    CHECK(cache.size() == 1);
    CHECK(joinHumanReadable(cache.hashes()) == sh(2));

    int const& a2 = cache.get_or_emplace(h(2), [](auto) { return -4; });
    CHECK(a2 == 4);
    CHECK(cache.at(h(2)) == 4);
    CHECK(cache.size() == 1);

    int const& b = cache.get_or_emplace(h(3), [](auto) { return 6; });
    CHECK(b == 6);
    CHECK(cache.at(h(3)) == 6);
    CHECK(cache.size() == 2);
    CHECK(joinHumanReadable(cache.hashes()) == sh(3, 2));

    int const& c = cache.get_or_emplace(h(4), [](auto) { return 8; });
    CHECK(joinHumanReadable(cache.hashes()) == sh(4, 3));
    CHECK(c == 8);
    CHECK(cache.at(h(4)) == 8);
    CHECK(cache.size() == 2);
    CHECK(cache.contains(h(3)));
    CHECK_FALSE(cache.contains(h(2))); // thrown out

    int const& b2 = cache.get_or_emplace(h(3), [](auto) { return -3; });
    CHECK(joinHumanReadable(cache.hashes()) == sh(3, 4));
    CHECK(b2 == 6);
    CHECK(cache.at(h(3)) == 6);
    CHECK(cache.size() == 2);
}

TEST_CASE("strong_lru_hashtable.remove", "")
{
    auto cachePtr = strong_lru_hashtable<int>::create(strong_hashtable_size { 8 }, lru_capacity { 4 });
    auto& cache = *cachePtr;
    for (int i = 1; i <= 4; ++i)
        cache[h(i)] = 2 * i;
    REQUIRE(joinHumanReadable(cache.hashes()) == sh(4, 3, 2, 1));

    // remove at head
    cache.remove(h(4));
    REQUIRE(joinHumanReadable(cache.hashes()) == sh(3, 2, 1));

    // remove in middle
    cache.remove(h(2));
    REQUIRE(joinHumanReadable(cache.hashes()) == sh(3, 1));

    // remove at tail
    cache.remove(h(1));
    REQUIRE(joinHumanReadable(cache.hashes()) == sh(3));

    // remove last
    cache.remove(h(3));
    REQUIRE(joinHumanReadable(cache.hashes()).empty());
}

TEST_CASE("strong_lru_hashtable.insert_with_cache_collision", "")
{
    auto cachePtr = strong_lru_hashtable<int>::create(strong_hashtable_size { 8 }, lru_capacity { 4 });
    auto& cache = *cachePtr;

    cache[collidingHash(1)] = 1;
    CHECK(joinHumanReadable(cache.hashes()) == ch(1));

    cache[collidingHash(2)] = 2;
    CHECK(joinHumanReadable(cache.hashes()) == ch(2, 1));

    cache[collidingHash(3)] = 3;
    CHECK(joinHumanReadable(cache.hashes()) == ch(3, 2, 1));

    cache[collidingHash(4)] = 4;
    CHECK(joinHumanReadable(cache.hashes()) == ch(4, 3, 2, 1));

    // verify that we're having 3 cache collisions
    // cache.inspect(cout);
}

TEST_CASE("strong_lru_hashtable.remove_with_hashTable_lookup_collision", "")
{
    auto cachePtr = strong_lru_hashtable<int>::create(strong_hashtable_size { 8 }, lru_capacity { 4 });
    auto& cache = *cachePtr;
    for (int i = 1; i <= 4; ++i)
        cache[collidingHash(i)] = 2 * i;
    REQUIRE(joinHumanReadable(cache.hashes()) == ch(4, 3, 2, 1));

    // remove at head
    cache.remove(collidingHash(4));
    REQUIRE(joinHumanReadable(cache.hashes()) == ch(3, 2, 1));

    // remove in middle
    cache.remove(collidingHash(2));
    REQUIRE(joinHumanReadable(cache.hashes()) == ch(3, 1));

    // remove at tail
    cache.remove(collidingHash(1));
    REQUIRE(joinHumanReadable(cache.hashes()) == ch(3));

    // remove last
    cache.remove(collidingHash(3));
    REQUIRE(joinHumanReadable(cache.hashes()).empty());
}

TEST_CASE("strong_lru_hashtable.peek", "")
{
    auto cachePtr = strong_lru_hashtable<int>::create(strong_hashtable_size { 8 }, lru_capacity { 4 });
    auto& cache = *cachePtr;
    for (int i = 1; i <= 4; ++i)
        cache[h(i)] = 2 * i;

    for (int i = 1; i <= 4; ++i)
    {
        INFO(std::format("i: {}", i));
        REQUIRE(cache.peek(h(1)) == 2);
        REQUIRE(joinHumanReadable(cache.hashes()) == sh(4, 3, 2, 1));
    }
}
