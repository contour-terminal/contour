// SPDX-License-Identifier: Apache-2.0
#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <orchestrator/core/Automation.hpp>
#include <orchestrator/core/Config.hpp>
#include <orchestrator/core/WorkItems.hpp>

using namespace std::string_view_literals;
using namespace orchestrator;

namespace
{

/// A ready MR of the repository `contour`, with the given CI, review and head commit.
[[nodiscard]] WorkItem mrItem(uint64_t number,
                              CiState ci,
                              ReviewState review = ReviewState::Required,
                              std::string headSha = "abc")
{
    auto mr = ChangeRequest {};
    mr.number = number;
    mr.title = "change " + std::to_string(number);
    mr.url = "https://example.com/mr/" + std::to_string(number);
    mr.head = "feature";
    mr.base = "master";
    mr.ci = ci;
    mr.review = review;
    mr.headSha = std::move(headSha);
    auto const group = groupOf(mr);
    return WorkItem {
        .group = group, .repo = "contour", .changeRequest = std::move(mr), .item = ChangeRequestRef { number }
    };
}

/// A ticket row of the repository `contour`, in progress and named by no MR.
[[nodiscard]] WorkItem ticketItem(std::string const& key, std::string status = "In Progress")
{
    auto issue = Issue {};
    issue.item = JiraRef { key };
    issue.title = "ticket " + key;
    issue.url = "https://example.com/browse/" + key;
    issue.status = std::move(status);
    issue.category = StatusCategory::InProgress;
    return WorkItem { .group = WorkGroup::NoMrYet,
                      .repo = "contour",
                      .ticket = std::move(issue),
                      .ticketKey = key,
                      .item = JiraRef { key } };
}

/// A configuration with a fix_ci, an address_review and an issue task.
[[nodiscard]] Config configOf()
{
    auto const json = R"({ "worktree_root": "~/worktrees",
      "repos": [ { "path": "~/repo/contour", "forge": "github" } ],
      "tasks": [
        { "id": "fix_ci", "key": "f", "label": "fix CI", "applies_to": "pr", "model": "opus", "prompt": "/x {url}" },
        { "id": "address_review", "key": "r", "label": "address review", "applies_to": "pr", "model": "opus",
          "prompt": "/x {url}" },
        { "id": "issue", "key": "w", "label": "work on issue", "applies_to": "issue", "model": "opus",
          "prompt": "/x {url}" } ] })"sv;
    auto const host = HostInfo { .home = "/home/u",
                                 .worktreeRoot = "/home/u/worktrees",
                                 .repoPaths = { "/home/u/repo/contour" },
                                 .utcOffsetMinutes = 0 };
    auto config = Config::parse(json, host, sourceTables());
    REQUIRE(config.has_value());
    return std::move(*config);
}

[[nodiscard]] AutomationRule ruleOf(AutomationCondition when, std::string task)
{
    return AutomationRule { .when = when, .task = std::move(task) };
}

/// What a plan starts or queues, as `<item index>:<task>`.
[[nodiscard]] std::vector<std::string> spelled(std::span<PlannedStart const> starts)
{
    auto spelledStarts = std::vector<std::string> {};
    for (auto const& start: starts)
        spelledStarts.push_back(std::to_string(start.item) + ":" + start.task);
    return spelledStarts;
}

/// The planner's input over @p items and @p rules, every rule active, nothing busy, no bulk start.
struct Scenario
{
    Config config = configOf();
    std::vector<WorkItem> items;
    std::vector<AutomationRule> rules;
    std::vector<RuleState> ruleStates = std::vector<RuleState>(rules.size(), RuleState::Active);
    AutomationMemory memory = {};
    std::vector<std::string> busy = {};
    std::vector<std::string> bulkItems = {};
    std::vector<std::string> bulkTasks = {};
    std::size_t freeSlots = 10;

    [[nodiscard]] AutomationPlan plan() const
    {
        return planAutomation(PlanInput { .items = items,
                                          .rules = rules,
                                          .ruleStates = ruleStates,
                                          .memory = memory,
                                          .busyItems = busy,
                                          .bulkItems = bulkItems,
                                          .bulkTasks = bulkTasks,
                                          .config = config,
                                          .freeSlots = freeSlots });
    }
};

} // namespace

TEST_CASE("automation: each condition matches its own items", "[automation]")
{
    auto draft = mrItem(1, CiState::Failing);
    draft.changeRequest->readiness = Readiness::Draft;
    auto conflicting = mrItem(2, CiState::Passing);
    conflicting.changeRequest->conflicts = Conflicts::Conflicting;

    CHECK(matchesCondition(mrItem(1, CiState::Failing), AutomationCondition::PipelineFailed));
    CHECK_FALSE(matchesCondition(mrItem(1, CiState::Passing), AutomationCondition::PipelineFailed));
    CHECK_FALSE(matchesCondition(draft, AutomationCondition::PipelineFailed));
    CHECK(matchesCondition(conflicting, AutomationCondition::Conflicts));
    CHECK_FALSE(matchesCondition(mrItem(2, CiState::Passing), AutomationCondition::Conflicts));
    CHECK(matchesCondition(mrItem(3, CiState::Passing, ReviewState::ChangesRequested),
                           AutomationCondition::ChangesRequested));
    CHECK(matchesCondition(mrItem(4, CiState::Passing, ReviewState::Approved),
                           AutomationCondition::ReadyToMerge));
    CHECK_FALSE(matchesCondition(mrItem(4, CiState::Passing, ReviewState::Required),
                                 AutomationCondition::ReadyToMerge));
    CHECK(matchesCondition(ticketItem("ABC-1"), AutomationCondition::NoMrYet));
    auto notStarted = ticketItem("ABC-2");
    notStarted.group = WorkGroup::EverythingElse;
    CHECK_FALSE(matchesCondition(notStarted, AutomationCondition::NoMrYet));
    for (auto const& row: AutomationConditionRows)
        if (row.appliesTo == AppliesTo::PullRequest)
            CHECK_FALSE(matchesCondition(ticketItem("ABC-1"), row.condition));
    CHECK_FALSE(matchesCondition(mrItem(1, CiState::Failing), AutomationCondition::NoMrYet));
}

