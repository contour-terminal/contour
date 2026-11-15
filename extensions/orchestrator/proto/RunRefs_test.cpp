// SPDX-License-Identifier: Apache-2.0
#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstddef>
#include <expected>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <orchestrator/core/Model.hpp>
#include <orchestrator/core/Run.hpp>
#include <orchestrator/proto/RunRefs.hpp>

using namespace std::string_view_literals;
using namespace orchestrator;
using namespace orchestrator::proto;

namespace
{

[[nodiscard]] Run makeRun(std::string id, ItemRef item, RunState state)
{
    auto run = Run {};
    run.id = std::move(id);
    run.repo = "contour";
    run.model = "opus";
    run.item = std::move(item);
    run.state = std::move(state);
    return run;
}

[[nodiscard]] Finished finished(Fate fate)
{
    return Finished { .ended = Ended::EndedByYou,
                      .fate = fate,
                      .entries = fate == Fate::Kept ? std::vector<std::string> { "a rebase is in progress" }
                                                    : std::vector<std::string> {},
                      .seen = Seen::New,
                      .endedWallMs = 0 };
}

/// Two runs on PR #1847 (one running, one finished and removed), one blocked on PR #18, a kept one on issue
/// #1847, and a running Jira one.
[[nodiscard]] std::vector<Run> fixtureRuns()
{
    auto runs = std::vector<Run> {};
    runs.push_back(makeRun("pr-1847-20261115-174348", ChangeRequestRef { .number = 1847 }, Running {}));
    runs.push_back(
        makeRun("pr-1847-20261114-090000", ChangeRequestRef { .number = 1847 }, finished(Fate::Removed)));
    runs.push_back(makeRun("pr-18-20261115-100000", ChangeRequestRef { .number = 18 }, Blocked {}));
    runs.push_back(makeRun("issue-1847-20261115-110000", IssueRef { .number = 1847 }, finished(Fate::Kept)));
    runs.push_back(makeRun("abc-12-20261115-120000", JiraRef { .key = "ABC-12" }, Done {}));
    return runs;
}

/// The ids the last `runs` printed: rows 1 to 4, then a run that has gone since.
[[nodiscard]] std::vector<std::string> printedRows()
{
    return { "pr-1847-20261115-174348",
             "pr-1847-20261114-090000",
             "pr-18-20261115-100000",
             "issue-1847-20261115-110000",
             "pr-9-20261101-000000" };
}

/// What resolveRun() answers: a run's id, or a problem and its candidates.
struct Expected
{
    std::string_view id;
    RunRefProblem problem = RunRefProblem::Unknown;
    std::vector<std::string_view> candidates;
};

[[nodiscard]] Expected found(std::string_view id)
{
    return Expected { .id = id, .problem = RunRefProblem::Unknown, .candidates = {} };
}

[[nodiscard]] Expected refused(RunRefProblem problem, std::vector<std::string_view> candidates)
{
    return Expected { .id = {}, .problem = problem, .candidates = std::move(candidates) };
}

struct Row
{
    std::string_view name;
    std::string_view word;
    RunAction action;
    Expected expected;
};

} // namespace

