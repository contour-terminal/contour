// SPDX-License-Identifier: Apache-2.0
#include <vtbackend/screen/SearchTallyWalk.hpp>
#include <vtbackend/testing/MockTerm.hpp>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <format>
#include <memory>
#include <optional>
#include <ranges>
#include <string>
#include <string_view>
#include <tuple>
#include <utility>

using namespace vtbackend;

namespace
{
/// A walk run to its end, and how many steps that took.
struct Drained
{
    SearchTallyWalk walk;
    int steps = 0;
};

/// Runs @p walk's steps over @p grid at @p budget until it finishes; every step must succeed.
Drained drain(SearchTallyWalk walk, Grid const& grid, LineCount budget)
{
    auto steps = 0;
    auto progress = WalkProgress::Running;
    while (progress == WalkProgress::Running)
    {
        auto const result = walk.step(grid, budget);
        REQUIRE(result.has_value());
        progress = *result;
        ++steps;
    }
    return Drained { .walk = std::move(walk), .steps = steps };
}

/// What the walk must arrive at: every segment's matches counted directly, top down, with no limit.
SearchMatchTally oracle(Grid const& grid, SearchQuery const& query, CellLocation position)
{
    auto tally = SearchMatchTally {};
    auto segment = std::optional { searchSegmentAt(
        grid, -boxed_cast<LineOffset>(grid.historyLineCount()), query.segmentCells()) };
    while (segment)
    {
        auto const matches = query.matchesIn(grid, *segment);
        REQUIRE(matches.has_value());
        for (auto const& match: *matches)
        {
            ++tally.total;
            if (match.first == position)
                tally.ordinal = tally.total;
        }
        segment = nextSearchSegment(grid, *segment, query.segmentCells());
    }
    return tally;
}

std::shared_ptr<SearchQuery const> compiled(std::u32string_view pattern, SearchOptions options = {})
{
    auto query = SearchQuery::compile(pattern, options, U" ");
    REQUIRE(query.has_value());
    return std::make_shared<SearchQuery const>(std::move(*query));
}

/// Writes @p count rows, row @p i reading @p rowText(i); each but the last ends in a line break.
template <typename RowText>
void writeRows(MockTerm<>& mock, int count, RowText rowText)
{
    auto text = std::string {};
    for (auto const row: std::views::iota(0, count))
    {
        text += rowText(row);
        if (row + 1 < count)
            text += "\r\n";
    }
    mock.writeToScreen(text);
}

/// The grid row offset of the row written @p index-th, the oldest still being in the history's top row.
LineOffset rowWritten(Grid const& grid, int index)
{
    return -boxed_cast<LineOffset>(grid.historyLineCount()) + LineOffset(index);
}

constexpr auto Regex = SearchOptions { .syntax = SearchSyntax::Regex };

/// 2 010 rows of a 10 x 20 page, so 2 000 of them in the history: "needle {i}" every seventh, "hay {i}" else.
std::unique_ptr<MockTerm<>> needlesAndHay()
{
    auto mock = std::make_unique<MockTerm<>>(PageSize { LineCount(10), ColumnCount(20) }, LineCount(3000));
    writeRows(*mock, 2010, [](int row) {
        return row % 7 == 0 ? std::format("needle {}", row) : std::format("hay {}", row);
    });
    return mock;
}
} // namespace

TEST_CASE("searchWalk.chunkedEqualsUnchunked", "[searchwalk]")
{
    auto const mock = needlesAndHay();
    auto const& grid = mock->terminal.primaryScreen().grid();
    REQUIRE(grid.historyLineCount() == LineCount(2000));
    auto const tenthNeedle = CellLocation { .line = rowWritten(grid, 63), .column = ColumnOffset(0) };
    REQUIRE(grid.lineText(tenthNeedle.line).starts_with("needle 63"));

    auto const patterns = std::array {
        std::tuple { std::u32string_view { U"needle" }, SearchOptions {} },
        std::tuple { std::u32string_view { U"ne+dle [0-9]*5\\b" }, Regex },
        std::tuple { std::u32string_view { U"e" }, SearchOptions {} },
    };
    for (auto const& [pattern, options]: patterns)
    {
        auto const query = compiled(pattern, options);
        auto const expected = oracle(grid, *query, tenthNeedle);
        REQUIRE(expected.total > 0);
        for (auto const budget: { LineCount(1), LineCount(7), LineCount(500) })
        {
            INFO(std::format("budget {}", budget));
            auto const drained = drain(SearchTallyWalk { query, grid, tenthNeedle }, grid, budget);
            CHECK(drained.walk.tally() == expected);
        }
    }
    CHECK(oracle(grid, *compiled(U"needle"), tenthNeedle).ordinal == 10);
}

