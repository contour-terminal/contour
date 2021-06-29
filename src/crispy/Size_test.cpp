/**
 * This file is part of the "libterminal" project
 *   Copyright (c) 2019-2020 Christian Parpart <christian@parpart.family>
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
#include <terminal/Size.h>
#include <catch2/catch_all.hpp>
#include <string_view>
#include <array>

using terminal::Size;
using terminal::Coordinate;

TEST_CASE("Size.iterator", "")
{
    Size const s { 3, 2 };
    Size::iterator i = s.begin();
    Size::iterator e = s.end();
    REQUIRE(i != e);

    REQUIRE(*i == Coordinate{0, 0});

    ++i;
    REQUIRE(*i == Coordinate{0, 1});

    ++i;
    REQUIRE(*i == Coordinate{0, 2});

    ++i;
    REQUIRE(*i == Coordinate{1, 0});

    ++i;
    REQUIRE(*i == Coordinate{1, 1});

    ++i;
    REQUIRE(*i == Coordinate{1, 2});

    ++i;
    REQUIRE(i == e);
}
