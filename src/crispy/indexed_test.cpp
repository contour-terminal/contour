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
#include <crispy/indexed.h>

#include <fmt/format.h>

#include <catch2/catch.hpp>

#include <array>
#include <vector>

using namespace crispy;

using std::array;
using std::vector;

namespace
{
vector<char> getVec()
{
    vector<char> v;
    v.push_back('a');
    v.push_back('b');
    v.push_back('c');
    return v;
}
} // namespace

TEST_CASE("indexed.basic", "[indexed]")
{
    auto const rng = crispy::indexed(array { 'a', 'b', 'c' });
    auto i = rng.begin();
    REQUIRE(i.index == 0);
    REQUIRE(*i.iter == 'a');

    ++i;
    REQUIRE(i.index == 1);
    REQUIRE(*i.iter == 'b');

    ++i;
    REQUIRE(i.index == 2);
    REQUIRE(*i.iter == 'c');

    ++i;
    REQUIRE(i == rng.end());
}

TEST_CASE("indexed.for_loop_basic_lvalue", "[indexed]")
{
    size_t k = 0;
    auto const a = array { 'a', 'b', 'c' };
    for (auto const&& [i, c]: crispy::indexed(a))
    {
        REQUIRE(i == k);
        switch (k)
        {
        case 0: REQUIRE(c == 'a'); break;
        case 1: REQUIRE(c == 'b'); break;
        case 2: REQUIRE(c == 'c'); break;
        default: REQUIRE(false);
        }
        ++k;
    }
}

TEST_CASE("indexed.for_loop_basic_rvalue", "[indexed]")
{
    size_t k = 0;
    for (auto const&& [i, c]: crispy::indexed(vector { 'a', 'b', 'c' }))
    {
        REQUIRE(i == k);
        switch (k)
        {
        case 0: REQUIRE(c == 'a'); break;
        case 1: REQUIRE(c == 'b'); break;
        case 2: REQUIRE(c == 'c'); break;
        default: REQUIRE(false);
        }
        ++k;
    }
}

TEST_CASE("indexed.for_loop_basic_rvalue_via_call", "[indexed]")
{
    size_t k = 0;
    for (auto const&& [i, c]: crispy::indexed(getVec()))
    {
        REQUIRE(i == k);
        switch (k)
        {
        case 0: REQUIRE(c == 'a'); break;
        case 1: REQUIRE(c == 'b'); break;
        case 2: REQUIRE(c == 'c'); break;
        default: REQUIRE(false);
        }
        ++k;
    }
}