TEST_CASE("searchWalk.aRegexStepFeedsTheEngineOneSegmentBudget", "[searchwalk]")
{
    // C1: `.*x` over a long run of `a` fails at every start without tripping libc++'s complexity limit, so
    // its cost grows with the square of what it is fed. One wrapped line of 3 200 cells: a step of a regex
    // walk feeds it RegexSegmentCells at most, however many rows the caller allows; a literal's is unbounded.
    auto constexpr Columns = 80;
    auto constexpr Rows = 40;
    auto mock = MockTerm<> { PageSize { LineCount(Rows), ColumnCount(Columns) }, LineCount(0) };
    mock.writeToScreen(std::string(std::size_t { Columns } * Rows, 'a'));
    auto const& grid = mock.terminal.primaryScreen().grid();
    REQUIRE(grid.isLineWrapped(LineOffset(Rows - 1)));

    auto regexWalk = SearchTallyWalk { compiled(U".*x", Regex), grid, CellLocation {} };
    auto steps = 0;
    auto progress = WalkProgress::Running;
    while (progress == WalkProgress::Running)
    {
        auto const result = regexWalk.step(grid, SearchWalkStepLines);
        REQUIRE(result.has_value());
        progress = *result;
        ++steps;
        CHECK(unbox<std::size_t>(regexWalk.rowsLastStep()) * Columns <= RegexSegmentCells);
    }
    CHECK(regexWalk.tally().total == 0);
    CHECK(steps == 2); // 25 rows (2 000 cells), then the other 15.

    auto literalWalk = SearchTallyWalk { compiled(U"x"), grid, CellLocation {} };
    REQUIRE(literalWalk.step(grid, SearchWalkStepLines).has_value());
    CHECK(literalWalk.rowsLastStep() == LineCount(Rows));
}

TEST_CASE("searchWalk.aStepReadsWholeSegmentsWithinItsBudget", "[searchwalk]")
{
    SECTION("one-row lines: no step reads more than its budget")
    {
        auto const mock = needlesAndHay();
        auto const& grid = mock->terminal.primaryScreen().grid();
        auto walk = SearchTallyWalk { compiled(U"needle"), grid, CellLocation {} };
        auto progress = WalkProgress::Running;
        while (progress == WalkProgress::Running)
        {
            auto const result = walk.step(grid);
            REQUIRE(result.has_value());
            progress = *result;
            CHECK(walk.rowsLastStep() <= SearchWalkStepLines);
        }
    }

    SECTION("a 600-row line is read whole in one step, and a match on its cut counted once")
    {
        // 600 rows of 100 columns: two segments, cut after 36 864 / 100 = 368 rows; "needle" spans the cut.
        auto mock = MockTerm { PageSize { LineCount(3), ColumnCount(100) }, LineCount(1000) };
        auto text = std::string(std::size_t { 100 } * 600, 'x');
        text.replace((std::size_t { 100 } * 368) - 3, 6, "needle");
        mock.writeToScreen(text);
        auto const& grid = mock.terminal.primaryScreen().grid();

        auto const drained =
            drain(SearchTallyWalk { compiled(U"needle"), grid, CellLocation {} }, grid, SearchWalkStepLines);
        CHECK(drained.steps == 1);
        CHECK(drained.walk.rowsLastStep() == LineCount(600));
        CHECK(drained.walk.tally().total == 1);
    }
}

