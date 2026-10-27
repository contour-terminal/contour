// SPDX-License-Identifier: Apache-2.0
#include <vtbackend/grid/GraphemeClusters.hpp>
#include <vtbackend/screen/SearchQuery.hpp>
#include <vtbackend/testing/MockTerm.hpp>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

using vtbackend::CaseComparison;
using vtbackend::CellFlag;
using vtbackend::ColumnCount;
using vtbackend::escapeSearchRegex;
using vtbackend::Grid;
using vtbackend::LineCount;
using vtbackend::LineOffset;
using vtbackend::MockTerm;
using vtbackend::PageSize;
using vtbackend::SearchCaseSensitivity;
using vtbackend::SearchOptions;
using vtbackend::SearchPatternError;
using vtbackend::SearchQuery;
using vtbackend::SearchSegment;
using vtbackend::SearchSyntax;
using vtbackend::SearchWordBoundary;

namespace
{
constexpr auto Delimiters = std::u32string_view { U" " };
constexpr auto RegexOptions = SearchOptions { .syntax = SearchSyntax::Regex };

/// The delimiters the matching cases use: a space, a dot and a slash.
constexpr auto WordDelimiters = std::u32string_view { U" ./" };

/// A match as (first line, first column, last line, last column), so a mismatch prints what it found.
using Span = std::array<int, 4>;

/// The matches @p pattern finds in the segment holding @p row of @p grid, as spans.
std::vector<Span> spansOf(Grid const& grid, int row, std::u32string_view pattern, SearchOptions options = {})
{
    auto const query = SearchQuery::compile(pattern, options, WordDelimiters);
    REQUIRE(query.has_value());
    auto const matches =
        query->matchesIn(grid, vtbackend::searchSegmentAt(grid, LineOffset(row), query->segmentCells()));
    REQUIRE(matches.has_value());
    auto spans = std::vector<Span> {};
    for (auto const& match: *matches)
        spans.push_back(Span { unbox(match.first.line),
                               unbox(match.first.column),
                               unbox(match.second.line),
                               unbox(match.second.column) });
    return spans;
}

/// A terminal of @p lines x @p columns showing @p text.
std::unique_ptr<MockTerm<>> termWith(int lines, int columns, std::string_view text)
{
    auto mock =
        std::make_unique<MockTerm<>>(PageSize { LineCount(lines), ColumnCount(columns) }, LineCount(10));
    mock->writeToScreen(text);
    return mock;
}

[[nodiscard]] Grid const& gridOf(std::unique_ptr<MockTerm<>> const& mock)
{
    return mock->terminal.primaryScreen().grid();
}
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

TEST_CASE("searchQuery.literalCountsOverlappingStarts", "[searchquery]")
{
    auto const mock = termWith(1, 10, "aaa");
    CHECK(spansOf(gridOf(mock), 0, U"aa") == std::vector<Span> { { 0, 0, 0, 1 }, { 0, 1, 0, 2 } });
}

TEST_CASE("searchQuery.regexMatchesWithinASegment", "[searchquery]")
{
    auto const mock = termWith(2, 10, "abc 12 de 345"); // wraps after "de "
    CHECK(spansOf(gridOf(mock), 0, U"[0-9]+", RegexOptions)
          == std::vector<Span> { { 0, 4, 0, 5 }, { 1, 0, 1, 2 } });
}

TEST_CASE("searchQuery.aRegexSpansASoftWrapButNotAHardBreak", "[searchquery]")
{
    SECTION("a soft wrap is one segment")
    {
        auto const mock = termWith(2, 10, "abc 12 de 345");
        CHECK(vtbackend::searchSegmentAt(gridOf(mock), LineOffset(0), vtbackend::RegexSegmentCells)
              == SearchSegment { .first = LineOffset(0), .last = LineOffset(1) });
        CHECK(spansOf(gridOf(mock), 0, U"de 3", RegexOptions) == std::vector<Span> { { 0, 7, 1, 0 } });
    }

    SECTION("a hard break ends one")
    {
        auto const mock = termWith(2, 10, "abc 12 de\r\n345");
        CHECK(vtbackend::searchSegmentAt(gridOf(mock), LineOffset(1), vtbackend::RegexSegmentCells)
              == SearchSegment { .first = LineOffset(1), .last = LineOffset(1) });
        CHECK(spansOf(gridOf(mock), 0, U"de.*3", RegexOptions).empty());
        CHECK(spansOf(gridOf(mock), 1, U"de.*3", RegexOptions).empty());
    }
}

TEST_CASE("searchQuery.wholeWordUsesTheWordDelimiters", "[searchquery]")
{
    auto const mock = termWith(1, 30, "foo foobar barfoo foo.");
    auto constexpr WholeWord = SearchOptions { .wordBoundary = SearchWordBoundary::WholeWord };
    auto constexpr WholeWordRegex =
        SearchOptions { .syntax = SearchSyntax::Regex, .wordBoundary = SearchWordBoundary::WholeWord };
    auto const wholeWords = std::vector<Span> { { 0, 0, 0, 2 }, { 0, 18, 0, 20 } };

    CHECK(spansOf(gridOf(mock), 0, U"foo", WholeWord) == wholeWords);
    CHECK(spansOf(gridOf(mock), 0, U"[f]o+", WholeWordRegex) == wholeWords);
    CHECK(spansOf(gridOf(mock), 0, U"foo").size() == 4);
    CHECK(spansOf(gridOf(mock), 0, U"[f]o+", RegexOptions).size() == 4);
}

TEST_CASE("searchQuery.wholeWordLooksAcrossASoftWrap", "[searchquery]")
{
    auto constexpr WholeWord = SearchOptions { .wordBoundary = SearchWordBoundary::WholeWord };
    CHECK(spansOf(gridOf(termWith(2, 4, "ab foo")), 0, U"foo", WholeWord)
          == std::vector<Span> { { 0, 3, 1, 1 } });
    CHECK(spansOf(gridOf(termWith(2, 4, "ab foox")), 0, U"foo", WholeWord).empty());
}

TEST_CASE("searchQuery.wideAndCombiningCharacters", "[searchquery]")
{
    // Cells: 日 0-1, 本 2-3, 語 4-5, a space at 6, c 7, a 8, f 9, é 10. The columns are the grid's own
    // measure of the text before each, so the expectations are not hand-counted.
    auto const columnOf = [](std::string_view textBefore) {
        return unbox<int>(
            vtbackend::columnsOnTheGrid(textBefore, vtbackend::ClusterWidthPolicy::ClusterAware));
    };
    auto const c = columnOf("\xE6\x97\xA5\xE6\x9C\xAC\xE8\xAA\x9E "); // "日本語 "
    REQUIRE(c == 7);

    // é precomposed (U+00E9), and decomposed (e, U+0301) as macOS's file names print it.
    for (auto const* const cafe: { "caf\xC3\xA9", "cafe\xCC\x81" })
    {
        INFO(cafe);
        auto const mock = termWith(1, 12, std::string("\xE6\x97\xA5\xE6\x9C\xAC\xE8\xAA\x9E ") + cafe);
        auto const& grid = gridOf(mock);

        CHECK(spansOf(grid, 0, U"本語") == std::vector<Span> { { 0, 2, 0, 5 } });
        CHECK(spansOf(grid, 0, U"日.", RegexOptions) == std::vector<Span> { { 0, 0, 0, 3 } });
        CHECK(spansOf(grid, 0, U"語 c", RegexOptions) == std::vector<Span> { { 0, 4, 0, c } });

        CHECK(spansOf(grid, 0, U"caf\u00E9") == std::vector<Span> { { 0, c, 0, c + 3 } });
        CHECK(spansOf(grid, 0, U"cafe\u0301") == std::vector<Span> { { 0, c, 0, c + 3 } });
        CHECK(spansOf(grid, 0, U"ca[f].", RegexOptions) == std::vector<Span> { { 0, c, 0, c + 3 } });

        // The cell is é, not e: no match ends inside its cluster.
        CHECK(spansOf(grid, 0, U"cafe").empty());
        CHECK(spansOf(grid, 0, U"cafe", RegexOptions).empty());
        CHECK(spansOf(grid, 0, U"e", RegexOptions).empty());
    }

    // A cluster NFC cannot compose -- e and U+20DD COMBINING ENCLOSING CIRCLE -- stays two codepoints in one
    // cell, and still no match begins or ends between them.
    auto const enclosed = termWith(1, 12, "xe\xE2\x83\x9D");
    CHECK(spansOf(gridOf(enclosed), 0, U"xe").empty());
    CHECK(spansOf(gridOf(enclosed), 0, U"xe", RegexOptions).empty());
    CHECK(spansOf(gridOf(enclosed), 0, U"\u20DD", RegexOptions).empty());
    CHECK(spansOf(gridOf(enclosed), 0, U"xe.", RegexOptions) == std::vector<Span> { { 0, 0, 0, 1 } });
}

TEST_CASE("searchQuery.blankRowsAndPaddingAreNotText", "[searchquery]")
{
    SECTION("the padding a wide character leaves at a row's end")
    {
        // "abcd", padding, and the wide character on the row that continues it.
        auto const mock = termWith(2, 5, "abcd\xE4\xB8\xAD");
        REQUIRE(gridOf(mock).lineAt(LineOffset(0)).storage().sgr[4].flags.contains(CellFlag::WrapPadding));
        CHECK(spansOf(gridOf(mock), 0, U"d中") == std::vector<Span> { { 0, 3, 1, 1 } });
        CHECK(spansOf(gridOf(mock), 0, U"d.", RegexOptions) == std::vector<Span> { { 0, 3, 1, 1 } });
    }

    // "xxxa", a continuation row erased to nothing, and "bzzz" continuing that: a row with no text is one
    // line break, so nothing matches across it, whether the erase left its storage materialized or not.
    auto const blankRowBetween = [](std::string_view erase) {
        auto mock = termWith(3, 4, "xxxayyyybzzz");
        mock->writeToScreen("\033[2;1H");
        mock->writeToScreen(erase);
        return mock;
    };

    SECTION("an erased line")
    {
        auto const mock = blankRowBetween("\033[2K");
        REQUIRE(gridOf(mock).isLineWrapped(LineOffset(1)));
        REQUIRE(gridOf(mock).isLineWrapped(LineOffset(2)));
        CHECK(spansOf(gridOf(mock), 0, U"a.b", RegexOptions).empty());
        CHECK(spansOf(gridOf(mock), 0, U"a\\s*b", RegexOptions).empty());
        CHECK(spansOf(gridOf(mock), 0, U"a[^x]b", RegexOptions).empty());
        CHECK(spansOf(gridOf(mock), 0, U"a[\\s\\S]*b", RegexOptions).empty());
        CHECK(spansOf(gridOf(mock), 0, U"b") == std::vector<Span> { { 2, 0, 2, 0 } });
    }

    SECTION("erased characters")
    {
        auto const mock = blankRowBetween("\033[4X");
        REQUIRE_FALSE(gridOf(mock).lineAt(LineOffset(1)).isBlank());
        REQUIRE(gridOf(mock).isLineWrapped(LineOffset(1)));
        CHECK(spansOf(gridOf(mock), 0, U"a.b", RegexOptions).empty());
        CHECK(spansOf(gridOf(mock), 0, U"a\\s*b", RegexOptions).empty());
        CHECK(spansOf(gridOf(mock), 0, U"a[^x]b", RegexOptions).empty());
        CHECK(spansOf(gridOf(mock), 0, U"a[\\s\\S]*b", RegexOptions).empty());
    }
}

TEST_CASE("searchQuery.zeroLengthAndAnchors", "[searchquery]")
{
    auto const mock = termWith(1, 10, "bab");
    CHECK(spansOf(gridOf(mock), 0, U"a*", RegexOptions) == std::vector<Span> { { 0, 1, 0, 1 } });
    CHECK(spansOf(gridOf(mock), 0, U"^b", RegexOptions) == std::vector<Span> { { 0, 0, 0, 0 } });
    // The row's trailing empty cells are no text, so the segment ends right after the last "b".
    CHECK(spansOf(gridOf(mock), 0, U"b$", RegexOptions) == std::vector<Span> { { 0, 2, 0, 2 } });
}

TEST_CASE("searchQuery.foldedRegexIsUnicodeAware", "[searchquery]")
{
    CHECK(spansOf(gridOf(termWith(1, 10, "\xD0\x9F\xD0\xA0\xD0\x98\xD0\x92\xD0\x95\xD0\xA2")), // ПРИВЕТ
                  0,
                  U"привет",
                  RegexOptions)
          == std::vector<Span> { { 0, 0, 0, 5 } });
    CHECK(spansOf(gridOf(termWith(1, 10, "12ab")), 0, U"\\D+", RegexOptions)
          == std::vector<Span> { { 0, 2, 0, 3 } });
}

TEST_CASE("searchQuery.segmentsCutALongLineAtFixedRows", "[searchquery]")
{
    // One logical line 400 rows long, 200 columns wide: a literal's segment holds 36 864 / 200 = 184 of its
    // rows.
    auto constexpr Literal = vtbackend::MaxScanCells;
    auto constexpr Columns = 200;
    auto constexpr Rows = 400;
    auto const mock = termWith(Rows, Columns, std::string(std::size_t { Columns } * Rows, 'x'));
    auto const& grid = gridOf(mock);
    auto const segment = [](int first, int last) {
        return SearchSegment { .first = LineOffset(first), .last = LineOffset(last) };
    };

    CHECK(vtbackend::searchSegmentAt(grid, LineOffset(0), Literal) == segment(0, 183));
    CHECK(vtbackend::searchSegmentAt(grid, LineOffset(183), Literal) == segment(0, 183));
    CHECK(vtbackend::searchSegmentAt(grid, LineOffset(184), Literal) == segment(184, 367));
    CHECK(vtbackend::nextSearchSegment(grid, segment(0, 183), Literal) == segment(184, 367));
    CHECK(vtbackend::nextSearchSegment(grid, segment(368, Rows - 1), Literal) == std::nullopt);
    CHECK(vtbackend::previousSearchSegment(grid, segment(184, 367), Literal) == segment(0, 183));
    CHECK(vtbackend::previousSearchSegment(grid, segment(0, 183), Literal) == std::nullopt);
    // Stepping within the line, both ways, lands on the cuts searchSegmentAt() makes.
    CHECK(vtbackend::nextSearchSegment(grid, segment(184, 367), Literal) == segment(368, Rows - 1));
    CHECK(vtbackend::previousSearchSegment(grid, segment(368, Rows - 1), Literal) == segment(184, 367));
    CHECK(vtbackend::searchSegmentAt(grid, LineOffset(Rows - 1), Literal) == segment(368, Rows - 1));
}

TEST_CASE("searchQuery.aLiteralSpansASegmentCutButARegexDoesNot", "[searchquery]")
{
    // 200 columns: a literal's segment is 184 rows (MaxScanCells), a regex's 10 (RegexSegmentCells). One
    // "needle" straddles each cut: rows 9 and 10, rows 183 and 184.
    auto constexpr Columns = 200;
    auto constexpr Rows = 400;
    auto text = std::string(std::size_t { Columns } * Rows, 'x');
    text.replace((std::size_t { Columns } * 10) - 3, 6, "needle");
    text.replace((std::size_t { Columns } * 184) - 3, 6, "needle");
    auto const mock = termWith(Rows, Columns, text);
    auto const& grid = gridOf(mock);

    // A literal finds each once, by the segment it begins in.
    CHECK(spansOf(grid, 0, U"needle") == std::vector<Span> { { 9, 197, 10, 2 }, { 183, 197, 184, 2 } });
    CHECK(spansOf(grid, 184, U"needle").empty());
    // A regex finds neither across its own cut, and the second within one of its segments.
    CHECK(spansOf(grid, 0, U"needle", RegexOptions).empty());
    CHECK(spansOf(grid, 10, U"needle", RegexOptions).empty());
    CHECK(spansOf(grid, 10, U"dle", RegexOptions) == std::vector<Span> { { 10, 0, 10, 2 } });
    CHECK(spansOf(grid, 180, U"needle", RegexOptions) == std::vector<Span> { { 183, 197, 184, 2 } });
}

TEST_CASE("searchQuery.wholeWordResumesAtTheNextDelimiter", "[searchquery]")
{
    // A rejected candidate skips to the next word start; what is found is what stepping one unit at a time
    // finds.
    auto constexpr WholeWordRegex =
        SearchOptions { .syntax = SearchSyntax::Regex, .wordBoundary = SearchWordBoundary::WholeWord };
    auto const mock = termWith(1, 30, "afoo foo bfoo foo.xfoo fooo");
    CHECK(spansOf(gridOf(mock), 0, U"f[o]+", WholeWordRegex)
          == std::vector<Span> { { 0, 5, 0, 7 }, { 0, 14, 0, 16 }, { 0, 23, 0, 26 } });
    CHECK(spansOf(gridOf(mock), 0, U"o+", WholeWordRegex).empty());
    CHECK(spansOf(gridOf(mock), 0, U"[a-z]*o", WholeWordRegex).size() == 6);
}

TEST_CASE("searchQuery.aFoldedClassRangeKeepsItsEnds", "[searchquery]")
{
    auto constexpr InsensitiveRegex = SearchOptions { .caseSensitivity = SearchCaseSensitivity::Insensitive,
                                                      .syntax = SearchSyntax::Regex };
    auto const mock = termWith(1, 20, "_ q Q @ a Z");

    // [@-Z] is @ and the capitals; folded it takes their lowercase too, and never the _ between them.
    CHECK(spansOf(gridOf(mock), 0, U"[@-Z]", InsensitiveRegex)
          == std::vector<Span> {
              { 0, 2, 0, 2 }, { 0, 4, 0, 4 }, { 0, 6, 0, 6 }, { 0, 8, 0, 8 }, { 0, 10, 0, 10 } });

    // [Z-a] runs from Z up to a, past _; folding its ends must not reverse it.
    auto const zToA = SearchQuery::compile(U"[Z-a]", InsensitiveRegex, WordDelimiters);
    REQUIRE(zToA.has_value());
    CHECK(spansOf(gridOf(mock), 0, U"[Z-a]", InsensitiveRegex)
          == std::vector<Span> { { 0, 0, 0, 0 }, { 0, 8, 0, 8 }, { 0, 10, 0, 10 } });
}

TEST_CASE("searchQuery.aClassesLeadingCaretIsNoRangeEnd", "[searchquery]")
{
    // The ^ right after [ negates the class; it is no low end of a range with the - after it.
    auto constexpr InsensitiveRegex = SearchOptions { .caseSensitivity = SearchCaseSensitivity::Insensitive,
                                                      .syntax = SearchSyntax::Regex };
    auto const mock = termWith(1, 10, "a-Zz");
    CHECK(spansOf(gridOf(mock), 0, U"[^-z]", InsensitiveRegex) == std::vector<Span> { { 0, 0, 0, 0 } });
    CHECK(spansOf(gridOf(mock), 0, U"[^-Z]", InsensitiveRegex) == std::vector<Span> { { 0, 0, 0, 0 } });
    CHECK(SearchQuery::compile(U"[^-!]", RegexOptions, WordDelimiters).has_value());
}
