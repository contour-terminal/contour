// SPDX-License-Identifier: Apache-2.0
#include <core/Times.hpp>

#include <catch2/catch_test_macros.hpp>

#include <cstddef>
#include <cstdint>
#include <ranges>
#include <tuple>
#include <type_traits>

TEST_CASE("times.count-simple")
{
    using namespace core;
    std::string s;
    times(5) | [&]() {
        s += 'A';
    };
    REQUIRE(s == "AAAAA");
}

TEST_CASE("times.count")
{
    using namespace core;
    std::string s;
    times(5) | [&](auto i) {
        s += std::to_string(i);
    };
    REQUIRE(s == "01234");
}

TEST_CASE("times.start_count")
{
    using namespace core;
    std::string s;
    times(5, 2) | [&](auto i) {
        s += std::to_string(i);
    };
    REQUIRE(s == "56");
}

TEST_CASE("times.start_count_step")
{
    using namespace core;
    std::string s;
    times(5, 3, 2) | [&](auto i) {
        s += std::to_string(i);
    };
    REQUIRE(s == "579");
}

TEST_CASE("times.iterator.post_increment_answers_the_prior_position")
{
    auto it = core::times(5).begin();
    auto const prior = it++;
    CHECK(*prior == 0);
    CHECK(*it == 1);
}

TEST_CASE("times.iterator.post_decrement_steps_back_and_answers_the_prior_position")
{
    auto it = core::times(5).begin();
    ++it;
    ++it;
    auto const prior = it--;
    CHECK(*prior == 2);
    CHECK(*it == 1);
}

TEST_CASE("times2D.iterator.post_increment_answers_the_prior_position")
{
    auto const grid = core::times(2) * core::times(3);
    auto it = grid.begin();
    auto const prior = it++;
    CHECK(*prior == std::tuple { 0, 0 });
    CHECK(*it == std::tuple { 0, 1 });
}

// Subscripting and iterating must agree on what an element is: operator[] answered the inner
// coordinate alone although value_type is a tuple of both.
TEST_CASE("times2D.subscript_answers_the_same_element_iteration_does")
{
    auto const grid = core::times(2) * core::times(3);

    REQUIRE(grid.size() == 6);
    STATIC_CHECK(std::is_same_v<decltype(grid)::value_type, std::tuple<int, int>>);
    STATIC_CHECK(std::is_same_v<decltype(grid[0]), decltype(*grid.begin())>);

    auto it = grid.begin();
    for (auto const i: std::views::iota(std::size_t { 0 }, grid.size()))
    {
        INFO("element " << i);
        CHECK(grid[i] == *it);
        ++it;
    }

    // The inner range advances fastest, so the outer coordinate changes every three steps.
    CHECK(grid[0] == std::tuple { 0, 0 });
    CHECK(grid[2] == std::tuple { 0, 2 });
    CHECK(grid[3] == std::tuple { 1, 0 });
    CHECK(grid[5] == std::tuple { 1, 2 });
}

// Times::size() and Times::operator[] had never been instantiated by anything in the repository,
// so the conversions their arithmetic implies had never been compiled -- and they did not, under
// -Wsign-conversion and -Wshorten-64-to-32. These cases instantiate both directly, for every
// shape of times() there is, so an untested template cannot rot back into that state.
TEST_CASE("times.size_and_subscript")
{
    SECTION("the count-only form is 0 to count - 1")
    {
        auto const range = core::times(5);
        CHECK(range.size() == 5);
        CHECK(range[0] == 0);
        CHECK(range[4] == 4);
    }

    SECTION("the start/count/step form steps as it is told")
    {
        auto const range = core::times(10, 4, 3);
        CHECK(range.size() == 4);
        CHECK(range[0] == 10);
        CHECK(range[1] == 13);
        CHECK(range[3] == 19);
    }

    SECTION("a negative step counts down")
    {
        auto const range = core::times(10, 3, -2);
        CHECK(range.size() == 3);
        CHECK(range[0] == 10);
        CHECK(range[2] == 6);
    }

    SECTION("subscripting answers what iterating answers, at every position")
    {
        auto const range = core::times(7, 5, 2);
        auto it = range.begin();
        for (auto const i: std::views::iota(std::size_t { 0 }, range.size()))
        {
            INFO("element " << i);
            CHECK(range[i] == *it);
            ++it;
        }
    }

    SECTION("an unsigned value type is instantiated too")
    {
        // The conversions differ by value type, so the narrow and unsigned instantiations are
        // compiled here rather than left to a consumer to discover.
        auto const range = core::times(std::size_t { 3 });
        CHECK(range.size() == 3);
        CHECK(range[2] == std::size_t { 2 });

        auto const small = core::times(std::uint8_t { 1 }, std::uint8_t { 3 }, std::uint8_t { 2 });
        CHECK(small.size() == 3);
        CHECK(small[2] == std::uint8_t { 5 });
    }

    SECTION("it is all available at compile time")
    {
        STATIC_CHECK(core::times(5).size() == 5);
        STATIC_CHECK(core::times(10, 4, 3)[3] == 19);
    }
}
