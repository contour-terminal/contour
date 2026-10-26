// SPDX-License-Identifier: Apache-2.0
#include <vtbackend/screen/SearchQuery.hpp>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <string_view>

using vtbackend::CaseComparison;
using vtbackend::escapeSearchRegex;
using vtbackend::SearchCaseSensitivity;
using vtbackend::SearchOptions;
using vtbackend::SearchPatternError;
using vtbackend::SearchQuery;
using vtbackend::SearchSyntax;

namespace
{
constexpr auto Delimiters = std::u32string_view { U" " };
constexpr auto RegexOptions = SearchOptions { .syntax = SearchSyntax::Regex };
} // namespace

TEST_CASE("searchQuery.literalAlwaysCompiles", "[searchquery]")
{
    // What is invalid as a regex is plain text to a literal search.
    auto const query = SearchQuery::compile(U"a(b", {}, Delimiters);
    REQUIRE(query.has_value());
    CHECK(query->pattern() == U"a(b");
    CHECK(query->options() == SearchOptions {});
}

TEST_CASE("searchQuery.invalidRegexIsAnError", "[searchquery]")
{
    for (auto const pattern: std::array<std::u32string_view, 3> { U"a(b", U"[z-a]", U"*x" })
    {
        auto const query = SearchQuery::compile(pattern, RegexOptions, Delimiters);
        REQUIRE_FALSE(query.has_value());
        CHECK(query.error() == SearchPatternError::InvalidSyntax);
    }

    // A dash at a class's edge, or after a range, is a character, not a reversed range.
    for (auto const pattern: std::array<std::u32string_view, 4> { U"[a-z]", U"[-a]", U"[a-]", U"[a-c-0]" })
        CHECK(SearchQuery::compile(pattern, RegexOptions, Delimiters).has_value());
}

TEST_CASE("searchQuery.backReferenceIsRefused", "[searchquery]")
{
    auto const backReference = SearchQuery::compile(U"(a)\\1", RegexOptions, Delimiters);
    REQUIRE_FALSE(backReference.has_value());
    CHECK(backReference.error() == SearchPatternError::BackReference);

    // An escaped backslash and then a digit: no back-reference at all.
    CHECK(SearchQuery::compile(U"\\\\1", RegexOptions, Delimiters).has_value());
}

TEST_CASE("searchQuery.smartCaseReadsTheRegexsOwnLetters", "[searchquery]")
{
    auto const comparisonOf = [](std::u32string_view pattern, SearchOptions options) {
        auto const query = SearchQuery::compile(pattern, options, Delimiters);
        REQUIRE(query.has_value());
        return query->comparison();
    };

    // An escape's letter is the escape's, not an uppercase letter of the pattern.
    CHECK(comparisonOf(U"\\d+", RegexOptions) == CaseComparison::Folded);
    CHECK(comparisonOf(U"\\dA", RegexOptions) == CaseComparison::Exact);
    CHECK(comparisonOf(U"Foo", {}) == CaseComparison::Exact);
    CHECK(comparisonOf(U"foo", {}) == CaseComparison::Folded);
    CHECK(comparisonOf(U"Foo", { .caseSensitivity = SearchCaseSensitivity::Insensitive })
          == CaseComparison::Folded);
}

TEST_CASE("searchQuery.escapeSearchRegex", "[searchquery]")
{
    auto const escaped = escapeSearchRegex(U"a.b*(c)");
    CHECK(escaped == U"a\\.b\\*\\(c\\)");
    CHECK(SearchQuery::compile(escaped, RegexOptions, Delimiters).has_value());

    // Every metacharacter, escaped, compiles: none is left to mean anything.
    CHECK(SearchQuery::compile(escapeSearchRegex(U"\\^$.|?*+()[]{}"), RegexOptions, Delimiters).has_value());
}
