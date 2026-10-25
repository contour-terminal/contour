// SPDX-License-Identifier: Apache-2.0
#include <contour/window/TabWellLayout.hpp>

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <ranges>

using namespace contour::window;
using Catch::Matchers::WithinAbs;

namespace
{

// The macOS row's metrics: min/max tab width, 2·s5 + size.tabIcon, and s1 for the gap and the padding.
constexpr auto Metrics =
    TabWellMetrics { .minTab = 120, .maxTab = 240, .iconOnlyTab = 34, .gap = 2, .padding = 2 };

} // namespace

TEST_CASE("tabWell.widths", "[tabwell]")
{
    CHECK(layoutTabWell(1000, 3, Metrics)
          == TabWellLayout { 240, 728, TabLabelMode::Full, TabWellOverflow::Fits });
    CHECK(layoutTabWell(600, 4, Metrics)
          == TabWellLayout { 147.5, 600, TabLabelMode::Full, TabWellOverflow::Fits });

    auto const iconOnly = layoutTabWell(600, 6, Metrics);
    CHECK_THAT(iconOnly.tabWidth, WithinAbs((600.0 - 4 - 10) / 6, 1e-9));
    CHECK_THAT(iconOnly.wellWidth, WithinAbs(600, 1e-9));
    CHECK(iconOnly.label == TabLabelMode::IconOnly);
    CHECK(iconOnly.overflow == TabWellOverflow::Fits);

    CHECK(layoutTabWell(300, 12, Metrics)
          == TabWellLayout { 34, 300, TabLabelMode::IconOnly, TabWellOverflow::Scrolls });
    CHECK(layoutTabWell(-50, 2, Metrics)
          == TabWellLayout { 34, 0, TabLabelMode::IconOnly, TabWellOverflow::Scrolls });
    CHECK(layoutTabWell(500, 0, Metrics) == TabWellLayout {});
    static_assert(layoutTabWell(1000, 3, Metrics).tabWidth == 240);
}

TEST_CASE("tabWell.neverPastTheRange", "[tabwell]")
{
    constexpr auto Widths = std::array { 0.0, 120.0, 480.0, 900.0, 1600.0, 4000.0 };
    for (auto const n: std::views::iota(1, 41))
        for (auto const available: Widths)
        {
            INFO("n " << n << ", available " << available);
            auto const layout = layoutTabWell(available, n, Metrics);
            CHECK(layout.tabWidth <= Metrics.maxTab);
            CHECK(layout.wellWidth <= std::max(0.0, available));
            if (layout.label == TabLabelMode::Full)
                CHECK(layout.tabWidth >= Metrics.minTab);
        }
}

TEST_CASE("tabWell.overflow", "[tabwell]")
{
    // The drag reserve is the canvas's "at least 40 pt of empty bar always stays draggable" (mac-dark);
    // a literal until Task 4 lands ChromeTokenRows' space.dragReserve.
    constexpr auto DragReserve = 40.0;
    auto const twenty = layoutTabWell(900 - DragReserve, 20, Metrics);
    CHECK(twenty.label == TabLabelMode::IconOnly);
    CHECK(twenty.overflow == TabWellOverflow::Fits);
    CHECK(layoutTabWell(900 - DragReserve, 30, Metrics).overflow == TabWellOverflow::Scrolls);
}

TEST_CASE("tabWell.names", "[tabwell]")
{
    CHECK(tabLabelModeName(TabLabelMode::Full) == "full");
    CHECK(tabLabelModeName(TabLabelMode::IconOnly) == "iconOnly");
}
