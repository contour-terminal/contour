// SPDX-License-Identifier: Apache-2.0
//
// UNVERIFIED AGAINST REAL OUTPUT. The inputs below and in testdata/gitlab/ follow the GitLab REST API's
// documented response shapes, which `glab … -F json` is documented to print:
//   https://docs.gitlab.com/api/merge_requests/#list-merge-requests
//   https://docs.gitlab.com/api/issues/#list-issues
//   https://docs.gitlab.com/api/projects/#get-a-single-project
//   https://gitlab.com/gitlab-org/cli/-/blob/main/docs/source/mr/list.md
// No `glab` printed them (testdata/gitlab/README.md); the provider row stays Unmeasured until one does.
#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstddef>
#include <format>
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

} // namespace

TEST_CASE("GitLab: the documented merge requests map to the model", "[gitlab]")
{
    auto const listed = gitlab::changeRequests(readFixture(FixtureRoot::Testdata, "gitlab/mrs-open.json"),
                                               OutputExtent::Whole);
    REQUIRE(listed);
    CHECK(listed->completeness == ListCompleteness::Complete);
    CHECK(listed->skipped == 0);
    REQUIRE(listed->items.size() == 3);
    auto const& first = listed->items[0];
    CHECK(first.number == 41);
    CHECK(first.title == "Fix the scrollback reflow");
    CHECK(first.url == "https://gitlab.example.com/group/project/-/merge_requests/41");
    CHECK(first.head == "fix/reflow");
    CHECK(first.base == "main");
    CHECK(first.readiness == Readiness::Ready);
    CHECK(first.origin == HeadOrigin::SameRepository);
    CHECK(first.review == ReviewState::Approved);
    // GitLab's list says nothing about pipelines or the issues a merge request closes.
    CHECK(first.ci == CiState::None);
    CHECK(first.failingChecks.empty());
    CHECK(first.closesIssues.empty());
    CHECK(listed->items[2].base == "release/1.2");
    CHECK(listed->items[2].review == ReviewState::ChangesRequested);
}

TEST_CASE("a fork MR is a fork", "[gitlab]")
{
    auto const listed = gitlab::changeRequests(readFixture(FixtureRoot::Testdata, "gitlab/mrs-open.json"),
                                               OutputExtent::Whole);
    REQUIRE(listed);
    REQUIRE(listed->items.size() == 3);
    auto const& fork = listed->items[1];
    CHECK(fork.number == 40);
    CHECK(fork.origin == HeadOrigin::Fork);
    CHECK(fork.readiness == Readiness::Draft);
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

    CHECK(gitlab::defaultBranch(readFixture(FixtureRoot::Testdata, "gitlab/default-branch.json")) == "main");
    CHECK(gitlab::defaultBranch(R"({"default_branch":"--orphan"})").error() == ParseError::WrongShape);
    CHECK(gitlab::defaultBranch(R"({"default_branch":)").error() == ParseError::NotJson);
}