TEST_CASE("searchWalk.aMatchAcrossTheStepBoundaryCountsOnce", "[searchwalk]")
{
    SECTION("rows a moved cut hands back to the walk are not counted again")
    {
        // One logical line of 600 rows of 200 columns: cut every 36 864 / 200 = 184 rows from its head.
        // Needles at its rows 100 and 200 and 500, and one across the first cut ("nee" ends row 183, "dle"
        // begins 184).
        auto constexpr Columns = std::size_t { 200 };
        auto line = std::string(Columns * 600, 'x');
        for (auto const row: { 100, 200, 500 })
            line.replace((static_cast<std::size_t>(row) * Columns) + 10, 6, "needle");
        line.replace((Columns * 184) - 3, 6, "needle");

        // 600 rows of history and 10 of page: the line and the row after it hold 601.
        auto mock = MockTerm { PageSize { LineCount(10), ColumnCount(200) }, LineCount(600) };
        mock.writeToScreen(line + "\r\n");
        auto const& grid = mock.terminal.primaryScreen().grid();
        auto walk = SearchTallyWalk { compiled(U"needle"), grid, CellLocation {} };

        // The first step reads the first segment, rows 0-183: the needle at 100 and the one across the cut.
        REQUIRE(walk.step(grid, LineCount(1)) == WalkProgress::Running);
        REQUIRE(walk.tally().total == 2);

        // 59 more rows evict the oldest 50: the history's top passes the line's head but not row 184, where
        // the walk resumes. The line's cuts now fall from its row 50, so the segment holding row 184 begins
        // at row 50 and holds both needles counted already.
        writeRows(mock, 59, [](int) { return std::string("hay"); });
        REQUIRE(grid.lineText(-boxed_cast<LineOffset>(grid.historyLineCount())).starts_with("xx"));

        auto const drained = drain(std::move(walk), grid, SearchWalkStepLines);
        CHECK(drained.walk.tally().total == 4);
    }

    SECTION("rows appended to the walk's last line are the next walk's")
    {
        // The bottom row's line goes on after the walk began, wrapping a needle onto a new row.
        auto mock = MockTerm { PageSize { LineCount(3), ColumnCount(20) }, LineCount(100) };
        mock.writeToScreen("hay\r\nhay\r\n" + std::string(17, 'x'));
        auto const& grid = mock.terminal.primaryScreen().grid();
        auto walk = SearchTallyWalk { compiled(U"needle"), grid, CellLocation {} };
        mock.writeToScreen("abcdefg needle");
        REQUIRE(grid.lineText(LineOffset(2)).starts_with("defg needle"));

        auto const drained = drain(std::move(walk), grid, SearchWalkStepLines);
        CHECK(drained.walk.tally().total == 0);
    }
}

TEST_CASE("searchWalk.aLongWrappedLineIsReadSegmentBySegment", "[searchwalk]")
{
    // One logical line of 5 000 rows of 200 columns, cut every 184 rows: a step of 500 rows reads three
    // segments, and only the first is found by walking back to the line's head -- the others from the one
    // before them. A walk per segment would be quadratic in the line's length.
    auto constexpr Columns = std::size_t { 200 };
    auto line = std::string(Columns * 5000, 'x');
    for (auto const at: std::views::iota(std::size_t { 1 }, std::size_t { 100 }))
        line.replace(at * 9973, 6, "needle");
    auto mock = MockTerm { PageSize { LineCount(10), ColumnCount(200) }, LineCount(6000) };
    mock.writeToScreen(line);
    auto const& grid = mock.terminal.primaryScreen().grid();
    auto const query = compiled(U"needle");

    auto const drained = drain(SearchTallyWalk { query, grid, CellLocation {} }, grid, SearchWalkStepLines);
    CHECK(drained.walk.headLookups() == static_cast<std::size_t>(drained.steps));
    CHECK(drained.steps < 5000 / 184);
    CHECK(drained.walk.tally() == oracle(grid, *query, CellLocation {}));
}

