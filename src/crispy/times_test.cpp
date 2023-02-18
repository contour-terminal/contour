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
