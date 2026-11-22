// SPDX-License-Identifier: Apache-2.0
#include <catch2/catch_test_macros.hpp>

#include <cstdlib>
#include <filesystem>
#include <format>
#include <fstream>
#include <string>
#include <string_view>

#include <orchestrator/core/ArgumentKinds.hpp>
#include <orchestrator/core/Fixtures.hpp>
#include <orchestrator/core/Record.hpp>
#include <orchestrator/core/Run.hpp>
#include <orchestrator/core/Stats.hpp>

using namespace std::string_view_literals;
using namespace orchestrator;

namespace
{

/// The contract's example: a pull request fixed in thirty minutes, blocked once.
[[nodiscard]] Run finishedRun()
{
    auto run = Run {
        .id = "pr-2379-20261113-150912",
        .repo = "contour",
        .repoPath = "/home/u/projects/contour",
        .forge = Forge::GitHub,
        .remote = "origin",
        .base = "master",
        .item = ChangeRequestRef { .number = 2379 },
        .url = "https://github.com/contour-terminal/contour/pull/2379",
        .task = "fix_ci",
        .model = "sonnet",
        .prompt = "/contour-workflows:fix-ci https://github.com/contour-terminal/contour/pull/2379",
        .session = "123e4567-e89b-12d3-a456-426614174000",
        .branch = "agent/pr-2379-20261113-150912",
        .worktree = "/home/u/worktrees/contour/pr-2379-20261113-150912",
        .pane = std::nullopt,
        .state = Finished { .ended = Ended::Completed,
                            .fate = Fate::Removed,
                            .entries = {},
                            .seen = Seen::New,
                            .endedWallMs = 1'794'582'552'000 + 1'840'000 },
        .lastStatus = AgentStatus::Done,
        .failure = std::nullopt,
        .startedWallMs = 1'794'582'552'000,
        .attempt = 9,
        .runningSinceMs = 0,
        .changedMonoMs = 0,
    };
    run.history = {
        StateChange { .wallMs = 1'794'582'552'000, .state = "preparing" },
        StateChange { .wallMs = 1'794'582'572'000, .state = "running" },
        StateChange { .wallMs = 1'794'582'852'000, .state = "blocked" },
        StateChange { .wallMs = 1'794'582'947'000, .state = "running" },
        StateChange { .wallMs = 1'794'584'352'000, .state = "done" },
        StateChange { .wallMs = 1'794'584'382'000, .state = "ending" },
        StateChange { .wallMs = 1'794'584'392'000, .state = "finished" },
    };
    run.stats = parseStats(readFixture(FixtureRoot::Contract, "agent-stats/format-1/basic.json")).value();
    return run;
}

} // namespace

TEST_CASE("a finished run's line is the contract's", "[orchestrator][record]")
{
    auto const line = recordLine(finishedRun());
    auto const expected = readFixture(FixtureRoot::Contract, "agent-runs/format-1/finished.json");
    if (std::getenv("ORCHESTRATOR_UPDATE_GOLDENS") != nullptr)
    {
        auto file = std::ofstream { std::filesystem::path { ORCHESTRATOR_CONTRACT_DIR }
                                    / "agent-runs/format-1/finished.json" };
        file << line << '\n';
    }
    CHECK(line + '\n' == expected);
    CHECK(line.size() <= MaxTextBytes);
    CHECK_FALSE(line.contains('\n'));
}

TEST_CASE("a line that would not fit an argument leaves out the least telling first",
          "[orchestrator][record]")
{
    auto run = finishedRun();
    run.history.assign(MaxHistory, StateChange { .wallMs = 1, .state = std::string(100, 'r') });
    auto const withoutHistory = recordLine(run);
    CHECK(withoutHistory.size() <= MaxTextBytes);
    CHECK(withoutHistory.contains(R"("history":[])"));
    CHECK(withoutHistory.contains(R"("apiCalls":4)"));

    run.stats->tools.assign(MaxStoredTools, StatsTool { .name = std::string(1'000, 't'), .calls = 1 });
    auto const withoutTools = recordLine(run);
    CHECK(withoutTools.size() <= MaxTextBytes);
    CHECK(withoutTools.contains(R"("tools":[])"));
}

TEST_CASE("whether a run was recorded is stored with it", "[orchestrator][record]")
{
    auto run = finishedRun();
    CHECK(runFromJson(toJson(run))->ledger == Ledger::Pending);
    CHECK_FALSE(toJson(run).contains(R"("ledger")"));
    run.ledger = Ledger::Written;
    CHECK(toJson(run).contains(R"("ledger":"written")"));
    CHECK(runFromJson(toJson(run))->ledger == Ledger::Written);
}