TEST_CASE("RunRefs: the run a word names", "[orchestrator][proto]")
{
    using enum RunAction;
    using enum RunRefProblem;
    auto const cases = std::vector<Row> {
        // An exact id.
        { .name = "exact id",
          .word = "pr-18-20261115-100000",
          .action = End,
          .expected = found("pr-18-20261115-100000") },
        { .name = "exact id, not endable",
          .word = "pr-1847-20261114-090000",
          .action = End,
          .expected = refused(NotApplicable, { "pr-1847-20261114-090000" }) },
        // A row of the last `runs`.
        { .name = "row", .word = "1", .action = End, .expected = found("pr-1847-20261115-174348") },
        { .name = "row of a kept run",
          .word = "4",
          .action = Retry,
          .expected = found("issue-1847-20261115-110000") },
        { .name = "row of a removed run",
          .word = "2",
          .action = Dismiss,
          .expected = found("pr-1847-20261114-090000") },
        { .name = "row, not retryable",
          .word = "1",
          .action = Retry,
          .expected = refused(NotApplicable, { "pr-1847-20261115-174348" }) },
        { .name = "row beyond the list", .word = "6", .action = End, .expected = refused(NoSuchRow, {}) },
        { .name = "row zero", .word = "0", .action = End, .expected = refused(NoSuchRow, {}) },
        { .name = "row gone since",
          .word = "5",
          .action = End,
          .expected = refused(RowGone, { "pr-9-20261101-000000" }) },
        // A unique prefix, among the runs the action takes.
        { .name = "prefix, one endable",
          .word = "pr-1847",
          .action = End,
          .expected = found("pr-1847-20261115-174348") },
        { .name = "prefix, one dismissable",
          .word = "pr-1847",
          .action = Dismiss,
          .expected = found("pr-1847-20261114-090000") },
        { .name = "prefix, upper case",
          .word = "PR-18-",
          .action = End,
          .expected = found("pr-18-20261115-100000") },
        { .name = "prefix, ambiguous",
          .word = "pr-18",
          .action = End,
          .expected = refused(Ambiguous, { "pr-1847-20261115-174348", "pr-18-20261115-100000" }) },
        { .name = "prefix, none takeable",
          .word = "issue",
          .action = End,
          .expected = refused(NotApplicable, { "issue-1847-20261115-110000" }) },
        // An item's spelling.
        { .name = "#number, endable PR",
          .word = "#1847",
          .action = End,
          .expected = found("pr-1847-20261115-174348") },
        { .name = "#number, kept issue",
          .word = "#1847",
          .action = Retry,
          .expected = found("issue-1847-20261115-110000") },
        { .name = "PR#number", .word = "PR#18", .action = End, .expected = found("pr-18-20261115-100000") },
        { .name = "pr number", .word = "pr18", .action = End, .expected = found("pr-18-20261115-100000") },
        { .name = "PR#number names no issue",
          .word = "PR#1847",
          .action = Retry,
          .expected = refused(NotApplicable, { "pr-1847-20261115-174348", "pr-1847-20261114-090000" }) },
        { .name = "issue#number",
          .word = "issue#1847",
          .action = Retry,
          .expected = found("issue-1847-20261115-110000") },
        { .name = "Jira key", .word = "abc-12", .action = End, .expected = found("abc-12-20261115-120000") },
        // Nothing.
        { .name = "unknown", .word = "pr-77", .action = End, .expected = refused(Unknown, {}) },
        { .name = "unknown item", .word = "#77", .action = End, .expected = refused(Unknown, {}) },
    };
    auto const runs = fixtureRuns();
    auto const rows = printedRows();
    for (auto const& row: cases)
    {
        CAPTURE(row.name, row.word);
        auto const resolved = resolveRun(row.word, row.action, rows, runs);
        if (!row.expected.id.empty())
        {
            REQUIRE(resolved.has_value());
            CHECK(*resolved == row.expected.id);
            continue;
        }
        REQUIRE_FALSE(resolved.has_value());
        CHECK(resolved.error().problem == row.expected.problem);
        auto const candidates =
            std::vector<std::string>(row.expected.candidates.begin(), row.expected.candidates.end());
        CHECK(resolved.error().candidates == candidates);
    }
}

TEST_CASE("RunRefs: no rows before the first `runs`", "[orchestrator][proto]")
{
    auto const runs = fixtureRuns();
    auto const resolved = resolveRun("1", RunAction::End, std::span<std::string const> {}, runs);
    REQUIRE_FALSE(resolved.has_value());
    CHECK(resolved.error().problem == RunRefProblem::NoSuchRow);
}

TEST_CASE("RunRefs: what the user reads", "[orchestrator][proto]")
{
    auto const runs = fixtureRuns();
    struct TextRow
    {
        std::string_view word;
        RunAction action;
        RunRefError error;
        std::string_view text;
    };
    auto const cases = std::vector<TextRow> {
        { .word = "pr-77",
          .action = RunAction::End,
          .error = { .problem = RunRefProblem::Unknown, .candidates = {} },
          .text =
              "no run matches pr-77: name it by its row in `runs`, its id or the start of it, or its item "
              "(#1847)" },
        { .word = "6",
          .action = RunAction::End,
          .error = { .problem = RunRefProblem::NoSuchRow, .candidates = {} },
          .text = "no row 6 in the last `runs`" },
        { .word = "5",
          .action = RunAction::End,
          .error = { .problem = RunRefProblem::RowGone, .candidates = { "pr-9-20261101-000000" } },
          .text = "row 5 was pr-9-20261101-000000, which is no longer listed: type `runs` again" },
        { .word = "pr-18",
          .action = RunAction::End,
          .error = { .problem = RunRefProblem::Ambiguous,
                     .candidates = { "pr-1847-20261115-174348", "pr-18-20261115-100000" } },
          .text =
              "pr-18 matches 2 runs: pr-1847-20261115-174348, pr-18-20261115-100000; name one by its row or "
              "its id" },
        { .word = "4",
          .action = RunAction::End,
          .error = { .problem = RunRefProblem::NotApplicable,
                     .candidates = { "issue-1847-20261115-110000" } },
          .text =
              "`end` takes a running, blocked or done run; 4 names issue-1847-20261115-110000 (ended by you, "
              "worktree kept, new: a rebase is in progress)" },
    };
    for (auto const& row: cases)
    {
        CAPTURE(row.word);
        CHECK(refusalText(row.word, row.action, row.error, runs) == row.text);
    }
}