TEST_CASE("searchWalk.finishesWhileOutputStreamsIn", "[searchwalk]")
{
    // 1 200 rows, a needle every hundredth; 600 rows of hay arrive after every step.
    auto const streamWhileWalking = [](MockTerm<>& mock, LineCount budget) {
        writeRows(
            mock, 1200, [](int row) { return row % 100 == 0 ? std::string("needle") : std::string("hay"); });
        auto const& grid = mock.terminal.primaryScreen().grid();
        auto walk = SearchTallyWalk { compiled(U"needle"), grid, CellLocation {} };
        auto steps = 0;
        auto progress = WalkProgress::Running;
        while (progress == WalkProgress::Running)
        {
            auto const result = walk.step(grid, budget);
            REQUIRE(result.has_value());
            progress = *result;
            ++steps;
            mock.writeToScreen("\r\n");
            writeRows(mock, 600, [](int) { return std::string("hay"); });
        }
        return std::tuple { walk.tally(), steps };
    };

    SECTION("nothing is evicted: every match is counted, and rows appended since are not")
    {
        auto mock = MockTerm { PageSize { LineCount(10), ColumnCount(20) }, LineCount(10000) };
        auto const [tally, steps] = streamWhileWalking(mock, SearchWalkStepLines);
        CHECK(tally.total == 12);
        CHECK(steps <= ((1200 + 499) / 500) + 1);
    }

    SECTION("rows evicted before the walk reached them are skipped")
    {
        // A 1 500-row history and 100-row steps: after the first step, output evicts the oldest rows faster
        // than the walk reads them. It reads rows 0-99 (the needle at 0), resumes at the floor, row 290, and
        // reads 290-389 (the needle at 300); the next floor is 890, and it reads 890-989 (the needle at 900);
        // then the floor passes the walk's last row, 1 199, and the walk is done.
        auto mock = MockTerm { PageSize { LineCount(10), ColumnCount(20) }, LineCount(1500) };
        auto const [tally, steps] = streamWhileWalking(mock, LineCount(100));
        CHECK(tally.total == 3);
        CHECK(steps == 4);
    }
}

TEST_CASE("searchWalk.restartsWhenTheGridReflows", "[searchwalk]")
{
    auto mock = MockTerm { PageSize { LineCount(10), ColumnCount(20) }, LineCount(3000) };
    writeRows(
        mock, 1000, [](int row) { return row % 7 == 0 ? std::string("a needle here") : std::string("hay"); });
    auto& terminal = mock.terminal;
    auto const query = compiled(U"needle");
    auto walk = SearchTallyWalk { query, terminal.primaryScreen().grid(), CellLocation {} };
    REQUIRE(walk.step(terminal.primaryScreen().grid(), LineCount(100)) == WalkProgress::Running);

    auto const generation = terminal.primaryScreen().grid().stableIdGeneration();
    terminal.resizeScreen(PageSize { LineCount(10), ColumnCount(8) });
    auto const& grid = terminal.primaryScreen().grid();
    REQUIRE(grid.stableIdGeneration() != generation);

    auto const drained = drain(std::move(walk), grid, LineCount(100));
    CHECK(drained.walk.tally() == oracle(grid, *query, CellLocation {}));
}

