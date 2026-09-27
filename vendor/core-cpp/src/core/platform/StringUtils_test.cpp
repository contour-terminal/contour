// SPDX-License-Identifier: Apache-2.0
#include <core/platform/StringUtils.hpp>

#include <catch2/catch_test_macros.hpp>

#include <string>

using core::platform::trimInPlace;

TEST_CASE("trimInPlace strips whitespace from both ends only", "[platform][string]")
{
    auto text = std::string { " \t\r\n  two words \n\t" };
    trimInPlace(text);
    CHECK(text == "two words");

    auto untouched = std::string { "none" };
    trimInPlace(untouched);
    CHECK(untouched == "none");
}

TEST_CASE("trimInPlace clears a string of whitespace alone", "[platform][string]")
{
    auto blank = std::string { " \t\r\n" };
    trimInPlace(blank);
    CHECK(blank.empty());

    auto empty = std::string {};
    trimInPlace(empty);
    CHECK(empty.empty());
}
