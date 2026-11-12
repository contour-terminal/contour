// SPDX-License-Identifier: Apache-2.0
#include <vtbackend/core/Primitives.hpp>
#include <vtbackend/screen/TabStops.hpp>

#include <catch2/catch_test_macros.hpp>

#include <initializer_list>
#include <optional>
#include <vector>

using namespace vtbackend;

namespace
{
std::vector<ColumnOffset> columns(std::initializer_list<int> values)
{
    auto result = std::vector<ColumnOffset> {};
    for (auto const value: values)
        result.emplace_back(value);
    return result;
}
} // namespace

TEST_CASE("TabStops.initial", "[tabstops]")
{
    auto const stops = TabStops {};
    CHECK(stops.isInitial());
    CHECK(stops.within(ColumnCount(20)) == columns({ 0, 8, 16 }));
    CHECK(stops.within(ColumnCount(16)) == columns({ 0, 8 }));
    CHECK(stops.within(ColumnCount(0)).empty());
    CHECK(stops.nextAfter(ColumnOffset(0)) == ColumnOffset(8));
    CHECK(stops.nextAfter(ColumnOffset(7)) == ColumnOffset(8));
    CHECK(stops.nextAfter(ColumnOffset(8)) == ColumnOffset(16));
    CHECK(stops.previousBefore(ColumnOffset(0)) == std::nullopt);
    CHECK(stops.previousBefore(ColumnOffset(8)) == ColumnOffset(0));
    CHECK(stops.previousBefore(ColumnOffset(9)) == ColumnOffset(8));
}

TEST_CASE("TabStops.set", "[tabstops]")
{
    auto stops = TabStops {};
    stops.set(ColumnOffset(3));
    stops.set(ColumnOffset(3));
    CHECK_FALSE(stops.isInitial());
    CHECK(stops.within(ColumnCount(20)) == columns({ 0, 3, 8, 16 }));
    CHECK(stops.nextAfter(ColumnOffset(0)) == ColumnOffset(3));
    CHECK(stops.nextAfter(ColumnOffset(3)) == ColumnOffset(8));
    CHECK(stops.previousBefore(ColumnOffset(8)) == ColumnOffset(3));
    CHECK(stops.previousBefore(ColumnOffset(3)) == ColumnOffset(0));

    // A stop set between two default ones, far right of anything written out so far.
    stops.set(ColumnOffset(20));
    CHECK(stops.within(ColumnCount(30)) == columns({ 0, 3, 8, 16, 20, 24 }));
    CHECK(stops.previousBefore(ColumnOffset(24)) == ColumnOffset(20));
    CHECK(stops.previousBefore(ColumnOffset(100)) == ColumnOffset(96));
}

TEST_CASE("TabStops.clear", "[tabstops]")
{
    auto stops = TabStops {};
    stops.clear(ColumnOffset(8));
    CHECK(stops.within(ColumnCount(30)) == columns({ 0, 16, 24 }));
    CHECK(stops.nextAfter(ColumnOffset(0)) == ColumnOffset(16));
    CHECK(stops.previousBefore(ColumnOffset(16)) == ColumnOffset(0));

    // Clearing where there is none changes nothing that can be seen.
    stops.clear(ColumnOffset(29));
    CHECK(stops.within(ColumnCount(40)) == columns({ 0, 16, 24, 32 }));

    stops.set(ColumnOffset(5));
    stops.clear(ColumnOffset(5));
    CHECK(stops.within(ColumnCount(40)) == columns({ 0, 16, 24, 32 }));
}

TEST_CASE("TabStops.clearAll", "[tabstops]")
{
    auto stops = TabStops {};
    stops.clearAll();
    CHECK_FALSE(stops.isInitial());
    CHECK(stops.within(ColumnCount(200)).empty());
    CHECK(stops.nextAfter(ColumnOffset(0)) == std::nullopt);
    CHECK(stops.previousBefore(ColumnOffset(100)) == std::nullopt);

    stops.set(ColumnOffset(12));
    CHECK(stops.within(ColumnCount(200)) == columns({ 12 }));
    CHECK(stops.nextAfter(ColumnOffset(12)) == std::nullopt);

    stops.reset();
    CHECK(stops.isInitial());
    CHECK(stops == TabStops {});
}

TEST_CASE("TabStops.only", "[tabstops]")
{
    auto const stops = TabStops::only(columns({ 4, 9 }));
    CHECK(stops.within(ColumnCount(100)) == columns({ 4, 9 }));
    CHECK(stops.nextAfter(ColumnOffset(9)) == std::nullopt);
    CHECK(stops.previousBefore(ColumnOffset(9)) == ColumnOffset(4));
}
