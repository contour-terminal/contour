// SPDX-License-Identifier: Apache-2.0
#include <contour/session/SearchStatus.hpp>

#include <catch2/catch_test_macros.hpp>

using contour::session::describeSearch;
using contour::session::MatchNavigation;
using contour::session::SearchOutcome;
using vtbackend::SearchMatchTally;
using vtbackend::TallyExactness;

TEST_CASE("describeSearch.idle", "[searchstatus]")
{
    // No pattern: the bar shows a placeholder and no count whatsoever, not "0 matches".
    auto const status = describeSearch(U"", SearchMatchTally {});
    CHECK(status.summary.empty());
    CHECK(status.outcome == SearchOutcome::Idle);
    CHECK(status.navigation == MatchNavigation::Unavailable);

    // A tally left over from a previous pattern must not resurrect a count.
    auto const stale = describeSearch(U"", SearchMatchTally { .total = 27, .ordinal = 3 });
    CHECK(stale.summary.empty());
    CHECK(stale.outcome == SearchOutcome::Idle);
}

TEST_CASE("describeSearch.noMatch", "[searchstatus]")
{
    auto const status = describeSearch(U"zsh", SearchMatchTally {});
    CHECK(status.summary == "No results");
    CHECK(status.outcome == SearchOutcome::NoMatch);
    CHECK(status.navigation == MatchNavigation::Unavailable);
}

TEST_CASE("describeSearch.matched", "[searchstatus]")
{
    auto const status = describeSearch(U"search", SearchMatchTally { .total = 27, .ordinal = 3 });
    CHECK(status.summary == "3 of 27");
    CHECK(status.outcome == SearchOutcome::Matched);
    CHECK(status.navigation == MatchNavigation::Available);
}

TEST_CASE("describeSearch.cappedTallyIsMarked", "[searchstatus]")
{
    // The "+" is the difference between "there are 9999" and "there are at least 9999".
    auto const capped = describeSearch(
        U"e", SearchMatchTally { .total = 9999, .ordinal = 3, .exactness = TallyExactness::Capped });
    CHECK(capped.summary == "3 of 9999+");

    auto const offMatch = describeSearch(
        U"e", SearchMatchTally { .total = 9999, .ordinal = 0, .exactness = TallyExactness::Capped });
    CHECK(offMatch.summary == "9999+ matches");
}

TEST_CASE("describeSearch.offMatchReportsTheCountAlone", "[searchstatus]")
{
    // Standing on no match, "N of M" would have to invent N, so only M is reported.
    auto const many = describeSearch(U"search", SearchMatchTally { .total = 27, .ordinal = 0 });
    CHECK(many.summary == "27 matches");
    CHECK(many.outcome == SearchOutcome::Matched);

    auto const one = describeSearch(U"search", SearchMatchTally { .total = 1, .ordinal = 0 });
    CHECK(one.summary == "1 match"); // not "1 matches"
}

TEST_CASE("describeSearch.navigationNeedsSomewhereToGo", "[searchstatus]")
{
    auto const navigation = [](size_t total, size_t ordinal) {
        return describeSearch(U"x", SearchMatchTally { .total = total, .ordinal = ordinal }).navigation;
    };

    // Sitting on the only match: previous and next have nowhere to move.
    CHECK(navigation(1, 1) == MatchNavigation::Unavailable);

    // One match, but not on it: stepping takes you there, so the buttons stay live.
    CHECK(navigation(1, 0) == MatchNavigation::Available);

    CHECK(navigation(2, 1) == MatchNavigation::Available);
    CHECK(navigation(27, 27) == MatchNavigation::Available); // stepping wraps
}
