// SPDX-License-Identifier: Apache-2.0
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <ranges>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include <orchestrator/core/WorkItems.hpp>

using namespace std::string_view_literals;
using namespace orchestrator;

namespace
{

/// A ready, non-draft MR with the given number, title, CI and review; everything else defaulted.
ChangeRequest mrOf(uint64_t number, std::string title, CiState ci, ReviewState review)
{
    auto mr = ChangeRequest {};
    mr.number = number;
    mr.title = std::move(title);
    mr.url = "https://gitlab.example.com/group/project/-/merge_requests/" + std::to_string(number);
    mr.head = "feature";
    mr.base = "master";
    mr.ci = ci;
    mr.review = review;
    return mr;
}

/// A listed Jira ticket.
ListedIssue jiraOf(std::string const& key, Progress progress, std::size_t rank = 0)
{
    auto issue = Issue {};
    issue.item = JiraRef { key };
    issue.title = "ticket " + key;
    issue.url = "https://example.atlassian.net/browse/" + key;
    issue.status = progress == Progress::InProgress ? "In Progress" : "To Do";
    issue.category = progress == Progress::InProgress ? StatusCategory::InProgress : StatusCategory::ToDo;
    issue.listRank = rank;
    return ListedIssue { .issue = std::move(issue), .progress = progress };
}

/// A listed forge issue: always in progress.
ListedIssue forgeOf(uint64_t number)
{
    auto issue = Issue {};
    issue.item = IssueRef { number };
    issue.title = "issue " + std::to_string(number);
    issue.url = "https://gitlab.example.com/group/project/-/issues/" + std::to_string(number);
    return ListedIssue { .issue = std::move(issue), .progress = Progress::InProgress };
}

/// A run of @p repo on @p item in @p state, on @p worker.
Run runOf(std::string id, std::string repo, ItemRef item, std::string worker, RunState state)
{
    auto run = Run {};
    run.id = std::move(id);
    run.repo = std::move(repo);
    run.item = std::move(item);
    run.worker = std::move(worker);
    run.state = std::move(state);
    return run;
}

WorkItem const* findItem(std::span<WorkItem const> items, std::string_view key)
{
    auto const found =
        std::ranges::find_if(items, [&](WorkItem const& item) { return workItemKey(item) == key; });
    return found != items.end() ? &*found : nullptr;
}

/// The keys of @p items in the order @p order gives.
std::vector<std::string> keysInOrder(std::span<WorkItem const> items, std::span<std::size_t const> order)
{
    auto keys = std::vector<std::string> {};
    for (auto const index: order)
        keys.push_back(workItemKey(items[index]));
    return keys;
}

} // namespace

TEST_CASE("WorkItems: a key links only at word boundaries and in a covered project", "[orchestrator][work]")
{
    auto const projects = std::vector<std::string> { "ABC", "PRJ" };
    CHECK(linkedJiraKey("PRJ-1234: fix it", "", projects) == "PRJ-1234");
    CHECK(linkedJiraKey("fix it", "agent/prj-1234-20261113-150847", projects) == "PRJ-1234");
    CHECK(linkedJiraKey("ABC-7 then PRJ-1", "", projects) == "ABC-7");
    CHECK(linkedJiraKey("[PRJ-12] fix", "", projects) == "PRJ-12");
    CHECK(linkedJiraKey("UTF-8 for PRJ-5", "", projects) == "PRJ-5"); // a stray token does not stop the scan
    CHECK(linkedJiraKey("no key", "PRJ-3-branch", projects) == "PRJ-3");
    CHECK(linkedJiraKey("PRJ-9 in the title", "abc-1", projects) == "PRJ-9"); // the title wins
    CHECK_FALSE(linkedJiraKey("UTF-8 handling", "", projects));               // Review Focus 2
    CHECK_FALSE(linkedJiraKey("SHA-256 sums", "", projects));
    CHECK_FALSE(linkedJiraKey("XPRJ-1234", "", projects));
    CHECK_FALSE(linkedJiraKey("PRJ-1234x", "", projects));
    CHECK_FALSE(linkedJiraKey("PRJ-", "", projects));
    CHECK_FALSE(linkedJiraKey("OTHER-12", "", projects));            // not covered
    CHECK_FALSE(linkedJiraKey("prj-1234 in a title", "", projects)); // titles are case-sensitive
    CHECK_FALSE(linkedJiraKey("fix", "xprj-12", projects));
    CHECK_FALSE(linkedJiraKey("PRJ-1", "", std::vector<std::string> {}));
}

