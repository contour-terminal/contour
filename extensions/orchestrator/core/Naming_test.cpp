// SPDX-License-Identifier: Apache-2.0
#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstdint>
#include <string>
#include <string_view>

#include <orchestrator/core/ArgumentKinds.hpp>
#include <orchestrator/core/Model.hpp>
#include <orchestrator/core/Naming.hpp>

namespace
{

using orchestrator::Stamp;

struct StampRow
{
    uint64_t unixMs = 0;
    int32_t utcOffsetMinutes = 0;
    Stamp expected;
};

// `date -u -d @1794578927` prints Fri Nov 13 14:08:47 UTC 2026; `date -u -d @1835438400` prints
// Tue Feb 29 12:00:00 UTC 2028.
constexpr auto StampRows = std::array {
    StampRow { .unixMs = 1794578927000,
               .utcOffsetMinutes = 60,
               .expected = { .year = 2026, .month = 11, .day = 13, .hour = 15, .minute = 8, .second = 47 } },
    StampRow { .unixMs = 1794578927000,
               .utcOffsetMinutes = -480,
               .expected = { .year = 2026, .month = 11, .day = 13, .hour = 6, .minute = 8, .second = 47 } },
    StampRow { .unixMs = 1835438400000,
               .utcOffsetMinutes = 0,
               .expected = { .year = 2028, .month = 2, .day = 29, .hour = 12, .minute = 0, .second = 0 } },
    // Beyond the brief: the milliseconds are dropped, an offset moves the date, the epoch's edges.
    StampRow { .unixMs = 1794578927999,
               .utcOffsetMinutes = 0,
               .expected = { .year = 2026, .month = 11, .day = 13, .hour = 14, .minute = 8, .second = 47 } },
    StampRow { .unixMs = 1794578927000,
               .utcOffsetMinutes = 600,
               .expected = { .year = 2026, .month = 11, .day = 14, .hour = 0, .minute = 8, .second = 47 } },
    StampRow { .unixMs = 0,
               .utcOffsetMinutes = 0,
               .expected = { .year = 1970, .month = 1, .day = 1, .hour = 0, .minute = 0, .second = 0 } },
    StampRow { .unixMs = 0,
               .utcOffsetMinutes = -330,
               .expected = { .year = 1969, .month = 12, .day = 31, .hour = 18, .minute = 30, .second = 0 } },
    StampRow { .unixMs = UINT64_MAX,
               .utcOffsetMinutes = 0,
               .expected = { .year = 9999, .month = 12, .day = 31, .hour = 23, .minute = 59, .second = 59 } },
    // An offset that lands on a leap day, from either side (`date -u -d @1835393400`: 2028-02-28 23:30:00,
    // `date -u -d @1835483400`: 2028-03-01 00:30:00), and one across a month's end (`date -u -d
    // @1796081400`: 2026-11-30 23:30:00).
    StampRow { .unixMs = 1835393400000,
               .utcOffsetMinutes = 60,
               .expected = { .year = 2028, .month = 2, .day = 29, .hour = 0, .minute = 30, .second = 0 } },
    StampRow { .unixMs = 1835483400000,
               .utcOffsetMinutes = -60,
               .expected = { .year = 2028, .month = 2, .day = 29, .hour = 23, .minute = 30, .second = 0 } },
    StampRow { .unixMs = 1796081400000,
               .utcOffsetMinutes = 60,
               .expected = { .year = 2026, .month = 12, .day = 1, .hour = 0, .minute = 30, .second = 0 } },
    // The extreme real offsets, +14 h and -12 h.
    StampRow { .unixMs = 1794578927000,
               .utcOffsetMinutes = 840,
               .expected = { .year = 2026, .month = 11, .day = 14, .hour = 4, .minute = 8, .second = 47 } },
    StampRow { .unixMs = 1794578927000,
               .utcOffsetMinutes = -720,
               .expected = { .year = 2026, .month = 11, .day = 13, .hour = 2, .minute = 8, .second = 47 } },
    // Offsets beyond a day are clamped to one.
    StampRow { .unixMs = 1794578927000,
               .utcOffsetMinutes = INT32_MAX,
               .expected = { .year = 2026, .month = 11, .day = 14, .hour = 14, .minute = 8, .second = 47 } },
    StampRow { .unixMs = 1794578927000,
               .utcOffsetMinutes = INT32_MIN,
               .expected = { .year = 2026, .month = 11, .day = 12, .hour = 14, .minute = 8, .second = 47 } },
    // The year keeps four digits: a local time past 9999 counts as its last second.
    StampRow { .unixMs = orchestrator::MaxStampUnixMs,
               .utcOffsetMinutes = 60,
               .expected = { .year = 9999, .month = 12, .day = 31, .hour = 23, .minute = 59, .second = 59 } },
    StampRow { .unixMs = UINT64_MAX,
               .utcOffsetMinutes = orchestrator::MaxUtcOffsetMinutes,
               .expected = { .year = 9999, .month = 12, .day = 31, .hour = 23, .minute = 59, .second = 59 } },
    StampRow { .unixMs = orchestrator::MaxStampUnixMs,
               .utcOffsetMinutes = -60,
               .expected = { .year = 9999, .month = 12, .day = 31, .hour = 22, .minute = 59, .second = 59 } },
};

static_assert(orchestrator::stampFromUnixMs(StampRows[0].unixMs, StampRows[0].utcOffsetMinutes)
              == StampRows[0].expected);
static_assert(orchestrator::stampFromUnixMs(StampRows[1].unixMs, StampRows[1].utcOffsetMinutes)
              == StampRows[1].expected);
static_assert(orchestrator::stampFromUnixMs(StampRows[2].unixMs, StampRows[2].utcOffsetMinutes)
              == StampRows[2].expected);

/// What a row expects of isJiraKey.
enum class KeyExpectation : uint8_t
{
    Rejected,
    Accepted,
};

struct JiraKeyRow
{
    std::string_view text;
    KeyExpectation expectation;
};

constexpr auto JiraKeyRows = std::array {
    JiraKeyRow { .text = "CT-418", .expectation = KeyExpectation::Accepted },
    JiraKeyRow { .text = "AB2-1", .expectation = KeyExpectation::Accepted },
    JiraKeyRow { .text = "ct-418", .expectation = KeyExpectation::Rejected },
    JiraKeyRow { .text = "C-1", .expectation = KeyExpectation::Rejected },
    JiraKeyRow { .text = "CT-", .expectation = KeyExpectation::Rejected },
    JiraKeyRow { .text = "CT-1a", .expectation = KeyExpectation::Rejected },
    JiraKeyRow { .text = "CT-1 ", .expectation = KeyExpectation::Rejected },
    JiraKeyRow { .text = "-CT-1", .expectation = KeyExpectation::Rejected },
    // Beyond the brief: the empty key, a digit first, two dashes, and the length bound.
    JiraKeyRow { .text = "", .expectation = KeyExpectation::Rejected },
    JiraKeyRow { .text = "2C-1", .expectation = KeyExpectation::Rejected },
    JiraKeyRow { .text = "CT-1-2", .expectation = KeyExpectation::Rejected },
    JiraKeyRow { .text = "ABCDEFGHIJKLMNOPQRSTUVWXYZ-12345", .expectation = KeyExpectation::Accepted },
    JiraKeyRow { .text = "ABCDEFGHIJKLMNOPQRSTUVWXYZ-123456", .expectation = KeyExpectation::Rejected },
};

static_assert(orchestrator::isJiraKey("CT-418"));
static_assert(!orchestrator::isJiraKey("ct-418"));

struct NameRow
{
    orchestrator::ItemRef item;
    std::string_view slug;
    std::string_view runId;
    std::string_view branch;
    std::string_view worktree;
};

/// 2026-11-13 15:08:47, the first stamp above.
constexpr auto Afternoon =
    Stamp { .year = 2026, .month = 11, .day = 13, .hour = 15, .minute = 8, .second = 47 };

} // namespace

