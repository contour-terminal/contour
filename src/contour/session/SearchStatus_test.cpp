// SPDX-License-Identifier: Apache-2.0
#include <contour/session/SearchStatus.hpp>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstddef>
#include <ranges>
#include <set>
#include <string_view>

using contour::session::CasePinned;
using contour::session::CountReadiness;
using contour::session::describeSearch;
using contour::session::describeSearchCase;
using contour::session::describeSearchCounting;
using contour::session::describeSearchError;
using contour::session::describeSearchProgress;
using contour::session::describeSearchToggle;
using contour::session::MatchNavigation;
using contour::session::MatchPresence;
using contour::session::nextSearchCase;
using contour::session::nextSearchOptions;
using contour::session::SearchOutcome;
using contour::session::SearchToggle;
using contour::session::SearchToggleRows;
using contour::session::ToggleState;
using vtbackend::SearchCaseSensitivity;
using vtbackend::SearchMatchTally;
using vtbackend::SearchOptions;
using vtbackend::SearchPatternError;
using vtbackend::SearchSyntax;
using vtbackend::SearchWordBoundary;
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

    // Nothing matched anywhere -- the probe read the whole grid: that is already final, so there is
    // nothing left to count.
    auto const none = describeSearchCounting(MatchPresence::None);
    CHECK(none.outcome == SearchOutcome::NoMatch);
    CHECK(none.navigation == MatchNavigation::Unavailable);
    CHECK(none.readiness == CountReadiness::Settled);

    // Nothing within the probe's budget, and more grid beyond it: not known until the walk says. Neither
    // red nor navigable.
    auto const unknown = describeSearchCounting(MatchPresence::Unknown);
    CHECK(unknown.outcome == SearchOutcome::Pending);
    CHECK(unknown.navigation == MatchNavigation::Unavailable);
    CHECK(unknown.readiness == CountReadiness::Counting);
}

TEST_CASE("describeSearchProgress", "[searchstatus]")
{
    auto const nothingYet = describeSearchProgress({});
    CHECK(nothingYet.outcome == SearchOutcome::Pending);
    CHECK(nothingYet.navigation == MatchNavigation::Unavailable);
    CHECK(nothingYet.readiness == CountReadiness::Counting);

    auto const partial = SearchMatchTally { .total = 5, .ordinal = 2, .exactness = TallyExactness::Exact };
    auto const some = describeSearchProgress(partial);
    CHECK(some.outcome == SearchOutcome::Matched);
    CHECK(some.navigation == MatchNavigation::Available);
    CHECK(some.readiness == CountReadiness::Counting);
    CHECK(some.tally == partial);
}

TEST_CASE("describeSearchError", "[searchstatus]")
{
    for (auto const error: { SearchPatternError::InvalidSyntax,
                             SearchPatternError::BackReference,
                             SearchPatternError::TooComplex })
    {
        auto const status = describeSearchError(error);
        CHECK(status.outcome == SearchOutcome::Invalid);
        CHECK(status.navigation == MatchNavigation::Unavailable);
        CHECK(status.readiness == CountReadiness::Settled);
        CHECK(status.error == error);
    }
}

TEST_CASE("searchToggles.rowsAreInEnumeratorOrder", "[searchstatus]")
{
    auto names = std::set<std::string_view> {};
    for (auto const index: std::views::iota(std::size_t { 0 }, SearchToggleRows.size()))
    {
        CHECK(SearchToggleRows[index].toggle == static_cast<SearchToggle>(index));
        CHECK_FALSE(SearchToggleRows[index].accessibleName.empty());
        names.insert(SearchToggleRows[index].objectName);
    }
    CHECK(names.size() == SearchToggleRows.size());
}

namespace
{
/// How many of the three option fields differ between @p a and @p b.
int fieldsDiffering(SearchOptions a, SearchOptions b)
{
    return static_cast<int>(a.caseSensitivity != b.caseSensitivity) + static_cast<int>(a.syntax != b.syntax)
           + static_cast<int>(a.wordBoundary != b.wordBoundary);
}

/// Whether @p toggle's own field differs between @p a and @p b.
bool ownFieldDiffers(SearchToggle toggle, SearchOptions a, SearchOptions b)
{
    switch (toggle)
    {
        case SearchToggle::Case: return a.caseSensitivity != b.caseSensitivity;
        case SearchToggle::Regex: return a.syntax != b.syntax;
        case SearchToggle::WholeWord: return a.wordBoundary != b.wordBoundary;
    }
    return false;
}
} // namespace

TEST_CASE("searchToggles.eachFlipsOnlyItsOwnField", "[searchstatus]")
{
    for (auto const& row: SearchToggleRows)
    {
        auto const once = nextSearchOptions(row.toggle, {});
        CHECK(fieldsDiffering(once, {}) == 1);
        CHECK(ownFieldDiffers(row.toggle, once, {}));

        // Case cycles three policies; the other two flip between two.
        auto const cycle = row.toggle == SearchToggle::Case ? 3 : 2;
        auto options = SearchOptions {};
        for ([[maybe_unused]] auto const step: std::views::iota(0, cycle))
            options = nextSearchOptions(row.toggle, options);
        CHECK(options == SearchOptions {});
    }
}

TEST_CASE("searchToggles.glyphsAndState", "[searchstatus]")
{
    auto const regex = describeSearchToggle(SearchToggle::Regex, { .syntax = SearchSyntax::Regex });
    CHECK(regex.glyph == ".*");
    CHECK(regex.state == ToggleState::On);
    CHECK(describeSearchToggle(SearchToggle::Regex, {}).state == ToggleState::Off);

    auto const word = describeSearchToggle(SearchToggle::WholeWord, {});
    CHECK(word.glyph == "ab");
    CHECK(word.state == ToggleState::Off);
    CHECK(
        describeSearchToggle(SearchToggle::WholeWord, { .wordBoundary = SearchWordBoundary::WholeWord }).state
        == ToggleState::On);

    // Case keeps describeSearchCase()'s answers: its glyph, and lit exactly when pinned.
    for (auto const mode: { SearchCaseSensitivity::Smart,
                            SearchCaseSensitivity::Sensitive,
                            SearchCaseSensitivity::Insensitive })
    {
        auto const toggle = describeSearchToggle(SearchToggle::Case, { .caseSensitivity = mode });
        CHECK(toggle.glyph == describeSearchCase(mode).glyph);
        CHECK((toggle.state == ToggleState::On) == (describeSearchCase(mode).pinned == CasePinned::Yes));
    }
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