TEST_CASE("WorkItems: the covered projects come from listed keys and the JQL", "[orchestrator][work]")
{
    auto repos = std::vector<Repo>(2);
    repos[0].name = "contour";
    repos[0].issueQueries = {
        { .source = "jira",
          .text = R"(project = "abc" AND assignee = currentUser())",
          .site = "https://example.atlassian.net" },
        { .source = "jira",
          .text = "PROJECT in (Xyz, \"QQ\") ORDER BY updated",
          .site = "https://example.atlassian.net" },
        { .source = "forge", .text = "project = NOTJIRA", .site = "" },
    };
    repos[1].name = "other";
    repos[1].issueQueries = {
        { .source = "jira",
          .text = "project=abc AND myproject = NOPE",
          .site = "https://example.atlassian.net" },
        { .source = "jira",
          .text = "project != NEQ AND project = 10001",
          .site = "https://example.atlassian.net" },
    };
    auto const issues = std::vector<ListedIssue> { jiraOf("PRJ-1", Progress::NotStarted), forgeOf(5) };
    auto const listed =
        std::vector<RepoWork> { { .repo = "contour", .changeRequests = {}, .issues = issues } };
    CHECK(jiraProjectsOf(repos, listed) == std::vector<std::string> { "ABC", "PRJ", "QQ", "XYZ" });
}

TEST_CASE("WorkItems: an MR's group is the first row of the spec's table that matches",
          "[orchestrator][work]")
{
    struct Row
    {
        std::string_view name;
        Readiness readiness = Readiness::Ready;
        CiState ci = CiState::Passing;
        ReviewState review = ReviewState::Approved;
        Conflicts conflicts = Conflicts::None;
        StateSource source = StateSource::Complete;
        WorkGroup expected = WorkGroup::ReadyToMerge;
    };
    auto const rows = std::array {
        Row { .name = "draft and failing",
              .readiness = Readiness::Draft,
              .ci = CiState::Failing,
              .expected = WorkGroup::EverythingElse },
        Row { .name = "failing", .ci = CiState::Failing, .expected = WorkGroup::NeedsYou },
        Row { .name = "changes requested",
              .review = ReviewState::ChangesRequested,
              .expected = WorkGroup::NeedsYou },
        Row { .name = "conflicting and approved",
              .conflicts = Conflicts::Conflicting,
              .expected = WorkGroup::NeedsYou },
        Row { .name = "passing and approved", .expected = WorkGroup::ReadyToMerge },
        Row { .name = "no CI, no review",
              .ci = CiState::None,
              .review = ReviewState::None,
              .expected = WorkGroup::ReadyToMerge },
        Row { .name = "pending and approved", .ci = CiState::Pending, .expected = WorkGroup::InReview },
        Row { .name = "passing, review required",
              .review = ReviewState::Required,
              .expected = WorkGroup::InReview },
        Row { .name = "inferred, discussions not resolved",
              .ci = CiState::None,
              .review = ReviewState::None,
              .source = StateSource::Inferred,
              .expected = WorkGroup::InReview },
        Row { .name = "inferred, mergeable",
              .ci = CiState::None,
              .review = ReviewState::Approved,
              .source = StateSource::Inferred,
              .expected = WorkGroup::ReadyToMerge },
        Row { .name = "inferred, failing",
              .ci = CiState::Failing,
              .review = ReviewState::None,
              .source = StateSource::Inferred,
              .expected = WorkGroup::NeedsYou },
    };
    for (auto const& row: rows)
    {
        INFO(row.name);
        auto mr = mrOf(1, "t", row.ci, row.review);
        mr.readiness = row.readiness;
        mr.conflicts = row.conflicts;
        mr.source = row.source;
        CHECK(groupOf(mr) == row.expected);
    }
    CHECK(groupOf(Progress::InProgress) == WorkGroup::NoMrYet);
    CHECK(groupOf(Progress::NotStarted) == WorkGroup::EverythingElse);
}

