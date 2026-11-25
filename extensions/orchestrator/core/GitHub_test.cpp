// SPDX-License-Identifier: Apache-2.0
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <format>
#include <memory>
#include <optional>
#include <ranges>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

#include <glaze/glaze.hpp>
#include <orchestrator/core/Commands.hpp>
#include <orchestrator/core/Fixtures.hpp>
#include <orchestrator/core/GitHub.hpp>
#include <orchestrator/core/Model.hpp>

using namespace std::string_view_literals;

namespace
{

using namespace orchestrator;
using github::Check;

constexpr auto Success =
    Check { .typeName = "CheckRun", .status = "COMPLETED", .conclusion = "SUCCESS", .state = "" };
constexpr auto InProgress =
    Check { .typeName = "CheckRun", .status = "IN_PROGRESS", .conclusion = "", .state = "" };
constexpr auto Failure =
    Check { .typeName = "CheckRun", .status = "COMPLETED", .conclusion = "FAILURE", .state = "" };

/// Builds a check run that completed with @p conclusion.
constexpr Check completed(std::string_view conclusion)
{
    return Check { .typeName = "CheckRun", .status = "COMPLETED", .conclusion = conclusion, .state = "" };
}

/// Builds a commit status in @p state.
constexpr Check status(std::string_view state)
{
    return Check { .typeName = "StatusContext", .status = "", .conclusion = "", .state = state };
}

constexpr auto NoChecks = std::array<Check, 0> {};
constexpr auto CompletedSuccess = std::array { Success };
constexpr auto CompletedSkipped = std::array { completed("SKIPPED") };
constexpr auto CompletedNeutral = std::array { completed("NEUTRAL") };
constexpr auto RunInProgress = std::array { InProgress };
constexpr auto RunQueued =
    std::array { Check { .typeName = "CheckRun", .status = "QUEUED", .conclusion = "", .state = "" } };
constexpr auto StatusPending = std::array { status("PENDING") };
constexpr auto StatusSuccess = std::array { status("SUCCESS") };
constexpr auto CompletedFailure = std::array { Failure };
constexpr auto CompletedTimedOut = std::array { completed("TIMED_OUT") };
constexpr auto CompletedCancelled = std::array { completed("CANCELLED") };
constexpr auto CompletedActionRequired = std::array { completed("ACTION_REQUIRED") };
constexpr auto StatusError = std::array { status("ERROR") };
constexpr auto SuccessAndInProgress = std::array { Success, InProgress };
constexpr auto InProgressAndFailure = std::array { InProgress, Failure };
constexpr auto CompletedUnknown = std::array { completed("SOMETHING_NEW") };

struct CiRow
{
    std::string_view label;
    std::span<Check const> checks;
    CiState expected;
};

constexpr auto CiRows = std::array {
    CiRow { .label = "none", .checks = NoChecks, .expected = CiState::None },
    CiRow { .label = "SUCCESS", .checks = CompletedSuccess, .expected = CiState::Passing },
    CiRow { .label = "SKIPPED", .checks = CompletedSkipped, .expected = CiState::Passing },
    CiRow { .label = "NEUTRAL", .checks = CompletedNeutral, .expected = CiState::Passing },
    CiRow { .label = "IN_PROGRESS", .checks = RunInProgress, .expected = CiState::Pending },
    CiRow { .label = "QUEUED", .checks = RunQueued, .expected = CiState::Pending },
    CiRow { .label = "status PENDING", .checks = StatusPending, .expected = CiState::Pending },
    CiRow { .label = "status SUCCESS", .checks = StatusSuccess, .expected = CiState::Passing },
    CiRow { .label = "FAILURE", .checks = CompletedFailure, .expected = CiState::Failing },
    CiRow { .label = "TIMED_OUT", .checks = CompletedTimedOut, .expected = CiState::Failing },
    CiRow { .label = "CANCELLED", .checks = CompletedCancelled, .expected = CiState::Failing },
    CiRow { .label = "ACTION_REQUIRED", .checks = CompletedActionRequired, .expected = CiState::Failing },
    CiRow { .label = "status ERROR", .checks = StatusError, .expected = CiState::Failing },
    CiRow { .label = "SUCCESS + IN_PROGRESS", .checks = SuccessAndInProgress, .expected = CiState::Pending },
    CiRow { .label = "IN_PROGRESS + FAILURE", .checks = InProgressAndFailure, .expected = CiState::Failing },
    CiRow { .label = "SOMETHING_NEW", .checks = CompletedUnknown, .expected = CiState::Pending },
};

struct ReviewRow
{
    std::string_view decision;
    ReviewState expected;
};

constexpr auto ReviewRows = std::array {
    ReviewRow { .decision = "", .expected = ReviewState::None },
    ReviewRow { .decision = "REVIEW_REQUIRED", .expected = ReviewState::Required },
    ReviewRow { .decision = "APPROVED", .expected = ReviewState::Approved },
    ReviewRow { .decision = "CHANGES_REQUESTED", .expected = ReviewState::ChangesRequested },
    ReviewRow { .decision = "SOMETHING_NEW", .expected = ReviewState::Required },
};

/// What prs-open.json, recorded from `gh` 2.97.0 against contour-terminal/contour and projected by the
/// `gh-prs` filter (testdata/github/README.md), holds per pull request.
struct RecordedPullRequest
{
    uint64_t number;
    Readiness readiness;
    HeadOrigin origin;
    CiState ci;
    std::size_t failingChecks;
    std::span<uint64_t const> closesIssues;
};

constexpr auto ClosesNothing = std::array<uint64_t, 0> {};
constexpr auto Closes605 = std::array<uint64_t, 1> { 605 };

constexpr auto RecordedPullRequests = std::array {
    RecordedPullRequest { .number = 2375,
                          .readiness = Readiness::Ready,
                          .origin = HeadOrigin::SameRepository,
                          .ci = CiState::Pending,
                          .failingChecks = 0,
                          .closesIssues = ClosesNothing },
    RecordedPullRequest { .number = 2369,
                          .readiness = Readiness::Ready,
                          .origin = HeadOrigin::SameRepository,
                          .ci = CiState::Passing,
                          .failingChecks = 0,
                          .closesIssues = ClosesNothing },
    RecordedPullRequest { .number = 2346,
                          .readiness = Readiness::Draft,
                          .origin = HeadOrigin::Fork,
                          .ci = CiState::Passing,
                          .failingChecks = 0,
                          .closesIssues = ClosesNothing },
    RecordedPullRequest { .number = 2114,
                          .readiness = Readiness::Draft,
                          .origin = HeadOrigin::Fork,
                          .ci = CiState::Failing,
                          .failingChecks = 10,
                          .closesIssues = ClosesNothing },
    RecordedPullRequest { .number = 1847,
                          .readiness = Readiness::Draft,
                          .origin = HeadOrigin::SameRepository,
                          .ci = CiState::Failing,
                          .failingChecks = 3,
                          .closesIssues = Closes605 },
};

constexpr auto RecordedIssueNumbers = std::array<uint64_t, 5> { 2356, 2338, 2253, 2163, 2162 };

/// @return A `gh-prs` reply of @p count well-formed pull requests, in the command's positional projection.
std::string generatedPullRequests(std::size_t count)
{
    auto text = std::string { "[" };
    for (auto const index: std::views::iota(std::size_t { 0 }, count))
        text += std::format(
            R"({}[{},"t","u","h","master",false,false,"",[],[]])", index == 0 ? "" : ",", index + 1);
    return text + "]";
}

/// @return A `gh issue list` reply of @p count well-formed issues.
std::string generatedIssues(std::size_t count)
{
    auto text = std::string { "[" };
    for (auto const index: std::views::iota(std::size_t { 0 }, count))
        text += std::format(R"({}{{"number":{},"title":"t","url":"u"}})", index == 0 ? "" : ",", index + 1);
    return text + "]";
}

/// @return How many members the array projection that follows @p marker in @p filter builds; 0 if
///         @p marker is not followed by one. A jq filter of the `gh-prs` kind holds no string literal.
std::size_t projectedArity(std::string_view filter, std::string_view marker)
{
    auto const at = filter.find(marker);
    if (at == std::string_view::npos || !filter.substr(at + marker.size()).starts_with('['))
        return 0;
    auto depth = 0;
    auto commas = std::size_t { 0 };
    for (auto const c: filter.substr(at + marker.size()))
    {
        if (c == '[')
            ++depth;
        else if (c == ']' && --depth == 0)
            break;
        else if (c == ',' && depth == 1)
            ++commas;
    }
    return commas + 1;
}

/// @return The number of each change request, in order.
std::vector<uint64_t> numbersOf(std::span<ChangeRequest const> changeRequests)
{
    auto numbers = std::vector<uint64_t> {};
    for (auto const& changeRequest: changeRequests)
        numbers.push_back(changeRequest.number);
    return numbers;
}

/// @return Change request @p number, which closes @p issues.
ChangeRequest changeRequestClosing(uint64_t number, std::vector<uint64_t> issues)
{
    auto changeRequest = ChangeRequest {};
    changeRequest.number = number;
    changeRequest.closesIssues = std::move(issues);
    return changeRequest;
}

/// @return An open forge issue numbered @p number.
Issue issueNumbered(uint64_t number)
{
    return Issue { .item = IssueRef { .number = number }, .title = "t", .url = "u" };
}

} // namespace

