// SPDX-License-Identifier: Apache-2.0
//
// The font families the Settings window's picker lists.

#include <contour/platform/FontFamilySource.hpp>

#include <QtGui/QFontDatabase>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <ranges>
#include <string>

using namespace contour::platform;

TEST_CASE("fonts: the Qt source lists the bundled family first and once, then fixed-pitch families only",
          "[platform][fonts]")
{
    auto const families = QtFontFamilySource {}.monospaceFamilies();
    REQUIRE_FALSE(families.empty());
    CHECK(families.front() == BundledMonospaceFamily);
    CHECK(std::ranges::count(families, std::string { BundledMonospaceFamily }) == 1);

    // The bundled family is the application's own and may not be in Qt's database; every other entry is.
    for (auto const& family: families | std::views::drop(1))
    {
        INFO("family: " << family);
        CHECK(QFontDatabase::isFixedPitch(QString::fromStdString(family)));
    }

    auto const rest = families | std::views::drop(1);
    CHECK(std::ranges::is_sorted(rest));
    CHECK(std::ranges::adjacent_find(rest) == rest.end());
}

TEST_CASE("fonts: the null source offers the bundled family alone", "[platform][fonts]")
{
    auto const families = NullFontFamilySource {}.monospaceFamilies();
    REQUIRE(families.size() == 1);
    CHECK(families.front() == BundledMonospaceFamily);
}
