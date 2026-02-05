// SPDX-License-Identifier: Apache-2.0
#include <vtbackend/Size.h>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <string_view>

using terminal::Coordinate;
using terminal::Size;

TEST_CASE("Size.iterator", "")
{
    Size const s { 3, 2 };
    Size::iterator i = s.begin();
    Size::iterator e = s.end();
    REQUIRE(i != e);

    REQUIRE(*i == Coordinate { 0, 0 });

    ++i;
    REQUIRE(*i == Coordinate { 0, 1 });

    ++i;
    REQUIRE(*i == Coordinate { 0, 2 });

    ++i;
    REQUIRE(*i == Coordinate { 1, 0 });

    ++i;
    REQUIRE(*i == Coordinate { 1, 1 });

    ++i;
    REQUIRE(*i == Coordinate { 1, 2 });

    ++i;
    REQUIRE(i == e);
}
