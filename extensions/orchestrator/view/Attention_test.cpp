// SPDX-License-Identifier: Apache-2.0
#include <catch2/catch_test_macros.hpp>

#include <string>
#include <string_view>
#include <vector>

#include <orchestrator/view/Attention.hpp>
#include <orchestrator/view/ViewFixtures.hpp>

using namespace std::string_view_literals;
using namespace orchestrator;
using namespace orchestrator::view;
using namespace orchestrator::view::test;

TEST_CASE("a run first met is seen as it is; a change since is news", "[orchestrator][view][attention]")
{
    auto ledger = SeenLedger {};
    auto const working = RunMarks { .state = RowState::Working, .changedWallMs = 1, .log = "t1" };
    CHECK(ledger.newsOf("a", working) == News::None);
    CHECK(ledger.newsOf("a", working) == News::None);
    // Its transcript grew, as its statistics say; then it asks.
    auto grown = working;
    grown.log = "t2";
    CHECK(ledger.newsOf("a", grown) == News::Unseen);
    auto asking = grown;
    asking.state = RowState::Asking;
    asking.changedWallMs = 2;
    CHECK(ledger.newsOf("a", asking) == News::Unseen);
    // Looked at, it is seen.
    ledger.look("a", asking, {}, FocusTab::Transcript);
    CHECK(ledger.newsOf("a", asking) == News::None);
}

TEST_CASE("a tab has news once its content changed since it was shown", "[orchestrator][view][attention]")
{
    auto ledger = SeenLedger {};
    auto const marks = RunMarks { .state = RowState::Working, .changedWallMs = 1, .log = {} };
    // Nothing known yet: no news, and nothing seen.
    ledger.look("a", marks, {}, FocusTab::Board);
    CHECK(ledger.tabNewsOf("a", { .board = 5, .transcript = 7 }, FocusTab::Board) == News::None);
    CHECK(ledger.tabNewsOf("a", { .board = 5, .transcript = 7 }, FocusTab::Transcript) == News::None);
    // First known, a tab not shown is seen as it is.
    ledger.look("a", marks, { .board = 5, .transcript = 7 }, FocusTab::Board);
    CHECK(ledger.tabNewsOf("a", { .board = 5, .transcript = 7 }, FocusTab::Transcript) == News::None);
    // The transcript grows while the Board is shown: the Transcript tab has news, until it is shown.
    ledger.look("a", marks, { .board = 5, .transcript = 8 }, FocusTab::Board);
    CHECK(ledger.tabNewsOf("a", { .board = 5, .transcript = 8 }, FocusTab::Transcript) == News::Unseen);
    CHECK(ledger.tabNewsOf("a", { .board = 5, .transcript = 8 }, FocusTab::Board) == News::None);
    // An earlier page shown says nothing of how far it came: no news, and what was seen stays.
    CHECK(ledger.tabNewsOf("a", { .board = 5, .transcript = 0 }, FocusTab::Transcript) == News::None);
    ledger.look("a", marks, { .board = 5, .transcript = 8 }, FocusTab::Transcript);
    CHECK(ledger.tabNewsOf("a", { .board = 5, .transcript = 8 }, FocusTab::Transcript) == News::None);
    // The board changes while the Transcript is shown.
    ledger.look("a", marks, { .board = 6, .transcript = 8 }, FocusTab::Transcript);
    CHECK(ledger.tabNewsOf("a", { .board = 6, .transcript = 8 }, FocusTab::Board) == News::Unseen);
    CHECK(ledger.tabNewsOf("a", { .board = 6, .transcript = 8 }, FocusTab::Details) == News::None);
    // A run never looked at has no tab news.
    CHECK(ledger.tabNewsOf("b", { .board = 6, .transcript = 8 }, FocusTab::Board) == News::None);
}

TEST_CASE("the runs that entered Needs you", "[orchestrator][view][attention]")
{
    auto const rows = std::vector<Row> {
        Row { .kind = RowKind::Heading, .text = "NEEDS YOU" },
        Row { .kind = RowKind::Agent, .run = "b", .attention = FleetGroup::NeedsYou },
        Row { .kind = RowKind::Agent, .run = "a", .attention = FleetGroup::NeedsYou },
        Row { .kind = RowKind::Agent, .run = "c", .attention = FleetGroup::Working },
    };
    auto const now = needingRuns(rows);
    CHECK(now == std::vector<std::string> { "a", "b" });
    CHECK(needsYouEntries(std::vector<std::string> { "b" }, now) == std::vector<std::string> { "a" });
    CHECK(needsYouEntries(now, now).empty());
    CHECK(needsYouEntries(std::vector<std::string> { "a", "b", "z" }, now).empty());
}
