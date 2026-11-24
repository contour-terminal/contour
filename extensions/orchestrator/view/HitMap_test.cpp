// SPDX-License-Identifier: Apache-2.0
#include <catch2/catch_test_macros.hpp>

#include <vector>

#include <orchestrator/view/HitMap.hpp>

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