TEST_CASE("searchWalk.aCatastrophicPatternEndsTheWalk", "[searchwalk]")
{
    // (a+)+$ over sixty a's and a b is exponential for a backtracking engine.
    auto constexpr Rows = 10000;
    auto mock = MockTerm { PageSize { LineCount(10), ColumnCount(80) }, LineCount(Rows) };
    writeRows(mock, Rows, [](int) { return std::string(60, 'a') + "b"; });
    auto& terminal = mock.terminal;
    auto const& grid = terminal.primaryScreen().grid();
    REQUIRE(terminal.setSearchOptions(Regex).has_value());
    REQUIRE(terminal.setNewSearchTerm(U"(a+)+$", SearchOrigin::Typed).has_value());

    auto walk = SearchTallyWalk { terminal.search().query, grid, CellLocation {} };
#ifdef __GLIBCXX__
    // libstdc++'s breadth-first executor is polynomial: the walk ends, having found nothing.
    auto const drained = drain(std::move(walk), grid, SearchWalkStepLines);
    CHECK(drained.walk.tally().total == 0);
    CHECK(drained.steps <= (Rows + 499) / 500);
#else
    // libc++ and MSVC give up within their own budget: the first step says so.
    auto const first = walk.step(grid);
    REQUIRE_FALSE(first.has_value());
    CHECK(first.error() == SearchPatternError::TooComplex);

    // Through the terminal, the query ends and the search says why.
    auto tally = terminal.beginSearchTally(CellLocation {});
    REQUIRE(tally.has_value());
    auto const through = terminal.continueSearchTally(*tally);
    REQUIRE_FALSE(through.has_value());
    CHECK(terminal.search().problem == SearchPatternError::TooComplex);
    CHECK(terminal.search().query == nullptr);
#endif
}

TEST_CASE("searchWalk.capsAtTheLimit", "[searchwalk]")
{
    auto mock = MockTerm { PageSize { LineCount(10), ColumnCount(20) }, LineCount(10) };
    writeRows(mock, 5, [](int) { return std::string("needle"); });
    auto const& grid = mock.terminal.primaryScreen().grid();
    auto walk = SearchTallyWalk { compiled(U"needle"), grid, CellLocation {}, 3 };
    CHECK(walk.step(grid) == WalkProgress::Finished);
    CHECK(walk.tally() == SearchMatchTally { .total = 3, .ordinal = 1, .exactness = TallyExactness::Capped });
}

TEST_CASE("searchWalk.remembersTheNearestMatchBehindThePosition", "[searchwalk]")
{
    // 1 010 rows on a 10-row page: the row written k-th is at offset k - 1 000.
    auto mock = MockTerm { PageSize { LineCount(10), ColumnCount(20) }, LineCount(1000) };
    writeRows(mock, 1010, [](int row) {
        return row == 200 || row == 700 || row == 1005 ? std::string("needle") : std::string("hay");
    });
    auto const& grid = mock.terminal.primaryScreen().grid();
    REQUIRE(grid.historyLineCount() == LineCount(1000));

    auto const drained =
        drain(SearchTallyWalk { compiled(U"needle"), grid, CellLocation {} }, grid, SearchWalkStepLines);
    CHECK(drained.walk.nearestAtOrBefore(grid)
          == CellLocation { .line = LineOffset(-300), .column = ColumnOffset(0) });
    CHECK(drained.walk.lastMatch(grid) == CellLocation { .line = LineOffset(5), .column = ColumnOffset(0) });
    // Its ordinal, so a jump to it reads "2 of 3" at once.
    CHECK(drained.walk.nearestAtOrBeforeOrdinal() == 2);
}

TEST_CASE("probe.stopsAtItsBudget", "[searchwalk]")
{
    // The only needle is 800 rows above the bottom row.
    auto mock = MockTerm { PageSize { LineCount(10), ColumnCount(20) }, LineCount(1000) };
    writeRows(mock, 1000, [](int row) { return row == 199 ? std::string("needle") : std::string("hay"); });
    auto& terminal = mock.terminal;
    REQUIRE(terminal.setNewSearchTerm(U"needle", SearchOrigin::Typed).has_value());
    auto const bottom = CellLocation { .line = LineOffset(9), .column = ColumnOffset(19) };
    auto const needle = CellLocation { .line = LineOffset(-791), .column = ColumnOffset(0) };
    REQUIRE(terminal.primaryScreen().grid().lineText(needle.line).starts_with("needle"));

    auto const bounded = terminal.probeSearchReverse(bottom, LineCount(500));
    CHECK(bounded.outcome == SearchProbeOutcome::Undecided);
    CHECK_FALSE(bounded.match.has_value());

    auto const wider = terminal.probeSearchReverse(bottom, LineCount(1000));
    CHECK(wider.outcome == SearchProbeOutcome::Found);
    CHECK(wider.match == needle);
}

