// SPDX-License-Identifier: Apache-2.0
#include <vtbackend/core/Search.hpp>

#include <catch2/catch_test_macros.hpp>

#include <format>

using vtbackend::isCaseSensitiveSearch;
using vtbackend::SearchCaseSensitivity;
using vtbackend::SearchMatchTally;
using vtbackend::TallyExactness;

TEST_CASE("isCaseSensitiveSearch.smart", "[search]")
{
    auto constexpr Smart = SearchCaseSensitivity::Smart;

    // A needle with no uppercase letter is matched loosely, so "error" also finds "Error".
    CHECK(!isCaseSensitiveSearch(U"", Smart));
    CHECK(!isCaseSensitiveSearch(U"error", Smart));
    CHECK(!isCaseSensitiveSearch(U"error: 42", Smart));

    // One uppercase letter anywhere pins it.
    CHECK(isCaseSensitiveSearch(U"Error", Smart));
    CHECK(isCaseSensitiveSearch(U"setNewSearchTerm", Smart));
}

TEST_CASE("isCaseSensitiveSearch.smartIsCodepointAware", "[search]")
{
    auto constexpr Smart = SearchCaseSensitivity::Smart;

    // The regression this function exists for: std::isupper() is undefined above 0xFF and answers for
    // the wrong alphabet besides, so every one of these used to be decided incorrectly -- and, worse,
    // decided differently by the matcher and by the highlighter.
    CHECK(isCaseSensitiveSearch(U"Привет", Smart));
    CHECK(!isCaseSensitiveSearch(U"привет", Smart));
    CHECK(isCaseSensitiveSearch(U"Ünicode", Smart));
    CHECK(!isCaseSensitiveSearch(U"ünicode", Smart));

    // Deseret capital letter LONG I (U+10400) is an uppercase letter outside the BMP.
    CHECK(isCaseSensitiveSearch(U"\U00010400", Smart));

    // Codepoints with no case at all must not pin the search.
    CHECK(!isCaseSensitiveSearch(U"\U0001F600", Smart)); // emoji
    CHECK(!isCaseSensitiveSearch(U"12:04:11", Smart));
    CHECK(!isCaseSensitiveSearch(U"日本語", Smart));
}

TEST_CASE("isCaseSensitiveSearch.pinnedModesIgnoreTheNeedle", "[search]")
{
    // Both pinned modes answer without looking at the text -- that is what makes them pinned.
    CHECK(isCaseSensitiveSearch(U"error", SearchCaseSensitivity::Sensitive));
    CHECK(isCaseSensitiveSearch(U"Error", SearchCaseSensitivity::Sensitive));
    CHECK(isCaseSensitiveSearch(U"", SearchCaseSensitivity::Sensitive));

    CHECK(!isCaseSensitiveSearch(U"error", SearchCaseSensitivity::Insensitive));
    CHECK(!isCaseSensitiveSearch(U"Error", SearchCaseSensitivity::Insensitive));
    CHECK(!isCaseSensitiveSearch(U"", SearchCaseSensitivity::Insensitive));
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

TEST_CASE("SearchMatchTally.formatting", "[search]")
{
    CHECK(std::format("{}", SearchMatchTally { .total = 27, .ordinal = 3 }) == "3 of 27");

    // The "+" is the whole reason TallyExactness exists: "9999" and "9999+" must not read alike.
    CHECK(std::format("{}",
                      SearchMatchTally { .total = 9999, .ordinal = 3, .exactness = TallyExactness::Capped })
          == "3 of 9999+");
}
