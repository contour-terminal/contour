// SPDX-License-Identifier: Apache-2.0
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cstddef>
#include <iterator>
#include <vector>

#include <orchestrator/core/WorkItems.hpp>
#include <orchestrator/view/Dashboard.hpp>
#include <orchestrator/view/HitMap.hpp>
#include <orchestrator/view/ViewFixtures.hpp>
#include <orchestrator/view/WorkBoard.hpp>

using orchestrator::view::Hit;
using orchestrator::view::HitKind;
using orchestrator::view::HitMap;
using orchestrator::view::HitRecorder;

TEST_CASE("the hit map answers what was drawn last first", "[orchestrator][view][mouse]")
{
    auto map = HitMap {};
    map.add({ .x = 0, .y = 0, .width = 40, .height = 20 }, Hit { .kind = HitKind::FleetPane });
    map.add({ .x = 1, .y = 1, .width = 38, .height = 2 }, Hit { .kind = HitKind::Row, .value = 0 });
    map.add({ .x = 1, .y = 3, .width = 38, .height = 2 }, Hit { .kind = HitKind::Row, .value = 1 });
    map.add({ .x = 5, .y = 5, .width = 0, .height = 3 }, Hit { .kind = HitKind::Row, .value = 9 });

    CHECK(map.hitsAt(2, 4)
          == std::vector { Hit { .kind = HitKind::Row, .value = 1 }, Hit { .kind = HitKind::FleetPane } });
    CHECK(map.hitsAt(0, 10) == std::vector { Hit { .kind = HitKind::FleetPane } });
    CHECK(map.hitsAt(40, 0).empty());
    CHECK(map.regions().size() == 3);
    map.clear();
    CHECK(map.hitsAt(2, 4).empty());
}

TEST_CASE("a hit recorder records in its canvas's coordinates", "[orchestrator][view][mouse]")
{
    auto map = HitMap {};
    auto const focus = HitRecorder { map, { .x = 50, .y = 2 } };
    auto const inside = focus.shifted(1, 1);
    inside.add({ .x = 0, .y = 0, .width = 5, .height = 1 }, Hit { .kind = HitKind::Command, .value = 3 });
    CHECK(map.hitsAt(51, 3) == std::vector { Hit { .kind = HitKind::Command, .value = 3 } });
    CHECK(map.hitsAt(50, 3).empty());
}

TEST_CASE("the Work board records one WorkRow per drawn item line and one WorkScrollbar region",
          "[orchestrator][view][mouse][work]")
{
    using namespace orchestrator;
    using namespace orchestrator::view;
    auto fixture = test::DashboardFixture { test::workWorld() };
    static_cast<void>(fixture.dashboard.frame());
    CHECK(fixture.dashboard.input("w").empty());
    static_cast<void>(fixture.dashboard.frame());
    auto const items = fixture.engine.workItems();
    auto const order = orderOf(items, fixture.engine.repositories(), sortOf(fixture.engine));
    auto const lines = workLines(items, order, fixture.engine, fixture.dashboard.interaction().work);
    auto const regions = fixture.dashboard.hits().regions();
    auto const countOf = [&](HitKind kind) {
        return std::ranges::count_if(regions,
                                     [kind](HitRegion const& region) { return region.hit.kind == kind; });
    };
    auto const itemLines =
        std::ranges::count_if(lines, [](WorkLine const& line) { return line.kind == WorkLineKind::Item; });
    REQUIRE(itemLines > 0);
    CHECK(countOf(HitKind::WorkRow) == itemLines);
    CHECK(countOf(HitKind::WorkHeading) == static_cast<std::ptrdiff_t>(lines.size()) - itemLines);
    CHECK(countOf(HitKind::WorkScrollbar) == 1);
    // Each item line's region stands for its own line, and no two share a cell.
    auto rows = std::vector<HitRegion> {};
    std::ranges::copy_if(regions, std::back_inserter(rows), [](HitRegion const& region) {
        return region.hit.kind == HitKind::WorkRow;
    });
    for (auto const& row: rows)
    {
        REQUIRE(row.hit.value < lines.size());
        CHECK(lines[row.hit.value].kind == WorkLineKind::Item);
        CHECK(fixture.dashboard.hits().hitsAt(row.area.x + row.area.width - 1, row.area.y).front()
              == row.hit);
    }
}