TEST_CASE("GitHub.ciStateOf", "[github]")
{
    for (auto const& row: CiRows)
    {
        CAPTURE(row.label);
        CHECK(github::ciStateOf(row.checks) == row.expected);
    }
}

TEST_CASE("GitHub.reviewStateOf", "[github]")
{
    for (auto const& row: ReviewRows)
    {
        CAPTURE(row.decision);
        CHECK(github::reviewStateOf(row.decision) == row.expected);
    }
}

TEST_CASE("GitHub.changeRequests.recorded", "[github]")
{
    auto const listed = github::changeRequests(readFixture(FixtureRoot::Testdata, "github/prs-open.json"),
                                               OutputExtent::Whole);
    REQUIRE(listed.has_value());
    CHECK(listed->skipped == 0);
    CHECK(listed->completeness == ListCompleteness::Complete);
    REQUIRE(listed->items.size() == RecordedPullRequests.size());
    for (auto const index: std::views::iota(std::size_t { 0 }, RecordedPullRequests.size()))
    {
        auto const& expected = RecordedPullRequests[index];
        auto const& actual = listed->items[index];
        CAPTURE(expected.number);
        CHECK(actual.number == expected.number);
        CHECK(actual.readiness == expected.readiness);
        CHECK(actual.origin == expected.origin);
        CHECK(actual.ci == expected.ci);
        CHECK(actual.failingChecks.size() == expected.failingChecks);
        CHECK(std::ranges::equal(actual.closesIssues, expected.closesIssues));
        CHECK(actual.review == ReviewState::None);
        CHECK(actual.base == "master");
        CHECK(actual.url
              == std::format("https://github.com/contour-terminal/contour/pull/{}", expected.number));
        CHECK_FALSE(actual.title.empty());
        CHECK_FALSE(actual.head.empty());
    }
    CHECK(std::ranges::contains(listed->items[4].failingChecks, "Check clang-tidy"sv));
    for (auto const& pr: listed->items)
        CHECK(pr.updatedWallMs != 0);
}

