// SPDX-License-Identifier: Apache-2.0
#include <contour/session/SearchStatus.hpp>

#include <catch2/catch_test_macros.hpp>

using contour::session::CasePinned;
using contour::session::CountReadiness;
using contour::session::describeSearch;
using contour::session::describeSearchCase;
using contour::session::describeSearchCounting;
using contour::session::MatchNavigation;
using contour::session::MatchPresence;
using contour::session::nextSearchCase;
using contour::session::SearchOutcome;
using vtbackend::SearchCaseSensitivity;
using vtbackend::SearchMatchTally;
using vtbackend::TallyExactness;

// NB: no strings are asserted here. What the bar SAYS is rendered by TerminalSession through tr(),
// so it can be translated; what is decided here is which of the three things to say, and whether
// stepping is possible -- the parts with rules.

TEST_CASE("describeSearch.idle", "[searchstatus]")
{
    // No pattern: the bar shows no count whatsoever, not "0 matches".
    auto const status = describeSearch(U"", SearchMatchTally {});
    CHECK(status.outcome == SearchOutcome::Idle);
    CHECK(status.navigation == MatchNavigation::Unavailable);

    // A tally left over from a previous pattern must not resurrect a count.
    auto const stale = describeSearch(U"", SearchMatchTally { .total = 27, .ordinal = 3 });
    CHECK(stale.outcome == SearchOutcome::Idle);
}

TEST_CASE("describeSearch.noMatch", "[searchstatus]")
{
    auto const status = describeSearch(U"zsh", SearchMatchTally {});
    CHECK(status.outcome == SearchOutcome::NoMatch);
    CHECK(status.navigation == MatchNavigation::Unavailable);
}

TEST_CASE("describeSearch.matched", "[searchstatus]")
{
    auto const status = describeSearch(U"search", SearchMatchTally { .total = 27, .ordinal = 3 });
    CHECK(status.outcome == SearchOutcome::Matched);
    CHECK(status.navigation == MatchNavigation::Available);
    CHECK(status.readiness == CountReadiness::Settled);

    // The tally rides along, because the layer that renders the words needs the numbers.
    CHECK(status.tally.total == 27);
    CHECK(status.tally.ordinal == 3);
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
    // The last of many is still navigable, because the find bar wraps -- see wrapSearchTo().
    CHECK(navigation(27, 27) == MatchNavigation::Available);
}

TEST_CASE("describeSearchCounting", "[searchstatus]")
{
    // Typing knows whether anything matched without paying for the count, and that is enough to
    // tint the field and enable the buttons while the tally is still being walked.
    auto const some = describeSearchCounting(MatchPresence::Some);
    CHECK(some.outcome == SearchOutcome::Matched);
    CHECK(some.navigation == MatchNavigation::Available);
    CHECK(some.readiness == CountReadiness::Counting);

    // Nothing matched: that is already final, so there is nothing left to count.
    auto const none = describeSearchCounting(MatchPresence::None);
    CHECK(none.outcome == SearchOutcome::NoMatch);
    CHECK(none.navigation == MatchNavigation::Unavailable);
    CHECK(none.readiness == CountReadiness::Settled);
}

TEST_CASE("describeSearchCase.glyphNamesTheState", "[searchstatus]")
{
    // The mapping that was once inverted in three places at once. The glyph's own case is the claim:
    // "Aa" for a comparison that respects case, "aa" for one that ignores it.
    CHECK(describeSearchCase(SearchCaseSensitivity::Sensitive).glyph == "Aa");
    CHECK(describeSearchCase(SearchCaseSensitivity::Insensitive).glyph == "aa");

    // Smart shows the capitalised glyph too -- it becomes exact as soon as the term has a capital.
    CHECK(describeSearchCase(SearchCaseSensitivity::Smart).glyph == "Aa");
}

TEST_CASE("describeSearchCase.onlyPinnedModesAreLit", "[searchstatus]")
{
    // Lit means "you chose this"; unlit means smart case is still deciding.
    CHECK(describeSearchCase(SearchCaseSensitivity::Smart).pinned == CasePinned::No);
    CHECK(describeSearchCase(SearchCaseSensitivity::Sensitive).pinned == CasePinned::Yes);
    CHECK(describeSearchCase(SearchCaseSensitivity::Insensitive).pinned == CasePinned::Yes);
}

TEST_CASE("nextSearchCase.cyclesAllThreeAndCloses", "[searchstatus]")
{
    // Smart -> Sensitive -> Insensitive -> Smart. The order is stated here rather than derived from
    // the enumerator values, which do not run in that order.
    CHECK(nextSearchCase(SearchCaseSensitivity::Smart) == SearchCaseSensitivity::Sensitive);
    CHECK(nextSearchCase(SearchCaseSensitivity::Sensitive) == SearchCaseSensitivity::Insensitive);
    CHECK(nextSearchCase(SearchCaseSensitivity::Insensitive) == SearchCaseSensitivity::Smart);
}
