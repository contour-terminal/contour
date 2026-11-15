// SPDX-License-Identifier: Apache-2.0
#include <catch2/catch_test_macros.hpp>

#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <orchestrator/core/Model.hpp>
#include <orchestrator/core/Run.hpp>
#include <orchestrator/proto/RunChanges.hpp>

using namespace std::string_view_literals;
using namespace orchestrator;
using namespace orchestrator::proto;

namespace
{

constexpr auto Id = "pr-1847-20261115-174348"sv;

[[nodiscard]] Run makeRun(std::string_view id, RunState state)
{
    auto run = Run {};
    run.id = std::string(id);
    run.repo = "contour";
    run.model = "opus";
    run.item = ChangeRequestRef { .number = 1847 };
    run.state = std::move(state);
    return run;
}

[[nodiscard]] Finished finished(Fate fate, std::vector<std::string> entries)
{
    return Finished { .ended = Ended::EndedByYou,
                      .fate = fate,
                      .entries = std::move(entries),
                      .seen = Seen::New,
                      .endedWallMs = 0 };
}

[[nodiscard]] Ending ending(EndStep step)
{
    return Ending {
        .step = step, .cause = Ended::EndedByYou, .resumedFrom = std::nullopt, .removal = Removal::NotIssued
    };
}

/// One step of a run, and the lines it is reported with.
struct Row
{
    std::string_view name;
    std::vector<Run> before;
    std::vector<Run> after;
    std::vector<std::string_view> lines;
};

} // namespace

TEST_CASE("RunChanges: the lines a step is reported with", "[orchestrator][proto]")
{
    auto const cases = std::vector<Row> {
        { .name = "nothing changed",
          .before = { makeRun(Id, Running {}) },
          .after = { makeRun(Id, Running {}) },
          .lines = {} },
        { .name = "ended by the user",
          .before = { makeRun(Id, Running {}) },
          .after = { makeRun(Id, ending(EndStep::ClosePane)) },
          .lines = { "pr-1847-20261115-174348: running → ending: closing tab" } },
        { .name = "a step of the removal checks",
          .before = { makeRun(Id, ending(EndStep::Verify)) },
          .after = { makeRun(Id, ending(EndStep::CommonDir)) },
          .lines = { "pr-1847-20261115-174348: ending: verify → ending: common dir" } },
        { .name = "removed",
          .before = { makeRun(Id, ending(EndStep::Remove)) },
          .after = { makeRun(Id, finished(Fate::Removed, {})) },
          .lines = { "pr-1847-20261115-174348: ending: remove → ended by you, worktree removed, new" } },
        { .name = "kept, with its reasons",
          .before = { makeRun(Id, ending(EndStep::Status)) },
          .after = { makeRun(Id, finished(Fate::Kept, { " M README.md", "?? notes.txt" })) },
          .lines = { "pr-1847-20261115-174348: ending: status → ended by you, worktree kept, new:  M "
                     "README.md",
                     "    ?? notes.txt" } },
        { .name = "dispatched",
          .before = {},
          .after = { makeRun(Id, Preparing {}) },
          .lines = { "pr-1847-20261115-174348: dispatched → preparing: fetch" } },
        { .name = "dismissed",
          .before = { makeRun(Id, finished(Fate::Removed, {})) },
          .after = {},
          .lines = { "pr-1847-20261115-174348: ended by you, worktree removed, new → off the list" } },
        { .name = "only the changed run",
          .before = { makeRun("pr-1-20261115-000000", Blocked {}), makeRun(Id, Running {}) },
          .after = { makeRun("pr-1-20261115-000000", Blocked {}), makeRun(Id, Done {}) },
          .lines = { "pr-1847-20261115-174348: running → done" } },
    };
    for (auto const& row: cases)
    {
        CAPTURE(row.name);
        auto const lines = changeLines(snapshotRuns(row.before), snapshotRuns(row.after));
        CHECK(lines == std::vector<std::string>(row.lines.begin(), row.lines.end()));
    }
}