TEST_CASE("GitHub.changeRequests.largest", "[github]")
{
    // prs-100.json is prs-open.json scaled by harness/scale-fixture.py to the 100 entries `gh-prs` asks for:
    // the largest reply the command prints, and the one harness/ParseHarness.cpp is timed on under the
    // interpreter. It fits the host's 1 MiB per-stream cap, so it parses whole, at the list limit.
    auto const reply = readFixture(FixtureRoot::Testdata, "github/prs-100.json");
    CHECK(reply.size() <= std::size_t { 1024 } * 1024);
    auto const listed = github::changeRequests(reply, OutputExtent::Whole);
    REQUIRE(listed.has_value());
    CHECK(listed->skipped == 0);
    CHECK(listed->completeness == ListCompleteness::AtLimit);
    REQUIRE(listed->items.size() == 100);
    for (auto const index: std::views::iota(std::size_t { 0 }, listed->items.size()))
    {
        auto const& expected = RecordedPullRequests[index % RecordedPullRequests.size()];
        auto const& actual = listed->items[index];
        CAPTURE(index);
        CHECK(actual.number == 100000 + index);
        CHECK(actual.ci == expected.ci);
        CHECK(actual.failingChecks.size() == expected.failingChecks);
    }
}

TEST_CASE("GitHub.changeRequests.fixtures_have_the_filters_shape", "[github]")
{
    // The projected fixtures were made with the declared filter (testdata/github/README.md); a filter that
    // changes without them changes the arity of a row, a closing issue or a check.
    auto const* const command = findCommand("gh-prs");
    REQUIRE(command != nullptr);
    auto const filter = command->argv.back();
    auto const rowArity = projectedArity(filter, "[.[]|");
    auto const issueArity = projectedArity(filter, "[.closingIssuesReferences[]|");
    auto const checkArity = projectedArity(filter, "[.statusCheckRollup[]|");
    CHECK(rowArity == 12);
    CHECK(issueArity == 2);
    CHECK(checkArity == 6);
    for (auto const name: { "prs-open.json"sv, "prs-mixed-checks.json"sv, "prs-100.json"sv })
    {
        CAPTURE(name);
        auto rows = std::vector<std::vector<glz::generic>> {};
        REQUIRE_FALSE(
            glz::read_json(rows, readFixture(FixtureRoot::Testdata, "github/" + std::string(name))));
        REQUIRE_FALSE(rows.empty());
        for (auto const& row: rows)
        {
            REQUIRE(row.size() == rowArity);
            for (auto const& issue: row[8].get_array())
                CHECK(issue.get_array().size() == issueArity);
            for (auto const& check: row[9].get_array())
                CHECK(check.get_array().size() == checkArity);
        }
    }
}

