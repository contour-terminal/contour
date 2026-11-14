// SPDX-License-Identifier: Apache-2.0
#include <contour/config/ChromeTokenGroups.hpp>
#include <contour/config/ChromeTokenRows.hpp>
#include <contour/window/BandAllocation.hpp>
#include <contour/window/TabWellLayout.hpp>

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <ranges>
#include <string_view>
#include <utility>

using namespace contour::window;
using Catch::Matchers::WithinAbs;

namespace
{

constexpr auto const& MacRow =
    contour::config::ChromeTokenRows[static_cast<size_t>(contour::config::ChromeStyle::MacOS)];
constexpr auto const& MacStyle = contour::config::uiStyleTokens(contour::config::ChromeStyle::MacOS);

// The macOS row's metrics: min/max tab width, 2·s5 + size.tabIcon, and s1 for the gap and the padding.
constexpr auto Metrics = TabWellMetrics { .minTab = MacStyle.minTabUnits,
                                          .maxTab = MacStyle.maxTabUnits,
                                          .iconOnlyTab = (2 * MacRow.space.scale[4]) + MacRow.size.tabIcon,
                                          .gap = MacRow.space.scale[0],
                                          .padding = MacRow.space.scale[0] };
static_assert(Metrics.minTab == 120 && Metrics.maxTab == 240 && Metrics.iconOnlyTab == 34
              && Metrics.gap == 2);

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
    // The drag reserve is the canvas's "at least 40 pt of empty bar always stays draggable" (mac-dark).
    constexpr auto DragReserve = static_cast<double>(MacRow.space.dragReserve);
    static_assert(DragReserve == 40.0);
    auto const twenty = layoutTabWell(900 - DragReserve, 20, Metrics);
    CHECK(twenty.label == TabLabelMode::IconOnly);
    CHECK(twenty.overflow == TabWellOverflow::Fits);
    CHECK(layoutTabWell(900 - DragReserve, 30, Metrics).overflow == TabWellOverflow::Scrolls);
}

TEST_CASE("tabWell.minimums", "[tabwell]")
{
    // Literal rows: 2·2 padding + n·120 + (n-1)·2 labelled; 2·2 padding + one 34 pill icon-only.
    CHECK(labelledTabWellMinimum(0, Metrics) == 0);
    CHECK(labelledTabWellMinimum(1, Metrics) == 124);
    CHECK(labelledTabWellMinimum(3, Metrics) == 368);
    CHECK(labelledTabWellMinimum(10, Metrics) == 1222);
    CHECK(iconOnlyTabWellMinimum(0, Metrics) == 0);
    CHECK(iconOnlyTabWellMinimum(1, Metrics) == 38);
    CHECK(iconOnlyTabWellMinimum(30, Metrics) == 38);
    // The labelled minimum is the narrowest well that still shows every label.
    for (auto const n: std::views::iota(1, 21))
    {
        INFO("n " << n);
        CHECK(layoutTabWell(labelledTabWellMinimum(n, Metrics), n, Metrics).label == TabLabelMode::Full);
        CHECK(layoutTabWell(labelledTabWellMinimum(n, Metrics) - 1, n, Metrics).label
              == TabLabelMode::IconOnly);
    }
}

TEST_CASE("tabWell.names", "[tabwell]")
{
    CHECK(tabLabelModeName(TabLabelMode::Full) == "full");
    CHECK(tabLabelModeName(TabLabelMode::IconOnly) == "iconOnly");
}

TEST_CASE("tabWell.wellWidthOf", "[tabwell]")
{
    // The padding at both ends, the pills and the gaps between them: what the layout and both minimums use.
    CHECK(wellWidthOf(3, 100, Metrics) == (2 * 2) + 300 + (2 * 2));
    CHECK(wellWidthOf(1, 34, Metrics) == iconOnlyTabWellMinimum(1, Metrics));
    CHECK(wellWidthOf(5, Metrics.minTab, Metrics) == labelledTabWellMinimum(5, Metrics));
    CHECK(layoutTabWell(10'000, 3, Metrics).wellWidth == wellWidthOf(3, Metrics.maxTab, Metrics));
}

TEST_CASE("band.allocate", "[tabwell][band]")
{
    // The chip's steps, widest first: 300, 220, 160, 110 and its least, 90.
    auto const widthOf = [](std::string_view key) {
        constexpr auto Widths = std::array { std::pair { std::string_view { "full" }, 300.0 },
                                             std::pair { std::string_view { "noStateText" }, 220.0 },
                                             std::pair { std::string_view { "daemonElided" }, 160.0 },
                                             std::pair { std::string_view { "dotOnly" }, 110.0 } };
        auto const found = std::ranges::find(Widths, key, &std::pair<std::string_view, double>::first);
        return found != Widths.end() ? found->second : 90.0;
    };
    auto const labelled = labelledTabWellMinimum(3, Metrics);

    SECTION("the chip yields first: it takes the room beyond what keeps every tab labelled")
    {
        auto const allocation = allocateBand(widthOf, 3, Metrics, labelled + 250);
        CHECK(allocation.chip == LocationChipFit::NoStateText);
        CHECK(allocation.chipRoom == 250);
        CHECK(allocation.chipWidth == 220);
        CHECK(allocation.tabs.label == TabLabelMode::Full);
        CHECK(allocation.tabs.overflow == TabWellOverflow::Fits);
    }

    SECTION("with room to spare, the whole chip, and the well in the rest")
    {
        auto const allocation = allocateBand(widthOf, 3, Metrics, labelled + 1000);
        CHECK(allocation.chip == LocationChipFit::Full);
        CHECK(allocation.tabs == layoutTabWell(labelled + 1000 - 300, 3, Metrics));
    }

    SECTION("short of room, the chip is at its last step before any tab loses its label")
    {
        auto const allocation = allocateBand(widthOf, 3, Metrics, labelled + 50);
        CHECK(allocation.chip == LocationChipFit::WorkspaceElided);
        CHECK(allocation.chipWidth == 90);
        // The chip took 90 of the 50 spare: the tabs now pay for it, and that is the chip's least.
        CHECK(allocation.tabs == layoutTabWell(labelled + 50 - 90, 3, Metrics));
    }

    SECTION("with no tabs the chip has all the room")
    {
        auto const allocation = allocateBand(widthOf, 0, Metrics, 250);
        CHECK(allocation.chip == LocationChipFit::NoStateText);
        CHECK(allocation.tabs == TabWellLayout {});
    }

    SECTION("the yield order is the chip, then the labels, then the well")
    {
        CHECK(BandYieldOrder[0].part == BandPart::LocationChip);
        CHECK(BandYieldOrder[1].part == BandPart::TabLabels);
        CHECK(BandYieldOrder[2].part == BandPart::TabWell);
        CHECK(BandYieldOrder[0].wellKeeps(3, Metrics) == labelledTabWellMinimum(3, Metrics));
        CHECK(BandYieldOrder[1].wellKeeps(3, Metrics) == iconOnlyTabWellMinimum(3, Metrics));
        CHECK(BandYieldOrder[2].wellKeeps(3, Metrics) == 0);
    }
}