TEST_CASE("WorkItems: progress by category, by query, and forge issues always", "[orchestrator][work]")
{
    auto ticket = jiraOf("PRJ-1240", Progress::InProgress).issue;
    auto const keys = std::vector<std::string> { "PRJ-9" };
    auto const matching = std::vector<std::string> { "PRJ-1240" };
    CHECK(progressOf(ticket, std::nullopt) == Progress::InProgress);
    CHECK(progressOf(ticket, std::span<std::string const> { keys }) == Progress::NotStarted);
    ticket.category = StatusCategory::ToDo;
    CHECK(progressOf(ticket, std::nullopt) == Progress::NotStarted);
    CHECK(progressOf(ticket, std::span<std::string const> { matching }) == Progress::InProgress);
    CHECK(progressOf(forgeOf(78).issue, std::nullopt) == Progress::InProgress);
    CHECK(progressOf(forgeOf(78).issue, std::span<std::string const> { keys }) == Progress::InProgress);
}

TEST_CASE("WorkItems: a forge issue is named by closesIssues and by #<n> in the title",
          "[orchestrator][work]")
{
    auto mr = mrOf(1, "fix #77 and #12, not x#5 nor #6a; again #77", CiState::Passing, ReviewState::None);
    mr.closesIssues = { 12, 40 };
    CHECK(namedForgeIssues(mr) == std::vector<uint64_t> { 12, 40, 77 });
    CHECK(namedForgeIssues(mrOf(2, "(#3)", CiState::None, ReviewState::None)) == std::vector<uint64_t> { 3 });
    CHECK(namedForgeIssues(mrOf(3, "# 3 and #", CiState::None, ReviewState::None)).empty());
}

TEST_CASE("WorkItems: a named ticket shows on its MR, not alone", "[orchestrator][work]")
{
    auto const projects = std::vector<std::string> { "PRJ" };
    auto const mrs = std::vector<ChangeRequest> {
        mrOf(2375, "PRJ-1234: flaky test", CiState::Failing, ReviewState::Approved),
        mrOf(2380, "fix #77", CiState::Passing, ReviewState::Approved),
    };
    auto const issues = std::vector<ListedIssue> {
        jiraOf("PRJ-1234", Progress::InProgress),
        jiraOf("PRJ-1300", Progress::NotStarted, 1),
        forgeOf(77),
        forgeOf(78),
    };
    auto const repos =
        std::vector<RepoWork> { { .repo = "contour", .changeRequests = mrs, .issues = issues } };
    auto const items =
        workItems(WorkInput { .repos = repos, .linkedTickets = {}, .jiraProjects = projects, .runs = {} });

    REQUIRE(items.size() == 4);
    auto const* const failing = findItem(items, "contour/!2375");
    REQUIRE(failing != nullptr);
    CHECK(failing->group == WorkGroup::NeedsYou);
    CHECK(failing->ticketKey == "PRJ-1234");
    REQUIRE(failing->ticket.has_value());
    CHECK(failing->ticket->status == "In Progress");
    CHECK(failing->item == ItemRef { ChangeRequestRef { 2375 } });

    auto const* const ready = findItem(items, "contour/!2380");
    REQUIRE(ready != nullptr);
    CHECK(ready->group == WorkGroup::ReadyToMerge);
    CHECK(ready->ticketKey == "#77");
    REQUIRE(ready->ticket.has_value());
    CHECK(ready->ticket->item == ItemRef { IssueRef { 77 } });

    auto const* const issue = findItem(items, "contour/#78");
    REQUIRE(issue != nullptr);
    CHECK(issue->group == WorkGroup::NoMrYet);
    CHECK(!issue->changeRequest.has_value());
    CHECK(issue->ticketKey == "#78");
    CHECK(issue->item == ItemRef { IssueRef { 78 } });

    auto const* const notStarted = findItem(items, "contour/PRJ-1300");
    REQUIRE(notStarted != nullptr);
    CHECK(notStarted->group == WorkGroup::EverythingElse);
    CHECK(notStarted->item == ItemRef { JiraRef { "PRJ-1300" } });

    CHECK(findItem(items, "contour/PRJ-1234") == nullptr);
    CHECK(findItem(items, "contour/#77") == nullptr);
}