TEST_CASE("RunRefs: the confirmation of each action", "[orchestrator][proto]")
{
    struct ConfirmRow
    {
        RunAction action;
        std::string_view text;
    };
    static constexpr auto Cases = std::array {
        ConfirmRow {
            .action = RunAction::End,
            .text = "ending pr-1847-20261115-174348: closing its tab, then checking whether its worktree "
                    "can be removed" },
        ConfirmRow { .action = RunAction::Retry,
                     .text =
                         "checking pr-1847-20261115-174348 again: its kept worktree goes through the removal "
                         "checks, and is removed if they pass" },
        ConfirmRow { .action = RunAction::Dismiss,
                     .text = "dismissing pr-1847-20261115-174348: it leaves the list and the store" },
    };
    for (auto const& row: Cases)
        CHECK(confirmationText(row.action, "pr-1847-20261115-174348") == row.text);
}

TEST_CASE("RunRefs: a retry is refused while the main checkout is not known", "[orchestrator][proto]")
{
    struct Row
    {
        std::string_view name;
        RunAction action;
        CheckoutKnowledge checkout;
        std::optional<std::string_view> text;
    };
    static constexpr auto Rows = std::array {
        Row { .name = "retry, not known yet",
              .action = RunAction::Retry,
              .checkout = CheckoutKnowledge::Pending,
              .text = "not checked: the main checkout of contour is not known yet; try again after the next "
                      "listing"sv },
        Row { .name = "retry, not to be known",
              .action = RunAction::Retry,
              .checkout = CheckoutKnowledge::Unavailable,
              .text =
                  "not checked: the main checkout of contour cannot be found: the repository is configured "
                  "at another path than /home/u/projects/contour, or git cannot list its worktrees; fix the "
                  "configuration or the repository, then retry"sv },
        Row { .name = "retry, known",
              .action = RunAction::Retry,
              .checkout = CheckoutKnowledge::Known,
              .text = std::nullopt },
        // Ending waits for the checkout instead: its tab is closed now, its checks run once it is known.
        Row { .name = "end, not known",
              .action = RunAction::End,
              .checkout = CheckoutKnowledge::Pending,
              .text = std::nullopt },
        Row { .name = "dismiss, not known",
              .action = RunAction::Dismiss,
              .checkout = CheckoutKnowledge::Pending,
              .text = std::nullopt },
    };
    auto run = makeRun("issue-1847-20261115-110000", IssueRef { .number = 1847 }, finished(Fate::Kept));
    run.repoPath = "/home/u/projects/contour";
    for (auto const& row: Rows)
    {
        CAPTURE(row.name);
        auto const text = deferralText(row.action, run, row.checkout);
        CHECK(text.has_value() == row.text.has_value());
        if (text && row.text)
            CHECK(*text == *row.text);
    }
}

TEST_CASE("RunRefs: what the engine knows of the main checkout", "[orchestrator][proto]")
{
    CHECK(checkoutKnowledge(std::nullopt) == CheckoutKnowledge::Known);
    CHECK(checkoutKnowledge(CheckoutUnknown::Pending) == CheckoutKnowledge::Pending);
    CHECK(checkoutKnowledge(CheckoutUnknown::Unavailable) == CheckoutKnowledge::Unavailable);
}

TEST_CASE("RunRefs: which runs each action takes", "[orchestrator][proto]")
{
    struct StateRow
    {
        std::string_view name;
        RunState state;
        bool end;
        bool retry;
        bool dismiss;
    };
    auto const cases = std::vector<StateRow> {
        { .name = "preparing", .state = Preparing {}, .end = false, .retry = false, .dismiss = false },
        { .name = "running", .state = Running {}, .end = true, .retry = false, .dismiss = false },
        { .name = "blocked", .state = Blocked {}, .end = true, .retry = false, .dismiss = false },
        { .name = "done", .state = Done {}, .end = true, .retry = false, .dismiss = false },
        { .name = "ending", .state = Ending {}, .end = false, .retry = false, .dismiss = false },
        { .name = "kept", .state = finished(Fate::Kept), .end = false, .retry = true, .dismiss = false },
        { .name = "removed",
          .state = finished(Fate::Removed),
          .end = false,
          .retry = false,
          .dismiss = true },
    };
    for (auto const& row: cases)
    {
        CAPTURE(row.name);
        auto const run = makeRun("pr-1-20261115-000000", ChangeRequestRef { .number = 1 }, row.state);
        CHECK(actionApplies(RunAction::End, run) == row.end);
        CHECK(actionApplies(RunAction::Retry, run) == row.retry);
        CHECK(actionApplies(RunAction::Dismiss, run) == row.dismiss);
    }
}
