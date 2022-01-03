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
#include <crispy/StrongLRUCache.h>
#include <crispy/utils.h>

#include <catch2/catch.hpp>

#include <iostream>
#include <string_view>

using namespace crispy;
using namespace std;
using namespace std::string_view_literals;

TEST_CASE("StrongLRUCache.operator_index", "")
{
    auto cachePtr =
        StrongLRUCache<int, string_view>::create(StrongHashCapacity { 8 }, StrongCacheCapacity { 4 });
    auto& cache = *cachePtr;

    cache[1] = "1"sv;
    REQUIRE(cache[1] == "1"sv);
    REQUIRE(joinHumanReadable(cache.keys()) == "1");

    cache[2] = "2"sv;
    REQUIRE(cache[2] == "2"sv);
    REQUIRE(joinHumanReadable(cache.keys()) == "2, 1");

    cache[3] = "3"sv;
    REQUIRE(cache[3] == "3"sv);
    REQUIRE(joinHumanReadable(cache.keys()) == "3, 2, 1");

    cache[4] = "4"sv;
    REQUIRE(cache[4] == "4"sv);
    REQUIRE(joinHumanReadable(cache.keys()) == "4, 3, 2, 1");

    cache[5] = "5"sv;
    REQUIRE(cache[5] == "5"sv);
    REQUIRE(joinHumanReadable(cache.keys()) == "5, 4, 3, 2");

    cache[6] = "6"sv;
    REQUIRE(cache[6] == "6"sv);
    REQUIRE(joinHumanReadable(cache.keys()) == "6, 5, 4, 3");
}

TEST_CASE("StrongLRUCache.at", "")
{
    auto cachePtr = StrongLRUCache<int, string>::create(StrongHashCapacity { 8 }, StrongCacheCapacity { 4 });
    auto& cache = *cachePtr;
    for (int i = 1; i <= 4; ++i)
        cache[i] = std::to_string(i);
    REQUIRE(joinHumanReadable(cache.keys()) == "4, 3, 2, 1");

    CHECK_THROWS_AS(cache.at(-1), std::out_of_range);
    CHECK_NOTHROW(cache.at(1));
}

TEST_CASE("StrongLRUCache.clear", "[lrucache]")
{
    auto cachePtr = StrongLRUCache<int, string>::create(StrongHashCapacity { 8 }, StrongCacheCapacity { 4 });
    auto& cache = *cachePtr;
    for (int i = 1; i <= 4; ++i)
        cache[i] = std::to_string(i);
    REQUIRE(joinHumanReadable(cache.keys()) == "4, 3, 2, 1");

    CHECK(cache.size() == 4);
    cache.clear();
    CHECK(cache.size() == 0);
}

TEST_CASE("StrongLRUCache.touch", "")
{
    auto cachePtr = StrongLRUCache<int, string>::create(StrongHashCapacity { 8 }, StrongCacheCapacity { 4 });
    auto& cache = *cachePtr;
    for (int i = 1; i <= 4; ++i)
        cache[i] = std::to_string(i);
    REQUIRE(joinHumanReadable(cache.keys()) == "4, 3, 2, 1");

    // no-op (not found)
    cache.touch(-1);
    REQUIRE(joinHumanReadable(cache.keys()) == "4, 3, 2, 1");

    // no-op (found)
    cache.touch(4);
    REQUIRE(joinHumanReadable(cache.keys()) == "4, 3, 2, 1");

    // middle to front
    cache.touch(3);
    REQUIRE(joinHumanReadable(cache.keys()) == "3, 4, 2, 1");

    // back to front
    cache.touch(1);
    REQUIRE(joinHumanReadable(cache.keys()) == "1, 3, 4, 2");
}

