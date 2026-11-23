// SPDX-License-Identifier: Apache-2.0
//
// The merge request list and the project in testdata/gitlab/ were printed by glab 1.119.0
// (`glab mr list --output json`, `glab repo view --output json`) and scrubbed by harness/scrub-fixture.py;
// the non-empty issue list is still the REST API's documented shape (testdata/gitlab/README.md).
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <format>
#include <ranges>
#include <string>
#include <string_view>

#include <orchestrator/core/Fixtures.hpp>
#include <orchestrator/core/GitLab.hpp>
#include <orchestrator/core/Model.hpp>

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