TEST_CASE("Naming", "[orchestrator][naming]")
{
    SECTION("stampFromUnixMs")
    {
        for (auto const& row: StampRows)
        {
            CAPTURE(row.unixMs, row.utcOffsetMinutes);
            auto const stamp = orchestrator::stampFromUnixMs(row.unixMs, row.utcOffsetMinutes);
            CAPTURE(stamp.year, stamp.month, stamp.day, stamp.hour, stamp.minute, stamp.second);
            CHECK(stamp == row.expected);
        }
    }

    SECTION("isJiraKey")
    {
        for (auto const& row: JiraKeyRows)
        {
            CAPTURE(row.text, row.expectation);
            CHECK(orchestrator::isJiraKey(row.text) == (row.expectation == KeyExpectation::Accepted));
        }
    }

    SECTION("names")
    {
        auto const rows = std::array {
            NameRow { .item = orchestrator::ChangeRequestRef { .number = 2334 },
                      .slug = "pr-2334",
                      .runId = "pr-2334-20261113-150847",
                      .branch = "agent/pr-2334-20261113-150847",
                      .worktree = "/home/u/wt/contour/pr-2334-20261113-150847" },
            NameRow { .item = orchestrator::IssueRef { .number = 2301 },
                      .slug = "issue-2301",
                      .runId = "issue-2301-20261113-150847",
                      .branch = "agent/issue-2301-20261113-150847",
                      .worktree = "/home/u/wt/contour/issue-2301-20261113-150847" },
            NameRow { .item = orchestrator::JiraRef { .key = "CT-418" },
                      .slug = "ct-418",
                      .runId = "ct-418-20261113-150847",
                      .branch = "agent/ct-418-20261113-150847",
                      .worktree = "/home/u/wt/contour/ct-418-20261113-150847" },
            // The largest number and the longest key still make a Name.
            NameRow { .item = orchestrator::IssueRef { .number = UINT64_MAX },
                      .slug = "issue-18446744073709551615",
                      .runId = "issue-18446744073709551615-20261113-150847",
                      .branch = "agent/issue-18446744073709551615-20261113-150847",
                      .worktree = "/home/u/wt/contour/issue-18446744073709551615-20261113-150847" },
            NameRow { .item = orchestrator::JiraRef { .key = "ABCDEFGHIJKLMNOPQRSTUVWXYZ-12345" },
                      .slug = "abcdefghijklmnopqrstuvwxyz-12345",
                      .runId = "abcdefghijklmnopqrstuvwxyz-12345-20261113-150847",
                      .branch = "agent/abcdefghijklmnopqrstuvwxyz-12345-20261113-150847",
                      .worktree = "/home/u/wt/contour/abcdefghijklmnopqrstuvwxyz-12345-20261113-150847" },
        };
        for (auto const& row: rows)
        {
            CAPTURE(row.slug, row.runId, row.branch, row.worktree);
            auto const slug = orchestrator::itemSlug(row.item);
            REQUIRE(slug.has_value());
            CHECK(*slug == row.slug);
            auto const runId = orchestrator::runId(row.item, Afternoon);
            REQUIRE(runId.has_value());
            CHECK(runId->text() == row.runId);
            CHECK(orchestrator::argumentAccepts(orchestrator::ArgumentKind::Name, runId->text()));
            auto const branch = orchestrator::branchName(*runId);
            CHECK(branch == row.branch);
            CHECK(orchestrator::argumentAccepts(orchestrator::ArgumentKind::Ref, branch));
            auto const worktree = orchestrator::worktreePath("/home/u/wt", "contour", *runId);
            CHECK(worktree == row.worktree);
            CHECK(orchestrator::argumentAccepts(orchestrator::ArgumentKind::Path, worktree));
        }
    }

    SECTION("a key that is not a Jira key makes no run id")
    {
        constexpr auto Keys = std::array<std::string_view, 7> {
            "../evil", "", "ct-418", "CT-", "CT-1 ", "-CT-1", "ABCDEFGHIJKLMNOPQRSTUVWXYZ-123456",
        };
        for (auto const key: Keys)
        {
            CAPTURE(key);
            auto const item = orchestrator::ItemRef { orchestrator::JiraRef { .key = std::string { key } } };
            auto const slug = orchestrator::itemSlug(item);
            REQUIRE_FALSE(slug.has_value());
            CHECK(slug.error() == orchestrator::NamingError::NotJiraKey);
            auto const runId = orchestrator::runId(item, Afternoon);
            REQUIRE_FALSE(runId.has_value());
            CHECK(runId.error() == orchestrator::NamingError::NotJiraKey);
        }
    }

    SECTION("every run id is a Name, whatever the stamp")
    {
        constexpr auto Stamps = std::array {
            orchestrator::stampFromUnixMs(0, -orchestrator::MaxUtcOffsetMinutes),
            orchestrator::stampFromUnixMs(UINT64_MAX, orchestrator::MaxUtcOffsetMinutes),
            Afternoon,
        };
        for (auto const& stamp: Stamps)
        {
            CAPTURE(stamp.year, stamp.month, stamp.day, stamp.hour, stamp.minute, stamp.second);
            auto const runId = orchestrator::runId(orchestrator::IssueRef { .number = UINT64_MAX }, stamp);
            REQUIRE(runId.has_value());
            CHECK(runId->text().size()
                  == std::string_view { "issue-18446744073709551615-20261113-150847" }.size());
            CHECK(orchestrator::argumentAccepts(orchestrator::ArgumentKind::Name, runId->text()));
            CHECK(orchestrator::argumentAccepts(orchestrator::ArgumentKind::Ref,
                                                orchestrator::branchName(*runId)));
            CHECK(orchestrator::argumentAccepts(orchestrator::ArgumentKind::Path,
                                                orchestrator::worktreePath("/w", "r", *runId)));
        }
    }
}