TEST_CASE("WorkItems: a ticket listed by two repositories belongs to the first", "[orchestrator][work]")
{
    auto const projects = std::vector<std::string> { "PRJ" };
    auto const contourIssues =
        std::vector<ListedIssue> { jiraOf("PRJ-1240", Progress::InProgress), forgeOf(5) };
    auto const otherIssues =
        std::vector<ListedIssue> { jiraOf("PRJ-1240", Progress::InProgress), forgeOf(5) };

    SECTION("no MR names it")
    {
        auto const repos = std::vector<RepoWork> {
            { .repo = "contour", .changeRequests = {}, .issues = contourIssues },
            { .repo = "other", .changeRequests = {}, .issues = otherIssues },
        };
        auto const items = workItems(
            WorkInput { .repos = repos, .linkedTickets = {}, .jiraProjects = projects, .runs = {} });
        CHECK(std::ranges::count(items, "contour"sv, &WorkItem::repo) == 2);
        CHECK(findItem(items, "contour/PRJ-1240") != nullptr);
        CHECK(findItem(items, "other/PRJ-1240") == nullptr);
        // A forge issue's key is its repository and number: the same number in two repositories is two items.
        CHECK(findItem(items, "contour/#5") != nullptr);
        CHECK(findItem(items, "other/#5") != nullptr);
        CHECK(items.size() == 3);
    }
    SECTION("an MR of the second repository names it")
    {
        auto const mrs =
            std::vector<ChangeRequest> { mrOf(812, "PRJ-1240 fix", CiState::Pending, ReviewState::None) };
        auto const repos = std::vector<RepoWork> {
            { .repo = "contour", .changeRequests = {}, .issues = contourIssues },
            { .repo = "other", .changeRequests = mrs, .issues = otherIssues },
        };
        auto const items = workItems(
            WorkInput { .repos = repos, .linkedTickets = {}, .jiraProjects = projects, .runs = {} });
        auto const* const mr = findItem(items, "other/!812");
        REQUIRE(mr != nullptr);
        CHECK(mr->ticketKey == "PRJ-1240");
        CHECK(mr->ticket.has_value());
        CHECK(std::ranges::none_of(
            items, [](WorkItem const& item) { return !item.changeRequest && item.ticketKey == "PRJ-1240"; }));
    }
}

TEST_CASE("WorkItems: a key the sources did not list is found among the looked-up tickets",
          "[orchestrator][work]")
{
    auto const projects = std::vector<std::string> { "PRJ" };
    auto const mrs =
        std::vector<ChangeRequest> { mrOf(2375, "PRJ-1999 fix", CiState::Pending, ReviewState::None) };
    auto const repos = std::vector<RepoWork> { { .repo = "contour", .changeRequests = mrs, .issues = {} } };
    auto looked = jiraOf("PRJ-1999", Progress::NotStarted).issue;
    looked.status = "In Review";
    auto const linked = std::vector<Issue> { looked };

    SECTION("looked up")
    {
        auto const items = workItems(
            WorkInput { .repos = repos, .linkedTickets = linked, .jiraProjects = projects, .runs = {} });
        REQUIRE(items.size() == 1);
        CHECK(items.front().ticketKey == "PRJ-1999");
        REQUIRE(items.front().ticket.has_value());
        CHECK(items.front().ticket->status == "In Review");
    }
    SECTION("not looked up")
    {
        auto const items = workItems(
            WorkInput { .repos = repos, .linkedTickets = {}, .jiraProjects = projects, .runs = {} });
        REQUIRE(items.size() == 1);
        CHECK(items.front().ticketKey == "PRJ-1999");
        CHECK(!items.front().ticket.has_value());
    }
}

