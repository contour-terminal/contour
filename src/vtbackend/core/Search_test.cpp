// SPDX-License-Identifier: Apache-2.0
#include <vtbackend/core/Search.hpp>

#include <catch2/catch_test_macros.hpp>

using vtbackend::CaseComparison;
using vtbackend::caseComparisonFor;
using vtbackend::SearchCaseSensitivity;
using vtbackend::SearchMatchTally;
using vtbackend::TallyExactness;

TEST_CASE("caseComparisonFor.smart", "[search]")
{
    auto constexpr Smart = SearchCaseSensitivity::Smart;

    // A needle with no uppercase letter is matched loosely, so "error" also finds "Error".
    CHECK(caseComparisonFor(U"", Smart) == CaseComparison::Folded);
    CHECK(caseComparisonFor(U"error", Smart) == CaseComparison::Folded);
    CHECK(caseComparisonFor(U"error: 42", Smart) == CaseComparison::Folded);

    // One uppercase letter anywhere pins it.
    CHECK(caseComparisonFor(U"Error", Smart) == CaseComparison::Exact);
    CHECK(caseComparisonFor(U"setNewSearchTerm", Smart) == CaseComparison::Exact);
}

TEST_CASE("caseComparisonFor.smartIsCodepointAware", "[search]")
{
    auto constexpr Smart = SearchCaseSensitivity::Smart;

    // The regression this function exists for: std::isupper() is undefined above 0xFF and answers for
    // the wrong alphabet besides, so every one of these used to be decided incorrectly -- and, worse,
    // decided differently by the matcher and by the highlighter.
    CHECK(caseComparisonFor(U"Привет", Smart) == CaseComparison::Exact);
    CHECK(caseComparisonFor(U"привет", Smart) == CaseComparison::Folded);
    CHECK(caseComparisonFor(U"Ünicode", Smart) == CaseComparison::Exact);
    CHECK(caseComparisonFor(U"ünicode", Smart) == CaseComparison::Folded);

    // Deseret capital letter LONG I (U+10400) is an uppercase letter outside the BMP.
    CHECK(caseComparisonFor(U"\U00010400", Smart) == CaseComparison::Exact);

    // Codepoints with no case at all must not pin the search.
    CHECK(caseComparisonFor(U"\U0001F600", Smart) == CaseComparison::Folded); // emoji
    CHECK(caseComparisonFor(U"12:04:11", Smart) == CaseComparison::Folded);
    CHECK(caseComparisonFor(U"日本語", Smart) == CaseComparison::Folded);
}

TEST_CASE("caseComparisonFor.pinnedModesIgnoreTheNeedle", "[search]")
{
    // Both pinned modes answer without looking at the text -- that is what makes them pinned.
    CHECK(caseComparisonFor(U"error", SearchCaseSensitivity::Sensitive) == CaseComparison::Exact);
    CHECK(caseComparisonFor(U"Error", SearchCaseSensitivity::Sensitive) == CaseComparison::Exact);
    CHECK(caseComparisonFor(U"", SearchCaseSensitivity::Sensitive) == CaseComparison::Exact);

    CHECK(caseComparisonFor(U"error", SearchCaseSensitivity::Insensitive) == CaseComparison::Folded);
    CHECK(caseComparisonFor(U"Error", SearchCaseSensitivity::Insensitive) == CaseComparison::Folded);
    CHECK(caseComparisonFor(U"", SearchCaseSensitivity::Insensitive) == CaseComparison::Folded);
}

TEST_CASE("SearchCaseSensitivity.smartIsZero", "[search]")
{
    // A zero-initialized policy must still mean what the terminal has always done, so that a Search
    // that was never explicitly configured searches the way it used to.
    CHECK(SearchCaseSensitivity {} == SearchCaseSensitivity::Smart);
}

TEST_CASE("SearchMatchTally.empty", "[search]")
{
    CHECK(SearchMatchTally {}.empty());
    CHECK(SearchMatchTally { .total = 0, .ordinal = 0, .exactness = TallyExactness::Exact }.empty());
    CHECK(!SearchMatchTally { .total = 1, .ordinal = 1, .exactness = TallyExactness::Exact }.empty());

    // A tally can be non-empty while the caller stands on no match at all -- the pattern matches
    // somewhere, just not where they are.
    CHECK(!SearchMatchTally { .total = 27, .ordinal = 0, .exactness = TallyExactness::Exact }.empty());
}
