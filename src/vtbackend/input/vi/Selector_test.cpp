// SPDX-License-Identifier: Apache-2.0
#include <vtbackend/input/vi/Selector.hpp>
#include <vtbackend/screen/Screen.hpp>
#include <vtbackend/testing/MockTerm.hpp>

#include <catch2/catch_test_macros.hpp>

using namespace std;
using namespace std::placeholders;
using namespace vtbackend;

namespace
{

struct TestSelectionHelper: public vtbackend::SelectionHelper
{
    Screen* screen;
    explicit TestSelectionHelper(Screen& self): screen { &self } {}

    [[nodiscard]] PageSize pageSize() const noexcept override { return screen->pageSize(); }
    [[nodiscard]] bool wrappedLine(LineOffset line) const noexcept override
    {
        return screen->isLineWrapped(line);
    }
    /// Blank as the terminal's own helper has it: nothing written, or a space -- what a copy trims off a
    /// row's end.
    [[nodiscard]] bool cellEmpty(CellLocation pos) const noexcept override
    {
        return screen->isCellEmpty(pos);
    }
    [[nodiscard]] int cellWidth(CellLocation pos) const noexcept override { return screen->at(pos).width(); }
};

} // namespace

// Different cases to test
// - single cell
// - inside single line
// - multiple lines
// - multiple lines fully in history
// - multiple lines from history into main buffer
// all of the above with and without scrollback != 0.

namespace
{
[[maybe_unused]] void logScreenTextAlwaysLocal(Screen const& screen, string const& headline = "")
{
    std::cout << std::format("{}: ZI={} cursor={} HM={}..{}\n",
                             headline.empty() ? "screen dump"s : headline,
                             screen.grid().zeroIndex(),
                             screen.realCursorPosition(),
                             screen.margin().horizontal.from,
                             screen.margin().horizontal.to);
    std::cout << std::format("{}\n", dumpGrid(screen.grid()));
}

struct TextSelection
{
    Screen const* screen;
    string text;
    ColumnOffset lastColumn = ColumnOffset(0);

    explicit TextSelection(Screen const& s): screen { &s } {}

    void operator()(CellLocation const& pos)
    {
        auto const& cell = screen->at(pos);
        text += pos.column < lastColumn ? "\n" : "";
        text += cell.toUtf8();
        lastColumn = pos.column;
    }
};
} // namespace

