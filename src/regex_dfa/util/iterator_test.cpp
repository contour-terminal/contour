// This file is part of the "klex" project, http://github.com/christianparpart/klex>
//	 (c) 2018 Christian Parpart <christian@parpart.family>
//
// Licensed under the MIT License (the "License"); you may not use this
// file except in compliance with the License. You may obtain a copy of
// the License at: http://opensource.org/licenses/MIT

#include <klex/util/iterator.h>
#include <klex/util/testing.h>

#include <array>
#include <string>
#include <type_traits>
#include <vector>

using namespace std;
using namespace regex_dfa::util;

TEST(util_iterator_reversed, empty)
{
    const vector<int> v;
    auto x = reversed(v);
    auto i = begin(x);
    ASSERT_TRUE(i == end(x));
}

TEST(util_iterator_reversed, one)
{
    const vector<int> v { 1 };
    auto x = reversed(v);
    auto i = begin(x);
    ASSERT_EQ(1, *i);
    i++;
    ASSERT_TRUE(i == end(x));
}

TEST(util_iterator_reversed, many)
{
    const vector<int> v { 1, 2, 3 };
    auto x = reversed(v);
    auto i = begin(x);
    ASSERT_EQ(3, *i);
    i++;
    ASSERT_EQ(2, *i);
    i++;
    ASSERT_EQ(1, *i);
    i++;
    ASSERT_TRUE(i == end(x));
}

TEST(util_iterator_indexed, many_const)
{
    const vector<int> v { 10, 20, 30 };
    const auto x = indexed(v);
    static_assert(is_const<decltype(x)>::value);
    auto i = begin(x);

    ASSERT_EQ(0, (*i).first);
    ASSERT_EQ(10, (*i).second);
    i++;

    ASSERT_EQ(1, (*i).first);
    ASSERT_EQ(20, (*i).second);
    i++;

    ASSERT_EQ(2, (*i).first);
    ASSERT_EQ(30, (*i).second);
    i++;

    ASSERT_TRUE(i == end(x));
}

TEST(util_iterator_indexed, many)
{
    vector<string> v { "zero", "one", "two" };
    auto x = indexed(v);
    auto i = begin(x);

    ASSERT_EQ(0, (*i).first);
    ASSERT_EQ("zero", (*i).second);
    i++;

    ASSERT_EQ(1, (*i).first);
    ASSERT_EQ("one", (*i).second);
    i++;

    ASSERT_EQ(2, (*i).first);
    ASSERT_EQ("two", (*i).second);
    i++;

    ASSERT_TRUE(i == end(x));
}

TEST(util_iterator_indexed, range_based_for_loop)
{
    log("const:");
    const vector<int> v1 { 10, 20, 30 };
    for (const auto&& [index, value]: indexed(v1))
        logf("index {}, value {}", index, value);

    log("non-const:");
    vector<int> v2 { 10, 20, 30 };
    for (const auto&& [index, value]: indexed(v2))
        logf("index {}, value {}", index, value);
}

TEST(util_iterator_filter, for_range)
{
    const vector<int> nums = { 1, 2, 3, 4 };
    vector<int> odds;
    for (const int i: filter(nums, [](int x) { return x % 2 != 0; }))
        odds.push_back(i);

    ASSERT_EQ(2, odds.size());
    EXPECT_EQ(1, odds[0]);
    EXPECT_EQ(3, odds[1]);
}

TEST(util_iterator_filter, count_proc_invocations)
{
    static const array<int, 4> numbers = { 1, 2, 3, 4 };
    int count = 0;
    auto counter = [&](int) {
        ++count;
        return true;
    };
    const auto f = filter(numbers, counter);
    for_each(begin(f), end(f), [](int) {});
    ASSERT_EQ(4, count);
}

TEST(util_iterator_filter, for_range_initializer_list)
{
    static const array<int, 4> numbers = { 1, 2, 3, 4 };
    vector<int> odds;
    auto f_odd = [&](int x) {
        logf("f_odd: x={0}", x);
        return x % 2 != 0;
    };
    for (const int i: filter(numbers, f_odd))
        odds.push_back(i);

    ASSERT_EQ(2, odds.size());
    EXPECT_EQ(1, odds[0]);
    EXPECT_EQ(3, odds[1]);
}

TEST(util_iterator_translate, vector)
{
    const vector<int> in { 1, 2, 3, 4 };
    const vector<int> out = translate(in, [](auto i) -> int { return int(i * 2); });

    for (const auto&& [i, v]: indexed(out))
        logf("out[{}] = {}", i, v);

    ASSERT_EQ(4, out.size());

    EXPECT_EQ(2, out[0]);
    EXPECT_EQ(4, out[1]);
    EXPECT_EQ(6, out[2]);
    EXPECT_EQ(8, out[3]);
}

TEST(util_iterator_translate, chain_translate_join)
{
    const vector<int> in { 1, 2, 3, 4 };
    const string out { join(translate(in, [](int i) -> string { return to_string(i); }), ", ") };

    ASSERT_EQ("1, 2, 3, 4", out);
}

TEST(util_iterator, find_last)
{
    const vector<int> v { 1, 2, 3, 4 };
    const auto i = find_last(v, [](int i) { return i % 2 != 0; }); // find last odd value -> 3

    ASSERT_TRUE(i != end(v));
    ASSERT_EQ(3, *i);
}