TEST_CASE("WorkItems: an active run on the item or its ticket shows its worker", "[orchestrator][work]")
{
    auto const projects = std::vector<std::string> { "PRJ" };
    auto const mrs = std::vector<ChangeRequest> {
        mrOf(2375, "PRJ-1234: fix", CiState::Pending, ReviewState::None),
        mrOf(2380, "fix #77", CiState::Pending, ReviewState::None),
        mrOf(2390, "nothing linked", CiState::Pending, ReviewState::None),
    };
    auto const issues = std::vector<ListedIssue> { jiraOf("PRJ-1300", Progress::InProgress) };
    auto const repos =
        std::vector<RepoWork> { { .repo = "contour", .changeRequests = mrs, .issues = issues } };
    auto const itemsWith = [&](std::vector<Run> const& runs) {
        return workItems(
            WorkInput { .repos = repos, .linkedTickets = {}, .jiraProjects = projects, .runs = runs });
    };

    SECTION("on the MR itself")
    {
        auto const runs =
            std::vector<Run> { runOf("r1", "contour", ChangeRequestRef { 2375 }, "claude-work", Running {}) };
        auto const items = itemsWith(runs);
        auto const* const mr = findItem(items, "contour/!2375");
        REQUIRE(mr != nullptr);
        CHECK(mr->agentRun == "r1");
        CHECK(mr->agentWorker == "claude-work");
        CHECK(findItem(items, "contour/!2390")->agentRun.empty());
    }
    SECTION("on its linked Jira ticket, a run without a worker")
    {
        auto const runs = std::vector<Run> { runOf("r2", "contour", JiraRef { "PRJ-1234" }, "", Blocked {}) };
        auto const items = itemsWith(runs);
        auto const* const mr = findItem(items, "contour/!2375");
        REQUIRE(mr != nullptr);
        CHECK(mr->agentRun == "r2");
        CHECK(mr->agentWorker == "claude");
        CHECK(workerIdOf(runs.front()) == ImplicitWorkerId);
    }
    SECTION("on its linked forge issue")
    {
        auto const runs =
            std::vector<Run> { runOf("r3", "contour", IssueRef { 77 }, "copilot", Preparing {}) };
        auto const items = itemsWith(runs);
        auto const* const mr = findItem(items, "contour/!2380");
        REQUIRE(mr != nullptr);
        CHECK(mr->agentWorker == "copilot");
    }
    SECTION("on a ticket row")
    {
        auto const runs = std::vector<Run> { runOf("r4", "contour", JiraRef { "PRJ-1300" }, "w", Done {}) };
        auto const items = itemsWith(runs);
        auto const* const ticket = findItem(items, "contour/PRJ-1300");
        REQUIRE(ticket != nullptr);
        CHECK(ticket->agentWorker == "w");
    }
    SECTION("a finished run, or one of another repository, shows nothing")
    {
        auto const runs = std::vector<Run> {
            runOf("r5", "contour", ChangeRequestRef { 2375 }, "w", Finished {}),
            runOf("r6", "other", ChangeRequestRef { 2375 }, "w", Running {}),
        };
        auto const items = itemsWith(runs);
        CHECK(std::ranges::all_of(
            items, [](WorkItem const& item) { return item.agentRun.empty() && item.agentWorker.empty(); }));
    }
}

TEST_CASE("WorkItems: an item's key is stable and distinct", "[orchestrator][work]")
{
    auto const projects = std::vector<std::string> { "PRJ" };
    auto const mrs =
        std::vector<ChangeRequest> { mrOf(2375, "PRJ-1234: fix", CiState::Pending, ReviewState::None) };
    auto const issues = std::vector<ListedIssue> { jiraOf("PRJ-1300", Progress::NotStarted), forgeOf(78) };
    auto const repos =
        std::vector<RepoWork> { { .repo = "contour", .changeRequests = mrs, .issues = issues } };
    auto const input =
        WorkInput { .repos = repos, .linkedTickets = {}, .jiraProjects = projects, .runs = {} };
    auto const first = workItems(input);
    auto const second = workItems(input);
    CHECK(first == second);

    auto keys = std::vector<std::string> {};
    for (auto const& item: first)
        keys.push_back(workItemKey(item));
    CHECK(keys == std::vector<std::string> { "contour/!2375", "contour/PRJ-1300", "contour/#78" });
}

