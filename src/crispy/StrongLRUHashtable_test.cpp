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
#include <crispy/StrongLRUHashtable.h>
#include <crispy/utils.h>

#include <catch2/catch.hpp>

#include <iostream>
#include <string_view>

using namespace crispy;
using namespace std;
using namespace std::string_view_literals;

namespace
{
// simple 32-bit hash
inline StrongHash h(uint32_t v)
{
    return StrongHash(0, 0, 0, v);
}

inline string sh(uint32_t value)
{
    return to_structured_string(h(value));
}

template <typename Value, typename... Values>
inline string sh(Value value, Value next, Values... more)
{
    auto left = to_structured_string(h(value));
    auto right = sh(next, more...);
    return left + ", " + right;
}

StrongHash collidingHash(uint32_t v) noexcept
{
    return StrongHash(0, 0, v, 0);
}

inline string ch(uint32_t value)
{
    return to_structured_string(collidingHash(value));
}

template <typename Value, typename... Values>
inline string ch(Value value, Value next, Values... more)
{
    auto left = to_structured_string(collidingHash(value));
    auto right = ch(next, more...);
    return left + ", " + right;
}
} // namespace

TEST_CASE("StrongHash", "")
{
    auto empty = StrongHash::compute("");
    auto a = StrongHash::compute("A");
    auto b = StrongHash::compute("AB");
    auto c = StrongHash::compute("ABC");
    auto d = StrongHash::compute("ABCD");
    auto e = StrongHash::compute("ABCDE");
    auto f = StrongHash::compute("ABCDEF");

    REQUIRE(a != empty);
    REQUIRE(a != b);
    REQUIRE(a != c);
    REQUIRE(a != d);
    REQUIRE(a != e);
    REQUIRE(a != f);
}