TEST_CASE("GitHub.changeRequests.empty", "[github]")
{
    auto const listed = github::changeRequests(readFixture(FixtureRoot::Testdata, "github/prs-empty.json"),
                                               OutputExtent::Whole);
    REQUIRE(listed.has_value());
    CHECK(listed->items.empty());
    CHECK(listed->skipped == 0);
    CHECK(listed->completeness == ListCompleteness::Complete);
}

TEST_CASE("GitHub.changeRequests.mixed_checks", "[github]")
{
    auto const listed = github::changeRequests(
        readFixture(FixtureRoot::Testdata, "github/prs-mixed-checks.json"), OutputExtent::Whole);
    REQUIRE(listed.has_value());
    // The second entry's base is "--upload-pack=x": it must never reach a command line.
    CHECK(listed->skipped == 1);
    REQUIRE(listed->items.size() == 1);
    auto const& pr = listed->items.front();
    CHECK(numbersOf(listed->items) == std::vector<uint64_t> { 2375 });
    CHECK(pr.ci == CiState::Failing);
    // A commit status is named by its context.
    CHECK(pr.failingChecks == std::vector<std::string> { "Check clang-tidy", "ci/legacy-lint" });
    CHECK(pr.review == ReviewState::ChangesRequested);
    CHECK(pr.origin == HeadOrigin::Fork);
    CHECK(pr.readiness == Readiness::Ready);
    CHECK(pr.closesIssues == std::vector<uint64_t> { 2301 });
    CHECK_FALSE(pr.title.contains('\x1b'));
    CHECK(pr.title == "[31mSay why the stdio relay failed[0m");
}

TEST_CASE("GitHub: mergeable CONFLICTING is a conflict, and updatedAt the update time", "[github]")
{
    auto const listed = github::changeRequests(
        readFixture(FixtureRoot::Testdata, "github/prs-mixed-checks.json"), OutputExtent::Whole);
    REQUIRE(listed.has_value());
    // prs-mixed-checks: #2375 CONFLICTING (its #2376 row is skipped for its base, as before).
    REQUIRE(listed->items.size() == 1);
    CHECK(listed->items[0].conflicts == Conflicts::Conflicting);
    CHECK(listed->items[0].updatedWallMs == 1'794'571'200'000); // "2026-11-13T12:00:00Z"
    CHECK(listed->items[0].ciSource == StateSource::Complete);
    CHECK(listed->items[0].reviewSource == StateSource::Complete);
    // prs-open: every row MERGEABLE, so none conflicts.
    auto const open = github::changeRequests(readFixture(FixtureRoot::Testdata, "github/prs-open.json"),
                                             OutputExtent::Whole);
    REQUIRE(open.has_value());
    CHECK(std::ranges::none_of(open->items,
                               [](auto const& pr) { return pr.conflicts == Conflicts::Conflicting; }));
}