TEST_CASE("StrongLRUCache.contains", "")
{
    auto cachePtr = StrongLRUCache<int, string>::create(StrongHashCapacity { 8 }, StrongCacheCapacity { 4 });
    auto& cache = *cachePtr;
    for (int i = 1; i <= 4; ++i)
        cache[i] = std::to_string(i);
    REQUIRE(joinHumanReadable(cache.keys()) == "4, 3, 2, 1");

    // not found: no-op
    REQUIRE(!cache.contains(-1));
    REQUIRE(joinHumanReadable(cache.keys()) == "4, 3, 2, 1");

    // found: front is no-op
    REQUIRE(cache.contains(4));
    REQUIRE(joinHumanReadable(cache.keys()) == "4, 3, 2, 1");

    // found: middle to front
    REQUIRE(cache.contains(3));
    REQUIRE(joinHumanReadable(cache.keys()) == "3, 4, 2, 1");

    // found: back to front
    REQUIRE(cache.contains(1));
    REQUIRE(joinHumanReadable(cache.keys()) == "1, 3, 4, 2");
}

TEST_CASE("StrongLRUCache.try_emplace", "")
{
    auto cachePtr = StrongLRUCache<int, int>::create(StrongHashCapacity { 4 }, StrongCacheCapacity { 2 });
    auto& cache = *cachePtr;

    auto rv = cache.try_emplace(2, []() { return 4; });
    CHECK(rv);
    CHECK(joinHumanReadable(cache.keys()) == "2");
    CHECK(cache.at(2) == 4);

    rv = cache.try_emplace(3, []() { return 6; });
    CHECK(rv);
    CHECK(joinHumanReadable(cache.keys()) == "3, 2");
    CHECK(cache.at(2) == 4);
    CHECK(cache.at(3) == 6);

    rv = cache.try_emplace(2, []() { return -1; });
    CHECK_FALSE(rv);
    CHECK(joinHumanReadable(cache.keys()) == "2, 3");
    CHECK(cache.at(2) == 4);
    CHECK(cache.at(3) == 6);
}

TEST_CASE("StrongLRUCache.try_get", "")
{
    auto cachePtr = StrongLRUCache<int, string>::create(StrongHashCapacity { 8 }, StrongCacheCapacity { 4 });
    auto& cache = *cachePtr;
    for (int i = 1; i <= 4; ++i)
        cache[i] = std::to_string(i);
    REQUIRE(joinHumanReadable(cache.keys()) == "4, 3, 2, 1");

    // no-op (not found)
    REQUIRE(cache.try_get(-1) == nullptr);
    REQUIRE(joinHumanReadable(cache.keys()) == "4, 3, 2, 1");

    // no-op (found)
    auto const p1 = cache.try_get(4);
    REQUIRE(p1 != nullptr);
    REQUIRE(*p1 == "4");
    REQUIRE(joinHumanReadable(cache.keys()) == "4, 3, 2, 1");

    // middle to front
    auto const p2 = cache.try_get(3);
    REQUIRE(p2 != nullptr);
    REQUIRE(*p2 == "3");
    REQUIRE(joinHumanReadable(cache.keys()) == "3, 4, 2, 1");

    // back to front
    auto const p3 = cache.try_get(1);
    REQUIRE(p3 != nullptr);
    REQUIRE(*p3 == "1");
    REQUIRE(joinHumanReadable(cache.keys()) == "1, 3, 4, 2");
}

