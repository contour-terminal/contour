// SPDX-License-Identifier: Apache-2.0
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cstdint>
#include <iterator>
#include <optional>
#include <ranges>
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

namespace
{

[[nodiscard]] Finished keptFor(std::string reason)
{
    return finished(Fate::Kept, { std::move(reason) });
}

/// A kept worktree being checked again, at @p step.
[[nodiscard]] Ending recheckAt(EndStep step, std::string reason)
{
    auto again = ending(step);
    again.resumedFrom = keptFor(std::move(reason));
    return again;
}

/// Whether the user asked for the re-check before the first step.
enum class Asked : uint8_t
{
    No,  ///< A poll checks again.
    Yes, ///< `retry`.
};

/// A run's states, one look after another, and every line said on the way.
struct SequenceRow
{
    std::string_view name;
    Asked asked;
    std::vector<RunState> looks;
    std::vector<std::string_view> lines;
};

} // namespace

TEST_CASE("RunChanges: a re-check is reported by its outcome", "[orchestrator][proto]")
{
    constexpr auto Rebase = "rebase in progress"sv;
    auto const cases = std::vector<SequenceRow> {
        { .name = "a poll finds the same",
          .asked = Asked::No,
          .looks = { keptFor(std::string(Rebase)),
                     recheckAt(EndStep::Verify, std::string(Rebase)),
                     recheckAt(EndStep::OperationState, std::string(Rebase)),
                     keptFor(std::string(Rebase)) },
          .lines = {} },
        { .name = "a poll removes it",
          .asked = Asked::No,
          .looks = { keptFor(std::string(Rebase)),
                     recheckAt(EndStep::Verify, std::string(Rebase)),
                     recheckAt(EndStep::Remove, std::string(Rebase)),
                     finished(Fate::Removed, {}) },
          .lines = { "pr-1847-20261115-174348: ended by you, worktree kept, new: rebase in progress → ended "
                     "by "
                     "you, worktree removed, new" } },
        { .name = "a poll finds another reason",
          .asked = Asked::No,
          .looks = { keptFor(std::string(Rebase)),
                     recheckAt(EndStep::Status, std::string(Rebase)),
                     keptFor(" M a.txt") },
          .lines = { "pr-1847-20261115-174348: ended by you, worktree kept, new: rebase in progress → ended "
                     "by "
                     "you, worktree kept, new:  M a.txt" } },
        { .name = "a retry finds the same",
          .asked = Asked::Yes,
          .looks = { keptFor(std::string(Rebase)),
                     recheckAt(EndStep::Verify, std::string(Rebase)),
                     keptFor(std::string(Rebase)) },
          .lines = { "pr-1847-20261115-174348: checked again, unchanged: ended by you, worktree kept, new: "
                     "rebase in progress" } },
        { .name = "a retry removes it",
          .asked = Asked::Yes,
          .looks = { keptFor(std::string(Rebase)),
                     recheckAt(EndStep::Verify, std::string(Rebase)),
                     finished(Fate::Removed, {}) },
          .lines = { "pr-1847-20261115-174348: ended by you, worktree kept, new: rebase in progress → ended "
                     "by "
                     "you, worktree removed, new" } },
        { .name = "a retry the core left as it was",
          .asked = Asked::Yes,
          .looks = { keptFor(std::string(Rebase)), keptFor(std::string(Rebase)) },
          .lines = { "pr-1847-20261115-174348: checked again, unchanged: ended by you, worktree kept, new: "
                     "rebase in progress" } },
        // The start's own look: the runs as stored, then as the start left them.
        { .name = "the start finds a tab gone",
          .asked = Asked::No,
          .looks = { Running {}, ending(EndStep::MainCheckout) },
          .lines = { "pr-1847-20261115-174348: running → ending: waiting for the main checkout" } },
        { .name = "a first ending is said step by step",
          .asked = Asked::No,
          .looks = { Running {}, ending(EndStep::Verify), ending(EndStep::CommonDir) },
          .lines = { "pr-1847-20261115-174348: running → ending: verify",
                     "pr-1847-20261115-174348: ending: verify → ending: common dir" } },
    };
    for (auto const& row: cases)
    {
        CAPTURE(row.name);
        auto report = ChangeReport {};
        auto said = std::vector<std::string> {};
        auto previous = snapshotRuns(std::vector { makeRun(Id, row.looks.front()) });
        if (row.asked == Asked::Yes)
            report.requested(std::string(Id));
        for (auto const& look: row.looks | std::views::drop(1))
        {
            auto next = snapshotRuns(std::vector { makeRun(Id, look) });
            std::ranges::move(report.lines(previous, next), std::back_inserter(said));
            previous = std::move(next);
        }
        CHECK(said == std::vector<std::string>(row.lines.begin(), row.lines.end()));
    }
}