TEST_CASE("GitHub.changeRequests.completeness", "[github]")
{
    struct Row
    {
        std::size_t count;
        ListCompleteness expected;
    };
    static constexpr auto Rows = std::array {
        Row { .count = ListLimit - 1, .expected = ListCompleteness::Complete },
        Row { .count = ListLimit, .expected = ListCompleteness::AtLimit },
    };
    for (auto const& row: Rows)
    {
        CAPTURE(row.count);
        auto const prs = github::changeRequests(generatedPullRequests(row.count), OutputExtent::Whole);
        REQUIRE(prs.has_value());
        CHECK(prs->items.size() == row.count);
        CHECK(prs->completeness == row.expected);
        auto const issues = github::issues(generatedIssues(row.count), OutputExtent::Whole);
        REQUIRE(issues.has_value());
        CHECK(issues->items.size() == row.count);
        CHECK(issues->completeness == row.expected);
    }
}

TEST_CASE("GitHub.refused_output", "[github]")
{
    auto const recorded = readFixture(FixtureRoot::Testdata, "github/prs-open.json");
    struct Row
    {
        std::string_view label;
        std::string_view text;
        ParseError expected;
    };
    auto const rows = std::array {
        Row { .label = "not json", .text = "not json", .expected = ParseError::NotJson },
        Row { .label = "empty", .text = "", .expected = ParseError::NotJson },
        Row { .label = "an object", .text = R"({"a":1})", .expected = ParseError::WrongShape },
        Row { .label = "bytes after the document", .text = "[]x", .expected = ParseError::NotJson },
        Row { .label = "two documents", .text = "[] []", .expected = ParseError::NotJson },
        Row { .label = "a cut-off reply",
              .text = std::string_view { recorded }.substr(0, recorded.size() / 2),
              .expected = ParseError::NotJson },
    };
    for (auto const& row: rows)
    {
        CAPTURE(row.label);
        auto const prs = github::changeRequests(row.text, OutputExtent::Whole);
        REQUIRE_FALSE(prs.has_value());
        CHECK(prs.error() == row.expected);
        auto const issues = github::issues(row.text, OutputExtent::Whole);
        REQUIRE_FALSE(issues.has_value());
        CHECK(issues.error() == row.expected);
    }

    // Valid JSON of the wrong shape: a pull request is a positional row, an issue an object.
    struct ShapeRow
    {
        std::string_view label;
        std::string_view pullRequests;
        std::string_view issues;
    };
    auto const shapeRows = std::array {
        ShapeRow { .label = "a number above uint64",
                   .pullRequests = R"([[18446744073709551616,"t","u","h","master",false,false,"",[],[]]])",
                   .issues = R"([{"number":18446744073709551616,"title":"t","url":"u"}])" },
        ShapeRow { .label = "a negative number",
                   .pullRequests = R"([[-1,"t","u","h","master",false,false,"",[],[]]])",
                   .issues = R"([{"number":-1,"title":"t","url":"u"}])" },
        ShapeRow { .label = "a number as a string",
                   .pullRequests = R"([["7","t","u","h","master",false,false,"",[],[]]])",
                   .issues = R"([{"number":"7","title":"t","url":"u"}])" },
        ShapeRow { .label = "the object gh prints without the projection",
                   .pullRequests = R"([{"number":7,"title":"t","url":"u","baseRefName":"master"}])",
                   .issues = R"([7])" },
        ShapeRow { .label = "a null where a check takes none",
                   .pullRequests =
                       R"([[7,"t","u","h","master",false,false,"",[],[[null,null,null,null,null,null]]]])",
                   .issues = R"([null])" },
    };
    for (auto const& row: shapeRows)
    {
        CAPTURE(row.label);
        auto const prs = github::changeRequests(row.pullRequests, OutputExtent::Whole);
        REQUIRE_FALSE(prs.has_value());
        CHECK(prs.error() == ParseError::WrongShape);
        auto const issues = github::issues(row.issues, OutputExtent::Whole);
        REQUIRE_FALSE(issues.has_value());
        CHECK(issues.error() == ParseError::WrongShape);
    }
}