// NOLINTBEGIN(misc-const-correctness,readability-function-cognitive-complexity)
TEST_CASE("Selector.Linear", "[selector]")
{
    auto term = MockTerm(PageSize { LineCount(3), ColumnCount(11) }, LineCount(5));
    auto& screen = term.terminal.primaryScreen();
    auto selectionHelper = TestSelectionHelper(screen);
    term.writeToScreen(
        //       0123456789A
        /* 0 */ "12345,67890"s +
        /* 1 */ "ab,cdefg,hi"s +
        /* 2 */ "12345,67890"s);

    REQUIRE(screen.grid().lineText(LineOffset(0)) == "12345,67890");
    REQUIRE(screen.grid().lineText(LineOffset(1)) == "ab,cdefg,hi");
    REQUIRE(screen.grid().lineText(LineOffset(2)) == "12345,67890");

    SECTION("single-cell")
    { // "b"
        auto const pos = CellLocation { .line = LineOffset(1), .column = ColumnOffset(1) };
        auto selector = LinearSelection(selectionHelper, pos, []() {});
        (void) selector.extend(pos);
        selector.complete();

        vector<Selection::Range> const selection = selector.ranges();
        REQUIRE(selection.size() == 1);
        Selection::Range const& r1 = selection[0];
        CHECK(r1.line == pos.line);
        CHECK(r1.fromColumn == pos.column);
        CHECK(r1.toColumn == pos.column);
        CHECK(r1.length() == ColumnCount(1));

        auto selectedText = TextSelection { screen };
        renderSelection(selector, selectedText);
        CHECK(selectedText.text == "b");
    }

    SECTION("forward single-line")
    { // "b,c"
        auto const pos = CellLocation { .line = LineOffset(1), .column = ColumnOffset(1) };
        auto selector = LinearSelection(selectionHelper, pos, []() {});
        (void) selector.extend(CellLocation { .line = LineOffset(1), .column = ColumnOffset(3) });
        selector.complete();

        vector<Selection::Range> const selection = selector.ranges();
        REQUIRE(selection.size() == 1);
        Selection::Range const& r1 = selection[0];
        CHECK(r1.line == LineOffset(1));
        CHECK(r1.fromColumn == ColumnOffset(1));
        CHECK(r1.toColumn == ColumnOffset(3));
        CHECK(r1.length() == ColumnCount(3));

        auto selectedText = TextSelection { screen };
        renderSelection(selector, selectedText);
        CHECK(selectedText.text == "b,c");
    }

    SECTION("forward multi-line")
    { // "b,cdefg,hi\n1234"
        auto const pos = CellLocation { .line = LineOffset(1), .column = ColumnOffset(1) };
        auto selector = LinearSelection(selectionHelper, pos, []() {});
        (void) selector.extend(CellLocation { .line = LineOffset(2), .column = ColumnOffset(3) });
        selector.complete();

        vector<Selection::Range> const selection = selector.ranges();
        REQUIRE(selection.size() == 2);

        Selection::Range const& r1 = selection[0];
        CHECK(r1.line == LineOffset(1));
        CHECK(r1.fromColumn == ColumnOffset(1));
        CHECK(r1.toColumn == ColumnOffset(10));
        CHECK(r1.length() == ColumnCount(10));

        Selection::Range const& r2 = selection[1];
        CHECK(r2.line == LineOffset(2));
        CHECK(r2.fromColumn == ColumnOffset(0));
        CHECK(r2.toColumn == ColumnOffset(3));
        CHECK(r2.length() == ColumnCount(4));

        auto selectedText = TextSelection { screen };
        renderSelection(selector, selectedText);
        CHECK(selectedText.text == "b,cdefg,hi\n1234");
    }

    SECTION("multiple lines fully in history")
    {
        term.writeToScreen("foo\r\nbar\r\n"); // move first two lines into history.
        /*
         * |  0123456789A
        -2 | "12345,67890"
        -1 | "ab,cdefg,hi"       [fg,hi]
         0 | "12345,67890"       [123]
         1 | "foo"
         2 | "bar"
        */

        auto selector = LinearSelection(
            selectionHelper, CellLocation { .line = LineOffset(-2), .column = ColumnOffset(6) }, []() {});
        (void) selector.extend(CellLocation { .line = LineOffset(-1), .column = ColumnOffset(2) });
        selector.complete();

        vector<Selection::Range> const selection = selector.ranges();
        REQUIRE(selection.size() == 2);

        Selection::Range const& r1 = selection[0];
        CHECK(r1.line == LineOffset(-2));
        CHECK(r1.fromColumn == ColumnOffset(6));
        CHECK(r1.toColumn == ColumnOffset(10));
        CHECK(r1.length() == ColumnCount(5));

        Selection::Range const& r2 = selection[1];
        CHECK(r2.line == LineOffset(-1));
        CHECK(r2.fromColumn == ColumnOffset(0));
        CHECK(r2.toColumn == ColumnOffset(2));
        CHECK(r2.length() == ColumnCount(3));

        auto selectedText = TextSelection { screen };
        renderSelection(selector, selectedText);
        CHECK(selectedText.text == "fg,hi\n123");
    }

    SECTION("multiple lines from history into main buffer")
    {
        term.writeToScreen("foo\r\nbar\r\n"); // move first two lines into history.
        /*
        -3 | "12345,67890"
        -2 | "ab,cdefg,hi"         (--
        -1 | "12345,67890" -----------
         0 | "foo"         --)
         1 | "bar"
         2 | ""
        */

        auto selector = LinearSelection(
            selectionHelper, CellLocation { .line = LineOffset(-2), .column = ColumnOffset(8) }, []() {});
        (void) selector.extend(CellLocation { .line = LineOffset(0), .column = ColumnOffset(1) });
        selector.complete();

        vector<Selection::Range> const selection = selector.ranges();
        REQUIRE(selection.size() == 3);

        Selection::Range const& r1 = selection[0];
        CHECK(r1.line == LineOffset(-2));
        CHECK(r1.fromColumn == ColumnOffset(8));
        CHECK(r1.toColumn == ColumnOffset(10));
        CHECK(r1.length() == ColumnCount(3));

        Selection::Range const& r2 = selection[1];
        CHECK(r2.line == LineOffset(-1));
        CHECK(r2.fromColumn == ColumnOffset(0));
        CHECK(r2.toColumn == ColumnOffset(10));
        CHECK(r2.length() == ColumnCount(11));

        Selection::Range const& r3 = selection[2];
        CHECK(r3.line == LineOffset(0));
        CHECK(r3.fromColumn == ColumnOffset(0));
        CHECK(r3.toColumn == ColumnOffset(1));
        CHECK(r3.length() == ColumnCount(2));

        auto selectedText = TextSelection { screen };
        renderSelection(selector, selectedText);
        CHECK(selectedText.text == ",hi\n12345,67890\nfo");
    }
}

TEST_CASE("Selector.LinearWordWise", "[selector]")
{
    // TODO
}

TEST_CASE("Selector.FullLine", "[selector]")
{
    // TODO
}

TEST_CASE("Selector.Rectangular", "[selector]")
{
    // TODO
}

// NOLINTEND(misc-const-correctness,readability-function-cognitive-complexity)

