// SPDX-License-Identifier: Apache-2.0
//
// The merge request list and the project in testdata/gitlab/ were printed by glab 1.119.0
// (`glab mr list --output json`, `glab repo view --output json`) and scrubbed by harness/scrub-fixture.py;
// the non-empty issue list is still the REST API's documented shape (testdata/gitlab/README.md). The
// pipelines and failed jobs were printed by `glab api` and scrubbed the same way; their `-hand` files are
// hand-made.
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <format>
#include <optional>
#include <ranges>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <orchestrator/core/Fixtures.hpp>
#include <orchestrator/core/GitLab.hpp>
#include <orchestrator/core/Model.hpp>
#include <orchestrator/core/Timestamps.hpp>

using namespace std::string_view_literals;

namespace
{

using namespace orchestrator;

/// One merge request with only the keys the mapping reads.
[[nodiscard]] std::string mergeRequest(uint64_t iid,
                                       std::string_view title = "A change",
                                       std::string_view target = "main",
                                       std::string_view status = "mergeable")
{
    return std::format(
        R"({{"iid":{},"title":"{}","web_url":"https://gitlab.example.com/group/project/-/merge_requests/{}",)"
        R"("source_branch":"topic","target_branch":"{}","draft":false,"source_project_id":7,)"
        R"("target_project_id":7,"detailed_merge_status":"{}"}})",
        iid,
        title,
        iid,
        target,
        status);
}

/// What the recorded list says of each merge request, in its order: its `detailed_merge_status` and `draft`.
struct Recorded
{
    std::string_view status;
    Readiness readiness;
};

// clang-format off
constexpr auto RecordedRows = std::array {
        Recorded { .status = "discussions_not_resolved", .readiness = Readiness::Ready },
        Recorded { .status = "ci_still_running", .readiness = Readiness::Ready },
        Recorded { .status = "draft_status", .readiness = Readiness::Draft },
        Recorded { .status = "discussions_not_resolved", .readiness = Readiness::Ready },
        Recorded { .status = "ci_still_running", .readiness = Readiness::Ready },
        Recorded { .status = "not_approved", .readiness = Readiness::Ready },
        Recorded { .status = "draft_status", .readiness = Readiness::Draft },
        Recorded { .status = "draft_status", .readiness = Readiness::Draft },
        Recorded { .status = "draft_status", .readiness = Readiness::Draft },
        Recorded { .status = "draft_status", .readiness = Readiness::Draft },
        Recorded { .status = "draft_status", .readiness = Readiness::Draft },
        Recorded { .status = "draft_status", .readiness = Readiness::Draft },
        Recorded { .status = "conflict", .readiness = Readiness::Ready },
        Recorded { .status = "draft_status", .readiness = Readiness::Draft },
        Recorded { .status = "discussions_not_resolved", .readiness = Readiness::Ready },
        Recorded { .status = "not_approved", .readiness = Readiness::Ready },
};
// clang-format on

} // namespace

TEST_CASE("GitLab: the recorded merge requests map to the model", "[gitlab]")
{
    auto const listed = gitlab::changeRequests(readFixture(FixtureRoot::Testdata, "gitlab/mrs-open.json"),
                                               OutputExtent::Whole);
    REQUIRE(listed);
    CHECK(listed->completeness == ListCompleteness::Complete);
    CHECK(listed->skipped == 0);
    // The fifteen glab printed, and the hand-made fork after them (testdata/gitlab/README.md).
    REQUIRE(listed->items.size() == 16);
    auto const& first = listed->items[0];
    CHECK(first.number == 5752);
    CHECK(first.title == "Merge request 1");
    CHECK(first.url == "https://gitlab.example.com/group/project/-/merge_requests/5752");
    CHECK(first.head == "feature/branch-1");
    CHECK(first.base == "master");
    CHECK(first.origin == HeadOrigin::SameRepository);
    // GitLab's list says nothing about the issues a merge request closes, nor which checks fail.
    CHECK(first.failingChecks.empty());
    CHECK(first.closesIssues.empty());

    REQUIRE(RecordedRows.size() == listed->items.size());
    for (auto const& [request, recorded]: std::views::zip(listed->items, RecordedRows))
    {
        CAPTURE(request.number, recorded.status);
        CHECK(request.number != 0);
        CHECK(request.readiness == recorded.readiness);
        CHECK(request.review == gitlab::reviewStateOf(recorded.status));
        CHECK(request.ci == (recorded.status == "ci_still_running" ? CiState::Pending : CiState::None));
    }
}

