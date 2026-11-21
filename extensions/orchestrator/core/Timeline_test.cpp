// SPDX-License-Identifier: Apache-2.0
#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <string>
#include <vector>

#include <orchestrator/core/Run.hpp>
#include <orchestrator/core/Stats.hpp>
#include <orchestrator/core/Timeline.hpp>

using namespace orchestrator;

namespace
{

constexpr auto Hour = TimelineBucketMs;
constexpr auto Now = uint64_t { 1'794'600'000'000 };

/// @return A run whose states changed at @p history's times, with @p tokens of output.
[[nodiscard]] Run runWith(std::vector<StateChange> history, RunState state, uint64_t tokens = 0)
{
    auto run = Run {};
    run.state = std::move(state);
    run.startedWallMs = history.front().wallMs;
    run.history = std::move(history);
    if (tokens != 0)
    {
        run.stats = RunStats {};
        run.stats->tokens.output = tokens;
    }
    return run;
}

} // namespace

TEST_CASE("no runs, nothing charted", "[orchestrator][timeline]")
{
    CHECK(timelineOf({}, Now) == Timeline {});
}

TEST_CASE("a finished run counts where it started, ran and ended", "[orchestrator][timeline]")
{
    // Dispatched three hours and a half ago, active for two hours, ended an hour later.
    auto const start = Now - (3 * Hour) - (Hour / 2);
    auto const runs = std::vector { runWith(
        {
            StateChange { .wallMs = start, .state = "preparing" },
            StateChange { .wallMs = start, .state = "running" },
            StateChange { .wallMs = start + (2 * Hour), .state = "ending" },
            StateChange { .wallMs = start + (2 * Hour) + 1, .state = "finished" },
        },
        Finished { .ended = Ended::Completed,
                   .fate = Fate::Removed,
                   .entries = {},
                   .seen = Seen::New,
                   .endedWallMs = start + (2 * Hour) + 1 },
        2'000) };
    auto const timeline = timelineOf(runs, Now);
    CHECK(timeline.started[20] == 1);
    CHECK(timeline.ended[22] == 1);
    // Active from the middle of hour 20 to the middle of hour 22: three buckets, its tokens spread by time.
    CHECK(timeline.active[19] == 0);
    CHECK(timeline.active[20] == 1);
    CHECK(timeline.active[21] == 1);
    CHECK(timeline.active[22] == 1);
    CHECK(timeline.active[23] == 0);
    CHECK(timeline.tokens[20] == 500);
    CHECK(timeline.tokens[21] == 1'000);
    CHECK(timeline.tokens[22] == 500);
    CHECK(timeline.activeNow == 0);
}

TEST_CASE("a run still active counts up to now", "[orchestrator][timeline]")
{
    auto const runs = std::vector {
        runWith({ StateChange { .wallMs = Now - Hour - (Hour / 2), .state = "preparing" },
                  StateChange { .wallMs = Now - Hour - (Hour / 2), .state = "running" },
                  StateChange { .wallMs = Now - (Hour / 2), .state = "blocked" } },
                Blocked {}),
        runWith({ StateChange { .wallMs = Now - 10, .state = "preparing" } },
                Preparing { PrepareStep::Fetch }),
    };
    auto const timeline = timelineOf(runs, Now);
    CHECK(timeline.activeNow == 1);
    CHECK(timeline.active[22] == 1);
    CHECK(timeline.active[23] == 1);
    CHECK(timeline.started[22] == 1);
    CHECK(timeline.started[23] == 1);
}

TEST_CASE("what lies before the window is left out", "[orchestrator][timeline]")
{
    auto const start = Now - (30 * Hour);
    auto const runs = std::vector { runWith({ StateChange { .wallMs = start, .state = "preparing" },
                                              StateChange { .wallMs = start, .state = "running" },
                                              StateChange { .wallMs = start + Hour, .state = "finished" } },
                                            Finished { .ended = Ended::Completed,
                                                       .fate = Fate::Removed,
                                                       .entries = {},
                                                       .seen = Seen::New,
                                                       .endedWallMs = start + Hour },
                                            5'000) };
    CHECK(timelineOf(runs, Now) == Timeline {});
}

TEST_CASE("a live run without a history counts from its dispatch", "[orchestrator][timeline]")
{
    auto run = Run {};
    run.state = Running {};
    run.startedWallMs = Now - (Hour / 2);
    auto const timeline = timelineOf(std::vector { run }, Now);
    CHECK(timeline.activeNow == 1);
    CHECK(timeline.active[23] == 1);
    CHECK(timeline.started[23] == 1);
}
