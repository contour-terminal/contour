// This file is part of the "klex" project, http://github.com/christianparpart/klex>
//	 (c) 2018 Christian Parpart <christian@parpart.family>
//
// Licensed under the MIT License (the "License"); you may not use this
// file except in compliance with the License. You may obtain a copy of
// the License at: http://opensource.org/licenses/MIT

#include <regex_dfa/util/iterator.h>

#include <fmt/format.h>

#include <catch2/catch.hpp>

#include <array>
#include <string>
#include <type_traits>
#include <vector>

using namespace std;
using namespace regex_dfa::util;

TEST_CASE("util_iterator_reversed.empty")
{
    const vector<int> v;
    auto x = reversed(v);
    auto i = begin(x);
    REQUIRE(i == end(x));
}

TEST_CASE("util_iterator_reversed.one")
{
    const vector<int> v { 1 };
    auto x = reversed(v);
    auto i = begin(x);
    REQUIRE(1 == *i);
    i++;
    REQUIRE(i == end(x));
}

TEST_CASE("util_iterator_reversed.many")
{
    const vector<int> v { 1, 2, 3 };
    auto x = reversed(v);
    auto i = begin(x);
    REQUIRE(3 == *i);
    i++;
    REQUIRE(2 == *i);
    i++;
    REQUIRE(1 == *i);
    i++;
    REQUIRE(i == end(x));
}

TEST_CASE("util_iterator_indexed.many_const")
{
    const vector<int> v { 10, 20, 30 };
    const auto x = indexed(v);
    static_assert(is_const<decltype(x)>::value);
    auto i = begin(x);

    REQUIRE(0 == (*i).first);
    REQUIRE(10 == (*i).second);
    i++;

    REQUIRE(1 == (*i).first);
    REQUIRE(20 == (*i).second);
    i++;

    REQUIRE(2 == (*i).first);
    REQUIRE(30 == (*i).second);
    i++;

    REQUIRE(i == end(x));
}

TEST_CASE("util_iterator_indexed.many")
{
    vector<string> v { "zero", "one", "two" };
    auto x = indexed(v);
    auto i = begin(x);

    REQUIRE(0 == (*i).first);
    REQUIRE("zero" == (*i).second);
    i++;

    REQUIRE(1 == (*i).first);
    REQUIRE("one" == (*i).second);
    i++;

    REQUIRE(2 == (*i).first);
    REQUIRE("two" == (*i).second);
    i++;

    REQUIRE(i == end(x));
}

TEST_CASE("util_iterator_indexed.range_based_for_loop")
{
    INFO("const:");
    const vector<int> v1 { 10, 20, 30 };
    for (const auto&& [index, value]: indexed(v1))
        INFO(fmt::format("index {}, value {}", index, value));

    INFO("non-const:");
    vector<int> v2 { 10, 20, 30 };
    for (const auto&& [index, value]: indexed(v2))
        INFO(fmt::format("index {}, value {}", index, value));
}

TEST_CASE("util_iterator_filter.for_range")
{
    const vector<int> nums = { 1, 2, 3, 4 };
    vector<int> odds;
    for (const int i: filter(nums, [](int x) { return x % 2 != 0; }))
        odds.push_back(i);

    REQUIRE(2 == odds.size());
    REQUIRE(1 == odds[0]);
    CHECK(3 == odds[1]);
}

TEST_CASE("util_iterator_filter.count_proc_invocations")
{
    static const array<int, 4> numbers = { 1, 2, 3, 4 };
    int count = 0;
    auto counter = [&](int) {
        ++count;
        return true;
    };
    const auto f = filter(numbers, counter);
    for_each(begin(f), end(f), [](int) {});
    REQUIRE(4 == count);
}

TEST_CASE("util_iterator_filter.for_range_initializer_list")
{
    static const array<int, 4> numbers = { 1, 2, 3, 4 };
    vector<int> odds;
    auto f_odd = [&](int x) {
        INFO(fmt::format("f_odd: x={0}", x));
        return x % 2 != 0;
    };
    for (const int i: filter(numbers, f_odd))
        odds.push_back(i);

    REQUIRE(2 == odds.size());
    CHECK(1 == odds[0]);
    CHECK(3 == odds[1]);
}

TEST_CASE("util_iterator_translate.vector")
{
    const vector<int> in { 1, 2, 3, 4 };
    const vector<int> out = translate(in, [](auto i) -> int { return int(i * 2); });

    for (const auto&& [i, v]: indexed(out))
        INFO(fmt::format("out[{}] = {}", i, v));

    REQUIRE(4 == out.size());

    CHECK(2 == out[0]);
    CHECK(4 == out[1]);
    CHECK(6 == out[2]);
    CHECK(8 == out[3]);
}

TEST_CASE("util_iterator_translate.chain_translate_join")
{
    const vector<int> in { 1, 2, 3, 4 };
    const string out { join(translate(in, [](int i) -> string { return to_string(i); }), ", ") };

    REQUIRE("1, 2, 3, 4" == out);
}

TEST_CASE("util_iterator.find_last")
{
    const vector<int> v { 1, 2, 3, 4 };
    const auto i = find_last(v, [](int i) { return i % 2 != 0; }); // find last odd value -> 3

    REQUIRE(i != end(v));
    REQUIRE(3 == *i);
}