TEST_CASE("a fork MR is a fork", "[gitlab]")
{
    auto const listed = gitlab::changeRequests(readFixture(FixtureRoot::Testdata, "gitlab/mrs-open.json"),
                                               OutputExtent::Whole);
    REQUIRE(listed);
    REQUIRE(listed->items.size() == 16);
    CHECK(std::ranges::count(listed->items, HeadOrigin::Fork, &ChangeRequest::origin) == 1);
    auto const& fork = listed->items.back();
    CHECK(fork.number == 5800);
    CHECK(fork.origin == HeadOrigin::Fork);
    CHECK(fork.readiness == Readiness::Ready);
    CHECK(fork.review == ReviewState::Required);
}

TEST_CASE("a base that is not a ref skips the MR", "[gitlab]")
{
    auto const text = std::format(
        "[{},{},{}]", mergeRequest(1, "A change", "--upload-pack=touch x"), mergeRequest(2), mergeRequest(0));
    auto const listed = gitlab::changeRequests(text, OutputExtent::Whole);
    REQUIRE(listed);
    CHECK(listed->skipped == 2);
    REQUIRE(listed->items.size() == 1);
    CHECK(listed->items.front().number == 2);
}

TEST_CASE("review states by table", "[gitlab]")
{
    struct Row
    {
        std::string_view status;
        ReviewState expected;
    };
    constexpr auto Rows = std::array {
        Row { .status = "not_approved", .expected = ReviewState::Required },
        Row { .status = "requested_changes", .expected = ReviewState::ChangesRequested },
        Row { .status = "mergeable", .expected = ReviewState::Approved },
        Row { .status = "ci_must_pass", .expected = ReviewState::None },
        Row { .status = "draft_status", .expected = ReviewState::None },
        Row { .status = "conflict", .expected = ReviewState::None },
        Row { .status = "discussions_not_resolved", .expected = ReviewState::None },
        Row { .status = "ci_still_running", .expected = ReviewState::None },
        Row { .status = "", .expected = ReviewState::None },
        Row { .status = "something_newer", .expected = ReviewState::None },
    };
    for (auto const& row: Rows)
    {
        CAPTURE(row.status);
        CHECK(gitlab::reviewStateOf(row.status) == row.expected);
    }
    // An instance too old to print detailed_merge_status, or one printing null.
    auto const absent = gitlab::changeRequests(
        R"([{"iid":3,"title":"t","web_url":"u","source_branch":"a","target_branch":"main","draft":false,)"
        R"("source_project_id":7,"target_project_id":7,"detailed_merge_status":null}])",
        OutputExtent::Whole);
    REQUIRE(absent);
    REQUIRE(absent->items.size() == 1);
    CHECK(absent->items.front().review == ReviewState::None);
}

TEST_CASE("CI states by table", "[gitlab]")
{
    struct Row
    {
        std::string_view status;
        CiState expected;
    };
    constexpr auto Rows = std::array {
        Row { .status = "ci_still_running", .expected = CiState::Pending },
        Row { .status = "ci_must_pass", .expected = CiState::Failing },
        Row { .status = "conflict", .expected = CiState::None },
        Row { .status = "draft_status", .expected = CiState::None },
        Row { .status = "mergeable", .expected = CiState::None },
        Row { .status = "", .expected = CiState::None },
    };
    for (auto const& row: Rows)
    {
        CAPTURE(row.status);
        CHECK(gitlab::ciStateOf(row.status) == row.expected);
    }
}

TEST_CASE("100 MRs are AtLimit", "[gitlab]")
{
    auto const listed = gitlab::changeRequests(readFixture(FixtureRoot::Testdata, "gitlab/mrs-100.json"),
                                               OutputExtent::Whole);
    REQUIRE(listed);
    CHECK(listed->items.size() == ListLimit);
    CHECK(listed->completeness == ListCompleteness::AtLimit);
    auto const empty = gitlab::changeRequests(readFixture(FixtureRoot::Testdata, "gitlab/mrs-empty.json"),
                                              OutputExtent::Whole);
    REQUIRE(empty);
    CHECK(empty->items.empty());
    CHECK(empty->completeness == ListCompleteness::Complete);
}

