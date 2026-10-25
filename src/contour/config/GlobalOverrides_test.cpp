// SPDX-License-Identifier: Apache-2.0
#include <contour/config/GlobalOverrides.hpp>

#include <catch2/catch_test_macros.hpp>

#include <set>
#include <string_view>

using contour::config::globalOverrideFor;
using contour::config::globalOverrides;

TEST_CASE("GlobalOverrides: a scalar row covers its own key alone", "[config]")
{
    auto const* row = globalOverrideFor("reflow_on_resize");
    REQUIRE(row != nullptr);
    CHECK(row->key == "reflow_on_resize");
    CHECK_FALSE(row->covers("reflow_on_resize.wrap"));
    CHECK(globalOverrideFor("reflow_on_resiz") == nullptr);
}

TEST_CASE("GlobalOverrides: chrome_density is a scalar row covering its own key alone", "[config]")
{
    auto const* row = globalOverrideFor("chrome_density");
    REQUIRE(row != nullptr);
    CHECK(row->key == "chrome_density");
    CHECK_FALSE(row->covers("chrome_density.x"));
}

TEST_CASE("GlobalOverrides: a section row covers each of its leaves, not the section itself", "[config]")
{
    auto const* row = globalOverrideFor("gutter.line_numbers");
    REQUIRE(row != nullptr);
    CHECK(row->key == "gutter");
    CHECK(globalOverrideFor("gutter.timestamps") == row);

    // A hand-edited scalar where the section belongs is no leaf of it, and neither is a key that merely
    // begins with the section's name.
    CHECK(globalOverrideFor("gutter") == nullptr);
    CHECK(globalOverrideFor("gutters.line_numbers") == nullptr);
}

TEST_CASE("GlobalOverrides: no two rows share a key", "[config]")
{
    auto keys = std::set<std::string_view> {};
    for (auto const& row: globalOverrides())
    {
        INFO(row.key);
        CHECK(keys.insert(row.key).second);
    }
}