TEST_CASE("probe.aGridWithinTheBudgetIsAnsweredWhole", "[searchwalk]")
{
    // F8: where the probe can read every row, "nothing behind the cursor" is not the end of it.
    auto mock = MockTerm { PageSize { LineCount(10), ColumnCount(20) }, LineCount(100) };
    mock.writeToScreen("hay\r\nneedle\r\nhay");
    auto& terminal = mock.terminal;
    auto const top = CellLocation {};

    SECTION("a match below the cursor is found: the last one, as a finished walk would wrap to")
    {
        REQUIRE(terminal.setNewSearchTerm(U"needle", SearchOrigin::Typed).has_value());
        auto const probe = terminal.probeSearchReverse(top);
        CHECK(probe.outcome == SearchProbeOutcome::Found);
        CHECK(probe.match == CellLocation { .line = LineOffset(1), .column = ColumnOffset(0) });
    }

    SECTION("no match anywhere is proved")
    {
        REQUIRE(terminal.setNewSearchTerm(U"haystack", SearchOrigin::Typed).has_value());
        auto const probe = terminal.probeSearchReverse(top);
        CHECK(probe.outcome == SearchProbeOutcome::Absent);
        CHECK_FALSE(probe.match.has_value());
    }
}

TEST_CASE("probe.aRegexProvesAbsenceOnAPageTallerThanOneSegment", "[searchwalk]")
{
    // Q1: the probe's bound for a regex is what it costs the engine, not one segment's rows. At 200 columns a
    // regex segment is 10 rows; a page of 30 short lines costs 30 x 200 squared, far under RegexSegmentCells
    // squared, so a keystroke still proves a regex absent there. One line wrapped over all 30 rows is three
    // full segments, which a keystroke does not read: undecided, for the walk to settle.
    auto constexpr Columns = 200;
    auto constexpr Rows = 30;
    auto const bottom = CellLocation { .line = LineOffset(Rows - 1), .column = ColumnOffset(Columns - 1) };

    auto shortLines = MockTerm { PageSize { LineCount(Rows), ColumnCount(Columns) }, LineCount(0) };
    writeRows(shortLines, Rows, [](int) { return std::string("hay"); });
    REQUIRE(shortLines.terminal.setSearchOptions(Regex).has_value());
    REQUIRE(shortLines.terminal.setNewSearchTerm(U"z+", SearchOrigin::Typed).has_value());
    CHECK(shortLines.terminal.probeSearchReverse(bottom).outcome == SearchProbeOutcome::Absent);

    auto longLine = MockTerm { PageSize { LineCount(Rows), ColumnCount(Columns) }, LineCount(0) };
    longLine.writeToScreen(std::string(std::size_t { Columns } * Rows, 'h'));
    REQUIRE(longLine.terminal.setSearchOptions(Regex).has_value());
    REQUIRE(longLine.terminal.setNewSearchTerm(U"z+", SearchOrigin::Typed).has_value());
    CHECK(longLine.terminal.probeSearchReverse(bottom).outcome == SearchProbeOutcome::Undecided);
    // A literal's probe is bounded by rows alone.
    REQUIRE(longLine.terminal.setSearchOptions({}).has_value());
    CHECK(longLine.terminal.probeSearchReverse(bottom).outcome == SearchProbeOutcome::Absent);
}

TEST_CASE("searchWalk.aRegexStepStartsNoSecondLongSegment", "[searchwalk]")
{
    // A step that has read a short line does not go on to a regex segment longer than half the budget: one
    // segment's cost per step, not two.
    auto constexpr Columns = 80;
    auto mock = MockTerm<> { PageSize { LineCount(30), ColumnCount(Columns) }, LineCount(0) };
    mock.writeToScreen("x\r\n" + std::string(std::size_t { Columns } * 25, 'a'));
    auto const& grid = mock.terminal.primaryScreen().grid();

    auto walk = SearchTallyWalk { compiled(U"q", Regex), grid, CellLocation {} };
    REQUIRE(walk.step(grid, SearchWalkStepLines).has_value());
    CHECK(walk.rowsLastStep() == LineCount(1));
    // The next begins with it, and goes on to the four empty rows below, each a short segment that costs
    // little.
    REQUIRE(walk.step(grid, SearchWalkStepLines).has_value());
    CHECK(walk.rowsLastStep() == LineCount(29));
}