TEST_CASE("a cut reply is Cut", "[gitlab]")
{
    auto const whole = readFixture(FixtureRoot::Testdata, "gitlab/mrs-100.json");
    auto const cut = std::string_view(whole).substr(0, whole.size() / 2);
    auto const listed = gitlab::changeRequests(cut, OutputExtent::Truncated);
    REQUIRE(listed);
    CHECK(listed->completeness == ListCompleteness::Cut);
    CHECK(!listed->items.empty());
    CHECK(listed->items.size() < ListLimit);
    // Read as whole, the same bytes are no JSON document.
    CHECK(gitlab::changeRequests(cut, OutputExtent::Whole).error() == ParseError::NotJson);
    // Cut before its first entry closes, nothing can be read.
    CHECK(gitlab::changeRequests(cut.substr(0, 20), OutputExtent::Truncated).error() == ParseError::NotJson);
}

TEST_CASE("a title's escapes are sanitised", "[gitlab]")
{
    auto const listed = gitlab::changeRequests(
        std::format("[{}]", mergeRequest(5, R"(\u001b]8;;https://evil\u0007click\u001b[2J)")),
        OutputExtent::Whole);
    REQUIRE(listed);
    REQUIRE(listed->items.size() == 1);
    auto const& title = listed->items.front().title;
    CHECK(!title.contains('\x1b'));
    CHECK(!title.contains('\x07'));
    CHECK(title.contains("click"));
}

TEST_CASE("GitLab: issues and the default branch", "[gitlab]")
{
    auto const issues =
        gitlab::issues(readFixture(FixtureRoot::Testdata, "gitlab/issues-open.json"), OutputExtent::Whole);
    REQUIRE(issues);
    REQUIRE(issues->items.size() == 2);
    CHECK(issues->items[0].item == ItemRef { IssueRef { .number = 12 } });
    CHECK(issues->items[0].title == "Crash when resizing during output");
    CHECK(issues->items[0].url == "https://gitlab.example.com/group/project/-/issues/12");
    CHECK(gitlab::issues(R"([{"title":"no iid"}])", OutputExtent::Whole)->skipped == 1);
    // `-F` is `--output-format` on `glab issue list`: `-F json` prints a sentence, not JSON.
    CHECK(gitlab::issues("No open issues match your search in group/project.\n\n\n", OutputExtent::Whole)
              .error()
          == ParseError::NotJson);

    CHECK(gitlab::defaultBranch(readFixture(FixtureRoot::Testdata, "gitlab/default-branch.json")) == "main");
    CHECK(gitlab::defaultBranch(R"({"default_branch":"--orphan"})").error() == ParseError::WrongShape);
    CHECK(gitlab::defaultBranch(R"({"default_branch":)").error() == ParseError::NotJson);
}

TEST_CASE("GitLab: an empty issue list", "[gitlab]")
{
    auto const listed =
        gitlab::issues(readFixture(FixtureRoot::Testdata, "gitlab/issues-empty.json"), OutputExtent::Whole);
    REQUIRE(listed);
    CHECK(listed->items.empty());
    CHECK(listed->skipped == 0);
    CHECK(listed->completeness == ListCompleteness::Complete);
}

TEST_CASE("GitLab: pipeline statuses by table", "[gitlab]")
{
    using Row = std::pair<std::string_view, CiState>;
    for (auto const [text, state]: std::array { Row { "success"sv, CiState::Passing },
                                                Row { "skipped"sv, CiState::Passing },
                                                Row { "failed"sv, CiState::Failing },
                                                Row { "canceled"sv, CiState::Failing },
                                                Row { "running"sv, CiState::Pending },
                                                Row { "manual"sv, CiState::Pending },
                                                Row { "new_value"sv, CiState::Pending } })
    {
        CAPTURE(text);
        CHECK(gitlab::pipelineStateOf(text) == state);
    }
}

TEST_CASE("GitLab: the recorded pipelines and failed jobs map", "[gitlab]")
{
    auto const pipes =
        gitlab::pipelines(readFixture(FixtureRoot::Testdata, "gitlab/pipelines.json"), OutputExtent::Whole);
    auto const jobs = gitlab::failedJobs(readFixture(FixtureRoot::Testdata, "gitlab/jobs-failed.json"),
                                         OutputExtent::Whole);
    REQUIRE(pipes.has_value());
    REQUIRE(jobs.has_value());
    CHECK_FALSE(pipes->empty());
    CHECK_FALSE(jobs->empty());
    CHECK(std::ranges::all_of(*pipes, [](PipelineInfo const& p) { return p.id != 0 && !p.sha.empty(); }));
    CHECK(std::ranges::all_of(
        *jobs, [](FailedJob const& j) { return j.pipelineId != 0 && j.name.starts_with("job-"); }));
}