TEST_CASE("StrongLRUCache.get_or_emplace", "[lrucache]")
{
    auto cachePtr = StrongLRUCache<int, int>::create(StrongHashCapacity { 4 }, StrongCacheCapacity { 2 });
    auto& cache = *cachePtr;

    int& a = cache.get_or_emplace(2, []() { return 4; });
    CHECK(a == 4);
    CHECK(cache.at(2) == 4);
    CHECK(cache.size() == 1);
    CHECK(joinHumanReadable(cache.keys()) == "2"sv);

    int& a2 = cache.get_or_emplace(2, []() { return -4; });
    CHECK(a2 == 4);
    CHECK(cache.at(2) == 4);
    CHECK(cache.size() == 1);

    int& b = cache.get_or_emplace(3, []() { return 6; });
    CHECK(b == 6);
    CHECK(cache.at(3) == 6);
    CHECK(cache.size() == 2);
    CHECK(joinHumanReadable(cache.keys()) == "3, 2"sv);

    int& c = cache.get_or_emplace(4, []() { return 8; });
    CHECK(joinHumanReadable(cache.keys()) == "4, 3"sv);
    CHECK(c == 8);
    CHECK(cache.at(4) == 8);
    CHECK(cache.size() == 2);
    CHECK(cache.contains(3));
    CHECK_FALSE(cache.contains(2)); // thrown out

    int& b2 = cache.get_or_emplace(3, []() { return -3; });
    CHECK(joinHumanReadable(cache.keys()) == "3, 4"sv);
    CHECK(b2 == 6);
    CHECK(cache.at(3) == 6);
    CHECK(cache.size() == 2);
}

TEST_CASE("StrongLRUCache.erase", "")
{
    auto cachePtr = StrongLRUCache<int, string>::create(StrongHashCapacity { 8 }, StrongCacheCapacity { 4 });
    auto& cache = *cachePtr;
    for (int i = 1; i <= 4; ++i)
        cache[i] = std::to_string(i);
    REQUIRE(joinHumanReadable(cache.keys()) == "4, 3, 2, 1");

    // erase at head
    cache.erase(4);
    REQUIRE(joinHumanReadable(cache.keys()) == "3, 2, 1");

    // erase in middle
    cache.erase(2);
    REQUIRE(joinHumanReadable(cache.keys()) == "3, 1");

    // erase at tail
    cache.erase(1);
    REQUIRE(joinHumanReadable(cache.keys()) == "3");

    // erase last
    cache.erase(3);
    REQUIRE(joinHumanReadable(cache.keys()) == "");
}

// clang-format off
struct CollidingHasher
{
    StrongHash operator()(int v) noexcept
    {
        // Since the hashtable lookup only looks at the
        // least significant 32 bit, this will always cause
        // a hash-table entry collision.
        return StrongHash { _mm_set_epi32(0, 0, v, 0) };
    }
};
// clang-format on

TEST_CASE("StrongLRUCache.insert_with_cache_collision", "")
{
    auto cachePtr = StrongLRUCache<int, int, CollidingHasher>::create(StrongHashCapacity { 8 },
                                                                      StrongCacheCapacity { 4 });
    auto& cache = *cachePtr;

    cache[1] = 1;
    REQUIRE(joinHumanReadable(cache.keys()) == "1");

    cache[2] = 2;
    REQUIRE(joinHumanReadable(cache.keys()) == "2, 1");

    cache[3] = 3;
    REQUIRE(joinHumanReadable(cache.keys()) == "3, 2, 1");

    cache[4] = 4;
    REQUIRE(joinHumanReadable(cache.keys()) == "4, 3, 2, 1");
}

TEST_CASE("StrongLRUCache.erase_with_hashTable_lookup_collision", "")
{
    auto cachePtr = StrongLRUCache<int, int, CollidingHasher>::create(StrongHashCapacity { 8 },
                                                                      StrongCacheCapacity { 4 });
    auto& cache = *cachePtr;
    for (int i = 1; i <= 4; ++i)
        cache[i] = 2 * i;
    REQUIRE(joinHumanReadable(cache.keys()) == "4, 3, 2, 1");

    // erase at head
    cache.erase(4);
    REQUIRE(joinHumanReadable(cache.keys()) == "3, 2, 1");

    // erase in middle
    cache.erase(2);
    REQUIRE(joinHumanReadable(cache.keys()) == "3, 1");

    // erase at tail
    cache.erase(1);
    REQUIRE(joinHumanReadable(cache.keys()) == "3");

    // erase last
    cache.erase(3);
    REQUIRE(joinHumanReadable(cache.keys()) == "");
}
