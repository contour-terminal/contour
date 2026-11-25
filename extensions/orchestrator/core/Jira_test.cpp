// SPDX-License-Identifier: Apache-2.0
//
// testdata/jira/ holds what `acli jira workitem search --json` printed (acli 1.3.41), scrubbed by
// harness/scrub-fixture.py; testdata/jira/README.md says which file is hand-made.
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <format>
#include <optional>
#include <ranges>
#include <string>
#include <string_view>
#include <variant>

#include <orchestrator/core/Fixtures.hpp>
#include <orchestrator/core/IssueSources.hpp>
#include <orchestrator/core/Jira.hpp>
#include <orchestrator/core/Model.hpp>

using namespace std::string_view_literals;

namespace
{

using namespace orchestrator;

constexpr auto Site = IssueSourceContext { .site = "https://example.atlassian.net" };

/// The work items in testdata/jira/issues-open.json.
constexpr auto RecordedWorkItems = std::size_t { 31 };

} // namespace

TEST_CASE("Jira: the recorded work items map to the model", "[jira]")
{
    auto const listed =
        jira::issues(readFixture(FixtureRoot::Testdata, "jira/issues-open.json"), OutputExtent::Whole, Site);
    REQUIRE(listed);
    CHECK(listed->skipped == 0);
    CHECK(listed->completeness == ListCompleteness::Complete);
    REQUIRE(listed->items.size() == RecordedWorkItems);
    CHECK(listed->items.front().item == ItemRef { JiraRef { .key = "ABC-1" } });
    CHECK(listed->items.front().title == "Work item 1");
    for (auto const& issue: listed->items)
    {
        auto const* const ref = std::get_if<JiraRef>(&issue.item);
        REQUIRE(ref != nullptr);
        CHECK(issue.url == std::format("https://example.atlassian.net/browse/{}", ref->key));
    }
}

TEST_CASE("Jira: status and category", "[jira]")
{
    auto const listed =
        jira::issues(readFixture(FixtureRoot::Testdata, "jira/issues-work.json"), OutputExtent::Whole, Site);
    REQUIRE(listed.has_value());
    REQUIRE(listed->items.size() == 5);
    CHECK(listed->items[0].status == "In Progress");
    CHECK(listed->items[0].category == StatusCategory::InProgress);
    CHECK(listed->items[1].status == "Selected for Development");
    CHECK(listed->items[1].category == StatusCategory::ToDo);
    CHECK(listed->items[3].category == StatusCategory::InProgress);
    CHECK(listed->items[4].category == StatusCategory::InProgress);
    CHECK_FALSE(listed->items[4].status.contains('\x1b'));
    // acli's search cannot print `updated`: a Jira ticket's update time is unknown.
    CHECK(std::ranges::all_of(listed->items, [](Issue const& issue) { return issue.updatedWallMs == 0; }));
    CHECK(jira::statusCategoryOf("new") == StatusCategory::ToDo);
    CHECK(jira::statusCategoryOf("indeterminate") == StatusCategory::InProgress);
    CHECK(jira::statusCategoryOf("done") == StatusCategory::Done);
    CHECK(jira::statusCategoryOf("other") == StatusCategory::Unknown);
    CHECK(jira::statusCategoryOf("") == StatusCategory::Unknown);
}

TEST_CASE("Jira: the recorded search carries status and category", "[jira]")
{
    auto const listed =
        jira::issues(readFixture(FixtureRoot::Testdata, "jira/issues-open.json"), OutputExtent::Whole, Site);
    REQUIRE(listed.has_value());
    CHECK(std::ranges::all_of(listed->items, [](Issue const& issue) {
        return !issue.status.empty() && issue.category != StatusCategory::Unknown;
    }));
}

TEST_CASE("Jira: a ticket's rank is its position in the search result", "[jira]")
{
    SECTION("in the array's order")
    {
        auto const listed = jira::issues(
            readFixture(FixtureRoot::Testdata, "jira/issues-work.json"), OutputExtent::Whole, Site);
        REQUIRE(listed.has_value());
        constexpr auto Keys =
            std::array { "PRJ-1234"sv, "PRJ-1240"sv, "PRJ-1300"sv, "PRJ-1301"sv, "PRJ-1302"sv };
        REQUIRE(listed->items.size() == Keys.size());
        for (auto const& [rank, key]: std::views::enumerate(Keys))
        {
            CAPTURE(rank, key);
            CHECK(listed->items[static_cast<std::size_t>(rank)].item
                  == ItemRef { JiraRef { .key = std::string(key) } });
            CHECK(listed->items[static_cast<std::size_t>(rank)].listRank == static_cast<std::size_t>(rank));
        }
    }
    SECTION("a skipped entry keeps its place")
    {
        // issues-bad-key.json's only Jira key is its last entry.
        auto const listed = jira::issues(
            readFixture(FixtureRoot::Testdata, "jira/issues-bad-key.json"), OutputExtent::Whole, Site);
        REQUIRE(listed.has_value());
        REQUIRE(listed->items.size() == 1);
        CHECK(listed->items.front().listRank == 3);
    }
    SECTION("a status without a category")
    {
        auto const listed =
            jira::issues(R"([{"key":"PRJ-1","fields":{"summary":"s","status":{"name":"To Do"}}},)"
                         R"({"key":"PRJ-2","fields":{"summary":"s"}}])",
                         OutputExtent::Whole,
                         Site);
        REQUIRE(listed.has_value());
        REQUIRE(listed->items.size() == 2);
        CHECK(listed->items[0].status == "To Do");
        CHECK(listed->items[0].category == StatusCategory::Unknown);
        CHECK(listed->items[1].status.empty());
        CHECK(listed->items[1].listRank == 1);
    }
}