namespace
{
/// The cell at @p line, @p column.
[[nodiscard]] constexpr CellLocation cellAt(int line, int column)
{
    return CellLocation { .line = LineOffset(line), .column = ColumnOffset(column) };
}

/// The extent of a selection of type @p S from @p from to @p to, extended as a mouse drag would.
template <typename S>
[[nodiscard]] SelectionExtent extentOf(SelectionHelper const& helper, CellLocation from, CellLocation to)
{
    auto selection = S(helper, from, []() {});
    (void) selection.extend(to);
    return selection.extent();
}
} // namespace

TEST_CASE("Selector.extentCountsLinesAndCharacters", "[selector]")
{
    auto term = MockTerm(PageSize { LineCount(3), ColumnCount(11) }, LineCount(5));
    auto& screen = term.terminal.primaryScreen();
    auto const helper = TestSelectionHelper(screen);
    // Row 1 holds three characters and eight cells nothing was written to.
    term.writeToScreen("12345,67890abc\r\n12345,67890");
    REQUIRE(screen.grid().lineText(LineOffset(1)).starts_with("abc"));

    // Row 0 from column 3 (8 cells), row 1 whole (3 written), row 2 to column 4 (5 cells).
    auto const linear = SelectionExtent { .lines = LineCount(3), .characters = 16 };
    CHECK(extentOf<LinearSelection>(helper, cellAt(0, 3), cellAt(2, 4)) == linear);
    CHECK(extentOf<LinearSelection>(helper, cellAt(2, 4), cellAt(0, 3)) == linear);

    // Columns 1 to 3 of each row: "234", "bc" and an empty cell, "234" -- dragged either way.
    auto const rectangle = SelectionExtent { .lines = LineCount(3), .characters = 8 };
    CHECK(extentOf<RectangularSelection>(helper, cellAt(0, 1), cellAt(2, 3)) == rectangle);
    CHECK(extentOf<RectangularSelection>(helper, cellAt(2, 3), cellAt(0, 1)) == rectangle);
}

TEST_CASE("Selector.extentCountsWhatACopyYields", "[selector]")
{
    auto const extentOfRow = [](std::string_view text) {
        auto term = MockTerm(PageSize { LineCount(3), ColumnCount(16) }, LineCount(5));
        term.writeToScreen(text);
        auto const helper = TestSelectionHelper(term.terminal.primaryScreen());
        return extentOf<LinearSelection>(helper, cellAt(0, 0), cellAt(0, 15)).characters;
    };

    // The space between the words is copied; the three after them and the padding beyond are not.
    CHECK(extentOfRow("hello world   ") == 11);
    // A wide character is one character over its two cells.
    CHECK(extentOfRow("a\u4E2Db") == 3);
    // A combining mark is part of the character it combines with.
    CHECK(extentOfRow("e\u0301x") == 2);
}

TEST_CASE("Selector.extentReachesIntoTheScrollback", "[selector]")
{
    auto term = MockTerm(PageSize { LineCount(3), ColumnCount(11) }, LineCount(5));
    term.writeToScreen("aaa\r\nbbb\r\nccc\r\nddd\r\neee");
    auto& screen = term.terminal.primaryScreen();
    REQUIRE(screen.grid().lineText(LineOffset(-2)).starts_with("aaa"));
    auto const helper = TestSelectionHelper(screen);

    // From "aaa", two rows into the history, to the end of "ccc", the page's top row.
    CHECK(extentOf<LinearSelection>(helper, cellAt(-2, 0), cellAt(0, 10))
          == SelectionExtent { .lines = LineCount(3), .characters = 9 });
}

TEST_CASE("Selector.extentCountsCharactersOnlyBelowTheLimit", "[selector]")
{
    // 99 rows of 101 columns is one cell below the limit; 100 of 100 is the limit itself.
    auto below = MockTerm(PageSize { LineCount(99), ColumnCount(101) }, LineCount(0));
    below.writeToScreen("ab");
    auto const belowHelper = TestSelectionHelper(below.terminal.primaryScreen());
    REQUIRE(std::size_t { 99 } * 101 == SelectionCharacterCountLimit - 1);
    CHECK(extentOf<LinearSelection>(belowHelper, cellAt(0, 0), cellAt(98, 100))
          == SelectionExtent { .lines = LineCount(99), .characters = 2 });

    auto atLimit = MockTerm(PageSize { LineCount(100), ColumnCount(100) }, LineCount(0));
    atLimit.writeToScreen("ab");
    auto const atLimitHelper = TestSelectionHelper(atLimit.terminal.primaryScreen());
    REQUIRE(std::size_t { 100 } * 100 == SelectionCharacterCountLimit);
    CHECK(extentOf<LinearSelection>(atLimitHelper, cellAt(0, 0), cellAt(99, 99))
          == SelectionExtent { .lines = LineCount(100), .characters = std::nullopt });

    // A rectangle is bounded by its own width: 100 rows of two columns are counted on a page whose
    // full rows would be at the limit.
    CHECK(extentOf<RectangularSelection>(atLimitHelper, cellAt(0, 0), cellAt(99, 1))
          == SelectionExtent { .lines = LineCount(100), .characters = 2 });
}
