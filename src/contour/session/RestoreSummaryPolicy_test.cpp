// SPDX-License-Identifier: Apache-2.0
#include <contour/session/RestoreSummaryPolicy.hpp>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <string_view>

using namespace contour::session;

namespace
{

struct FateCase
{
    std::string_view name;
    LayoutWindows layout;
    AttachSpawn spawn;
    AttachBoot boot;
    RestoreSummaryFate expected;
};

constexpr auto FateCases = std::array {
    FateCase { "no layout yet",
               LayoutWindows::None,
               AttachSpawn::Idle,
               AttachBoot::Adopted,
               RestoreSummaryFate::Keep },
    FateCase { "boot window not up",
               LayoutWindows::Some,
               AttachSpawn::Idle,
               AttachBoot::Pending,
               RestoreSummaryFate::Keep },
    FateCase { "a window is spawning",
               LayoutWindows::Some,
               AttachSpawn::Spawning,
               AttachBoot::Adopted,
               RestoreSummaryFate::Keep },
    FateCase { "nothing left to bind",
               LayoutWindows::Some,
               AttachSpawn::Idle,
               AttachBoot::Adopted,
               RestoreSummaryFate::Expire },
    FateCase { "empty layout, all else settled",
               LayoutWindows::None,
               AttachSpawn::Idle,
               AttachBoot::Pending,
               RestoreSummaryFate::Keep },
    FateCase { "spawning and pending",
               LayoutWindows::Some,
               AttachSpawn::Spawning,
               AttachBoot::Pending,
               RestoreSummaryFate::Keep },
};

} // namespace

TEST_CASE("restoreSummaryFateAfterLayout: the summary survives until every window of the layout is bound",
          "[restore]")
{
    for (auto const& row: FateCases)
    {
        INFO(row.name);
        CHECK(restoreSummaryFateAfterLayout(row.layout, row.spawn, row.boot) == row.expected);
    }
}
