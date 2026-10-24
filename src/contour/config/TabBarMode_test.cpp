// SPDX-License-Identifier: Apache-2.0
#include <contour/config/TabBarMode.hpp>

#include <catch2/catch_test_macros.hpp>

using contour::config::isTabBarShown;
using contour::config::TabBarVisibility;

TEST_CASE("TabBarMode.isTabBarShown", "[tabbar]")
{
    CHECK(isTabBarShown(TabBarVisibility::Always, 1));
    CHECK(isTabBarShown(TabBarVisibility::Always, 2));
    CHECK_FALSE(isTabBarShown(TabBarVisibility::Never, 1));
    CHECK_FALSE(isTabBarShown(TabBarVisibility::Never, 2));
    CHECK_FALSE(isTabBarShown(TabBarVisibility::Multiple, 1));
    CHECK(isTabBarShown(TabBarVisibility::Multiple, 2));
}