TEST_CASE("GitLab: pipelines and failed jobs leave out what they cannot place", "[gitlab]")
{
    auto const pipes =
        gitlab::pipelines(R"([{"id":3,"sha":"abc","ref":"refs/merge-requests/7/merge","status":"failed"},)"
                          R"({"sha":"abc","status":"success"},{"id":4,"status":"success"}])",
                          OutputExtent::Whole);
    REQUIRE(pipes.has_value());
    CHECK(*pipes
          == std::vector { PipelineInfo {
              .id = 3, .sha = "abc", .ref = "refs/merge-requests/7/merge", .ci = CiState::Failing } });
    CHECK(gitlab::pipelines(R"({"id":3})", OutputExtent::Whole).error() == ParseError::WrongShape);
    CHECK(
        gitlab::pipelines(R"([{"id":3,"sha":"abc","status":"failed"},{"id")", OutputExtent::Truncated)->size()
        == 1);

    auto const jobs = gitlab::failedJobs(
        R"([{"name":"a\u001b[31m","pipeline":{"id":3}},{"name":"b"},{"name":"c","pipeline":null},)"
        R"({"name":"","pipeline":{"id":3}}])",
        OutputExtent::Whole);
    REQUIRE(jobs.has_value());
    REQUIRE(jobs->size() == 1);
    CHECK(jobs->front().pipelineId == 3);
    CHECK_FALSE(jobs->front().name.contains('\x1b'));
    CHECK(gitlab::failedJobs("not json", OutputExtent::Whole).error() == ParseError::NotJson);
}

TEST_CASE("GitLab: an MR's newest pipeline of its sha decides, and its failed jobs are named", "[gitlab]")
{
    auto listed = gitlab::changeRequests(readFixture(FixtureRoot::Testdata, "gitlab/mrs-open.json"),
                                         OutputExtent::Whole);
    auto const pipes = gitlab::pipelines(readFixture(FixtureRoot::Testdata, "gitlab/pipelines-hand.json"),
                                         OutputExtent::Whole);
    auto const jobs = gitlab::failedJobs(readFixture(FixtureRoot::Testdata, "gitlab/jobs-failed-hand.json"),
                                         OutputExtent::Whole);
    REQUIRE(listed.has_value());
    REQUIRE(pipes.has_value());
    REQUIRE(jobs.has_value());
    auto const mrs = gitlab::enriched(listed->items, std::span { *pipes }, std::span { *jobs });
    REQUIRE(mrs.size() == listed->items.size());
    CHECK(mrs[0].ci == CiState::Failing);
    CHECK(mrs[0].failingChecks == std::vector<std::string> { "test-windows", "lint" }); // `lint` ran twice
    CHECK(mrs[1].ci == CiState::Passing);
    CHECK(mrs[2].ci == CiState::Pending);
    CHECK(mrs[0].review == listed->items[0].review); // reviewStateOf(), unchanged by the join
    CHECK(mrs[0].ciSource == StateSource::Complete);
    CHECK(mrs[0].reviewSource == StateSource::Inferred); // a pipeline says nothing about review
    CHECK(mrs[3].ciSource == StateSource::Inferred);     // no pipeline of its sha
    CHECK(mrs[3] == listed->items[3]);
    CHECK(gitlab::enriched(listed->items, std::nullopt, std::nullopt)[0].ciSource == StateSource::Inferred);
    CHECK(gitlab::enriched(listed->items, std::nullopt, std::nullopt) == listed->items);
    CHECK(gitlab::enriched(listed->items, std::span { *pipes }, std::nullopt)[0].failingChecks.empty());
}

TEST_CASE("GitLab: the REST list carries conflicts, update time and the head commit", "[gitlab]")
{
    auto const listed = gitlab::changeRequests(readFixture(FixtureRoot::Testdata, "gitlab/mrs-open.json"),
                                               OutputExtent::Whole);
    REQUIRE(listed.has_value());
    auto const byIid = [&](uint64_t iid) {
        return *std::ranges::find(listed->items, iid, &ChangeRequest::number);
    };
    CHECK(byIid(5696).conflicts == Conflicts::Conflicting); // detailed_merge_status `conflict`
    CHECK(byIid(5729).conflicts == Conflicts::Conflicting); // `draft_status`, has_conflicts true
    CHECK(byIid(5752).conflicts == Conflicts::None);
    CHECK(byIid(5696).updatedWallMs == unixMsOf("2026-11-20T10:24:50.48+01:00"));
    CHECK(byIid(5752).ciSource == StateSource::Inferred);
    CHECK(byIid(5752).reviewSource == StateSource::Inferred);
    CHECK(std::ranges::all_of(
        listed->items, [](ChangeRequest const& mr) { return mr.updatedWallMs != 0 && !mr.headSha.empty(); }));
}

TEST_CASE("GitLab: a merge request's pipelines are found by its head commit or its merge request refs",
          "[gitlab]")
{
    auto const mr = [](uint64_t number, std::string_view sha) {
        return ChangeRequest { .number = number,
                               .title = "Example title",
                               .url = {},
                               .head = "topic",
                               .base = "main",
                               .ci = CiState::None,
                               .failingChecks = {},
                               .review = ReviewState::Required,
                               .closesIssues = {},
                               .ciSource = StateSource::Inferred,
                               .reviewSource = StateSource::Inferred,
                               .headSha = std::string(sha) };
    };
    auto const listed = std::vector {
        mr(7, "aaa"),  // only a merged-results pipeline, on another commit
        mr(8, "bbb"),  // only a pipeline for its head ref, on another commit
        mr(9, "ccc"),  // its head commit and its merge ref: the merge ref's pipeline is newer
        mr(10, "ddd"), // the same, but the head commit's pipeline is newer
        mr(11, "eee"), // only refs of other merge requests
    };
    auto const pipelines = std::vector {
        PipelineInfo { .id = 10, .sha = "m7", .ref = "refs/merge-requests/7/merge", .ci = CiState::Failing },
        PipelineInfo { .id = 11, .sha = "zzz", .ref = "refs/merge-requests/8/head", .ci = CiState::Passing },
        PipelineInfo { .id = 20, .sha = "ccc", .ref = "feature/x", .ci = CiState::Passing },
        PipelineInfo { .id = 21, .sha = "m9", .ref = "refs/merge-requests/9/merge", .ci = CiState::Failing },
        PipelineInfo {
            .id = 29, .sha = "m10", .ref = "refs/merge-requests/10/merge", .ci = CiState::Passing },
        PipelineInfo { .id = 30, .sha = "ddd", .ref = "feature/y", .ci = CiState::Failing },
        PipelineInfo {
            .id = 40, .sha = "m111", .ref = "refs/merge-requests/111/merge", .ci = CiState::Failing },
        PipelineInfo { .id = 41, .sha = "m1", .ref = "refs/merge-requests/1/head", .ci = CiState::Failing },
        PipelineInfo {
            .id = 42, .sha = "t11", .ref = "refs/merge-requests/11/train", .ci = CiState::Failing },
    };
    auto const jobs = std::vector {
        FailedJob { .pipelineId = 21, .name = "e2e" },
        FailedJob { .pipelineId = 20, .name = "stale" },
        FailedJob { .pipelineId = 10, .name = "merged-build" },
    };
    auto const mrs = gitlab::enriched(listed, std::span { pipelines }, std::span { jobs });
    REQUIRE(mrs.size() == listed.size());

    CHECK(mrs[0].ci == CiState::Failing);
    CHECK(mrs[0].ciSource == StateSource::Complete);
    CHECK(mrs[0].failingChecks == std::vector<std::string> { "merged-build" });

    CHECK(mrs[1].ci == CiState::Passing);
    CHECK(mrs[1].ciSource == StateSource::Complete);

    CHECK(mrs[2].ci == CiState::Failing);
    CHECK(mrs[2].failingChecks == std::vector<std::string> { "e2e" });

    CHECK(mrs[3].ci == CiState::Failing);
    CHECK(mrs[3].failingChecks.empty());

    CHECK(mrs[4] == listed[4]);
    CHECK(mrs[4].ciSource == StateSource::Inferred);
}

TEST_CASE("GitLab: the recorded merged-results pipelines keep their merge request refs", "[gitlab]")
{
    auto const pipes =
        gitlab::pipelines(readFixture(FixtureRoot::Testdata, "gitlab/pipelines.json"), OutputExtent::Whole);
    REQUIRE(pipes.has_value());
    CHECK(std::ranges::any_of(*pipes, [](PipelineInfo const& p) {
        return p.ref.starts_with("refs/merge-requests/") && p.ref.ends_with("/merge");
    }));
    CHECK(std::ranges::any_of(*pipes, [](PipelineInfo const& p) {
        return p.ref.starts_with("refs/merge-requests/") && p.ref.ends_with("/head");
    }));
}
