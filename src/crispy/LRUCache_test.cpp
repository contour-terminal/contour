// SPDX-License-Identifier: Apache-2.0
#include <crispy/LRUCache.h>

#include <fmt/format.h>

#include <catch2/catch_test_macros.hpp>

#include <functional>
#include <iostream>

using namespace std;
using namespace std::string_view_literals;

// template <typename A, typename B>
// void dump_it(crispy::LRUCache<A, B> const& cache, std::string_view header)
// {
//     std::ostream& out = std::cout;
//
//     out << fmt::format("lru_cache({}/{}): {}\n", cache.size(), cache.capacity(), header);
//     for (typename crispy::lru_cache<A, B>::Item const& item: cache)
//     {
//         out << fmt::format("{}: {}\n", item.key, item.value);
//     }
//     out << "\n";
// }

template <typename T>
static std::string join(std::vector<T> const& list, std::string_view delimiter = " "sv)
{
    std::string s;
    for (size_t i = 0; i < list.size(); ++i)
    {
        if (i)
            s += delimiter;
        s += fmt::format("{}", list[i]);
    }
    return s;
}

TEST_CASE("lru_cache.ctor", "[lrucache]")
{
    auto cache = crispy::lru_cache<int, int>(4);
    CHECK(cache.size() == 0);
    CHECK(cache.capacity() == 4);
}

TEST_CASE("lru_cache.at", "[lrucache]")
{
    auto cache = crispy::lru_cache<int, int>(2);

    CHECK_THROWS_AS(cache.at(2), std::out_of_range);
    cache[2] = 4;
    CHECK_NOTHROW(cache.at(2));
}

TEST_CASE("lru_cache.get_or_emplace", "[lrucache]")
{
    auto cache = crispy::lru_cache<int, int>(2);

    int& a = cache.get_or_emplace(2, []() { return 4; });
    CHECK(a == 4);
    CHECK(cache.at(2) == 4);
    CHECK(cache.size() == 1);
    CHECK(join(cache.keys()) == "2"sv);

    int& a2 = cache.get_or_emplace(2, []() { return -4; });
    CHECK(a2 == 4);
    CHECK(cache.at(2) == 4);
    CHECK(cache.size() == 1);

    int& b = cache.get_or_emplace(3, []() { return 6; });
    CHECK(b == 6);
    CHECK(cache.at(3) == 6);
    CHECK(cache.size() == 2);
    CHECK(join(cache.keys()) == "3 2"sv);

    int& c = cache.get_or_emplace(4, []() { return 8; });
    CHECK(join(cache.keys()) == "4 3"sv);
    CHECK(c == 8);
    CHECK(cache.at(4) == 8);
    CHECK(cache.size() == 2);
    CHECK(cache.contains(3));
    CHECK_FALSE(cache.contains(2)); // thrown out

    int& b2 = cache.get_or_emplace(3, []() { return -3; });
    CHECK(join(cache.keys()) == "3 4"sv);
    CHECK(b2 == 6);
    CHECK(cache.at(3) == 6);
    CHECK(cache.size() == 2);
}

TEST_CASE("lru_cache.operator[]", "[lrucache]")
{
    auto cache = crispy::lru_cache<int, int>(2);

    (void) cache[2];
    CHECK(join(cache.keys()) == "2"sv);
    CHECK(cache[2] == 0);
    cache[2] = 4;
    CHECK(cache[2] == 4);
    CHECK(cache.size() == 1);

    cache[3] = 6;
    CHECK(join(cache.keys()) == "3 2"sv);
    CHECK(cache[3] == 6);
    CHECK(cache.size() == 2);

    cache[4] = 8;
    CHECK(join(cache.keys()) == "4 3"sv);
    CHECK(cache[4] == 8);
    CHECK(cache.size() == 2);
    CHECK(cache.contains(3));
    CHECK_FALSE(cache.contains(2)); // thrown out

    (void) cache[3]; // move 3 to the front (currently at the back)
    CHECK(join(cache.keys()) == "3 4"sv);
    cache[5] = 10;
    CHECK(join(cache.keys()) == "5 3"sv);
    CHECK(cache.at(5) == 10);
    CHECK(cache.size() == 2);
    CHECK(cache.contains(5));
    CHECK(cache.contains(3));
    CHECK_FALSE(cache.contains(4)); // thrown out
}

TEST_CASE("lru_cache.clear", "[lrucache]")
{
    auto cache = crispy::lru_cache<int, int>(4);
    cache[2] = 4;
    cache[3] = 6;
    CHECK(cache.size() == 2);
    cache.clear();
    CHECK(cache.size() == 0);
}

TEST_CASE("lru_cache.try_emplace", "[lrucache]")
{
    auto cache = crispy::lru_cache<int, int>(2);
    auto rv = cache.try_emplace(2, []() { return 4; });
    CHECK(rv);
    CHECK(join(cache.keys()) == "2");
    CHECK(cache.at(2) == 4);

    rv = cache.try_emplace(3, []() { return 6; });
    CHECK(rv);
    CHECK(join(cache.keys()) == "3 2");
    CHECK(cache.at(2) == 4);
    CHECK(cache.at(3) == 6);

    rv = cache.try_emplace(2, []() { return -1; });
    CHECK_FALSE(rv);
    CHECK(join(cache.keys()) == "2 3");
    CHECK(cache.at(2) == 4);
    CHECK(cache.at(3) == 6);
}