TEST_CASE("StrongLRUHashtable.operator_index", "")
{
    auto cachePtr = StrongLRUHashtable<int>::create(StrongHashtableSize { 8 }, LRUCapacity { 4 });
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

TEST_CASE("StrongLRUHashtable.at", "")
{
    auto cachePtr = StrongLRUHashtable<int>::create(StrongHashtableSize { 8 }, LRUCapacity { 4 });
    auto& cache = *cachePtr;
    for (int i = 1; i <= 4; ++i)
        cache[h(i)] = 2 * i;
    REQUIRE(joinHumanReadable(cache.hashes()) == sh(4, 3, 2, 1));

    CHECK_THROWS_AS(cache.at(h(-1)), std::out_of_range);
    CHECK_NOTHROW(cache.at(h(1)));
}

TEST_CASE("StrongLRUHashtable.clear", "[lrucache]")
{
    auto cachePtr = StrongLRUHashtable<int>::create(StrongHashtableSize { 8 }, LRUCapacity { 4 });
    auto& cache = *cachePtr;
    for (int i = 1; i <= 4; ++i)
        cache[h(i)] = 2 * i;
    REQUIRE(joinHumanReadable(cache.hashes()) == sh(4, 3, 2, 1));

    CHECK(cache.size() == 4);
    cache.clear();
    CHECK(cache.size() == 0);
}

TEST_CASE("StrongLRUHashtable.touch", "")
{
    auto cachePtr = StrongLRUHashtable<int>::create(StrongHashtableSize { 8 }, LRUCapacity { 4 });
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

TEST_CASE("StrongLRUHashtable.contains", "")
{
    auto cachePtr = StrongLRUHashtable<int>::create(StrongHashtableSize { 8 }, LRUCapacity { 4 });
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

TEST_CASE("StrongLRUHashtable.try_emplace", "")
{
    auto cachePtr = StrongLRUHashtable<int>::create(StrongHashtableSize { 4 }, LRUCapacity { 2 });
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

TEST_CASE("StrongLRUHashtable.try_get", "")
{
    auto cachePtr = StrongLRUHashtable<int>::create(StrongHashtableSize { 8 }, LRUCapacity { 4 });
    auto& cache = *cachePtr;
    for (int i = 1; i <= 4; ++i)
        cache[h(i)] = 2 * i;
    REQUIRE(joinHumanReadable(cache.hashes()) == sh(4, 3, 2, 1));

    // no-op (not found)
    REQUIRE(cache.try_get(h(-1)) == nullptr);
    REQUIRE(joinHumanReadable(cache.hashes()) == sh(4, 3, 2, 1));

    // no-op (found)
    auto const p1 = cache.try_get(h(4));
    REQUIRE(p1 != nullptr);
    REQUIRE(*p1 == 8);
    REQUIRE(joinHumanReadable(cache.hashes()) == sh(4, 3, 2, 1));

    // middle to front
    auto const p2 = cache.try_get(h(3));
    REQUIRE(p2 != nullptr);
    REQUIRE(*p2 == 6);
    REQUIRE(joinHumanReadable(cache.hashes()) == sh(3, 4, 2, 1));

    // back to front
    auto const p3 = cache.try_get(h(1));
    REQUIRE(p3 != nullptr);
    REQUIRE(*p3 == 2);
    REQUIRE(joinHumanReadable(cache.hashes()) == sh(1, 3, 4, 2));
}

static optional<int> createTwo(uint32_t entryIndex)
{
    return { -2 };
}

static optional<int> createOne(StrongLRUHashtable<int>& cache, uint32_t entryIndex, int** b)
{
    auto hash = h(2);
    *b = cache.get_or_try_emplace(hash, createTwo);
    return { -1 };
}

TEST_CASE("StrongLRUHashtable.get_or_try_emplace.recursive", "[lrucache]")
{
    auto cachePtr = StrongLRUHashtable<int>::create(StrongHashtableSize { 4 }, LRUCapacity { 2 });
    auto& cache = *cachePtr;

    auto hash1 = h(1);

    int* b = nullptr;
    int* a = cache.get_or_try_emplace(h(1), [&b, &cache](auto i) -> optional<int> {
        b = cache.get_or_try_emplace(h(2), [&](auto k) -> optional<int> { return { -2 }; });
        return { -1 };
    });

    REQUIRE(a != nullptr);
    CHECK(*a == -1);

    REQUIRE(b != nullptr);
    CHECK(*b == -2);
}

TEST_CASE("StrongLRUHashtable.get_or_try_emplace", "[lrucache]")
{
    auto cachePtr = StrongLRUHashtable<int>::create(StrongHashtableSize { 4 }, LRUCapacity { 2 });
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

TEST_CASE("StrongLRUHashtable.get_or_emplace", "[lrucache]")
{
    auto cachePtr = StrongLRUHashtable<int>::create(StrongHashtableSize { 4 }, LRUCapacity { 2 });
    auto& cache = *cachePtr;

    int& a = cache.get_or_emplace(h(2), [](auto) { return 4; });
    CHECK(a == 4);
    CHECK(cache.at(h(2)) == 4);
    CHECK(cache.size() == 1);
    CHECK(joinHumanReadable(cache.hashes()) == sh(2));

    int& a2 = cache.get_or_emplace(h(2), [](auto) { return -4; });
    CHECK(a2 == 4);
    CHECK(cache.at(h(2)) == 4);
    CHECK(cache.size() == 1);

    int& b = cache.get_or_emplace(h(3), [](auto) { return 6; });
    CHECK(b == 6);
    CHECK(cache.at(h(3)) == 6);
    CHECK(cache.size() == 2);
    CHECK(joinHumanReadable(cache.hashes()) == sh(3, 2));

    int& c = cache.get_or_emplace(h(4), [](auto) { return 8; });
    CHECK(joinHumanReadable(cache.hashes()) == sh(4, 3));
    CHECK(c == 8);
    CHECK(cache.at(h(4)) == 8);
    CHECK(cache.size() == 2);
    CHECK(cache.contains(h(3)));
    CHECK_FALSE(cache.contains(h(2))); // thrown out

    int& b2 = cache.get_or_emplace(h(3), [](auto) { return -3; });
    CHECK(joinHumanReadable(cache.hashes()) == sh(3, 4));
    CHECK(b2 == 6);
    CHECK(cache.at(h(3)) == 6);
    CHECK(cache.size() == 2);
}

TEST_CASE("StrongLRUHashtable.remove", "")
{
    auto cachePtr = StrongLRUHashtable<int>::create(StrongHashtableSize { 8 }, LRUCapacity { 4 });
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
    REQUIRE(joinHumanReadable(cache.hashes()) == "");
}

TEST_CASE("StrongLRUHashtable.insert_with_cache_collision", "")
{
    auto cachePtr = StrongLRUHashtable<int>::create(StrongHashtableSize { 8 }, LRUCapacity { 4 });
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

TEST_CASE("StrongLRUHashtable.remove_with_hashTable_lookup_collision", "")
{
    auto cachePtr = StrongLRUHashtable<int>::create(StrongHashtableSize { 8 }, LRUCapacity { 4 });
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
    REQUIRE(joinHumanReadable(cache.hashes()) == "");
}

TEST_CASE("StrongLRUHashtable.peek", "")
{
    auto cachePtr = StrongLRUHashtable<int>::create(StrongHashtableSize { 8 }, LRUCapacity { 4 });
    auto& cache = *cachePtr;
    for (int i = 1; i <= 4; ++i)
        cache[h(i)] = 2 * i;

    for (int i = 1; i <= 4; ++i)
    {
        INFO(fmt::format("i: {}", i))
        REQUIRE(cache.peek(h(1)) == 2);
        REQUIRE(joinHumanReadable(cache.hashes()) == sh(4, 3, 2, 1));
    }
}
