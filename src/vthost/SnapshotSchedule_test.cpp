// SPDX-License-Identifier: Apache-2.0
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <optional>
#include <utility>

#include <vthost/SnapshotSchedule.hpp>

using namespace std::chrono_literals;
using namespace vthost;

namespace
{

constexpr auto T = core::platform::SteadyTimePoint {} + 100s;
constexpr auto Policy = SchedulePolicy { .killGrace = 5s, .layoutDebounce = 1s };

struct ScheduleRow
{
    SnapshotTrigger trigger;
    SnapshotState state;
    std::optional<std::chrono::seconds> killedAgo;
    WriteDecision expected;
};

[[nodiscard]] constexpr WriteDecision decide(ScheduleRow const& row) noexcept
{
    auto const state =
        ScheduleState { .snapshot = row.state,
                        .lastKill = row.killedAgo ? std::optional { T - *row.killedAgo } : std::nullopt };
    return decideSnapshotWrite(row.trigger, state, T, Policy);
}

constexpr auto Skip = WriteDecision { .action = WriteAction::Skip };
constexpr auto Now = WriteDecision { .action = WriteAction::WriteNow };

[[nodiscard]] constexpr WriteDecision deferTo(std::chrono::seconds after) noexcept
{
    return WriteDecision { .action = WriteAction::Defer, .at = T + after };
}

using enum SnapshotTrigger;
using enum SnapshotState;

constexpr auto Rows = std::array {
    ScheduleRow { Periodic, Clean, std::nullopt, Skip },
    ScheduleRow { Periodic, Dirty, std::nullopt, Now },
    ScheduleRow { Periodic, Dirty, 2s, deferTo(3s) },
    ScheduleRow { Periodic, Dirty, 6s, Now },
    ScheduleRow { LayoutChanged, Clean, std::nullopt, deferTo(1s) },
    ScheduleRow { LayoutChanged, Dirty, std::nullopt, deferTo(1s) },
    ScheduleRow { LayoutChanged, Dirty, 2s, deferTo(3s) },
    ScheduleRow { SessionExited, Clean, std::nullopt, Now },
    ScheduleRow { SessionClosedByRequest, Dirty, std::nullopt, Now },
    ScheduleRow { SessionClosedByRequest, Dirty, 1s, deferTo(4s) },
    ScheduleRow { SessionKilled, Clean, std::nullopt, deferTo(5s) },
    ScheduleRow { SessionKilled, Dirty, 0s, deferTo(5s) },
    ScheduleRow { Shutdown, Clean, std::nullopt, Now },
    ScheduleRow { Shutdown, Dirty, 2s, Skip },
    ScheduleRow { Shutdown, Dirty, 6s, Now },
};

} // namespace

TEST_CASE("decideSnapshotWrite", "[vthost][snapshot][schedule]")
{
    // Constant-evaluated: the whole table is decided at compile time.
    STATIC_CHECK(
        std::ranges::all_of(Rows, [](ScheduleRow const& row) { return decide(row) == row.expected; }));
    for (auto const& row: Rows)
    {
        INFO("trigger " << std::to_underlying(row.trigger) << ", state " << std::to_underlying(row.state)
                        << ", killed " << (row.killedAgo ? row.killedAgo->count() : -1) << "s ago");
        CHECK(decide(row) == row.expected);
    }
}
