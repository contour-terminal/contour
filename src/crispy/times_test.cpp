// SPDX-License-Identifier: Apache-2.0
#include <crispy/times.h>

#include <catch2/catch.hpp>

TEST_CASE("times.count-simple")
{
    using namespace crispy;
    std::string s;
    times(5) | [&]() {
        s += 'A';
    };
    REQUIRE(s == "AAAAA");
}

TEST_CASE("times.count")
{
    using namespace crispy;
    std::string s;
    times(5) | [&](auto i) {
        s += std::to_string(i);
    };
    REQUIRE(s == "01234");
}

TEST_CASE("times.start_count")
{
    using namespace crispy;
    std::string s;
    times(5, 2) | [&](auto i) {
        s += std::to_string(i);
    };
    REQUIRE(s == "56");
}

TEST_CASE("times.start_count_step")
{
    using namespace crispy;
    std::string s;
    times(5, 3, 2) | [&](auto i) {
        s += std::to_string(i);
    };
    REQUIRE(s == "579");
}