TEST_CASE("GitHub.changeRequests.short_row", "[github]")
{
    // As an absent key did in gh's objects, a member a row ends before keeps its default: no checks, no
    // closed issues, not a draft. A row without its base is left out, the base being no Ref.
    auto const listed = github::changeRequests(
        R"([[7,"t","https://github.com/o/r/pull/7","h","master"],[8,"t"]])", OutputExtent::Whole);
    REQUIRE(listed.has_value());
    CHECK(listed->skipped == 1);
    REQUIRE(listed->items.size() == 1);
    auto const& pr = listed->items.front();
    CHECK(pr.number == 7);
    CHECK(pr.readiness == Readiness::Ready);
    CHECK(pr.ci == CiState::None);
    CHECK(pr.closesIssues.empty());
}

TEST_CASE("GitHub.cut_output", "[github]")
{
    auto const recorded = readFixture(FixtureRoot::Testdata, "github/prs-open.json");
    auto const lastRow = recorded.rfind(std::format("[{},", RecordedPullRequests.back().number));
    REQUIRE(lastRow != std::string::npos);
    auto const many = generatedPullRequests(ListLimit);
    enum class Outcome : uint8_t
    {
        Refused, ///< No entry before the cut was whole.
        Prefix,  ///< The entries before the cut, marked Cut.
    };
    struct Row
    {
        std::string_view label;
        std::string_view text;
        Outcome outcome;
        std::size_t items; ///< How many entries are read, when there is a prefix.
    };
    auto const rows = std::array {
        Row { .label = "cut in the first entry",
              .text = std::string_view { recorded }.substr(0, 40),
              .outcome = Outcome::Refused,
              .items = 0 },
        Row { .label = "cut in the last entry",
              .text = std::string_view { recorded }.substr(0, lastRow),
              .outcome = Outcome::Prefix,
              .items = RecordedPullRequests.size() - 1 },
        Row { .label = "cut after the whole array",
              .text = recorded,
              .outcome = Outcome::Prefix,
              .items = RecordedPullRequests.size() },
        Row { .label = "a hundred entries, cut in the last",
              .text = std::string_view { many }.substr(0, many.size() - 10),
              .outcome = Outcome::Prefix,
              .items = ListLimit - 1 },
    };
    for (auto const& row: rows)
    {
        CAPTURE(row.label);
        auto const listed = github::changeRequests(row.text, OutputExtent::Truncated);
        CHECK((listed ? Outcome::Prefix : Outcome::Refused) == row.outcome);
        if (!listed)
        {
            CHECK(listed.error() == ParseError::NotJson);
            continue;
        }
        CHECK(listed->completeness == ListCompleteness::Cut);
        CHECK(listed->items.size() == row.items);
    }
}

TEST_CASE("GitHub.issues.recorded", "[github]")
{
    auto const listed =
        github::issues(readFixture(FixtureRoot::Testdata, "github/issues-open.json"), OutputExtent::Whole);
    REQUIRE(listed.has_value());
    CHECK(listed->skipped == 0);
    CHECK(listed->completeness == ListCompleteness::Complete);
    REQUIRE(listed->items.size() == RecordedIssueNumbers.size());
    for (auto const index: std::views::iota(std::size_t { 0 }, RecordedIssueNumbers.size()))
    {
        auto const number = RecordedIssueNumbers[index];
        auto const& issue = listed->items[index];
        CAPTURE(number);
        auto const* ref = std::get_if<IssueRef>(&issue.item);
        REQUIRE(ref != nullptr);
        CHECK(ref->number == number);
        CHECK(issue.url == std::format("https://github.com/contour-terminal/contour/issues/{}", number));
        CHECK_FALSE(issue.title.empty());
    }
}

TEST_CASE("GitHub.defaultBranch", "[github]")
{
    struct Row
    {
        std::string_view label;
        std::string_view text;
        std::string_view expected; // Empty: refused with WrongShape.
    };
    auto const recorded = readFixture(FixtureRoot::Testdata, "github/default-branch.json");
    auto const rows = std::array {
        Row { .label = "recorded", .text = recorded, .expected = "master" },
        Row { .label = "an option", .text = R"({"defaultBranchRef":{"name":"-x"}})", .expected = "" },
        Row { .label = "no name", .text = R"({"defaultBranchRef":{}})", .expected = "" },
        Row { .label = "a list", .text = "[]", .expected = "" },
    };
    for (auto const& row: rows)
    {
        CAPTURE(row.label);
        auto const branch = github::defaultBranch(row.text);
        if (row.expected.empty())
        {
            REQUIRE_FALSE(branch.has_value());
            CHECK(branch.error() == ParseError::WrongShape);
        }
        else
        {
            REQUIRE(branch.has_value());
            CHECK(*branch == row.expected);
        }
    }
    auto const cutOff = github::defaultBranch("{");
    REQUIRE_FALSE(cutOff.has_value());
    CHECK(cutOff.error() == ParseError::NotJson);
}

TEST_CASE("GitHub.reads_no_byte_past_the_reply", "[github]")
{
    // Each reply sits in a heap block of exactly its size, so that ASan reports a read of the byte after it:
    // nothing terminates the output of a command.
    auto const recorded = readFixture(FixtureRoot::Testdata, "github/prs-open.json");
    struct Row
    {
        std::string_view label;
        std::string_view text;
        std::size_t items; // Unused when refused.
        std::optional<ParseError> expected;
    };
    auto const rows = std::array {
        Row { .label = "a cut-off reply",
              .text = std::string_view { recorded }.substr(0, recorded.size() / 2),
              .items = 0,
              .expected = ParseError::NotJson },
        Row { .label = "cut off inside a number",
              .text = R"([[12)",
              .items = 0,
              .expected = ParseError::NotJson },
        Row { .label = "a whole reply",
              .text = recorded,
              .items = RecordedPullRequests.size(),
              .expected = std::nullopt },
        Row { .label = "an empty list", .text = "[]", .items = 0, .expected = std::nullopt },
    };
    for (auto const& row: rows)
    {
        CAPTURE(row.label);
        auto const buffer = std::make_unique<char[]>(row.text.size());
        std::ranges::copy(row.text, buffer.get());
        auto const exact = std::string_view { buffer.get(), row.text.size() };
        auto const listed = github::changeRequests(exact, OutputExtent::Whole);
        if (row.expected.has_value())
        {
            REQUIRE_FALSE(listed.has_value());
            CHECK(listed.error() == *row.expected);
        }
        else
        {
            REQUIRE(listed.has_value());
            CHECK(listed->items.size() == row.items);
        }
    }
}

TEST_CASE("GitHub.closes_only_issues_of_its_own_repository", "[github]")
{
    struct Row
    {
        std::string_view label;
        std::string_view reference; // One closingIssuesReferences entry, projected.
        std::vector<uint64_t> closes;
        std::vector<uint64_t> unclaimed; // Of this repository's issues 5 and 6.
    };
    auto const rows = std::array {
        Row { .label = "this repository",
              .reference = R"([5,"https://github.com/o/r/issues/5"])",
              .closes = { 5 },
              .unclaimed = { 6 } },
        Row { .label = "another repository",
              .reference = R"([5,"https://github.com/other/repo/issues/5"])",
              .closes = {},
              .unclaimed = { 5, 6 } },
        Row { .label = "a repository whose name extends this one's",
              .reference = R"([5,"https://github.com/o/r2/issues/5"])",
              .closes = {},
              .unclaimed = { 5, 6 } },
        Row { .label = "another host",
              .reference = R"([5,"https://example.com/o/r/issues/5"])",
              .closes = {},
              .unclaimed = { 5, 6 } },
        Row { .label = "no url", .reference = R"([5,null])", .closes = {}, .unclaimed = { 5, 6 } },
    };
    for (auto const& row: rows)
    {
        CAPTURE(row.label);
        auto const text =
            std::format(R"([[1,"t","https://github.com/o/r/pull/1","h","master",false,false,"",[{}],[]]])",
                        row.reference);
        auto const listed = github::changeRequests(text, OutputExtent::Whole);
        REQUIRE(listed.has_value());
        REQUIRE(listed->items.size() == 1);
        CHECK(listed->items.front().closesIssues == row.closes);

        auto kept = std::vector<uint64_t> {};
        for (auto const& issue: unclaimed({ issueNumbered(5), issueNumbered(6) }, listed->items))
            kept.push_back(std::get<IssueRef>(issue.item).number);
        CHECK(kept == row.unclaimed);
    }
}

TEST_CASE("Model.unclaimed", "[model]")
{
    auto const mine = std::array {
        changeRequestClosing(10, { 1, 3 }),
        changeRequestClosing(11, { 5 }),
    };
    auto issues = std::vector {
        issueNumbered(1), issueNumbered(2), issueNumbered(3), issueNumbered(4), issueNumbered(5)
    };
    issues.push_back(Issue { .item = JiraRef { .key = "ABC-1" }, .title = "j", .url = "u" });

    auto const left = unclaimed(std::move(issues), mine);

    auto kept = std::vector<uint64_t> {};
    for (auto const& issue: left)
        if (auto const* ref = std::get_if<IssueRef>(&issue.item))
            kept.push_back(ref->number);
    CHECK(kept == std::vector<uint64_t> { 2, 4 });
    CHECK(left.size() == 3);
    CHECK(std::holds_alternative<JiraRef>(left.back().item));
}