TEST_CASE("WorkItems: order by importance or by last update", "[orchestrator][work]")
{
    auto const repoOrder = std::vector<std::string> { "contour", "other" };
    auto const mrItem = [](std::string repo, uint64_t number, WorkGroup group, uint64_t updated) {
        auto item = WorkItem {};
        item.group = group;
        item.repo = std::move(repo);
        item.changeRequest = mrOf(number, "t", CiState::None, ReviewState::None);
        item.item = ChangeRequestRef { number };
        item.updatedWallMs = updated;
        return item;
    };
    auto const ticketItem = [](std::string repo, ListedIssue listed, WorkGroup group) {
        auto item = WorkItem {};
        item.group = group;
        item.repo = std::move(repo);
        item.item = listed.issue.item;
        if (auto const* const jira = std::get_if<JiraRef>(&listed.issue.item))
            item.ticketKey = jira->key;
        else
            item.ticketKey = "#" + std::to_string(std::get<IssueRef>(listed.issue.item).number);
        item.updatedWallMs = listed.issue.updatedWallMs;
        item.ticket = std::move(listed.issue);
        return item;
    };

    auto const items = std::vector<WorkItem> {
        mrItem("contour", 10, WorkGroup::InReview, 1000),
        mrItem("contour", 11, WorkGroup::NeedsYou, 500),
        mrItem("other", 12, WorkGroup::InReview, 3000),
        ticketItem("contour", jiraOf("PRJ-10", Progress::InProgress, 2), WorkGroup::NoMrYet),
        ticketItem("contour", jiraOf("PRJ-9", Progress::InProgress, 0), WorkGroup::NoMrYet),
        ticketItem("other", forgeOf(5), WorkGroup::NoMrYet),
        mrItem("other", 7, WorkGroup::InReview, 1000),   // ties with !10: repository order
        mrItem("contour", 9, WorkGroup::InReview, 1000), // ties with !10: number ascending
        ticketItem("contour",
                   jiraOf("PRJ-8", Progress::InProgress, 2),
                   WorkGroup::NoMrYet), // ties with PRJ-10: 8 < 10
    };

    SECTION("by importance")
    {
        CHECK(keysInOrder(items, orderOf(items, repoOrder, WorkSort::Importance))
              == std::vector<std::string> {
                  "contour/!11",
                  "other/!12",
                  "contour/!9",
                  "contour/!10",
                  "other/!7",
                  "contour/PRJ-9",
                  "other/#5",
                  "contour/PRJ-8",
                  "contour/PRJ-10",
              });
    }
    SECTION("by last update: those with a time first, newest first, then by rank")
    {
        CHECK(keysInOrder(items, orderOf(items, repoOrder, WorkSort::LastUpdate))
              == std::vector<std::string> {
                  "other/!12",
                  "contour/!9",
                  "contour/!10",
                  "other/!7",
                  "contour/!11",
                  "contour/PRJ-9",
                  "other/#5",
                  "contour/PRJ-8",
                  "contour/PRJ-10",
              });
    }
    SECTION("an MR row before a ticket row, other things equal")
    {
        auto const tied = std::vector<WorkItem> {
            ticketItem("contour", forgeOf(1), WorkGroup::NoMrYet),
            mrItem("contour", 99, WorkGroup::NoMrYet, 0),
        };
        CHECK(keysInOrder(tied, orderOf(tied, repoOrder, WorkSort::Importance))
              == std::vector<std::string> { "contour/!99", "contour/#1" });
    }
    SECTION("nothing to order")
    {
        CHECK(orderOf({}, repoOrder, WorkSort::Importance).empty());
    }
}

TEST_CASE("WorkItems: the filter matches number, key, title, repository and URL", "[orchestrator][work]")
{
    auto const projects = std::vector<std::string> { "PRJ" };
    auto const contourMrs = std::vector<ChangeRequest> { mrOf(
        2375, "PRJ-1234: Flaky test", CiState::Failing, ReviewState::None) };
    auto const otherMrs =
        std::vector<ChangeRequest> { mrOf(812, "Bump deps", CiState::Passing, ReviewState::None) };
    auto const repos = std::vector<RepoWork> {
        { .repo = "contour", .changeRequests = contourMrs, .issues = {} },
        { .repo = "other", .changeRequests = otherMrs, .issues = {} },
    };
    auto const items =
        workItems(WorkInput { .repos = repos, .linkedTickets = {}, .jiraProjects = projects, .runs = {} });
    auto const& flaky = *findItem(items, "contour/!2375");
    auto const& bump = *findItem(items, "other/!812");

    for (auto const filter:
         { ""sv, "#2375"sv, "!2375"sv, "2375"sv, "prj-1234"sv, "flaky"sv, "FLAKY"sv, "contour"sv })
    {
        INFO(filter);
        CHECK(matchesFilter(flaky, filter));
    }
    CHECK(matchesFilter(flaky, flaky.changeRequest->url));
    for (auto const filter: { "!812"sv, "#812"sv, "812"sv, "other"sv, "bump"sv })
    {
        INFO(filter);
        CHECK(matchesFilter(bump, filter));
    }
    for (auto const filter: { "nothing"sv, "!812"sv, "prj-9"sv })
    {
        INFO(filter);
        CHECK_FALSE(matchesFilter(flaky, filter));
    }
    CHECK_FALSE(matchesFilter(bump, "!2375"));
}
