// SPDX-License-Identifier: Apache-2.0
#include <catch2/catch_test_macros.hpp>

#include <format>
#include <ranges>
#include <string>
#include <string_view>

#include <orchestrator/core/Fixtures.hpp>
#include <orchestrator/core/Stats.hpp>

using namespace std::string_view_literals;
using namespace orchestrator;

TEST_CASE("format 1 reports parse", "[orchestrator][stats]")
{
    SECTION("a finished session")
    {
        auto const stats = parseStats(readFixture(FixtureRoot::Contract, "agent-stats/format-1/basic.json"));
        REQUIRE(stats.has_value());
        CHECK(stats->writtenBy == "2.1.330");
        CHECK(stats->tokens
              == StatsTokens {
                  .input = 16, .output = 200, .thinking = 80, .cacheWrite = 2400, .cacheRead = 27300 });
        CHECK(stats->apiCalls == 4);
        CHECK(stats->apiErrors == 1);
        CHECK(stats->prompts == 2);
        CHECK(stats->activeMs == 150'000);
        CHECK(stats->toolCalls == 3);
        CHECK(stats->toolFailures == 1);
        CHECK(stats->tools
              == std::vector { StatsTool { .name = "Bash", .calls = 2 },
                               StatsTool { .name = "Read", .calls = 1 } });
        CHECK(stats->compactions == 1);
        CHECK(stats->pullRequests
              == std::vector { StatsPullRequest { .number = 7, .url = "https://github.com/o/r/pull/7" } });
        REQUIRE(stats->cost.has_value());
        CHECK(stats->cost->usd == 0.123457);
        CHECK(stats->cost->linesAdded == 12);
    }
    SECTION("a session still running has no cost")
    {
        auto const stats =
            parseStats(readFixture(FixtureRoot::Contract, "agent-stats/format-1/running.json"));
        REQUIRE(stats.has_value());
        CHECK_FALSE(stats->cost.has_value());
        CHECK(stats->apiCalls == 4);
    }
    SECTION("an empty log")
    {
        auto const stats = parseStats(readFixture(FixtureRoot::Contract, "agent-stats/format-1/empty.json"));
        REQUIRE(stats.has_value());
        CHECK(*stats == RunStats {});
    }
}

TEST_CASE("another format is a mismatch, anything else no report", "[orchestrator][stats]")
{
    CHECK(parseStats(R"({"format":2,"tokens":"elsewhere"})").error() == StatsReadError::FormatMismatch);
    CHECK(parseStats("not json").error() == StatsReadError::NotStats);
    CHECK(parseStats(R"({"format":1,"tokens":7})").error() == StatsReadError::NotStats);
    CHECK(classifyStatsFailure("unsupported stats format 1; this contour prints 2\n")
          == StatsReadError::FormatMismatch);
    CHECK(classifyStatsFailure("no session log for x") == StatsReadError::NotStats);
}

TEST_CASE("a report keeps no more than it may store", "[orchestrator][stats]")
{
    auto tools = std::string {};
    for (auto const index: std::views::iota(0, 20))
        tools += std::format(R"({}{{"name":"T{}","calls":1}})", index == 0 ? "" : ",", index);
    auto const json = std::format(R"({{"format":1,"tools":[{}],"writtenBy":"{}","pullRequests":[{}]}})",
                                  tools,
                                  std::string(1000, 'v'),
                                  R"({"number":1,"url":"https://x/\u001b[31m"})");
    auto const stats = parseStats(json);
    REQUIRE(stats.has_value());
    CHECK(stats->tools.size() == MaxStoredTools);
    CHECK(stats->writtenBy.size() <= MaxStoredStatsText * 4);
    REQUIRE(stats->pullRequests.size() == 1);
    CHECK_FALSE(stats->pullRequests.front().url.contains('\x1b'));
}

TEST_CASE("the cache hit is the share of input read from the cache", "[orchestrator][stats]")
{
    CHECK_FALSE(cacheHitOf(StatsTokens {}).has_value());
    auto const tokens =
        StatsTokens { .input = 10, .output = 5, .thinking = 0, .cacheWrite = 30, .cacheRead = 60 };
    CHECK(inputTokensOf(tokens) == 100);
    CHECK(cacheHitOf(tokens) == 0.6);
}
