// SPDX-License-Identifier: Apache-2.0
//
// UNVERIFIED AGAINST REAL OUTPUT. The inputs below and in testdata/jira/ follow the shape Atlassian
// documents:
//   https://developer.atlassian.com/cloud/acli/reference/commands/jira-workitem-search/
//   https://developer.atlassian.com/cloud/jira/platform/rest/v3/api-group-issue-search/
// No `acli` printed them (testdata/jira/README.md); the issue source stays Unmeasured until one does.
#include <catch2/catch_test_macros.hpp>

#include <array>
#include <optional>
#include <string>
#include <string_view>

#include <orchestrator/core/Fixtures.hpp>
#include <orchestrator/core/IssueSources.hpp>
#include <orchestrator/core/Jira.hpp>
#include <orchestrator/core/Model.hpp>

using namespace std::string_view_literals;

namespace
{

using namespace orchestrator;

constexpr auto Site = IssueSourceContext { .site = "https://example.atlassian.net" };

} // namespace

TEST_CASE("Jira: the documented work items map to the model", "[jira]")
{
    auto const listed =
        jira::issues(readFixture(FixtureRoot::Testdata, "jira/issues-open.json"), OutputExtent::Whole, Site);
    REQUIRE(listed);
    CHECK(listed->skipped == 0);
    CHECK(listed->completeness == ListCompleteness::Complete);
    REQUIRE(listed->items.size() == 2);
    CHECK(listed->items[0].item == ItemRef { JiraRef { .key = "ABC-123" } });
    CHECK(listed->items[0].title == "Crash when resizing during output");
    CHECK(listed->items[1].item == ItemRef { JiraRef { .key = "ABC-7" } });
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
    auto const whole = readFixture(FixtureRoot::Testdata, "jira/issues-open.json");
    auto const cut = std::string_view(whole).substr(0, (whole.size() * 3) / 4);
    auto const listed = jira::issues(cut, OutputExtent::Truncated, Site);
    REQUIRE(listed);
    CHECK(listed->completeness == ListCompleteness::Cut);
    CHECK(listed->items.size() == 1);
    CHECK(!jira::issues("{}", OutputExtent::Whole, Site));
}