TEST_CASE("search.theFocusedMatchIsReadThroughTheMemo", "[searchwalk]")
{
    // Every walk step refreshes the current match; a regex runs over the cursor's segment once for all of
    // them.
    auto mock = MockTerm<> { PageSize { LineCount(3), ColumnCount(10) }, LineCount(0) };
    mock.writeToScreen("abc");
    auto& terminal = mock.terminal;
    REQUIRE(terminal.setSearchOptions(Regex).has_value());
    REQUIRE(terminal.setNewSearchTerm(U"ab.", SearchOrigin::Typed).has_value());
    terminal.moveNormalModeCursorTo(CellLocation {});
    auto const& memo = terminal.searchMatchMemo();

    REQUIRE(terminal.focusedSearchMatch().has_value());
    CHECK(memo.engineRuns() == 1);
    REQUIRE(terminal.focusedSearchMatch().has_value());
    CHECK(memo.engineRuns() == 1);

    // A new query starts the memo over.
    REQUIRE(terminal.setNewSearchTerm(U"a.c", SearchOrigin::Typed).has_value());
    REQUIRE(terminal.focusedSearchMatch().has_value());
    CHECK(memo.engineRuns() == 2);
}

TEST_CASE("searchWalk.theTerminalRestartsTheWalkOnAnotherPage", "[searchwalk]")
{
    auto mock = MockTerm { PageSize { LineCount(3), ColumnCount(20) }, LineCount(10) };
    mock.writeToScreen("needle needle");
    auto& terminal = mock.terminal;
    REQUIRE(terminal.setNewSearchTerm(U"needle", SearchOrigin::Typed).has_value());
    auto tally = terminal.beginSearchTally(CellLocation {});
    REQUIRE(tally.has_value());

    // The alternate screen is another page, holding no needle.
    mock.writeToScreen("\033[?1049h");
    REQUIRE(terminal.displayedPageIndex() != tally->page);
    CHECK(terminal.continueSearchTally(*tally) == WalkProgress::Finished);
    CHECK(tally->page == terminal.displayedPageIndex());
    CHECK(tally->walk.tally().total == 0);
}

TEST_CASE("searchWalk.countingSteppingAndTheCurrentMatchCutARegexAlike", "[searchwalk]")
{
    // One wrapped line, `ab` straddling the regex cut after 25 rows (2 000 of 80-column cells): no reader
    // finds it as a regex, every reader finds it as a literal, which reads on past a cut.
    auto constexpr Columns = 80;
    auto constexpr Rows = 30;
    auto constexpr Cut = 25 * Columns;
    auto mock = MockTerm<> { PageSize { LineCount(Rows), ColumnCount(Columns) }, LineCount(0) };
    auto text = std::string(std::size_t { Columns } * Rows, '.');
    text[Cut - 1] = 'a';
    text[Cut] = 'b';
    mock.writeToScreen(text);
    auto& terminal = mock.terminal;
    auto const at = CellLocation { .line = LineOffset(24), .column = ColumnOffset(Columns - 1) };

    for (auto const& [options, found]: { std::pair { Regex, false }, std::pair { SearchOptions {}, true } })
    {
        INFO(std::format("regex: {}", options.syntax == SearchSyntax::Regex));
        REQUIRE(terminal.setSearchOptions(options).has_value());
        REQUIRE(terminal.setNewSearchTerm(U"ab", SearchOrigin::Typed).has_value());
        CHECK((terminal.tallySearchMatches(at).total == 1) == found);
        CHECK(terminal.search(CellLocation {}).has_value() == found);
        terminal.moveNormalModeCursorTo(at);
        CHECK(terminal.focusedSearchMatch().has_value() == found);
    }
}