TEST_CASE("automation: an item's state signature", "[automation]")
{
    CHECK(stateSignatureOf(mrItem(1, CiState::Failing)) == "sha:abc");
    auto noSha = mrItem(1, CiState::Failing, ReviewState::Required, "");
    noSha.changeRequest->updatedWallMs = 5;
    CHECK(stateSignatureOf(noSha) == "upd:5");
    CHECK(stateSignatureOf(ticketItem("ABC-1")) == "status:In Progress");
    CHECK(stateSignatureOf(ticketItem("ABC-1", "")) == "cat:2");
}

TEST_CASE("automation: the planner starts up to the free slots and queues the rest, in item order",
          "[automation]")
{
    auto scenario = Scenario {
        .items = { mrItem(1, CiState::Failing), mrItem(2, CiState::Failing), mrItem(3, CiState::Failing) },
        .rules = { ruleOf(AutomationCondition::PipelineFailed, "fix_ci") }
    };
    scenario.freeSlots = 2;
    auto const plan = scenario.plan();
    CHECK(spelled(plan.start) == std::vector<std::string> { "0:fix_ci", "1:fix_ci" });
    CHECK(spelled(plan.queued) == std::vector<std::string> { "2:fix_ci" });
    REQUIRE(plan.start.front().rule.has_value());
    CHECK(*plan.start.front().rule == 0);
}

TEST_CASE("automation: no free slot queues everything", "[automation]")
{
    auto scenario = Scenario { .items = { mrItem(1, CiState::Failing) },
                               .rules = { ruleOf(AutomationCondition::PipelineFailed, "fix_ci") } };
    scenario.freeSlots = 0;
    auto const plan = scenario.plan();
    CHECK(plan.start.empty());
    CHECK(spelled(plan.queued) == std::vector<std::string> { "0:fix_ci" });
}

TEST_CASE("automation: an item with a run, or a start in flight, is skipped", "[automation]")
{
    auto running = mrItem(1, CiState::Failing);
    running.agentRun = "pr-1";
    auto scenario = Scenario { .items = { running, mrItem(2, CiState::Failing) },
                               .rules = { ruleOf(AutomationCondition::PipelineFailed, "fix_ci") } };
    scenario.busy = { "contour/!2" };
    auto const plan = scenario.plan();
    CHECK(plan.start.empty());
    CHECK(plan.queued.empty());
}

TEST_CASE("automation: the first matching rule wins, and a paused rule is skipped", "[automation]")
{
    auto item = mrItem(1, CiState::Failing, ReviewState::ChangesRequested);
    auto scenario = Scenario { .items = { item },
                               .rules = { ruleOf(AutomationCondition::ChangesRequested, "address_review"),
                                          ruleOf(AutomationCondition::PipelineFailed, "fix_ci") } };
    CHECK(spelled(scenario.plan().start) == std::vector<std::string> { "0:address_review" });
    scenario.ruleStates = { RuleState::Paused, RuleState::Active };
    auto const plan = scenario.plan();
    CHECK(spelled(plan.start) == std::vector<std::string> { "0:fix_ci" });
    CHECK(*plan.start.front().rule == 1);
}

TEST_CASE("automation: an unchanged state does not fire again; a changed one does", "[automation]")
{
    auto scenario = Scenario { .items = { mrItem(1, CiState::Failing) },
                               .rules = { ruleOf(AutomationCondition::PipelineFailed, "fix_ci") } };
    scenario.memory[memoryKeyOf("pipeline_failed/fix_ci", "contour/!1")] = "sha:abc";
    CHECK(scenario.plan().start.empty());
    CHECK(scenario.plan().queued.empty());
    scenario.items = { mrItem(1, CiState::Failing, ReviewState::Required, "def") };
    CHECK(spelled(scenario.plan().start) == std::vector<std::string> { "0:fix_ci" });
}

TEST_CASE("automation: bulk items go first, ignore the memory and the rules, and drop what no task takes",
          "[automation]")
{
    auto scenario =
        Scenario { .items = { mrItem(1, CiState::Failing), mrItem(2, CiState::Passing), ticketItem("ABC-1") },
                   .rules = { ruleOf(AutomationCondition::PipelineFailed, "fix_ci") } };
    scenario.memory[memoryKeyOf("pipeline_failed/fix_ci", "contour/!1")] = "sha:abc";
    // The suggested task for each, then one task for all that only some items take.
    scenario.bulkItems = { "contour/!2", "contour/ABC-1", "contour/!9" };
    scenario.bulkTasks = { "", "", "" };
    auto const suggested = scenario.plan();
    CHECK(spelled(suggested.start) == std::vector<std::string> { "1:fix_ci", "2:issue" });
    CHECK_FALSE(suggested.start.front().rule.has_value());

    scenario.bulkTasks = { "address_review", "address_review", "address_review" };
    CHECK(spelled(scenario.plan().start) == std::vector<std::string> { "1:address_review" });
}