TEST_CASE("Jira: an empty search", "[jira]")
{
    // A search matching nothing prints `[]` and exits 0.
    auto const listed =
        jira::issues(readFixture(FixtureRoot::Testdata, "jira/issues-empty.json"), OutputExtent::Whole, Site);
    REQUIRE(listed);
    CHECK(listed->items.empty());
    CHECK(listed->skipped == 0);
    CHECK(listed->completeness == ListCompleteness::Complete);
}

TEST_CASE("a key that is not a Jira key is skipped", "[jira]")
{
    auto const listed = jira::issues(
        readFixture(FixtureRoot::Testdata, "jira/issues-bad-key.json"), OutputExtent::Whole, Site);
    REQUIRE(listed);
    CHECK(listed->skipped == 3);
    REQUIRE(listed->items.size() == 1);
    CHECK(listed->items.front().item == ItemRef { JiraRef { .key = "ABC-9" } });
}

TEST_CASE("the url is built from the site", "[jira]")
{
    // A `self` or any other URL in the output is never read.
    auto const listed =
        jira::issues(R"([{"key":"ABC-123","self":"https://evil.example/x","fields":{"summary":"s"}}])",
                     OutputExtent::Whole,
                     Site);
    REQUIRE(listed);
    REQUIRE(listed->items.size() == 1);
    CHECK(listed->items.front().url == "https://example.atlassian.net/browse/ABC-123");
    CHECK(jira::browseUrl("https://jira.example.com:8443", "X1-2")
          == "https://jira.example.com:8443/browse/X1-2");
}

TEST_CASE("Jira: a site is https with a host and no path", "[jira]")
{
    struct Row
    {
        std::string_view text;
        std::optional<std::string_view> expected;
    };
    constexpr auto Rows = std::array {
        Row { .text = "https://example.atlassian.net", .expected = "https://example.atlassian.net" },
        Row { .text = "https://example.atlassian.net/", .expected = "https://example.atlassian.net" },
        Row { .text = "https://jira.example.com:8443", .expected = "https://jira.example.com:8443" },
        Row { .text = "http://example.atlassian.net", .expected = std::nullopt },
        Row { .text = "https://", .expected = std::nullopt },
        Row { .text = "https:///", .expected = std::nullopt },
        Row { .text = "https://example.atlassian.net/jira", .expected = std::nullopt },
        Row { .text = "https://example.atlassian.net//", .expected = std::nullopt },
        Row { .text = "https://user:pass"
                      "\x40"
                      "example.atlassian.net",
              .expected = std::nullopt },
        Row { .text = "https://example.atlassian.net?x=1", .expected = std::nullopt },
        Row { .text = "https://-example.net", .expected = std::nullopt },
        Row { .text = "example.atlassian.net", .expected = std::nullopt },
    };
    for (auto const& row: Rows)
    {
        CAPTURE(row.text);
        CHECK(jira::siteOf(row.text) == row.expected);
    }
}

TEST_CASE("Jira: a cut reply keeps the whole entries before the cut", "[jira]")
{
    SECTION("compact")
    {
        constexpr auto Whole =
            R"([{"key":"ABC-123","fields":{"summary":"a"}},{"key":"ABC-7","fields":{"summary":"b"}}])"sv;
        auto const listed =
            jira::issues(Whole.substr(0, (Whole.size() * 3) / 4), OutputExtent::Truncated, Site);
        REQUIRE(listed);
        CHECK(listed->completeness == ListCompleteness::Cut);
        CHECK(listed->items.size() == 1);
    }
    SECTION("recorded, cut in the middle of its second entry")
    {
        auto const whole = readFixture(FixtureRoot::Testdata, "jira/issues-open.json");
        auto const second = whole.find(R"("key": "ABC-2")");
        REQUIRE(second != std::string::npos);
        auto const listed =
            jira::issues(std::string_view(whole).substr(0, second), OutputExtent::Truncated, Site);
        REQUIRE(listed);
        CHECK(listed->completeness == ListCompleteness::Cut);
        REQUIRE(listed->items.size() == 1);
        CHECK(listed->items.front().item == ItemRef { JiraRef { .key = "ABC-1" } });
    }
    CHECK(!jira::issues("{}", OutputExtent::Whole, Site));
}
